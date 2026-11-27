// Console ID3D12PipelineState.
//
// Console pipeline states carry console GPU code, translated by gpu/shader_programs, and
// fixed-function state as hardware registers, decoded here into desktop descriptions. Desktop
// pipelines also depend on the formats drawn to, which console pipelines leave to the bound
// targets: they are made on a worker thread when a draw first uses a set of formats, and the draw
// waits for them (and for the programs). A console never skips a draw, and one pass of a multi-pass
// effect left out looks broken where a late frame only looks late.
//
// So that draws seldom wait, the formats each pipeline was drawn with are remembered, in memory and in
// the shader cache's record log, by the pipeline's identity (its programs and fixed-function state).
// A pipeline created with an identity drawn with before, in this run or an earlier one, gets those
// desktop pipelines made in the background once its programs are built. A pipeline equal to one
// still alive (created again, or derived without changes) uses that one's desktop pipelines.
//
// (The formats aren't predicted from the console pipeline itself: its packet holds them only in
// hardware encodings, which don't tell the desktop format of the view bound (sRGB or not, the stand-in
// formats), and titles create tens of thousands of pipelines they never draw with; desktop pipelines
// for all of them would cost minutes of driver compilation and gigabytes of memory.)
//
// Desktop pipelines themselves aren't kept on disk here: drivers keep compiled pipelines in disk
// caches of their own, keyed by the same inputs, so remaking one drawn in an earlier run is quick. A
// pipeline library (ID3D12PipelineLibrary) would duplicate that as one blob, written whole and thrown
// away whenever the driver changes.

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/log.h"
#include "gpu/formats.h"
#include "gpu/gpu.h"
#include "gpu/shader_programs.h"
#include "hle/d3d12/d3d12_objects.h"
#include "hle/d3d12/d3d12_tables.h"
#include "shader/console_container.h"

static_assert(offsetof(ConsolePipelineState, packet) == 0x10, "pipeline state layout");
static_assert(offsetof(ConsolePipelineState, root_signature) == 0x30,
              "pipeline state state follows the public members");

static const GUID* const k_pipeline_state_iids[] = {&__uuidof(ID3D12Object), &__uuidof(ID3D12DeviceChild),
                                                    &__uuidof(ID3D12Pageable),
                                                    &__uuidof(ID3D12PipelineState)};

static void pipeline_state_destroy(void* object);

static const ConsoleClass k_pipeline_state_class = {
    "ID3D12PipelineState", k_pipeline_state_iids, ARRAY_COUNT(k_pipeline_state_iids), pipeline_state_destroy};

static void* const* g_pipeline_state_table = nullptr;
static ID3D12Device* g_device = nullptr;

static void unlist_pipeline(ConsolePipelineState* pso);

static void pipeline_state_destroy(void* object) {
    ConsolePipelineState* pso = (ConsolePipelineState*)object;
    unlist_pipeline(pso);
    if (pso->twin)
        console_release(pso->twin);
    for (u32 i = 0; i < (u32)pso->variant_count; i++)
        gpu_retire(pso->variants[i].desktop);
    if (pso->root_signature)
        console_release(pso->root_signature);
}

static void load_known_variants();

void console_pipeline_state_init() {
    g_pipeline_state_table = console_build_table("ID3D12PipelineState", k_pipeline_state_entries,
                                                 ARRAY_COUNT(k_pipeline_state_entries), nullptr, 0);
    g_device = gpu_device(); // opens the shader cache, which holds the variants drawn before
    load_known_variants();
}

static ConsolePipelineState* new_pipeline_state(bool compute, void* root_signature, const void* packet,
                                                usize packet_size) {
    ConsolePipelineState* pso = (ConsolePipelineState*)console_alloc(
        sizeof(ConsolePipelineState), &k_pipeline_state_class, g_pipeline_state_table, nullptr);
    if (packet)
        memcpy(pso->packet, packet, packet_size < sizeof(pso->packet) ? packet_size : sizeof(pso->packet));
    pso->compute = compute;
    InitializeSRWLock(&pso->lock);
    if (root_signature && console_try_add_ref(root_signature))
        pso->root_signature = (ConsoleRootSignature*)root_signature;
    return pso;
}

// --- fixed-function state from registers -------------------------------------------------------------

// Hardware stencil operations by D3D12_STENCIL_OP (from 1), and blend combine functions by
// D3D12_BLEND_OP (from 1).
static const u8 k_stencil_ops[8] = {0, 1, 3, 5, 6, 7, 8, 9};
static const u8 k_blend_ops[5] = {0, 1, 4, 2, 3};

static D3D12_STENCIL_OP stencil_op(u32 hw) {
    for (u32 i = 0; i < ARRAY_COUNT(k_stencil_ops); i++)
        if (k_stencil_ops[i] == hw)
            return (D3D12_STENCIL_OP)(i + 1);
    LOG_W_ONCE("d3d12", "stencil operation %u has no desktop equivalent; kept as KEEP", hw);
    return D3D12_STENCIL_OP_KEEP;
}

static D3D12_BLEND_OP blend_op(u32 hw) {
    for (u32 i = 0; i < ARRAY_COUNT(k_blend_ops); i++)
        if (k_blend_ops[i] == hw)
            return (D3D12_BLEND_OP)(i + 1);
    return D3D12_BLEND_OP_ADD;
}

// The logic operation by the ROP3 code (the console names copy as 0xCC).
static D3D12_LOGIC_OP logic_op(u32 rop3) {
    switch (rop3) {
    case 0x00:
        return D3D12_LOGIC_OP_CLEAR;
    case 0x11:
        return D3D12_LOGIC_OP_NOR;
    case 0x22:
        return D3D12_LOGIC_OP_AND_INVERTED;
    case 0x33:
        return D3D12_LOGIC_OP_COPY_INVERTED;
    case 0x44:
        return D3D12_LOGIC_OP_AND_REVERSE;
    case 0x55:
        return D3D12_LOGIC_OP_INVERT;
    case 0x66:
        return D3D12_LOGIC_OP_XOR;
    case 0x77:
        return D3D12_LOGIC_OP_NAND;
    case 0x88:
        return D3D12_LOGIC_OP_AND;
    case 0x99:
        return D3D12_LOGIC_OP_EQUIV;
    case 0xAA:
        return D3D12_LOGIC_OP_NOOP;
    case 0xBB:
        return D3D12_LOGIC_OP_OR_INVERTED;
    case 0xDD:
        return D3D12_LOGIC_OP_OR_REVERSE;
    case 0xEE:
        return D3D12_LOGIC_OP_OR;
    case 0xFF:
        return D3D12_LOGIC_OP_SET;
    default:
        return D3D12_LOGIC_OP_COPY;
    }
}

static u32 reg(const u8* context, u32 offset) {
    u32 v;
    memcpy(&v, context + offset, 4);
    return v;
}

static f32 reg_f32(const u8* context, u32 offset) {
    f32 v;
    memcpy(&v, context + offset, 4);
    return v;
}

// Context register block offsets.
#define CTX_COLOR_CONTROL   36 // CB_COLOR_CONTROL (ROP3 in bits 16..23)
#define CTX_CLIP_CONTROL    68 // PA_CL_CLIP_CNTL
#define CTX_MODE_CONTROL    72 // PA_SU_SC_MODE_CNTL
#define CTX_BIAS_CLAMP      84 // polygon offset clamp, scale (x16), offset
#define CTX_BIAS_SLOPE      88
#define CTX_BIAS_OFFSET     92
#define CTX_DEPTH_CONTROL   96  // DB_DEPTH_CONTROL
#define CTX_STENCIL_CONTROL 100 // DB_STENCIL_CONTROL
#define CTX_STENCIL_MASKS   104 // front ref, read mask, write mask, op value; then back
#define CTX_ALPHA_TO_MASK   112
#define CTX_TARGET_MASK     116 // CB_TARGET_MASK: 4 bits per target
#define CTX_BLEND_CONTROL   120 // CB_BLEND0_CONTROL..CB_BLEND7_CONTROL
#define CTX_SIZE            156

static void decode_fixed_state(const u8* ctx, const u8* metadata, PipelineFixedState* f) {
    D3D12_DEPTH_STENCIL_DESC& ds = f->depth_stencil;
    u32 depth = reg(ctx, CTX_DEPTH_CONTROL);
    u32 stencil = reg(ctx, CTX_STENCIL_CONTROL);
    ds.StencilEnable = depth & 1;
    ds.DepthEnable = (depth >> 1) & 1;
    ds.DepthWriteMask = (depth >> 2) & 1 ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    ds.DepthFunc = (D3D12_COMPARISON_FUNC)(((depth >> 4) & 7) + 1);
    ds.StencilReadMask = ctx[CTX_STENCIL_MASKS + 1];
    ds.StencilWriteMask = ctx[CTX_STENCIL_MASKS + 2];
    ds.FrontFace.StencilFailOp = stencil_op(stencil & 0xF);
    ds.FrontFace.StencilPassOp = stencil_op((stencil >> 4) & 0xF);
    ds.FrontFace.StencilDepthFailOp = stencil_op((stencil >> 8) & 0xF);
    ds.FrontFace.StencilFunc = (D3D12_COMPARISON_FUNC)(((depth >> 8) & 7) + 1);
    // Back faces have their own stencil state only when enabled for them (bit 7); otherwise the
    // front state applies to both (vector fills draw both windings with culling off).
    if ((depth >> 7) & 1) {
        ds.BackFace.StencilFailOp = stencil_op((stencil >> 12) & 0xF);
        ds.BackFace.StencilPassOp = stencil_op((stencil >> 16) & 0xF);
        ds.BackFace.StencilDepthFailOp = stencil_op((stencil >> 20) & 0xF);
        ds.BackFace.StencilFunc = (D3D12_COMPARISON_FUNC)(((depth >> 20) & 7) + 1);
    } else {
        ds.BackFace = ds.FrontFace;
    }

    D3D12_RASTERIZER_DESC& rs = f->rasterizer;
    u32 mode = reg(ctx, CTX_MODE_CONTROL);
    rs.FillMode = (D3D12_FILL_MODE)(((mode >> 8) & 7) + 1);
    if (rs.FillMode != D3D12_FILL_MODE_WIREFRAME && rs.FillMode != D3D12_FILL_MODE_SOLID)
        rs.FillMode = D3D12_FILL_MODE_SOLID;
    rs.CullMode = (mode & 3) == 0 ? D3D12_CULL_MODE_NONE
                  : (mode & 1)    ? D3D12_CULL_MODE_FRONT
                                  : D3D12_CULL_MODE_BACK;
    rs.FrontCounterClockwise = ((mode >> 2) & 1) == 0;
    rs.DepthBias = (INT)reg_f32(ctx, CTX_BIAS_OFFSET);
    rs.DepthBiasClamp = reg_f32(ctx, CTX_BIAS_CLAMP);
    rs.SlopeScaledDepthBias = reg_f32(ctx, CTX_BIAS_SLOPE) * 0.0625f;
    rs.DepthClipEnable = ((reg(ctx, CTX_CLIP_CONTROL) >> 26) & 1) == 0;
    u8 flags = metadata[44];
    rs.MultisampleEnable = (flags >> 1) & 1;
    rs.AntialiasedLineEnable = (flags >> 2) & 1;
    rs.ForcedSampleCount = metadata[42] & 0xF;
    rs.ConservativeRaster = (flags >> 3) & 1 ? D3D12_CONSERVATIVE_RASTERIZATION_MODE_ON
                                             : D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

    D3D12_BLEND_DESC& b = f->blend;
    b.AlphaToCoverageEnable = reg(ctx, CTX_ALPHA_TO_MASK) & 1;
    b.IndependentBlendEnable = TRUE;
    D3D12_LOGIC_OP logic = logic_op(ctx[CTX_COLOR_CONTROL + 2]);
    u32 target_mask = reg(ctx, CTX_TARGET_MASK);
    for (u32 i = 0; i < 8; i++) {
        D3D12_RENDER_TARGET_BLEND_DESC& t = b.RenderTarget[i];
        u32 c = reg(ctx, CTX_BLEND_CONTROL + 4 * i);
        t.BlendEnable = (c >> 30) & 1;
        t.SrcBlend = (D3D12_BLEND)((c & 0x1F) + 1);
        t.BlendOp = blend_op((c >> 5) & 7);
        t.DestBlend = (D3D12_BLEND)(((c >> 8) & 0x1F) + 1);
        t.SrcBlendAlpha = (D3D12_BLEND)(((c >> 16) & 0x1F) + 1);
        t.BlendOpAlpha = blend_op((c >> 21) & 7);
        t.DestBlendAlpha = (D3D12_BLEND)(((c >> 24) & 0x1F) + 1);
        // Without separate alpha factors the color ones apply to alpha as well.
        if (!((c >> 29) & 1)) {
            t.SrcBlendAlpha = t.SrcBlend;
            t.DestBlendAlpha = t.DestBlend;
            t.BlendOpAlpha = t.BlendOp;
        }
        t.LogicOpEnable = !((c >> 31) & 1) && logic != D3D12_LOGIC_OP_COPY;
        t.LogicOp = logic;
        t.RenderTargetWriteMask = (UINT8)((target_mask >> (4 * i)) & 0xF);
        if (t.LogicOpEnable)
            t.BlendEnable = FALSE;
    }
    // Dual-source blending is defined for the first target alone.
    D3D12_BLEND src = b.RenderTarget[0].SrcBlend, dst = b.RenderTarget[0].DestBlend;
    if ((src >= D3D12_BLEND_SRC1_COLOR && src <= D3D12_BLEND_INV_SRC1_ALPHA) ||
        (dst >= D3D12_BLEND_SRC1_COLOR && dst <= D3D12_BLEND_INV_SRC1_ALPHA))
        b.IndependentBlendEnable = FALSE;
}

// --- creation ----------------------------------------------------------------------------------------

static void pipeline_created(ConsolePipelineState* pso);

static void request_graphics_programs(ConsolePipelineState* pso, const ConsoleSerializedPipeline* s,
                                      const ConsoleGraphicsShaders& g) {
    const ConsoleRootSignature* rs = pso->root_signature;
    const ConsoleSerializedShader& vs = s->stages[HW_STAGE_VS];
    const ConsoleSerializedShader& ps = s->stages[HW_STAGE_PS];
    if (!rs || !vs.code || !vs.code_size)
        return;
    ShaderInfo info;
    memset(&info, 0, sizeof(info));
    info.stage = SHADER_VERTEX;
    console_user_data_layout(rs->layout, CONSOLE_FIRST_ROOT_SGPR_GRAPHICS, &info);
    info.user_sgprs = g.vs_user_sgprs;
    info.element_count = g.element_count;
    memcpy(info.elements, g.elements, sizeof(info.elements));
    pso->programs[0] = shader_program_get(vs.code, (u32)vs.code_size, info);
    if (ps.code && ps.code_size) {
        memset(&info, 0, sizeof(info));
        info.stage = SHADER_PIXEL;
        console_user_data_layout(rs->layout, CONSOLE_FIRST_ROOT_SGPR_GRAPHICS, &info);
        info.user_sgprs = g.ps_user_sgprs;
        info.ps_input_addr = g.ps_input_ena;
        info.ps_color_format = g.ps_color_format;
        pso->programs[1] = shader_program_get(ps.code, (u32)ps.code_size, info);
    }
    pso->translated = pso->programs[0] != nullptr;
}

HRESULT console_deserialize_pipeline_state(ConsoleDevice*, bool compute, const ConsoleSerializedPipeline* s,
                                           REFIID iid, void** out) {
    if (!s || !s->packet)
        return E_INVALIDARG;
    if (!out)
        return E_POINTER;
    ConsolePipelineState* pso = new_pipeline_state(compute, s->root_signature, s->packet, s->packet_size);
    if (compute) {
        ConsoleComputeShader c;
        const ConsoleSerializedShader& cs = s->stages[0];
        if (pso->root_signature && cs.code && console_parse_compute(s->packet, s->packet_size, &c)) {
            ShaderInfo info;
            memset(&info, 0, sizeof(info));
            info.stage = SHADER_COMPUTE;
            console_user_data_layout(pso->root_signature->layout, CONSOLE_FIRST_ROOT_SGPR_COMPUTE, &info);
            info.user_sgprs = c.user_sgprs;
            memcpy(info.threads, c.threads, sizeof(info.threads));
            info.group_id_mask = c.group_id_mask;
            info.thread_id_count = c.thread_id_count;
            info.lds_bytes = c.lds_bytes;
            info.wave_size = c.wave32 ? 32 : 64;
            pso->programs[0] = shader_program_get(cs.code, (u32)cs.code_size, info);
            pso->translated = pso->programs[0] != nullptr;
            pipeline_created(pso);
        } else {
            LOG_W_ONCE("d3d12",
                       "compute pipeline in an unknown serialized form; dispatches with it are skipped");
        }
        return console_return(pso, iid, out);
    }
    ConsoleGraphicsShaders g;
    usize context = console_graphics_context_offset(s->packet, s->packet_size);
    if (!s->metadata ||
        !console_parse_graphics(s->packet, s->packet_size, s->metadata, s->metadata_size, &g) ||
        context + CTX_SIZE > s->packet_size) {
        LOG_W_ONCE("d3d12", "graphics pipeline in an unknown serialized form; draws with it are skipped");
        return console_return(pso, iid, out);
    }
    PipelineFixedState& f = pso->fixed;
    decode_fixed_state((const u8*)s->packet + context, (const u8*)s->metadata, &f);
    f.sample_mask = g.sample_mask;
    f.topology_type = (D3D12_PRIMITIVE_TOPOLOGY_TYPE)g.primitive_topology_type;
    if (f.topology_type >= D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH) { // no tessellation stages here
        LOG_W_ONCE("d3d12", "pipeline with console primitive topology type %u; drawn as triangles",
                   f.topology_type);
        f.topology_type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    }
    f.strip_cut = (D3D12_INDEX_BUFFER_STRIP_CUT_VALUE)(((const u8*)s->metadata)[39] & 3);
    f.element_count = g.element_count;
    memcpy(f.elements, g.elements, sizeof(f.elements));
    memcpy(f.element_per_instance, g.element_per_instance, sizeof(f.element_per_instance));
    memcpy(f.element_step_rate, g.element_step_rate, sizeof(f.element_step_rate));
    request_graphics_programs(pso, s, g);
    pipeline_created(pso);
    return console_return(pso, iid, out);
}

// --- pipeline state streams --------------------------------------------------------------------------

// Stream subobjects: a type, then its description at its natural alignment; each subobject starts
// pointer-aligned. Types through the mesh shader are desktop's (same descriptions); the console adds
// its own after them, skipped by size.
enum StreamSubobject : u32 {
    STREAM_ROOT_SIGNATURE = 0,
    STREAM_VS = 1,
    STREAM_PS = 2,
    STREAM_DS = 3,
    STREAM_HS = 4,
    STREAM_GS = 5,
    STREAM_CS = 6,
    STREAM_BLEND = 8,
    STREAM_SAMPLE_MASK = 9,
    STREAM_RASTERIZER = 10,
    STREAM_DEPTH_STENCIL = 11,
    STREAM_INPUT_LAYOUT = 12,
    STREAM_STRIP_CUT = 13,
    STREAM_TOPOLOGY_TYPE = 14,
    STREAM_DEPTH_STENCIL1 = 21,
    STREAM_AS = 23,
    STREAM_MS = 24,
};

static const u16 k_subobject_size[30] = {8,  16, 16, 16, 16, 16, 16, 32, 328, 4,  44, 52, 16, 4,  4,
                                         36, 4,  8,  4,  16, 4,  56, 24, 16,  16, 32, 24, 60, 12, 24};
static const u8 k_subobject_align[30] = {8, 8, 8, 8, 8, 8, 8, 8, 4, 4, 4, 4, 8, 4, 4,
                                         4, 4, 4, 4, 8, 4, 4, 8, 8, 8, 4, 4, 4, 4, 4};

struct StreamPipeline {
    ConsoleRootSignature* root_signature;
    D3D12_SHADER_BYTECODE vs, ps;
    D3D12_INPUT_LAYOUT_DESC layout; // the title's, valid while the pipeline is made
    PipelineFixedState fixed;
};

// D3D12's defaults for what a stream leaves out.
static void default_fixed_state(PipelineFixedState* f) {
    memset(f, 0, sizeof(*f));
    for (u32 i = 0; i < 8; i++) {
        D3D12_RENDER_TARGET_BLEND_DESC& rt = f->blend.RenderTarget[i];
        rt.SrcBlend = rt.SrcBlendAlpha = D3D12_BLEND_ONE;
        rt.DestBlend = rt.DestBlendAlpha = D3D12_BLEND_ZERO;
        rt.BlendOp = rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        rt.LogicOp = D3D12_LOGIC_OP_NOOP;
        rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    }
    f->rasterizer.FillMode = D3D12_FILL_MODE_SOLID;
    f->rasterizer.CullMode = D3D12_CULL_MODE_BACK;
    f->rasterizer.DepthClipEnable = TRUE;
    f->depth_stencil.DepthEnable = TRUE;
    f->depth_stencil.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    f->depth_stencil.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    f->depth_stencil.StencilReadMask = f->depth_stencil.StencilWriteMask = 0xFF;
    D3D12_DEPTH_STENCILOP_DESC op = {D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP,
                                     D3D12_COMPARISON_FUNC_ALWAYS};
    f->depth_stencil.FrontFace = f->depth_stencil.BackFace = op;
    f->sample_mask = UINT_MAX;
    f->topology_type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
}

static void input_layout(const D3D12_INPUT_LAYOUT_DESC& layout, PipelineFixedState* f) {
    u32 next[D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT] = {};
    for (u32 i = 0; i < layout.NumElements && f->element_count < SHADER_MAX_VERTEX_ELEMENTS; i++) {
        const D3D12_INPUT_ELEMENT_DESC& e = layout.pInputElementDescs[i];
        u32 slot = e.InputSlot % D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT;
        u32 offset = e.AlignedByteOffset == D3D12_APPEND_ALIGNED_ELEMENT ? next[slot] : e.AlignedByteOffset;
        next[slot] = offset + format_info(e.Format).bytes_per_element;
        u32 n = f->element_count++;
        f->elements[n] = {(u8)slot, offset, (u32)e.Format};
        f->element_per_instance[n] = e.InputSlotClass == D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA;
        f->element_step_rate[n] = e.InstanceDataStepRate;
    }
}

// Reads the stream; false (with the reason) for what isn't translated.
static bool read_stream(const D3D12_PIPELINE_STATE_STREAM_DESC* desc, StreamPipeline* out, const char** why) {
    memset(out, 0, sizeof(*out));
    default_fixed_state(&out->fixed);
    const u8* base = (const u8*)desc->pPipelineStateSubobjectStream;
    usize at = 0;
    while (at + 4 <= desc->SizeInBytes) {
        u32 type;
        memcpy(&type, base + at, 4);
        if (type >= ARRAY_COUNT(k_subobject_size)) {
            *why = "a subobject of an unknown type";
            return false;
        }
        usize inner = ALIGN_UP(at + 4, (usize)k_subobject_align[type]);
        if (inner + k_subobject_size[type] > desc->SizeInBytes) {
            *why = "a truncated subobject";
            return false;
        }
        const void* p = base + inner;
        switch (type) {
        case STREAM_ROOT_SIGNATURE:
            memcpy(&out->root_signature, p, sizeof(void*));
            break;
        case STREAM_VS:
            memcpy(&out->vs, p, sizeof(out->vs));
            break;
        case STREAM_PS:
            memcpy(&out->ps, p, sizeof(out->ps));
            break;
        case STREAM_DS:
        case STREAM_HS:
        case STREAM_GS:
        case STREAM_CS:
        case STREAM_AS:
        case STREAM_MS: {
            D3D12_SHADER_BYTECODE b;
            memcpy(&b, p, sizeof(b));
            if (b.BytecodeLength) {
                *why = "tessellation, geometry, mesh or compute stages";
                return false;
            }
            break;
        }
        case STREAM_BLEND:
            memcpy(&out->fixed.blend, p, sizeof(out->fixed.blend));
            break;
        case STREAM_SAMPLE_MASK:
            memcpy(&out->fixed.sample_mask, p, 4);
            break;
        case STREAM_RASTERIZER:
            memcpy(&out->fixed.rasterizer, p, sizeof(out->fixed.rasterizer));
            break;
        case STREAM_DEPTH_STENCIL:
            memcpy(&out->fixed.depth_stencil, p, sizeof(out->fixed.depth_stencil));
            break;
        case STREAM_DEPTH_STENCIL1: // the same description with depth bounds after it
            memcpy(&out->fixed.depth_stencil, p, sizeof(out->fixed.depth_stencil));
            break;
        case STREAM_INPUT_LAYOUT: {
            D3D12_INPUT_LAYOUT_DESC layout;
            memcpy(&layout, p, sizeof(layout));
            input_layout(layout, &out->fixed);
            out->layout = layout;
            break;
        }
        case STREAM_STRIP_CUT:
            memcpy(&out->fixed.strip_cut, p, 4);
            break;
        case STREAM_TOPOLOGY_TYPE:
            memcpy(&out->fixed.topology_type, p, 4);
            break;
        default:
            break; // formats, sample description, flags and console extensions: not needed here
        }
        at = ALIGN_UP(inner + k_subobject_size[type], (usize)8);
    }
    if (!out->vs.pShaderBytecode || !out->vs.BytecodeLength) {
        *why = "no vertex shader";
        return false;
    }
    return true;
}

// Container vertex shaders leave loading their inputs to the driver's fetch subroutine, which loads
// each input layout element the shader has an input for (matched by semantic) into that input's
// registers.
static void fetch_layout(const D3D12_INPUT_LAYOUT_DESC& layout, const ConsoleContainerShader& vs,
                         ShaderInfo* info) {
    for (u32 i = 0; i < layout.NumElements && i < SHADER_MAX_VERTEX_ELEMENTS; i++) {
        const D3D12_INPUT_ELEMENT_DESC& e = layout.pInputElementDescs[i];
        for (u32 k = 0; e.SemanticName && k < vs.input_count; k++) {
            const ContainerInput& in = vs.inputs[k];
            if (in.semantic_index != e.SemanticIndex || _stricmp(in.semantic, e.SemanticName) != 0)
                continue;
            u32 vgpr = CONTAINER_FETCH_FIRST_VGPR + 4u * in.reg;
            if (in.mask && vgpr + 4 <= 256) {
                info->fetch_vgpr[i] = (u8)vgpr;
                info->fetch_components[i] = in.mask & 8 ? 4 : in.mask & 4 ? 3 : in.mask & 2 ? 2 : 1;
            }
            break;
        }
    }
}

// Programs from the stream's shader containers.
static bool request_stream_programs(ConsolePipelineState* pso, const StreamPipeline& sp, const char** why) {
    const ConsoleRootSignature* rs = pso->root_signature;
    if (!rs) {
        *why = "no root signature";
        return false;
    }
    ConsoleContainerShader vs, ps;
    if (!console_container_open(sp.vs.pShaderBytecode, sp.vs.BytecodeLength, SHADER_VERTEX, &vs, why))
        return false;
    ShaderInfo info;
    memset(&info, 0, sizeof(info));
    info.stage = SHADER_VERTEX;
    console_user_data_layout(rs->layout, CONSOLE_FIRST_ROOT_SGPR_GRAPHICS, &info);
    info.user_sgprs = vs.user_sgprs;
    info.element_count = pso->fixed.element_count;
    memcpy(info.elements, pso->fixed.elements, sizeof(info.elements));
    fetch_layout(sp.layout, vs, &info);
    pso->programs[0] = shader_program_get(vs.code, vs.code_size, info);
    console_container_close(&vs);
    if (sp.ps.pShaderBytecode && sp.ps.BytecodeLength) {
        if (!console_container_open(sp.ps.pShaderBytecode, sp.ps.BytecodeLength, SHADER_PIXEL, &ps, why))
            return false;
        memset(&info, 0, sizeof(info));
        info.stage = SHADER_PIXEL;
        console_user_data_layout(rs->layout, CONSOLE_FIRST_ROOT_SGPR_GRAPHICS, &info);
        info.user_sgprs = ps.user_sgprs;
        info.ps_input_addr = ps.ps_input_addr;
        info.ps_color_format = ps.ps_color_format;
        pso->programs[1] = shader_program_get(ps.code, ps.code_size, info);
        console_container_close(&ps);
    }
    return pso->programs[0] != nullptr;
}

HRESULT console_create_pipeline_state(ConsoleDevice*, const D3D12_PIPELINE_STATE_STREAM_DESC* desc,
                                      REFIID iid, void** out) {
    if (!desc || !desc->pPipelineStateSubobjectStream)
        return E_INVALIDARG;
    if (!out)
        return E_POINTER;
    StreamPipeline sp;
    const char* why = nullptr;
    bool readable = read_stream(desc, &sp, &why);
    ConsolePipelineState* pso = new_pipeline_state(false, readable ? sp.root_signature : nullptr, nullptr, 0);
    if (readable) {
        pso->fixed = sp.fixed;
        if (pso->fixed.topology_type >= D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH ||
            pso->fixed.topology_type == D3D12_PRIMITIVE_TOPOLOGY_TYPE_UNDEFINED)
            pso->fixed.topology_type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pso->translated = request_stream_programs(pso, sp, &why);
        pipeline_created(pso);
    }
    if (!pso->translated)
        LOG_W("d3d12",
              "pipeline state %p from a state stream is not translated (%s); draws with it are skipped",
              (void*)pso, why ? why : "unknown");
    return console_return(pso, iid, out);
}

HRESULT console_create_compute_pipeline_state(ConsoleDevice*, const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc,
                                              REFIID iid, void** out) {
    if (!desc)
        return E_INVALIDARG;
    if (!out)
        return E_POINTER;
    LOG_W_ONCE("d3d12",
               "compute pipeline states from shader containers are not translated yet; dispatches with "
               "them are skipped");
    return console_return(new_pipeline_state(true, desc->pRootSignature, nullptr, 0), iid, out);
}

// Console derived state descriptions: a type, then the description it replaces.
enum DerivedStateType : u32 {
    DERIVED_DEPTH_STENCIL,
    DERIVED_RASTERIZER,
    DERIVED_BLEND,
    DERIVED_SAMPLE_MASK,
    DERIVED_PIXEL_SHADER_OPTIONS,
    DERIVED_DEPTH_STENCIL_OPTIONS,
    DERIVED_SHADER_LIMITS,
    DERIVED_TESSELLATION_PARAMETERS,
};

struct DerivedState {
    DerivedStateType type;
    union {
        D3D12_DEPTH_STENCIL_DESC depth_stencil;
        D3D12_RASTERIZER_DESC rasterizer;
        D3D12_BLEND_DESC blend;
        UINT sample_mask;
    };
};

HRESULT console_derive_pipeline_state(ConsoleDevice*, void* source, u32 count, const void* descs, REFIID iid,
                                      void** out) {
    const ConsolePipelineState* src = (const ConsolePipelineState*)source;
    if (!src || (count && !descs))
        return E_INVALIDARG;
    if (!out)
        return E_POINTER;
    ConsolePipelineState* pso =
        new_pipeline_state(src->compute, src->root_signature, src->packet, sizeof(src->packet));
    pso->translated = src->translated;
    pso->programs[0] = src->programs[0];
    pso->programs[1] = src->programs[1];
    pso->fixed = src->fixed;
    if (!src->compute) {
        const DerivedState* d = (const DerivedState*)descs;
        for (u32 i = 0; i < count; i++) {
            switch (d[i].type) {
            case DERIVED_DEPTH_STENCIL:
                pso->fixed.depth_stencil = d[i].depth_stencil;
                break;
            case DERIVED_RASTERIZER:
                pso->fixed.rasterizer = d[i].rasterizer;
                break;
            case DERIVED_BLEND:
                pso->fixed.blend = d[i].blend;
                break;
            case DERIVED_SAMPLE_MASK:
                pso->fixed.sample_mask = d[i].sample_mask;
                break;
            default:
                break; // console options and limits: nothing on the desktop
            }
        }
    }
    pipeline_created(pso);
    return console_return(pso, iid, out);
}

// --- desktop pipelines -------------------------------------------------------------------------------

static bool programs_ready(const ConsolePipelineState* pso) {
    for (u32 i = 0; i < 2; i++)
        if (pso->programs[i] && pso->programs[i]->state != PROGRAM_READY)
            return false;
    return true;
}

static bool programs_failed(const ConsolePipelineState* pso) {
    for (u32 i = 0; i < 2; i++)
        if (pso->programs[i] && pso->programs[i]->state == PROGRAM_FAILED)
            return true;
    return false;
}

static bool desktop_signature_exists(const ConsolePipelineState* pso) {
    return pso->translated && pso->root_signature && pso->root_signature->desktop;
}

void console_pipeline_report_skip(ConsolePipelineState* pso, bool graphics) {
    const char* what = graphics ? "draws" : "dispatches";
    if (!pso) {
        LOG_W_ONCE("d3d12", "%s without a pipeline state skipped", what);
        return;
    }
    if (pso->skip_reported)
        return;
    char reason[160];
    if (!pso->translated) {
        snprintf(reason, sizeof(reason), "it is not translated (see the creation warning)");
    } else if (!pso->root_signature || !pso->root_signature->desktop) {
        snprintf(reason, sizeof(reason), "its root signature has no desktop signature");
    } else if (programs_failed(pso)) {
        const ShaderProgram* a = pso->programs[0];
        const ShaderProgram* b = pso->programs[1];
        snprintf(reason, sizeof(reason), "a program failed: %016llx %s, %016llx %s",
                 a ? (unsigned long long)a->key : 0ull, a && a->state == PROGRAM_FAILED ? "failed" : "ok",
                 b ? (unsigned long long)b->key : 0ull, b && b->state == PROGRAM_FAILED ? "failed" : "ok");
    } else {
        snprintf(reason, sizeof(reason), "its desktop pipeline could not be made (see above)");
    }
    if (InterlockedExchange(&pso->skip_reported, 1) == 0)
        LOG_W("d3d12", "%s with pipeline %p skipped: %s", what, (void*)pso, reason);
}

static ID3D12PipelineState* create_graphics(const ConsolePipelineState* pso, const PipelineOutputs& o,
                                            ID3D12RootSignature* signature) {
    const PipelineFixedState& f = pso->fixed;
    const ShaderBinary& vs = pso->programs[0]->binary;
    const ShaderBinary* ps = pso->programs[1] ? &pso->programs[1]->binary : nullptr;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
    d.pRootSignature = signature;
    d.VS = {vs.dxil, vs.dxil_size};
    if (ps)
        d.PS = {ps->dxil, ps->dxil_size};
    if (o.rect_list) {
        const D3D12_SHADER_BYTECODE* gs = shader_rect_list_gs(vs.vs_param_mask);
        if (!gs)
            return nullptr;
        d.GS = *gs;
    }
    d.BlendState = f.blend;
    d.SampleMask = f.sample_mask;
    d.RasterizerState = f.rasterizer;
    d.DepthStencilState = f.depth_stencil;
    if (o.depth_format == DXGI_FORMAT_UNKNOWN) {
        d.DepthStencilState.DepthEnable = FALSE;
        d.DepthStencilState.StencilEnable = FALSE;
    }
    // Input elements the vertex shader reads, by element index; raw fetches read dwords.
    D3D12_INPUT_ELEMENT_DESC elements[SHADER_MAX_VERTEX_ELEMENTS];
    u32 element_count = 0;
    for (u32 i = 0; i < f.element_count; i++) {
        if (!(vs.vertex_element_mask & (1u << i)))
            continue;
        D3D12_INPUT_ELEMENT_DESC& e = elements[element_count++];
        e.SemanticName = "E";
        e.SemanticIndex = i;
        e.Format = (DXGI_FORMAT)vs.element_format[i];
        e.InputSlot = f.elements[i].slot;
        e.AlignedByteOffset = f.elements[i].offset;
        e.InputSlotClass = f.element_per_instance[i] ? D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA
                                                     : D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
        e.InstanceDataStepRate = f.element_per_instance[i] ? f.element_step_rate[i] : 0;
    }
    d.InputLayout = {elements, element_count};
    d.IBStripCutValue = f.strip_cut;
    d.PrimitiveTopologyType = f.topology_type ? f.topology_type : D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    d.NumRenderTargets = o.render_target_count;
    for (u32 i = 0; i < 8; i++)
        d.RTVFormats[i] = (DXGI_FORMAT)o.render_target_formats[i];
    d.DSVFormat = (DXGI_FORMAT)o.depth_format;
    d.SampleDesc.Count = o.sample_count ? o.sample_count : 1;
    ID3D12PipelineState* desktop = nullptr;
    HRESULT hr = g_device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&desktop));
    if (FAILED(hr))
        LOG_W("d3d12", "creating a translated graphics pipeline failed (0x%08lx): vs %016llx ps %016llx",
              (unsigned long)hr, (unsigned long long)pso->programs[0]->key,
              pso->programs[1] ? (unsigned long long)pso->programs[1]->key : 0ull);
    return desktop;
}

static ID3D12PipelineState* create_compute(const ConsolePipelineState* pso, ID3D12RootSignature* signature) {
    const ShaderBinary& cs = pso->programs[0]->binary;
    D3D12_COMPUTE_PIPELINE_STATE_DESC d = {};
    d.pRootSignature = signature;
    d.CS = {cs.dxil, cs.dxil_size};
    ID3D12PipelineState* desktop = nullptr;
    HRESULT hr = g_device->CreateComputePipelineState(&d, IID_PPV_ARGS(&desktop));
    if (FAILED(hr))
        LOG_W("d3d12", "creating a translated compute pipeline failed (0x%08lx): cs %016llx",
              (unsigned long)hr, (unsigned long long)pso->programs[0]->key);
    return desktop;
}

// What a variant is keyed by: the formats drawn to. A pipeline that neither tests nor writes depth or
// stencil works with any depth target bound; desktop wants no depth format in it then (and its
// variants don't differ by depth target). Programs must be ready.
static PipelineOutputs variant_key(const ConsolePipelineState* pso, const PipelineOutputs* outputs) {
    PipelineOutputs key;
    memset(&key, 0, sizeof(key));
    if (!pso->compute && outputs)
        key = *outputs;
    const ShaderProgram* ps = pso->programs[1];
    if (!pso->fixed.depth_stencil.DepthEnable && !pso->fixed.depth_stencil.StencilEnable &&
        !(ps && ps->binary.ps_writes_depth))
        key.depth_format = DXGI_FORMAT_UNKNOWN;
    return key;
}

static PipelineVariant* find_variant(ConsolePipelineState* pso, const PipelineOutputs& key) {
    long count = pso->variant_count;
    for (long i = 0; i < count; i++)
        if (memcmp(&pso->variants[i].outputs, &key, sizeof(key)) == 0)
            return &pso->variants[i];
    return nullptr;
}

// The variant for `key`: an existing one, or a new one being built (`*added`: the caller builds it,
// `builder` being the background worker that does, or 0). nullptr (logged once) when the pipeline has
// no room for more.
static PipelineVariant* add_variant(ConsolePipelineState* pso, const PipelineOutputs& key, u32 builder,
                                    bool* added) {
    *added = false;
    AcquireSRWLockExclusive(&pso->lock);
    PipelineVariant* v = find_variant(pso, key);
    if (!v && pso->variant_count < PIPELINE_MAX_VARIANTS) {
        v = &pso->variants[pso->variant_count];
        v->outputs = key;
        v->state = VARIANT_BUILDING;
        v->builder = builder;
        v->desktop = nullptr;
        v->signature = nullptr;
        InterlockedIncrement(&pso->variant_count); // publishes it, filled in
        *added = true;
    }
    ReleaseSRWLockExclusive(&pso->lock);
    if (!v)
        LOG_W_ONCE("d3d12", "a pipeline is drawn with more than %d sets of formats; draws skipped",
                   PIPELINE_MAX_VARIANTS);
    return v;
}

static void finish_variant(PipelineVariant* v, ID3D12PipelineState* desktop, ID3D12RootSignature* signature) {
    v->signature = signature;
    v->desktop = desktop;
    InterlockedExchange((volatile LONG*)&v->state, desktop ? VARIANT_READY : VARIANT_FAILED);
    WakeByAddressAll((void*)&v->state);
}

// Builds an added variant. Runs on worker threads; the programs are ready.
static void build_variant(ConsolePipelineState* pso, PipelineVariant* v) {
    // The programs' inline samplers go to the root signature, each stage's for that stage.
    SamplerSet sets[2];
    u32 set_count = 0;
    for (u32 i = 0; i < 2; i++) {
        const ShaderProgram* p = pso->programs[i];
        if (p && p->binary.sampler_count)
            sets[set_count++] = {p->binary.samplers, p->binary.sampler_count,
                                 pso->compute ? D3D12_SHADER_VISIBILITY_ALL
                                 : i == 0     ? D3D12_SHADER_VISIBILITY_VERTEX
                                              : D3D12_SHADER_VISIBILITY_PIXEL};
    }
    ID3D12RootSignature* signature = console_root_signature_desktop(pso->root_signature, sets, set_count);
    ID3D12PipelineState* desktop = nullptr;
    if (signature)
        desktop = pso->compute ? create_compute(pso, signature) : create_graphics(pso, v->outputs, signature);
    finish_variant(v, desktop, signature);
}

struct VariantJob {
    ConsolePipelineState* pso; // referenced for the job
    PipelineVariant* variant;
};

static void variant_job(void* context) {
    VariantJob* job = (VariantJob*)context;
    build_variant(job->pso, job->variant);
    console_release(job->pso);
    free(job);
}

static bool wait_variant(PipelineVariant* v) {
    // One built ahead is made at background priority (urgent jobs run at normal priority already).
    if (v->builder)
        gpu_job_hurry(v->builder);
    for (;;) {
        u32 state = v->state;
        if (state != VARIANT_BUILDING)
            return state == VARIANT_READY;
        WaitOnAddress(&v->state, &state, sizeof(state), INFINITE);
    }
}

static void remember_variant(const ConsolePipelineState* pso, const PipelineOutputs& key);

// A new variant a draw needs, built on a worker ahead of other work.
static PipelineVariant* request_variant(ConsolePipelineState* pso, const PipelineOutputs& key) {
    bool added;
    PipelineVariant* v = add_variant(pso, key, 0, &added);
    if (!added)
        return v;
    VariantJob* job = (VariantJob*)malloc(sizeof(VariantJob));
    if (!job || !console_try_add_ref(pso)) {
        free(job);
        finish_variant(v, nullptr, nullptr);
        return v;
    }
    job->pso = pso;
    job->variant = v;
    if (!gpu_job_queue(variant_job, job)) {
        console_release(pso);
        free(job);
        finish_variant(v, nullptr, nullptr);
        return v;
    }
    remember_variant(pso, key);
    return v;
}

// Waits worth knowing about are logged, each once: a pipeline waits at most once per set of formats.
static void report_wait(const ConsolePipelineState* pso, const LARGE_INTEGER& start) {
    LARGE_INTEGER now, frequency;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    f64 ms = (f64)(now.QuadPart - start.QuadPart) * 1000.0 / (f64)frequency.QuadPart;
    const ShaderProgram* a = pso->programs[0];
    const ShaderProgram* b = pso->programs[1];
    if (ms >= 50.0)
        LOG_I("d3d12", "a %s waited %.0f ms for pipeline %p (programs %016llx %016llx) to be made",
              pso->compute ? "dispatch" : "draw", ms, (const void*)pso, a ? (unsigned long long)a->key : 0ull,
              b ? (unsigned long long)b->key : 0ull);
    else
        LOG_D("d3d12", "a %s waited %.1f ms for pipeline %p to be made", pso->compute ? "dispatch" : "draw",
              ms, (const void*)pso);
}

ID3D12PipelineState* console_pipeline_desktop(ConsolePipelineState* pso, const PipelineOutputs* outputs,
                                              ID3D12RootSignature** signature) {
    if (pso && pso->twin)
        pso = pso->twin;
    if (!pso || !desktop_signature_exists(pso))
        return nullptr;
    // What isn't made yet is waited for: the draw is late, never left out.
    LARGE_INTEGER wait_start = {};
    if (!programs_ready(pso)) {
        QueryPerformanceCounter(&wait_start);
        for (u32 i = 0; i < 2; i++)
            if (pso->programs[i] && !shader_program_wait(pso->programs[i]))
                return nullptr;
    }
    PipelineOutputs key = variant_key(pso, outputs);
    PipelineVariant* v = find_variant(pso, key);
    if (!v && !(v = request_variant(pso, key)))
        return nullptr;
    if (v->state == VARIANT_BUILDING) {
        if (!wait_start.QuadPart)
            QueryPerformanceCounter(&wait_start);
        wait_variant(v);
    }
    if (wait_start.QuadPart)
        report_wait(pso, wait_start);
    if (v->state != VARIANT_READY)
        return nullptr;
    *signature = v->signature;
    return v->desktop;
}

// --- desktop pipelines drawn before ------------------------------------------------------------------

// Every variant a draw adds is remembered by its pipeline's identity (in memory and in the shader
// cache's record log); a pipeline created later with that identity gets the variant built ahead.
#define KNOWN_CAPACITY     65536 // power of two; filled to half at most
#define KNOWN_PER_PIPELINE 4     // built ahead at most (the rest of the variant slots stay for draws)

static_assert(sizeof(PipelineOutputs) <= CACHE_RECORD_DATA_BYTES, "variants fit cache records");

struct KnownVariant {
    u64 identity; // 0: free
    PipelineOutputs outputs;
};

static KnownVariant* g_known;
static u32 g_known_count;
static SRWLOCK g_known_lock = SRWLOCK_INIT;

static u64 hash_u64(u64 h, u64 v) {
    for (u32 i = 0; i < 8; i++) {
        h ^= (v >> (8 * i)) & 0xFF;
        h *= 0x100000001b3ull;
    }
    return h;
}

// Under g_known_lock. False if known already or the table is full.
static bool known_add(u64 identity, const PipelineOutputs& outputs) {
    if (!g_known)
        return false;
    u32 slot = (u32)identity & (KNOWN_CAPACITY - 1);
    for (; g_known[slot].identity; slot = (slot + 1) & (KNOWN_CAPACITY - 1))
        if (g_known[slot].identity == identity &&
            memcmp(&g_known[slot].outputs, &outputs, sizeof(outputs)) == 0)
            return false;
    if (g_known_count >= KNOWN_CAPACITY / 2) {
        LOG_W_ONCE("d3d12", "more than %d desktop pipelines drawn; more aren't remembered",
                   KNOWN_CAPACITY / 2);
        return false;
    }
    g_known[slot].identity = identity;
    g_known[slot].outputs = outputs;
    g_known_count++;
    return true;
}

static u32 known_find(u64 identity, PipelineOutputs* out) {
    u32 count = 0;
    AcquireSRWLockShared(&g_known_lock);
    if (g_known) {
        u32 slot = (u32)identity & (KNOWN_CAPACITY - 1);
        for (; g_known[slot].identity && count < KNOWN_PER_PIPELINE; slot = (slot + 1) & (KNOWN_CAPACITY - 1))
            if (g_known[slot].identity == identity)
                out[count++] = g_known[slot].outputs;
    }
    ReleaseSRWLockShared(&g_known_lock);
    return count;
}

static void load_known_variants() {
    g_known = (KnownVariant*)calloc(KNOWN_CAPACITY, sizeof(KnownVariant));
    CacheRecord* records = (CacheRecord*)malloc(KNOWN_CAPACITY / 2 * sizeof(CacheRecord));
    if (!g_known || !records) {
        free(records);
        return;
    }
    u32 count = shader_cache_read_records(records, KNOWN_CAPACITY / 2);
    u32 known = 0;
    AcquireSRWLockExclusive(&g_known_lock);
    for (u32 i = 0; i < count; i++) {
        PipelineOutputs outputs;
        memcpy(&outputs, records[i].data, sizeof(outputs));
        known += records[i].id && known_add(records[i].id, outputs);
    }
    ReleaseSRWLockExclusive(&g_known_lock);
    free(records);
    if (known)
        LOG_I("d3d12", "%u desktop pipelines drawn in earlier runs are built ahead when titles create them",
              known);
}

static void append_record_job(void* context) {
    CacheRecord* record = (CacheRecord*)context;
    shader_cache_append_record(*record);
    free(record);
}

static void remember_variant(const ConsolePipelineState* pso, const PipelineOutputs& key) {
    if (!pso->identity)
        return;
    AcquireSRWLockExclusive(&g_known_lock);
    bool added = known_add(pso->identity, key);
    ReleaseSRWLockExclusive(&g_known_lock);
    if (!added)
        return;
    // To the log from a worker: the write may block, and this is a thread recording a command list.
    CacheRecord* record = (CacheRecord*)calloc(1, sizeof(CacheRecord));
    if (!record)
        return;
    record->id = pso->identity;
    memcpy(record->data, &key, sizeof(key));
    if (!gpu_job_queue_background(append_record_job, record))
        free(record);
}

// Field by field: the descriptions have padding, which titles leave as it was on their side.
static u64 hash_fixed_state(const PipelineFixedState& f, u64 h) {
    const D3D12_BLEND_DESC& b = f.blend;
    h = hash_u64(h, b.AlphaToCoverageEnable);
    h = hash_u64(h, b.IndependentBlendEnable);
    for (u32 i = 0; i < 8; i++) {
        const D3D12_RENDER_TARGET_BLEND_DESC& t = b.RenderTarget[i];
        h = hash_u64(h, t.BlendEnable | (u64)t.LogicOpEnable << 1 | (u64)t.RenderTargetWriteMask << 2);
        h = hash_u64(h, t.SrcBlend | (u64)t.DestBlend << 8 | (u64)t.BlendOp << 16 |
                            (u64)t.SrcBlendAlpha << 24 | (u64)t.DestBlendAlpha << 32 |
                            (u64)t.BlendOpAlpha << 40 | (u64)t.LogicOp << 48);
    }
    const D3D12_RASTERIZER_DESC& r = f.rasterizer;
    h = hash_u64(h, r.FillMode | (u64)r.CullMode << 8 | (u64)r.FrontCounterClockwise << 16 |
                        (u64)r.DepthClipEnable << 17 | (u64)r.MultisampleEnable << 18 |
                        (u64)r.AntialiasedLineEnable << 19 | (u64)r.ConservativeRaster << 20 |
                        (u64)r.ForcedSampleCount << 32);
    u32 bits;
    h = hash_u64(h, (u32)r.DepthBias);
    memcpy(&bits, &r.DepthBiasClamp, 4);
    h = hash_u64(h, bits);
    memcpy(&bits, &r.SlopeScaledDepthBias, 4);
    h = hash_u64(h, bits);
    const D3D12_DEPTH_STENCIL_DESC& d = f.depth_stencil;
    h = hash_u64(h, d.DepthEnable | (u64)d.DepthWriteMask << 1 | (u64)d.DepthFunc << 8 |
                        (u64)d.StencilEnable << 16 | (u64)d.StencilReadMask << 24 |
                        (u64)d.StencilWriteMask << 32);
    const D3D12_DEPTH_STENCILOP_DESC* faces[2] = {&d.FrontFace, &d.BackFace};
    for (const D3D12_DEPTH_STENCILOP_DESC* s : faces)
        h = hash_u64(h, s->StencilFailOp | (u64)s->StencilDepthFailOp << 8 | (u64)s->StencilPassOp << 16 |
                            (u64)s->StencilFunc << 24);
    h = hash_u64(h, f.sample_mask);
    h = hash_u64(h, f.topology_type | (u64)f.strip_cut << 8 | (u64)f.element_count << 16);
    for (u32 i = 0; i < f.element_count && i < SHADER_MAX_VERTEX_ELEMENTS; i++) {
        const VertexElement& e = f.elements[i];
        h = hash_u64(h, e.slot | (u64)f.element_per_instance[i] << 8 | (u64)e.format << 16);
        h = hash_u64(h, e.offset | (u64)f.element_step_rate[i] << 32);
    }
    return h;
}

static u64 pipeline_identity(const ConsolePipelineState* pso) {
    u64 h = hash_u64(0xcbf29ce484222325ull, pso->compute);
    for (u32 i = 0; i < 2; i++)
        h = hash_u64(h, pso->programs[i] ? pso->programs[i]->key : 0);
    if (!pso->compute)
        h = hash_fixed_state(pso->fixed, h);
    return h ? h : 1;
}

struct PrepareJob {
    ConsolePipelineState* pso; // referenced for the job
    u32 count;
    PipelineOutputs outputs[KNOWN_PER_PIPELINE];
};

static void prepare_job(void* context) {
    PrepareJob* job = (PrepareJob*)context;
    ConsolePipelineState* pso = job->pso;
    // The programs were queued before this job, in the same lane, so workers have started them (or
    // finished). One still pending was requested first by another thread that hadn't queued it yet:
    // the job goes to the back of the lane rather than wait for work it might be holding up.
    for (u32 i = 0; i < 2; i++) {
        if (pso->programs[i] && !shader_program_wait_started(pso->programs[i])) {
            if (gpu_job_queue_background(prepare_job, job))
                return;
            break; // queue full: draws make the variants
        }
    }
    if (programs_ready(pso) && desktop_signature_exists(pso)) {
        u32 worker = gpu_job_worker();
        for (u32 i = 0; i < job->count; i++) {
            bool added;
            PipelineVariant* v = add_variant(pso, job->outputs[i], worker, &added);
            if (added)
                build_variant(pso, v);
        }
    }
    console_release(pso);
    free(job);
}

// Titles create the same pipeline several times over (deserialized again, derived without changes). A
// pipeline equal to a live one uses that one's desktop pipelines: one set to make, and what a draw
// with one waited for is ready for draws with the others.
#define LISTED_BUCKETS 16384 // power of two

static ConsolePipelineState* g_listed[LISTED_BUCKETS];
static SRWLOCK g_listed_lock = SRWLOCK_INIT;

// The live pipeline equal to `pso`, referenced; or nullptr, and `pso` is listed for later ones.
static ConsolePipelineState* find_twin_or_list(ConsolePipelineState* pso) {
    ConsolePipelineState** bucket = &g_listed[pso->identity & (LISTED_BUCKETS - 1)];
    AcquireSRWLockExclusive(&g_listed_lock);
    for (ConsolePipelineState* p = *bucket; p; p = p->next_listed) {
        // The root signatures are the same object, kept alive by both. A pipeline being destroyed
        // (no references left) is still listed until its destruction takes the lock.
        if (p->identity == pso->identity && p->root_signature == pso->root_signature &&
            p->compute == pso->compute && console_try_add_ref(p)) {
            ReleaseSRWLockExclusive(&g_listed_lock);
            return p;
        }
    }
    pso->next_listed = *bucket;
    pso->listed = true;
    *bucket = pso;
    ReleaseSRWLockExclusive(&g_listed_lock);
    return nullptr;
}

static void unlist_pipeline(ConsolePipelineState* pso) {
    if (!pso->listed)
        return;
    AcquireSRWLockExclusive(&g_listed_lock);
    ConsolePipelineState** link = &g_listed[pso->identity & (LISTED_BUCKETS - 1)];
    while (*link && *link != pso)
        link = &(*link)->next_listed;
    if (*link)
        *link = pso->next_listed;
    ReleaseSRWLockExclusive(&g_listed_lock);
}

// A new pipeline: its identity, then the twin it uses, or the variants drawn with that identity
// before, queued for building behind its programs.
static void pipeline_created(ConsolePipelineState* pso) {
    if (!pso->translated)
        return;
    pso->identity = pipeline_identity(pso);
    pso->twin = find_twin_or_list(pso);
    if (pso->twin)
        return;
    PrepareJob found;
    found.count = known_find(pso->identity, found.outputs);
    if (!found.count)
        return;
    PrepareJob* job = (PrepareJob*)malloc(sizeof(PrepareJob));
    if (!job || !console_try_add_ref(pso)) {
        free(job);
        return;
    }
    *job = found;
    job->pso = pso;
    if (!gpu_job_queue_background(prepare_job, job)) {
        console_release(pso);
        free(job);
    }
}
