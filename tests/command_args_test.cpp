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

static void test_sample_mask_override() {
    u16 mask = 0x1234;
    // The pipeline's own mask, or one that differs only in samples the targets lack: no variant.
    CHECK(!console_sample_mask_override(0xFFFF, UINT32_MAX, 1, &mask) && mask == 0x1234);
    CHECK(!console_sample_mask_override(0xFFFF, UINT32_MAX, 0, &mask));
    CHECK(!console_sample_mask_override(0x000F, UINT32_MAX, 4, &mask));
    CHECK(!console_sample_mask_override(0xFF05, 0x5, 4, &mask));
    CHECK(!console_sample_mask_override(0xFFFF, UINT32_MAX, 16, &mask));
    // Fewer samples than the pipeline keeps: the mask, cut to the targets' samples.
    CHECK(console_sample_mask_override(0xFFF5, UINT32_MAX, 4, &mask) && mask == 0x5);
    CHECK(console_sample_mask_override(0, UINT32_MAX, 1, &mask) && mask == 0);
    CHECK(console_sample_mask_override(0x7FFF, UINT32_MAX, 16, &mask) && mask == 0x7FFF);
    // It replaces the pipeline's mask, so it can also bring back samples the pipeline leaves out.
    CHECK(console_sample_mask_override(0xFFFF, 0x1, 4, &mask) && mask == 0xF);
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
    CHECK(f[0].Footprint.RowPitch == 512); // planar rows: twice the pitch alignment
    CHECK(f[1].Footprint.Format == DXGI_FORMAT_R8_TYPELESS && row_sizes[1] == 64 && f[1].Offset == 512 * 64);
    CHECK(f[1].Footprint.RowPitch == 512 && total == 512 * 64 + 512 * 63 + 64);
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

// The desktop runtime applies the same rules to formats both sides share: footprints must match it
// exactly, as titles size and fill their copy buffers by them.
static void test_copyable_footprints_match_desktop() {
    ID3D12Device* device = nullptr;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)))) {
        printf("copyable footprints vs desktop: skipped (no D3D12 device)\n");
        return;
    }
    struct Case {
        D3D12_RESOURCE_DIMENSION dim;
        u64 width;
        u32 height;
        u16 depth;
        u16 mips;
        DXGI_FORMAT format;
    };
    const Case cases[] = {
        {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 100, 30, 1, 3, DXGI_FORMAT_R8G8B8A8_UNORM},
        {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 1920, 1080, 1, 0, DXGI_FORMAT_R8G8B8A8_UNORM},
        {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 333, 17, 6, 0, DXGI_FORMAT_R16G16B16A16_FLOAT},
        {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 10, 10, 1, 0, DXGI_FORMAT_BC1_UNORM},
        {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 1000, 600, 2, 4, DXGI_FORMAT_BC7_UNORM},
        {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 129, 65, 1, 0, DXGI_FORMAT_BC5_SNORM},
        {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 7, 3, 1, 0, DXGI_FORMAT_R32G32B32_FLOAT},
        {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 640, 360, 1, 1, DXGI_FORMAT_D32_FLOAT},
        {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 640, 360, 3, 2, DXGI_FORMAT_D32_FLOAT_S8X24_UINT},
        {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 300, 200, 1, 1, DXGI_FORMAT_D24_UNORM_S8_UINT},
        {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 100, 10, 2, 1, DXGI_FORMAT_R24G8_TYPELESS},
        {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 64, 64, 1, 0, DXGI_FORMAT_R32G8X24_TYPELESS},
        {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 100, 10, 1, 1, DXGI_FORMAT_D16_UNORM},
        {D3D12_RESOURCE_DIMENSION_TEXTURE1D, 4000, 1, 5, 0, DXGI_FORMAT_R8_UNORM},
        {D3D12_RESOURCE_DIMENSION_TEXTURE3D, 64, 32, 20, 0, DXGI_FORMAT_R32_FLOAT},
        {D3D12_RESOURCE_DIMENSION_TEXTURE3D, 17, 9, 5, 0, DXGI_FORMAT_BC4_UNORM},
        {D3D12_RESOURCE_DIMENSION_BUFFER, 1000, 1, 1, 1, DXGI_FORMAT_UNKNOWN},
    };
    const u64 bases[] = {0, 1536};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT ours[256], theirs[256];
    u32 our_rows[256], their_rows[256];
    u64 our_sizes[256], their_sizes[256];
    for (u32 c = 0; c < ARRAY_COUNT(cases); c++) {
        const Case& k = cases[c];
        D3D12_RESOURCE_DESC1 d1 = texture_desc(k.dim, k.width, k.height, k.depth, k.mips, k.format);
        if (k.dim == D3D12_RESOURCE_DIMENSION_BUFFER)
            d1.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_RESOURCE_DESC d = {d1.Dimension, 0,         d1.Width,      d1.Height, d1.DepthOrArraySize,
                                 d1.MipLevels, d1.Format, d1.SampleDesc, d1.Layout, d1.Flags};
        // How many subresources there are: as many as desktop gives footprints for. Then the whole
        // resource, and its last subresource alone.
        u64 their_total;
        u32 count = 0;
        for (u32 n = 1; n <= ARRAY_COUNT(theirs); n++) {
            their_total = UINT64_MAX;
            device->GetCopyableFootprints(&d, 0, n, 0, nullptr, nullptr, nullptr, &their_total);
            if (their_total == UINT64_MAX)
                break;
            count = n;
        }
        CHECK(count > 0);
        for (u32 b = 0; b < ARRAY_COUNT(bases); b++) {
            for (u32 first = 0; first < count; first += count > 1 ? count - 1 : 1) {
                u32 n = count - first;
                u64 our_total = 0;
                their_total = 0;
                device->GetCopyableFootprints(&d, first, n, bases[b], theirs, their_rows, their_sizes,
                                              &their_total);
                bool ok = console_copyable_footprints(d1, first, n, bases[b], ours, our_rows, our_sizes,
                                                      &our_total);
                CHECK(ok && our_total == their_total);
                if (!ok || our_total != their_total)
                    printf("  case %u (format %d), base %llu, first %u: total %llu, desktop %llu\n", c,
                           k.format, (unsigned long long)bases[b], first, (unsigned long long)our_total,
                           (unsigned long long)their_total);
                for (u32 i = 0; ok && i < n; i++) {
                    // Field by field: the footprint struct has padding.
                    const D3D12_SUBRESOURCE_FOOTPRINT& o = ours[i].Footprint;
                    const D3D12_SUBRESOURCE_FOOTPRINT& t = theirs[i].Footprint;
                    bool same = ours[i].Offset == theirs[i].Offset && o.Format == t.Format &&
                                o.Width == t.Width && o.Height == t.Height && o.Depth == t.Depth &&
                                o.RowPitch == t.RowPitch && our_rows[i] == their_rows[i] &&
                                our_sizes[i] == their_sizes[i];
                    CHECK(same);
                    if (!same) {
                        printf("  case %u sub %u: ours %llu %d %ux%ux%u pitch %u rows %u size %llu; desktop "
                               "%llu %d %ux%ux%u pitch %u rows %u size %llu\n",
                               c, first + i, (unsigned long long)ours[i].Offset, o.Format, o.Width, o.Height,
                               o.Depth, o.RowPitch, our_rows[i], (unsigned long long)our_sizes[i],
                               (unsigned long long)theirs[i].Offset, t.Format, t.Width, t.Height, t.Depth,
                               t.RowPitch, their_rows[i], (unsigned long long)their_sizes[i]);
                        break;
                    }
                }
            }
        }
    }
    device->Release();
}

void test_command_args() {
    test_copy_flags();
    test_quad_sample_mask();
    test_sample_mask_override();
    test_shading_rates();
    test_shading_rate_settings();
    test_sample_positions();
    test_copyable_footprints();
    test_copyable_footprints_match_desktop();
}
