// Console pipeline descriptions as titles hand them over in serialized form: the parts shader
// translation needs (user data layout, stage registers, input layout).
//
// A serialized graphics pipeline is a packet (fixed header, shader register block, context register
// block, secondary block), pipeline metadata (API state the registers do not keep, stream output,
// input layout) and per stage code. Root parameters are allocated to user SGPRs in order from s8;
// heap base registers for directly indexed heaps follow them.
//
// The packet's configuration says which hardware stages run. Vertex pipelines (bits 1:0 clear) have a
// vertex and a pixel shader. Geometry-stage pipelines (bit 1) run their program in the GS slot: mesh
// shaders are these; their register block is longer and the context block starts later. Tessellation
// pipelines (bit 0) are not read yet.

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
// buffer table, base vertex, ...), compute s0..s3. Mesh shaders get their user SGPRs from s8 (s0..s7
// hold launch state) and keep s8..s15 for the system, so their root parameters start at s16.
#define CONSOLE_FIRST_ROOT_SGPR_GRAPHICS 8
#define CONSOLE_FIRST_ROOT_SGPR_COMPUTE  4
#define CONSOLE_MESH_FIRST_USER_SGPR     8
#define CONSOLE_MESH_FIRST_ROOT_SGPR     16

// Graphics packet offsets (bytes): the configuration, then the shader registers.
#define PACKET_CONFIG    4   // bit 0: tessellation stages, bit 1: a geometry-stage program
#define PACKET_VS_RSRC2  60  // SPI_SHADER_PGM_RSRC2_VS: user SGPR count in bits 5:1
#define PACKET_PS_RSRC1  76  // SPI_SHADER_PGM_RSRC1_PS
#define PACKET_PS_RSRC2  80  // SPI_SHADER_PGM_RSRC2_PS
#define PACKET_GS_RSRC1  96  // geometry-stage pipelines: SPI_SHADER_PGM_RSRC1_GS
#define PACKET_GS_RSRC2  100 // SPI_SHADER_PGM_RSRC2_GS: user SGPRs in bits 5:1, LDS in bits 26:19
#define PACKET_GS_ONCHIP 112 // VGT_GS_ONCHIP_CNTL: vertices per group in bits 10:0, primitives in 21:11
// Context register block offsets the shaders depend on.
#define CTX_PS_INPUT_ENA    0  // SPI_PS_INPUT_ENA
#define CTX_PS_COLOR_FORMAT 24 // SPI_SHADER_COL_FORMAT
#define CTX_VS_OUT_CNTL     64 // PA_CL_VS_OUT_CNTL: clip and cull distances (see ShaderInfo::vs_out_cntl)

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
    u8 config; // the packet's configuration (PACKET_CONFIG, bits 1:0)
    u8 vs_user_sgprs;
    u8 ps_user_sgprs;
    u32 ps_input_ena;
    u32 ps_color_format;
    u32 vs_out_cntl; // PA_CL_VS_OUT_CNTL

    // Geometry-stage pipelines (config bit 1). The hardware launches max(vertices, primitives) threads
    // per group.
    bool geometry;
    u8 gs_user_sgprs;
    u16 mesh_max_vertices;
    u16 mesh_max_primitives;
    u16 mesh_threads;
    u32 mesh_lds_bytes;
    u32 gs_onchip; // VGT_GS_ONCHIP_CNTL as it is
    // Packet dwords at +116, +120 and +168, not decoded: kept so that `shader_tool pipelines` shows
    // pipelines that differ in them.
    u32 gs_unknown[3];

    u8 element_count;
    VertexElement elements[SHADER_MAX_VERTEX_ELEMENTS];
    bool element_per_instance[SHADER_MAX_VERTEX_ELEMENTS];
    u32 element_step_rate[SHADER_MAX_VERTEX_ELEMENTS];
    const char* element_semantic[SHADER_MAX_VERTEX_ELEMENTS]; // in the metadata, NUL terminated
    u32 element_semantic_index[SHADER_MAX_VERTEX_ELEMENTS];
    u32 sample_mask;
    u8 primitive_topology_type;
    const char* error; // why parsing failed
};

bool console_parse_graphics(const void* packet, usize packet_size, const void* metadata, usize metadata_size,
                            ConsoleGraphicsShaders* out);

// Whether a parsed pipeline is a mesh pipeline: a geometry-stage program, no other stage besides the
// pixel shader (`other_stages`: whether any of them has code) and no input layout. A vertex pipeline
// compiled for the geometry stage would look the same; translation tells them apart by what the
// program reads at entry.
bool console_mesh_pipeline(const ConsoleGraphicsShaders& g, bool has_gs, bool other_stages);

// Byte offset of the context register block in a graphics packet.
usize console_graphics_context_offset(const void* packet, usize packet_size);
