#include "gpu/gpu_window.h"

#include <stdlib.h>
#include <string.h>

#include "common/log.h"
#include "core/memory.h"

// Translations: title address ranges the GPU reaches through a window, sorted by title address and
// never overlapping. Dropping one frees nothing: windows live on their own terms (see below).
struct Window {
    u64 base;
    u64 end;
    u64 desktop_address; // of `base`
    ID3D12Resource* buffer;
    u64 buffer_offset; // of `base` in `buffer`
    bool own_heap;     // a window over memory outside physical memory (see PrivateWindow)
};

// Windows over title memory outside physical memory (GPU-visible allocations) each have a heap of
// their own, which pins their pages. When the GPU reaches more of an allocation, a larger window
// replaces the translation, but the old window stays open: recorded command lists and descriptors
// may still use its addresses. All of them close when the title remaps or releases their memory.
struct PrivateWindow {
    u64 base;
    u64 end;
    ID3D12Resource* buffer;
};

struct WindowList {
    Window* entries;
    u32 count;
    u32 capacity;
};

static struct {
    WindowList list;
    PrivateWindow* replaced; // private windows no translation uses any more
    u32 replaced_count;
    u32 replaced_capacity;
    SRWLOCK lock;
} g_windows = {{nullptr, 0, 0}, nullptr, 0, 0, SRWLOCK_INIT};

// Number of translations with base <= address. Lock held.
static u32 windows_at_or_below(u64 address) {
    u32 lo = 0, hi = g_windows.list.count;
    while (lo < hi) {
        u32 mid = lo + (hi - lo) / 2;
        if (g_windows.list.entries[mid].base <= address)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

static volatile long g_generation = 1;

u32 gpu_window_generation() {
    return (u32)g_generation;
}

// The translation containing `address`, or nullptr. Lock held.
static const Window* find_window(u64 address) {
    u32 at = windows_at_or_below(address);
    if (at > 0 && address < g_windows.list.entries[at - 1].end)
        return &g_windows.list.entries[at - 1];
    return nullptr;
}

static void keep_replaced(const Window& w) {
    if (g_windows.replaced_count == g_windows.replaced_capacity) {
        u32 capacity = g_windows.replaced_capacity ? g_windows.replaced_capacity * 2 : 16;
        PrivateWindow* grown = (PrivateWindow*)realloc(g_windows.replaced, capacity * sizeof(PrivateWindow));
        if (!grown)
            FATAL("out of memory tracking GPU windows");
        g_windows.replaced = grown;
        g_windows.replaced_capacity = capacity;
    }
    g_windows.replaced[g_windows.replaced_count++] = {w.base, w.end, w.buffer};
}

// Drops the translations overlapping [start, end). Their private windows close if the memory under
// them changed (`changed`), and stay open otherwise (a larger window replaces them). Lock held
// exclusively.
static void remove_windows_locked(u64 start, u64 end, bool changed) {
    u32 last = windows_at_or_below(end - 1);
    u32 first = last;
    while (first > 0 && g_windows.list.entries[first - 1].end > start)
        first--;
    for (u32 i = first; i < last; i++) {
        const Window& w = g_windows.list.entries[i];
        if (!w.own_heap)
            continue; // a physical memory chunk's window: open for good
        if (changed)
            gpu_retire(w.buffer);
        else
            keep_replaced(w);
    }
    memmove(&g_windows.list.entries[first], &g_windows.list.entries[last],
            (g_windows.list.count - last) * sizeof(Window));
    g_windows.list.count -= last - first;
}

static void fill(const Window* w, u64 address, GpuAddress* out) {
    out->buffer = w->buffer;
    out->offset = w->buffer_offset + (address - w->base);
    out->desktop_address = w->desktop_address + (address - w->base);
    out->available = w->end - address;
}

// Translations over large blocks (a title's multi-gigabyte pools) cover only the granules in use, so
// only memory the GPU touches gets backed and pinned. Translations already in the way are merged in,
// which keeps them from overlapping; the new one replaces them.
#define WINDOW_GRANULE MiB(64)

static MemBlock window_extent(const MemBlock& block, u64 address, u64 size) {
    // Physical memory is seen through the chunk's window whatever the extent, so translations there
    // cover what is accessed and back nothing more (memory only the GPU uses stays in video memory).
    u32 chunk;
    u64 chunk_offset;
    u64 granule = mem_linear_location(block.host, &chunk, &chunk_offset) ? MEM_PAGE_SIZE : WINDOW_GRANULE;
    u64 block_end = (u64)block.base + block.size;
    u64 lo = ALIGN_DOWN(address, granule);
    u64 hi = ALIGN_UP(address + size, granule);
    lo = lo > (u64)block.base ? lo : (u64)block.base;
    hi = hi < block_end ? hi : block_end;
    AcquireSRWLockShared(&g_windows.lock);
    for (u32 i = windows_at_or_below(hi - 1); i-- > 0 && g_windows.list.entries[i].end > lo;) {
        if (g_windows.list.entries[i].base < lo)
            lo = g_windows.list.entries[i].base;
        if (g_windows.list.entries[i].end > hi)
            hi = g_windows.list.entries[i].end;
    }
    ReleaseSRWLockShared(&g_windows.lock);
    lo = lo > (u64)block.base ? lo : (u64)block.base; // a translation can't reach past its block
    hi = hi < block_end ? hi : block_end;
    return {(u8*)lo, hi - lo, block.host + (lo - (u64)block.base)};
}

// A capture tool (RenderDoc) is loaded: windows are mapped for it (see place_window).
static bool g_capture_tool;

// A buffer over [offset, offset + size) of `heap`; takes over the caller's heap reference.
static ID3D12Resource* place_window(ID3D12Heap* heap, u64 offset, u64 size) {
    D3D12_RESOURCE_DESC1 desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ID3D12Resource* buffer = nullptr;
    HRESULT hr = gpu_device()->CreatePlacedResource2(heap, offset, &desc, D3D12_BARRIER_LAYOUT_UNDEFINED,
                                                     nullptr, 0, nullptr, IID_PPV_ARGS(&buffer));
    heap->Release(); // the buffer keeps it alive
    if (FAILED(hr)) {
        gpu_device_lost(hr);
        LOG_E("gpu", "cannot place a window buffer of 0x%llx bytes (0x%08lx)", size, (unsigned long)hr);
        return nullptr;
    }
    // RenderDoc sees CPU writes to buffer memory only through mappings it knows of; mapping windows
    // keeps captures faithful to what the title wrote during the frame (constant buffers above all).
    if (g_capture_tool) {
        void* mapped = nullptr;
        D3D12_RANGE none = {0, 0};
        if (FAILED(buffer->Map(0, &none, &mapped)))
            LOG_W_ONCE("gpu", "cannot map window buffers for the capture tool; captures miss CPU writes");
    }
    return buffer;
}

// Physical memory chunks are sections, which the GPU sees through heaps opened over their views in
// the linear view (mapped for the life of the process). Not over the section handles: heaps opened
// from file mappings keep the section's commit charged after the heap, the section and even the
// process are gone, so every run would leak all the physical memory it touched. Each chunk gets one
// window over all of it, opened when the GPU first reaches the chunk and kept: chunks live as long
// as the process, and so every address handed out for physical memory stays valid.
struct ChunkWindow {
    ID3D12Resource* buffer;
    u64 desktop_address;
};

static ChunkWindow* g_chunk_windows = nullptr;
static SRWLOCK g_chunk_lock = SRWLOCK_INIT;

static const ChunkWindow* chunk_window(u32 chunk, void* chunk_view) {
    AcquireSRWLockExclusive(&g_chunk_lock);
    if (!g_chunk_windows)
        g_chunk_windows = (ChunkWindow*)calloc(mem_physical_chunk_count(), sizeof(ChunkWindow));
    ChunkWindow* w = g_chunk_windows ? &g_chunk_windows[chunk] : nullptr;
    if (w && !w->buffer) {
        ID3D12Heap* heap = nullptr;
        HRESULT hr =
            gpu_device()->OpenExistingHeapFromAddress1(chunk_view, MEM_CHUNK_SIZE, IID_PPV_ARGS(&heap));
        if (SUCCEEDED(hr)) {
            // Named as well as the buffer: debug layer messages may name the heap.
            gpu_set_name(heap, "window heap over physical memory chunk %u", chunk);
            w->buffer = place_window(heap, 0, MEM_CHUNK_SIZE);
            if (w->buffer) {
                w->desktop_address = w->buffer->GetGPUVirtualAddress();
                gpu_set_name(w->buffer, "window over physical memory chunk %u", chunk);
            }
        } else {
            gpu_device_lost(hr);
            LOG_E("gpu", "cannot open a GPU heap over physical memory chunk %u (0x%08lx)", chunk,
                  (unsigned long)hr);
        }
    }
    ReleaseSRWLockExclusive(&g_chunk_lock);
    return w && w->buffer ? w : nullptr;
}

// A window of its own over a block outside physical memory.
static ID3D12Resource* open_private_window(const MemBlock& block) {
    ID3D12Heap* heap = nullptr;
    HRESULT hr = gpu_device()->OpenExistingHeapFromAddress1(block.host, block.size, IID_PPV_ARGS(&heap));
    if (FAILED(hr)) {
        gpu_device_lost(hr);
        LOG_E("gpu", "cannot open a GPU heap over title memory %p+0x%llx (0x%08lx)", block.base, block.size,
              (unsigned long)hr);
        return nullptr;
    }
    gpu_set_name(heap, "window heap over title memory %p+0x%llx", block.base, block.size);
    ID3D12Resource* buffer = place_window(heap, 0, block.size);
    if (buffer)
        gpu_set_name(buffer, "window over title memory %p+0x%llx", block.base, block.size);
    return buffer;
}

static bool translate(u64 address, u64 size, bool quiet, GpuAddress* out) {
    if (size == 0)
        size = 1;
    AcquireSRWLockShared(&g_windows.lock);
    const Window* w = find_window(address);
    bool ok = w && size <= w->end - address;
    if (ok)
        fill(w, address, out);
    ReleaseSRWLockShared(&g_windows.lock);
    if (ok)
        return true;

    // No translation yet, or one too short: the block may reach further.
    MemBlock block;
    mem_prepare((const void*)address, size); // the GPU can't fault pages in
    if (!mem_backing_block((const void*)address, &block)) {
        LOG_E("gpu", "GPU access to unmapped title memory 0x%llx+0x%llx", address, size);
        return false;
    }
    if (size > (u64)block.base + block.size - address) {
        if (quiet)
            return false;
        LOG_E("gpu", "GPU access 0x%llx+0x%llx crosses the end of its memory block at 0x%llx", address, size,
              (u64)block.base + block.size);
        return false;
    }
    block = window_extent(block, address, size);
    mem_prepare(block.base, block.size); // GPU accesses anywhere in it take the fast path above

    Window entry = {(u64)block.base, (u64)block.base + block.size, 0, nullptr, 0, false};
    u32 chunk;
    u64 chunk_offset;
    if (mem_linear_location(block.host, &chunk, &chunk_offset)) {
        const ChunkWindow* cw = chunk_window(chunk, block.host - chunk_offset);
        if (!cw)
            return false;
        entry.buffer = cw->buffer;
        entry.buffer_offset = chunk_offset;
        entry.desktop_address = cw->desktop_address + chunk_offset;
    } else {
        entry.buffer = open_private_window(block);
        if (!entry.buffer)
            return false;
        entry.desktop_address = entry.buffer->GetGPUVirtualAddress();
        entry.own_heap = true;
    }

    AcquireSRWLockExclusive(&g_windows.lock);
    w = find_window(address);
    if (w && size <= w->end - address) {
        if (entry.own_heap)
            entry.buffer->Release(); // another thread was first; never used
    } else {
        if (g_windows.list.count == g_windows.list.capacity) {
            u32 capacity = g_windows.list.capacity ? g_windows.list.capacity * 2 : 64;
            Window* grown = (Window*)realloc(g_windows.list.entries, capacity * sizeof(Window));
            if (!grown)
                FATAL("out of memory tracking GPU windows");
            g_windows.list.entries = grown;
            g_windows.list.capacity = capacity;
        }
        // The new translation covers the ones it merged; the memory under them is unchanged.
        remove_windows_locked(entry.base, entry.end, false);
        u32 at = windows_at_or_below(entry.base);
        memmove(&g_windows.list.entries[at + 1], &g_windows.list.entries[at],
                (g_windows.list.count - at) * sizeof(Window));
        g_windows.list.entries[at] = entry;
        g_windows.list.count++;
        w = &g_windows.list.entries[at];
    }
    fill(w, address, out);
    ReleaseSRWLockExclusive(&g_windows.lock);
    return true;
}

void gpu_window_usage(u64* chunks, u64* blocks, u64* replaced) {
    *chunks = *blocks = *replaced = 0;
    AcquireSRWLockShared(&g_chunk_lock);
    for (u32 i = 0; g_chunk_windows && i < mem_physical_chunk_count(); i++)
        *chunks += g_chunk_windows[i].buffer ? MEM_CHUNK_SIZE : 0;
    ReleaseSRWLockShared(&g_chunk_lock);
    AcquireSRWLockShared(&g_windows.lock);
    for (u32 i = 0; i < g_windows.list.count; i++)
        if (g_windows.list.entries[i].own_heap)
            *blocks += g_windows.list.entries[i].end - g_windows.list.entries[i].base;
    for (u32 i = 0; i < g_windows.replaced_count; i++)
        *replaced += g_windows.replaced[i].end - g_windows.replaced[i].base;
    ReleaseSRWLockShared(&g_windows.lock);
}

bool gpu_window_translate(u64 address, u64 size, GpuAddress* out) {
    return translate(address, size, false, out);
}

bool gpu_translate_contiguous(u64 address, u64 size, GpuAddress* out) {
    return translate(address, size, true, out);
}

// Title mappings changed: translations over those pages may lead to the wrong frames, or memory
// that is gone. Drop them (the next access translates again) and close the private windows there.
static void on_mapping_changed(const u8* base, u64 size) {
    u64 start = (u64)base, end = (u64)base + size;
    AcquireSRWLockExclusive(&g_windows.lock);
    u32 count = g_windows.list.count;
    remove_windows_locked(start, end, true);
    bool dropped = g_windows.list.count != count;
    for (u32 i = 0; i < g_windows.replaced_count;) {
        PrivateWindow& r = g_windows.replaced[i];
        if (r.base < end && r.end > start) {
            gpu_retire(r.buffer);
            r = g_windows.replaced[--g_windows.replaced_count];
        } else {
            i++;
        }
    }
    if (dropped && InterlockedIncrement(&g_generation) == 0)
        InterlockedIncrement(&g_generation);
    ReleaseSRWLockExclusive(&g_windows.lock);
}

void gpu_window_init() {
    g_capture_tool = GetModuleHandleW(L"renderdoc.dll") != nullptr;
    mem_add_mapping_listener(on_mapping_changed);
}
