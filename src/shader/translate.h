// Console shader translation: RDNA 2 machine code to HLSL for the desktop compiler.
//
// The model, in short:
//
// - Every register becomes a 32-bit HLSL variable of the same name. Each desktop thread runs the
//   console program for one console lane; scalar registers hold the same value in every thread.
// - Lane masks (EXEC, compare results) hold, in each thread, all ones or zero: that lane's bit,
//   broadcast. Bitwise mask operations then work unchanged, and the operations that look at the
//   whole mask (branches on EXECZ/VCCZ, bit counts) use wave intrinsics.
// - Scalar branches are uniform, so desktop waves stay converged; vector instructions only write
//   lanes whose EXEC bit is set, except where nothing reads the other lanes before they are written
//   again (see translate_lanes.cpp). The blocks between branches become structured HLSL control flow:
//   if/else, loops and labelled exits rebuilt from the dominator tree (see translate_flow.cpp).
//
// Compute programs run with console waves mapped exactly onto desktop waves, so that cross-lane
// operations and whole-mask queries see the console wave and nothing else:
// - The desktop wave size is pinned to 32 ([WaveSize]). A wave32 program runs one console lane per
//   desktop thread; a wave64 program runs two, console lanes L and L + 32 in desktop lane L: every
//   vector register has a variable per half (v<n>_0, v<n>_1) and vector instructions are emitted once
//   per half. Desktop groups are whole desktop waves, each one console wave (numbered at the start);
//   the console thread IDs are computed from the console wave and lane.
// - Mask dword h (exec_lo/exec_hi, vcc_lo/vcc_hi, ...) belongs to half h: in desktop lane L it holds
//   bit L of that dword, broadcast. 32-bit mask operations then work dword by dword unchanged.
// - Which registers hold masks is tracked statically (written by compares, carries, saveexec, moves
//   and bitwise operations of masks; EXEC always). Other values are plain bits, the same in every
//   thread: they are converted to the broadcast form where used as a mask (and when written to EXEC),
//   and masks are converted to bits (with ballots) where used as data. A register that holds a mask
//   on one path and data on another fails translation where a conversion would depend on it.
// - User data (the console's user SGPRs) comes from a desktop root constant block holding the same
//   values, except that descriptor tables hold byte offsets into the desktop descriptor heaps:
//   console code computes descriptor addresses with its own arithmetic, and the address of a loaded
//   descriptor, divided by the slot size, is its desktop heap index. Registers loaded from the heap
//   hold the byte address of each dword instead of descriptor bits, so translation fails where a
//   program changes a loaded descriptor or puts one together (except buffer descriptor fields a raw
//   desktop view ignores). Where a program reads a descriptor's dwords as values, translated code
//   reads them from the descriptor shadow (below) at that address.
// - The descriptor shadow: a buffer laid out like console resource heap memory, one 32-byte record per
//   desktop heap slot holding the console descriptor's first four dwords (buffer descriptors whole)
//   and the kind of the desktop view in the slot. Root signature ranges don't say reliably what a slot
//   holds (titles label them loosely, and console hardware never looks), so a program's own accesses
//   decide: a slot written through is an unordered access view, one sampled through a shader resource
//   view, and one only read takes the kind the shadow records.
// - Root descriptors (buffer addresses in user data) become desktop root SRVs/UAVs read as raw
//   buffers; registers holding their high address dword are tracked statically.
// - Vertex fetches (loads through the vertex buffer table) become desktop input assembler inputs.

#pragma once

#include "common/types.h"
#include "shader/text.h"

enum ShaderStage : u8 {
    SHADER_VERTEX,
    SHADER_PIXEL,
    SHADER_COMPUTE,
};

// What a root parameter places in user SGPRs.
enum UserDataKind : u8 {
    USER_DATA_NONE,
    USER_DATA_RESOURCE_TABLE, // byte offset of the table in the desktop CBV/SRV/UAV heap
    USER_DATA_SAMPLER_TABLE,  // byte offset of the table in the desktop sampler heap
    USER_DATA_CONSTANTS,
    USER_DATA_ROOT_CBV, // root descriptors: 2 SGPRs, read through desktop root SRVs/UAVs
    USER_DATA_ROOT_SRV,
    USER_DATA_ROOT_UAV,
};

// The kind of descriptor a resource table range holds.
enum TableRangeKind : u8 {
    RANGE_SRV,
    RANGE_UAV,
    RANGE_CBV,
};

struct TableRange {
    TableRangeKind kind;
    u32 offset; // in descriptors from the table start
    u32 count;  // UINT32_MAX: unbounded
};

#define SHADER_MAX_ROOT_PARAMETERS 64
#define SHADER_MAX_TABLE_RANGES    16
#define SHADER_MAX_VERTEX_ELEMENTS 32
#define NO_HEAP_SGPR               0xFF

struct UserDataParameter {
    UserDataKind kind;
    u8 sgpr;          // first user SGPR
    u8 dwords;        // SGPRs used
    u8 root_register; // root descriptors: desktop register (t or u) in the translated shader's space
    u8 range_count;   // resource tables: the table's ranges
    TableRange ranges[SHADER_MAX_TABLE_RANGES];
};

struct VertexElement {
    u8 slot;
    u32 offset; // byte offset in the vertex
    u32 format; // DXGI_FORMAT
};

// Everything about the program that is not in its code.
struct ShaderInfo {
    ShaderStage stage;
    u8 user_sgprs; // user SGPR count; system SGPRs follow
    u8 parameter_count;
    // Compute shaders: the wave size the pipeline launches with (32 or 64), 0 if unknown. (Here so that
    // it fills what used to be padding: dumps of older builds load with 0.)
    u8 wave_size;
    UserDataParameter parameters[SHADER_MAX_ROOT_PARAMETERS];
    // Directly indexed heaps: the user SGPR holding the heap's base (the translated program's value
    // is 0, the start of the desktop heap), or NO_HEAP_SGPR.
    u8 resource_heap_sgpr;
    u8 sampler_heap_sgpr;

    // Vertex shaders: the input layout; fetches are matched to elements by slot and offset.
    u8 element_count;
    VertexElement elements[SHADER_MAX_VERTEX_ELEMENTS];
    // Vertex shaders that leave loading their inputs to the driver's fetch subroutine (they call it with
    // s_swappc_b64): the subroutine loads element i into fetch_components[i] VGPRs from fetch_vgpr[i]
    // (0: not loaded), converted by the element's format.
    u8 fetch_vgpr[SHADER_MAX_VERTEX_ELEMENTS];
    u8 fetch_components[SHADER_MAX_VERTEX_ELEMENTS];

    // Pixel shaders: which input VGPRs exist (SPI_PS_INPUT_ADDR layout) and the export formats.
    u32 ps_input_addr;
    u32 ps_color_format; // SPI_SHADER_COL_FORMAT: 4 bits per render target

    // Compute shaders.
    u16 threads[3];
    u8 group_id_mask;   // which of x/y/z workgroup IDs follow the user SGPRs
    u8 thread_id_count; // thread ID VGPRs: 1..3
    u32 lds_bytes;

    // Mixed-interpolation split: 0 = keep flat reads on P<i>, 1 + dst = route flat reads/exports
    // of attribute i onto P<dst> so VS/PS don't need SV_Barycentrics / GetAttributeAtVertex.
    u8 flat_remap[32];
};

// A sampler built into the program: console code can sample with a sampler descriptor made from
// constants (static samplers are compiled that way). Decoded to D3D12 terms (filter, address modes,
// comparison and border are the D3D12 enumerations); the pipeline's desktop root signature carries it
// as a static sampler.
struct InlineSampler {
    u32 filter;
    u8 address[3];
    u8 max_anisotropy;
    u8 comparison;
    u8 border_color;
    bool unnormalized;
    u8 unused[3];
    f32 mip_bias;
    f32 min_lod;
    f32 max_lod;
};

#define SHADER_MAX_INLINE_SAMPLERS  16
#define TABLE_REACH_UNKNOWN         0xFFFF
#define SHADER_INLINE_SAMPLER_SPACE 103 // register s<i> holds inline sampler i

// The descriptor shadow (see the model above): a raw buffer at register t0 of this space, a record per
// desktop resource heap slot. Dwords 0-3: the console descriptor's dwords 0-3; dword 4: the desktop
// view's kind; dwords 5-7: zero.
#define SHADER_DESCRIPTOR_SHADOW_SPACE 101
#define SHADER_SHADOW_RECORD_BYTES     32
#define SHADER_SHADOW_KIND_OFFSET      16

enum ShadowViewKind : u32 {
    SHADOW_VIEW_NONE = 0, // a null view or nothing written
    SHADOW_VIEW_SRV = 1,
    SHADOW_VIEW_UAV = 2,
    SHADOW_VIEW_CBV = 3, // a constant buffer, viewed as a raw shader resource view
};

// The translation's result: HLSL source plus what the pipeline needs to know about it.
struct TranslatedShader {
    Text hlsl;
    u32 vertex_element_mask; // vertex shaders: input layout elements the shader reads (by index)
    u32 vs_param_mask;       // vertex shaders: exported parameters
    u32 ps_attribute_mask;   // pixel shaders: attributes read
    u32 ps_mixed_mask;       // pixel shaders: attributes declared flat, interpolated in the shader
    u32 ps_target_mask;      // pixel shaders: render targets written
    bool ps_writes_depth;
    bool ps_discards;
    bool uses_int64_atomics; // 64-bit atomics on typed views of the descriptor heap (an optional feature)
    // Vertex shaders: the desktop input format of each element read (raw fetches read dwords).
    u32 element_format[SHADER_MAX_VERTEX_ELEMENTS];
    u32 sampler_count;
    InlineSampler samplers[SHADER_MAX_INLINE_SAMPLERS];
    // Per root parameter: descriptor tables' slots the program loads from (one past the last), which
    // can be more than the root signature declares; TABLE_REACH_UNKNOWN if not all at known places.
    u16 table_reach[SHADER_MAX_ROOT_PARAMETERS];
    // The wave size the code itself proves (32 or 64; 0 if it doesn't tell). ShaderInfo::wave_size
    // decides only where the code doesn't.
    u8 code_wave_size;
    char error[160]; // why translation failed
};

// What the desktop device runs: the wave sizes (lanes) it may launch compute waves with.
struct ShaderTarget {
    u8 wave_min;
    u8 wave_max;
};

// Translates `code` (RDNA 2 instructions, `size` bytes) for `target`. Returns false with `out->error`
// set if the program uses something the translator does not handle.
bool shader_translate(const void* code, u32 size, const ShaderInfo& info, const ShaderTarget& target,
                      TranslatedShader* out);
