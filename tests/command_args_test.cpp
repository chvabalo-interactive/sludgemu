// Console-only command list arguments in desktop terms (hle/d3d12/command_args.h).

#include <string.h>

#include "hle/d3d12/command_args.h"
#include "test.h"

static void test_copy_flags() {
    CHECK(console_copy_flags_unsupported(0) == 0);
    // Compression metadata management and engine choice: a plain desktop copy.
    CHECK(console_copy_flags_unsupported(0x10) == 0);   // resummarize depth metadata
    CHECK(console_copy_flags_unsupported(0x60) == 0);   // decompress depth and stencil
    CHECK(console_copy_flags_unsupported(0x1000) == 0); // eliminate color compression
    CHECK(console_copy_flags_unsupported(0x800) == 0);  // copy engine choice
    CHECK(console_copy_flags_unsupported(0x1C70) == 0);
    // Depth or stencil samples into color channels: no desktop copy does that.
    CHECK(console_copy_flags_unsupported(0x80) == 0x80);
    CHECK(console_copy_flags_unsupported(0x180) == 0x180);
    CHECK(console_copy_flags_unsupported(0x200 | 0x10) == 0x200);
    // Bits nobody defines aren't taken for harmless.
    CHECK(console_copy_flags_unsupported(0x10000) == 0x10000);
}

static void test_quad_sample_mask() {
    bool uniform = false;
    CHECK(console_quad_sample_mask(~0ull, &uniform) == 0xFFFF && uniform);
    CHECK(console_quad_sample_mask(0x0003000300030003ull, &uniform) == 0x0003 && uniform);
    CHECK(console_quad_sample_mask(0, &uniform) == 0 && uniform);
    // Pixels asking for different samples: every sample one of them keeps.
    CHECK(console_quad_sample_mask(0x0001000200020001ull, &uniform) == 0x0003 && !uniform);
    CHECK(console_quad_sample_mask(0x8000000000000000ull, &uniform) == 0x8000 && !uniform);
}

static void test_shading_rates() {
    CHECK(console_shading_rate(0x0, false) == D3D12_SHADING_RATE_1X1);
    CHECK(console_shading_rate(0x1, false) == D3D12_SHADING_RATE_1X2);
    CHECK(console_shading_rate(0x4, false) == D3D12_SHADING_RATE_2X1);
    CHECK(console_shading_rate(0x5, false) == D3D12_SHADING_RATE_2X2);
    // The additional rates only where the GPU has them, else the coarsest common one.
    CHECK(console_shading_rate(0x6, false) == D3D12_SHADING_RATE_2X2);
    CHECK(console_shading_rate(0xA, false) == D3D12_SHADING_RATE_2X2);
    CHECK(console_shading_rate(0xA, true) == D3D12_SHADING_RATE_4X4);
    CHECK(console_shading_rate(0x9, true) == D3D12_SHADING_RATE_4X2);
    CHECK(console_shading_rate(0x3, true) == D3D12_SHADING_RATE_1X1); // not a rate
    CHECK(console_shading_rate_combiner(4) == D3D12_SHADING_RATE_COMBINER_SUM);
    CHECK(console_shading_rate_combiner(7) == D3D12_SHADING_RATE_COMBINER_PASSTHROUGH);
}

// Settings words built from their fields as the console runtime packs them.
static u32 settings_word(u32 vertex, u32 primitive, u32 image, u32 rate_x, u32 rate_y) {
    return vertex << 1 | primitive << 4 | image << 7 | rate_x << 15 | rate_y << 17;
}

static void test_shading_rate_settings() {
    ConsoleShadingRateSettings s = console_shading_rate_settings(settings_word(0, 0, 0, 1, 1), false);
    CHECK(s.base == D3D12_SHADING_RATE_2X2 && !s.supersampling);
    CHECK(s.combiners[0] == D3D12_SHADING_RATE_COMBINER_PASSTHROUGH);
    CHECK(s.combiners[1] == D3D12_SHADING_RATE_COMBINER_PASSTHROUGH);
    s = console_shading_rate_settings(settings_word(3, 0, 2, 1, 0), false);
    CHECK(s.base == D3D12_SHADING_RATE_2X1);
    CHECK(s.combiners[0] == D3D12_SHADING_RATE_COMBINER_MAX);
    CHECK(s.combiners[1] == D3D12_SHADING_RATE_COMBINER_MIN);
    // The primitive combiner wins when set.
    s = console_shading_rate_settings(settings_word(3, 1, 0, 0, 1), false);
    CHECK(s.base == D3D12_SHADING_RATE_1X2 && s.combiners[0] == D3D12_SHADING_RATE_COMBINER_OVERRIDE);
    // Supersampling (negative log2 rates) has no desktop rate.
    s = console_shading_rate_settings(settings_word(0, 0, 0, 3, 1), false);
    CHECK(s.supersampling && s.base == D3D12_SHADING_RATE_1X2);
}

static void test_sample_positions() {
    ConsoleSampleLocations in;
    memset(&in, 0, sizeof(in));
    const s8 k[4][2] = {{-2, -6}, {6, -2}, {-6, 2}, {2, 6}};
    for (u32 p = 0; p < 4; p++)
        for (u32 s = 0; s < 4; s++) {
            in.pixels[p][s][0] = k[s][0];
            in.pixels[p][s][1] = k[s][1];
        }
    D3D12_SAMPLE_POSITION out[64];
    CHECK(console_sample_positions(in, 4, out) == 1);
    CHECK(out[0].X == -2 && out[0].Y == -6 && out[3].X == 2 && out[3].Y == 6);
    // One pixel differs: all four go, in quad order, and offsets past desktop's range are clamped.
    in.pixels[3][1][0] = 8;
    in.pixels[2][0][1] = -9;
    CHECK(console_sample_positions(in, 4, out) == 4);
    CHECK(out[3 * 4 + 1].X == 7 && out[3 * 4 + 1].Y == -2);
    CHECK(out[2 * 4 + 0].X == -2 && out[2 * 4 + 0].Y == -8);
    CHECK(out[1 * 4 + 2].X == -6 && out[1 * 4 + 2].Y == 2);
}

void test_command_args() {
    test_copy_flags();
    test_quad_sample_mask();
    test_shading_rates();
    test_shading_rate_settings();
    test_sample_positions();
}
