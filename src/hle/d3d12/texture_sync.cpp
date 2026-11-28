#include "hle/d3d12/texture_sync.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/log.h"
#include "core/memory.h"
#include "gpu/console_layout.h"
#include "gpu/formats.h"
#include "gpu/gpu.h"
#include "gpu/gpu_window.h"
#include "gpu/texture_untile.h"
#include "gpu/tile_pool.h"

struct TextureTiles;

struct SyncEntry {
    u64 start;
    u64 end;
    ConsoleResource* resource; // registered while alive (texture_sync_remove before destruction)
    bool dirty;
    bool linear; // laid out for CPU writes: copied by the GPU where the layout allows
    // How its subresources are laid out, for detiling (made at the first upload; plan order).
    ConsoleSubresourceAddressing* addressing;
    TextureTiles* tiles; // reserved desktop textures: their tiles (made at the first upload)
};

static struct {
    SyncEntry* entries; // sorted by start; textures may overlap (aliasing)
    u32 count;
    u32 capacity;
    SRWLOCK lock;
    volatile long dirty; // entries marked and not uploaded yet
} g_sync = {nullptr, 0, 0, SRWLOCK_INIT, 0};

static ConsoleTextureDesc layout_desc(const D3D12_RESOURCE_DESC1& d);

// Textures traced (SLUDGE_TRACE_TEXTURES=<width>x<height>), or width 0.
static struct {
    u64 width;
    u32 height;
    char dump_dir[260];
    volatile long dumps;
} g_trace;

bool texture_sync_traced(const D3D12_RESOURCE_DESC1& desc) {
    return g_trace.width && desc.Width == g_trace.width && desc.Height == g_trace.height;
}

// Titles map the pages of a texture as they stream its data in; the GPU reads the mapped ones. These
// walk [start, end) in pieces of one page at most, each mapped or not.
static u64 piece_end(u64 at, u64 end) {
    u64 next = ALIGN_DOWN(at, MEM_PAGE_SIZE) + MEM_PAGE_SIZE;
    return next < end ? next : end;
}

// Mapped bytes of [start, end).
static u64 mapped_bytes(u64 start, u64 end) {
    u64 mapped = 0;
    for (u64 at = start; at < end; at = piece_end(at, end))
        if (mem_is_mapped((const void*)at, piece_end(at, end) - at))
            mapped += piece_end(at, end) - at;
    return mapped;
}

// Copies [start, end) of title memory to `dst`. What isn't mapped is zeros, or left as it is when only
// tiles without memory read it (reserved textures). Frames nothing has written yet are zeros too, read
// without backing them: memory only the GPU uses (render targets seen through views of placed memory)
// stays without system memory.
static void copy_mapped(u8* dst, u64 start, u64 end, bool zero_unmapped) {
    for (u64 at = start; at < end; at = piece_end(at, end)) {
        u64 n = piece_end(at, end) - at;
        if (mem_any_pending((const void*)at, n)) {
            memset(dst + (at - start), 0, n); // a write after this marks the texture again (its watch)
        } else if (mem_is_mapped((const void*)at, n)) {
            mem_prepare((const void*)at, n);
            memcpy(dst + (at - start), (const void*)at, n);
        } else if (zero_unmapped) {
            memset(dst + (at - start), 0, n);
        }
    }
}

// Mapped bytes and those of them that aren't zero: tells "no data yet" from "data the upload loses".
static void trace_contents(const char* what, const SyncEntry& e) {
    u64 nonzero = 0;
    for (u64 at = e.start; at < e.end; at = piece_end(at, e.end)) {
        u64 n = piece_end(at, e.end) - at;
        if (!mem_is_mapped((const void*)at, n))
            continue;
        mem_prepare((const void*)at, n);
        for (u64 i = 0; i < n; i++)
            nonzero += ((const u8*)at)[i] != 0;
    }
    LOG_I("d3d12", "texture trace: %s %p at 0x%llx-0x%llx: %llu of %llu bytes mapped, %llu non-zero", what,
          (void*)e.resource, e.start, e.end, mapped_bytes(e.start, e.end), e.end - e.start, nonzero);
    char unmapped[512];
    u32 used = 0;
    unmapped[0] = 0;
    for (u64 at = e.start; at < e.end && used < sizeof(unmapped) - 32; at = piece_end(at, e.end))
        if (!mem_is_mapped((const void*)at, piece_end(at, e.end) - at))
            used += (u32)snprintf(unmapped + used, sizeof(unmapped) - used, " +0x%llx", at - e.start);
    if (used)
        LOG_I("d3d12", "texture trace: %p unmapped pieces at%s", (void*)e.resource, unmapped);
    if (g_trace.dump_dir[0] && InterlockedIncrement(&g_trace.dumps) <= 64) {
        u64 size = e.end - e.start;
        u8* copy = (u8*)malloc(size);
        if (copy) {
            copy_mapped(copy, e.start, e.end, true);
            char path[400];
            snprintf(path, sizeof(path), "%s/tex_%llx_%03ld.bin", g_trace.dump_dir, e.start, g_trace.dumps);
            FILE* f = fopen(path, "wb");
            if (f) {
                fwrite(copy, 1, size, f);
                fclose(f);
            }
            free(copy);
        }
    }
}

// Detiled data per submission: the rest waits for the next one (a texture larger than this goes
// alone).
#define UPLOAD_BUDGET MiB(128)

bool texture_sync_tracks(const D3D12_RESOURCE_DESC1& desc) {
    const D3D12_RESOURCE_FLAGS written = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
                                         D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL |
                                         D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    return desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER && !(desc.Flags & written) &&
           desc.SampleDesc.Count <= 1;
}

// Console layouts store a swizzle mode; mode 0 is linear.
static bool linear_layout(const D3D12_RESOURCE_DESC1& desc) {
    return desc.Layout == D3D12_TEXTURE_LAYOUT_ROW_MAJOR || (u32)desc.Layout == CONSOLE_LAYOUT_SWIZZLE;
}

bool texture_sync_tiles(const D3D12_RESOURCE_DESC1& desc) {
    return texture_sync_tracks(desc) && !linear_layout(desc);
}

// The desktop texture's mips: the console texture's from ConsoleResource::first_mip on.
static u32 desktop_mips(const ConsoleResource* r) {
    return (r->desc.MipLevels ? r->desc.MipLevels : 1u) - r->first_mip;
}

// Where a texture's contents end in title memory: its whole layout, or for one holding mips from a
// first one on (see ConsoleResource::first_mip), where those end (console layouts put small mips first).
static u64 contents_end(const ConsoleResource* r, u64 size) {
    if (!r->first_mip)
        return r->gpu_address + size;
    ConsoleTextureLayout layout;
    if (!console_texture_layout(layout_desc(r->desc), &layout) || layout.plane_count != 1)
        return r->gpu_address + size;
    const ConsolePlaneLayout& p = layout.planes[0];
    u32 slices = r->desc.DepthOrArraySize ? r->desc.DepthOrArraySize : 1u;
    u64 end = 0;
    for (u32 s = 0; s < slices; s++) {
        for (u32 m = r->first_mip; m < layout.mip_levels; m++) {
            u64 at = p.base_offset + (u64)s * p.slice_size + p.mips[m].offset + p.mips[m].size;
            end = at > end ? at : end;
        }
    }
    return r->gpu_address + end;
}

static u32 lower_bound(u64 start) {
    u32 lo = 0, hi = g_sync.count;
    while (lo < hi) {
        u32 mid = lo + (hi - lo) / 2;
        if (g_sync.entries[mid].start < start)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

void texture_sync_add(ConsoleResource* r) {
    D3D12_RESOURCE_ALLOCATION_INFO info = console_resource_allocation_info(r->desc);
    if (info.SizeInBytes == UINT64_MAX)
        return;
    AcquireSRWLockExclusive(&g_sync.lock);
    if (g_sync.count == g_sync.capacity) {
        u32 capacity = g_sync.capacity ? g_sync.capacity * 2 : 256;
        SyncEntry* grown = (SyncEntry*)realloc(g_sync.entries, capacity * sizeof(SyncEntry));
        if (!grown) {
            ReleaseSRWLockExclusive(&g_sync.lock);
            LOG_E("d3d12", "out of memory tracking textures; texture at 0x%llx won't get its contents",
                  r->gpu_address);
            return;
        }
        g_sync.entries = grown;
        g_sync.capacity = capacity;
    }
    u32 at = lower_bound(r->gpu_address);
    memmove(&g_sync.entries[at + 1], &g_sync.entries[at], (g_sync.count - at) * sizeof(SyncEntry));
    // Its memory may hold the contents already; if it isn't mapped yet, the upload is skipped and the
    // mapping marks it again.
    g_sync.entries[at] = {
        r->gpu_address, contents_end(r, info.SizeInBytes), r, true, linear_layout(r->desc), nullptr, nullptr};
    g_sync.count++;
    if (texture_sync_traced(r->desc)) {
        LOG_I(
            "d3d12",
            "texture trace: added %p at 0x%llx-0x%llx (%llux%u, %u mips, format %d, layout 0x%x, flags 0x%x)",
            (void*)r, r->gpu_address, r->gpu_address + info.SizeInBytes, r->desc.Width, r->desc.Height,
            r->desc.MipLevels, r->desc.Format, r->desc.Layout, r->desc.Flags);
        ConsoleTextureLayout layout;
        if (console_texture_layout(layout_desc(r->desc), &layout)) {
            const ConsolePlaneLayout& p = layout.planes[0];
            LOG_I("d3d12", "texture trace: %p layout: plane base 0x%llx, %u standard mips, %u in the tail",
                  (void*)r, p.base_offset, p.standard_mip_count, p.tail_mip_count);
            for (u32 m = 0; m < layout.mip_levels; m++)
                LOG_I(
                    "d3d12",
                    "texture trace: %p mip %u: offset 0x%llx size 0x%llx swizzle %u pitch %u, %ux%u elements "
                    "(padded %ux%u), tail %u at %u,%u",
                    (void*)r, m, p.mips[m].offset, p.mips[m].size, p.mips[m].swizzle_mode,
                    p.mips[m].pitch_pixels, p.mips[m].width, p.mips[m].height, p.mips[m].padded_width,
                    p.mips[m].padded_height, p.mips[m].in_mip_tail, (u32)p.mips[m].tail_x,
                    (u32)p.mips[m].tail_y);
        }
    }
    InterlockedIncrement(&g_sync.dirty);
    ReleaseSRWLockExclusive(&g_sync.lock);
}

static void free_tiles(TextureTiles* t);

void texture_sync_remove(ConsoleResource* r) {
    u64 start = 0, size = 0;
    AcquireSRWLockExclusive(&g_sync.lock);
    for (u32 i = lower_bound(r->gpu_address); i < g_sync.count && g_sync.entries[i].start == r->gpu_address;
         i++) {
        if (g_sync.entries[i].resource != r)
            continue;
        if (g_sync.entries[i].dirty)
            InterlockedDecrement(&g_sync.dirty);
        free(g_sync.entries[i].addressing);
        free_tiles(g_sync.entries[i].tiles);
        start = g_sync.entries[i].start;
        size = g_sync.entries[i].end - start;
        memmove(&g_sync.entries[i], &g_sync.entries[i + 1], (g_sync.count - i - 1) * sizeof(SyncEntry));
        g_sync.count--;
        break;
    }
    ReleaseSRWLockExclusive(&g_sync.lock);
    mem_unwatch_writes((const void*)start, size); // its memory needn't stay protected
}

void texture_sync_mark(u64 address, u64 size) {
    u64 end = address + size;
    AcquireSRWLockExclusive(&g_sync.lock);
    // Entries starting before `end` may overlap; textures are at most a few hundred MiB, so look back
    // from the first entry starting at or after `end`.
    for (u32 i = lower_bound(end); i > 0; i--) {
        SyncEntry& e = g_sync.entries[i - 1];
        if (e.end <= address) {
            if (address - e.start > GiB(4))
                break; // far before: nothing earlier can reach
            continue;
        }
        if (texture_sync_traced(e.resource->desc))
            LOG_I("d3d12", "texture trace: %p marked by 0x%llx+0x%llx%s", (void*)e.resource, address, size,
                  e.dirty ? " (already marked)" : "");
        if (!e.dirty) {
            e.dirty = true;
            InterlockedIncrement(&g_sync.dirty);
        }
    }
    ReleaseSRWLockExclusive(&g_sync.lock);
}

bool texture_sync_trace_overlaps(u64 address, u64 size) {
    if (!g_trace.width)
        return false;
    bool found = false;
    AcquireSRWLockShared(&g_sync.lock);
    for (u32 i = 0; i < g_sync.count && !found; i++) {
        const SyncEntry& e = g_sync.entries[i];
        found = e.start < address + size && e.end > address && texture_sync_traced(e.resource->desc);
    }
    ReleaseSRWLockShared(&g_sync.lock);
    return found;
}

// Traced textures over [base, base + size): which of their pages are mapped now, and to which frames.
static void trace_mapping(const u8* base, u64 size) {
    AcquireSRWLockShared(&g_sync.lock);
    for (u32 i = 0; i < g_sync.count; i++) {
        const SyncEntry& e = g_sync.entries[i];
        if (e.start >= (u64)base + size || e.end <= (u64)base || !texture_sync_traced(e.resource->desc))
            continue;
        char text[640];
        u32 used = 0;
        text[0] = 0;
        for (u64 at = ALIGN_DOWN(e.start, MEM_PAGE_SIZE); at < e.end && used < sizeof(text) - 40;
             at += MEM_PAGE_SIZE) {
            u64 pfn = MEM_INVALID_PFN;
            mem_frames_at((const void*)at, 1, &pfn);
            used +=
                (u32)snprintf(text + used, sizeof(text) - used, " %llx", pfn == MEM_INVALID_PFN ? 0ull : pfn);
        }
        LOG_I("d3d12", "texture trace: %p remapped by 0x%llx+0x%llx; frames now%s", (void*)e.resource,
              (u64)base, size, text);
    }
    ReleaseSRWLockShared(&g_sync.lock);
}

static void on_mapping_changed(const u8* base, u64 size) {
    if (g_trace.width)
        trace_mapping(base, size);
    texture_sync_mark((u64)base, size);
}

// A device finished writing [base, base + size): every texture there has new contents. A CPU write
// ended the watch over [base, base + size) instead, armed by the textures over exactly that memory
// (see texture_sync_record): textures overlapping it elsewhere have watches of their own, which the
// write ended too if it hit their memory; marking them for this one would upload them whenever a
// neighbour is written.
static void on_written(u64 base, u64 size, MemWriteKind kind) {
    if (g_trace.width && texture_sync_trace_overlaps(base, size))
        LOG_I("d3d12", "texture trace: %s 0x%llx+0x%llx",
              kind == MEM_DEVICE_WROTE ? "device wrote" : "watch ended", base, size);
    if (kind == MEM_DEVICE_WROTE) {
        texture_sync_mark(base, size);
        return;
    }
    if (kind == MEM_WATCH_DROPPED)
        return; // other memory mapped there: the mapping listener marks the textures
    AcquireSRWLockExclusive(&g_sync.lock);
    for (u32 i = lower_bound(base); i < g_sync.count && g_sync.entries[i].start == base; i++) {
        SyncEntry& e = g_sync.entries[i];
        if (e.end != base + size)
            continue;
        if (texture_sync_traced(e.resource->desc))
            LOG_I("d3d12", "texture trace: %p marked by its write watch", (void*)e.resource);
        if (!e.dirty) {
            e.dirty = true;
            InterlockedIncrement(&g_sync.dirty);
        }
    }
    ReleaseSRWLockExclusive(&g_sync.lock);
}

void texture_sync_init() {
    char trace[32];
    DWORD n = GetEnvironmentVariableA("SLUDGE_TRACE_TEXTURES", trace, sizeof(trace));
    if (n && n < sizeof(trace)) {
        char* x = nullptr;
        g_trace.width = strtoull(trace, &x, 10);
        g_trace.height = x && *x == 'x' ? (u32)strtoul(x + 1, nullptr, 10) : 0;
        if (!g_trace.height)
            g_trace.width = 0;
        LOG_I("d3d12", "tracing textures of %llux%u", g_trace.width, g_trace.height);
        n = GetEnvironmentVariableA("SLUDGE_TRACE_TEXTURES_DUMP", g_trace.dump_dir, sizeof(g_trace.dump_dir));
        if (n >= sizeof(g_trace.dump_dir))
            g_trace.dump_dir[0] = 0;
    }
    mem_add_mapping_listener(on_mapping_changed);
    mem_add_write_listener(on_written);
}

bool texture_sync_pending() {
    return g_sync.dirty != 0 || mem_device_writes_outstanding();
}

// --- uploads -----------------------------------------------------------------------------------------

struct SubresourceCopy {
    u32 mip;
    u32 slice;
    u32 width, height, depth; // in texels, block aligned for the footprint
    u64 row_pitch;
    u64 slice_pitch;
    u64 offset; // in the upload buffer
};

// One texture's subresources and where they go in the upload buffer, from `offset` on; with `mapped`
// (reserved textures: per subresource, whether any of its tiles has memory), only those that have.
// False for textures whose desktop format stores texels differently (stand-ins need conversion).
static bool plan_texture(const ConsoleResource* r, const u32* mapped, SubresourceCopy* copies, u32 capacity,
                         u32* count, u64* offset) {
    const D3D12_RESOURCE_DESC1& d = r->desc;
    FormatInfo console = format_info(d.Format);
    FormatInfo desktop = format_info(format_desktop_resource(d.Format));
    if (!console.bytes_per_element || console.bytes_per_element != desktop.bytes_per_element ||
        console.block_width != desktop.block_width) {
        LOG_W_ONCE("d3d12",
                   "textures of format %d are stored differently on desktop; their contents aren't "
                   "uploaded yet",
                   d.Format);
        return false;
    }
    u32 mips = desktop_mips(r), first = r->first_mip; // desktop mip m is console mip first + m
    u32 slices = d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1 : d.DepthOrArraySize;
    *count = 0;
    for (u32 s = 0; s < slices; s++) {
        for (u32 m = 0; m < mips; m++) {
            if (mapped && !mapped[s * mips + m])
                continue;
            if (*count == capacity)
                return false;
            SubresourceCopy& c = copies[(*count)++];
            u32 w = (u32)(d.Width >> (first + m)), h = d.Height >> (first + m);
            u32 depth =
                d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? d.DepthOrArraySize >> (first + m) : 1;
            w = w ? w : 1;
            h = h ? h : 1;
            depth = depth ? depth : 1;
            u32 bw = (w + console.block_width - 1) / console.block_width;
            u32 bh = (h + console.block_height - 1) / console.block_height;
            c.mip = m;
            c.slice = s;
            c.width = bw * console.block_width;
            c.height = bh * console.block_height;
            c.depth = depth;
            c.row_pitch =
                ALIGN_UP((u64)bw * console.bytes_per_element, (u64)D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
            c.slice_pitch = c.row_pitch * bh;
            *offset = ALIGN_UP(*offset, (u64)D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
            c.offset = *offset;
            *offset += c.slice_pitch * depth;
        }
    }
    return true;
}

static ConsoleTextureDesc layout_desc(const D3D12_RESOURCE_DESC1& d) {
    ConsoleTextureDesc t = {};
    t.dimension = d.Dimension;
    t.width = d.Width;
    t.height = d.Height;
    t.depth_or_array_size = d.DepthOrArraySize;
    t.mip_levels = d.MipLevels;
    t.format = d.Format;
    t.sample_count = d.SampleDesc.Count;
    t.layout = d.Layout;
    t.flags = d.Flags;
    return t;
}

static void texture_barrier(ID3D12GraphicsCommandList8* list, ID3D12Resource* texture, bool before_copy) {
    D3D12_TEXTURE_BARRIER b = {};
    b.SyncBefore = before_copy ? D3D12_BARRIER_SYNC_ALL : D3D12_BARRIER_SYNC_COPY;
    b.SyncAfter = before_copy ? D3D12_BARRIER_SYNC_COPY : D3D12_BARRIER_SYNC_ALL;
    b.AccessBefore = before_copy ? D3D12_BARRIER_ACCESS_COMMON : D3D12_BARRIER_ACCESS_COPY_DEST;
    b.AccessAfter = before_copy ? D3D12_BARRIER_ACCESS_COPY_DEST : D3D12_BARRIER_ACCESS_COMMON;
    b.LayoutBefore = D3D12_BARRIER_LAYOUT_COMMON;
    b.LayoutAfter = D3D12_BARRIER_LAYOUT_COMMON;
    b.pResource = texture;
    b.Subresources.IndexOrFirstMipLevel = 0xFFFFFFFF; // all
    D3D12_BARRIER_GROUP g = {D3D12_BARRIER_TYPE_TEXTURE, 1};
    g.pTextureBarriers = &b;
    list->Barrier(1, &g);
}

// Copies from buffers into textures need 512-byte aligned offsets and 256-byte aligned row pitches,
// unless the device lifts that.
static bool unrestricted_copy_pitch() {
    static volatile long s_known = 0; // 1: restricted, 2: unrestricted
    if (!s_known) {
        D3D12_FEATURE_DATA_D3D12_OPTIONS13 o = {};
        bool lifted =
            SUCCEEDED(gpu_device()->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS13, &o, sizeof(o))) &&
            o.UnrestrictedBufferTextureCopyPitchSupported;
        InterlockedExchange(&s_known, lifted ? 2 : 1);
    }
    return s_known == 2;
}

// Holds a reference to a resource the recorded copies use. False if there is no room for another.
static bool keep_source(TextureUploadSources* sources, ID3D12Resource* resource) {
    for (u32 i = 0; i < sources->count; i++)
        if (sources->resources[i] == resource)
            return true;
    if (sources->count == TEXTURE_UPLOAD_SOURCES - 1)
        return false; // the last entry is for the upload buffer
    resource->AddRef();
    sources->resources[sources->count++] = resource;
    return true;
}

// A linear texture of one array slice, copied by the GPU straight from title memory through its
// window: each mip is a footprint at its offset and pitch in the console layout. False (nothing
// recorded) if the layout isn't one copies can take.
static bool copy_from_title_memory(ID3D12GraphicsCommandList8* list, ConsoleResource* r, u64 size,
                                   TextureUploadSources* sources) {
    const D3D12_RESOURCE_DESC1& d = r->desc;
    if (d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D || d.DepthOrArraySize != 1)
        return false;
    FormatInfo console = format_info(d.Format);
    FormatInfo desktop = format_info(format_desktop_resource(d.Format));
    if (!console.bytes_per_element || console.bytes_per_element != desktop.bytes_per_element ||
        console.block_width != desktop.block_width)
        return false;
    ConsoleTextureDesc t = layout_desc(d);
    ConsoleTextureLayout layout;
    if (!console_texture_layout(t, &layout) || layout.plane_count != 1)
        return false;
    const ConsolePlaneLayout& p = layout.planes[0];
    bool unrestricted = unrestricted_copy_pitch();
    GpuAddress window;
    if (!gpu_translate_current(r->gpu_address, size, &window))
        return false; // not one block of memory for the GPU
    for (u32 m = r->first_mip; m < layout.mip_levels; m++) {
        u64 at = window.offset + p.base_offset + p.mips[m].offset;
        if (p.mips[m].swizzle_mode != 0 ||
            (!unrestricted && (at % D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT ||
                               p.mips[m].pitch_bytes % D3D12_TEXTURE_DATA_PITCH_ALIGNMENT)))
            return false;
    }
    u64 unused;
    ID3D12Resource* texture = console_resource_desktop(r, &unused, GPU_READ);
    if (!texture || sources->count + 2 >= TEXTURE_UPLOAD_SOURCES)
        return false;
    keep_source(sources, window.buffer);
    keep_source(sources, texture);
    texture_barrier(list, texture, true);
    for (u32 m = r->first_mip; m < layout.mip_levels; m++) {
        const ConsoleMipLayout& mip = p.mips[m];
        u32 w = (u32)(d.Width >> m), h = d.Height >> m;
        u32 bw = ((w ? w : 1) + console.block_width - 1) / console.block_width;
        u32 bh = ((h ? h : 1) + console.block_height - 1) / console.block_height;
        D3D12_TEXTURE_COPY_LOCATION dst = {texture, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        dst.SubresourceIndex = m - r->first_mip;
        D3D12_TEXTURE_COPY_LOCATION src = {window.buffer, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        src.PlacedFootprint.Offset = window.offset + p.base_offset + mip.offset;
        src.PlacedFootprint.Footprint = {format_desktop_resource(d.Format), bw * console.block_width,
                                         bh * console.block_height, 1, mip.pitch_bytes};
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    texture_barrier(list, texture, false);
    return true;
}

#define MAX_SUBRESOURCES 512
#define MAX_UPLOADS      64

// Console layouts whose addressing was checked against the layout (see
// console_subresource_addressing): textures described the same way skip the check.
#define CHECKED_LAYOUTS 4096
static u64 g_checked[CHECKED_LAYOUTS]; // hashes of descriptions; under g_sync.lock

static u64 desc_hash(const ConsoleTextureDesc& t) {
    u64 h = 0xcbf29ce484222325ull;
    const u8* b = (const u8*)&t;
    for (usize i = 0; i < sizeof(t); i++)
        h = (h ^ b[i]) * 0x100000001b3ull;
    return h | 1;
}

// The addressing of every subresource of a texture, in plan order (slice by slice, mip by mip);
// nullptr (logged) if its layout can't be detiled.
static ConsoleSubresourceAddressing* make_addressing(const ConsoleResource* r, u32 count) {
    ConsoleTextureDesc t = layout_desc(r->desc);
    ConsoleTextureLayout layout;
    if (!console_texture_layout(t, &layout))
        return nullptr;
    ConsoleSubresourceAddressing* a = (ConsoleSubresourceAddressing*)malloc(count * sizeof(*a));
    if (!a)
        return nullptr;
    u64 h = desc_hash(t);
    u32 slot = (u32)(h >> 40) % CHECKED_LAYOUTS;
    bool checked = g_checked[slot] == h;
    u32 first = r->first_mip, mips = layout.mip_levels - first; // per desktop subresource
    for (u32 i = 0; i < count; i++) {
        // Each mip of the first slice is checked once per kind of texture.
        bool check = !checked && i < mips;
        if (!console_subresource_addressing(t, layout, 0, first + i % mips, i / mips, &a[i], check)) {
            LOG_W_ONCE("d3d12", "textures of format %d in layout 0x%x can't be detiled; they get no contents",
                       t.format, t.layout);
            free(a);
            return nullptr;
        }
    }
    g_checked[slot] = h;
    return a;
}

// --- tiles of reserved textures ----------------------------------------------------------------------
//
// A reserved texture's tiles are mapped in groups, each while the title memory its texels come from is
// mapped: a tile of a mip laid out in 64 KiB blocks of the tile's shape reads one page, a tile of
// another mip (small ones, those in the console's mip tail) reads the mip's memory as a whole, and the
// tiles of a slice's packed mips, which desktop D3D12 maps together, read those of all of them.

struct TileGroup {
    D3D12_TILED_RESOURCE_COORDINATE at; // its first tile
    u32 count;                          // 1, or the tiles of a slice's packed mips
    u32 first;                          // its tiles in TextureTiles::tiles
    u64 source;                         // title memory its texels come from, from the texture's start
    u64 source_size;
    bool mapped;
};

struct TextureTiles {
    u32 group_count;
    u32 mips;
    u32 standard_mips; // the desktop texture's: the mips after them are packed
    u32* tiles;        // per tile: the pool tile of a mapped group, GPU_NO_TILE otherwise
    u32* mapped;       // per subresource: how many of its groups are mapped
    TileGroup groups[1];
};

static void free_tiles(TextureTiles* t) {
    if (!t)
        return;
    for (u32 i = 0; i < t->group_count; i++)
        if (t->groups[i].mapped)
            gpu_tiles_free(t->groups[i].count, &t->tiles[t->groups[i].first]);
    free(t);
}

// The title memory of a subresource as a whole: [start, end) from the texture's start.
static void subresource_memory(const ConsolePlaneLayout& p, u32 mip, u32 slice, u64* start, u64* end) {
    *start = p.base_offset + slice * p.slice_size + p.mips[mip].offset;
    *end = *start + p.mips[mip].size;
}

// The tile groups of a reserved texture, none mapped yet; nullptr (logged) if it can't have any.
static TextureTiles* make_tiles(const SyncEntry& e, ID3D12Resource* texture) {
    const D3D12_RESOURCE_DESC1& d = e.resource->desc;
    u32 mips = desktop_mips(e.resource), base_mip = e.resource->first_mip; // desktop mip m: console base_mip + m
    u32 slices = d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1 : d.DepthOrArraySize;
    u32 subresources = mips * slices;
    ConsoleTextureLayout layout;
    D3D12_SUBRESOURCE_TILING* tilings = (D3D12_SUBRESOURCE_TILING*)malloc(subresources * sizeof(*tilings));
    if (!tilings || !console_texture_layout(layout_desc(d), &layout) || layout.plane_count != 1) {
        free(tilings);
        LOG_W_ONCE("d3d12", "a reserved texture (format %d) can't have its tiles mapped; it reads zeros",
                   d.Format);
        return nullptr;
    }
    const ConsolePlaneLayout& p = layout.planes[0];
    UINT total = 0, tiling_count = subresources;
    D3D12_PACKED_MIP_INFO packed = {};
    D3D12_TILE_SHAPE shape = {};
    gpu_device()->GetResourceTiling(texture, &total, &packed, &shape, &tiling_count, 0, tilings);
    u32 standard = packed.NumStandardMips < mips ? packed.NumStandardMips : mips;
    u32 group_count = 0;
    for (u32 s = 0; s < slices; s++) {
        for (u32 m = 0; m < standard; m++) {
            const D3D12_SUBRESOURCE_TILING& st = tilings[s * mips + m];
            group_count += st.WidthInTiles * st.HeightInTiles * st.DepthInTiles;
        }
        group_count += standard < mips ? 1 : 0;
    }
    usize size = sizeof(TextureTiles) + (group_count ? group_count - 1 : 0) * sizeof(TileGroup) +
                 (total + subresources) * sizeof(u32);
    TextureTiles* t = (TextureTiles*)malloc(size);
    if (!t) {
        free(tilings);
        LOG_E("d3d12", "out of memory for the tiles of a texture at 0x%llx", e.start);
        return nullptr;
    }
    t->group_count = group_count;
    t->mips = mips;
    t->standard_mips = standard;
    t->tiles = (u32*)&t->groups[group_count ? group_count : 1];
    t->mapped = t->tiles + total;
    memset(t->tiles, 0xff, total * sizeof(u32));
    memset(t->mapped, 0, subresources * sizeof(u32));

    // The tile's shape in elements.
    FormatInfo f = format_info(d.Format);
    u32 tw = shape.WidthInTexels / (f.block_width ? f.block_width : 1);
    u32 th = shape.HeightInTexels / (f.block_height ? f.block_height : 1);
    u32 td = shape.DepthInTexels;
    u32 g = 0, first = 0;
    for (u32 s = 0; s < slices; s++) {
        for (u32 m = 0; m < standard; m++) {
            u32 sub = s * mips + m;
            const D3D12_SUBRESOURCE_TILING& st = tilings[sub];
            const ConsoleSubresourceAddressing& a = e.addressing[sub];
            bool paged = !a.linear && a.block_bytes_shift == 16 && (1u << a.block_shift[0]) == tw &&
                         (1u << a.block_shift[1]) == th && (1u << a.block_shift[2]) == td &&
                         !p.mips[base_mip + m].in_mip_tail;
            u64 whole_start, whole_end;
            subresource_memory(p, base_mip + m, s, &whole_start, &whole_end);
            for (u32 z = 0; z < st.DepthInTiles; z++) {
                for (u32 y = 0; y < st.HeightInTiles; y++) {
                    for (u32 x = 0; x < st.WidthInTiles; x++) {
                        TileGroup& tg = t->groups[g++];
                        tg.at = {x, y, z, sub};
                        tg.count = 1;
                        tg.first = first++;
                        tg.mapped = false;
                        if (paged) {
                            u64 at = console_subresource_offset(a, x * tw, y * th, z * td);
                            tg.source = ALIGN_DOWN(at, GPU_TILE_SIZE);
                            tg.source_size = GPU_TILE_SIZE;
                        } else {
                            tg.source = whole_start;
                            tg.source_size = whole_end - whole_start;
                        }
                    }
                }
            }
        }
        if (standard < mips) {
            u64 lo = ~0ull, hi = 0;
            for (u32 m = standard; m < mips; m++) {
                u64 start, end;
                subresource_memory(p, base_mip + m, s, &start, &end);
                lo = start < lo ? start : lo;
                hi = end > hi ? end : hi;
            }
            TileGroup& tg = t->groups[g++];
            tg.at = {0, 0, 0, s * mips + standard};
            tg.count = packed.NumTilesForPackedMips;
            tg.first = first;
            tg.mapped = false;
            tg.source = lo;
            tg.source_size = hi - lo;
            first += packed.NumTilesForPackedMips;
        }
    }
    free(tilings);
    if (first > total) {
        LOG_E("d3d12", "tiles of a texture at 0x%llx don't add up (%u of %u)", e.start, first, total);
        free(t);
        return nullptr;
    }
    return t;
}

// Tile mapping changes of one texture, gathered into few UpdateTileMappings calls.
#define TILE_BATCH 256

struct TileBatch {
    ID3D12Heap* heap; // the heap of every range; nullptr: unmapping
    u32 regions;
    u32 ranges;
    u32 tiles;
    D3D12_TILED_RESOURCE_COORDINATE at[TILE_BATCH];
    D3D12_TILE_REGION_SIZE sizes[TILE_BATCH];
    UINT offsets[TILE_BATCH];
    UINT counts[TILE_BATCH];
};

static TileBatch s_map_batch, s_unmap_batch; // under g_sync.lock

static void flush_tiles(ID3D12CommandQueue* queue, ID3D12Resource* texture, TileBatch* b) {
    if (!b->regions)
        return;
    if (b->heap) {
        queue->UpdateTileMappings(texture, b->regions, b->at, b->sizes, b->heap, b->ranges, nullptr,
                                  b->offsets, b->counts, D3D12_TILE_MAPPING_FLAG_NONE);
    } else {
        D3D12_TILE_RANGE_FLAGS null_range = D3D12_TILE_RANGE_FLAG_NULL;
        UINT count = b->tiles;
        queue->UpdateTileMappings(texture, b->regions, b->at, b->sizes, nullptr, 1, &null_range, nullptr,
                                  &count, D3D12_TILE_MAPPING_FLAG_NONE);
    }
    b->regions = b->ranges = b->tiles = 0;
}

// Adds a group to a batch: mapped to `tiles` of `heap`, or unmapped (heap nullptr). The tiles of all
// regions go to those of all ranges in order, so tiles following each other in a heap share a range.
static void add_group(ID3D12CommandQueue* queue, ID3D12Resource* texture, TileBatch* b, ID3D12Heap* heap,
                      const TileGroup& g, const u32* tiles) {
    if (b->regions && (b->heap != heap || b->regions == TILE_BATCH || b->ranges + g.count > TILE_BATCH))
        flush_tiles(queue, texture, b);
    b->heap = heap;
    b->at[b->regions] = g.at;
    b->sizes[b->regions] = {g.count, FALSE, 0, 0, 0};
    b->regions++;
    b->tiles += g.count;
    for (u32 i = 0; heap && i < g.count; i++) {
        u32 offset = gpu_tile_offset(tiles[i]);
        if (b->ranges && b->offsets[b->ranges - 1] + b->counts[b->ranges - 1] == offset) {
            b->counts[b->ranges - 1]++;
        } else {
            b->offsets[b->ranges] = offset;
            b->counts[b->ranges] = 1;
            b->ranges++;
        }
    }
}

static void count_mapped(TextureTiles* t, const TileGroup& g, s32 change) {
    if (g.count == 1 && g.at.Subresource % t->mips < t->standard_mips) {
        t->mapped[g.at.Subresource] += change;
        return;
    }
    u32 slice_start = g.at.Subresource - g.at.Subresource % t->mips;
    for (u32 m = t->standard_mips; m < t->mips; m++)
        t->mapped[slice_start + m] += change;
}

// Maps the groups whose title memory is mapped now, on `queue`, and unmaps the others. A texture whose
// mappings change is kept in `sources` until the queue passes the change. Returns whether any group is
// mapped.
static bool update_tiles(ID3D12CommandQueue* queue, TextureUploadSources* sources, SyncEntry& e,
                         ID3D12Resource* texture) {
    TextureTiles* t = e.tiles;
    bool any = false, changed = false;
    // Mips no view reads (more detailed than the first viewed one) get no tiles: their memory is
    // another texture's while the title streams this one (see ConsoleResource::view_first_mip).
    u32 first_mip = e.resource->view_first_mip == 0xFFFFFFFFu ? 0 : e.resource->view_first_mip;
    for (u32 i = 0; i < t->group_count; i++) {
        TileGroup& g = t->groups[i];
        bool packed = g.count > 1 || g.at.Subresource % t->mips >= t->standard_mips;
        bool viewed = packed || e.resource->first_mip + g.at.Subresource % t->mips >= first_mip;
        bool want = viewed && mem_is_mapped((const void*)(e.start + g.source), g.source_size);
        if (want != g.mapped) {
            u32* tiles = &t->tiles[g.first];
            if (want) {
                if (g.count > TILE_BATCH || !gpu_tiles_alloc(g.count, tiles))
                    continue; // without VRAM its texels read zero (the pool logged why)
                add_group(queue, texture, &s_map_batch, gpu_tile_heap(tiles[0]), g, tiles);
            } else {
                add_group(queue, texture, &s_unmap_batch, nullptr, g, tiles);
                gpu_tiles_free(g.count, tiles);
                for (u32 k = 0; k < g.count; k++)
                    tiles[k] = GPU_NO_TILE;
            }
            g.mapped = want;
            count_mapped(t, g, want ? 1 : -1);
            changed = true;
        }
        any |= g.mapped;
    }
    flush_tiles(queue, texture, &s_map_batch);
    flush_tiles(queue, texture, &s_unmap_batch);
    if (changed)
        keep_source(sources, texture); // room was checked before
    return any;
}

// A texture to detile in this submission: its subresources' places in the scratch ring.
// Memory the GPU can't see as one block (frames that don't follow each other) is copied as it is into
// the staging ring first, and detiled from there.
#define NOT_STAGED ~0ull

struct Load {
    SyncEntry* entry;
    ID3D12Resource* texture;
    u64 source;       // desktop GPU address of the texture's memory (or its staged copy)
    u64 staged;       // offset of its copy in this submission's staging region, or NOT_STAGED
    u32 first, count; // in s_copies
};

static SubresourceCopy s_copies[MAX_UPLOADS * 16]; // under g_sync.lock

static void scratch_barrier(ID3D12GraphicsCommandList8* list, ID3D12Resource* buffer,
                            D3D12_BARRIER_SYNC sync_before, D3D12_BARRIER_ACCESS before,
                            D3D12_BARRIER_SYNC sync_after, D3D12_BARRIER_ACCESS after) {
    D3D12_BUFFER_BARRIER b = {sync_before, sync_after, before, after, buffer, 0, UINT64_MAX};
    D3D12_BARRIER_GROUP g = {D3D12_BARRIER_TYPE_BUFFER, 1};
    g.pBufferBarriers = &b;
    list->Barrier(1, &g);
}

bool texture_sync_record(ID3D12CommandQueue* queue, ID3D12GraphicsCommandList8* list,
                         TextureUploadSources* sources, GpuRing* scratch, GpuRing* staging) {
    sources->count = 0;
    mem_poll_device_writes(); // finished ones mark their textures
    if (!g_sync.dirty)
        return false;
    Load loads[MAX_UPLOADS];
    u32 load_count = 0, copy_count = 0;
    u64 total = 0, staged_total = 0;
    bool copied = false;

    AcquireSRWLockExclusive(&g_sync.lock);
    // Linear textures the GPU can copy from title memory are copied now; the others are detiled on
    // the GPU within the budget, and the rest wait for the next submission.
    for (u32 i = 0; i < g_sync.count; i++) {
        SyncEntry& e = g_sync.entries[i];
        if (!e.dirty)
            continue;
        // Still being written: uploaded once the writes finish (they mark it again). Protecting it now
        // would also fail writes the kernel makes.
        bool traced = texture_sync_traced(e.resource->desc);
        if (mem_device_writes_pending(e.start, e.end - e.start)) {
            if (traced)
                LOG_I("d3d12", "texture trace: %p waits for device writes", (void*)e.resource);
            continue;
        }
        const D3D12_RESOURCE_DESC1& d = e.resource->desc;
        u32 subresources = desktop_mips(e.resource) *
                           (d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1 : d.DepthOrArraySize);
        bool full = load_count == MAX_UPLOADS || (load_count && total >= UPLOAD_BUDGET) ||
                    copy_count + subresources > ARRAY_COUNT(s_copies);
        if (full && !e.linear)
            continue;
        // Partly mapped textures are loaded too: the GPU reads what is mapped, and mapping more pages
        // marks the texture again.
        bool mapped = mem_is_mapped((const void*)e.start, e.end - e.start);
        bool any_mapped = mapped || mapped_bytes(e.start, e.end) != 0;
        // Watched before it is read: what the CPU writes from now on marks it again (watches cover
        // the mapped pages).
        if (any_mapped)
            mem_watch_writes((const void*)e.start, e.end - e.start);
        if (traced)
            trace_contents(full ? "loading (or deferred)" : "loading", e);
        if (mapped && e.linear && copy_from_title_memory(list, e.resource, e.end - e.start, sources)) {
            if (traced)
                LOG_I("d3d12", "texture trace: %p copied from title memory", (void*)e.resource);
            e.dirty = false;
            InterlockedDecrement(&g_sync.dirty);
            copied = true;
            continue;
        }
        if (full)
            continue;
        if (sources->count + 4 >= TEXTURE_UPLOAD_SOURCES)
            continue; // no room left: with the next submission
        e.dirty = false;
        InterlockedDecrement(&g_sync.dirty);
        if (!any_mapped) {
            // Nothing to read yet: pages arrive with a later mapping, which marks it again. Tiles of
            // memory the title unmapped go now.
            if (e.tiles)
                update_tiles(queue, sources, e, e.resource->texture);
            continue;
        }
        u64 unused;
        ID3D12Resource* texture = console_resource_desktop(e.resource, &unused, GPU_READ);
        if (texture && !e.addressing)
            e.addressing = make_addressing(e.resource, subresources);
        if (!texture || !e.addressing) {
            if (traced)
                LOG_I("d3d12", "texture trace: %p can't be loaded (%s)", (void*)e.resource,
                      texture ? "can't be detiled" : "no desktop texture");
            continue;
        }
        const u32* mapped_subresources = nullptr;
        if (e.resource->reserved) {
            if (!e.tiles)
                e.tiles = make_tiles(e, texture);
            if (!e.tiles || !update_tiles(queue, sources, e, texture)) {
                if (traced)
                    LOG_I("d3d12", "texture trace: %p has no tile with memory", (void*)e.resource);
                continue;
            }
            mapped_subresources = e.tiles->mapped;
        }
        u32 count = 0;
        u64 offset = total;
        if (!plan_texture(e.resource, mapped_subresources, &s_copies[copy_count],
                          ARRAY_COUNT(s_copies) - copy_count, &count, &offset)) {
            if (traced)
                LOG_I("d3d12", "texture trace: %p can't be loaded (no upload plan)", (void*)e.resource);
            continue;
        }
        keep_source(sources, texture);
        GpuAddress window = {};
        if (traced)
            LOG_I("d3d12", "texture trace: %p detiled on the GPU (%u subresources)", (void*)e.resource,
                  count);
        // The GPU's copy where the page tables hold one (GPU work may have written it), else title memory.
        if (gpu_translate_current(e.start, e.end - e.start, &window)) {
            keep_source(sources, window.buffer);
            loads[load_count++] = {&e, texture, window.desktop_address, NOT_STAGED, copy_count, count};
        } else {
            loads[load_count++] = {&e, texture, 0, staged_total, copy_count, count};
            staged_total = ALIGN_UP(staged_total + (e.end - e.start), (u64)256);
        }
        copy_count += count;
        total = offset;
    }
    u64 base = 0, staged_base = 0;
    if (load_count && (!gpu_ring_alloc(scratch, total, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, &base) ||
                       (staged_total && !gpu_ring_alloc(staging, staged_total, 256, &staged_base)))) {
        for (u32 l = 0; l < load_count; l++) { // with the next submission
            loads[l].entry->dirty = true;
            InterlockedIncrement(&g_sync.dirty);
        }
        load_count = 0;
    }
    if (!load_count) {
        ReleaseSRWLockExclusive(&g_sync.lock);
        console_memory_check(); // textures made meanwhile, tiles mapped now
        return copied;
    }
    keep_source(sources, scratch->buffer);
    if (staged_total) {
        keep_source(sources, staging->buffer);
        for (u32 l = 0; l < load_count; l++) {
            Load& ld = loads[l];
            if (ld.staged == NOT_STAGED)
                continue;
            u64 size = ld.entry->end - ld.entry->start;
            copy_mapped(staging->mapped + staged_base + ld.staged, ld.entry->start, ld.entry->start + size,
                        !ld.entry->resource->reserved);
            ld.source = staging->address + staged_base + ld.staged;
        }
    }

    // Detile everything into the ring, then copy it into the textures.
    scratch_barrier(list, scratch->buffer, D3D12_BARRIER_SYNC_NONE, D3D12_BARRIER_ACCESS_NO_ACCESS,
                    D3D12_BARRIER_SYNC_COMPUTE_SHADING, D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
    for (u32 l = 0; l < load_count; l++) {
        const Load& ld = loads[l];
        u32 mips = desktop_mips(ld.entry->resource);
        for (u32 k = 0; k < ld.count; k++) {
            const SubresourceCopy& c = s_copies[ld.first + k];
            gpu_untile(list, ld.source, ld.entry->addressing[c.slice * mips + c.mip],
                       scratch->address + base + c.offset, (u32)c.row_pitch, (u32)c.slice_pitch);
        }
    }
    scratch_barrier(list, scratch->buffer, D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                    D3D12_BARRIER_ACCESS_UNORDERED_ACCESS, D3D12_BARRIER_SYNC_COPY,
                    D3D12_BARRIER_ACCESS_COPY_SOURCE);
    for (u32 l = 0; l < load_count; l++) {
        const Load& ld = loads[l];
        const D3D12_RESOURCE_DESC1& d = ld.entry->resource->desc;
        u32 mips = desktop_mips(ld.entry->resource);
        texture_barrier(list, ld.texture, true);
        for (u32 k = 0; k < ld.count; k++) {
            const SubresourceCopy& c = s_copies[ld.first + k];
            D3D12_TEXTURE_COPY_LOCATION dst = {ld.texture, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
            dst.SubresourceIndex = c.mip + c.slice * mips;
            D3D12_TEXTURE_COPY_LOCATION src = {scratch->buffer, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
            src.PlacedFootprint.Offset = base + c.offset;
            src.PlacedFootprint.Footprint = {format_desktop_resource(d.Format), c.width, c.height, c.depth,
                                             (UINT)c.row_pitch};
            list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        }
        texture_barrier(list, ld.texture, false);
    }
    scratch_barrier(list, scratch->buffer, D3D12_BARRIER_SYNC_COPY, D3D12_BARRIER_ACCESS_COPY_SOURCE,
                    D3D12_BARRIER_SYNC_NONE, D3D12_BARRIER_ACCESS_NO_ACCESS);
    ReleaseSRWLockExclusive(&g_sync.lock);
    console_memory_check();
    return true;
}
