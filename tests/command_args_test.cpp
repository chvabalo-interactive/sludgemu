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

static D3D12_RESOURCE_DESC1 texture_desc(D3D12_RESOURCE_DIMENSION dim, u64 w, u32 h, u16 depth, u16 mips,
                                         DXGI_FORMAT format) {
    D3D12_RESOURCE_DESC1 d = {};
    d.Dimension = dim;
    d.Width = w;
    d.Height = h;
    d.DepthOrArraySize = depth;
    d.MipLevels = mips;
    d.Format = format;
    d.SampleDesc.Count = 1;
    return d;
}

static void test_copyable_footprints() {
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT f[16];
    u32 rows[16];
    u64 row_sizes[16], total = 0;
    // RGBA8 100x30, 3 mips: rows pitch-aligned to 256, subresources to 512.
    D3D12_RESOURCE_DESC1 d =
        texture_desc(D3D12_RESOURCE_DIMENSION_TEXTURE2D, 100, 30, 1, 3, DXGI_FORMAT_R8G8B8A8_UNORM);
    CHECK(console_copyable_footprints(d, 0, 3, 0, f, rows, row_sizes, &total));
    CHECK(f[0].Offset == 0 && f[0].Footprint.Width == 100 && f[0].Footprint.Height == 30);
    CHECK(f[0].Footprint.RowPitch == 512 && rows[0] == 30 && row_sizes[0] == 400);
    CHECK(f[1].Offset == 15360 && f[1].Footprint.Width == 50 && f[1].Footprint.RowPitch == 256 &&
          rows[1] == 15);
    CHECK(f[2].Offset == 19456 && f[2].Footprint.Width == 25 && rows[2] == 7);
    CHECK(total == 19456 + 256 * 6 + 100);
    // Block compressed: footprints in whole blocks, rows of blocks.
    d = texture_desc(D3D12_RESOURCE_DIMENSION_TEXTURE2D, 10, 10, 1, 1, DXGI_FORMAT_BC1_UNORM);
    CHECK(console_copyable_footprints(d, 0, 1, 1024, f, rows, row_sizes, &total));
    CHECK(f[0].Offset == 1024 && f[0].Footprint.Width == 12 && f[0].Footprint.Height == 12);
    CHECK(rows[0] == 3 && row_sizes[0] == 24 && total == 256 * 2 + 24);
    // Depth-stencil: the depth plane, then the stencil plane.
    d = texture_desc(D3D12_RESOURCE_DIMENSION_TEXTURE2D, 64, 64, 1, 1, DXGI_FORMAT_D32_FLOAT_S8X24_UINT);
    CHECK(console_copyable_footprints(d, 0, 2, 0, f, rows, row_sizes, &total));
    CHECK(f[0].Footprint.Format == DXGI_FORMAT_R32_TYPELESS && row_sizes[0] == 256);
    CHECK(f[1].Footprint.Format == DXGI_FORMAT_R8_TYPELESS && row_sizes[1] == 64 && f[1].Offset == 256 * 64);
    CHECK(!console_copyable_footprints(d, 1, 2, 0, f, rows, row_sizes, &total)); // past the planes
    // Volumes: depth slices one after another.
    d = texture_desc(D3D12_RESOURCE_DIMENSION_TEXTURE3D, 16, 16, 4, 1, DXGI_FORMAT_R32_FLOAT);
    CHECK(console_copyable_footprints(d, 0, 1, 0, f, rows, row_sizes, &total));
    CHECK(f[0].Footprint.Depth == 4 && total == 256 * (16 * 4 - 1) + 64);
    // Buffers: one subresource of the buffer's bytes.
    d = texture_desc(D3D12_RESOURCE_DIMENSION_BUFFER, 1000, 1, 1, 1, DXGI_FORMAT_UNKNOWN);
    CHECK(console_copyable_footprints(d, 0, 1, 0, f, rows, row_sizes, &total));
    CHECK(total == 1000 && f[0].Footprint.Width == 1000 && rows[0] == 1);
}

void test_command_args() {
    test_copy_flags();
    test_quad_sample_mask();
    test_shading_rates();
    test_shading_rate_settings();
    test_sample_positions();
    test_copyable_footprints();
}
