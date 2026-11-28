#include "core/memory.h"

#include <windows.h>
#include <stdlib.h>

#include "common/log.h"
#include "common/thread_slot.h"
#include "core/memory_internal.h"

MemoryState g_mem;

static LONG CALLBACK on_access_fault(EXCEPTION_POINTERS* ep);
static ThreadSlot g_retry_address;
static ThreadSlot g_retry_count;

static inline bool bit_get(const u64* bm, u64 i) {
    return (bm[i >> 6] >> (i & 63)) & 1;
}
static inline void bit_set(u64* bm, u64 i) {
    bm[i >> 6] |= 1ull << (i & 63);
}
static inline void bit_clear(u64* bm, u64 i) {
    bm[i >> 6] &= ~(1ull << (i & 63));
}

bool mem_init(u64 physical_bytes) {
    InitializeSRWLock(&g_mem.phys_lock);
    InitializeSRWLock(&g_mem.range_lock);

    HMODULE kb = GetModuleHandleW(L"kernelbase.dll");
    g_mem.virtual_alloc2 = (VirtualAlloc2Fn)GetProcAddress(kb, "VirtualAlloc2");
    g_mem.map_view3 = (MapViewOfFile3Fn)GetProcAddress(kb, "MapViewOfFile3");
    g_mem.unmap_view2 = (UnmapViewOfFile2Fn)GetProcAddress(kb, "UnmapViewOfFile2");
    if (!g_mem.virtual_alloc2 || !g_mem.map_view3 || !g_mem.unmap_view2) {
        LOG_E("memory", "placeholder APIs unavailable (Windows 10 1803+ required)");
        return false;
    }

    physical_bytes = ALIGN_UP(physical_bytes, MEM_CHUNK_SIZE);
    g_mem.total_pages = physical_bytes >> MEM_PAGE_SHIFT;
    g_mem.chunk_count = (u32)(physical_bytes / MEM_CHUNK_SIZE);
    if (g_mem.chunk_count > MAX_CHUNKS) {
        LOG_E("memory", "physical memory size %llu MiB exceeds %u chunks", physical_bytes >> 20, MAX_CHUNKS);
        return false;
    }
    // The linear view is one placeholder per chunk; a chunk's section replaces its placeholder when
    // the chunk is created.
    g_mem.linear =
        (u8*)g_mem.virtual_alloc2(GetCurrentProcess(), nullptr, physical_bytes,
                                  MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0);
    if (!g_mem.linear) {
        LOG_E("memory", "cannot reserve the physical memory view (%lu)", GetLastError());
        return false;
    }
    for (u32 i = 0; i + 1 < g_mem.chunk_count; i++) {
        if (!VirtualFree(g_mem.linear + (u64)i * MEM_CHUNK_SIZE, MEM_CHUNK_SIZE,
                         MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) {
            LOG_E("memory", "cannot split the physical memory view (%lu)", GetLastError());
            return false;
        }
    }
    u64 words = (g_mem.total_pages + 63) / 64;
    u64 table_bytes = words * 8 * 3 + g_mem.total_pages * 4 * 2 + g_mem.total_pages * 8;
    u8* tables = (u8*)VirtualAlloc(nullptr, table_bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!tables) {
        LOG_E("memory", "cannot allocate page frame tables (%lu)", GetLastError());
        return false;
    }
    g_mem.id_bitmap = (u64*)tables;
    g_mem.real_bitmap = g_mem.id_bitmap + words;
    g_mem.dirty_bitmap = g_mem.real_bitmap + words;
    g_mem.id_to_real = (u32*)(g_mem.dirty_bitmap + words);
    g_mem.real_to_id = g_mem.id_to_real + g_mem.total_pages;
    g_mem.frame_va = (u64*)(g_mem.real_to_id + g_mem.total_pages);
    memset(g_mem.id_to_real, 0xFF, g_mem.total_pages * 4);
    // Frame 0 is never handed out: titles map page number 0 where they want no page (see
    // map_span_locked), so it must not name a frame.
    bit_set(g_mem.id_bitmap, 0);
    g_mem.used_pages = 1;
    g_mem.id_hint = 1;

    g_mem.gpu_lowest = GiB(2);
    g_mem.gpu_highest = (1ull << 40) - 1;

    tracking_init();
    g_retry_address.init();
    g_retry_count.init();

    // First in line: faults on pending pages are ours, not crashes.
    if (!AddVectoredExceptionHandler(1, on_access_fault)) {
        LOG_E("memory", "cannot install the page backing handler");
        return false;
    }

    LOG_I("memory", "physical memory: %llu MiB (%llu pages), linear view at %p", physical_bytes >> 20,
          g_mem.total_pages, g_mem.linear);
    return true;
}

// ---------------------------------------------------------------------------------------------
// Physical pages

u64 mem_physical_total_pages() {
    return g_mem.total_pages;
}
u64 mem_physical_used_pages() {
    return g_mem.used_pages;
}

u32 mem_physical_chunk_count() {
    return g_mem.chunk_count;
}

u64 mem_physical_bound_pages() {
    u64 bound = 0;
    AcquireSRWLockShared(&g_mem.phys_lock);
    for (u64 w = 0; w < (g_mem.total_pages + 63) / 64; w++)
        bound += __popcnt64(g_mem.real_bitmap[w]);
    ReleaseSRWLockShared(&g_mem.phys_lock);
    return bound;
}

u64 mem_physical_bound_pages_at(const void* va, u64 size) {
    u64 bound = 0;
    AcquireSRWLockShared(&g_mem.range_lock);
    RangeSlot* s = find_range_locked(va);
    if (s && (s->info.flags & MEM_RANGE_PLACEHOLDER)) {
        u64 first = (u64)((const u8*)va - s->info.base) >> MEM_PAGE_SHIFT;
        u64 last = ((u64)((const u8*)va - s->info.base) + size + MEM_PAGE_SIZE - 1) >> MEM_PAGE_SHIFT;
        last = last < range_pages(s) ? last : range_pages(s);
        AcquireSRWLockShared(&g_mem.phys_lock);
        for (u64 i = first; i < last; i++) {
            u32 id = frame_at_locked(s, (u32)i);
            bound += id != MEM_INVALID_PFN && g_mem.id_to_real[id] != MEM_INVALID_PFN;
        }
        ReleaseSRWLockShared(&g_mem.phys_lock);
    }
    ReleaseSRWLockShared(&g_mem.range_lock);
    return bound;
}

u32 mem_physical_chunks_created() {
    u32 created = 0;
    for (u32 i = 0; i < g_mem.chunk_count; i++)
        created += g_mem.chunks[i] != nullptr;
    return created;
}
void* mem_physical_chunk(u32 index) {
    return index < g_mem.chunk_count ? g_mem.chunks[index] : nullptr;
}

bool mem_linear_location(const void* host, u32* chunk, u64* offset) {
    u64 at = (u64)((const u8*)host - g_mem.linear);
    if ((const u8*)host < g_mem.linear || at >= g_mem.total_pages << MEM_PAGE_SHIFT)
        return false;
    *chunk = (u32)(at / MEM_CHUNK_SIZE);
    *offset = at % MEM_CHUNK_SIZE;
    return true;
}

static u8* real_ptr(u32 real) {
    return g_mem.linear + ((u64)real << MEM_PAGE_SHIFT);
}

bool mem_commit_available(u64 bytes) {
    MEMORYSTATUSEX m = {sizeof(m)};
    return !GlobalMemoryStatusEx(&m) || m.ullAvailPageFile >= bytes;
}

// Creates the section for a chunk and maps it into its slot of the linear view. phys_lock held.
static void create_chunk(u32 index) {
    HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE | SEC_COMMIT,
                                        (DWORD)(MEM_CHUNK_SIZE >> 32), (DWORD)MEM_CHUNK_SIZE, nullptr);
    if (!section)
        FATAL("cannot commit %llu MiB of physical memory (%lu): the system is out of commit", MEM_CHUNK_SIZE >> 20,
              GetLastError());
    u8* slot = g_mem.linear + (u64)index * MEM_CHUNK_SIZE;
    if (!g_mem.map_view3(section, GetCurrentProcess(), slot, 0, MEM_CHUNK_SIZE, MEM_REPLACE_PLACEHOLDER,
                         PAGE_READWRITE, nullptr, 0))
        FATAL("cannot map physical memory chunk %u (%lu)", index, GetLastError());
    g_mem.chunks[index] = section;
    g_mem.chunks_created++;
    LOG_D("memory", "physical chunk %u committed (%llu MiB)", index, MEM_CHUNK_SIZE >> 20);
}

// Whether the chunks made so far have frames free beyond a slack of an eighth: worth filling before
// another chunk takes commit charge (every chunk is committed whole). phys_lock held.
static bool chunks_have_room() {
    u64 capacity = (u64)g_mem.chunks_created * MEM_CHUNK_PAGES;
    return capacity - g_mem.bound_real > capacity / 8;
}

// Claims a region that has no real frame bound and no range slice preferring it, for a slice: first
// in chunks that exist already, which keeps commit charge close to what is used. Returns its first
// real frame, or MEM_INVALID_PFN. phys_lock held.
static u32 claim_free_region() {
    const u32 regions = (u32)(g_mem.total_pages / REGION_PAGES);
    const u32 per_chunk = (u32)(MEM_CHUNK_PAGES / REGION_PAGES);
    for (int pass = 0; pass < 2; pass++) {
        for (u32 r = 0; r < regions; r++) {
            bool created = g_mem.chunks[r / per_chunk] != nullptr;
            if (g_mem.region_claimed[r] || created != (pass == 0))
                continue;
            const u64* w = g_mem.real_bitmap + (u64)r * (REGION_PAGES / 64);
            u64 any = 0;
            for (u32 i = 0; i < REGION_PAGES / 64; i++)
                any |= w[i];
            if (!any) {
                g_mem.region_claimed[r] = true;
                return r * REGION_PAGES;
            }
        }
    }
    return MEM_INVALID_PFN;
}

// A free real frame: in chunks that exist, outside regions slices claimed if possible, then in the
// holes of claimed regions; only then in a new chunk. phys_lock held.
static u32 find_free_real() {
    const u64 words = g_mem.total_pages / 64;
    const u64 start = g_mem.real_hint / 64;
    for (int pass = 0; pass < 4; pass++) {
        bool existing_only = pass < 2, unclaimed_only = pass % 2 == 0;
        for (u64 n = 0; n < words; n++) {
            u64 w = (start + n) % words;
            u64 first = w * 64;
            if (existing_only && !g_mem.chunks[first / MEM_CHUNK_PAGES])
                continue;
            if (unclaimed_only && g_mem.region_claimed[first / REGION_PAGES])
                continue;
            u64 free = ~g_mem.real_bitmap[w];
            if (!free)
                continue;
            unsigned long bit;
            _BitScanForward64(&bit, free);
            u64 i = first + bit;
            g_mem.real_hint = i + 1 < g_mem.total_pages ? i + 1 : 0;
            return (u32)i;
        }
    }
    FATAL("no free real page frame although allocated IDs reserve them");
}

// Binds frame ID `id` to real memory, at `preferred` if that frame is free. Chunks are created as
// needed and recycled frames are zeroed, so a frame reads as zero until written. phys_lock held.
static u32 bind_frame(u32 id, u32 preferred) {
    u32 real = g_mem.id_to_real[id];
    if (real != MEM_INVALID_PFN)
        return real;
    // The preferred frame only where it costs no new chunk while the others have room.
    bool usable = preferred < g_mem.total_pages && !bit_get(g_mem.real_bitmap, preferred) &&
                  (g_mem.chunks[preferred / MEM_CHUNK_PAGES] || !chunks_have_room());
    real = usable ? preferred : find_free_real();
    bit_set(g_mem.real_bitmap, real);
    g_mem.bound_real++;
    g_mem.id_to_real[id] = real;
    g_mem.real_to_id[real] = id;
    u32 chunk = real / (u32)MEM_CHUNK_PAGES;
    if (!g_mem.chunks[chunk])
        create_chunk(chunk);
    if (bit_get(g_mem.dirty_bitmap, real)) {
        memset(real_ptr(real), 0, MEM_PAGE_SIZE);
        bit_clear(g_mem.dirty_bitmap, real);
    }
    return real;
}

// The frame ID behind page number `pfn`, or MEM_INVALID_PFN unless that's an allocated frame.
// phys_lock held (shared is enough).
static u32 frame_of_locked(u64 pfn) {
    u64 id = pfn >> MEM_PAGE_NUMBER_SHIFT;
    // Frame 0 is reserved (see mem_init): page number 0 names no frame.
    if ((pfn & ((1u << MEM_PAGE_NUMBER_SHIFT) - 1)) || id == 0 || id >= g_mem.total_pages ||
        !bit_get(g_mem.id_bitmap, id))
        return MEM_INVALID_PFN;
    return (u32)id;
}

bool mem_physical_bound(u64 pfn) {
    AcquireSRWLockShared(&g_mem.phys_lock);
    u32 id = frame_of_locked(pfn);
    bool bound = id != MEM_INVALID_PFN && g_mem.id_to_real[id] != MEM_INVALID_PFN;
    ReleaseSRWLockShared(&g_mem.phys_lock);
    return bound;
}

static MemFramesFreedListener g_frames_freed_listener;

void mem_set_frames_freed_listener(MemFramesFreedListener listener) {
    g_frames_freed_listener = listener;
}

u8* mem_physical_ptr(u64 pfn) {
    AcquireSRWLockExclusive(&g_mem.phys_lock);
    u32 id = frame_of_locked(pfn);
    u32 real = id != MEM_INVALID_PFN ? bind_frame(id, MEM_INVALID_PFN) : MEM_INVALID_PFN;
    ReleaseSRWLockExclusive(&g_mem.phys_lock);
    return real != MEM_INVALID_PFN ? real_ptr(real) : nullptr;
}

bool mem_physical_alloc(u64 count, u64* pfns) {
    AcquireSRWLockExclusive(&g_mem.phys_lock);
    if (g_mem.used_pages + count > g_mem.total_pages) {
        ReleaseSRWLockExclusive(&g_mem.phys_lock);
        LOG_W("memory", "out of physical memory: want %llu pages, %llu free", count,
              g_mem.total_pages - g_mem.used_pages);
        return false;
    }
    u64 found = 0;
    u64 i = g_mem.id_hint;
    for (u64 scanned = 0; found < count && scanned < g_mem.total_pages; scanned++, i++) {
        if (i >= g_mem.total_pages)
            i = 0;
        if (!bit_get(g_mem.id_bitmap, i)) {
            bit_set(g_mem.id_bitmap, i);
            pfns[found++] = i << MEM_PAGE_NUMBER_SHIFT;
        }
    }
    SLUDGE_ASSERT(found == count);
    g_mem.id_hint = i < g_mem.total_pages ? i : 0;
    g_mem.used_pages += count;
    ReleaseSRWLockExclusive(&g_mem.phys_lock);
    return true;
}

// Frees frames without telling the listener (the caller does, with no locks held).
static void physical_free(u64 count, const u64* pfns) {
    AcquireSRWLockExclusive(&g_mem.phys_lock);
    for (u64 k = 0; k < count; k++) {
        u32 id = frame_of_locked(pfns[k]);
        if (id == MEM_INVALID_PFN || id == 0) {
            LOG_W("memory", "freeing invalid/unallocated page frame 0x%llx", pfns[k]);
            continue;
        }
        if (g_mem.frame_va[id])
            LOG_W_ONCE("memory", "frame 0x%x freed while still mapped at 0x%llx; that address goes on showing "
                       "memory given to others", id, g_mem.frame_va[id]);
        bit_clear(g_mem.id_bitmap, id);
        g_mem.used_pages--;
        tracking_frame_freed(id);
        u32 real = g_mem.id_to_real[id];
        if (real != MEM_INVALID_PFN) {
            g_mem.id_to_real[id] = MEM_INVALID_PFN;
            bit_clear(g_mem.real_bitmap, real);
            g_mem.bound_real--;
            bit_set(g_mem.dirty_bitmap, real);
            // Hand the RAM back; the frame is zeroed when it's bound again.
            DiscardVirtualMemory(real_ptr(real), MEM_PAGE_SIZE);
        }
    }
    ReleaseSRWLockExclusive(&g_mem.phys_lock);
}

void mem_physical_free(u64 count, const u64* pfns) {
    physical_free(count, pfns);
    if (g_frames_freed_listener)
        g_frames_freed_listener(count, pfns);
}

// ---------------------------------------------------------------------------------------------
// Ranges
//
// A placeholder range keeps a sorted list of regions that mirrors the address space exactly:
// every region is either one Win32 placeholder or one mapped view of consecutive page frames.
// All changes go through split_at / make_placeholder, which update Windows and the list together,
// so the two never disagree. A Win32 failure in there means they would, which is not recoverable.

static void region_insert(RangeSlot* s, u32 at, MemRegion region) {
    if (s->region_count == s->region_capacity) {
        u32 capacity = s->region_capacity ? s->region_capacity * 2 : 16;
        MemRegion* grown = (MemRegion*)realloc(s->regions, capacity * sizeof(MemRegion));
        if (!grown)
            FATAL("out of memory tracking mappings");
        s->regions = grown;
        s->region_capacity = capacity;
    }
    memmove(&s->regions[at + 1], &s->regions[at], (s->region_count - at) * sizeof(MemRegion));
    s->regions[at] = region;
    s->region_count++;
}

// Replaces regions [first, last] with one region.
static void region_replace(RangeSlot* s, u32 first, u32 last, MemRegion region) {
    s->regions[first] = region;
    u32 removed = last - first;
    if (removed) {
        memmove(&s->regions[first + 1], &s->regions[last + 1],
                (s->region_count - last - 1) * sizeof(MemRegion));
        s->region_count -= removed;
    }
}

static void win_split(u8* va, u64 bytes) {
    if (!VirtualFree(va, bytes, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER))
        FATAL("splitting placeholder at %p+0x%llx failed (%lu)", va, bytes, GetLastError());
}

static void win_coalesce(u8* va, u64 bytes) {
    if (!VirtualFree(va, bytes, MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS))
        FATAL("coalescing placeholders at %p (0x%llx bytes) failed (%lu)", va, bytes, GetLastError());
}

static void win_unmap_to_placeholder(u8* va) {
    if (!g_mem.unmap_view2(GetCurrentProcess(), va, MEM_PRESERVE_PLACEHOLDER))
        FATAL("unmapping view at %p failed (%lu)", va, GetLastError());
}

// Maps `pages` consecutive frames starting at `pfn` (all in one chunk) over a placeholder that
// spans exactly [va, va + pages * MEM_PAGE_SIZE).
static void win_map(const RangeSlot* s, u8* va, u32 pfn, u32 pages, u32 protect) {
    HANDLE chunk = g_mem.chunks[pfn / MEM_CHUNK_PAGES];
    u64 offset = (u64)(pfn % MEM_CHUNK_PAGES) << MEM_PAGE_SHIFT;
    void* v = g_mem.map_view3(chunk, GetCurrentProcess(), va, offset, (u64)pages << MEM_PAGE_SHIFT,
                              MEM_REPLACE_PLACEHOLDER, protect, nullptr, 0);
    if (v != va)
        FATAL("mapping frames 0x%x+%u at %p failed (%lu)", pfn, pages, va, GetLastError());
    tracking_view_mapped(s, (u32)((u64)(va - s->info.base) >> MEM_PAGE_SHIFT), pages, pfn);
}

// Ensures a region boundary at `page`. A view that straddles it is unmapped, split and mapped
// again as two views.
static void split_at(RangeSlot* s, u32 page) {
    if (page == 0 || page >= range_pages(s))
        return;
    u32 i = region_find(s, page);
    MemRegion r = s->regions[i];
    if (r.first_page == page)
        return;
    u32 end = region_end(s, i);
    u32 head = page - r.first_page;
    bool view = r.pfn != REGION_PLACEHOLDER;
    if (view)
        win_unmap_to_placeholder(page_va(s, r.first_page));
    win_split(page_va(s, r.first_page), (u64)head << MEM_PAGE_SHIFT);
    if (view) {
        u32 protect = mapping_protect(s);
        win_map(s, page_va(s, r.first_page), r.pfn, head, protect);
        win_map(s, page_va(s, page), r.pfn + head, end - page, protect);
    }
    MemRegion tail = {page, view ? r.pfn + head : REGION_PLACEHOLDER};
    region_insert(s, i + 1, tail);
}

// Turns [first, end) into exactly one placeholder and returns its region index.
static u32 make_placeholder(RangeSlot* s, u32 first, u32 end) {
    split_at(s, first);
    split_at(s, end);
    u32 i0 = region_find(s, first);
    u32 i1 = region_find(s, end - 1);
    for (u32 i = i0; i <= i1; i++) {
        if (s->regions[i].pfn != REGION_PLACEHOLDER)
            win_unmap_to_placeholder(page_va(s, s->regions[i].first_page));
    }
    if (i1 > i0)
        win_coalesce(page_va(s, first), (u64)(end - first) << MEM_PAGE_SHIFT);
    MemRegion merged = {first, REGION_PLACEHOLDER};
    region_replace(s, i0, i1, merged);
    return i0;
}

static void map_run(RangeSlot* s, u32 first, u32 count, u32 pfn) {
    u32 i = region_find(s, first);
    const MemRegion& r = s->regions[i];
    if (r.first_page == first && region_end(s, i) == first + count && r.pfn == pfn)
        return; // already mapped exactly like this
    i = make_placeholder(s, first, first + count);
    win_map(s, page_va(s, first), pfn, count, mapping_protect(s));
    s->regions[i].pfn = pfn;
}

static void unmap_run(RangeSlot* s, u32 first, u32 count) {
    make_placeholder(s, first, first + count);
}

bool mem_query_range(const void* addr, MemRange* out) {
    AcquireSRWLockShared(&g_mem.range_lock);
    RangeSlot* s = find_range_locked(addr);
    if (s)
        *out = s->info;
    ReleaseSRWLockShared(&g_mem.range_lock);
    return s != nullptr;
}

bool mem_backing_block(const void* addr, MemBlock* out) {
    bool ok = false;
    AcquireSRWLockShared(&g_mem.range_lock);
    RangeSlot* s = find_range_locked(addr);
    if (s && (s->info.flags & MEM_RANGE_SECTION)) {
        *out = {s->info.base, s->info.size, s->host}; // the GPU sees the section through our view
        ok = true;
    } else if (s && !(s->info.flags & MEM_RANGE_PLACEHOLDER)) {
        *out = {s->info.base, s->info.size, s->info.base}; // one VirtualAlloc allocation
        ok = true;
    } else if (s) {
        u32 page = (u32)((u64)((const u8*)addr - s->info.base) >> MEM_PAGE_SHIFT);
        u32 index = region_find(s, page);
        const MemRegion* r = s->regions;
        if (r[index].pfn != REGION_PLACEHOLDER) {
            // Neighbouring views continue the block while their frames continue the sequence
            // within the chunk.
            u32 chunk = r[index].pfn / (u32)MEM_CHUNK_PAGES;
            u32 first = index, last = index;
            while (first > 0 && r[first - 1].pfn != REGION_PLACEHOLDER &&
                   r[first - 1].pfn + (r[first].first_page - r[first - 1].first_page) == r[first].pfn &&
                   r[first - 1].pfn / (u32)MEM_CHUNK_PAGES == chunk)
                first--;
            while (last + 1 < s->region_count && r[last + 1].pfn != REGION_PLACEHOLDER &&
                   r[last].pfn + (r[last + 1].first_page - r[last].first_page) == r[last + 1].pfn &&
                   r[last + 1].pfn / (u32)MEM_CHUNK_PAGES == chunk)
                last++;
            out->base = page_va(s, r[first].first_page);
            out->size = (u64)(region_end(s, last) - r[first].first_page) << MEM_PAGE_SHIFT;
            out->host = real_ptr(r[first].pfn);
            ok = true;
        }
    }
    ReleaseSRWLockShared(&g_mem.range_lock);
    return ok;
}

// --- where frames are mapped ---------------------------------------------------------------------

// Frame `id` is now mapped at `va` too. Range lock held exclusively.
static void note_mapped(u32 id, u64 va) {
    if (!g_mem.frame_va[id]) {
        g_mem.frame_va[id] = va;
        return;
    }
    if (g_mem.alias_count == g_mem.alias_capacity) {
        u32 capacity = g_mem.alias_capacity ? g_mem.alias_capacity * 2 : 1024;
        FrameAlias* grown = (FrameAlias*)realloc(g_mem.aliases, capacity * sizeof(FrameAlias));
        if (!grown)
            FATAL("out of memory tracking where frames are mapped");
        g_mem.aliases = grown;
        g_mem.alias_capacity = capacity;
    }
    u32 at = alias_lower_bound(id);
    memmove(&g_mem.aliases[at + 1], &g_mem.aliases[at], (g_mem.alias_count - at) * sizeof(FrameAlias));
    g_mem.aliases[at] = {id, va};
    g_mem.alias_count++;
}

// Frame `id` is no longer mapped at `va`. Range lock held exclusively.
static void note_unmapped(u32 id, u64 va) {
    u32 at = alias_lower_bound(id);
    if (g_mem.frame_va[id] == va) {
        // The first further address, if any, becomes the first.
        if (at < g_mem.alias_count && g_mem.aliases[at].id == id) {
            g_mem.frame_va[id] = g_mem.aliases[at].va;
        } else {
            g_mem.frame_va[id] = 0;
            return;
        }
    } else {
        while (at < g_mem.alias_count && g_mem.aliases[at].id == id && g_mem.aliases[at].va != va)
            at++;
        if (at == g_mem.alias_count || g_mem.aliases[at].id != id)
            return;
    }
    memmove(&g_mem.aliases[at], &g_mem.aliases[at + 1], (g_mem.alias_count - at - 1) * sizeof(FrameAlias));
    g_mem.alias_count--;
}

bool mem_frames_at(const void* va, u64 count, u64* pfns) {
    AcquireSRWLockShared(&g_mem.range_lock);
    RangeSlot* s = find_range_locked(va);
    u64 first = s ? (u64)((const u8*)va - s->info.base) >> MEM_PAGE_SHIFT : 0;
    bool ok = s && (s->info.flags & MEM_RANGE_PLACEHOLDER) && first + count <= range_pages(s);
    if (ok) {
        AcquireSRWLockShared(&g_mem.phys_lock);
        for (u64 i = 0; i < count; i++) {
            u32 id = frame_at_locked(s, (u32)(first + i));
            pfns[i] = id == MEM_INVALID_PFN ? MEM_INVALID_PFN : (u64)id << MEM_PAGE_NUMBER_SHIFT;
        }
        ReleaseSRWLockShared(&g_mem.phys_lock);
    }
    ReleaseSRWLockShared(&g_mem.range_lock);
    return ok;
}

bool mem_is_mapped(const void* addr, u64 size) {
    const u8* p = (const u8*)addr;
    const u8* end = p + size;
    bool ok = true;
    AcquireSRWLockShared(&g_mem.range_lock);
    AcquireSRWLockShared(&g_mem.phys_lock);
    while (ok && p < end) {
        RangeSlot* s = find_range_locked(p);
        if (!s) {
            ok = false;
            break;
        }
        const u8* range_end = s->info.base + s->info.size;
        const u8* stop = end < range_end ? end : range_end;
        if (s->info.flags & MEM_RANGE_PLACEHOLDER) {
            u32 first = (u32)((u64)(p - s->info.base) >> MEM_PAGE_SHIFT);
            u32 last = (u32)(((u64)(stop - s->info.base) + MEM_PAGE_SIZE - 1) >> MEM_PAGE_SHIFT);
            for (u32 page = first; ok && page < last; page++)
                ok = frame_at_locked(s, page) != MEM_INVALID_PFN;
        }
        p = stop;
    }
    ReleaseSRWLockShared(&g_mem.phys_lock);
    ReleaseSRWLockShared(&g_mem.range_lock);
    return ok;
}

bool mem_any_pending(const void* addr, u64 size) {
    const u8* p = (const u8*)addr;
    const u8* end = p + size;
    bool any = false;
    AcquireSRWLockShared(&g_mem.range_lock);
    AcquireSRWLockShared(&g_mem.phys_lock);
    while (!any && p < end) {
        RangeSlot* s = find_range_locked(p);
        if (!s)
            break;
        const u8* range_end = s->info.base + s->info.size;
        const u8* stop = end < range_end ? end : range_end;
        if ((s->info.flags & MEM_RANGE_PLACEHOLDER) && s->pending) {
            u32 first = (u32)((u64)(p - s->info.base) >> MEM_PAGE_SHIFT);
            u32 last = (u32)(((u64)(stop - s->info.base) + MEM_PAGE_SIZE - 1) >> MEM_PAGE_SHIFT);
            // A page waits for its first touch here even when its frame is bound already (written
            // through another address that shows it): only an unbound frame reads as zero.
            for (u32 page = first; !any && page < last; page++) {
                u32 id = s->pending[page];
                any = id != MEM_INVALID_PFN && g_mem.id_to_real[id] == MEM_INVALID_PFN;
            }
        }
        p = stop;
    }
    ReleaseSRWLockShared(&g_mem.phys_lock);
    ReleaseSRWLockShared(&g_mem.range_lock);
    return any;
}

void mem_add_mapping_listener(MemMappingListener listener) {
    long n = g_mem.mapping_listener_count;
    if (n == MAX_MAPPING_LISTENERS)
        FATAL("too many memory mapping listeners");
    g_mem.mapping_listeners[n] = listener;
    InterlockedExchange(&g_mem.mapping_listener_count, n + 1); // publishes the entry
}

static void notify_mapping_changed(const u8* base, u64 size) {
    tracking_mapping_changed(base, size); // other frames there now
    long n = g_mem.mapping_listener_count;
    for (long i = 0; i < n; i++)
        g_mem.mapping_listeners[i](base, size);
}

bool mem_is_tracked(const void* addr) {
    MemRange r;
    return mem_query_range(addr, &r);
}

void mem_set_range_protect(const void* addr, u32 protect) {
    AcquireSRWLockExclusive(&g_mem.range_lock);
    if (RangeSlot* s = find_range_locked(addr)) {
        s->info.protect = protect;
        // A range reserved without access and given some later is the CPU's too from now on.
        if ((protect & 0xFF) != PAGE_NOACCESS && (protect & 0xFF) != 0)
            s->info.flags |= MEM_RANGE_CPU_ACCESS;
    }
    ReleaseSRWLockExclusive(&g_mem.range_lock);
}

// Publishes a fully initialized range. `regions` and `anchors` (placeholder ranges only) are owned
// by the range from here on.
static void add_range(u8* base, u64 size, u32 flags, u32 protect, u64 xflags, MemRegion* regions,
                      u32* anchors, HANDLE section = nullptr, u8* host = nullptr) {
    AcquireSRWLockExclusive(&g_mem.range_lock);
    if (g_mem.range_count >= MAX_RANGES) {
        ReleaseSRWLockExclusive(&g_mem.range_lock);
        FATAL("too many memory ranges");
    }
    RangeSlot& s = g_mem.ranges[g_mem.range_count++];
    s.info.base = base;
    s.info.size = size;
    s.info.flags = flags;
    s.info.protect = protect;
    s.info.xflags = xflags;
    s.regions = regions;
    s.region_count = regions ? 1 : 0;
    s.region_capacity = regions ? 1 : 0;
    s.anchors = anchors;
    s.pending = nullptr;
    s.section = section;
    s.host = host;
    ReleaseSRWLockExclusive(&g_mem.range_lock);
}

static void fill_address_requirements(const MemAddressLimits& limits, MEM_ADDRESS_REQUIREMENTS* req) {
    memset(req, 0, sizeof(*req));
    req->LowestStartingAddress = (PVOID)ALIGN_UP(limits.lowest, MEM_PAGE_SIZE);
    if (limits.highest)
        req->HighestEndingAddress = (PVOID)(ALIGN_DOWN(limits.highest + 1, MEM_PAGE_SIZE) - 1);
    req->Alignment = limits.alignment > MEM_PAGE_SIZE ? limits.alignment : 0;
}

void* mem_reserve_placeholder(u64 size, const MemAddressLimits& limits, u32 range_flags, u32 protect,
                              u64 xflags) {
    size = ALIGN_UP(size, MEM_PAGE_SIZE);
    if (size == 0 || (size >> MEM_PAGE_SHIFT) > 0xFFFFFFFFull) {
        LOG_E("memory", "placeholder reservation of 0x%llx bytes is not supported", size);
        return nullptr;
    }
    MemRegion* regions = (MemRegion*)malloc(sizeof(MemRegion));
    u64 slices = ((size >> MEM_PAGE_SHIFT) + REGION_PAGES - 1) / REGION_PAGES;
    u32* anchors = (u32*)malloc(slices * sizeof(u32));
    if (!regions || !anchors)
        FATAL("out of memory tracking mappings");
    regions[0].first_page = 0;
    regions[0].pfn = REGION_PLACEHOLDER;
    memset(anchors, 0xFF, slices * sizeof(u32));

    MEM_ADDRESS_REQUIREMENTS req;
    fill_address_requirements(limits, &req);
    MEM_EXTENDED_PARAMETER param = {};
    param.Type = MemExtendedParameterAddressRequirements;
    param.Pointer = &req;
    u8* base = (u8*)g_mem.virtual_alloc2(GetCurrentProcess(), nullptr, size,
                                         MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, &param, 1);
    if (!base) {
        LOG_E("memory", "placeholder reservation of %llu MiB failed (%lu)", size >> 20, GetLastError());
        free(regions);
        free(anchors);
        return nullptr;
    }
    add_range(base, size, range_flags | MEM_RANGE_PLACEHOLDER, protect, xflags, regions, anchors);
    LOG_I("memory", "placeholder range %p..%p (%llu MiB)%s", base, base + size, size >> 20,
          (range_flags & MEM_RANGE_GPU) ? " [gpu]" : "");
    return base;
}

// A GPU-visible range: a section viewed by the title where it asked, and by us elsewhere. Pages are
// committed through the title's view, now or (lazy) as they are touched.
static void* alloc_section_range(u64 size, bool commit, bool lazy, u32 protect,
                                 const MemAddressLimits& limits, u32 range_flags, u64 xflags) {
    size = ALIGN_UP(size, MEM_PAGE_SIZE);
    HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE | SEC_RESERVE,
                                        (DWORD)(size >> 32), (DWORD)size, nullptr);
    if (!section) {
        LOG_E("memory", "cannot create a %llu MiB section for GPU memory (%lu)", size >> 20, GetLastError());
        return nullptr;
    }
    MEM_ADDRESS_REQUIREMENTS req;
    fill_address_requirements(limits, &req);
    MEM_EXTENDED_PARAMETER param = {};
    param.Type = MemExtendedParameterAddressRequirements;
    param.Pointer = &req;
    // Caching follows the section: plain write-back, which the GPU snoops.
    u32 page_protect =
        (protect & 0xFF) == PAGE_NOACCESS || !(protect & 0xFF) ? PAGE_READWRITE : protect & 0xFF;
    // Views take only some placement requirements; a placeholder takes them all and the view replaces it.
    u8* placeholder = (u8*)g_mem.virtual_alloc2(
        GetCurrentProcess(), nullptr, size, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, &param, 1);
    u8* title = placeholder ? (u8*)g_mem.map_view3(section, GetCurrentProcess(), placeholder, 0, size,
                                                   MEM_REPLACE_PLACEHOLDER, page_protect, nullptr, 0)
                            : nullptr;
    if (placeholder && !title)
        VirtualFree(placeholder, 0, MEM_RELEASE);
    u8* host = title ? (u8*)g_mem.map_view3(section, GetCurrentProcess(), nullptr, 0, size, 0, PAGE_READWRITE,
                                            nullptr, 0)
                     : nullptr;
    if (!host) {
        LOG_E("memory", "cannot map a %llu MiB GPU memory section (%lu)", size >> 20, GetLastError());
        if (title)
            g_mem.unmap_view2(GetCurrentProcess(), title, 0);
        CloseHandle(section);
        return nullptr;
    }
    if (commit && !lazy && !VirtualAlloc(title, size, MEM_COMMIT, page_protect)) {
        LOG_E("memory", "cannot commit %llu MiB of GPU memory (%lu)", size >> 20, GetLastError());
        g_mem.unmap_view2(GetCurrentProcess(), host, 0);
        g_mem.unmap_view2(GetCurrentProcess(), title, 0);
        CloseHandle(section);
        return nullptr;
    }
    add_range(title, size, range_flags | MEM_RANGE_SECTION | (lazy ? MEM_RANGE_LAZY : 0), page_protect,
              xflags, nullptr, nullptr, section, host);
    return title;
}

void* mem_alloc_virtual(void* base, u64 size, u32 alloc_type, u32 protect, const MemAddressLimits& limits,
                        u32 range_flags, u64 xflags) {
    void* p;
    if (!base && (range_flags & MEM_RANGE_GPU) && (alloc_type & (MEM_RESERVE | MEM_COMMIT))) {
        bool commit = (alloc_type & MEM_COMMIT) != 0;
        return alloc_section_range(size, commit, commit && size >= LAZY_COMMIT_MIN, protect, limits,
                                   range_flags, xflags);
    }
    // A title's big commits are pools it fills over time: reserve them, commit as they're touched.
    bool lazy = !base && (alloc_type & MEM_COMMIT) && size >= LAZY_COMMIT_MIN;
    if (lazy) {
        alloc_type = (alloc_type & ~MEM_COMMIT) | MEM_RESERVE;
        range_flags |= MEM_RANGE_LAZY;
    }
    if (base) {
        // Committing inside an existing reservation (or a fixed-address request): no constraints.
        // Section views commit with the protection they were mapped with (caching is the section's).
        MemRange r;
        if (mem_query_range(base, &r) && (r.flags & MEM_RANGE_SECTION))
            protect = r.protect;
        // Committing pages already committed resets their protection: tracking protects them again,
        // under the range lock so that no access sees them in between.
        AcquireSRWLockExclusive(&g_mem.range_lock);
        p = VirtualAlloc(base, size, alloc_type, protect);
        RangeSlot* s = p && (alloc_type & MEM_COMMIT) ? find_range_locked(base) : nullptr;
        if (s && !(s->info.flags & MEM_RANGE_PLACEHOLDER)) {
            u64 lo = ALIGN_DOWN((u64)base, (u64)HOST_PAGE), hi = ALIGN_UP((u64)base + size, (u64)HOST_PAGE);
            u64 end = (u64)s->info.base + s->info.size;
            tracking_committed(s, lo, hi < end ? hi : end);
        }
        ReleaseSRWLockExclusive(&g_mem.range_lock);
    } else {
        MEM_ADDRESS_REQUIREMENTS req;
        fill_address_requirements(limits, &req);
        MEM_EXTENDED_PARAMETER param = {};
        param.Type = MemExtendedParameterAddressRequirements;
        param.Pointer = &req;
        p = g_mem.virtual_alloc2(GetCurrentProcess(), nullptr, size, alloc_type, protect, &param, 1);
    }
    if (!p)
        return nullptr;
    // Fresh reservations are tracked; commits into an existing tracked range are not new ranges.
    if (!base || !mem_is_tracked(base)) {
        if (range_flags & (MEM_RANGE_GPU | MEM_RANGE_LAZY))
            add_range((u8*)p, ALIGN_UP(size, MEM_PAGE_SIZE), range_flags, protect, xflags, nullptr, nullptr);
    }
    return p;
}

static bool frame_allocated(u64 pfn) {
    AcquireSRWLockShared(&g_mem.phys_lock);
    bool ok = frame_of_locked(pfn) != MEM_INVALID_PFN;
    ReleaseSRWLockShared(&g_mem.phys_lock);
    return ok;
}

// Where a frame mapped at `page` of a placeholder range should live in real memory. Each slice of
// REGION_PAGES pages of the range claims a free region when it first needs one and lays its pages
// out in it in address order, so pages the title maps next to each other are next to each other in
// memory, whatever order it maps them in. That is what lets the GPU see a range of title memory as
// one block (see MemBlock); slices touched in order usually get neighbouring regions, which extends
// blocks across them. Without a region to claim, a page follows its predecessor. Range lock and
// phys_lock held.
static u32 preferred_real(RangeSlot* s, u32 page, u32 previous) {
    // Only memory the GPU reads in place needs this: a window over title memory is one block of real
    // memory (GPU-visible memory the CPU can access; the page tables serve the rest, and CPU-only
    // views don't need it). Other frames fill holes in the chunks there are (find_free_real).
    if ((s->info.flags & (MEM_RANGE_GPU | MEM_RANGE_CPU_ACCESS)) != (MEM_RANGE_GPU | MEM_RANGE_CPU_ACCESS))
        return MEM_INVALID_PFN;
    u32 slice = page / REGION_PAGES;
    if (s->anchors[slice] == MEM_INVALID_PFN)
        s->anchors[slice] = claim_free_region();
    if (s->anchors[slice] != MEM_INVALID_PFN)
        return s->anchors[slice] + page % REGION_PAGES;
    if (previous == MEM_INVALID_PFN && page > 0) {
        const MemRegion& r = s->regions[region_find(s, page - 1)];
        if (r.pfn != REGION_PLACEHOLDER)
            previous = r.pfn + (page - 1 - r.first_page);
    }
    return previous == MEM_INVALID_PFN ? MEM_INVALID_PFN : previous + 1;
}

// Gives up the range's claims on regions. Range lock held.
static void release_anchors(RangeSlot* s) {
    u32 slices = (range_pages(s) + REGION_PAGES - 1) / REGION_PAGES;
    AcquireSRWLockExclusive(&g_mem.phys_lock);
    for (u32 i = 0; i < slices; i++) {
        if (s->anchors[i] != MEM_INVALID_PFN)
            g_mem.region_claimed[s->anchors[i] / REGION_PAGES] = false;
    }
    ReleaseSRWLockExclusive(&g_mem.phys_lock);
    free(s->anchors);
    s->anchors = nullptr;
}

// Backs the frames `ids` (MEM_INVALID_PFN: none) at pages [first, first + count) and maps them, one
// view per run of consecutive real frames within a chunk. Range lock held exclusively.
static void back_and_map_locked(RangeSlot* s, u32 first, u32 count, const u32* ids) {
    u32* reals = (u32*)malloc(count * sizeof(u32));
    if (!reals)
        FATAL("out of memory mapping %u pages", count);
    AcquireSRWLockExclusive(&g_mem.phys_lock);
    for (u32 i = 0; i < count; i++) {
        u32 previous = i > 0 ? reals[i - 1] : MEM_INVALID_PFN;
        reals[i] = ids[i] == MEM_INVALID_PFN ? MEM_INVALID_PFN
                                             : bind_frame(ids[i], preferred_real(s, first + i, previous));
    }
    ReleaseSRWLockExclusive(&g_mem.phys_lock);
    for (u32 i = 0; i < count;) {
        u32 real = reals[i];
        u32 run = 1;
        if (real == MEM_INVALID_PFN) {
            while (i + run < count && reals[i + run] == MEM_INVALID_PFN)
                run++;
            unmap_run(s, first + i, run);
        } else {
            while (i + run < count && reals[i + run] == real + run && (real + run) % MEM_CHUNK_PAGES != 0)
                run++;
            map_run(s, first + i, run, real);
        }
        i += run;
    }
    free(reals);
}

// --- Lazy backing ------------------------------------------------------------------------------------
//
// Titles map far more frames than they touch (whole pools at once), and every backed frame costs
// commit charge. So a mapped frame stays pending, its page a placeholder, until the page is first
// touched: by the CPU (an access fault, see on_access_fault) or by us for the GPU and for kernel I/O
// (mem_prepare). Then the pending frames of the page's whole slice are backed together, which keeps
// the slice's frames consecutive in one region for the GPU and makes later touches nearby free.

// Backs the pending frames of the slice holding `page`. Range lock held exclusively.
static void back_slice_locked(RangeSlot* s, u32 page) {
    u32 lo = page - page % REGION_PAGES;
    u32 hi = lo + REGION_PAGES < range_pages(s) ? lo + REGION_PAGES : range_pages(s);
    for (u32 p = lo; p < hi;) {
        if (s->pending[p] == MEM_INVALID_PFN) {
            p++;
            continue;
        }
        u32 run = 1;
        while (p + run < hi && s->pending[p + run] != MEM_INVALID_PFN)
            run++;
        back_and_map_locked(s, p, run, &s->pending[p]);
        memset(&s->pending[p], 0xFF, run * sizeof(u32));
        p += run;
    }
}

// Backs the pending frames under [addr, addr + size) of a placeholder range. Range lock held
// exclusively. Returns whether anything was pending.
static bool back_range_locked(RangeSlot* s, const u8* addr, u64 size) {
    if (!s->pending)
        return false;
    u32 first = (u32)((u64)(addr - s->info.base) >> MEM_PAGE_SHIFT);
    u64 end = ((u64)(addr - s->info.base) + size + MEM_PAGE_SIZE - 1) >> MEM_PAGE_SHIFT;
    u32 last = (u32)(end < range_pages(s) ? end : range_pages(s));
    bool any = false;
    for (u32 p = first; p < last; p++) {
        if (s->pending[p] != MEM_INVALID_PFN) {
            back_slice_locked(s, p);
            any = true;
        }
    }
    return any;
}

// Commits the granules of a lazily committed range under [addr, addr + size). Commits already
// there stay. Range lock held.
static bool commit_lazy_locked(const RangeSlot* s, const u8* addr, u64 size) {
    const u8* range_end = s->info.base + s->info.size;
    u8* lo = (u8*)ALIGN_DOWN((uptr)addr, LAZY_COMMIT_GRANULE);
    u8* hi = (u8*)ALIGN_UP((uptr)(addr + size), LAZY_COMMIT_GRANULE);
    lo = lo > s->info.base ? lo : s->info.base;
    hi = hi < range_end ? hi : (u8*)range_end;
    u64 reserved = 0; // what committing charges: the pages not committed yet
    for (const u8* p = lo; p < hi;) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(p, &mbi, sizeof(mbi)))
            break;
        const u8* stop = (const u8*)mbi.BaseAddress + mbi.RegionSize;
        stop = stop < hi ? stop : hi;
        if (mbi.State == MEM_RESERVE)
            reserved += (u64)(stop - p);
        p = stop;
    }
    if (!reserved)
        return true;
    if (VirtualAlloc(lo, (u64)(hi - lo), MEM_COMMIT, mapping_protect(s))) {
        tracking_committed(s, (u64)lo, (u64)hi); // committing pages already there resets their protection
        return true;
    }
    LOG_E("memory", "cannot commit title memory %p+0x%llx (%lu)", lo, (u64)(hi - lo), GetLastError());
    return false;
}

void mem_prepare(const void* addr, u64 size) {
    const u8* p = (const u8*)addr;
    const u8* end = p + size;
    AcquireSRWLockExclusive(&g_mem.range_lock);
    while (p < end) {
        RangeSlot* s = find_range_locked(p);
        if (!s)
            break;
        const u8* range_end = s->info.base + s->info.size;
        u64 n = (u64)((end < range_end ? end : range_end) - p);
        if (s->info.flags & MEM_RANGE_PLACEHOLDER)
            back_range_locked(s, p, n);
        else if (s->info.flags & MEM_RANGE_LAZY)
            commit_lazy_locked(s, p, n);
        p = range_end;
    }
    ReleaseSRWLockExclusive(&g_mem.range_lock);
}

// Whether the page holding `addr` now allows the faulting access (0 read, 1 write, 8 execute).
bool access_allowed(const void* addr, ULONG_PTR access) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(addr, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD))
        return false;
    const u32 writable = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    const u32 readable = writable | PAGE_READONLY | PAGE_EXECUTE_READ;
    const u32 executable = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    u32 p = mbi.Protect & 0xFF;
    return access == 1 ? (p & writable) != 0 : access == 8 ? (p & executable) != 0 : (p & readable) != 0;
}

// Retries in a row of one address that tracking protects against but doesn't claim, per thread: an
// access retried again and again is one nobody claims (a fault that would otherwise loop forever).
#define MAX_RETRIES 64

static bool retry_allowed(u64 addr) {
    if (g_retry_address.get() != addr) {
        g_retry_address.set(addr);
        g_retry_count.set(0);
    }
    u64 n = g_retry_count.get() + 1;
    g_retry_count.set(n);
    if (n <= MAX_RETRIES)
        return true;
    LOG_E("memory", "an access to %p keeps faulting and nothing claims it", (void*)addr);
    return false;
}

// CPU access to title memory we haven't backed yet: back it and retry the access. Remapping a
// placeholder range can also leave a page briefly unmapped (a view is split by unmapping it and
// mapping its pieces again) while other threads use it; holding the range lock waits that out, and
// the access is retried if the page is there now.
static LONG access_fault(EXCEPTION_POINTERS* ep) {
    const EXCEPTION_RECORD* e = ep->ExceptionRecord;
    if (e->ExceptionCode == EXCEPTION_SINGLE_STEP)
        return tracking_single_step(ep) ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
    if (e->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || e->NumberParameters < 2)
        return EXCEPTION_CONTINUE_SEARCH;
    const u8* addr = (const u8*)e->ExceptionInformation[1];
    if (tracking_access_fault((u64)addr) ||
        (e->ExceptionInformation[0] == 1 && tracking_write_fault(ep, (u64)addr))) {
        g_retry_address.set(0); // claimed: retries start over
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    ULONG_PTR access = e->ExceptionInformation[0];
    AcquireSRWLockExclusive(&g_mem.range_lock);
    RangeSlot* s = find_range_locked(addr);
    bool backed = false;
    if (s && (s->info.flags & MEM_RANGE_PLACEHOLDER)) {
        backed = back_range_locked(s, addr, 1);
    } else if (s && (s->info.flags & MEM_RANGE_LAZY)) {
        // Only reserved pages are ours to commit.
        MEMORY_BASIC_INFORMATION mbi;
        backed = VirtualQuery(addr, &mbi, sizeof(mbi)) && mbi.State == MEM_RESERVE && commit_lazy_locked(s, addr, 1);
    }
    // Other threads may have changed the page since this one faulted (backed or committed it, protected
    // it for tracking, released it). With the range lock held exclusively its protection is settled: an
    // access it allows now is retried, and so is one tracking protects against (claimed when it faults
    // again). A few retries in a row on one address mean nobody claims it.
    bool allowed = !backed && s && access_allowed(addr, access);
    bool tracked = !backed && !allowed && s && tracking_protects_access(s, (u64)addr, access == 1);
    ReleaseSRWLockExclusive(&g_mem.range_lock);
    // A retry the page allows can't fault again by itself; one tracking protects against faults again
    // and is claimed then, unless tracking keeps declining it: only those count.
    bool retry = backed || allowed || (tracked && retry_allowed((u64)addr));
    if (backed || allowed)
        g_retry_address.set(0);
    if (!retry && s && access == 1)
        tracking_describe_write_fault((u64)addr);
    return retry ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
}

// The faulting thread's last error is the title's: whatever handling the fault takes, it stays.
static LONG CALLBACK on_access_fault(EXCEPTION_POINTERS* ep) {
    DWORD error = GetLastError();
    LONG result = access_fault(ep);
    SetLastError(error);
    return result;
}

// The frame a page mapping entry names, or MEM_INVALID_PFN. Titles release slots by mapping page
// number 0 over them (console page numbers start above it), so 0 unmaps like a missing array.
static u32 entry_frame(const u64* pfns, u64 i) {
    bool frame = pfns && pfns[i] != MEM_INVALID_PFN && pfns[i] != 0;
    return frame ? (u32)(pfns[i] >> MEM_PAGE_NUMBER_SHIFT) : MEM_INVALID_PFN;
}

// Maps (or unmaps, for MEM_INVALID_PFN entries or pfns == nullptr) `count` consecutive pages at
// `va`. Frames are backed when first touched (see "Lazy backing"). Pages that keep their frame are
// left as they are: titles map the same frames again often (whole pools every frame), and that is
// no change. [*changed_first, + *changed_count) gets the pages that did change (count 0: none).
// Range lock held exclusively.
static bool map_span_locked(void* va, u64 count, const u64* pfns, u64* changed_first, u64* changed_count) {
    *changed_first = 0;
    *changed_count = 0;
    RangeSlot* s = find_range_locked(va);
    if (!s || !(s->info.flags & MEM_RANGE_PLACEHOLDER) || ((uptr)va & (MEM_PAGE_SIZE - 1))) {
        LOG_E("memory", "physical map target %p is not a page in a placeholder range", va);
        return false;
    }
    u32 first = (u32)((u64)((u8*)va - s->info.base) >> MEM_PAGE_SHIFT);
    if (count > range_pages(s) - first) {
        LOG_E("memory", "physical map of %llu pages at %p runs past its range", count, va);
        return false;
    }
    // Validate everything before changing anything.
    for (u64 i = 0; pfns && i < count; i++) {
        if (pfns[i] != MEM_INVALID_PFN && pfns[i] != 0 && !frame_allocated(pfns[i])) {
            LOG_E("memory", "page 0x%llx mapped at %p is not an allocated page frame", pfns[i],
                  (u8*)va + (i << MEM_PAGE_SHIFT));
            return false;
        }
    }
    // The pages whose frame changes: [lo, hi) of the span.
    u64 lo = count, hi = 0;
    AcquireSRWLockShared(&g_mem.phys_lock);
    for (u64 i = 0; i < count; i++) {
        if (frame_at_locked(s, first + (u32)i) != entry_frame(pfns, i)) {
            lo = lo < i ? lo : i;
            hi = i + 1;
        }
    }
    ReleaseSRWLockShared(&g_mem.phys_lock);
    if (lo >= hi)
        return true;
    first += (u32)lo;
    pfns = pfns ? pfns + lo : nullptr;
    count = hi - lo;
    *changed_first = lo;
    *changed_count = count;

    if (pfns && !s->pending) {
        s->pending = (u32*)malloc((u64)range_pages(s) * sizeof(u32));
        if (!s->pending)
            FATAL("out of memory tracking mappings");
        memset(s->pending, 0xFF, (u64)range_pages(s) * sizeof(u32));
    }
    // Whatever was mapped there goes; the new frames wait for their first touch.
    AcquireSRWLockShared(&g_mem.phys_lock);
    for (u64 i = 0; i < count; i++) {
        u32 old = frame_at_locked(s, first + (u32)i);
        if (old != MEM_INVALID_PFN)
            note_unmapped(old, (u64)page_va(s, first + (u32)i));
    }
    ReleaseSRWLockShared(&g_mem.phys_lock);
    unmap_run(s, first, (u32)count);
    for (u64 i = 0; s->pending && i < count; i++) {
        s->pending[first + i] = entry_frame(pfns, i);
        if (s->pending[first + i] != MEM_INVALID_PFN)
            note_mapped(s->pending[first + i], (u64)page_va(s, first + (u32)i));
    }
    return true;
}

bool mem_map_physical(void* va, u64 count, const u64* pfns) {
    u64 first, changed;
    AcquireSRWLockExclusive(&g_mem.range_lock);
    bool ok = map_span_locked(va, count, pfns, &first, &changed);
    ReleaseSRWLockExclusive(&g_mem.range_lock);
    if (ok && changed)
        notify_mapping_changed((const u8*)va + (first << MEM_PAGE_SHIFT), changed << MEM_PAGE_SHIFT);
    return ok;
}

bool mem_map_physical_scatter(void* const* vas, u64 count, const u64* pfns) {
    // Entries at consecutive addresses form spans, which map_span_locked turns into runs. The changed
    // part of each span is noted for the listeners, called once the range lock is released.
    struct Change {
        const u8* base;
        u64 size;
    };
    Change local[16];
    Change* changes = local;
    u64 change_count = 0, change_capacity = ARRAY_COUNT(local);
    AcquireSRWLockExclusive(&g_mem.range_lock);
    bool ok = true;
    for (u64 i = 0; ok && i < count;) {
        u64 span = 1;
        while (i + span < count && (u8*)vas[i + span] == (u8*)vas[i] + (span << MEM_PAGE_SHIFT))
            span++;
        u64 first, changed;
        ok = map_span_locked(vas[i], span, pfns ? pfns + i : nullptr, &first, &changed);
        if (ok && changed) {
            if (change_count == change_capacity) {
                change_capacity *= 2;
                Change* grown = (Change*)malloc(change_capacity * sizeof(Change));
                if (!grown)
                    FATAL("out of memory mapping pages");
                memcpy(grown, changes, change_count * sizeof(Change));
                if (changes != local)
                    free(changes);
                changes = grown;
            }
            changes[change_count++] = {(const u8*)vas[i] + (first << MEM_PAGE_SHIFT),
                                       changed << MEM_PAGE_SHIFT};
        }
        i += span;
    }
    ReleaseSRWLockExclusive(&g_mem.range_lock);
    // Spans before a failed one did change.
    for (u64 i = 0; i < change_count; i++)
        notify_mapping_changed(changes[i].base, changes[i].size);
    if (changes != local)
        free(changes);
    return ok;
}

void* mem_alloc_physical_backed(u64 size, const MemAddressLimits& limits, u32 range_flags, u32 protect,
                                u64 xflags) {
    size = ALIGN_UP(size, MEM_PAGE_SIZE);
    u64 pages = size >> MEM_PAGE_SHIFT;
    u64* pfns = (u64*)VirtualAlloc(nullptr, pages * sizeof(u64), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!pfns)
        return nullptr;
    if (!mem_physical_alloc(pages, pfns)) {
        VirtualFree(pfns, 0, MEM_RELEASE);
        return nullptr;
    }
    void* base = mem_reserve_placeholder(size, limits, range_flags | MEM_RANGE_OWNS_PAGES, protect, xflags);
    if (!base) {
        mem_physical_free(pages, pfns);
    } else if (!mem_map_physical(base, pages, pfns)) {
        // A failed map changes nothing, so none of the frames is mapped (owned) yet.
        mem_release(base);
        mem_physical_free(pages, pfns);
        base = nullptr;
    }
    VirtualFree(pfns, 0, MEM_RELEASE);
    return base;
}

bool mem_release(void* base) {
    AcquireSRWLockExclusive(&g_mem.range_lock);
    RangeSlot* s = find_range_locked(base);
    if (!s || s->info.base != base) {
        ReleaseSRWLockExclusive(&g_mem.range_lock);
        return false;
    }
    tracking_range_released(s);
    // Frames the range owned, told to the listener once no lock is held (listeners take their own locks,
    // then ours).
    u64* freed = nullptr;
    u64 freed_count = 0;
    if ((s->info.flags & MEM_RANGE_PLACEHOLDER) && (s->info.flags & MEM_RANGE_OWNS_PAGES)) {
        freed = (u64*)malloc((u64)range_pages(s) * sizeof(u64));
        if (!freed)
            FATAL("out of memory releasing title memory");
    }
    if (s->info.flags & MEM_RANGE_PLACEHOLDER) {
        AcquireSRWLockShared(&g_mem.phys_lock);
        for (u32 p = 0; p < range_pages(s); p++) {
            u32 id = frame_at_locked(s, p);
            if (id != MEM_INVALID_PFN)
                note_unmapped(id, (u64)page_va(s, p));
        }
        ReleaseSRWLockShared(&g_mem.phys_lock);
        // Each region is one placeholder or one view, so each is released exactly once. Unmapping
        // a view without MEM_PRESERVE_PLACEHOLDER frees its address space as well.
        bool owns = (s->info.flags & MEM_RANGE_OWNS_PAGES) != 0;
        for (u32 i = 0; i < s->region_count; i++) {
            const MemRegion& r = s->regions[i];
            u8* va = page_va(s, r.first_page);
            bool ok;
            if (r.pfn == REGION_PLACEHOLDER) {
                ok = VirtualFree(va, 0, MEM_RELEASE) != FALSE;
            } else {
                ok = g_mem.unmap_view2(GetCurrentProcess(), va, 0) != FALSE;
                if (owns) {
                    for (u32 p = 0; p < region_end(s, i) - r.first_page; p++)
                        freed[freed_count++] = (u64)g_mem.real_to_id[r.pfn + p] << MEM_PAGE_NUMBER_SHIFT;
                }
            }
            if (!ok)
                LOG_E("memory", "releasing %p failed (%lu)", va, GetLastError());
        }
        // Frames that were never touched are owned too.
        for (u32 p = 0; owns && s->pending && p < range_pages(s); p++) {
            if (s->pending[p] != MEM_INVALID_PFN)
                freed[freed_count++] = (u64)s->pending[p] << MEM_PAGE_NUMBER_SHIFT;
        }
        free(s->pending);
        free(s->regions);
        release_anchors(s);
    } else if (s->info.flags & MEM_RANGE_SECTION) {
        // GPU heaps over our view keep its pages until the GPU is done with them (windows are retired
        // by the mapping notification below).
        if (!g_mem.unmap_view2(GetCurrentProcess(), s->info.base, 0) ||
            !g_mem.unmap_view2(GetCurrentProcess(), s->host, 0))
            LOG_E("memory", "releasing GPU memory %p failed (%lu)", s->info.base, GetLastError());
        CloseHandle(s->section);
    } else {
        VirtualFree(s->info.base, 0, MEM_RELEASE);
    }
    u64 size = s->info.size;
    *s = g_mem.ranges[--g_mem.range_count];
    ReleaseSRWLockExclusive(&g_mem.range_lock);
    notify_mapping_changed((const u8*)base, size);
    // Nothing shows the owned frames any more; they're free for others once the listener let go of them.
    if (freed_count && g_frames_freed_listener)
        g_frames_freed_listener(freed_count, freed);
    if (freed_count)
        physical_free(freed_count, freed);
    free(freed);
    return true;
}

void mem_set_gpu_window(u64 lowest, u64 highest) {
    g_mem.gpu_lowest = lowest;
    g_mem.gpu_highest = highest;
}

void mem_gpu_window(u64* lowest, u64* highest) {
    *lowest = g_mem.gpu_lowest;
    *highest = g_mem.gpu_highest;
}
