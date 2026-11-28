#include "gpu/console_layout.h"

#include <d3d12.h>
#include <stdlib.h>
#include <string.h>

#include "addrinterface.h"
#include "common/log.h"
#include "gpu/formats.h"

// addrlib chip identification: the console GPUs are RDNA2 (gfx10.3, with RB+), the same addressing
// as the first desktop RDNA2 chip.
#define ADDR_ENGINE_GFX9_AND_LATER 0x0D
#define ADDR_FAMILY_NAVI           0x8F
#define ADDR_REVISION_NAVI21       0x28

// GB_ADDR_CONFIG of the console GPUs: log2 pipes in bits 0-2, 256-byte pipe interleave (0) in bits
// 3-5, log2 max compressed fragments in bits 6-7, log2 packers in bits 8-10. Pipes and packers
// decide the XOR swizzle modes; they were matched against the console's layouts.
#define GB_ADDR_CONFIG(pipes_log2, packers_log2, frags_log2)                                                 \
    ((pipes_log2) | ((frags_log2) << 6) | ((packers_log2) << 8))

static ADDR_HANDLE g_addrlib = nullptr;

static VOID* ADDR_API addrlib_alloc(const ADDR_ALLOCSYSMEM_INPUT* in) {
    return malloc(in->sizeInBytes);
}

static ADDR_E_RETURNCODE ADDR_API addrlib_free(const ADDR_FREESYSMEM_INPUT* in) {
    free(in->pVirtAddr);
    return ADDR_OK;
}

bool console_layout_init(ConsoleModel model) {
    ADDR_CREATE_INPUT in = {};
    ADDR_CREATE_OUTPUT out = {};
    in.size = sizeof(in);
    out.size = sizeof(out);
    in.chipEngine = ADDR_ENGINE_GFX9_AND_LATER;
    in.chipFamily = ADDR_FAMILY_NAVI;
    in.chipRevision = ADDR_REVISION_NAVI21;
    in.callbacks.allocSysMem = addrlib_alloc;
    in.callbacks.freeSysMem = addrlib_free;
    in.regValue.gbAddrConfig = model == CONSOLE_SERIES_S ? GB_ADDR_CONFIG(3, 2, 1) : GB_ADDR_CONFIG(5, 3, 1);
    if (AddrCreate(&in, &out) != ADDR_OK) {
        LOG_E("gpu", "cannot set up texture layout computation");
        return false;
    }
    g_addrlib = out.hLib;
    return true;
}

// --- descriptions ------------------------------------------------------------------------------------

static AddrResourceType resource_type(u32 dimension) {
    switch (dimension) {
    case D3D12_RESOURCE_DIMENSION_TEXTURE1D:
        return ADDR_RSRC_TEX_1D;
    case D3D12_RESOURCE_DIMENSION_TEXTURE3D:
        return ADDR_RSRC_TEX_3D;
    default:
        return ADDR_RSRC_TEX_2D;
    }
}

// addrlib needs the format only where elements aren't plain texels.
static AddrFormat addr_format(DXGI_FORMAT format, const FormatInfo& info) {
    switch (format) {
    case DXGI_FORMAT_BC1_TYPELESS:
    case DXGI_FORMAT_BC1_UNORM:
    case DXGI_FORMAT_BC1_UNORM_SRGB:
        return ADDR_FMT_BC1;
    case DXGI_FORMAT_BC2_TYPELESS:
    case DXGI_FORMAT_BC2_UNORM:
    case DXGI_FORMAT_BC2_UNORM_SRGB:
        return ADDR_FMT_BC2;
    case DXGI_FORMAT_BC3_TYPELESS:
    case DXGI_FORMAT_BC3_UNORM:
    case DXGI_FORMAT_BC3_UNORM_SRGB:
        return ADDR_FMT_BC3;
    case DXGI_FORMAT_BC4_TYPELESS:
    case DXGI_FORMAT_BC4_UNORM:
    case DXGI_FORMAT_BC4_SNORM:
        return ADDR_FMT_BC4;
    case DXGI_FORMAT_BC5_TYPELESS:
    case DXGI_FORMAT_BC5_UNORM:
    case DXGI_FORMAT_BC5_SNORM:
        return ADDR_FMT_BC5;
    case DXGI_FORMAT_BC6H_TYPELESS:
    case DXGI_FORMAT_BC6H_UF16:
    case DXGI_FORMAT_BC6H_SF16:
        return ADDR_FMT_BC6;
    case DXGI_FORMAT_BC7_TYPELESS:
    case DXGI_FORMAT_BC7_UNORM:
    case DXGI_FORMAT_BC7_UNORM_SRGB:
        return ADDR_FMT_BC7;
    default:
        break;
    }
    switch (info.bytes_per_element) {
    case 1:
        return ADDR_FMT_8;
    case 2:
        return ADDR_FMT_16;
    case 4:
        return ADDR_FMT_32;
    case 8:
        return ADDR_FMT_32_32;
    case 12:
        return ADDR_FMT_32_32_32;
    case 16:
        return ADDR_FMT_32_32_32_32;
    default:
        return ADDR_FMT_INVALID;
    }
}

static u32 mip_count(const ConsoleTextureDesc& d) {
    if (d.mip_levels)
        return d.mip_levels;
    u64 largest = d.width > d.height ? d.width : d.height;
    if (d.dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D && d.depth_or_array_size > largest)
        largest = d.depth_or_array_size;
    u32 levels = 1;
    while (largest > 1) {
        largest >>= 1;
        levels++;
    }
    return levels;
}

static ADDR2_SURFACE_FLAGS surface_flags(const ConsoleTextureDesc& d, const FormatInfo& info, bool stencil) {
    ADDR2_SURFACE_FLAGS f = {};
    bool depth_format = info.kind == FORMAT_KIND_DEPTH || info.kind == FORMAT_KIND_DEPTH_STENCIL;
    f.color = (d.flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) != 0;
    f.depth = !stencil && depth_format && (d.flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0;
    f.stencil = stencil;
    f.texture = !(d.flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE);
    f.unordered = (d.flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0;
    return f;
}

// Console resource flag: the texture may use delta color compression.
#define CONSOLE_RESOURCE_FLAG_ALLOW_DCC 0x400000u

// The console runtime's choice, which differs from addrlib's own preference: small standard
// swizzled pages for textures that are only sampled, 64 KiB render-target swizzles for anything the
// GPU renders to or compresses, depth swizzles for depth, and for UAVs whichever page size fits the
// texture (larger than 64 KiB: 64 KiB pages).
u32 console_suggest_layout(const ConsoleTextureDesc& d) {
    FormatInfo info = format_info(d.format);
    if (info.kind == FORMAT_KIND_UNKNOWN)
        return 0;
    AddrSwizzleMode mode;
    if (d.flags & CONSOLE_RESOURCE_FLAG_ALLOW_DCC) {
        mode = ADDR_SW_64KB_R_X;
    } else if (d.dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D) {
        mode = ADDR_SW_LINEAR;
    } else if (d.flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) {
        mode = ADDR_SW_64KB_Z_X;
    } else if (d.flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) {
        mode = ADDR_SW_64KB_R_X;
    } else if ((d.flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) &&
               d.dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
        u64 width = (d.width + info.block_width - 1) / info.block_width;
        u64 height = (d.height + info.block_height - 1) / info.block_height;
        mode = width * height * info.bytes_per_element > KiB(64) ? ADDR_SW_64KB_R_X : ADDR_SW_4KB_S_X;
    } else {
        mode = ADDR_SW_4KB_S;
    }
    return CONSOLE_LAYOUT_SWIZZLE | mode;
}

u32 console_swizzle_mode(const ConsoleTextureDesc& d) {
    u32 layout = d.layout;
    if (layout == D3D12_TEXTURE_LAYOUT_ROW_MAJOR)
        return ADDR_SW_LINEAR;
    if ((layout & ~0xF000u) < CONSOLE_LAYOUT_SWIZZLE)
        layout = console_suggest_layout(d);
    return layout & 0x1F;
}

// --- layouts -----------------------------------------------------------------------------------------

// Computes one image plane (the color image, or the depth or stencil part of a depth format).
static bool image_plane(const ConsoleTextureDesc& d, const FormatInfo& info, AddrSwizzleMode swizzle,
                        bool stencil, ConsolePlaneUsage usage, ConsolePlaneLayout* p) {
    u32 mips = mip_count(d);
    u32 bpe = stencil ? 1 : info.bytes_per_element;
    ADDR2_COMPUTE_SURFACE_INFO_INPUT in = {};
    ADDR2_COMPUTE_SURFACE_INFO_OUTPUT out = {};
    ADDR2_MIP_INFO mip_info[CONSOLE_MAX_MIPS] = {};
    in.size = sizeof(in);
    out.size = sizeof(out);
    in.flags = surface_flags(d, info, stencil);
    in.swizzleMode = swizzle;
    in.resourceType = resource_type(d.dimension);
    in.format = stencil ? ADDR_FMT_8 : addr_format(d.format, info);
    in.bpp = bpe * 8;
    in.width = (u32)d.width;
    in.height = d.height;
    in.numSlices = d.depth_or_array_size;
    in.numMipLevels = mips;
    in.numSamples = d.sample_count ? d.sample_count : 1;
    in.numFrags = in.numSamples;
    out.pMipInfo = mip_info;
    if (Addr2ComputeSurfaceInfo(g_addrlib, &in, &out) != ADDR_OK)
        return false;

    // Linear surfaces have no mip tail. Volumes (thick swizzles) count a slice as one block deep.
    bool volume = d.dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    u32 first_in_tail = swizzle == ADDR_SW_LINEAR ? mips : out.firstMipIdInTail;
    u32 block_slice_bytes = out.blockWidth * out.blockHeight * bpe;
    p->usage = usage;
    p->size = out.surfSize;
    p->slice_size = volume ? out.sliceSize * out.blockSlices : out.sliceSize;
    p->base_alignment = out.baseAlign;
    p->bytes_per_element = bpe;
    p->standard_mip_count = first_in_tail;
    p->tail_mip_count = mips - first_in_tail;
    for (u32 m = 0; m < mips; m++) {
        const ADDR2_MIP_INFO& mi = mip_info[m];
        ConsoleMipLayout& ml = p->mips[m];
        u32 width_pixels = (u32)(d.width >> m) ? (u32)(d.width >> m) : 1;
        u32 height_pixels = (d.height >> m) ? (d.height >> m) : 1;
        u32 depth = volume ? ((d.depth_or_array_size >> m) ? (d.depth_or_array_size >> m) : 1)
                           : d.depth_or_array_size;
        bool in_tail = m >= first_in_tail;
        ml.slice_2d_size = (u64)mi.pitch * mi.height * bpe * in.numSamples;
        if (!volume) {
            ml.offset = mi.offset;
            ml.padded_depth_or_array_size = out.numSlices;
            ml.size = ml.slice_2d_size;
        } else {
            // Volume mips are padded from the depth halved and rounded up at each level, to whole
            // block slices; mips in the mip tail to half block slices. Mips follow each other block
            // slice by block slice, and a mip in the tail is placed by its tail coordinates.
            u32 rounded_depth = (d.depth_or_array_size + (1u << m) - 1) >> m;
            u32 slices = in_tail ? out.blockSlices / 2 : out.blockSlices;
            ml.padded_depth_or_array_size = (rounded_depth + slices - 1) & ~(slices - 1);
            if (in_tail)
                ml.slice_2d_size = block_slice_bytes;
            ml.offset = in_tail ? 0 : mi.macroBlockOffset;
            ml.size = ml.slice_2d_size * ml.padded_depth_or_array_size;
        }
        ml.pitch_pixels = mi.pitch * info.block_width;
        ml.pitch_bytes = mi.pitch * bpe;
        ml.padded_width = mi.pitch;
        ml.padded_height = mi.height;
        ml.width = (width_pixels + info.block_width - 1) / info.block_width;
        ml.height = (height_pixels + info.block_height - 1) / info.block_height;
        ml.width_pixels = width_pixels;
        ml.height_pixels = height_pixels;
        ml.depth_or_array_size = depth;
        ml.sample_count = in.numSamples;
        ml.swizzle_mode = swizzle;
        ml.slice_depth = volume ? out.blockSlices : 1;
        ml.in_mip_tail = in_tail;
        ml.tail_x = mi.mipTailCoordX;
        ml.tail_y = mi.mipTailCoordY;
        ml.tail_z = mi.mipTailCoordZ;
        ml.alignment = in_tail ? 256 : out.baseAlign;
    }
    return true;
}

// Console resource flags that keep compression metadata away from a texture.
#define CONSOLE_RESOURCE_FLAG_DENY_COLOR_COMPRESSION_DATA 0x100000u
#define CONSOLE_RESOURCE_FLAG_DENY_DEPTH_STENCIL_HTILE    0x80000000u

// A metadata plane: its size and alignment, recorded on the first mip as well, with the swizzle
// mode the metadata itself is laid out in.
static void metadata_plane(ConsolePlaneUsage usage, u64 size, u64 slice_size, u64 alignment, u32 swizzle,
                           ConsolePlaneLayout* p) {
    p->usage = usage;
    p->size = size;
    p->slice_size = slice_size;
    p->base_alignment = alignment;
    p->mips[0].size = size;
    p->mips[0].alignment = (u32)alignment;
    p->mips[0].swizzle_mode = swizzle;
}

// CMask: per-tile fast clear state of a render target. RDNA2 defines it against the fragment mask
// layout (64KB_Z_X), whatever the image's own swizzle.
static bool cmask_plane(const ConsoleTextureDesc& d, const FormatInfo& info, const ConsolePlaneLayout& image,
                        ConsolePlaneLayout* p) {
    ADDR2_COMPUTE_CMASK_INFO_INPUT in = {};
    ADDR2_COMPUTE_CMASK_INFO_OUTPUT out = {};
    in.size = sizeof(in);
    out.size = sizeof(out);
    in.cMaskFlags.pipeAligned = 1;
    in.cMaskFlags.rbAligned = 1;
    in.colorFlags = surface_flags(d, info, false);
    in.resourceType = ADDR_RSRC_TEX_2D;
    in.swizzleMode = ADDR_SW_64KB_Z_X;
    in.unalignedWidth = (u32)d.width;
    in.unalignedHeight = d.height;
    in.numSlices = d.depth_or_array_size;
    in.numMipLevels = mip_count(d);
    in.firstMipIdInTail = image.standard_mip_count;
    if (Addr2ComputeCmaskInfo(g_addrlib, &in, &out) != ADDR_OK)
        return false;
    metadata_plane(CONSOLE_PLANE_COLOR_MASK, out.cmaskBytes, out.sliceSize, out.baseAlign, ADDR_SW_LINEAR, p);
    return true;
}

// FMask: which fragment each sample of an MSAA render target uses.
static bool fmask_plane(const ConsoleTextureDesc& d, ConsolePlaneLayout* p) {
    ADDR2_COMPUTE_FMASK_INFO_INPUT in = {};
    ADDR2_COMPUTE_FMASK_INFO_OUTPUT out = {};
    in.size = sizeof(in);
    out.size = sizeof(out);
    in.swizzleMode = ADDR_SW_64KB_Z_X;
    in.unalignedWidth = (u32)d.width;
    in.unalignedHeight = d.height;
    in.numSlices = d.depth_or_array_size;
    in.numSamples = d.sample_count;
    in.numFrags = d.sample_count;
    if (Addr2ComputeFmaskInfo(g_addrlib, &in, &out) != ADDR_OK)
        return false;
    metadata_plane(CONSOLE_PLANE_FRAGMENT_MASK, out.fmaskBytes, out.sliceSize, out.baseAlign,
                   ADDR_SW_64KB_Z_X, p);
    return true;
}

// DCC: delta color compression keys, laid out in the image's own swizzle.
static bool dcc_plane(const ConsoleTextureDesc& d, const FormatInfo& info, const ConsolePlaneLayout& image,
                      ConsolePlaneLayout* p) {
    ADDR2_COMPUTE_DCCINFO_INPUT in = {};
    ADDR2_COMPUTE_DCCINFO_OUTPUT out = {};
    ADDR2_META_MIP_INFO mip_info[CONSOLE_MAX_MIPS] = {};
    in.size = sizeof(in);
    out.size = sizeof(out);
    in.dccKeyFlags.pipeAligned = 1;
    in.dccKeyFlags.rbAligned = 1;
    in.colorFlags = surface_flags(d, info, false);
    in.resourceType = resource_type(d.dimension);
    in.swizzleMode = (AddrSwizzleMode)image.mips[0].swizzle_mode;
    in.bpp = image.bytes_per_element * 8;
    in.unalignedWidth = (u32)d.width;
    in.unalignedHeight = d.height;
    in.numSlices = d.depth_or_array_size;
    in.numFrags = d.sample_count ? d.sample_count : 1;
    in.numMipLevels = mip_count(d);
    in.dataSurfaceSize = (u32)image.size;
    in.firstMipIdInTail = image.standard_mip_count;
    out.pMipInfo = mip_info;
    if (Addr2ComputeDccInfo(g_addrlib, &in, &out) != ADDR_OK)
        return false;
    // Aligned like the image, one slice's worth per slice.
    u64 alignment = out.dccRamBaseAlign > image.base_alignment ? out.dccRamBaseAlign : image.base_alignment;
    metadata_plane(CONSOLE_PLANE_DCC, out.dccRamSize, out.dccRamSliceSize, alignment, in.swizzleMode, p);
    p->bytes_per_element = 1;
    p->mips[0].size = in.numMipLevels > 1 ? mip_info[0].sliceSize : out.dccRamSliceSize;
    return true;
}

// HTile: depth compression metadata, per 8x8 pixel tile.
static bool htile_plane(const ConsoleTextureDesc& d, const FormatInfo& info, const ConsolePlaneLayout& depth,
                        ConsolePlaneLayout* p) {
    ADDR2_COMPUTE_HTILE_INFO_INPUT in = {};
    ADDR2_COMPUTE_HTILE_INFO_OUTPUT out = {};
    in.size = sizeof(in);
    out.size = sizeof(out);
    in.hTileFlags.pipeAligned = 1;
    in.hTileFlags.rbAligned = 1;
    in.depthFlags = surface_flags(d, info, false);
    in.swizzleMode = (AddrSwizzleMode)depth.mips[0].swizzle_mode;
    in.unalignedWidth = (u32)d.width;
    in.unalignedHeight = d.height;
    in.numSlices = d.depth_or_array_size;
    in.numMipLevels = mip_count(d);
    in.firstMipIdInTail = depth.standard_mip_count;
    if (Addr2ComputeHtileInfo(g_addrlib, &in, &out) != ADDR_OK)
        return false;
    metadata_plane(CONSOLE_PLANE_HTILE, out.htileBytes, 0, out.baseAlign, ADDR_SW_LINEAR, p);
    return true;
}

// Appends a plane after the ones laid out so far, at its alignment.
static void place_plane(ConsoleTextureLayout* out, const ConsolePlaneLayout& plane) {
    ConsolePlaneLayout& p = out->planes[out->plane_count++];
    p = plane;
    p.base_offset = (out->size + plane.base_alignment - 1) & ~(plane.base_alignment - 1);
    out->size = p.base_offset + p.size;
    if (plane.base_alignment > out->base_alignment)
        out->base_alignment = plane.base_alignment;
}

bool console_texture_layout(const ConsoleTextureDesc& d, ConsoleTextureLayout* out) {
    memset(out, 0, sizeof(*out));
    FormatInfo info = format_info(d.format);
    if (!g_addrlib || info.kind == FORMAT_KIND_UNKNOWN || d.width == 0 || d.height == 0)
        return false;
    AddrSwizzleMode swizzle = (AddrSwizzleMode)console_swizzle_mode(d);

    out->mip_levels = mip_count(d);
    out->dimension = d.dimension;
    if (d.flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) {
        // Depth buffers (typeless color formats included): HTile first, then depth, then stencil,
        // each its own surface. Only single-mip depth buffers get HTile, and a depth-only buffer
        // with mips is laid out as a plain texture.
        bool stencil_planes = info.kind == FORMAT_KIND_DEPTH_STENCIL;
        bool single_mip = out->mip_levels == 1;
        ConsolePlaneUsage usage = stencil_planes || single_mip ? CONSOLE_PLANE_DEPTH : CONSOLE_PLANE_DEFAULT;
        ConsolePlaneLayout depth = {}, stencil = {}, htile = {};
        if (!image_plane(d, info, swizzle, false, usage, &depth))
            return false;
        if (single_mip && !(d.flags & CONSOLE_RESOURCE_FLAG_DENY_DEPTH_STENCIL_HTILE)) {
            if (!htile_plane(d, info, depth, &htile))
                return false;
            place_plane(out, htile);
        }
        place_plane(out, depth);
        if (stencil_planes) {
            if (!image_plane(d, info, swizzle, true, CONSOLE_PLANE_STENCIL, &stencil))
                return false;
            place_plane(out, stencil);
        }
        return true;
    }

    ConsolePlaneLayout image = {};
    if (!image_plane(d, info, swizzle, false, CONSOLE_PLANE_DEFAULT, &image))
        return false;
    place_plane(out, image);

    // Render targets get compression metadata unless the title keeps it away: FMask with MSAA,
    // CMask (fast clear) for single-mip targets, except single-sampled ones that use DCC or have
    // 128-bit elements, and DCC where the title allows it.
    bool render_target = (d.flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) != 0;
    if (!render_target || (d.flags & CONSOLE_RESOURCE_FLAG_DENY_COLOR_COMPRESSION_DATA))
        return true;
    bool msaa = d.sample_count > 1;
    bool dcc = (d.flags & CONSOLE_RESOURCE_FLAG_ALLOW_DCC) != 0;
    if (msaa) {
        ConsolePlaneLayout fmask = {};
        if (!fmask_plane(d, &fmask))
            return false;
        place_plane(out, fmask);
    }
    if (out->mip_levels == 1 && (msaa || (!dcc && info.bytes_per_element <= 8))) {
        ConsolePlaneLayout cmask = {};
        if (!cmask_plane(d, info, image, &cmask))
            return false;
        place_plane(out, cmask);
    }
    if (dcc) {
        ConsolePlaneLayout keys = {};
        if (!dcc_plane(d, info, image, &keys))
            return false;
        place_plane(out, keys);
    }
    return true;
}

u64 console_texel_offset(const ConsoleTextureDesc& d, const ConsoleTextureLayout& layout, u32 plane, u32 mip,
                         u32 x, u32 y, u32 z_or_slice, u32 sample, u32* bit_position) {
    if (plane >= layout.plane_count || mip >= layout.mip_levels)
        return ~0ull;
    const ConsolePlaneLayout& p = layout.planes[plane];
    FormatInfo info = format_info(d.format);
    bool stencil = p.usage == CONSOLE_PLANE_STENCIL;
    ADDR2_COMPUTE_SURFACE_ADDRFROMCOORD_INPUT in = {};
    ADDR2_COMPUTE_SURFACE_ADDRFROMCOORD_OUTPUT out = {};
    in.size = sizeof(in);
    out.size = sizeof(out);
    in.x = x;
    in.y = y;
    in.slice = z_or_slice;
    in.sample = sample;
    in.mipId = mip;
    in.swizzleMode = (AddrSwizzleMode)p.mips[0].swizzle_mode;
    in.flags = surface_flags(d, info, stencil);
    in.resourceType = resource_type(d.dimension);
    in.bpp = p.bytes_per_element * 8;
    in.unalignedWidth = (u32)((d.width + info.block_width - 1) / info.block_width);
    in.unalignedHeight = (d.height + info.block_height - 1) / info.block_height;
    in.numSlices = d.depth_or_array_size;
    in.numMipLevels = layout.mip_levels;
    in.numSamples = d.sample_count ? d.sample_count : 1;
    in.numFrags = in.numSamples;
    if (Addr2ComputeSurfaceAddrFromCoord(g_addrlib, &in, &out) != ADDR_OK)
        return ~0ull;
    if (bit_position)
        *bit_position = out.bitPosition;
    return p.base_offset + out.addr;
}

// --- addressing for shaders --------------------------------------------------------------------------

static u32 log2_exact(u32 v) {
    u32 n = 0;
    while ((1u << n) < v)
        n++;
    return (1u << n) == v ? n : 0xFF;
}

static u32 pattern(const u16* bits, u32 count, u32 v) {
    u32 r = 0;
    for (u32 bit = 0; bit < count && (v >> bit); bit++)
        r ^= (v >> bit & 1) ? bits[bit] : 0;
    return r;
}

u64 console_subresource_offset(const ConsoleSubresourceAddressing& a, u32 x, u32 y, u32 z) {
    if (a.linear)
        return a.base + (u64)z * a.slice_pitch + (u64)y * a.pitch + (u64)x * a.bytes_per_element;
    u64 block = (u64)(y >> a.block_shift[1]) * a.row_blocks + (x >> a.block_shift[0]);
    u32 in_block = pattern(a.bits_x, 14, x + a.tail[0]) ^ pattern(a.bits_y, 14, y + a.tail[1]) ^
                   pattern(a.bits_z, 12, z + a.tail[2]) ^ a.block_xor;
    return a.base + (u64)(z >> a.block_shift[2]) * a.slice_stride + (block << a.block_bytes_shift) + in_block;
}

// The block of a swizzled plane, in elements.
static bool swizzle_block(const ConsoleTextureDesc& d, const ConsolePlaneLayout& p, u32* bw, u32* bh,
                          u32* bd) {
    FormatInfo info = format_info(d.format);
    bool stencil = p.usage == CONSOLE_PLANE_STENCIL;
    ADDR2_COMPUTE_SURFACE_INFO_INPUT in = {};
    ADDR2_COMPUTE_SURFACE_INFO_OUTPUT out = {};
    in.size = sizeof(in);
    out.size = sizeof(out);
    in.flags = surface_flags(d, info, stencil);
    in.swizzleMode = (AddrSwizzleMode)p.mips[0].swizzle_mode;
    in.resourceType = resource_type(d.dimension);
    in.format = stencil ? ADDR_FMT_8 : addr_format(d.format, info);
    in.bpp = p.bytes_per_element * 8;
    in.width = (u32)d.width;
    in.height = d.height;
    in.numSlices = d.depth_or_array_size;
    in.numMipLevels = mip_count(d);
    in.numSamples = 1;
    in.numFrags = 1;
    if (Addr2ComputeSurfaceInfo(g_addrlib, &in, &out) != ADDR_OK)
        return false;
    *bw = out.blockWidth;
    *bh = out.blockHeight;
    *bd = d.dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? out.blockSlices : 1;
    return true;
}

// Swizzle patterns depend only on the resource type, the swizzle mode and the element size: they are
// measured once each, on a texture large enough to have every coordinate bit, by placing elements
// with one coordinate bit set and taking away the block's own offset.
struct SwizzlePattern {
    u32 dimension, mode, bytes_per_element, flags;
    bool valid;
    u16 bits_x[14], bits_y[14], bits_z[12];
};

#define MAX_PATTERNS 64
static SwizzlePattern g_patterns[MAX_PATTERNS];
static u32 g_pattern_count;
static SRWLOCK g_pattern_lock = SRWLOCK_INIT;

static bool measure_pattern(const ConsoleTextureDesc& d, const ConsolePlaneLayout& p, u32 plane,
                            SwizzlePattern* out) {
    FormatInfo info = format_info(d.format);
    bool volume = d.dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    u32 bw, bh, bd;
    if (!swizzle_block(d, p, &bw, &bh, &bd))
        return false;
    u32 bytes_shift = log2_exact(bw * bh * bd * p.bytes_per_element);
    // As large as textures get: 16384 elements square with 2048 array slices, volumes 2048 cubed.
    u32 dim_shift = volume ? 11 : 14;
    ConsoleTextureDesc probe = d;
    probe.width = (1ull << dim_shift) * info.block_width;
    probe.height = (1u << dim_shift) * info.block_height;
    probe.depth_or_array_size = 1u << 11;
    probe.mip_levels = 1;
    ConsoleTextureLayout layout;
    if (!console_texture_layout(probe, &layout))
        return false;
    const ConsolePlaneLayout& pp = layout.planes[plane];
    u32 row_blocks = pp.mips[0].padded_width / bw;
    u64 slice_size = pp.slice_size; // a slice (2D) or a block slice (volume) of the whole texture
    u16* bits[3] = {out->bits_x, out->bits_y, out->bits_z};
    u32 counts[3] = {14, 14, 12};
    for (u32 k = 0; k < 3; k++) {
        for (u32 bit = 0; bit < counts[k]; bit++) {
            u32 c[3] = {0, 0, 0};
            c[k] = 1u << bit;
            if (bit >= (k == 2 ? 11u : dim_shift)) { // beyond any texture
                bits[k][bit] = 0;
                continue;
            }
            u64 at = console_texel_offset(probe, layout, plane, 0, c[0], c[1], c[2], 0, nullptr);
            u64 own = (((u64)(c[1] / bh) * row_blocks + c[0] / bw) << bytes_shift) +
                      (volume ? (u64)(c[2] / bd) * slice_size : (u64)c[2] * slice_size);
            if (at == ~0ull || at < own || ((at - own) >> bytes_shift))
                return false;
            bits[k][bit] = (u16)(at - own);
        }
    }
    return true;
}

static const SwizzlePattern* swizzle_pattern(const ConsoleTextureDesc& d, const ConsolePlaneLayout& p,
                                             u32 plane) {
    u32 mode = p.mips[0].swizzle_mode;
    // The flags decide only which modes are allowed, not the patterns; the render target flag is kept
    // for formats that need it to take render target modes.
    u32 flags = d.flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    AcquireSRWLockShared(&g_pattern_lock);
    for (u32 i = 0; i < g_pattern_count; i++) {
        const SwizzlePattern& sp = g_patterns[i];
        if (sp.dimension == d.dimension && sp.mode == mode && sp.bytes_per_element == p.bytes_per_element &&
            sp.flags == flags) {
            ReleaseSRWLockShared(&g_pattern_lock);
            return sp.valid ? &sp : nullptr;
        }
    }
    ReleaseSRWLockShared(&g_pattern_lock);
    SwizzlePattern sp = {d.dimension, mode, p.bytes_per_element, flags};
    sp.valid = measure_pattern(d, p, plane, &sp);
    AcquireSRWLockExclusive(&g_pattern_lock);
    const SwizzlePattern* result = nullptr;
    if (g_pattern_count < MAX_PATTERNS) {
        g_patterns[g_pattern_count] = sp;
        result = sp.valid ? &g_patterns[g_pattern_count] : nullptr;
        g_pattern_count++;
    }
    ReleaseSRWLockExclusive(&g_pattern_lock);
    return result;
}

bool console_subresource_addressing(const ConsoleTextureDesc& d, const ConsoleTextureLayout& layout,
                                    u32 plane, u32 mip, u32 slice, ConsoleSubresourceAddressing* out,
                                    bool check) {
    memset(out, 0, sizeof(*out));
    if (plane >= layout.plane_count || mip >= layout.mip_levels || d.sample_count > 1)
        return false;
    const ConsolePlaneLayout& p = layout.planes[plane];
    const ConsoleMipLayout& m = p.mips[mip];
    bool volume = d.dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    if (!p.bytes_per_element || log2_exact(p.bytes_per_element) > 4 ||
        (!volume && slice >= d.depth_or_array_size))
        return false;
    out->bytes_per_element = p.bytes_per_element;
    out->width = m.width;
    out->height = m.height;
    out->depth = volume ? m.depth_or_array_size : 1;
    u64 origin = console_texel_offset(d, layout, plane, mip, 0, 0, volume ? 0 : slice, 0, nullptr);
    if (origin == ~0ull)
        return false;
    if (m.swizzle_mode == ADDR_SW_LINEAR) {
        out->linear = true;
        out->base = origin;
        out->pitch = m.pitch_bytes;
        if (volume && out->depth > 1)
            out->slice_pitch =
                (u32)(console_texel_offset(d, layout, plane, mip, 0, 0, 1, 0, nullptr) - origin);
    } else {
        u32 bw, bh, bd;
        const SwizzlePattern* sp = swizzle_pattern(d, p, plane);
        if (!sp || !swizzle_block(d, p, &bw, &bh, &bd))
            return false;
        out->block_shift[0] = (u8)log2_exact(bw);
        out->block_shift[1] = (u8)log2_exact(bh);
        out->block_shift[2] = (u8)log2_exact(bd);
        out->block_bytes_shift = (u8)log2_exact(bw * bh * bd * p.bytes_per_element);
        out->row_blocks = m.padded_width / bw;
        out->slice_stride = volume ? p.slice_size : 0;
        memcpy(out->bits_x, sp->bits_x, sizeof(out->bits_x));
        memcpy(out->bits_y, sp->bits_y, sizeof(out->bits_y));
        if (volume)
            memcpy(out->bits_z, sp->bits_z, sizeof(out->bits_z));
        if (m.in_mip_tail) {
            out->tail[0] = m.tail_x;
            out->tail[1] = m.tail_y;
            out->tail[2] = volume ? m.tail_z : 0;
        }
        if (!volume) // an array slice's bits, and the mip's depth in the tail, are XORed in once
            out->block_xor = pattern(sp->bits_z, 12, slice + (m.in_mip_tail ? m.tail_z : 0));
        // The origin is element (0, 0, 0): the base is what is left without its pattern.
        u32 origin_pattern = pattern(out->bits_x, 14, out->tail[0]) ^ pattern(out->bits_y, 14, out->tail[1]) ^
                             pattern(out->bits_z, 12, out->tail[2]) ^ out->block_xor;
        if (origin_pattern > origin)
            return false;
        out->base = origin - origin_pattern;
    }

    if (!check)
        return true;
    // Corners and a spread of elements must come out where the layout puts them.
    u32 w = out->width, h = out->height, dd = out->depth;
    for (u32 k = 0; k < 16; k++) {
        u32 x = k < 4 ? (k & 1) * (w - 1) : (u32)((k * 2654435761u) % w);
        u32 y = k < 4 ? (k >> 1) * (h - 1) : (u32)((k * 40503u + 7) % h);
        u32 z = (u32)((k * 97u) % dd);
        u64 expect = console_texel_offset(d, layout, plane, mip, x, y, volume ? z : slice, 0, nullptr);
        u64 got = console_subresource_offset(*out, x, y, z);
        if (expect != got) {
            LOG_W_ONCE("gpu",
                       "texture addressing of swizzle mode %u, %u-byte elements, mip %u, differs from its "
                       "layout at (%u, %u, %u): 0x%llx, not 0x%llx; such textures get no contents",
                       m.swizzle_mode, p.bytes_per_element, mip, x, y, z, got, expect);
            return false;
        }
    }
    return true;
}

bool console_copy_subresource(const ConsoleTextureDesc& d, const ConsoleTextureLayout& layout, u32 plane,
                              u32 mip, u32 slice, void* texture, void* linear, u64 row_pitch, u64 slice_pitch,
                              bool into_texture) {
    if (plane >= layout.plane_count || mip >= layout.mip_levels)
        return false;
    const ConsolePlaneLayout& p = layout.planes[plane];
    if (p.bytes_per_element == 0)
        return false; // metadata planes have no texels
    const ConsoleMipLayout& m = p.mips[mip];
    FormatInfo info = format_info(d.format);
    bool stencil = p.usage == CONSOLE_PLANE_STENCIL;
    bool volume = d.dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    if (!volume && slice >= d.depth_or_array_size)
        return false;

    ADDR2_COPY_MEMSURFACE_INPUT in = {};
    in.size = sizeof(in);
    in.swizzleMode = (AddrSwizzleMode)m.swizzle_mode;
    in.format = stencil ? ADDR_FMT_8 : addr_format(d.format, info);
    in.flags = surface_flags(d, info, stencil);
    in.resourceType = resource_type(d.dimension);
    in.bpp = p.bytes_per_element * 8;
    in.unAlignedDims.width = (u32)d.width;
    in.unAlignedDims.height = d.height;
    in.unAlignedDims.depth = d.depth_or_array_size;
    in.numMipLevels = layout.mip_levels;
    in.numSamples = d.sample_count ? d.sample_count : 1;
    in.pMappedSurface = (u8*)texture + p.base_offset;

    ADDR2_COPY_MEMSURFACE_REGION region = {};
    region.size = sizeof(region);
    region.slice = volume ? 0 : slice;
    region.mipId = mip;
    region.copyDims.width = m.width;
    region.copyDims.height = m.height;
    region.copyDims.depth = volume ? m.depth_or_array_size : 1;
    region.pMem = linear;
    region.memRowPitch = row_pitch;
    region.memSlicePitch = slice_pitch;
    ADDR_E_RETURNCODE r = into_texture ? Addr2CopyMemToSurface(g_addrlib, &in, &region, 1)
                                       : Addr2CopySurfaceToMem(g_addrlib, &in, &region, 1);
    return r == ADDR_OK;
}
