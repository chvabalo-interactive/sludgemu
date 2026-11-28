// Console graphics pipeline parsing: synthetic serialized pipelines (a vertex pipeline and a mesh
// pipeline) through console_parse_graphics, and the user data layout mesh shaders get.

#include <string.h>

#include "shader/console_pipeline.h"
#include "test.h"

static void put_u32(u8* p, u32 offset, u32 value) {
    memcpy(p + offset, &value, 4);
}

// Metadata of the current version with no stream output; `with_element` adds one input element
// (POSITION, R32G32B32_FLOAT from slot 1 at offset 12, per vertex).
static usize make_metadata(u8* md, usize size, bool with_element) {
    memset(md, 0, size);
    put_u32(md, 4, 4096);         // version
    put_u32(md, 28, 0xFFFFFFFFu); // sample mask
    md[38] = 3;                   // triangles
    put_u32(md, 60, 0);           // stream output size
    put_u32(md, 64, with_element ? 1 : 0);
    if (!with_element)
        return 80;
    put_u32(md, 68, 9);
    memcpy(md + 72, "POSITION", 9);
    usize at = 72 + 9;
    put_u32(md, (u32)at + 0, 0);   // semantic index
    put_u32(md, (u32)at + 4, 6);   // DXGI_FORMAT_R32G32B32_FLOAT
    put_u32(md, (u32)at + 8, 1);   // slot
    put_u32(md, (u32)at + 12, 12); // offset
    put_u32(md, (u32)at + 16, 0);  // per vertex
    put_u32(md, (u32)at + 20, 0);  // step rate
    return at + 24;
}

// A vertex pipeline (configuration 4: no tessellation, no geometry stage): the vertex shader's RSRC2
// at +60, the pixel shader's at +80, context registers from 88.
static void test_vertex_pipeline() {
    u8 packet[256] = {};
    u8 md[160];
    usize md_size = make_metadata(md, sizeof(md), true);
    put_u32(packet, PACKET_CONFIG, 4);
    put_u32(packet, PACKET_VS_RSRC2, 9u << 1);
    put_u32(packet, PACKET_PS_RSRC2, 12u << 1);
    usize context = console_graphics_context_offset(packet, sizeof(packet));
    CHECK(context == 88);
    put_u32(packet, (u32)context + CTX_PS_INPUT_ENA, 0x2);
    put_u32(packet, (u32)context + CTX_PS_COLOR_FORMAT, 0x4);
    put_u32(packet, (u32)context + CTX_VS_OUT_CNTL, 0x01400303);
    ConsoleGraphicsShaders g;
    CHECK(console_parse_graphics(packet, sizeof(packet), md, md_size, &g));
    CHECK(g.error == nullptr);
    CHECK(g.config == 0);
    CHECK(!g.geometry);
    CHECK(g.vs_user_sgprs == 9);
    CHECK(g.ps_user_sgprs == 12);
    CHECK(g.ps_input_ena == 0x2);
    CHECK(g.ps_color_format == 0x4);
    CHECK(g.vs_out_cntl == 0x01400303);
    CHECK(g.primitive_topology_type == 3);
    CHECK(g.element_count == 1);
    CHECK(strcmp(g.element_semantic[0], "POSITION") == 0);
    CHECK(g.elements[0].format == 6 && g.elements[0].slot == 1 && g.elements[0].offset == 12);
    CHECK(!console_mesh_pipeline(g, false, true));

    // A depth-only pipeline has no pixel shader: its registers are zero, the vertex shader's count stays.
    put_u32(packet, PACKET_PS_RSRC2, 0);
    CHECK(console_parse_graphics(packet, sizeof(packet), md, md_size, &g));
    CHECK(g.vs_user_sgprs == 9 && g.ps_user_sgprs == 0);

    // Tessellation pipelines aren't read.
    put_u32(packet, PACKET_CONFIG, 5);
    CHECK(!console_parse_graphics(packet, sizeof(packet), md, md_size, &g));
    CHECK(g.error && strstr(g.error, "tessellation"));
}

// A mesh pipeline (configuration 6): the program in the geometry stage, its registers at +96/+100, the
// group shape at +112, context registers from 172.
static void make_mesh_packet(u8* packet, usize size) {
    memset(packet, 0, size);
    put_u32(packet, PACKET_CONFIG, 6);
    put_u32(packet, PACKET_PS_RSRC2, 17u << 1);
    put_u32(packet, PACKET_GS_RSRC2, 17u << 1 | 1u << 19); // 17 user SGPRs, 512 bytes of LDS
    put_u32(packet, PACKET_GS_ONCHIP, 0x20040080);         // 128 vertices, 128 primitives
    put_u32(packet, 116, 0x80);
    put_u32(packet, 120, 0x80);
    put_u32(packet, 168, 0x8004);
    put_u32(packet, 172 + CTX_VS_OUT_CNTL, 0x01400101);
}

static void test_mesh_pipeline() {
    u8 packet[336];
    u8 md[96];
    usize md_size = make_metadata(md, sizeof(md), false);
    make_mesh_packet(packet, sizeof(packet));
    CHECK(console_graphics_context_offset(packet, sizeof(packet)) == 172);
    ConsoleGraphicsShaders g;
    CHECK(console_parse_graphics(packet, sizeof(packet), md, md_size, &g));
    CHECK(g.config == 2);
    CHECK(g.geometry);
    CHECK(g.vs_user_sgprs == 0);
    CHECK(g.ps_user_sgprs == 17);
    CHECK(g.gs_user_sgprs == 17);
    CHECK(g.mesh_lds_bytes == 512);
    CHECK(g.gs_onchip == 0x20040080);
    CHECK(g.mesh_max_vertices == 128 && g.mesh_max_primitives == 128 && g.mesh_threads == 128);
    CHECK(g.gs_unknown[0] == 0x80 && g.gs_unknown[1] == 0x80 && g.gs_unknown[2] == 0x8004);
    CHECK(g.vs_out_cntl == 0x01400101);
    CHECK(g.element_count == 0);
    // A mesh pipeline: geometry-stage code and nothing else besides the pixel shader, no input layout.
    CHECK(console_mesh_pipeline(g, true, false));
    CHECK(!console_mesh_pipeline(g, false, false));
    CHECK(!console_mesh_pipeline(g, true, true));

    // Threads per group are the larger of the vertex and primitive counts.
    put_u32(packet, PACKET_GS_ONCHIP, 64u | 100u << 11);
    CHECK(console_parse_graphics(packet, sizeof(packet), md, md_size, &g));
    CHECK(g.mesh_max_vertices == 64 && g.mesh_max_primitives == 100 && g.mesh_threads == 100);

    // An input layout makes it something else (a vertex pipeline compiled for the geometry stage).
    u8 md_element[160];
    usize md_element_size = make_metadata(md_element, sizeof(md_element), true);
    CHECK(console_parse_graphics(packet, sizeof(packet), md_element, md_element_size, &g));
    CHECK(!console_mesh_pipeline(g, true, false));

    // The user SGPR count's extension bit isn't guessed at.
    make_mesh_packet(packet, sizeof(packet));
    put_u32(packet, PACKET_GS_RSRC2, 17u << 1 | 1u << 27);
    CHECK(!console_parse_graphics(packet, sizeof(packet), md, md_size, &g));
    CHECK(g.error && strstr(g.error, "extension bit"));

    // A packet too short for the longer register block.
    make_mesh_packet(packet, sizeof(packet));
    CHECK(!console_parse_graphics(packet, 200, md, md_size, &g));
}

// Mesh shaders' root parameters start at s16; with their user SGPRs from s8, a root signature fits the
// packet when its user data ends at s8 + the packet's count.
static void test_mesh_user_data() {
    ConsoleRootLayout layout = {};
    layout.parameter_count = 3;
    layout.parameters[0].type = ROOT_TABLE;
    layout.parameters[0].range_count = 1;
    layout.parameters[0].ranges[0] = {RANGE_SRV, 0, ~0u};
    layout.parameters[1].type = ROOT_CONSTANTS;
    layout.parameters[1].constants = 4;
    layout.parameters[2].type = ROOT_CBV;
    layout.flags = CONSOLE_ROOT_HEAP_RESOURCES;
    ShaderInfo info = {};
    u32 end = console_user_data_layout(layout, CONSOLE_MESH_FIRST_ROOT_SGPR, &info);
    CHECK(end == 16 + 1 + 4 + 2 + 1);
    CHECK(info.parameters[0].sgpr == 16 && info.parameters[1].sgpr == 17 && info.parameters[2].sgpr == 21);
    CHECK(info.resource_heap_sgpr == 23 && info.sampler_heap_sgpr == NO_HEAP_SGPR);
    CHECK(end == CONSOLE_MESH_FIRST_USER_SGPR + 16); // what a packet with 16 user SGPRs needs
}

void test_console_pipeline() {
    test_vertex_pipeline();
    test_mesh_pipeline();
    test_mesh_user_data();
}
