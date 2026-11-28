#include "gpu/tile_pool.h"

#include <stdlib.h>
#include <string.h>

#include "common/log.h"
#include "core/memory.h"

#define MAX_TILE_HEAPS GPU_TILE_MAX_HEAPS
#define HEAP_WORDS     (GPU_TILE_HEAP_TILES / 64)
// Empty heaps kept beyond those in use, so textures streaming in and out don't create and release
// heaps all the time.
#define SPARE_HEAPS 2

struct TileHeap {
    ID3D12Heap* heap; // nullptr: an empty slot
    u32 serial;       // of the heap, counting every heap made
    u32 free;
    u64 taken[HEAP_WORDS]; // a bit per tile: handed out, or freed and not reclaimed yet
};

// Tiles freed together: reusable once the GPU reached `point`.
struct FreedBatch {
    GpuRetirePoint point;
    u32 count; // the next `count` tiles of g_pool.freed
};

static struct {
    SRWLOCK lock;
    TileHeap heaps[MAX_TILE_HEAPS];
    u32 heap_count;
    u32 taken; // tiles taken in all heaps
    // Freed tiles in order: the sealed batches' first, then those freed since the last seal.
    u32* freed;
    u32 freed_count;
    u32 freed_capacity;
    FreedBatch* batches;
    u32 batch_count;
    u32 batch_capacity;
    u32 sealed; // tiles of g_pool.freed in batches
    u32 serial; // heaps made so far
} g_pool = {SRWLOCK_INIT};

static void release_tile_locked(u32 tile) {
    TileHeap& h = g_pool.heaps[tile / GPU_TILE_HEAP_TILES];
    u32 i = gpu_tile_offset(tile);
    h.taken[i / 64] &= ~(1ull << (i % 64));
    h.free++;
    g_pool.taken--;
}

// The tiles freed since the last seal form a batch, reusable once the GPU finished what was submitted
// until now (later than when they were freed, so never too early).
static void seal_locked() {
    u32 open = g_pool.freed_count - g_pool.sealed;
    if (!open)
        return;
    if (g_pool.batch_count == g_pool.batch_capacity) {
        u32 capacity = g_pool.batch_capacity ? g_pool.batch_capacity * 2 : 64;
        FreedBatch* grown = (FreedBatch*)realloc(g_pool.batches, capacity * sizeof(FreedBatch));
        if (!grown)
            return; // they stay open: sealed with a later batch
        g_pool.batches = grown;
        g_pool.batch_capacity = capacity;
    }
    FreedBatch& b = g_pool.batches[g_pool.batch_count++];
    gpu_retire_point(&b.point);
    b.count = open;
    g_pool.sealed += open;
}

// Takes back the tiles of batches the GPU is done with (oldest first), and releases empty heaps beyond
// the spares.
static void reclaim_locked() {
    u32 batches = 0, tiles = 0;
    while (batches < g_pool.batch_count && gpu_retire_point_reached(g_pool.batches[batches].point))
        tiles += g_pool.batches[batches++].count;
    if (!batches)
        return;
    for (u32 i = 0; i < tiles; i++)
        release_tile_locked(g_pool.freed[i]);
    memmove(g_pool.freed, g_pool.freed + tiles, (g_pool.freed_count - tiles) * sizeof(u32));
    g_pool.freed_count -= tiles;
    g_pool.sealed -= tiles;
    memmove(g_pool.batches, g_pool.batches + batches, (g_pool.batch_count - batches) * sizeof(FreedBatch));
    g_pool.batch_count -= batches;

    u32 empty = 0;
    for (u32 i = MAX_TILE_HEAPS; i-- > 0;) {
        TileHeap& h = g_pool.heaps[i];
        if (!h.heap || h.free != GPU_TILE_HEAP_TILES || ++empty <= SPARE_HEAPS)
            continue;
        gpu_retire(h.heap); // resources may still name its tiles in mappings nothing reads
        h.heap = nullptr;
        g_pool.heap_count--;
    }
}

static bool create_heap_locked(u32 slot) {
    // Video memory counts towards the system's commit too: without room for it, tiles go without.
    if (!mem_commit_available(GPU_TILE_HEAP_TILES * GPU_TILE_SIZE)) {
        static volatile long logged = 0;
        if (log_once_flag(&logged))
            LOG_E("gpu", "the system is out of commit; textures go without memory for some tiles");
        return false;
    }
    D3D12_HEAP_DESC desc = {};
    desc.SizeInBytes = GPU_TILE_HEAP_TILES * GPU_TILE_SIZE;
    desc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    desc.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    // Tiles back buffers (page tables) and textures alike, which heaps allow from resource heap tier 2
    // on (every current GPU). Every tile handed out is written before it is read, so the heap needn't
    // be cleared.
    static volatile long tier = -1;
    if (tier < 0) {
        D3D12_FEATURE_DATA_D3D12_OPTIONS o = {};
        gpu_device()->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &o, sizeof(o));
        tier = o.ResourceHeapTier;
        if (tier < D3D12_RESOURCE_HEAP_TIER_2)
            LOG_E("gpu", "the GPU's resource heap tier is 1: tiles can't back buffers and textures alike");
    }
    desc.Flags = D3D12_HEAP_FLAG_CREATE_NOT_ZEROED |
                 (tier < D3D12_RESOURCE_HEAP_TIER_2 ? D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES
                                                    : D3D12_HEAP_FLAG_ALLOW_ALL_BUFFERS_AND_TEXTURES);
    ID3D12Heap* heap = nullptr;
    HRESULT hr = gpu_device()->CreateHeap(&desc, IID_PPV_ARGS(&heap));
    if (FAILED(hr)) {
        static volatile long logged = 0;
        if (!gpu_device_lost(hr) && log_once_flag(&logged))
            LOG_E("gpu",
                  "cannot create a heap for texture tiles (0x%08lx, %u heaps of %llu MiB in use); "
                  "textures go without memory for some tiles",
                  (unsigned long)hr, g_pool.heap_count, desc.SizeInBytes >> 20);
        return false;
    }
    gpu_set_name(heap, "texture tiles %u", slot);
    TileHeap& h = g_pool.heaps[slot];
    h.heap = heap;
    h.serial = ++g_pool.serial;
    h.free = GPU_TILE_HEAP_TILES;
    memset(h.taken, 0, sizeof(h.taken));
    g_pool.heap_count++;
    return true;
}

bool gpu_tiles_alloc(u32 count, u32* tiles) {
    if (!count || count > GPU_TILE_HEAP_TILES)
        return false;
    AcquireSRWLockExclusive(&g_pool.lock);
    seal_locked();
    reclaim_locked();
    // The lowest heap with room: tiles gather in few heaps, and the others can empty.
    u32 slot = MAX_TILE_HEAPS, empty_slot = MAX_TILE_HEAPS;
    for (u32 i = 0; i < MAX_TILE_HEAPS && slot == MAX_TILE_HEAPS; i++) {
        if (g_pool.heaps[i].heap && g_pool.heaps[i].free >= count)
            slot = i;
        else if (!g_pool.heaps[i].heap && empty_slot == MAX_TILE_HEAPS)
            empty_slot = i;
    }
    if (slot == MAX_TILE_HEAPS) {
        if (empty_slot == MAX_TILE_HEAPS || !create_heap_locked(empty_slot)) {
            ReleaseSRWLockExclusive(&g_pool.lock);
            return false;
        }
        slot = empty_slot;
    }
    TileHeap& h = g_pool.heaps[slot];
    u32 n = 0;
    for (u32 w = 0; w < HEAP_WORDS && n < count; w++) {
        while (h.taken[w] != ~0ull && n < count) {
            unsigned long bit;
            _BitScanForward64(&bit, ~h.taken[w]);
            h.taken[w] |= 1ull << bit;
            tiles[n++] = slot * GPU_TILE_HEAP_TILES + w * 64 + bit;
        }
    }
    h.free -= count;
    g_pool.taken += count;
    ReleaseSRWLockExclusive(&g_pool.lock);
    return true;
}

void gpu_tiles_free(u32 count, const u32* tiles) {
    if (!count)
        return;
    AcquireSRWLockExclusive(&g_pool.lock);
    if (g_pool.freed_count + count > g_pool.freed_capacity) {
        u32 capacity = g_pool.freed_capacity ? g_pool.freed_capacity : 4096;
        while (capacity < g_pool.freed_count + count)
            capacity *= 2;
        u32* grown = (u32*)realloc(g_pool.freed, capacity * sizeof(u32));
        if (!grown) {
            // Out of memory: wait for the GPU rather than hand out tiles it may be using.
            ReleaseSRWLockExclusive(&g_pool.lock);
            LOG_E("gpu", "freed tile list full; waiting for the GPU to go idle");
            gpu_wait_idle();
            AcquireSRWLockExclusive(&g_pool.lock);
            for (u32 i = 0; i < count; i++)
                release_tile_locked(tiles[i]);
            ReleaseSRWLockExclusive(&g_pool.lock);
            return;
        }
        g_pool.freed = grown;
        g_pool.freed_capacity = capacity;
    }
    memcpy(g_pool.freed + g_pool.freed_count, tiles, count * sizeof(u32));
    g_pool.freed_count += count;
    ReleaseSRWLockExclusive(&g_pool.lock);
}

ID3D12Heap* gpu_tile_heap(u32 tile) {
    // A heap stays while any of its tiles is taken, and was made before they were handed out.
    return g_pool.heaps[tile / GPU_TILE_HEAP_TILES].heap;
}

u32 gpu_tile_heap_serial(u32 tile) {
    const TileHeap& h = g_pool.heaps[tile / GPU_TILE_HEAP_TILES];
    return h.heap ? h.serial : 0;
}

// --- slots ---------------------------------------------------------------------------------------------

struct SlotTile {
    u32 tile;
    u16 used; // a bit per slot
};

struct FreedSlots {
    GpuSlots slots;
    GpuRetirePoint point;
};

static struct {
    SRWLOCK lock;
    SlotTile* tiles; // tiles with slots handed out
    u32 count, capacity;
    FreedSlots* freed; // oldest first
    u32 freed_count, freed_capacity;
} g_slots = {SRWLOCK_INIT};

static u16 slot_mask(const GpuSlots& s) {
    return (u16)(((1u << s.count) - 1) << s.first);
}

// Takes back slots the GPU is done with; tiles left without any go back to the pool. Slots lock held.
static void reclaim_slots_locked() {
    u32 done = 0;
    while (done < g_slots.freed_count && gpu_retire_point_reached(g_slots.freed[done].point))
        done++;
    for (u32 i = 0; i < done; i++) {
        const GpuSlots& s = g_slots.freed[i].slots;
        for (u32 t = 0; t < g_slots.count; t++) {
            SlotTile& st = g_slots.tiles[t];
            if (st.tile != s.tile)
                continue;
            st.used &= (u16)~slot_mask(s);
            if (!st.used) {
                gpu_tiles_free(1, &st.tile);
                st = g_slots.tiles[--g_slots.count];
            }
            break;
        }
    }
    memmove(g_slots.freed, g_slots.freed + done, (g_slots.freed_count - done) * sizeof(FreedSlots));
    g_slots.freed_count -= done;
}

bool gpu_slots_alloc(u64 size, GpuSlots* out) {
    u32 count = (u32)((size + GPU_SLOT_SIZE - 1) / GPU_SLOT_SIZE);
    if (!count || count > GPU_TILE_SLOTS)
        return false;
    u16 want = (u16)((1u << count) - 1);
    AcquireSRWLockExclusive(&g_slots.lock);
    reclaim_slots_locked();
    // First fit over the tiles holding slots (a few thousand at most).
    for (u32 t = 0; t < g_slots.count; t++) {
        SlotTile& st = g_slots.tiles[t];
        for (u32 first = 0; first + count <= GPU_TILE_SLOTS; first++) {
            if (st.used & (u16)(want << first))
                continue;
            st.used |= (u16)(want << first);
            *out = {st.tile, (u16)first, (u16)count};
            ReleaseSRWLockExclusive(&g_slots.lock);
            return true;
        }
    }
    if (g_slots.count == g_slots.capacity) {
        u32 capacity = g_slots.capacity ? g_slots.capacity * 2 : 256;
        SlotTile* grown = (SlotTile*)realloc(g_slots.tiles, capacity * sizeof(SlotTile));
        if (!grown) {
            ReleaseSRWLockExclusive(&g_slots.lock);
            return false;
        }
        g_slots.tiles = grown;
        g_slots.capacity = capacity;
    }
    u32 tile = GPU_NO_TILE;
    if (!gpu_tiles_alloc(1, &tile)) {
        ReleaseSRWLockExclusive(&g_slots.lock);
        return false;
    }
    g_slots.tiles[g_slots.count++] = {tile, want};
    *out = {tile, 0, (u16)count};
    ReleaseSRWLockExclusive(&g_slots.lock);
    return true;
}

void gpu_slots_free(const GpuSlots& slots) {
    if (!slots.count)
        return;
    AcquireSRWLockExclusive(&g_slots.lock);
    if (g_slots.freed_count == g_slots.freed_capacity) {
        u32 capacity = g_slots.freed_capacity ? g_slots.freed_capacity * 2 : 256;
        FreedSlots* grown = (FreedSlots*)realloc(g_slots.freed, capacity * sizeof(FreedSlots));
        if (!grown) {
            // Out of memory: these slots are lost rather than handed out while the GPU may use them.
            ReleaseSRWLockExclusive(&g_slots.lock);
            LOG_E("gpu", "freed slot list full; 4 KiB slots of VRAM are lost");
            return;
        }
        g_slots.freed = grown;
        g_slots.freed_capacity = capacity;
    }
    FreedSlots& f = g_slots.freed[g_slots.freed_count++];
    f.slots = slots;
    gpu_retire_point(&f.point);
    ReleaseSRWLockExclusive(&g_slots.lock);
}

void gpu_tile_usage(u64* used, u64* reserved) {
    AcquireSRWLockShared(&g_pool.lock);
    *used = (u64)g_pool.taken * GPU_TILE_SIZE;
    *reserved = (u64)g_pool.heap_count * GPU_TILE_HEAP_TILES * GPU_TILE_SIZE;
    ReleaseSRWLockShared(&g_pool.lock);
}
