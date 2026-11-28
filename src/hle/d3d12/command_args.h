#pragma once

#include <windows.h>
#include <d3d12.h>

#include "common/types.h"

// Translation of console-only command list arguments into desktop ones. Pure functions of their
// arguments (no device, no state), so they're unit tested on their own.

// --- copy flags (CopyTextureRegionX, CopyResourceX, ...) ---------------------------------------------

// Flags that manage console compression metadata around a copy (HTILE, CMASK, FMASK, DCC: resummarize,
// decompress, eliminate, invalidate) or pick the engine doing it. Desktop drivers keep their own
// compression consistent across copies, so these change nothing there.
#define CONSOLE_COPY_FLAGS_METADATA 0x1C70u
// Flags that copy depth or stencil samples into the color channels of the other resource: a copy
// between a depth and a color format, which desktop copies don't do.
#define CONSOLE_COPY_FLAGS_PLANE_TO_COLOR 0x380u

// The flags of `flags` a desktop copy can't honour (CONSOLE_COPY_FLAGS_PLANE_TO_COLOR bits and bits
// nobody defines); 0 when a plain desktop copy is the same copy.
u32 console_copy_flags_unsupported(u32 flags);

// --- sample mask (OMSetSampleMaskX) ------------------------------------------------------------------

// The console mask covers a 2x2 pixel quad, 16 sample bits per pixel: (0,0) in bits 0-15, (1,0) in
// 16-31, (0,1) in 32-47, (1,1) in 48-63. Desktop masks apply to every pixel alike. Returns the 16-bit
// mask every pixel gets; `uniform` says whether all four pixels asked for the same one. When they
// differ, the pixels' masks are combined (a sample any pixel keeps is kept): no sample the title
// draws goes missing, at the cost of some it meant to leave alone.
u16 console_quad_sample_mask(u64 quad_mask, bool* uniform);

// --- variable rate shading (RSSetShadingRate, RSSetShadingRateX) -------------------------------------

// Console shading rates use the desktop encoding (log2 of the coarse size per axis, x in bits 2-3, y in
// bits 0-1). Desktop GPUs without the additional rates have no 2x4, 4x2 or 4x4: those become 2x2, the
// coarsest common rate. Unknown values become 1x1.
D3D12_SHADING_RATE console_shading_rate(u32 rate, bool additional_rates);

// Console combiners are the desktop ones (passthrough, override, min, max, sum); unknown values pass
// through.
D3D12_SHADING_RATE_COMBINER console_shading_rate_combiner(u32 combiner);

// RSSetShadingRateX with combiner settings: the console programs its VRS registers directly. What the
// desktop has of them: a base (state) rate and the two combiners (the vertex/primitive rate, and the
// shading rate image, which on console lives in the depth buffer's metadata).
struct ConsoleShadingRateSettings {
    D3D12_SHADING_RATE base;
    D3D12_SHADING_RATE_COMBINER combiners[2];
    bool supersampling; // the state rate asks for supersampling, which desktop VRS lacks (1x1 instead)
};

// Decodes the combiner settings word (the bit fields of the console settings' first union member).
ConsoleShadingRateSettings console_shading_rate_settings(u32 bits, bool additional_rates);

// --- sample positions (RSSetMSAAParametersX) ---------------------------------------------------------

// Console sample locations: for each pixel of a 2x2 quad (top left, top right, bottom left, bottom
// right), 16 positions as x, y offsets from the pixel center in 1/16 pixel.
struct ConsoleSampleLocations {
    s8 pixels[4][16][2];
};

// The desktop sample positions for `samples` samples per pixel (2 to 16): one pixel's when all four
// pixels have the same positions (returns 1), else all four in desktop order (returns 4; needs sample
// position tier 2). Offsets are clamped to what desktop allows (-8 to 7). `out` holds 64 positions.
u32 console_sample_positions(const ConsoleSampleLocations& in, u32 samples, D3D12_SAMPLE_POSITION* out);
