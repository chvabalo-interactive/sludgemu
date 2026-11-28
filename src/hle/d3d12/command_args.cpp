// Console-only command list arguments in desktop terms (see command_args.h).

#include "hle/d3d12/command_args.h"

#include "gpu/formats.h"

u32 console_copy_flags_unsupported(u32 flags) {
    return flags & ~CONSOLE_COPY_FLAGS_METADATA;
}

u16 console_quad_sample_mask(u64 quad_mask, bool* uniform) {
    u16 pixels[4];
    for (u32 i = 0; i < 4; i++)
        pixels[i] = (u16)(quad_mask >> (16 * i));
    *uniform = pixels[0] == pixels[1] && pixels[0] == pixels[2] && pixels[0] == pixels[3];
    return (u16)(pixels[0] | pixels[1] | pixels[2] | pixels[3]);
}

bool console_sample_mask_override(u16 mask, u32 pipeline_mask, u32 samples, u16* desktop_mask) {
    u32 present = samples >= 16 ? 0xFFFFu : (1u << (samples ? samples : 1)) - 1;
    if ((mask & present) == (pipeline_mask & present))
        return false;
    *desktop_mask = (u16)(mask & present);
    return true;
}

D3D12_SHADING_RATE console_shading_rate(u32 rate, bool additional_rates) {
    switch (rate) {
    case D3D12_SHADING_RATE_1X1:
    case D3D12_SHADING_RATE_1X2:
    case D3D12_SHADING_RATE_2X1:
    case D3D12_SHADING_RATE_2X2:
        return (D3D12_SHADING_RATE)rate;
    case D3D12_SHADING_RATE_2X4:
    case D3D12_SHADING_RATE_4X2:
    case D3D12_SHADING_RATE_4X4:
        return additional_rates ? (D3D12_SHADING_RATE)rate : D3D12_SHADING_RATE_2X2;
    default:
        return D3D12_SHADING_RATE_1X1;
    }
}

D3D12_SHADING_RATE_COMBINER console_shading_rate_combiner(u32 combiner) {
    return combiner <= D3D12_SHADING_RATE_COMBINER_SUM ? (D3D12_SHADING_RATE_COMBINER)combiner
                                                       : D3D12_SHADING_RATE_COMBINER_PASSTHROUGH;
}

// The settings word, low bit first: a reserved bit, then 3-bit combiners for the vertex rate, the
// primitive rate, the depth metadata (image) rate and supersampling iterations, two flags (coarse
// pixel input mask, forcing a zero rate hint into color metadata), then the state rate per axis as a
// 2-bit two's complement log2 (0: 1x, 1: 2x, -1 and -2: 2x and 4x supersampling).
ConsoleShadingRateSettings console_shading_rate_settings(u32 bits, bool additional_rates) {
    ConsoleShadingRateSettings s;
    u32 vertex = (bits >> 1) & 7, primitive = (bits >> 4) & 7, image = (bits >> 7) & 7;
    // One desktop combiner merges per-vertex and per-primitive rates (the shader's own rate);
    // titles enable one of the two.
    s.combiners[0] = console_shading_rate_combiner(
        primitive != D3D12_SHADING_RATE_COMBINER_PASSTHROUGH ? primitive : vertex);
    s.combiners[1] = console_shading_rate_combiner(image);
    u32 x = (bits >> 15) & 3, y = (bits >> 17) & 3;
    s.supersampling = x >= 2 || y >= 2;
    if (x >= 2)
        x = 0;
    if (y >= 2)
        y = 0;
    s.base = console_shading_rate((x << 2) | y, additional_rates);
    return s;
}

static s8 clamp_offset(s8 v) {
    return v < -8 ? (s8)-8 : v > 7 ? (s8)7 : v;
}

u32 console_sample_positions(const ConsoleSampleLocations& in, u32 samples, D3D12_SAMPLE_POSITION* out) {
    if (samples > 16)
        samples = 16;
    bool same = true;
    for (u32 p = 1; p < 4 && same; p++)
        for (u32 s = 0; s < samples && same; s++)
            same = in.pixels[p][s][0] == in.pixels[0][s][0] && in.pixels[p][s][1] == in.pixels[0][s][1];
    // Desktop orders the pixels of a quad row by row, as the console does.
    u32 pixels = same ? 1 : 4;
    for (u32 p = 0; p < pixels; p++) {
        for (u32 s = 0; s < samples; s++) {
            out[p * samples + s].X = clamp_offset(in.pixels[p][s][0]);
            out[p * samples + s].Y = clamp_offset(in.pixels[p][s][1]);
        }
    }
    return pixels;
}

// Depth-stencil formats copy as two planes: depth (32 bits per texel, whatever the depth format), then
// stencil.
static bool depth_stencil_planes(DXGI_FORMAT f, DXGI_FORMAT* depth) {
    switch (f) {
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    case DXGI_FORMAT_R32G8X24_TYPELESS:
        *depth = DXGI_FORMAT_R32_TYPELESS;
        return true;
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_R24G8_TYPELESS:
        *depth = DXGI_FORMAT_R32_TYPELESS;
        return true;
    default:
        return false;
    }
}

static u32 full_mip_count(const D3D12_RESOURCE_DESC1& d) {
    u64 size = d.Width > d.Height ? d.Width : d.Height;
    if (d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D && d.DepthOrArraySize > size)
        size = d.DepthOrArraySize;
    u32 mips = 1;
    while (size > 1) {
        size >>= 1;
        mips++;
    }
    return mips;
}

bool console_copyable_footprints(const D3D12_RESOURCE_DESC1& d, u32 first, u32 count, u64 base,
                                 D3D12_PLACED_SUBRESOURCE_FOOTPRINT* layouts, u32* rows, u64* row_sizes,
                                 u64* total) {
    if (total)
        *total = UINT64_MAX;
    if (d.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
        if (first != 0 || count != 1)
            return false;
        if (layouts)
            layouts[0] = {ALIGN_UP(base, (u64)D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT),
                          {DXGI_FORMAT_UNKNOWN, (UINT)d.Width, 1, 1,
                           (UINT)ALIGN_UP(d.Width, (u64)D3D12_TEXTURE_DATA_PITCH_ALIGNMENT)}};
        if (rows)
            rows[0] = 1;
        if (row_sizes)
            row_sizes[0] = d.Width;
        if (total)
            *total = d.Width;
        return true;
    }
    FormatInfo f = format_info(d.Format);
    if (f.kind == FORMAT_KIND_UNKNOWN || !f.bytes_per_element)
        return false;
    DXGI_FORMAT depth_format = DXGI_FORMAT_UNKNOWN;
    u32 planes = depth_stencil_planes(d.Format, &depth_format) ? 2 : 1;
    u32 mips = d.MipLevels ? d.MipLevels : full_mip_count(d);
    u32 slices = d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1 : d.DepthOrArraySize;
    if ((u64)first + count > (u64)mips * slices * planes)
        return false;
    u64 offset = 0; // from base, before alignment
    u64 end = 0;
    for (u32 i = 0; i < count; i++) {
        u32 s = first + i;
        u32 mip = s % mips, plane = s / (mips * slices);
        DXGI_FORMAT format = d.Format;
        u32 bpe = f.bytes_per_element, bw = f.block_width ? f.block_width : 1,
            bh = f.block_height ? f.block_height : 1;
        if (planes == 2) {
            format = plane == 0 ? depth_format : DXGI_FORMAT_R8_TYPELESS;
            bpe = plane == 0 ? 4 : 1;
            bw = bh = 1;
        }
        u64 w = d.Width >> mip ? d.Width >> mip : 1;
        u32 h = d.Height >> mip ? d.Height >> mip : 1;
        u32 depth = 1;
        if (d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D)
            depth = d.DepthOrArraySize >> mip ? d.DepthOrArraySize >> mip : 1;
        u64 row_size = (w + bw - 1) / bw * bpe;
        u32 row_count = (h + bh - 1) / bh;
        // Rows of planar formats align to the pitch alignment once per plane, as the D3D12 runtime lays
        // them out (test_copyable_footprints_match_desktop compares with it).
        u64 pitch = ALIGN_UP(row_size, (u64)D3D12_TEXTURE_DATA_PITCH_ALIGNMENT * planes);
        offset = ALIGN_UP(base + offset, (u64)D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT) - base;
        if (layouts)
            layouts[i] = {
                base + offset,
                {format, (UINT)ALIGN_UP(w, (u64)bw), (UINT)ALIGN_UP((u64)h, (u64)bh), depth, (UINT)pitch}};
        if (rows)
            rows[i] = row_count;
        if (row_sizes)
            row_sizes[i] = row_size;
        // The last row of the last slice takes only its own bytes.
        end = offset + pitch * ((u64)row_count * depth - 1) + row_size;
        offset += pitch * row_count * depth;
    }
    if (total)
        *total = end;
    return true;
}
