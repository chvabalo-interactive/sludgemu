#include "hle/d3d12/target_tiles.h"

#include <stdlib.h>
#include <string.h>

#include "common/log.h"
#include "core/memory.h"
#include "gpu/shared_tiles.h"
#include "gpu/tile_pool.h"

// The tiles of one subresource, or of one slice's packed mips (which desktop D3D12 maps together), in
// the order a texture's tiles are numbered here: slice by slice, mip by mip, the packed ones last.
struct TargetRegion {
    u32 subresource;
    u32 width;  // in tiles (packed mips: their tile count)
    u32 height; // in tiles (packed mips: 1)
    u32 first;  // number of its first tile
    u32 count;
};

struct TargetTexture {
    ConsoleResource* resource;
    ID3D12Resource* texture; // our reference
    u64 start;               // its title memory
    u64 size;
    u32 tile_count;
    u32 region_count;
    bool stale; // tiles to resolve before the next submission
    TargetRegion* regions;
    u64* keys;  // per tile: what names it (SHARED_TILE_PRIVATE: its own)
    u32* tiles; // per tile: the pool tile, or GPU_NO_TILE (no VRAM for it: reads zero)
};

static struct {
    SRWLOCK lock;
    bool enabled;
    TargetTexture** entries; // sorted by start
    u32 count;
    u32 capacity;
    u64 max_size;        // of any texture so far: how far before a change textures can reach it
    volatile long stale; // textures to resolve
    SharedTileMap shared;
    u64 own_tiles;
    u64 references;
    u64* scratch_keys; // resolving: a texture's new keys
    u32 scratch_capacity;
    // Keys whose tiles lost their last reference since the last submission: freed then unless taken
    // again (see shared_tile_unref). May repeat a key.
    u64* unused;
    volatile long unused_count;
    u32 unused_capacity;
    // The queue tile mappings change on, and its last submission (which title work waits for).
    ID3D12CommandQueue* queue;
    u32 slot;
    ID3D12Fence* fence;
    volatile u64 last;
} g_targets = {SRWLOCK_INIT};

// --- tiles -------------------------------------------------------------------------------------------

// A tile for `key`, with a reference; GPU_NO_TILE if VRAM ran out (the pool logged it). Lock held.
static u32 acquire_tile(u64* key) {
    u32 tile = GPU_NO_TILE;
    if (*key != SHARED_TILE_PRIVATE && shared_tile_ref(&g_targets.shared, *key, &tile)) {
        g_targets.references++;
        return tile;
    }
    if (!gpu_tiles_alloc(1, &tile))
        return GPU_NO_TILE;
    if (*key != SHARED_TILE_PRIVATE && !shared_tile_insert(&g_targets.shared, *key, tile)) {
        LOG_W_ONCE("d3d12", "out of memory sharing render target tiles; some are textures' own");
        *key = SHARED_TILE_PRIVATE;
    }
    if (*key == SHARED_TILE_PRIVATE)
        g_targets.own_tiles++;
    g_targets.references++;
    return tile;
}

// Gives the tile of `key` back to the pool (once the GPU is done with it) if it has no references.
// Lock held.
static void drop_unused(u64 key) {
    u32 tile = GPU_NO_TILE;
    if (shared_tile_drop(&g_targets.shared, key, &tile))
        gpu_tiles_free(1, &tile);
}

// Drops a reference to a tile. A texture's own tile goes back to the pool (once the GPU is done with it)
// now; a shared one losing its last reference waits for the next submission, so a texture over the
// same memory resolved before then finds its contents. Lock held.
static void release_tile(u64 key, u32 tile) {
    if (tile == GPU_NO_TILE)
        return;
    g_targets.references--;
    if (key == SHARED_TILE_PRIVATE) {
        g_targets.own_tiles--;
        gpu_tiles_free(1, &tile);
        return;
    }
    if (!shared_tile_unref(&g_targets.shared, key))
        return;
    if ((u32)g_targets.unused_count == g_targets.unused_capacity) {
        u32 capacity = g_targets.unused_capacity ? g_targets.unused_capacity * 2 : 1024;
        u64* grown = (u64*)realloc(g_targets.unused, capacity * sizeof(u64));
        if (!grown) {
            drop_unused(key); // out of memory: its contents go now
            return;
        }
        g_targets.unused = grown;
        g_targets.unused_capacity = capacity;
    }
    g_targets.unused[g_targets.unused_count] = key;
    g_targets.unused_count = g_targets.unused_count + 1;
}

// --- tile mapping ------------------------------------------------------------------------------------

#define MAP_BATCH 256

// Tile mapping changes of one texture, gathered into few UpdateTileMappings calls: regions of
// consecutive tiles, and (mapping) the heap ranges they go to, consecutive tiles of the heap sharing one.
struct MapBatch {
    ID3D12Heap* heap; // nullptr: unmapping
    u32 regions;
    u32 ranges;
    u32 tiles;
    u32 last_tile; // number of the last tile added (extends the last region when the next follows it)
    D3D12_TILED_RESOURCE_COORDINATE at[MAP_BATCH];
    D3D12_TILE_REGION_SIZE sizes[MAP_BATCH];
    UINT offsets[MAP_BATCH];
    UINT counts[MAP_BATCH];
};

static MapBatch s_map, s_unmap; // under g_targets.lock

static void flush_batch(ID3D12Resource* texture, MapBatch* b) {
    if (!b->regions)
        return;
    if (b->heap) {
        g_targets.queue->UpdateTileMappings(texture, b->regions, b->at, b->sizes, b->heap, b->ranges, nullptr,
                                            b->offsets, b->counts, D3D12_TILE_MAPPING_FLAG_NONE);
    } else {
        D3D12_TILE_RANGE_FLAGS null_range = D3D12_TILE_RANGE_FLAG_NULL;
        UINT count = b->tiles;
        g_targets.queue->UpdateTileMappings(texture, b->regions, b->at, b->sizes, nullptr, 1, &null_range,
                                            nullptr, &count, D3D12_TILE_MAPPING_FLAG_NONE);
    }
    b->regions = b->ranges = b->tiles = 0;
}

// Adds tile `n` (of region `r`) to a batch: mapped to `tile` of `heap`, or unmapped (heap nullptr).
static void add_tile(ID3D12Resource* texture, MapBatch* b, const TargetRegion& r, u32 n, ID3D12Heap* heap,
                     u32 tile) {
    if (b->regions && (b->heap != heap || b->regions == MAP_BATCH || b->ranges == MAP_BATCH))
        flush_batch(texture, b);
    b->heap = heap;
    // Regions without a box run through a subresource's tiles x first, then y, then z.
    if (b->regions && b->last_tile + 1 == n && n != r.first &&
        b->at[b->regions - 1].Subresource == r.subresource) {
        b->sizes[b->regions - 1].NumTiles++;
    } else {
        u32 i = n - r.first;
        b->at[b->regions] = {i % r.width, (i / r.width) % r.height, i / (r.width * r.height), r.subresource};
        b->sizes[b->regions] = {1, FALSE, 0, 0, 0};
        b->regions++;
    }
    b->last_tile = n;
    b->tiles++;
    if (!heap)
        return;
    u32 offset = gpu_tile_offset(tile);
    if (b->ranges && b->offsets[b->ranges - 1] + b->counts[b->ranges - 1] == offset) {
        b->counts[b->ranges - 1]++;
    } else {
        b->offsets[b->ranges] = offset;
        b->counts[b->ranges] = 1;
        b->ranges++;
    }
}

// Gives each tile of `t` the tile of what its page shows now, and maps the ones that changed on our
// queue. Lock held.
static void resolve(TargetTexture* t) {
    t->stale = false;
    if (g_targets.scratch_capacity < t->tile_count) {
        u64* grown = (u64*)realloc(g_targets.scratch_keys, t->tile_count * sizeof(u64));
        if (!grown) {
            LOG_E("d3d12", "out of memory resolving the tiles of a texture at 0x%llx", t->start);
            return;
        }
        g_targets.scratch_keys = grown;
        g_targets.scratch_capacity = t->tile_count;
    }
    u64* keys = g_targets.scratch_keys;
    // Only whole pages of its own memory name tiles: the rest of a last partial page belongs to the next
    // resource, and tiles past the memory exist only on desktop.
    u32 pages = (u32)(t->size >> MEM_PAGE_SHIFT);
    pages = pages < t->tile_count ? pages : t->tile_count;
    shared_tile_keys(t->start, pages, keys);
    for (u32 i = pages; i < t->tile_count; i++)
        keys[i] = SHARED_TILE_PRIVATE;

    for (u32 ri = 0; ri < t->region_count; ri++) {
        const TargetRegion& r = t->regions[ri];
        for (u32 n = r.first; n < r.first + r.count; n++) {
            u64 key = keys[n];
            if (key == t->keys[n] && t->tiles[n] != GPU_NO_TILE)
                continue;
            u32 old = t->tiles[n];
            release_tile(t->keys[n], old);
            u32 tile = acquire_tile(&key);
            t->keys[n] = key;
            t->tiles[n] = tile;
            if (tile != GPU_NO_TILE)
                add_tile(t->texture, &s_map, r, n, gpu_tile_heap(tile), tile);
            else if (old != GPU_NO_TILE)
                add_tile(t->texture, &s_unmap, r, n, nullptr, GPU_NO_TILE);
        }
    }
    flush_batch(t->texture, &s_map);
    flush_batch(t->texture, &s_unmap);
}

// --- textures ----------------------------------------------------------------------------------------

static u32 lower_bound(u64 start) {
    u32 lo = 0, hi = g_targets.count;
    while (lo < hi) {
        u32 mid = lo + (hi - lo) / 2;
        if (g_targets.entries[mid]->start < start)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

bool target_tiles_eligible(const ConsoleResource* r) {
    const u32 written = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL |
                        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    // Resources with memory of their own (committed ones, back buffers) share it with nothing. Tiles
    // name whole pages: a texture starting inside one would share it with whatever precedes it.
    return g_targets.enabled && r->desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER &&
           (r->desc.Flags & written) && !r->owned_memory &&
           !(r->heap_flags & D3D12_HEAP_FLAG_ALLOW_DISPLAY) && r->first_mip == 0 &&
           r->gpu_address % MEM_PAGE_SIZE == 0;
}

// The mips of a desktop description (0: the full chain).
static u32 mip_count(const D3D12_RESOURCE_DESC1& d) {
    if (d.MipLevels)
        return d.MipLevels;
    u64 largest = d.Width > d.Height ? d.Width : d.Height;
    if (d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D && d.DepthOrArraySize > largest)
        largest = d.DepthOrArraySize;
    u32 mips = 1;
    while (largest > 1) {
        largest >>= 1;
        mips++;
    }
    return mips;
}

// The texture's regions and tiles, none resolved yet; nullptr (logged) if its tiling isn't one this
// can follow.
static TargetTexture* make_texture(ConsoleResource* r, ID3D12Resource* texture, const D3D12_RESOURCE_DESC1& d,
                                   u64 size) {
    u32 mips = mip_count(d);
    u32 slices = d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1 : d.DepthOrArraySize;
    u32 subresources = mips * slices;
    D3D12_SUBRESOURCE_TILING* tilings = (D3D12_SUBRESOURCE_TILING*)malloc(subresources * sizeof(*tilings));
    if (!tilings)
        return nullptr;
    UINT total = 0, tiling_count = subresources;
    D3D12_PACKED_MIP_INFO packed = {};
    D3D12_TILE_SHAPE shape = {};
    gpu_device()->GetResourceTiling(texture, &total, &packed, &shape, &tiling_count, 0, tilings);
    u32 standard = packed.NumStandardMips < mips ? packed.NumStandardMips : mips;
    u32 region_count = slices * (standard + (standard < mips ? 1 : 0));
    usize bytes = sizeof(TargetTexture) + region_count * sizeof(TargetRegion) +
                  (usize)total * (sizeof(u64) + sizeof(u32));
    TargetTexture* t = total ? (TargetTexture*)malloc(bytes) : nullptr;
    if (!t) {
        free(tilings);
        return nullptr;
    }
    t->resource = r;
    t->texture = texture;
    t->start = r->gpu_address;
    t->size = size;
    t->tile_count = total;
    t->region_count = region_count;
    t->stale = true;
    t->regions = (TargetRegion*)(t + 1);
    t->keys = (u64*)(t->regions + region_count);
    t->tiles = (u32*)(t->keys + total);
    for (u32 i = 0; i < total; i++) {
        t->keys[i] = SHARED_TILE_PRIVATE;
        t->tiles[i] = GPU_NO_TILE;
    }
    u32 n = 0, first = 0;
    for (u32 s = 0; s < slices; s++) {
        for (u32 m = 0; m < standard; m++) {
            const D3D12_SUBRESOURCE_TILING& st = tilings[s * mips + m];
            u32 count = st.WidthInTiles * st.HeightInTiles * st.DepthInTiles;
            t->regions[n++] = {s * mips + m, st.WidthInTiles, st.HeightInTiles, first, count};
            first += count;
        }
        if (standard < mips) {
            t->regions[n++] = {s * mips + standard, packed.NumTilesForPackedMips, 1, first,
                               packed.NumTilesForPackedMips};
            first += packed.NumTilesForPackedMips;
        }
    }
    free(tilings);
    if (first != total) {
        // Planes of their own (depth with stencil) or other tilings this doesn't number.
        LOG_W_ONCE(
            "d3d12",
            "a render target texture's tiles don't add up (%u of %u, format %d); such textures are whole",
            first, total, d.Format);
        free(t);
        return nullptr;
    }
    return t;
}

bool target_tiles_add(ConsoleResource* r, ID3D12Resource* texture, const D3D12_RESOURCE_DESC1& desc) {
    D3D12_RESOURCE_ALLOCATION_INFO info = console_resource_allocation_info(r->desc);
    if (info.SizeInBytes == UINT64_MAX || !g_targets.queue)
        return false;
    TargetTexture* t = make_texture(r, texture, desc, info.SizeInBytes);
    if (!t)
        return false;
    AcquireSRWLockExclusive(&g_targets.lock);
    if (g_targets.count == g_targets.capacity) {
        u32 capacity = g_targets.capacity ? g_targets.capacity * 2 : 256;
        TargetTexture** grown =
            (TargetTexture**)realloc(g_targets.entries, capacity * sizeof(TargetTexture*));
        if (!grown) {
            ReleaseSRWLockExclusive(&g_targets.lock);
            free(t);
            LOG_E("d3d12", "out of memory tracking render target tiles");
            return false;
        }
        g_targets.entries = grown;
        g_targets.capacity = capacity;
    }
    texture->AddRef();
    u32 at = lower_bound(t->start);
    memmove(&g_targets.entries[at + 1], &g_targets.entries[at],
            (g_targets.count - at) * sizeof(TargetTexture*));
    g_targets.entries[at] = t;
    g_targets.count++;
    g_targets.max_size = info.SizeInBytes > g_targets.max_size ? info.SizeInBytes : g_targets.max_size;
    r->target = t;
    InterlockedIncrement(&g_targets.stale);
    ReleaseSRWLockExclusive(&g_targets.lock);
    return true;
}

void target_tiles_remove(ConsoleResource* r) {
    TargetTexture* t = r->target;
    if (!t)
        return;
    AcquireSRWLockExclusive(&g_targets.lock);
    for (u32 i = lower_bound(t->start); i < g_targets.count && g_targets.entries[i]->start == t->start; i++) {
        if (g_targets.entries[i] != t)
            continue;
        memmove(&g_targets.entries[i], &g_targets.entries[i + 1],
                (g_targets.count - i - 1) * sizeof(TargetTexture*));
        g_targets.count--;
        break;
    }
    if (t->stale)
        InterlockedDecrement(&g_targets.stale);
    for (u32 i = 0; i < t->tile_count; i++)
        release_tile(t->keys[i], t->tiles[i]);
    ReleaseSRWLockExclusive(&g_targets.lock);
    gpu_retire(t->texture); // mappings queued for it may not have run yet
    r->target = nullptr;
    free(t);
}

// Textures over [base, base + size) show other memory now: resolved again before the next submission.
static void on_mapping_changed(const u8* base, u64 size) {
    u64 start = (u64)base, end = start + size;
    AcquireSRWLockExclusive(&g_targets.lock);
    for (u32 i = lower_bound(end); i > 0; i--) {
        TargetTexture* t = g_targets.entries[i - 1];
        if (t->start + g_targets.max_size <= start)
            break; // nothing starting this early reaches the change
        if (t->start + t->size <= start || t->stale)
            continue;
        t->stale = true;
        InterlockedIncrement(&g_targets.stale);
    }
    ReleaseSRWLockExclusive(&g_targets.lock);
}

void target_tiles_submit(ID3D12CommandQueue* queue) {
    if (!g_targets.queue)
        return;
    if (g_targets.stale || g_targets.unused_count) {
        AcquireSRWLockExclusive(&g_targets.lock);
        if (g_targets.stale) {
            for (u32 i = 0; i < g_targets.count; i++) {
                if (g_targets.entries[i]->stale)
                    resolve(g_targets.entries[i]);
            }
            g_targets.last =
                gpu_queue_execute(g_targets.queue, g_targets.slot, 0, nullptr); // after the mappings
            // Only now: a submission on another queue seeing no stale textures must see the
            // submission mapping them.
            g_targets.stale = 0;
        }
        // Tiles nothing took again since they lost their last reference.
        for (long i = 0; i < g_targets.unused_count; i++)
            drop_unused(g_targets.unused[i]);
        g_targets.unused_count = 0;
        ReleaseSRWLockExclusive(&g_targets.lock);
    }
    u64 last = g_targets.last;
    if (last && gpu_queue_completed(g_targets.slot) < last)
        queue->Wait(g_targets.fence, last);
}

void target_tiles_usage(TargetTileUsage* out) {
    AcquireSRWLockShared(&g_targets.lock);
    out->textures = g_targets.count;
    out->shared = (u64)g_targets.shared.live * GPU_TILE_SIZE;
    out->own = g_targets.own_tiles * GPU_TILE_SIZE;
    out->referenced = g_targets.references * GPU_TILE_SIZE;
    ReleaseSRWLockShared(&g_targets.lock);
}

void target_tiles_init() {
    char value[8];
    DWORD n = GetEnvironmentVariableA("SLUDGE_TARGET_TILES", value, sizeof(value));
    if (n && n < sizeof(value) && value[0] == '0') {
        LOG_I("d3d12", "render target textures are whole (SLUDGE_TARGET_TILES=0)");
        return;
    }
    // Tiles of render targets and depth textures come from the tile pool's heaps, which hold them from
    // resource heap tier 2 on (gpu/tile_pool.h).
    D3D12_FEATURE_DATA_D3D12_OPTIONS o = {};
    gpu_device()->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &o, sizeof(o));
    if (o.ResourceHeapTier < D3D12_RESOURCE_HEAP_TIER_2)
        return;
    D3D12_COMMAND_QUEUE_DESC qd = {D3D12_COMMAND_LIST_TYPE_COPY};
    if (FAILED(gpu_device()->CreateCommandQueue(&qd, IID_PPV_ARGS(&g_targets.queue)))) {
        LOG_E("d3d12", "cannot create the render target tile queue; render target textures are whole");
        g_targets.queue = nullptr;
        return;
    }
    gpu_set_name(g_targets.queue, "render target tiles");
    g_targets.slot = gpu_queue_register(g_targets.queue);
    if (g_targets.slot == GPU_NO_QUEUE) {
        // Without a submission fence title work couldn't wait for the mappings.
        LOG_E("d3d12", "no queue slot for render target tiles; render target textures are whole");
        g_targets.queue->Release();
        g_targets.queue = nullptr;
        return;
    }
    g_targets.fence = gpu_queue_fence(g_targets.slot);
    mem_add_mapping_listener(on_mapping_changed);
    g_targets.enabled = true;
}
