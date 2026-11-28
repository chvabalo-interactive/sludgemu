#pragma once

#include <windows.h>
#include <dxgiformat.h>

#include "common/types.h"
#include "core/console_profile.h"

// Console GPU texture layouts.
//
// Console textures live in title memory in the GPU's own layout: RDNA2 swizzle modes, mip chains
// laid out smallest first, compression metadata planes next to the image. Titles size and place
// textures from these layouts, stream data into them, and read texels back, so they have to come
// out exactly as on console. They are computed with AMD's address library (addrlib), configured
// as the console GPU (the two console models differ in memory pipes and packers).
//
// The layout structs below follow the console layout library's ABI, so they can be handed to
// titles as they are.

// Swizzle mode as stored in a texture layout value: CONSOLE_LAYOUT_SWIZZLE | mode, with the bank
// rotation in bits 12-15.
#define CONSOLE_LAYOUT_SWIZZLE      0x100u
#define CONSOLE_LAYOUT_SWIZZLE_MASK 0x11Fu

enum ConsolePlaneUsage : u32 {
    CONSOLE_PLANE_UNUSED = 0,
    CONSOLE_PLANE_DEFAULT = 1,
    CONSOLE_PLANE_COLOR_MASK = 2,    // CMask: fast clear state per tile
    CONSOLE_PLANE_FRAGMENT_MASK = 3, // FMask: MSAA fragment indices
    CONSOLE_PLANE_HTILE = 4,         // depth compression metadata
    CONSOLE_PLANE_LUMA = 5,
    CONSOLE_PLANE_CHROMA = 6,
    CONSOLE_PLANE_DEPTH = 7,
    CONSOLE_PLANE_STENCIL = 8,
    CONSOLE_PLANE_DCC = 9, // delta color compression metadata
};

struct ConsoleMipLayout {
    u64 size;
    u64 offset; // from the start of the plane
    u64 slice_2d_size;
    u32 pitch_pixels;
    u32 pitch_bytes;
    u32 alignment;
    u32 padded_width; // in elements
    u32 padded_height;
    u32 padded_depth_or_array_size;
    u32 width; // in elements
    u32 height;
    u32 width_pixels;
    u32 height_pixels;
    u32 depth_or_array_size;
    u32 sample_count;
    u32 swizzle_mode;
    u64 bank_rotation_address_mask;
    u64 bank_rotation_bytes_per_slice;
    u32 slice_depth; // in elements
    u32 in_mip_tail;
    u32 tail_x : 11; // position inside the mip tail, in elements
    u32 tail_y : 11;
    u32 tail_z : 10;
};
static_assert(sizeof(ConsoleMipLayout) == 112, "console mip layout ABI");

#define CONSOLE_MAX_MIPS   15
#define CONSOLE_MAX_PLANES 4

struct ConsolePlaneLayout {
    ConsolePlaneUsage usage;
    u64 size;
    u64 slice_size; // one array slice: a whole mip chain
    u64 base_offset;
    u64 base_alignment;
    u32 bytes_per_element;
    u32 standard_mip_count; // mips before the mip tail
    u32 tail_mip_count;
    ConsoleMipLayout mips[CONSOLE_MAX_MIPS];
};

struct ConsoleTextureLayout {
    u64 size;
    u64 base_alignment;
    u32 mip_levels;
    u32 plane_count;
    ConsolePlaneLayout planes[CONSOLE_MAX_PLANES];
    u32 dimension; // D3D12_RESOURCE_DIMENSION
};

// What a layout is computed from (a D3D12 resource description).
struct ConsoleTextureDesc {
    u32 dimension; // D3D12_RESOURCE_DIMENSION
    u64 width;
    u32 height;
    u32 depth_or_array_size;
    u32 mip_levels; // 0 = full chain
    DXGI_FORMAT format;
    u32 sample_count;
    u32 layout; // D3D12_TEXTURE_LAYOUT, or a console swizzle mode (CONSOLE_LAYOUT_SWIZZLE | mode)
    u32 flags;  // D3D12 resource flags, console flags included
};

// Sets up layout computation for a console model. Call once, before any layout is computed.
bool console_layout_init(ConsoleModel model);

// The swizzle mode the console runtime picks for a texture (a CONSOLE_LAYOUT_SWIZZLE value), or 0.
u32 console_suggest_layout(const ConsoleTextureDesc& desc);

// The swizzle mode a texture is laid out in, as the hardware numbers them (addrlib's AddrSwizzleMode):
// the description's own, linear for row-major layouts, else the console runtime's pick.
u32 console_swizzle_mode(const ConsoleTextureDesc& desc);

// Computes the layout of a texture. Fails for descriptions the console can't create.
bool console_texture_layout(const ConsoleTextureDesc& desc, ConsoleTextureLayout* out);

// Byte offset of a texel element from the start of the texture, or ~0 if out of range.
u64 console_texel_offset(const ConsoleTextureDesc& desc, const ConsoleTextureLayout& layout, u32 plane,
                         u32 mip, u32 x, u32 y, u32 z_or_slice, u32 sample, u32* bit_position);

// How the elements of one subresource (a mip of an array slice; a whole volume mip for 3D) of a plane
// are laid out in texture memory, in a form a GPU shader evaluates. Swizzled layouts (as the layout
// library computes them) put elements in blocks, row after row of blocks, volumes block slice after
// block slice; inside a block, an element's place is the XOR of a fixed address pattern for every set
// bit of its coordinates (all of them: higher bits pick pipes and banks), coordinates counted from the
// mip's place in the mip tail, the array slice's own bits XORed in as well. Linear layouts are rows of
// `pitch` bytes.
//
//   x' = x + tail[0], y' = y + tail[1], z' = z + tail[2]
//   offset = base + (z >> block_shift[2]) * slice_stride
//          + ((y >> block_shift[1]) * row_blocks + (x >> block_shift[0])) << block_bytes_shift
//          + (pattern(x') ^ pattern(y') ^ pattern(z') ^ block_xor)
struct ConsoleSubresourceAddressing {
    u64 base;
    u32 bytes_per_element;
    u32 width, height, depth; // in elements
    bool linear;
    u32 pitch;       // linear: bytes per row
    u32 slice_pitch; // linear volumes: bytes per depth slice
    u8 block_shift[3];
    u8 block_bytes_shift;
    u32 row_blocks;                         // blocks per row
    u64 slice_stride;                       // volumes: bytes from one block slice to the next
    u32 block_xor;                          // the array slice's pattern
    u32 tail[3];                            // where the mip sits in its block
    u16 bits_x[14], bits_y[14], bits_z[12]; // the pattern of each coordinate bit
};

// The addressing of a subresource. Fails for layouts it doesn't describe (planes of other than 1, 2,
// 4, 8 or 16 bytes per element, multisampled textures) and, with `check`, logged, if a spread of
// elements doesn't come out where the layout puts them (checking costs layout computations; a kind
// of texture checked once needn't be again).
bool console_subresource_addressing(const ConsoleTextureDesc& desc, const ConsoleTextureLayout& layout,
                                    u32 plane, u32 mip, u32 slice, ConsoleSubresourceAddressing* out,
                                    bool check);

// Byte offset of element (x, y, z) of a subresource from the start of the texture.
u64 console_subresource_offset(const ConsoleSubresourceAddressing& a, u32 x, u32 y, u32 z);

// Copies one subresource (mip `mip` of array slice `slice`; a whole volume mip for 3D) of a plane
// between linear memory and texture memory in console layout.
bool console_copy_subresource(const ConsoleTextureDesc& desc, const ConsoleTextureLayout& layout, u32 plane,
                              u32 mip, u32 slice, void* texture, void* linear, u64 row_pitch, u64 slice_pitch,
                              bool into_texture);
