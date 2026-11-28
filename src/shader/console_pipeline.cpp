#include "shader/console_pipeline.h"

#include <string.h>

u32 console_user_data_layout(const ConsoleRootLayout& layout, u32 first_sgpr, ShaderInfo* info) {
    u32 sgpr = first_sgpr;
    info->parameter_count = layout.parameter_count;
    for (u32 i = 0; i < layout.parameter_count && i < SHADER_MAX_ROOT_PARAMETERS; i++) {
        const ConsoleRootParameter& src = layout.parameters[i];
        UserDataParameter& p = info->parameters[i];
        memset(&p, 0, sizeof(p));
        p.sgpr = (u8)sgpr;
        p.root_register = (u8)i;
        switch (src.type) {
        case ROOT_TABLE: {
            p.kind = src.sampler_table ? USER_DATA_SAMPLER_TABLE : USER_DATA_RESOURCE_TABLE;
            p.dwords = 1;
            // Ranges appended to the previous one get their place now.
            u32 next = 0;
            p.range_count = src.range_count;
            for (u32 r = 0; r < src.range_count; r++) {
                p.ranges[r] = src.ranges[r];
                if (p.ranges[r].offset == ~0u)
                    p.ranges[r].offset = next;
                next = p.ranges[r].count == ~0u ? ~0u : p.ranges[r].offset + p.ranges[r].count;
            }
            break;
        }
        case ROOT_CONSTANTS:
            p.kind = USER_DATA_CONSTANTS;
            p.dwords = src.constants;
            break;
        case ROOT_CBV:
            p.kind = USER_DATA_ROOT_CBV;
            p.dwords = 2;
            break;
        case ROOT_SRV:
            p.kind = USER_DATA_ROOT_SRV;
            p.dwords = 2;
            break;
        case ROOT_UAV:
            p.kind = USER_DATA_ROOT_UAV;
            p.dwords = 2;
            break;
        }
        sgpr += p.dwords;
    }
    info->resource_heap_sgpr = NO_HEAP_SGPR;
    info->sampler_heap_sgpr = NO_HEAP_SGPR;
    if (layout.flags & CONSOLE_ROOT_HEAP_RESOURCES)
        info->resource_heap_sgpr = (u8)sgpr++;
    if (layout.flags & CONSOLE_ROOT_HEAP_SAMPLERS)
        info->sampler_heap_sgpr = (u8)sgpr++;
    return sgpr;
}

static u32 read_u32(const u8* p) {
    u32 v;
    memcpy(&v, p, 4);
    return v;
}

usize console_graphics_context_offset(const void* packet, usize packet_size) {
    if (packet_size < 8)
        return 0;
    u32 config = read_u32((const u8*)packet + 4) & 3; // bit 0: tessellation, bit 1: geometry shader
    usize offset = 88;
    if (config & 1)
        offset += 24 + 16;
    if (config & 2)
        offset += 84;
    return offset;
}

#define METADATA_VERSION 4096

bool console_parse_graphics(const void* packet, usize packet_size, const void* metadata, usize metadata_size,
                            ConsoleGraphicsShaders* out) {
    memset(out, 0, sizeof(*out));
    const u8* pk = (const u8*)packet;
    const u8* md = (const u8*)metadata;
    if (packet_size < 160 || metadata_size < 68 || read_u32(md + 4) != METADATA_VERSION)
        return false;
    if (read_u32(pk + 4) & 3)
        return false; // tessellation and geometry shader pipelines lay their blocks out differently
    // Shader register block: RSRC2 of the pixel and vertex shaders hold their user SGPR counts.
    const u8* spi = pk + 44;
    out->ps_user_sgprs = (u8)((read_u32(spi + 16) >> 1) & 31);
    out->vs_user_sgprs = (u8)((read_u32(spi + 36) >> 1) & 31);
    usize context = console_graphics_context_offset(packet, packet_size);
    if (context + 28 > packet_size)
        return false;
    out->ps_input_ena = read_u32(pk + context);
    out->ps_color_format = read_u32(pk + context + 24);

    out->sample_mask = read_u32(md + 28);
    out->primitive_topology_type = md[38] & 0x3F;
    // Stream output, then the input layout (dword aligned).
    u32 so_size = read_u32(md + 60);
    usize at = (64 + (usize)so_size + 3) & ~(usize)3;
    if (at + 4 > metadata_size)
        return false;
    u32 count = read_u32(md + at);
    at += 4;
    if (count > SHADER_MAX_VERTEX_ELEMENTS)
        return false;
    for (u32 i = 0; i < count; i++) {
        if (at + 4 > metadata_size)
            return false;
        u32 name_size = read_u32(md + at);
        if (at + 4 + name_size > metadata_size || name_size == 0 || md[at + 4 + name_size - 1] != 0)
            return false;
        out->element_semantic[i] = (const char*)md + at + 4;
        at += 4 + name_size;
        if (at + 24 > metadata_size)
            return false;
        VertexElement& e = out->elements[i];
        out->element_semantic_index[i] = read_u32(md + at);
        e.format = read_u32(md + at + 4);
        e.slot = (u8)read_u32(md + at + 8);
        e.offset = read_u32(md + at + 12);
        out->element_per_instance[i] = read_u32(md + at + 16) != 0;
        out->element_step_rate[i] = read_u32(md + at + 20);
        at += 24;
    }
    out->element_count = (u8)count;
    return true;
}

// The compute packet: header, threads per group (x | y << 16, then z with launch flags above it),
// COMPUTE_PGM_RSRC1/RSRC2.
bool console_parse_compute(const void* packet, usize packet_size, ConsoleComputeShader* out) {
    memset(out, 0, sizeof(*out));
    const u8* pk = (const u8*)packet;
    if (packet_size < 28)
        return false;
    u32 xy = read_u32(pk + 12);
    u32 z = read_u32(pk + 16);
    out->threads[0] = (u16)(xy & 0xFFFF);
    out->threads[1] = (u16)(xy >> 16);
    out->threads[2] = (u16)z;
    out->wave32 = ((z >> 20) & 1u) != 0;
    u32 rsrc2 = read_u32(pk + 24);
    out->user_sgprs = (u8)((rsrc2 >> 1) & 31);
    out->group_id_mask = (u8)((rsrc2 >> 7) & 7);
    out->thread_id_count = (u8)(((rsrc2 >> 11) & 3) + 1);
    out->lds_bytes = ((rsrc2 >> 15) & 0x1FF) * 512;
    return out->threads[0] && out->threads[1] && out->threads[2];
}
