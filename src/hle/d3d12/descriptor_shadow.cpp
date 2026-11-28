// Descriptor shadow records (see descriptor_shadow.h).

#include "hle/d3d12/descriptor_shadow.h"

#include <string.h>

#include "common/log.h"
#include "gpu/console_layout.h"
#include "gpu/formats.h"

void shadow_buffer_descriptor(u64 base, u32 stride, u32 records, u32 out[4]) {
    out[0] = (u32)base;
    out[1] = ((u32)(base >> 32) & 0xFFFFu) | (stride & 0x3FFFu) << 16;
    out[2] = records;
    out[3] = (u32)NATIVE_CBV_FORMAT_BITS;
}

// --- image descriptors -------------------------------------------------------------------------------
//
// RDNA 2 image descriptor layout, by dword:
//   0      base address bits 8-39 (textures are 256-byte aligned)
//   1      base address bits 40-47 (0-7), minimum LOD in 4.8 fixed point (8-19), image format (20-28),
//          width - 1 bits 0-1 (30-31)
//   2      width - 1 bits 2-15 (0-13), height - 1 (14-29), resource level, set (31)
//   3      component selects x/y/z/w (3 bits each, 0-11), base level (12-15), last level (16-19),
//          swizzle mode (20-24), border color swizzle (25-27), type (28-31)
//   4      depth - 1 for 3D, else the last array slice (0-15), first array slice (16-31)
//   5      array pitch (0-3: 1 for a 3D view of some of its slices), mip count - 1 (4-7), ...
//   6, 7   counters and compression metadata
// Multisampled images keep log2 of the sample count where others have mip levels.

enum : u32 {
    IMAGE_TYPE_1D = 8,
    IMAGE_TYPE_2D = 9,
    IMAGE_TYPE_3D = 10,
    IMAGE_TYPE_CUBE = 11,
    IMAGE_TYPE_1D_ARRAY = 12,
    IMAGE_TYPE_2D_ARRAY = 13,
    IMAGE_TYPE_2D_MSAA = 14,
    IMAGE_TYPE_2D_MSAA_ARRAY = 15,
};

// Border color swizzles: where the border color's alpha and colors go, given the component selects.
enum : u32 {
    BORDER_XYZW = 0,
    BORDER_XWYZ = 1,
    BORDER_WZYX = 2,
    BORDER_WXYZ = 3,
    BORDER_ZYXW = 4,
    BORDER_YXWZ = 5,
};

// What a view picks out of its texture.
struct ImageView {
    DXGI_FORMAT format; // UNKNOWN: the texture's
    u32 type;
    u32 first_mip;
    u32 mips;         // UINT32_MAX: to the last
    u32 first_slice;  // array slices (cube faces); 3D: slices of a range
    u32 slices;       // UINT32_MAX: to the last
    bool slice_range; // a 3D view of some of its slices (unordered access views)
    f32 min_lod;
    u32 mapping; // D3D12 component mapping
};

static u32 log2_floor(u64 v) {
    u32 n = 0;
    while (v > 1) {
        v >>= 1;
        n++;
    }
    return n;
}

// How many mips or slices of `total` a view sees from `first`: UINT32_MAX runs to the last one, and a
// view sees one at least (also where `first` is past the end, which only invalid views ask for).
static u32 viewed_count(u32 count, u32 first, u32 total) {
    if (count == UINT32_MAX)
        count = first < total ? total - first : 1;
    return count ? count : 1;
}

// Mip levels of a texture: the description's, or the full chain.
static u32 texture_mips(const D3D12_RESOURCE_DESC1& r) {
    if (r.MipLevels)
        return r.MipLevels;
    u64 largest = r.Width > r.Height ? r.Width : r.Height;
    if (r.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D && r.DepthOrArraySize > largest)
        largest = r.DepthOrArraySize;
    return log2_floor(largest) + 1;
}

// The border color swizzle that goes with the selects: what matters is where alpha ends up (the
// predefined border colors have equal color channels).
static u32 border_swizzle(const u8 select[4]) {
    if (select[3] == IMAGE_SELECT_X)
        return select[2] == IMAGE_SELECT_Y ? BORDER_WZYX : BORDER_WXYZ;
    if (select[0] == IMAGE_SELECT_X)
        return select[1] == IMAGE_SELECT_Y ? BORDER_XYZW : BORDER_XWYZ;
    if (select[1] == IMAGE_SELECT_X)
        return BORDER_YXWZ;
    if (select[2] == IMAGE_SELECT_X)
        return BORDER_ZYXW;
    return BORDER_XYZW;
}

static u32 fixed_4_8(f32 lod) {
    if (!(lod > 0.0f)) // NaN too
        return 0;
    return lod >= 15.99609375f ? 0xFFFu : (u32)(lod * 256.0f);
}

static void image_descriptor(const D3D12_RESOURCE_DESC1& r, u64 address, const ImageView& v, u32 image_format,
                             u32 out[8]) {
    memset(out, 0, 8 * sizeof(u32));
    DXGI_FORMAT format = v.format != DXGI_FORMAT_UNKNOWN ? v.format : r.Format;
    ImageFormat f = format_image(format);
    if (image_format)
        f.format = image_format;
    else if (!f.format)
        LOG_W_ONCE("d3d12",
                   "no console image format for format %d: image descriptors of its views read as "
                   "null views where programs look at their bits",
                   format);

    // The view's channels, each taking the format channel the view's mapping picks (or a constant).
    u8 select[4];
    for (u32 c = 0; c < 4; c++) {
        u32 from = D3D12_DECODE_SHADER_4_COMPONENT_MAPPING(c, v.mapping);
        select[c] = from < 4 ? f.select[from]
                             : (u8)(from == D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1 ? IMAGE_SELECT_ONE
                                                                                         : IMAGE_SELECT_ZERO);
    }

    u32 texture_levels = texture_mips(r);
    u32 samples = r.SampleDesc.Count ? r.SampleDesc.Count : 1;
    bool multisampled = v.type == IMAGE_TYPE_2D_MSAA || v.type == IMAGE_TYPE_2D_MSAA_ARRAY;
    u32 base_level = 0, last_level = 0, max_mip = 0;
    if (multisampled) {
        last_level = max_mip = log2_floor(samples);
    } else {
        base_level = v.first_mip;
        last_level = v.first_mip + viewed_count(v.mips, v.first_mip, texture_levels) - 1;
        max_mip = texture_levels - 1;
    }

    bool volume = r.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    // A volume's mips halve its depth too, down to one slice (depths have 16 bits).
    u32 layers = r.DepthOrArraySize;
    if (volume)
        layers = v.first_mip < 16 && layers >> v.first_mip ? layers >> v.first_mip : 1;
    u32 last_slice = v.first_slice + viewed_count(v.slices, v.first_slice, layers) - 1;
    // 3D views of the whole volume: its depth - 1; others: the slices viewed, the last one in the depth.
    bool whole_volume = v.type == IMAGE_TYPE_3D && !v.slice_range;
    u32 depth = whole_volume ? r.DepthOrArraySize - 1u : last_slice;
    u32 first_slice = whole_volume ? 0 : v.first_slice;

    ConsoleTextureDesc layout = {};
    layout.dimension = r.Dimension;
    layout.width = r.Width;
    layout.height = r.Height;
    layout.depth_or_array_size = r.DepthOrArraySize;
    layout.mip_levels = r.MipLevels;
    layout.format = r.Format;
    layout.sample_count = samples;
    layout.layout = r.Layout;
    layout.flags = r.Flags;

    u32 width = (u32)r.Width - 1;
    u32 height = r.Height ? r.Height - 1 : 0;
    out[0] = (u32)(address >> 8);
    out[1] = (u32)(address >> 40) & 0xFFu | fixed_4_8(v.min_lod) << 8 | (f.format & 0x1FFu) << 20 |
             (width & 3u) << 30;
    out[2] = (width >> 2) & 0x3FFFu | (height & 0xFFFFu) << 14 | 1u << 31;
    out[3] = (u32)select[0] | (u32)select[1] << 3 | (u32)select[2] << 6 | (u32)select[3] << 9 |
             (base_level & 15u) << 12 | (last_level & 15u) << 16 |
             (console_swizzle_mode(layout) & 0x1Fu) << 20 | border_swizzle(select) << 25 | v.type << 28;
    out[4] = (depth & 0xFFFFu) | (first_slice & 0xFFFFu) << 16;
    out[5] = (v.slice_range ? 1u : 0u) | (max_mip & 15u) << 4;
}

void shadow_texture_srv(const D3D12_RESOURCE_DESC1& texture, u64 address,
                        const D3D12_SHADER_RESOURCE_VIEW_DESC& d, u32 image_format, u32 out[8]) {
    ImageView v = {d.Format, 0, 0, UINT32_MAX, 0, 1, false, 0.0f, d.Shader4ComponentMapping};
    switch (d.ViewDimension) {
    case D3D12_SRV_DIMENSION_TEXTURE1D:
        v.type = IMAGE_TYPE_1D;
        v.first_mip = d.Texture1D.MostDetailedMip;
        v.mips = d.Texture1D.MipLevels;
        v.min_lod = d.Texture1D.ResourceMinLODClamp;
        break;
    case D3D12_SRV_DIMENSION_TEXTURE1DARRAY:
        v.type = IMAGE_TYPE_1D_ARRAY;
        v.first_mip = d.Texture1DArray.MostDetailedMip;
        v.mips = d.Texture1DArray.MipLevels;
        v.first_slice = d.Texture1DArray.FirstArraySlice;
        v.slices = d.Texture1DArray.ArraySize;
        v.min_lod = d.Texture1DArray.ResourceMinLODClamp;
        break;
    case D3D12_SRV_DIMENSION_TEXTURE2D:
        v.type = IMAGE_TYPE_2D;
        v.first_mip = d.Texture2D.MostDetailedMip;
        v.mips = d.Texture2D.MipLevels;
        v.min_lod = d.Texture2D.ResourceMinLODClamp;
        break;
    case D3D12_SRV_DIMENSION_TEXTURE2DARRAY:
        v.type = IMAGE_TYPE_2D_ARRAY;
        v.first_mip = d.Texture2DArray.MostDetailedMip;
        v.mips = d.Texture2DArray.MipLevels;
        v.first_slice = d.Texture2DArray.FirstArraySlice;
        v.slices = d.Texture2DArray.ArraySize;
        v.min_lod = d.Texture2DArray.ResourceMinLODClamp;
        break;
    case D3D12_SRV_DIMENSION_TEXTURE2DMS:
        v.type = IMAGE_TYPE_2D_MSAA;
        break;
    case D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY:
        v.type = IMAGE_TYPE_2D_MSAA_ARRAY;
        v.first_slice = d.Texture2DMSArray.FirstArraySlice;
        v.slices = d.Texture2DMSArray.ArraySize;
        break;
    case D3D12_SRV_DIMENSION_TEXTURE3D:
        v.type = IMAGE_TYPE_3D;
        v.first_mip = d.Texture3D.MostDetailedMip;
        v.mips = d.Texture3D.MipLevels;
        v.min_lod = d.Texture3D.ResourceMinLODClamp;
        break;
    case D3D12_SRV_DIMENSION_TEXTURECUBE:
        v.type = IMAGE_TYPE_CUBE;
        v.first_mip = d.TextureCube.MostDetailedMip;
        v.mips = d.TextureCube.MipLevels;
        v.slices = 6;
        v.min_lod = d.TextureCube.ResourceMinLODClamp;
        break;
    case D3D12_SRV_DIMENSION_TEXTURECUBEARRAY:
        v.type = IMAGE_TYPE_CUBE;
        v.first_mip = d.TextureCubeArray.MostDetailedMip;
        v.mips = d.TextureCubeArray.MipLevels;
        v.first_slice = d.TextureCubeArray.First2DArrayFace;
        v.slices = 6 * d.TextureCubeArray.NumCubes;
        v.min_lod = d.TextureCubeArray.ResourceMinLODClamp;
        break;
    default:
        LOG_W_ONCE("d3d12", "no image descriptor for shader resource views of dimension %d", d.ViewDimension);
        memset(out, 0, 8 * sizeof(u32));
        return;
    }
    image_descriptor(texture, address, v, image_format, out);
}

void shadow_texture_uav(const D3D12_RESOURCE_DESC1& texture, u64 address,
                        const D3D12_UNORDERED_ACCESS_VIEW_DESC& d, u32 out[8]) {
    // Unordered access views see one mip, with the texture's own components.
    ImageView v = {d.Format, 0, 0, 1, 0, 1, false, 0.0f, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING};
    switch (d.ViewDimension) {
    case D3D12_UAV_DIMENSION_TEXTURE1D:
        v.type = IMAGE_TYPE_1D;
        v.first_mip = d.Texture1D.MipSlice;
        break;
    case D3D12_UAV_DIMENSION_TEXTURE1DARRAY:
        v.type = IMAGE_TYPE_1D_ARRAY;
        v.first_mip = d.Texture1DArray.MipSlice;
        v.first_slice = d.Texture1DArray.FirstArraySlice;
        v.slices = d.Texture1DArray.ArraySize;
        break;
    case D3D12_UAV_DIMENSION_TEXTURE2D:
        v.type = IMAGE_TYPE_2D;
        v.first_mip = d.Texture2D.MipSlice;
        break;
    case D3D12_UAV_DIMENSION_TEXTURE2DARRAY:
        v.type = IMAGE_TYPE_2D_ARRAY;
        v.first_mip = d.Texture2DArray.MipSlice;
        v.first_slice = d.Texture2DArray.FirstArraySlice;
        v.slices = d.Texture2DArray.ArraySize;
        break;
    case D3D12_UAV_DIMENSION_TEXTURE2DMS:
        v.type = IMAGE_TYPE_2D_MSAA;
        break;
    case D3D12_UAV_DIMENSION_TEXTURE2DMSARRAY:
        v.type = IMAGE_TYPE_2D_MSAA_ARRAY;
        v.first_slice = d.Texture2DMSArray.FirstArraySlice;
        v.slices = d.Texture2DMSArray.ArraySize;
        break;
    case D3D12_UAV_DIMENSION_TEXTURE3D:
        // The slices of the mip it names, whose depth programs take from the slice range.
        v.type = IMAGE_TYPE_3D;
        v.first_mip = d.Texture3D.MipSlice;
        v.first_slice = d.Texture3D.FirstWSlice;
        v.slices = d.Texture3D.WSize;
        v.slice_range = true;
        break;
    default:
        LOG_W_ONCE("d3d12", "no image descriptor for unordered access views of dimension %d",
                   d.ViewDimension);
        memset(out, 0, 8 * sizeof(u32));
        return;
    }
    image_descriptor(texture, address, v, 0, out);
}

// --- records -----------------------------------------------------------------------------------------

void shadow_record(const u8* slot, const u32* image_words, u32 record[SHADER_SHADOW_RECORD_BYTES / 4]) {
    memset(record, 0, SHADER_SHADOW_RECORD_BYTES);
    u64 q0, q1;
    memcpy(&q0, slot, 8);
    memcpy(&q1, slot + 8, 8);
    if ((q0 >> 48) == NATIVE_CBV_STRIDE_BITS && (q1 >> 32) == NATIVE_CBV_FORMAT_BITS) {
        memcpy(record, slot, 16);
        record[SHADER_SHADOW_KIND_OFFSET / 4] = SHADOW_VIEW_CBV;
    } else if ((u32)q0 == DESCRIPTOR_MARKER_MAGIC) {
        u32 type_word = (u32)(q0 >> 32);
        memcpy(record, slot + DESCRIPTOR_MARKER_BITS, 16);
        if ((type_word & DESCRIPTOR_MARKER_IMAGE) && image_words)
            memcpy(record + 4, image_words, 16);
        record[SHADER_SHADOW_KIND_OFFSET / 4] = (type_word >> 8) & 0xFF;
    }
}
