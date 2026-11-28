// Console ID3D12Resource / ID3D12Resource1 / ID3D12Resource2.

#include <windows.h>
#include <psapi.h>
#include <stdio.h>
#include <stdlib.h>

#include "common/log.h"
#include "core/memory.h"
#include "hle/d3d12/barriers.h"
#include "hle/d3d12/d3d12_objects.h"
#include "hle/d3d12/target_tiles.h"
#include "hle/d3d12/texture_sync.h"
#include "hle/d3d12/d3d12_tables.h"
#include "gpu/console_layout.h"
#include "gpu/formats.h"
#include "gpu/gpu_window.h"
#include "gpu/tile_pool.h"
#include "hle/xmem/xmem.h"

static const GUID* const k_resource_iids[] = {&__uuidof(ID3D12Object),    &__uuidof(ID3D12DeviceChild),
                                              &__uuidof(ID3D12Pageable),  &__uuidof(ID3D12Resource),
                                              &__uuidof(ID3D12Resource1), &__uuidof(ID3D12Resource2)};

static void resource_destroy(void* object);
static void forget_placed_texture(ConsoleResource* r);
static void count_texture(ConsoleResource* r, const D3D12_RESOURCE_DESC1& desktop_desc);
static void uncount_texture(ConsoleResource* r);
static D3D12_RESOURCE_DESC desc0_from_desc1(const D3D12_RESOURCE_DESC1& d);

static const ConsoleClass k_resource_class = {"ID3D12Resource", k_resource_iids, ARRAY_COUNT(k_resource_iids),
                                              resource_destroy};

static void* const* g_resource_table = nullptr;

bool console_translate_gpu_address(u64 address, u64 size, GpuAccess access, u64* desktop_address) {
    GpuAddress a;
    if (!gpu_translate(address, size, access, &a))
        return false;
    *desktop_address = a.desktop_address;
    return true;
}

bool console_translate_coherent(u64 address, u64 size, u64* desktop_address) {
    GpuAddress a;
    if (!gpu_window_translate(address, size, &a))
        return false;
    *desktop_address = a.desktop_address;
    return true;
}

// --- desktop backing ---------------------------------------------------------------------------------

// A texture couldn't be created for lack of memory: the next console_memory_check reports it.
static volatile bool g_texture_out_of_memory;

// Console resource flags that desktop D3D12 knows; the rest describe console hardware (compression,
// tiling preferences) or are implied on desktop.
static const u32 k_desktop_resource_flags =
    D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL |
    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE |
    D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS | D3D12_RESOURCE_FLAG_RAYTRACING_ACCELERATION_STRUCTURE;

// Console layouts are hardware swizzle modes; desktop picks its own.
static D3D12_TEXTURE_LAYOUT desktop_texture_layout(D3D12_TEXTURE_LAYOUT layout) {
    return layout == D3D12_TEXTURE_LAYOUT_ROW_MAJOR ? layout : D3D12_TEXTURE_LAYOUT_UNKNOWN;
}

// Console depth textures can also be written as UAVs; desktop allows one or the other. Depth
// rendering is what they're for, so their UAV views stay null and their UAV layouts become common
// ones (console_texture_layout).
bool console_texture_loses_uav(const D3D12_RESOURCE_DESC1& desc) {
    const u32 depth_uav =
        D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    return desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER && (desc.Flags & depth_uav) == depth_uav;
}

// The desktop description of a console texture.
static D3D12_RESOURCE_DESC1 desktop_texture_desc(const D3D12_RESOURCE_DESC1& console, u32 first_mip) {
    D3D12_RESOURCE_DESC1 desc = console;
    desc.Alignment = 0;
    if (first_mip) { // from that mip on (see ConsoleResource::first_mip; 2D textures only)
        desc.Width = desc.Width >> first_mip ? desc.Width >> first_mip : 1;
        desc.Height = desc.Height >> first_mip ? desc.Height >> first_mip : 1;
        desc.MipLevels = (u16)(desc.MipLevels - first_mip);
    }
    desc.Format = format_desktop_resource(console.Format);
    desc.Layout = desktop_texture_layout(desc.Layout);
    desc.Flags = (D3D12_RESOURCE_FLAGS)(desc.Flags & k_desktop_resource_flags);
    if (console_texture_loses_uav(console)) {
        LOG_W_ONCE("d3d12",
                   "depth textures with UAV access have no desktop equivalent; their UAV views are null");
        desc.Flags = (D3D12_RESOURCE_FLAGS)(desc.Flags & ~D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    }
    return desc;
}

// Whether desktop D3D12 makes reserved textures of this description (with its features, which are
// asked once).
static bool reserved_texture_supported(const D3D12_RESOURCE_DESC1& desc) {
    static volatile long tier = -1;
    static volatile u8 tiled_formats[256]; // 0: not asked yet, 1: tiled, 2: not
    if (tier < 0) {
        D3D12_FEATURE_DATA_D3D12_OPTIONS o = {};
        gpu_device()->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &o, sizeof(o));
        tier = o.TiledResourcesTier;
        if (tier < D3D12_TILED_RESOURCES_TIER_2)
            LOG_W("d3d12", "the GPU's tiled resources tier is below 2: textures are created whole, and "
                           "shaders that clamp the sampled LOD don't build");
    }
    // Tier 2 reads zeros from tiles without memory and drops writes to them, which uploads rely on.
    if (tier < D3D12_TILED_RESOURCES_TIER_2 || desc.SampleDesc.Count != 1 || (u32)desc.Format >= 256)
        return false;
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
        (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE3D || tier < D3D12_TILED_RESOURCES_TIER_3))
        return false;
    if (!tiled_formats[desc.Format]) {
        D3D12_FEATURE_DATA_FORMAT_SUPPORT f = {desc.Format};
        bool tiled =
            SUCCEEDED(gpu_device()->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &f, sizeof(f))) &&
            (f.Support2 & D3D12_FORMAT_SUPPORT2_TILED);
        tiled_formats[desc.Format] = tiled ? 1 : 2;
    }
    return tiled_formats[desc.Format] == 1;
}

// Textures following the title's mappings get reserved desktop textures (see texture_sync): their
// tiles get VRAM only where the title mapped memory. nullptr where that can't be.
static ID3D12Resource* create_reserved_texture(ConsoleResource* r, const D3D12_RESOURCE_DESC1& desc,
                                               D3D12_BARRIER_LAYOUT layout) {
    if (!texture_sync_tiles(r->desc) || !reserved_texture_supported(desc))
        return nullptr;
    D3D12_RESOURCE_DESC reserved = desc0_from_desc1(desc);
    reserved.Layout = D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE;
    ID3D12Resource* texture = nullptr;
    HRESULT hr = gpu_device()->CreateReservedResource2(&reserved, layout, nullptr, nullptr, 0, nullptr,
                                                       IID_PPV_ARGS(&texture));
    if (FAILED(hr)) {
        if (!gpu_device_lost(hr))
            LOG_W("d3d12",
                  "cannot create a reserved texture for 0x%llx (dim %d, %llux%ux%u, mips %u, format %d): "
                  "0x%08lx; making a whole one",
                  r->gpu_address, r->desc.Dimension, r->desc.Width, r->desc.Height, r->desc.DepthOrArraySize,
                  r->desc.MipLevels, r->desc.Format, (unsigned long)hr);
        return nullptr;
    }
    r->reserved = true;
    return texture;
}

// Textures the GPU only reads whose most detailed mip is small take 4 KiB slots of the tile pool, not a
// 64 KiB tile or more. nullptr where that can't be.
static ID3D12Resource* create_small_texture(ConsoleResource* r, const D3D12_RESOURCE_DESC1& desc,
                                            D3D12_BARRIER_LAYOUT layout) {
    if (!texture_sync_tracks(r->desc))
        return nullptr;
    D3D12_RESOURCE_DESC1 placed = desc;
    placed.Alignment = D3D12_SMALL_RESOURCE_PLACEMENT_ALIGNMENT;
    D3D12_RESOURCE_ALLOCATION_INFO info = gpu_device()->GetResourceAllocationInfo2(0, 1, &placed, nullptr);
    if (info.Alignment != D3D12_SMALL_RESOURCE_PLACEMENT_ALIGNMENT || info.SizeInBytes > GPU_TILE_SIZE)
        return nullptr;
    GpuSlots slots;
    if (!gpu_slots_alloc(info.SizeInBytes, &slots))
        return nullptr;
    ID3D12Resource* texture = nullptr;
    HRESULT hr = gpu_device()->CreatePlacedResource2(gpu_tile_heap(slots.tile), gpu_slots_offset(slots), &placed,
                                                     layout, nullptr, 0, nullptr, IID_PPV_ARGS(&texture));
    if (FAILED(hr)) {
        gpu_slots_free(slots);
        if (!gpu_device_lost(hr))
            LOG_W_ONCE("d3d12", "cannot place a placed texture (0x%08lx); making it the usual way",
                       (unsigned long)hr);
        return nullptr;
    }
    r->slots = slots;
    return texture;
}

// Render target, depth and UAV textures over title memory get reserved desktop textures whose tiles are
// those of the memory (see target_tiles): textures the title aliases share video memory. nullptr where
// that can't be.
static ID3D12Resource* create_target_texture(ConsoleResource* r, const D3D12_RESOURCE_DESC1& desc,
                                             D3D12_BARRIER_LAYOUT layout, const D3D12_CLEAR_VALUE* clear) {
    if (!target_tiles_eligible(r) || !reserved_texture_supported(desc))
        return nullptr;
    D3D12_RESOURCE_DESC reserved = desc0_from_desc1(desc);
    reserved.Layout = D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE;
    ID3D12Resource* texture = nullptr;
    HRESULT hr = gpu_device()->CreateReservedResource2(&reserved, layout, clear, nullptr, 0, nullptr,
                                                       IID_PPV_ARGS(&texture));
    if (hr == E_INVALIDARG && clear) // see create_texture
        hr = gpu_device()->CreateReservedResource2(&reserved, layout, nullptr, nullptr, 0, nullptr,
                                                   IID_PPV_ARGS(&texture));
    if (FAILED(hr)) {
        if (!gpu_device_lost(hr))
            LOG_W_ONCE("d3d12",
                       "cannot create a reserved render target texture (format %d, flags 0x%x): 0x%08lx; "
                       "such textures are whole",
                       desc.Format, desc.Flags, (unsigned long)hr);
        return nullptr;
    }
    if (!target_tiles_add(r, texture, desc)) {
        texture->Release(); // never used
        return nullptr;
    }
    return texture;
}

static ID3D12Resource* create_texture(ConsoleResource* r) {
    D3D12_RESOURCE_DESC1 desc = desktop_texture_desc(r->desc, r->first_mip);
    if (desc.Format == DXGI_FORMAT_UNKNOWN) {
        LOG_E("d3d12", "texture at 0x%llx has format %d, which desktop D3D12 has no equivalent for",
              r->gpu_address, r->desc.Format);
        return nullptr;
    }
    D3D12_HEAP_PROPERTIES heap = {D3D12_HEAP_TYPE_DEFAULT};
    D3D12_CLEAR_VALUE clear_value = r->clear_value;
    clear_value.Format = format_desktop(r->clear_value.Format);
    bool clear_allowed =
        desc.Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    const D3D12_CLEAR_VALUE* clear = r->has_clear_value && clear_allowed ? &clear_value : nullptr;

    ID3D12Resource* texture = nullptr;
    // The layout the title's first barrier will name. A texture created undefined has no contents
    // to keep, so any layout serves.
    D3D12_BARRIER_LAYOUT layout = console_texture_layout(
        r, r->initial_layout == D3D12_BARRIER_LAYOUT_UNDEFINED ? D3D12_BARRIER_LAYOUT_COMMON
                                                               : r->initial_layout);
    texture = create_small_texture(r, desc, layout);
    if (!texture)
        texture = create_reserved_texture(r, desc, layout);
    if (!texture)
        texture = create_target_texture(r, desc, layout, clear);
    if (texture) {
        const char* kind = r->reserved ? "reserved" : r->target ? "render target" : "small";
        gpu_set_name(texture, "%s texture at 0x%llx (dim %d, %llux%ux%u, format %d, from mip %u)", kind,
                     r->gpu_address, r->desc.Dimension, r->desc.Width,
                     r->desc.Height, r->desc.DepthOrArraySize, r->desc.Format, r->first_mip);
        count_texture(r, desc);
        return texture;
    }
    HRESULT hr = gpu_device()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &desc, layout, clear,
                                                        nullptr, 0, nullptr, IID_PPV_ARGS(&texture));
    if (hr == E_INVALIDARG && clear) {
        // The clear value only tells the driver which clears are fast; desktop is stricter about
        // its format (never typeless), so do without one it refuses.
        hr = gpu_device()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &desc, layout, nullptr,
                                                    nullptr, 0, nullptr, IID_PPV_ARGS(&texture));
        if (SUCCEEDED(hr))
            LOG_W_ONCE("d3d12", "texture clear value format %d refused; creating textures without it",
                       r->clear_value.Format);
    }
    if (FAILED(hr)) {
        LOG_E("d3d12",
              "cannot create the desktop texture for 0x%llx (dim %d, %llux%ux%u, mips %u, format %d, "
              "flags 0x%x): 0x%08lx",
              r->gpu_address, r->desc.Dimension, r->desc.Width, r->desc.Height, r->desc.DepthOrArraySize,
              r->desc.MipLevels, r->desc.Format, r->desc.Flags, (unsigned long)hr);
        static volatile long reported = 0;
        if (hr == E_OUTOFMEMORY && log_once_flag(&reported))
            g_texture_out_of_memory = true; // reported by the next check (locks are held here)
        return nullptr;
    }
    count_texture(r, desc);
    gpu_set_name(texture, "%s at 0x%llx (dim %d, %llux%ux%u, format %d)",
                 r->placed_view ? "texture of placed memory" : "texture", r->gpu_address, r->desc.Dimension,
                 r->desc.Width, r->desc.Height, r->desc.DepthOrArraySize, r->desc.Format);
    return texture;
}

// Textures take what their console layout takes: titles place them in their own memory and stream
// data into them in that layout.
D3D12_RESOURCE_ALLOCATION_INFO console_resource_allocation_info(const D3D12_RESOURCE_DESC1& desc) {
    if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
        u64 alignment = desc.Alignment ? desc.Alignment : D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
        return {ALIGN_UP(desc.Width, alignment), alignment};
    }
    ConsoleTextureDesc t = {};
    t.dimension = desc.Dimension;
    t.width = desc.Width;
    t.height = desc.Height;
    t.depth_or_array_size = desc.DepthOrArraySize;
    t.mip_levels = desc.MipLevels;
    t.format = desc.Format;
    t.sample_count = desc.SampleDesc.Count;
    t.layout = desc.Layout;
    t.flags = desc.Flags;
    ConsoleTextureLayout layout;
    if (!console_texture_layout(t, &layout)) {
        LOG_E("d3d12",
              "no console layout for texture dim %d, %llux%ux%u, mips %u, format %d, layout 0x%x, flags 0x%x",
              desc.Dimension, desc.Width, desc.Height, desc.DepthOrArraySize, desc.MipLevels, desc.Format,
              desc.Layout, desc.Flags);
        return {UINT64_MAX, 0};
    }
    return {layout.size, layout.base_alignment};
}

ID3D12Resource* console_resource_desktop(ConsoleResource* r, u64* offset, GpuAccess access) {
    if (r->desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
        GpuAddress a;
        if (!gpu_translate(r->gpu_address, r->desc.Width, access, &a))
            return nullptr;
        *offset = a.offset;
        return a.buffer;
    }
    *offset = 0;
    AcquireSRWLockExclusive(&r->lock);
    if (!r->texture)
        r->texture = create_texture(r);
    ID3D12Resource* texture = r->texture;
    ReleaseSRWLockExclusive(&r->lock);
    return texture;
}

static void resource_destroy(void* object) {
    ConsoleResource* r = (ConsoleResource*)object;
    if (r->placed_view)
        forget_placed_texture(r);
    if (texture_sync_tracks(r->desc))
        texture_sync_remove(r);
    uncount_texture(r);
    target_tiles_remove(r);
    gpu_retire(r->texture);
    gpu_slots_free(r->slots); // handed out again once the GPU is done with the texture
    if (r->owned_memory)
        xmem_free(r->owned_memory, r->owned_attributes);
}

// --- entries -----------------------------------------------------------------------------------------

static D3D12_RESOURCE_DESC desc0_from_desc1(const D3D12_RESOURCE_DESC1& d) {
    D3D12_RESOURCE_DESC r;
    r.Dimension = d.Dimension;
    r.Alignment = d.Alignment;
    r.Width = d.Width;
    r.Height = d.Height;
    r.DepthOrArraySize = d.DepthOrArraySize;
    r.MipLevels = d.MipLevels;
    r.Format = d.Format;
    r.SampleDesc = d.SampleDesc;
    r.Layout = d.Layout;
    r.Flags = d.Flags;
    return r;
}

// Field by field: the version-1 description's extra member starts inside the padding at the end of
// the version-0 one, so the two can't be copied as blocks.
D3D12_RESOURCE_DESC1 console_desc1_from_desc(const D3D12_RESOURCE_DESC& d) {
    D3D12_RESOURCE_DESC1 r = {};
    r.Dimension = d.Dimension;
    r.Alignment = d.Alignment;
    r.Width = d.Width;
    r.Height = d.Height;
    r.DepthOrArraySize = d.DepthOrArraySize;
    r.MipLevels = d.MipLevels;
    r.Format = d.Format;
    r.SampleDesc = d.SampleDesc;
    r.Layout = d.Layout;
    r.Flags = d.Flags;
    return r;
}

// The title's memory is the resource's memory, mapped for the CPU at the GPU address.
static HRESULT WINAPI resource_map(ConsoleResource* self, UINT subresource, const D3D12_RANGE*, void** data) {
    if (subresource != 0) {
        LOG_W("d3d12", "ID3D12Resource::Map: subresource %u not supported", subresource);
        return E_INVALIDARG;
    }
    if (data)
        *data = (void*)self->gpu_address;
    return S_OK;
}

static void WINAPI resource_unmap(ConsoleResource*, UINT, const D3D12_RANGE*) {}

static void WINAPI resource_get_desc(ConsoleResource* self, D3D12_RESOURCE_DESC* out) {
    *out = desc0_from_desc1(self->desc);
}

static D3D12_GPU_VIRTUAL_ADDRESS WINAPI resource_get_gpu_virtual_address(ConsoleResource* self) {
    return self->gpu_address;
}

static HRESULT WINAPI resource_get_heap_properties(ConsoleResource* self, D3D12_HEAP_PROPERTIES* properties,
                                                   D3D12_HEAP_FLAGS* flags) {
    if (properties)
        *properties = self->heap_properties;
    if (flags)
        *flags = self->heap_flags;
    return S_OK;
}

// Struct returns go through a hidden pointer, received before the object.
static D3D12_RESOURCE_DESC* WINAPI resource_get_placement_desc_x(D3D12_RESOURCE_DESC* ret,
                                                                 ConsoleResource* self) {
    *ret = desc0_from_desc1(self->desc);
    return ret;
}

static D3D12_RESOURCE_DESC1* WINAPI resource_get_desc1(D3D12_RESOURCE_DESC1* ret, ConsoleResource* self) {
    *ret = self->desc;
    return ret;
}

static const ConsoleEntry k_resource_impl[] = {
    CONSOLE_ENTRY(8, resource_map),
    CONSOLE_ENTRY(9, resource_unmap),
    CONSOLE_ENTRY(10, resource_get_desc),
    CONSOLE_ENTRY(11, resource_get_gpu_virtual_address),
    CONSOLE_ENTRY(14, resource_get_heap_properties),
    CONSOLE_ENTRY(15, resource_get_placement_desc_x),
    CONSOLE_ENTRY(19, resource_get_desc1),
};

void console_resource_init() {
    g_resource_table =
        console_build_table("ID3D12Resource", k_resource_entries, ARRAY_COUNT(k_resource_entries),
                            k_resource_impl, ARRAY_COUNT(k_resource_impl));
}

// Creates the object for a resource at `gpu_address`; the caller hands it to the title.
static ConsoleResource* new_resource(u64 gpu_address, const D3D12_RESOURCE_DESC1* desc,
                                     const D3D12_HEAP_PROPERTIES& heap, D3D12_HEAP_FLAGS heap_flags,
                                     D3D12_BARRIER_LAYOUT initial_layout, const D3D12_CLEAR_VALUE* clear,
                                     u32 first_mip = 0) {
    ConsoleResource* r = (ConsoleResource*)console_alloc(sizeof(ConsoleResource), &k_resource_class,
                                                         g_resource_table, nullptr);
    r->desc = *desc;
    r->gpu_address = gpu_address;
    r->heap_properties = heap;
    r->heap_flags = heap_flags;
    r->initial_layout = initial_layout;
    if (clear) {
        r->has_clear_value = true;
        r->clear_value = *clear;
    }
    InitializeSRWLock(&r->lock);
    r->first_mip = first_mip;
    if (texture_sync_tracks(r->desc))
        texture_sync_add(r);
    else if (texture_sync_traced(r->desc))
        LOG_I("d3d12", "texture trace: %p at 0x%llx isn't synced (flags 0x%x, format %d, layout 0x%x)",
              (void*)r, gpu_address, r->desc.Flags, r->desc.Format, r->desc.Layout);
    return r;
}

HRESULT console_create_committed_resource(ConsoleDevice* device, const D3D12_HEAP_PROPERTIES* heap,
                                          D3D12_HEAP_FLAGS heap_flags, const D3D12_RESOURCE_DESC1* desc,
                                          D3D12_BARRIER_LAYOUT initial_layout, const D3D12_CLEAR_VALUE* clear,
                                          REFIID iid, void** out) {
    if (!heap || !desc)
        return E_INVALIDARG;
    D3D12_RESOURCE_ALLOCATION_INFO info = console_resource_allocation_info(*desc);
    if (info.SizeInBytes == UINT64_MAX)
        return E_INVALIDARG;
    // CPU caching of the memory follows what the heap is for.
    bool write_combined = heap->Type == D3D12_HEAP_TYPE_UPLOAD ||
                          (heap->Type == D3D12_HEAP_TYPE_CUSTOM &&
                           heap->CPUPageProperty == D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE);
    u32 object =
        (heap_flags & D3D12_HEAP_FLAG_ALLOW_DISPLAY) ? XMEM_D3D_OBJECT_PRESENTABLE : XMEM_D3D_OBJECT_RESOURCE;
    u32 memory_type = write_combined ? XMEM_TYPE_GRAPHICS_WRITECOMBINE : XMEM_TYPE_GRAPHICS_CACHEABLE;
    u64 attributes = xmem_d3d_attributes(object, memory_type, info.SizeInBytes, info.Alignment, false);
    void* memory = xmem_alloc(info.SizeInBytes, attributes);
    if (!memory) {
        LOG_E("d3d12", "no memory for a committed resource of %llu KiB", info.SizeInBytes >> 10);
        return E_OUTOFMEMORY;
    }
    LOG_D("d3d12",
          "committed resource at %p: heap %d, heap flags 0x%x, dim %d, %llux%ux%u, mips %u, format %d, "
          "flags 0x%x",
          memory, heap->Type, heap_flags, desc->Dimension, desc->Width, desc->Height, desc->DepthOrArraySize,
          desc->MipLevels, desc->Format, desc->Flags);
    ConsoleResource* r = new_resource((u64)memory, desc, *heap, heap_flags, initial_layout, clear);
    r->owned_memory = memory;
    r->owned_attributes = attributes;
    return console_return(r, iid, out);
}

HRESULT console_create_placed_resource(ConsoleDevice* device, u64 gpu_address,
                                       const D3D12_RESOURCE_DESC1* desc, D3D12_BARRIER_LAYOUT initial_layout,
                                       const D3D12_CLEAR_VALUE* clear, REFIID iid, void** out) {
    if (!desc || !gpu_address)
        return E_INVALIDARG;
    if (desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER && desc->Width == 0)
        return E_INVALIDARG;
    LOG_D("d3d12",
          "placed resource at 0x%llx: dim %d, %llux%ux%u, mips %u, format %d, layout 0x%x, flags 0x%x",
          gpu_address, desc->Dimension, desc->Width, desc->Height, desc->DepthOrArraySize, desc->MipLevels,
          desc->Format, desc->Layout, desc->Flags);

    D3D12_HEAP_PROPERTIES heap = {D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
                                  D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
    ConsoleResource* r = new_resource(gpu_address, desc, heap, D3D12_HEAP_FLAG_NONE, initial_layout, clear);
    return console_return(r, iid, out);
}

// --- textures of placed memory ----------------------------------------------------------------------

static struct {
    ConsoleResource** entries; // sorted by address; the slots keeping them hold the references
    u32 count;
    u32 capacity;
    SRWLOCK lock;
} g_placed = {nullptr, 0, 0, SRWLOCK_INIT};

static u32 placed_lower_bound(u64 address) {
    u32 lo = 0, hi = g_placed.count;
    while (lo < hi) {
        u32 mid = lo + (hi - lo) / 2;
        if (g_placed.entries[mid]->gpu_address < address)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

// Whether two descriptions lay out the same texture (the placement alignment doesn't matter).
static bool same_texture(const D3D12_RESOURCE_DESC1& a, const D3D12_RESOURCE_DESC1& b) {
    return a.Dimension == b.Dimension && a.Width == b.Width && a.Height == b.Height &&
           a.DepthOrArraySize == b.DepthOrArraySize && a.MipLevels == b.MipLevels && a.Format == b.Format &&
           a.SampleDesc.Count == b.SampleDesc.Count && a.SampleDesc.Quality == b.SampleDesc.Quality &&
           a.Layout == b.Layout && a.Flags == b.Flags;
}

static void forget_placed_texture(ConsoleResource* r) {
    AcquireSRWLockExclusive(&g_placed.lock);
    for (u32 i = placed_lower_bound(r->gpu_address);
         i < g_placed.count && g_placed.entries[i]->gpu_address == r->gpu_address; i++) {
        if (g_placed.entries[i] == r) {
            memmove(&g_placed.entries[i], &g_placed.entries[i + 1],
                    (g_placed.count - i - 1) * sizeof(ConsoleResource*));
            g_placed.count--;
            break;
        }
    }
    ReleaseSRWLockExclusive(&g_placed.lock);
}

// The first mip a desktop texture of `desc` can start at, at most `wanted`: 2D textures only, and the
// smaller texture must be a valid desktop one (block compressed formats need whole blocks).
static u32 placed_first_mip(const D3D12_RESOURCE_DESC1& desc, u32 wanted) {
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || !desc.MipLevels)
        return 0;
    FormatInfo f = format_info(format_desktop_resource(desc.Format));
    u32 bw = f.block_width ? f.block_width : 1, bh = f.block_height ? f.block_height : 1;
    u32 k = wanted < desc.MipLevels ? wanted : desc.MipLevels - 1u;
    for (; k > 0; k--) {
        u64 w = desc.Width >> k;
        u32 h = desc.Height >> k;
        if (w && h && w % bw == 0 && h % bh == 0)
            break;
    }
    return k;
}

ConsoleResource* console_placed_texture(u64 gpu_address, const D3D12_RESOURCE_DESC1& desc, u32 wanted_first_mip) {
    D3D12_RESOURCE_ALLOCATION_INFO info = console_resource_allocation_info(desc);
    if (!gpu_address || info.SizeInBytes == UINT64_MAX)
        return nullptr;
    u32 first_mip = placed_first_mip(desc, wanted_first_mip);
    AcquireSRWLockExclusive(&g_placed.lock);
    u32 at = placed_lower_bound(gpu_address);
    for (u32 i = at; i < g_placed.count && g_placed.entries[i]->gpu_address == gpu_address; i++) {
        ConsoleResource* r = g_placed.entries[i];
        // One being destroyed (its last slot was just rewritten) is left to go.
        if (same_texture(r->desc, desc) && r->first_mip == first_mip && console_try_add_ref(r)) {
            ReleaseSRWLockExclusive(&g_placed.lock);
            texture_sync_mark(gpu_address, info.SizeInBytes);
            return r;
        }
    }
    if (g_placed.count == g_placed.capacity) {
        u32 capacity = g_placed.capacity ? g_placed.capacity * 2 : 1024;
        ConsoleResource** grown =
            (ConsoleResource**)realloc(g_placed.entries, capacity * sizeof(ConsoleResource*));
        if (!grown) {
            ReleaseSRWLockExclusive(&g_placed.lock);
            LOG_E("d3d12", "out of memory tracking textures of placed memory");
            return nullptr;
        }
        g_placed.entries = grown;
        g_placed.capacity = capacity;
    }
    D3D12_HEAP_PROPERTIES heap = {D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
                                  D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
    ConsoleResource* r = new_resource(gpu_address, &desc, heap, D3D12_HEAP_FLAG_NONE, D3D12_BARRIER_LAYOUT_COMMON,
                                      nullptr, first_mip);
    r->placed_view = true;
    r->view_first_mip = 0xFFFFFFFFu;
    memmove(&g_placed.entries[at + 1], &g_placed.entries[at],
            (g_placed.count - at) * sizeof(ConsoleResource*));
    g_placed.entries[at] = r;
    g_placed.count++;
    ReleaseSRWLockExclusive(&g_placed.lock);
    return r;
}

// --- memory accounting -------------------------------------------------------------------------------
//
// Desktop textures are memory on top of the title's own, which holds the same contents in console
// layout. Each is counted by what it backs; reports compare that to the title memory the textures
// describe (aliased textures share it) and to how much of it the title has mapped.

#define TEXTURE_REPORT_STEP MiB(256)

enum TextureKind { TEXTURE_WRITTEN, TEXTURE_SAMPLED, TEXTURE_PLACED_VIEW, TEXTURE_KIND_COUNT };

static const char* const k_texture_kind_names[TEXTURE_KIND_COUNT] = {
    "render target, depth and UAV textures", "sampled textures", "textures of placed views"};

static struct {
    ConsoleResource** live; // resources with a desktop texture (ConsoleResource::texture_index)
    u32 count;
    u32 capacity;
    u64 bytes;       // their desktop size (reserved ones' tiles are the tile pool's)
    u64 next_report; // the next high of `bytes` and tiles that logs a report
    SRWLOCK lock;
} g_textures = {nullptr, 0, 0, 0, TEXTURE_REPORT_STEP, SRWLOCK_INIT};

static TextureKind texture_kind(const ConsoleResource* r) {
    const u32 written = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL |
                        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (r->placed_view)
        return TEXTURE_PLACED_VIEW;
    return (r->desc.Flags & written) ? TEXTURE_WRITTEN : TEXTURE_SAMPLED;
}

// Counts the desktop texture just created for `r`.
static void count_texture(ConsoleResource* r, const D3D12_RESOURCE_DESC1& desktop_desc) {
    // Reserved textures take tiles, which the tile pool counts (and target_tiles, for render targets).
    D3D12_RESOURCE_ALLOCATION_INFO desktop = {};
    if (r->slots.count)
        desktop.SizeInBytes = (u64)r->slots.count * GPU_SLOT_SIZE;
    else if (!r->reserved && !r->target)
        desktop = gpu_device()->GetResourceAllocationInfo2(0, 1, &desktop_desc, nullptr);
    D3D12_RESOURCE_ALLOCATION_INFO console = console_resource_allocation_info(r->desc);
    AcquireSRWLockExclusive(&g_textures.lock);
    if (g_textures.count == g_textures.capacity) {
        u32 capacity = g_textures.capacity ? g_textures.capacity * 2 : 1024;
        ConsoleResource** grown =
            (ConsoleResource**)realloc(g_textures.live, capacity * sizeof(ConsoleResource*));
        if (!grown) {
            ReleaseSRWLockExclusive(&g_textures.lock);
            return; // left out of the reports
        }
        g_textures.live = grown;
        g_textures.capacity = capacity;
    }
    r->texture_counted = true;
    r->texture_index = g_textures.count;
    r->desktop_bytes = desktop.SizeInBytes;
    r->console_bytes = console.SizeInBytes;
    g_textures.live[g_textures.count++] = r;
    g_textures.bytes += desktop.SizeInBytes;
    ReleaseSRWLockExclusive(&g_textures.lock);
}

void console_memory_check() {
    u64 tiles = 0, tile_heaps = 0;
    gpu_tile_usage(&tiles, &tile_heaps);
    AcquireSRWLockExclusive(&g_textures.lock);
    u64 total = g_textures.bytes + tiles;
    bool report = total >= g_textures.next_report;
    if (report)
        g_textures.next_report = ALIGN_DOWN(total, TEXTURE_REPORT_STEP) + TEXTURE_REPORT_STEP;
    ReleaseSRWLockExclusive(&g_textures.lock);
    if (g_texture_out_of_memory) {
        g_texture_out_of_memory = false;
        console_memory_report("out of memory");
    } else if (report) {
        console_memory_report("new high");
    }
}

static void uncount_texture(ConsoleResource* r) {
    if (!r->texture_counted)
        return;
    AcquireSRWLockExclusive(&g_textures.lock);
    ConsoleResource* last = g_textures.live[--g_textures.count];
    g_textures.live[r->texture_index] = last;
    last->texture_index = r->texture_index;
    g_textures.bytes -= r->desktop_bytes;
    ReleaseSRWLockExclusive(&g_textures.lock);
}

struct TextureSpan {
    u64 start;
    u64 end;
    u64 desktop_bytes;
    TextureKind kind;
};

static int compare_spans(const void* a, const void* b) {
    const TextureSpan* x = (const TextureSpan*)a;
    const TextureSpan* y = (const TextureSpan*)b;
    if (x->kind != y->kind)
        return x->kind < y->kind ? -1 : 1;
    return x->start < y->start ? -1 : x->start > y->start ? 1 : 0;
}

// Mapped bytes of [start, end), in 64 KiB steps (titles map texture memory in 64 KiB pages).
static u64 mapped_bytes(u64 start, u64 end) {
    u64 mapped = 0;
    for (u64 at = start; at < end;) {
        u64 next = ALIGN_DOWN(at, KiB(64)) + KiB(64);
        u64 piece_end = next < end ? next : end;
        if (mem_is_mapped((const void*)at, piece_end - at))
            mapped += piece_end - at;
        at = piece_end;
    }
    return mapped;
}

void console_memory_report(const char* why) {
    AcquireSRWLockShared(&g_textures.lock);
    u32 count = g_textures.count;
    u64 total = g_textures.bytes;
    TextureSpan* spans = (TextureSpan*)malloc((count ? count : 1) * sizeof(TextureSpan));
    for (u32 i = 0; spans && i < count; i++) {
        const ConsoleResource* r = g_textures.live[i];
        spans[i] = {r->gpu_address, r->gpu_address + r->console_bytes, r->desktop_bytes, texture_kind(r)};
    }
    ReleaseSRWLockShared(&g_textures.lock);

    GpuVideoMemory video = {};
    gpu_video_memory(&video);
    u64 tiles = 0, tile_heaps = 0;
    gpu_tile_usage(&tiles, &tile_heaps);
    PROCESS_MEMORY_COUNTERS_EX process = {};
    K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&process, sizeof(process));
    PERFORMANCE_INFORMATION system = {};
    K32GetPerformanceInfo(&system, sizeof(system));
    u64 chunk_windows = 0, block_windows = 0, replaced_windows = 0;
    gpu_window_usage(&chunk_windows, &block_windows, &replaced_windows);
    u64 page_table_tiles = 0, downloads = 0;
    gpu_page_tables_usage(&page_table_tiles, &downloads);
    LOG_I("d3d12",
          "memory (%s): whole desktop textures %llu MiB, tiles %llu MiB (heaps %llu MiB); VRAM %llu of %llu "
          "MiB, shared %llu of %llu MiB; title physical memory %llu of %llu MiB used; commit %llu MiB",
          why, total >> 20, tiles >> 20, tile_heaps >> 20, video.local_usage >> 20, video.local_budget >> 20,
          video.shared_usage >> 20, video.shared_budget >> 20,
          (mem_physical_used_pages() << MEM_PAGE_SHIFT) >> 20,
          (mem_physical_total_pages() << MEM_PAGE_SHIFT) >> 20, (u64)process.PrivateUsage >> 20);
    LOG_I(
        "d3d12",
        "memory:   system: commit %llu of %llu MiB, %llu MiB RAM free; this process: private %llu MiB "
        "(VRAM is charged here too), working set %llu MiB; physical memory chunks %llu MiB (%llu MiB bound); "
        "GPU windows over chunks %llu MiB, over other memory %llu MiB (+%llu MiB replaced); page tables %llu "
        "MiB of tiles, %llu pages copied back",
        (u64)system.CommitTotal * system.PageSize >> 20, (u64)system.CommitLimit * system.PageSize >> 20,
        (u64)system.PhysicalAvailable * system.PageSize >> 20, (u64)process.PrivateUsage >> 20,
        (u64)process.WorkingSetSize >> 20, ((u64)mem_physical_chunks_created() * MEM_CHUNK_SIZE) >> 20,
        (mem_physical_bound_pages() << MEM_PAGE_SHIFT) >> 20, chunk_windows >> 20, block_windows >> 20,
        replaced_windows >> 20, page_table_tiles >> 20, downloads);
    if (!spans)
        return;

    // Per kind: what the desktop textures take, the title memory they describe, how much of that is
    // distinct (aliased textures share it) and how much the title has mapped.
    qsort(spans, count, sizeof(TextureSpan), compare_spans);
    for (u32 i = 0; i < count;) {
        TextureKind kind = spans[i].kind;
        u32 textures = 0;
        u64 desktop = 0, console = 0, distinct = 0, mapped = 0, bound = 0;
        u64 run_start = spans[i].start, run_end = spans[i].start;
        for (; i < count && spans[i].kind == kind; i++) {
            textures++;
            desktop += spans[i].desktop_bytes;
            console += spans[i].end - spans[i].start;
            if (spans[i].start > run_end) {
                distinct += run_end - run_start;
                mapped += mapped_bytes(run_start, run_end);
                bound += mem_physical_bound_pages_at((const void*)run_start, run_end - run_start);
                run_start = spans[i].start;
            }
            run_end = spans[i].end > run_end ? spans[i].end : run_end;
        }
        distinct += run_end - run_start;
        mapped += mapped_bytes(run_start, run_end);
        bound += mem_physical_bound_pages_at((const void*)run_start, run_end - run_start);
        LOG_I("d3d12",
              "memory:   %s: %u, %llu MiB as whole desktop textures; %llu MiB of title memory, %llu MiB "
              "distinct, %llu MiB mapped (%llu MiB bound to system memory)",
              k_texture_kind_names[kind], textures, desktop >> 20, console >> 20, distinct >> 20,
              mapped >> 20, (bound << MEM_PAGE_SHIFT) >> 20);
    }
    free(spans);
}
