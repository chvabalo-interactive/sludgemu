// Console-only command list arguments in desktop terms (see command_args.h).

#include "hle/d3d12/command_args.h"

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
