#include "gpu/page_tables.h"

#include <stdlib.h>
#include <string.h>

#include "common/log.h"
#include "core/memory.h"
#include "gpu/gpu_ring.h"
#include "gpu/gpu_window.h"
#include "gpu/tile_pool.h"

// --- layout ------------------------------------------------------------------------------------------

#define SEGMENT_SHIFT 31 // segments start every 2 GiB
#define SEGMENT_SIZE  GiB(4)
#define SEGMENT_PAGES ((u32)(SEGMENT_SIZE >> MEM_PAGE_SHIFT))
#define MAX_SEGMENTS  512 // a graphics address space of 1 TiB

// A segment page's entry: 0 until routed, then the tile + 1, or EMPTY where the title maps nothing, or
// WINDOW where windows over title memory serve the GPU (see prefers_window). STALE: the title changed
// what is mapped there; resolved again before the GPU next runs title work. The top bit caches that
// what backs the page is writable.
#define ENTRY_WINDOW   0x7FFFFFFDu
#define ENTRY_EMPTY    0x7FFFFFFEu
#define ENTRY_STALE    0x7FFFFFFFu
#define ENTRY_VALUE    0x7FFFFFFFu
#define ENTRY_WRITABLE 0x80000000u

struct Segment {
    ID3D12Resource* buffer;
    u64 desktop_address;
    u32* entries; // per page
};

// What backs pages: a frame of physical memory (every address showing it), or a page of other graphics
// memory. Units are named by keys: frames by their ID with FRAME_KEY set, other pages by address.
#define FRAME_KEY (1ull << 63)

enum UnitFlags {
    UNIT_DIRTY = 1 << 0,    // title memory is newer than the tile: uploaded at the next submission
    UNIT_WATCHED = 1 << 1,  // a write watch over `va` reports CPU writes
    UNIT_WRITABLE = 1 << 2, // GPU work may write it
    UNIT_TAKEN = 1 << 3,    // taken from the CPU: the tile is newer
    UNIT_LISTED = 1 << 4,   // in g_pt.work
    UNIT_CPU = 1 << 5,      // the CPU may access the memory
    UNIT_HOST = 1 << 6,     // its CPU copy has an address of ours too (a frame, a section page): takeable
    UNIT_FRESH = 1 << 7,    // its tile holds nothing written yet
};

// The page tables' write watches, apart from other clients' over the same memory.
#define WATCH_CLIENT 1

struct Unit {
    u32 tile; // GPU_NO_TILE until the GPU reaches it
    u32 flags;
    u64 va; // an address showing it, which watches and taking go through (0: none known)
};

// The units of a range of graphics memory outside physical memory, one per page.
struct SectionUnits {
    u64 base;
    u64 size;
    Unit* units;
};

struct PendingMap {
    u32 segment;
    u32 page;
    u32 tile; // GPU_NO_TILE: unmapped
};

struct StalePage {
    u32 segment;
    u32 page;
};

// Lists recorded for the page tables' queue, reused once the GPU is done with them.
#define COPY_LISTS 4

struct CopyList {
    ID3D12CommandAllocator* allocator;
    ID3D12GraphicsCommandList* list;
    u64 submission;
};

template <typename T> struct Array {
    T* items;
    u32 count;
    u32 capacity;

    void push(const T& item) {
        if (count == capacity) {
            u32 grown = capacity ? capacity * 2 : 256;
            T* p = (T*)realloc(items, grown * sizeof(T));
            if (!p)
                FATAL("out of memory for GPU page tables");
            items = p;
            capacity = grown;
        }
        items[count++] = item;
    }
};

// Open addressing over page addresses (0: empty slot, 1: removed).
struct WatchMap {
    u64* vas;
    u64* keys;
    u32 capacity; // a power of two, or 0
    u32 used;     // slots not empty, removed ones included
};

static struct {
    SRWLOCK lock;
    bool live;
    Segment segments[MAX_SEGMENTS];
    Unit* frames; // by frame ID
    u32 frame_count;
    Array<SectionUnits> sections; // sorted by base
    u64* tile_owner;              // per pool tile: the key of the unit holding it
    // Work for the next submission.
    WatchMap watched; // address of each armed watch -> its unit's key
    Array<u64> work;  // unit keys (UNIT_LISTED)
    Array<PendingMap> maps;
    Array<StalePage> stale;
    // The page tables' queue (a copy queue) and what runs on it.
    ID3D12CommandQueue* queue;
    u32 slot;
    ID3D12Fence* fence;
    CopyList lists[COPY_LISTS];
    u32 next_list;
    CopyList download;
    GpuRing staging;
    ID3D12Resource* pool_view;            // every tile of the pool at tile * GPU_TILE_SIZE, for copies
    u32 pool_serials[GPU_TILE_MAX_HEAPS]; // the heaps the view maps (gpu_tile_heap_serial)
    ID3D12Resource* zeros;                // a tile of zeros (upload heap)
    ID3D12Resource* readback;             // a tile's worth of readback memory
    u8* readback_mapped;
    u64 last_submission; // of page table work, which every title submission waits for
    u64 tiles;           // held by units
    u64 downloads;       // so far
} g_pt = {SRWLOCK_INIT};

// --- units -------------------------------------------------------------------------------------------

static SectionUnits* find_section(u64 va) {
    u32 lo = 0, hi = g_pt.sections.count;
    while (lo < hi) {
        u32 mid = lo + (hi - lo) / 2;
        if (g_pt.sections.items[mid].base <= va)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo == 0)
        return nullptr;
    SectionUnits* s = &g_pt.sections.items[lo - 1];
    return va < s->base + s->size ? s : nullptr;
}

static SectionUnits* add_section(const MemRange& range) {
    Unit* units = (Unit*)calloc(range.size >> MEM_PAGE_SHIFT, sizeof(Unit));
    if (!units)
        FATAL("out of memory for GPU page tables");
    for (u64 i = 0; i < range.size >> MEM_PAGE_SHIFT; i++) {
        units[i].tile = GPU_NO_TILE;
        units[i].va = (u64)range.base + (i << MEM_PAGE_SHIFT);
    }
    SectionUnits entry = {(u64)range.base, range.size, units};
    g_pt.sections.push(entry);
    u32 at = g_pt.sections.count - 1;
    while (at > 0 && g_pt.sections.items[at - 1].base > entry.base) {
        g_pt.sections.items[at] = g_pt.sections.items[at - 1];
        at--;
    }
    g_pt.sections.items[at] = entry;
    return &g_pt.sections.items[at];
}

static Unit* unit_of(u64 key) {
    if (key & FRAME_KEY) {
        u64 id = key & ~FRAME_KEY;
        return id < g_pt.frame_count ? &g_pt.frames[id] : nullptr;
    }
    SectionUnits* s = find_section(key);
    return s ? &s->units[(key - s->base) >> MEM_PAGE_SHIFT] : nullptr;
}

static void list_unit(Unit* u, u64 key) {
    if (u->flags & UNIT_LISTED)
        return;
    u->flags |= UNIT_LISTED;
    g_pt.work.push(key);
}

// --- armed watches by address ------------------------------------------------------------------------

#define WATCH_EMPTY   0ull
#define WATCH_REMOVED 1ull

static u32 watch_slot(u64 va, u32 capacity) {
    return (u32)(((va >> MEM_PAGE_SHIFT) * 0x9E3779B97F4A7C15ull) >> 32) & (capacity - 1);
}

static void watched_put(u64 va, u64 key);

static void watched_grow() {
    WatchMap old = g_pt.watched;
    u32 capacity = old.capacity ? old.capacity * 2 : 4096;
    g_pt.watched.vas = (u64*)calloc(capacity, sizeof(u64));
    g_pt.watched.keys = (u64*)malloc(capacity * sizeof(u64));
    if (!g_pt.watched.vas || !g_pt.watched.keys)
        FATAL("out of memory for GPU page tables");
    g_pt.watched.capacity = capacity;
    g_pt.watched.used = 0;
    for (u32 i = 0; i < old.capacity; i++)
        if (old.vas[i] > WATCH_REMOVED)
            watched_put(old.vas[i], old.keys[i]);
    free(old.vas);
    free(old.keys);
}

// Notes the unit `key` watched at `va` (replacing what was noted there). Page tables lock held.
static void watched_put(u64 va, u64 key) {
    if ((g_pt.watched.used + 1) * 2 > g_pt.watched.capacity)
        watched_grow();
    WatchMap& m = g_pt.watched;
    u32 removed = m.capacity;
    for (u32 i = watch_slot(va, m.capacity);; i = (i + 1) & (m.capacity - 1)) {
        if (m.vas[i] == va) {
            m.keys[i] = key;
            return;
        }
        if (m.vas[i] == WATCH_REMOVED && removed == m.capacity)
            removed = i;
        if (m.vas[i] == WATCH_EMPTY) {
            if (removed == m.capacity)
                m.used++;
            else
                i = removed;
            m.vas[i] = va;
            m.keys[i] = key;
            return;
        }
    }
}

// The unit noted as watched at `va`, forgotten; 0 if none. Page tables lock held.
static u64 watched_take(u64 va) {
    WatchMap& m = g_pt.watched;
    if (!m.capacity)
        return 0;
    for (u32 i = watch_slot(va, m.capacity); m.vas[i] != WATCH_EMPTY; i = (i + 1) & (m.capacity - 1)) {
        if (m.vas[i] == va) {
            m.vas[i] = WATCH_REMOVED;
            return m.keys[i];
        }
    }
    return 0;
}

// The unit backing title page `va` (in `range`), or nullptr where the title maps nothing; with `create`,
// section units come into being. Page tables lock held exclusively (shared without `create`).
static Unit* unit_at(u64 va, const MemRange& range, bool create, u64* key) {
    if (range.flags & MEM_RANGE_PLACEHOLDER) {
        u64 pfn = MEM_INVALID_PFN;
        if (!mem_frames_at((const void*)va, 1, &pfn) || pfn == MEM_INVALID_PFN)
            return nullptr;
        u64 id = pfn >> MEM_PAGE_NUMBER_SHIFT;
        *key = FRAME_KEY | id;
        Unit* u = id < g_pt.frame_count ? &g_pt.frames[id] : nullptr;
        if (u && !u->va)
            u->va = va;
        return u;
    }
    SectionUnits* s = find_section(va);
    if (!s && create)
        s = add_section(range);
    if (!s)
        return nullptr;
    *key = va;
    return &s->units[(va - s->base) >> MEM_PAGE_SHIFT];
}

static Unit* tiled_unit_at(u64 va, u64* key);

// Whether what backs any page of [address, address + size) has a tile: the GPU's copy of it is in video
// memory, whichever segment or address it was reached through. Page tables lock held.
static bool range_tiled(u64 address, u64 size) {
    for (u64 va = ALIGN_DOWN(address, MEM_PAGE_SIZE); va < address + size; va += MEM_PAGE_SIZE) {
        u64 key = 0;
        if (tiled_unit_at(va, &key))
            return true;
    }
    return false;
}

static void free_unit_tile(Unit* u) {
    if (u->tile == GPU_NO_TILE)
        return;
    gpu_tiles_free(1, &u->tile); // reused once the GPU is done with work submitted until now
    g_pt.tile_owner[u->tile] = 0;
    u->tile = GPU_NO_TILE;
    g_pt.tiles -= GPU_TILE_SIZE;
}

static void mark_writable(Unit* u, u64 key) {
    if (u->flags & UNIT_WRITABLE)
        return;
    u->flags |= UNIT_WRITABLE;
    if (u->flags & UNIT_CPU)
        list_unit(u, key); // taken from the CPU at the next submission
}

// --- segments ----------------------------------------------------------------------------------------

static bool create_segment(u32 index) {
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = SEGMENT_SIZE;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ID3D12Resource* buffer = nullptr;
    HRESULT hr = gpu_device()->CreateReservedResource2(&desc, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0,
                                                       nullptr, IID_PPV_ARGS(&buffer));
    u32* entries = SUCCEEDED(hr) ? (u32*)calloc(SEGMENT_PAGES, sizeof(u32)) : nullptr;
    if (!entries) {
        if (buffer)
            buffer->Release();
        if (!gpu_device_lost(hr))
            LOG_E("gpu", "cannot create the page table segment at 0x%llx (0x%08lx)", (u64)index << SEGMENT_SHIFT,
                  (unsigned long)hr);
        return false;
    }
    gpu_set_name(buffer, "page table segment 0x%llx", (u64)index << SEGMENT_SHIFT);
    Segment& s = g_pt.segments[index];
    s.entries = entries;
    s.desktop_address = buffer->GetGPUVirtualAddress();
    s.buffer = buffer; // published last: the fast path reads it under the shared lock
    return true;
}

static bool entry_mapped(u32 entry) {
    u32 value = entry & ENTRY_VALUE;
    return entry && value != ENTRY_EMPTY && value != ENTRY_STALE && value != ENTRY_WINDOW;
}

static void set_entry(u32 index, u32 page, u32 entry) {
    Segment& s = g_pt.segments[index];
    u32 old = s.entries[page];
    s.entries[page] = entry;
    u32 value = entry & ENTRY_VALUE, old_value = old & ENTRY_VALUE;
    if (value == old_value && old)
        return;
    if (value == ENTRY_EMPTY && !entry_mapped(old) && old_value != ENTRY_STALE)
        return; // nothing was mapped there
    g_pt.maps.push({index, page, value == ENTRY_EMPTY ? GPU_NO_TILE : value - 1});
}

// Resolves segment page `page`: maps it to the tile of what backs it now (giving that a tile), or unmaps
// it. False if the address isn't graphics memory. Page tables lock held exclusively.
static bool resolve_page(u32 index, u32 page, bool writable) {
    u64 va = ((u64)index << SEGMENT_SHIFT) + ((u64)page << MEM_PAGE_SHIFT);
    MemRange range;
    if (!mem_query_range((const void*)va, &range) || !(range.flags & MEM_RANGE_GPU))
        return false;
    u64 key = 0;
    Unit* u = unit_at(va, range, true, &key);
    // Unmapped pages keep how the GPU uses them: memory mapped there later is written by the same views.
    u32 empty = ENTRY_EMPTY | (writable ? ENTRY_WRITABLE : 0);
    if (!u) {
        set_entry(index, page, empty);
        return true;
    }
    if (u->tile == GPU_NO_TILE) {
        if (!gpu_tiles_alloc(1, &u->tile)) {
            u->tile = GPU_NO_TILE;
            set_entry(index, page, empty); // without video memory it reads zero (the pool logged why)
            return true;
        }
        g_pt.tile_owner[u->tile] = key;
        g_pt.tiles += GPU_TILE_SIZE;
        // A frame may show at other addresses the CPU reaches (titles write through a CPU view and let
        // the GPU read through another): watching and taking go by the frame, and cover those.
        bool cpu = (range.flags & MEM_RANGE_CPU_ACCESS) || (key & FRAME_KEY);
        bool host = (key & FRAME_KEY) || (range.flags & MEM_RANGE_SECTION);
        u->flags = (u->flags & ~(UNIT_TAKEN | UNIT_WATCHED | UNIT_CPU | UNIT_HOST)) | UNIT_DIRTY | UNIT_FRESH |
                   (cpu ? UNIT_CPU : 0) | (host ? UNIT_HOST : 0);
        list_unit(u, key);
    } else if ((u->flags & UNIT_CPU) && !(u->flags & UNIT_WATCHED)) {
        list_unit(u, key); // its watch ended unseen (other frames were mapped where it was): armed again
    }
    if (writable)
        mark_writable(u, key);
    set_entry(index, page, (u->tile + 1) | ((u->flags & UNIT_WRITABLE) ? ENTRY_WRITABLE : 0));
    return true;
}

static void fill_address(u32 index, u64 address, GpuAddress* out) {
    const Segment& s = g_pt.segments[index];
    u64 offset = address - ((u64)index << SEGMENT_SHIFT);
    out->buffer = s.buffer;
    out->offset = offset;
    out->desktop_address = s.desktop_address + offset;
    out->available = SEGMENT_SIZE - offset;
}

// Whether the page tables serve page `entry` for `access` as they are.
static bool page_ready(u32 entry, GpuAccess access) {
    u32 value = entry & ENTRY_VALUE;
    if (!entry || value == ENTRY_STALE || value == ENTRY_WINDOW)
        return false;
    return access == GPU_READ || (entry & ENTRY_WRITABLE);
}

// Memory both CPU and GPU use is served by windows over title memory (gpu/gpu_window.h): the GPU reads
// and writes the CPU's memory in place, coherent with its caches line by line, as on console. That is
// the title's own graphics allocations (one block of memory each), write-combined memory (what the CPU
// writes for the GPU to read once) and physical memory something has written. Memory the CPU can't
// access, and physical memory nothing has written yet (render target memory, buffers the GPU fills),
// gets tiles of video memory instead and no system memory. Page tables lock held.
static bool prefers_window(u64 address, u64 size) {
    for (u64 va = ALIGN_DOWN(address, MEM_PAGE_SIZE); va < address + size; va += MEM_PAGE_SIZE) {
        MemRange range;
        if (!mem_query_range((const void*)va, &range) || !(range.flags & MEM_RANGE_CPU_ACCESS))
            return false;
        if ((range.protect & PAGE_WRITECOMBINE) || !(range.flags & MEM_RANGE_PLACEHOLDER))
            return true;
        u64 pfn = MEM_INVALID_PFN;
        if (mem_frames_at((const void*)va, 1, &pfn) && pfn != MEM_INVALID_PFN && mem_physical_bound(pfn))
            return true;
    }
    return false;
}

bool gpu_translate(u64 address, u64 size, GpuAccess access, GpuAddress* out) {
    size = size ? size : 1;
    u64 index = address >> SEGMENT_SHIFT;
    u64 segment_base = index << SEGMENT_SHIFT;
    if (!g_pt.live || index >= MAX_SEGMENTS || address + size < address ||
        address + size > segment_base + SEGMENT_SIZE) {
        LOG_W_ONCE("gpu", "GPU access 0x%llx+0x%llx is outside the page tables", address, size);
        return false;
    }
    u32 first = (u32)((address - segment_base) >> MEM_PAGE_SHIFT);
    u32 last = (u32)((address + size - 1 - segment_base) >> MEM_PAGE_SHIFT);
    Segment& s = g_pt.segments[index];
    AcquireSRWLockShared(&g_pt.lock);
    bool ready = s.buffer != nullptr, windowed = ready;
    for (u32 p = first; (ready || windowed) && p <= last; p++) {
        ready = ready && page_ready(s.entries[p], access);
        windowed = windowed && (s.entries[p] & ENTRY_VALUE) == ENTRY_WINDOW;
    }
    if (ready)
        fill_address((u32)index, address, out);
    ReleaseSRWLockShared(&g_pt.lock);
    if (ready)
        return true;
    if (windowed && gpu_translate_contiguous(address, size, out))
        return true;

    // Routed once per page, and for good: views made of either kind of address stay valid.
    AcquireSRWLockExclusive(&g_pt.lock);
    bool ok = s.buffer || create_segment((u32)index);
    bool any_window = false, any_tiled = false;
    for (u32 p = first; ok && p <= last; p++) {
        u32 value = s.entries[p] & ENTRY_VALUE;
        any_window |= value == ENTRY_WINDOW;
        any_tiled |= s.entries[p] && value != ENTRY_WINDOW;
    }
    any_tiled = ok && (any_tiled || range_tiled(address, size));
    if (ok && !any_tiled && (any_window || prefers_window(address, size))) {
        GpuAddress w;
        if (gpu_translate_contiguous(address, size, &w)) {
            for (u32 p = first; p <= last; p++)
                s.entries[p] = ENTRY_WINDOW;
            ReleaseSRWLockExclusive(&g_pt.lock);
            *out = w;
            return true;
        }
        // Not one block of memory (frames mapped out of order, aliases): the page tables serve it.
    }
    if (ok && any_window)
        LOG_W_ONCE("gpu", "GPU access 0x%llx+0x%llx mixes windowed memory with page tables; the page tables "
                          "serve it, and earlier views of the windowed part see title memory",
                   address, size);
    for (u32 p = first; ok && p <= last; p++) {
        if (page_ready(s.entries[p], access))
            continue;
        ok = resolve_page((u32)index, p, access == GPU_WRITE);
        if (!ok)
            LOG_E("gpu", "GPU access 0x%llx+0x%llx reaches 0x%llx, which isn't graphics memory", address, size,
                  segment_base + ((u64)p << MEM_PAGE_SHIFT));
    }
    if (ok)
        fill_address((u32)index, address, out);
    ReleaseSRWLockExclusive(&g_pt.lock);
    return ok;
}

bool gpu_translate_reach(u64 address, u64 reach, GpuAccess access, GpuAddress* out) {
    if (!gpu_translate(address, 1, access, out))
        return false;
    u64 index = address >> SEGMENT_SHIFT;
    MemRange range;
    if (reach <= 1 || out->buffer != g_pt.segments[index].buffer || !mem_query_range((const void*)address, &range))
        return true; // a window reaches the rest of its block
    u64 end = address + reach;
    u64 range_end = (u64)range.base + range.size;
    u64 segment_end = (index << SEGMENT_SHIFT) + SEGMENT_SIZE;
    end = end < range_end ? end : range_end;
    end = end < segment_end ? end : segment_end;
    // Pages windows serve stay theirs (views over them see title memory).
    AcquireSRWLockShared(&g_pt.lock);
    const Segment& s = g_pt.segments[index];
    for (u64 va = ALIGN_DOWN(address, MEM_PAGE_SIZE) + MEM_PAGE_SIZE; va < end; va += MEM_PAGE_SIZE) {
        if ((s.entries[(va - (index << SEGMENT_SHIFT)) >> MEM_PAGE_SHIFT] & ENTRY_VALUE) == ENTRY_WINDOW) {
            end = va;
            break;
        }
    }
    ReleaseSRWLockShared(&g_pt.lock);
    GpuAddress wide;
    if (end > ALIGN_DOWN(address, MEM_PAGE_SIZE) + MEM_PAGE_SIZE && gpu_translate(address, end - address, access, &wide))
        *out = wide;
    return true;
}

bool gpu_translate_current(u64 address, u64 size, GpuAddress* out) {
    size = size ? size : 1;
    u64 index = address >> SEGMENT_SHIFT;
    bool tiled = false;
    if (g_pt.live && index < MAX_SEGMENTS && address + size <= (index << SEGMENT_SHIFT) + SEGMENT_SIZE) {
        u64 segment_base = index << SEGMENT_SHIFT;
        u32 first = (u32)((address - segment_base) >> MEM_PAGE_SHIFT);
        u32 last = (u32)((address + size - 1 - segment_base) >> MEM_PAGE_SHIFT);
        AcquireSRWLockExclusive(&g_pt.lock); // looking units up may note where frames show
        const Segment& s = g_pt.segments[index];
        for (u32 p = first; s.buffer && !tiled && p <= last; p++)
            tiled = entry_mapped(s.entries[p]) || (s.entries[p] & ENTRY_VALUE) == ENTRY_STALE;
        tiled = tiled || range_tiled(address, size);
        ReleaseSRWLockExclusive(&g_pt.lock);
    }
    if (tiled)
        return gpu_translate(address, size, GPU_READ, out);
    // A window would back frames nothing has written (and pin them for the GPU) only to read zeros: the
    // caller reads title memory itself, taking zeros for them (see mem_any_pending).
    if (mem_any_pending((const void*)address, size))
        return false;
    return gpu_translate_contiguous(address, size, out);
}

// --- title memory changes ----------------------------------------------------------------------------

// Pages of [base, base + size) in segment `index` that were resolved become stale. Watches over units
// shown there ended unreported (other frames are mapped there now): they're armed again elsewhere.
static void stale_pages(u32 index, u64 base, u64 size) {
    Segment& s = g_pt.segments[index];
    if (!s.buffer)
        return;
    u64 segment_base = (u64)index << SEGMENT_SHIFT;
    u64 lo = base > segment_base ? base : segment_base;
    u64 hi = base + size < segment_base + SEGMENT_SIZE ? base + size : segment_base + SEGMENT_SIZE;
    for (u64 va = ALIGN_DOWN(lo, MEM_PAGE_SIZE); va < hi; va += MEM_PAGE_SIZE) {
        u32 page = (u32)((va - segment_base) >> MEM_PAGE_SHIFT);
        u32 entry = s.entries[page];
        u32 value = entry & ENTRY_VALUE;
        if (!entry || value == ENTRY_STALE)
            continue;
        if (value == ENTRY_WINDOW) {
            s.entries[page] = 0; // windows follow mappings themselves; the next access routes it again
            continue;
        }
        if (value != ENTRY_EMPTY) {
            u64 key = g_pt.tile_owner[value - 1];
            Unit* u = key ? unit_of(key) : nullptr;
            if (u && u->va >= base && u->va < base + size) {
                if (u->flags & UNIT_WATCHED)
                    watched_take(u->va);
                u->flags &= ~UNIT_WATCHED;
                u->va = 0; // another address is found for it
                if (u->flags & UNIT_CPU)
                    list_unit(u, key);
            }
        }
        s.entries[page] = ENTRY_STALE | (entry & ENTRY_WRITABLE);
        g_pt.stale.push({index, page});
    }
}

// Pages of [start, start + size) in every segment reaching them become stale. Page tables lock held.
static void stale_range(u64 start, u64 size) {
    for (u64 index = start >> SEGMENT_SHIFT; index < MAX_SEGMENTS && (index << SEGMENT_SHIFT) < start + size;
         index++) {
        stale_pages((u32)index, start, size); // the segment starting in this granule
        if (index > 0)
            stale_pages((u32)index - 1, start, size); // and the one reaching into it
    }
}

static void on_mapping_changed(const u8* base, u64 size) {
    if (!g_pt.live)
        return;
    AcquireSRWLockExclusive(&g_pt.lock);
    u64 start = (u64)base;
    stale_range(start, size);
    // A range of graphics memory released (and maybe another made at its address since): its units and
    // their tiles go.
    MemRange range;
    SectionUnits* sec = find_section(start);
    if (sec && (!mem_query_range((const void*)sec->base, &range) || (u64)range.base != sec->base ||
                range.size != sec->size)) {
        for (u64 i = 0; i < sec->size >> MEM_PAGE_SHIFT; i++)
            free_unit_tile(&sec->units[i]);
        free(sec->units);
        u32 at = (u32)(sec - g_pt.sections.items);
        memmove(sec, sec + 1, (g_pt.sections.count - at - 1) * sizeof(SectionUnits));
        g_pt.sections.count--;
    }
    ReleaseSRWLockExclusive(&g_pt.lock);
}

static void on_frames_freed(u64 count, const u64* pfns) {
    if (!g_pt.live)
        return;
    AcquireSRWLockExclusive(&g_pt.lock);
    for (u64 i = 0; i < count; i++) {
        u64 id = pfns[i] >> MEM_PAGE_NUMBER_SHIFT;
        if (id >= g_pt.frame_count)
            continue;
        Unit& u = g_pt.frames[id];
        if (u.tile != GPU_NO_TILE) {
            // Freed while still mapped: the pages showing it no longer map its tile, which goes to others.
            u64 vas[4];
            u32 n = mem_frame_addresses(pfns[i], vas, 4);
            for (u32 k = 0; k < n && k < 4; k++)
                stale_range(vas[k], MEM_PAGE_SIZE);
        }
        free_unit_tile(&u);
        u.flags &= UNIT_LISTED; // a listed key finds nothing to do
        u.va = 0;
    }
    ReleaseSRWLockExclusive(&g_pt.lock);
}

// The unit at `va` if it has a tile. Frames are found through any address showing them, CPU-only views
// included. Page tables lock held.
static Unit* tiled_unit_at(u64 va, u64* key) {
    MemRange range;
    if (!mem_query_range((const void*)va, &range) || !(range.flags & (MEM_RANGE_GPU | MEM_RANGE_PLACEHOLDER)))
        return nullptr;
    Unit* u = unit_at(va, range, false, key);
    return u && u->tile != GPU_NO_TILE ? u : nullptr;
}

// CPU writes to pages with tiles: uploaded at the next submission. Write watches report the range armed
// (several ended at once may come merged into one report), devices what they wrote.
static void on_written(u64 base, u64 size, MemWriteKind kind) {
    if (!g_pt.live)
        return;
    AcquireSRWLockExclusive(&g_pt.lock);
    if (kind == MEM_WATCH_DROPPED) {
        // Other memory mapped where a unit's watch was: it is armed again where the unit shows now, and
        // what the CPU wrote meanwhile goes up with it. Found by the watch's address (what is mapped
        // there now is another unit).
        if (size == MEM_PAGE_SIZE && base % MEM_PAGE_SIZE == 0) {
            u64 key = watched_take(base);
            Unit* u = key ? unit_of(key) : nullptr;
            if (u && (u->flags & UNIT_WATCHED) && u->va == base) {
                u->flags &= ~UNIT_WATCHED;
                u->va = 0;
                list_unit(u, key);
            }
        }
    } else if (kind == MEM_WATCH_ENDED) {
        for (u64 va = ALIGN_UP(base, MEM_PAGE_SIZE); va + MEM_PAGE_SIZE <= base + size; va += MEM_PAGE_SIZE) {
            u64 key = 0;
            Unit* u = tiled_unit_at(va, &key);
            if (u && (u->flags & UNIT_WATCHED) && u->va == va) {
                u->flags = (u->flags & ~UNIT_WATCHED) | UNIT_DIRTY;
                list_unit(u, key);
                watched_take(va);
            }
        }
    } else {
        for (u64 va = ALIGN_DOWN(base, MEM_PAGE_SIZE); va < base + size; va += MEM_PAGE_SIZE) {
            u64 key = 0;
            Unit* u = tiled_unit_at(va, &key);
            if (!u)
                continue;
            if (u->flags & UNIT_TAKEN) {
                // A device wrote a page the GPU may have written too: the device's data is the newer.
                u->flags &= ~UNIT_TAKEN;
                mem_give_pages((const void*)va, MEM_PAGE_SIZE);
            }
            u->flags |= UNIT_DIRTY;
            list_unit(u, key);
        }
    }
    ReleaseSRWLockExclusive(&g_pt.lock);
}

// --- the page tables' queue --------------------------------------------------------------------------

// Records into `l` once the GPU is done with its last use. Page tables lock held.
static bool begin_list(CopyList& l) {
    HRESULT hr;
    if (!l.list) {
        hr = gpu_device()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(&l.allocator));
        if (SUCCEEDED(hr))
            hr = gpu_device()->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, l.allocator, nullptr,
                                                 IID_PPV_ARGS(&l.list));
    } else {
        gpu_queue_wait(g_pt.slot, l.submission);
        hr = l.allocator->Reset();
        if (SUCCEEDED(hr))
            hr = l.list->Reset(l.allocator, nullptr);
    }
    if (FAILED(hr) && !gpu_device_lost(hr))
        LOG_E("gpu", "cannot record page table work (0x%08lx)", (unsigned long)hr);
    return SUCCEEDED(hr);
}

// Maps the heaps of the pool the view doesn't show yet (tiles handed out since). Page tables lock held.
static void map_pool_heaps() {
    for (u32 h = 0; h < GPU_TILE_MAX_HEAPS; h++) {
        u32 serial = gpu_tile_heap_serial(h * GPU_TILE_HEAP_TILES);
        if (!serial || serial == g_pt.pool_serials[h])
            continue;
        D3D12_TILED_RESOURCE_COORDINATE at = {h * GPU_TILE_HEAP_TILES, 0, 0, 0};
        D3D12_TILE_REGION_SIZE region = {GPU_TILE_HEAP_TILES, FALSE, 0, 0, 0};
        UINT offset = 0, count = GPU_TILE_HEAP_TILES;
        g_pt.queue->UpdateTileMappings(g_pt.pool_view, 1, &at, &region, gpu_tile_heap(h * GPU_TILE_HEAP_TILES), 1,
                                       nullptr, &offset, &count, D3D12_TILE_MAPPING_FLAG_NONE);
        g_pt.pool_serials[h] = serial;
    }
}

// Applies the pending segment mappings, a call per segment and heap. Page tables lock held.
static void apply_maps() {
    static D3D12_TILED_RESOURCE_COORDINATE at[256];
    static D3D12_TILE_REGION_SIZE regions[256];
    static UINT offsets[256], counts[256];
    for (u32 i = 0; i < g_pt.maps.count;) {
        u32 segment = g_pt.maps.items[i].segment;
        u32 tile = g_pt.maps.items[i].tile;
        ID3D12Heap* heap = tile == GPU_NO_TILE ? nullptr : gpu_tile_heap(tile);
        u32 n = 0, tiles = 0, last_page = 0xFFFFFFFFu;
        while (i < g_pt.maps.count && n < 256) {
            const PendingMap& m = g_pt.maps.items[i];
            ID3D12Heap* h = m.tile == GPU_NO_TILE ? nullptr : gpu_tile_heap(m.tile);
            if (m.segment != segment || h != heap)
                break;
            i++;
            if (m.page == last_page)
                continue; // the same map again (maps hold what pages show in the end)
            u32 offset = heap ? gpu_tile_offset(m.tile) : 0;
            // Pages in a row mapped to tiles in a row are one region: the driver's cost goes by region.
            if (n && m.page == last_page + 1 && (!heap || offsets[n - 1] + counts[n - 1] == offset)) {
                regions[n - 1].NumTiles++;
                counts[n - 1]++;
            } else {
                at[n] = {m.page, 0, 0, 0};
                regions[n] = {1, FALSE, 0, 0, 0};
                offsets[n] = offset;
                counts[n] = 1;
                n++;
            }
            tiles++;
            last_page = m.page;
        }
        ID3D12Resource* buffer = g_pt.segments[segment].buffer;
        if (heap) {
            g_pt.queue->UpdateTileMappings(buffer, n, at, regions, heap, n, nullptr, offsets, counts,
                                           D3D12_TILE_MAPPING_FLAG_NONE);
        } else {
            D3D12_TILE_RANGE_FLAGS null_range = D3D12_TILE_RANGE_FLAG_NULL;
            UINT all = tiles;
            g_pt.queue->UpdateTileMappings(buffer, n, at, regions, nullptr, 1, &null_range, nullptr, &all,
                                           D3D12_TILE_MAPPING_FLAG_NONE);
        }
    }
    g_pt.maps.count = 0;
}

static int compare_maps(const void* a, const void* b) {
    const PendingMap* x = (const PendingMap*)a;
    const PendingMap* y = (const PendingMap*)b;
    if (x->segment != y->segment)
        return x->segment < y->segment ? -1 : 1;
    u64 hx = x->tile == GPU_NO_TILE ? 0 : (u64)gpu_tile_heap(x->tile);
    u64 hy = y->tile == GPU_NO_TILE ? 0 : (u64)gpu_tile_heap(y->tile);
    if (hx != hy)
        return hx < hy ? -1 : 1;
    return x->page < y->page ? -1 : x->page > y->page ? 1 : 0; // pages in a row form runs
}

// Where the CPU's copy of a unit is: its frame through the linear view (nullptr while the frame has no
// system memory: it reads zero), or a section page through the section's host view.
static u8* host_page(Unit* u, u64 key, bool bind) {
    if (key & FRAME_KEY) {
        u64 pfn = (key & ~FRAME_KEY) << MEM_PAGE_NUMBER_SHIFT;
        return bind || mem_physical_bound(pfn) ? mem_physical_ptr(pfn) : nullptr;
    }
    MemBlock block;
    if (!mem_backing_block((const void*)u->va, &block))
        return nullptr;
    u8* host = block.host + (u->va - (u64)block.base);
    MEMORY_BASIC_INFORMATION m;
    if (!bind && (!VirtualQuery(host, &m, sizeof(m)) || m.State != MEM_COMMIT))
        return nullptr; // not committed yet: reads zero
    if (bind)
        mem_prepare((const void*)u->va, MEM_PAGE_SIZE);
    return host;
}

// An address showing frame unit `key` now, or 0.
static u64 frame_address(u64 key) {
    u64 va = 0;
    if (!mem_frame_addresses((key & ~FRAME_KEY) << MEM_PAGE_NUMBER_SHIFT, &va, 1))
        return 0;
    return va;
}

// Uploads, watches and takes what the listed units need. Watches are armed before title memory is
// read, so a write after the read reports. Page tables lock held.
struct Upload {
    u32 tile;
    const u8* host; // nullptr: zeros
    u64 key;
};

static Array<Upload> s_uploads; // under the page tables lock
static Array<u64> s_deferred;   // units listed again for the next flush

static bool record_units(ID3D12GraphicsCommandList* list) {
    s_uploads.count = 0;
    s_deferred.count = 0;
    u32 with_data = 0;
    for (u32 i = 0; i < g_pt.work.count; i++) {
        u64 key = g_pt.work.items[i];
        Unit* u = unit_of(key);
        if (!u)
            continue;
        u->flags &= ~UNIT_LISTED;
        if (u->tile == GPU_NO_TILE)
            continue;
        if ((key & FRAME_KEY) && !u->va)
            u->va = frame_address(key); // 0 while mapped nowhere: listed again when the GPU reaches it
        bool cpu = (u->flags & UNIT_CPU) && u->va;
        if (cpu && mem_device_writes_pending(u->va, MEM_PAGE_SIZE)) {
            // A device (a file read, DirectStorage) writes it now, which fails on a watched or taken page
            // instead of faulting; its report brings the data. Meanwhile the GPU reads zeros, not
            // whatever the tile held before.
            if (u->flags & UNIT_FRESH)
                s_uploads.push({u->tile, nullptr, key});
            u->flags &= ~UNIT_FRESH;
            s_deferred.push(key);
            continue;
        }
        if (cpu && !(u->flags & UNIT_WATCHED)) {
            // Armed before title memory is read: what the CPU wrote while nothing watched goes up with it.
            mem_watch_writes((const void*)u->va, MEM_PAGE_SIZE, WATCH_CLIENT);
            watched_put(u->va, key);
            u->flags |= UNIT_WATCHED | UNIT_DIRTY;
        }
        // A taken unit's GPU copy is newer: a CPU write to it downloads it first.
        if ((u->flags & UNIT_DIRTY) && !(u->flags & UNIT_TAKEN)) {
            const u8* host = host_page(u, key, false);
            s_uploads.push({u->tile, host, key});
            with_data += host != nullptr;
        }
        u->flags &= ~(UNIT_DIRTY | UNIT_FRESH);
        // Taken where our own address of its CPU copy lets a download land without touching the title's.
        if (cpu && (u->flags & (UNIT_WRITABLE | UNIT_HOST)) == (UNIT_WRITABLE | UNIT_HOST) &&
            !(u->flags & UNIT_TAKEN)) {
            mem_take_pages((const void*)u->va, MEM_PAGE_SIZE);
            u->flags |= UNIT_TAKEN;
        }
    }
    g_pt.work.count = 0;
    for (u32 i = 0; i < s_deferred.count; i++)
        list_unit(unit_of(s_deferred.items[i]), s_deferred.items[i]);
    // One region of the staging ring for all of it: growing the ring retires its buffer, which a list
    // being recorded can't lose.
    u64 at = 0;
    bool staged = with_data && gpu_ring_alloc(&g_pt.staging, (u64)with_data * GPU_TILE_SIZE,
                                              D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, &at);
    if (with_data && !staged)
        LOG_E("gpu", "no staging memory for %u pages of title memory; they're uploaded later", with_data);
    for (u32 i = 0; i < s_uploads.count; i++) {
        const Upload& up = s_uploads.items[i];
        u64 dst = (u64)up.tile * GPU_TILE_SIZE;
        if (up.host && staged) {
            memcpy(g_pt.staging.mapped + at, up.host, GPU_TILE_SIZE);
            list->CopyBufferRegion(g_pt.pool_view, dst, g_pt.staging.buffer, at, GPU_TILE_SIZE);
            at += GPU_TILE_SIZE;
            continue;
        }
        list->CopyBufferRegion(g_pt.pool_view, dst, g_pt.zeros, 0, GPU_TILE_SIZE); // defined meanwhile
        if (up.host) {
            // Uploaded later; until then the tile isn't the newer copy, so the CPU keeps the page.
            Unit* u = unit_of(up.key);
            if (u->flags & UNIT_TAKEN) {
                u->flags &= ~UNIT_TAKEN;
                mem_give_pages((const void*)u->va, MEM_PAGE_SIZE);
            }
            u->flags |= UNIT_DIRTY;
            list_unit(u, up.key);
        }
    }
    return s_uploads.count != 0;
}

// Maps what was translated and uploads what the CPU wrote since the last time, on the page tables'
// queue; title work submitted from then on waits for it (g_pt.last_submission). Page tables lock held.
static void flush_locked() {
    for (u32 i = 0; i < g_pt.stale.count; i++) {
        const StalePage& p = g_pt.stale.items[i];
        Segment& s = g_pt.segments[p.segment];
        if ((s.entries[p.page] & ENTRY_VALUE) == ENTRY_STALE &&
            !resolve_page(p.segment, p.page, (s.entries[p.page] & ENTRY_WRITABLE) != 0))
            set_entry(p.segment, p.page, ENTRY_EMPTY); // no longer graphics memory
    }
    g_pt.stale.count = 0;
    if (!g_pt.work.count && !g_pt.maps.count)
        return;
    CopyList& l = g_pt.lists[g_pt.next_list];
    bool recording = begin_list(l);
    bool copied = recording && record_units(l.list);
    if (recording)
        l.list->Close();
    if (!copied && !g_pt.maps.count)
        return; // only units waiting for devices
    map_pool_heaps(); // before copies reach new tiles through the view
    // A page changed several times since the last flush has several maps: each maps what the page shows
    // now, so their order doesn't matter.
    for (u32 i = 0; i < g_pt.maps.count; i++) {
        PendingMap& m = g_pt.maps.items[i];
        u32 entry = g_pt.segments[m.segment].entries[m.page];
        m.tile = entry_mapped(entry) ? (entry & ENTRY_VALUE) - 1 : GPU_NO_TILE;
    }
    qsort(g_pt.maps.items, g_pt.maps.count, sizeof(PendingMap), compare_maps);
    apply_maps();
    ID3D12CommandList* lists[] = {l.list};
    u64 number = gpu_queue_execute(g_pt.queue, g_pt.slot, copied ? 1 : 0, lists); // after the mappings
    if (copied) {
        l.submission = number;
        gpu_ring_submitted(&g_pt.staging, number);
        g_pt.next_list = (g_pt.next_list + 1) % COPY_LISTS;
    }
    g_pt.last_submission = number;
}

static bool work_pending() {
    return g_pt.work.count || g_pt.maps.count || g_pt.stale.count;
}

// Flushes what is pending and returns the page table work submitted so far. Under the lock: another
// thread's flush may hold what the caller's work needs and not be submitted yet.
static u64 flush() {
    mem_poll_device_writes(); // writes finished since go up now
    AcquireSRWLockExclusive(&g_pt.lock);
    if (work_pending())
        flush_locked();
    u64 last = g_pt.last_submission;
    ReleaseSRWLockExclusive(&g_pt.lock);
    return last;
}

void gpu_page_tables_submit(ID3D12CommandQueue* queue) {
    if (!g_pt.live)
        return;
    u64 last = flush();
    if (last)
        queue->Wait(g_pt.fence, last);
}

void gpu_page_tables_release() {
    if (!g_pt.live)
        return;
    u64 last = flush();
    if (last && gpu_queue_completed(g_pt.slot) < last)
        gpu_queue_wait(g_pt.slot, last); // the page tables' queue never waits for anything: no deadlock
}

// --- taken pages -------------------------------------------------------------------------------------

// The CPU reached a page GPU work may have written: once the GPU is done with the work submitted so far,
// its tile comes back to title memory.
static void on_taken(const void* addr) {
    u64 va = ALIGN_DOWN((u64)addr, MEM_PAGE_SIZE);
    // A device that just wrote the page gives it back with its data (the newer), see on_written.
    mem_poll_device_writes();
    AcquireSRWLockExclusive(&g_pt.lock);
    u64 key = 0;
    Unit* u = tiled_unit_at(va, &key);
    if (!u || !(u->flags & UNIT_TAKEN)) {
        ReleaseSRWLockExclusive(&g_pt.lock);
        mem_give_pages((const void*)va, MEM_PAGE_SIZE); // not ours (any more): the access goes on
        return;
    }
    // The tile is copied back as it is now: titles read what GPU work wrote after waiting for that work
    // (a fence), so it is there; waiting for all the work submitted instead could wait for work that
    // waits for this thread.
    if (begin_list(g_pt.download)) {
        g_pt.download.list->CopyBufferRegion(g_pt.readback, 0, g_pt.pool_view, (u64)u->tile * GPU_TILE_SIZE,
                                             GPU_TILE_SIZE);
        g_pt.download.list->Close();
        ID3D12CommandList* lists[] = {g_pt.download.list};
        g_pt.download.submission = gpu_queue_execute(g_pt.queue, g_pt.slot, 1, lists);
        gpu_queue_wait(g_pt.slot, g_pt.download.submission);
        if (u8* host = host_page(u, key, true))
            memcpy(host, g_pt.readback_mapped, GPU_TILE_SIZE);
        u->flags &= ~UNIT_TAKEN;
        list_unit(u, key); // taken again at the next submission
        g_pt.downloads++;
    }
    ReleaseSRWLockExclusive(&g_pt.lock);
    mem_give_pages((const void*)va, MEM_PAGE_SIZE);
}

// --- setup -------------------------------------------------------------------------------------------

static ID3D12Resource* create_buffer(D3D12_HEAP_TYPE type, u64 size, const char* name) {
    D3D12_HEAP_PROPERTIES heap = {type};
    D3D12_RESOURCE_DESC1 desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* buffer = nullptr;
    if (FAILED(gpu_device()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                      D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr,
                                                      IID_PPV_ARGS(&buffer))))
        return nullptr;
    gpu_set_name(buffer, "%s", name);
    return buffer;
}

void gpu_page_tables_init() {
    ID3D12Device13* device = gpu_device();
    if (!device)
        return;
    D3D12_COMMAND_QUEUE_DESC qd = {D3D12_COMMAND_LIST_TYPE_COPY};
    if (FAILED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&g_pt.queue)))) {
        LOG_E("gpu", "cannot create the page table queue");
        return;
    }
    gpu_set_name(g_pt.queue, "page tables");
    g_pt.slot = gpu_queue_register(g_pt.queue);
    g_pt.fence = gpu_queue_fence(g_pt.slot);
    g_pt.staging.upload = true;
    g_pt.staging.slot = g_pt.slot;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = (u64)GPU_TILE_MAX_HEAPS * GPU_TILE_HEAP_TILES * GPU_TILE_SIZE;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    HRESULT hr = device->CreateReservedResource2(&desc, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr,
                                                 IID_PPV_ARGS(&g_pt.pool_view));
    g_pt.zeros = create_buffer(D3D12_HEAP_TYPE_UPLOAD, GPU_TILE_SIZE, "page tables: zeros");
    g_pt.readback = create_buffer(D3D12_HEAP_TYPE_READBACK, GPU_TILE_SIZE, "page tables: readback");
    void* zeros = nullptr;
    if (FAILED(hr) || !g_pt.zeros || !g_pt.readback || FAILED(g_pt.zeros->Map(0, nullptr, &zeros)) ||
        FAILED(g_pt.readback->Map(0, nullptr, (void**)&g_pt.readback_mapped))) {
        LOG_E("gpu", "cannot create the page tables' buffers (0x%08lx)", (unsigned long)hr);
        return;
    }
    memset(zeros, 0, GPU_TILE_SIZE);
    gpu_set_name(g_pt.pool_view, "page tables: tile pool view");

    g_pt.frame_count = (u32)mem_physical_total_pages();
    g_pt.frames = (Unit*)calloc(g_pt.frame_count, sizeof(Unit));
    g_pt.tile_owner = (u64*)calloc((u64)GPU_TILE_MAX_HEAPS * GPU_TILE_HEAP_TILES, sizeof(u64));
    if (!g_pt.frames || !g_pt.tile_owner)
        FATAL("out of memory for GPU page tables");
    for (u32 i = 0; i < g_pt.frame_count; i++)
        g_pt.frames[i].tile = GPU_NO_TILE;
    mem_add_mapping_listener(on_mapping_changed);
    mem_add_write_listener(on_written);
    mem_set_taken_listener(on_taken);
    mem_set_frames_freed_listener(on_frames_freed);
    g_pt.live = true;
}

void gpu_page_tables_usage(u64* tiles, u64* downloads) {
    AcquireSRWLockShared(&g_pt.lock);
    *tiles = g_pt.tiles;
    *downloads = g_pt.downloads;
    ReleaseSRWLockShared(&g_pt.lock);
}
