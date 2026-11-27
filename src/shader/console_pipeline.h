// Console pipeline descriptions as titles hand them over in serialized form: the parts shader
// translation needs (user data layout, stage registers, input layout).
//
// A serialized graphics pipeline is a packet (fixed header, shader register block, context register
// block, secondary block), pipeline metadata (API state the registers do not keep, stream output,
// input layout) and per stage code. Root parameters are allocated to user SGPRs in order from s8;
// heap base registers for directly indexed heaps follow them.

#pragma once

#include "common/types.h"
#include "shader/translate.h"

// The root signature as far as user data goes.
enum ConsoleRootParameterType : u8 {
    ROOT_TABLE,
    ROOT_CONSTANTS,
    ROOT_CBV,
    ROOT_SRV,
    ROOT_UAV,
};

struct ConsoleRootParameter {
    ConsoleRootParameterType type;
    bool sampler_table; // a table of samplers
    u8 constants;       // ROOT_CONSTANTS: dwords
    u8 range_count;
    TableRange ranges[SHADER_MAX_TABLE_RANGES];
};

#define CONSOLE_ROOT_HEAP_RESOURCES 0x400 // root signature flags: directly indexed heaps
#define CONSOLE_ROOT_HEAP_SAMPLERS  0x800

struct ConsoleRootLayout {
    u32 flags;
    u8 parameter_count;
    ConsoleRootParameter parameters[SHADER_MAX_ROOT_PARAMETERS];
};

// Where root parameters start in user SGPRs: graphics stages keep s0..s7 for the system (vertex
// buffer table, base vertex, ...), compute s0..s3.
#define CONSOLE_FIRST_ROOT_SGPR_GRAPHICS 8
#define CONSOLE_FIRST_ROOT_SGPR_COMPUTE  4

// Fills the user data part of `info` (parameters, heap base registers) from the root signature, for
// root parameters from `first_sgpr`. Returns the number of user SGPRs the layout takes.
u32 console_user_data_layout(const ConsoleRootLayout& layout, u32 first_sgpr, ShaderInfo* info);

// What the compute packet says about the shader.
struct ConsoleComputeShader {
    u16 threads[3];
    u8 user_sgprs;
    u8 group_id_mask;
    u8 thread_id_count;
    // Launched as wave32 (else wave64). Read from the bit all wave64 programs seen have clear; no
    // wave32 program has confirmed it yet (the translator checks it against the code).
    bool wave32;
    u32 lds_bytes;
};

bool console_parse_compute(const void* packet, usize packet_size, ConsoleComputeShader* out);

// What the graphics packet and metadata say about the shaders.
struct ConsoleGraphicsShaders {
    u8 vs_user_sgprs;
    u8 ps_user_sgprs;
    u32 ps_input_ena;
    u32 ps_color_format;
    u8 element_count;
    VertexElement elements[SHADER_MAX_VERTEX_ELEMENTS];
    bool element_per_instance[SHADER_MAX_VERTEX_ELEMENTS];
    u32 element_step_rate[SHADER_MAX_VERTEX_ELEMENTS];
    const char* element_semantic[SHADER_MAX_VERTEX_ELEMENTS]; // in the metadata, NUL terminated
    u32 element_semantic_index[SHADER_MAX_VERTEX_ELEMENTS];
    u32 sample_mask;
    u8 primitive_topology_type;
};

bool console_parse_graphics(const void* packet, usize packet_size, const void* metadata, usize metadata_size,
                            ConsoleGraphicsShaders* out);

// Byte offset of the context register block in a graphics packet.
usize console_graphics_context_offset(const void* packet, usize packet_size);
