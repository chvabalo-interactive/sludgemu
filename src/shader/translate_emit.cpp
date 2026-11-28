// Console shader translation: the HLSL program around the instructions, stage inputs and outputs,
// control flow.

#include <stdio.h>
#include <string.h>

#include "shader/translate_internal.h"

// --- helpers -----------------------------------------------------------------------------------------

const char* fmt(Translator& t, const char* format, ...) {
    char* out = t.ring[t.ring_at++ % 32];
    va_list args;
    va_start(args, format);
    int n = vsnprintf(out, sizeof(t.ring[0]), format, args);
    va_end(args);
    if (n < 0 || n >= (int)sizeof(t.ring[0]))
        translator_fail(t, "expression too long");
    return out;
}

void emit(Translator& t, const char* format, ...) {
    char line[1024];
    va_list args;
    va_start(args, format);
    int n = vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    if (n < 0 || n >= (int)sizeof(line))
        translator_fail(t, "statement too long");
    t.body.line(t.depth, "%s", line);
}

void use_sgpr(Translator& t, u32 reg) {
    t.sgpr_used[reg & 127] = 1;
}

void use_vgpr(Translator& t, u32 reg) {
    t.vgpr_used[reg & 255] = 1;
}

const char* sgpr(Translator& t, u32 reg) {
    use_sgpr(t, reg);
    return fmt(t, "s%u", reg & 127);
}

const char* vgpr(Translator& t, u32 reg) {
    return vgpr_half(t, reg, t.half);
}

const char* vgpr_half(Translator& t, u32 reg, u32 half) {
    use_vgpr(t, reg);
    if (t.lanes == 2)
        return fmt(t, "v%u_%u", reg & 255, half & 1);
    return fmt(t, "v%u", reg & 255);
}

u32 mask_dwords(const Translator& t) {
    return t.wave32 ? 1 : 2;
}

const char* exec_cond(const Translator& t) {
    if (t.lanes == 2)
        return t.half ? "EXEC1" : "EXEC0";
    return "EXEC";
}

u32 lane_mask_part(const Translator& t) {
    return t.lanes == 2 ? t.half : 0u;
}

bool sgpr_is_mask(Translator& t, u32 reg) {
    reg &= 127;
    return tag_kind(t.tags.sgpr[reg]) == TAG_MASK || reg == SRC_EXEC_LO || reg == SRC_EXEC_HI;
}

// A constant dword of bits in mask form: all clear and all set read the same either way.
static const char* lane_mask_constant(Translator& t, u32 bits) {
    if (bits == 0)
        return "0u";
    if (bits == 0xFFFFFFFFu)
        return "0xffffffffu";
    return fmt(t, "lane_mask(0x%08xu)", bits);
}

bool literal_value(const char* s, u32* value) {
    if (strcmp(s, "0u") == 0) {
        *value = 0;
        return true;
    }
    if (strlen(s) != 11 || s[0] != '0' || s[1] != 'x' || s[10] != 'u')
        return false;
    u32 v = 0;
    for (u32 i = 2; i < 10; i++) {
        char c = s[i];
        u32 d = c >= '0' && c <= '9' ? (u32)(c - '0') : c >= 'a' && c <= 'f' ? (u32)(c - 'a' + 10) : 16u;
        if (d == 16)
            return false;
        v = v << 4 | d;
    }
    *value = v;
    return true;
}

const char* lane_mask_of(Translator& t, const char* bits) {
    u32 value;
    if (literal_value(bits, &value))
        return lane_mask_constant(t, value);
    return fmt(t, "lane_mask(%s)", bits);
}

static bool is_scalar_register(const RdnaOperand& o) {
    return o.kind == OPND_SGPR || (o.kind == OPND_SPECIAL && o.reg < 128 && o.reg != SRC_NULL);
}

const char* raw_operand(Translator& t, const RdnaInst& in, u32 i, u32 part) {
    const RdnaOperand& o = in.src[i];
    if (is_scalar_register(o))
        return sgpr(t, o.reg + part);
    return src_u32(t, in, i, TYPE_32, part);
}

const char* mask_operand(Translator& t, const RdnaInst& in, u32 i, u32 part) {
    if (!t.exact)
        return src_u32(t, in, i, TYPE_32, part);
    const RdnaOperand& o = in.src[i];
    if (is_scalar_register(o)) {
        u32 reg = (o.reg + part) & 127;
        u32 value;
        if (sgpr_constant(t.tags, reg, &value) && !sgpr_is_mask(t, reg))
            return lane_mask_constant(t, value);
        if (sgpr_is_mask(t, reg) || sgpr_neutral(t.tags, reg))
            return sgpr(t, reg);
        return fmt(t, "lane_mask(%s)", sgpr(t, reg));
    }
    if (o.kind == OPND_VGPR) {
        translator_fail(t, "lane mask from a vector register in %s at 0x%x", rdna_op_name(in.op), in.offset);
        return "0u";
    }
    return lane_mask_of(t, src_u32(t, in, i, TYPE_32, part));
}

// Whether any lane of a mask register pair (or of a 32-bit mask in wave32 programs) is set, in scalar
// code. Data in VCC counts its bits.
static const char* mask_nonzero(Translator& t, u32 reg) {
    if (!t.exact)
        return fmt(t, "wave_any(%s)", sgpr(t, reg));
    if (t.in_lane_op) {
        translator_fail(t, "whole-mask test of s%u by a vector instruction at 0x%x", reg, t.inst_offset);
        return "false";
    }
    bool mask = sgpr_is_mask(t, reg);
    if (!t.wave32 && sgpr_is_mask(t, reg + 1) != mask) {
        translator_fail(t, "s%u and s%u: one a lane mask, the other data", reg, reg + 1);
        return "false";
    }
    if (mask)
        return t.lanes == 2 ? fmt(t, "mask_any(%s, %s)", sgpr(t, reg), sgpr(t, reg + 1))
                            : fmt(t, "wave_any(%s)", sgpr(t, reg));
    return t.wave32 ? fmt(t, "(%s != 0u)", sgpr(t, reg))
                    : fmt(t, "((%s | %s) != 0u)", sgpr(t, reg), sgpr(t, reg + 1));
}

static const u32 k_inline_f32[9] = {0x3f000000, 0xbf000000, 0x3f800000, 0xbf800000, 0x40000000,
                                    0xc0000000, 0x40800000, 0xc0800000, 0x3e22f983};
static const u32 k_inline_f16[9] = {0x3800, 0xb800, 0x3c00, 0xbc00, 0x4000, 0xc000, 0x4400, 0xc400, 0x3118};
static const u32 k_inline_f64_hi[9] = {0x3fe00000, 0xbfe00000, 0x3ff00000, 0xbff00000, 0x40000000,
                                       0xc0000000, 0x40100000, 0xc0100000, 0x3fc45f30};

static const char* operand_u32(Translator& t, const RdnaOperand& o, OperandType type, u32 part) {
    switch (o.kind) {
    case OPND_SGPR:
        return sgpr_data(t, o.reg + part);
    case OPND_VGPR:
        return vgpr_data(t, o.reg + part);
    case OPND_LITERAL:
        return part ? "0u" : fmt(t, "0x%08xu", o.value);
    case OPND_INLINE: {
        if (o.reg <= SRC_INT_NEG_MAX) {
            s32 v = o.reg <= SRC_INT_MAX ? (s32)o.reg - SRC_ZERO : SRC_INT_MAX - (s32)o.reg;
            if (part)
                return v < 0 ? "0xffffffffu" : "0u";
            return fmt(t, "0x%08xu", (u32)v);
        }
        u32 index = o.reg - SRC_HALF;
        if (type == TYPE_16)
            return part ? "0u" : fmt(t, "0x%04xu", k_inline_f16[index]);
        if (type == TYPE_64)
            return part ? fmt(t, "0x%08xu", k_inline_f64_hi[index]) : (index == 8 ? "0x6dc9c882u" : "0u");
        return part ? "0u" : fmt(t, "0x%08xu", k_inline_f32[index]);
    }
    case OPND_SPECIAL:
        switch (o.reg) {
        case SRC_VCC_LO:
        case SRC_VCC_HI:
        case SRC_EXEC_LO:
        case SRC_EXEC_HI:
        case SRC_M0:
            return sgpr_data(t, o.reg + part);
        case SRC_NULL:
            return "0u";
        case SRC_SCC:
            t.scc_used = true;
            return "(scc ? 1u : 0u)";
        case SRC_VCCZ:
            return fmt(t, "(%s ? 0u : 1u)", mask_nonzero(t, SRC_VCC_LO));
        case SRC_EXECZ:
            return fmt(t, "(%s ? 0u : 1u)", mask_nonzero(t, SRC_EXEC_LO));
        default:
            translator_fail(t, "unsupported operand %u", o.reg);
            return "0u";
        }
    default:
        translator_fail(t, "missing operand");
        return "0u";
    }
}

const char* register_data(Translator& t, Tag tag, const char* raw, bool vector) {
    if (!heap_loaded(tag) || t.copying)
        return raw;
    u32 payload = tag_payload(tag);
    if (payload & HEAP_MODIFIED)
        translator_fail(t, "register read as a value at 0x%x holds a descriptor dword on some paths only",
                        t.inst_offset);
    else if ((payload & 1) == HEAP_SAMPLERS)
        translator_fail(t, "sampler descriptor dword read as a value at 0x%x", t.inst_offset);
    // The shadow holds each desktop slot's descriptor as console heap memory does, image descriptors'
    // dwords too (shadow_at finds the dword at a heap byte address). Lanes outside EXEC may hold anything
    // in a vector register, and the shadow, a root descriptor, has no bounds: they read the first record
    // instead.
    t.needs_descriptor_info = true;
    if (vector)
        return fmt(t, "g_descriptor_info.Load(shadow_at(%s ? %s : 0u))", exec_cond(t), raw);
    return fmt(t, "g_descriptor_info.Load(shadow_at(%s))", raw);
}

// With exact lanes a mask is read as its bits too, with a ballot: inline in scalar code, before the
// instruction for per-lane ones (see hoist_mask_reads; under their EXEC guards not all lanes would take
// part). Unlike descriptor dwords, masks are converted for copies as well: only scalar moves keep the
// mask form, and they read their sources with raw_operand.
const char* sgpr_data(Translator& t, u32 reg) {
    reg &= 127;
    if (!t.exact || !sgpr_is_mask(t, reg))
        return register_data(t, t.tags.sgpr[reg], sgpr(t, reg), false);
    if (!t.in_lane_op)
        return fmt(t, "mask_bits(%s)", sgpr(t, reg));
    for (u32 k = 0; k < t.hoisted_count; k++)
        if (t.hoisted[k] == reg)
            return fmt(t, "mb%u_", reg);
    translator_fail(t, "lane mask s%u read as data by a vector instruction at 0x%x", reg, t.inst_offset);
    return "0u";
}

const char* vgpr_data(Translator& t, u32 reg) {
    return register_data(t, t.tags.vgpr[reg & 255], vgpr(t, reg), true);
}

const char* src_u32(Translator& t, const RdnaInst& in, u32 i, OperandType type, u32 part) {
    if (t.dpp_active && i == 0 && part == 0)
        return "dpp0_";
    const RdnaOperand& o = in.src[i];
    if (o.kind == OPND_SPECIAL && (o.reg == SRC_VCC_LO || o.reg == SRC_EXEC_LO))
        t.sgpr_used[o.reg] = 1;
    const char* value = operand_u32(t, o, type, part);
    if (in.has_sdwa && i < 2 && in.sdwa.src_sel[i] != SDWA_DWORD) {
        u32 sel = in.sdwa.src_sel[i];
        u32 shift = sel <= SDWA_BYTE_3 ? sel * 8 : (sel - SDWA_WORD_0) * 16;
        u32 width = sel <= SDWA_BYTE_3 ? 8 : 16;
        if (in.sdwa.src_sext[i])
            return fmt(t, "((uint)((int)(%s << %u) >> %u))", value, 32 - shift - width, 32 - width);
        return fmt(t, "((%s >> %u) & 0x%xu)", value, shift, (1u << width) - 1);
    }
    return value;
}

const char* src_f32(Translator& t, const RdnaInst& in, u32 i) {
    const char* v = fmt(t, "asfloat(%s)", src_u32(t, in, i));
    const RdnaOperand& o = in.src[i];
    if (o.abs)
        v = fmt(t, "abs(%s)", v);
    if (o.neg)
        v = fmt(t, "(-%s)", v);
    return v;
}

const char* float_result(Translator& t, const RdnaInst& in, const char* value) {
    static const char* const k_omod[4] = {"", " * 2.0f", " * 4.0f", " * 0.5f"};
    const char* v = value;
    if (in.omod)
        v = fmt(t, "(%s%s)", v, k_omod[in.omod & 3]);
    if (in.clamp)
        v = fmt(t, "saturate(%s)", v);
    return fmt(t, "asuint(%s)", v);
}

// Whether a value may be computed in lanes outside EXEC too: everything but cross-lane operations (the
// lanes outside EXEC would take part), texture sampling with derivatives and reads through root
// descriptors, which aren't bounds-checked (addresses there may be anything). Buffer views and LDS
// are bounds-checked and contained, and translated code already reads them in every lane.
static bool computable_in_every_lane(const char* value) {
    static const char* const k_lane_bound[] = {
        "g_root",   "WaveActive", "WavePrefix", "WaveRead",      "WaveMatch",   "WaveMulti", "QuadRead",
        "wave_any", "wave_count", "wave_first", "wave_last",     "read_first",  "quad_any",  "mask_bits",
        "mask_any", "Sample",     "Gather",     "LevelOfDetail", "Interlocked",
    };
    for (u32 i = 0; i < ARRAY_COUNT(k_lane_bound); i++)
        if (strstr(value, k_lane_bound[i]))
            return false;
    return true;
}

void write_vgpr(Translator& t, const RdnaInst& in, u32 reg, const char* value) {
    const char* dst = vgpr(t, reg);
    const RdnaOperand& d = in.dst[0];
    u32 at = (reg - d.reg) & 255;
    bool in_destination = d.kind == OPND_VGPR && at < (d.count ? d.count : 1u);
    if (in_destination && at < 32)
        t.lane_written |= 1u << at;
    if (in.has_sdwa && in.sdwa.dst_sel != SDWA_DWORD) {
        u32 sel = in.sdwa.dst_sel;
        u32 shift = sel <= SDWA_BYTE_3 ? sel * 8 : (sel - SDWA_WORD_0) * 16;
        u32 width = sel <= SDWA_BYTE_3 ? 8 : 16;
        u32 field = ((1u << width) - 1) << shift;
        const char* bits = fmt(t, "((%s << %u) & 0x%08xu)", value, shift, field);
        if (in.sdwa.dst_unused == 2) { // preserve the other bits
            value = fmt(t, "((%s & 0x%08xu) | %s)", dst, ~field, bits);
            if (lane_write_replaces(t, in))
                t.lane_check_failed = true;
        } else {
            value = bits;
        }
    }
    if (strcmp(value, dst) == 0)
        return; // a move onto itself (v_mov v1, v1 and the like)
    bool every_lane = computable_in_every_lane(value);
    if (t.write_all_lanes) {
        // The lanes outside EXEC are never read again (see translate_lanes.cpp): write them all, so
        // that the old value dies here. Only the destination the analysis knows may be written so.
        if (!in_destination || t.dpp_has_mask)
            t.lane_check_failed = true;
        if (every_lane) {
            emit(t, "%s = %s;", dst, value);
            return;
        }
    }
    // The lanes outside EXEC keep the old value. In compute programs a select says so without a branch
    // (long programs full of branches on EXEC keep the desktop compiler's jump threading busy for
    // minutes); it computes the value in every lane. Graphics programs keep the branches, which the
    // compiler merges into fewer instructions there.
    if (t.dpp_has_mask)
        emit(t, "if (%s && dpp_en_) %s = %s;", exec_cond(t), dst, value);
    else if (every_lane && t.exact)
        emit(t, "%s = select(%s, %s, %s);", dst, exec_cond(t), value, dst);
    else
        emit(t, "if (%s) %s = %s;", exec_cond(t), dst, value);
}

void write_sgpr(Translator& t, const RdnaOperand& dst, u32 part, const char* value) {
    if (dst.kind == OPND_SPECIAL && dst.reg == SRC_NULL)
        return;
    if (dst.kind != OPND_SGPR && dst.kind != OPND_SPECIAL) {
        translator_fail(t, "bad scalar destination");
        return;
    }
    u32 reg = (dst.reg + part) & 127;
    if (t.exec_bits && (reg == SRC_EXEC_LO || reg == SRC_EXEC_HI))
        value = lane_mask_of(t, value);
    const char* name = sgpr(t, reg);
    if (strcmp(value, name) == 0)
        return; // a move onto itself
    emit(t, "%s = %s;", name, value);
}

// --- pixel shader input VGPRs ------------------------------------------------------------------------

// SPI_PS_INPUT_ADDR bits and the VGPRs each one brings, in VGPR order.
static const u8 k_ps_input_vgprs[16] = {2, 2, 2, 3, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1};

enum PsInput : u8 {
    PS_PERSP_SAMPLE,
    PS_PERSP_CENTER,
    PS_PERSP_CENTROID,
    PS_PERSP_PULL_MODEL,
    PS_LINEAR_SAMPLE,
    PS_LINEAR_CENTER,
    PS_LINEAR_CENTROID,
    PS_LINE_STIPPLE,
    PS_POS_X,
    PS_POS_Y,
    PS_POS_Z,
    PS_POS_W,
    PS_FRONT_FACE,
    PS_ANCILLARY,
    PS_SAMPLE_COVERAGE,
    PS_POS_FIXED_PT,
};

static void layout_ps_inputs(Translator& t) {
    u32 next = 0;
    for (u32 i = 0; i < 16; i++) {
        t.ps_input_vgpr[i] = ~0u;
        if (t.info->ps_input_addr & (1u << i)) {
            t.ps_input_vgpr[i] = next;
            next += k_ps_input_vgprs[i];
        }
    }
}

static bool covers(const RdnaOperand& o, u32 reg) {
    return o.kind == OPND_VGPR && reg - o.reg < (o.count ? o.count : 1u);
}

// Whether the program reads `reg` before writing it, taken in program order: input VGPRs are read
// where the program starts, before their registers are reused.
static bool vgpr_live_in(const Translator& t, u32 reg) {
    const Program& p = t.program;
    for (u32 i = 0; i < p.count; i++) {
        const RdnaInst& in = p.insts[i];
        for (u32 j = 0; j < in.num_src; j++)
            if (covers(in.src[j], reg))
                return true;
        if (covers(in.dst[0], reg) || covers(in.dst[1], reg))
            return false;
    }
    return false;
}

// Whether the program reads a pixel shader input: any of its VGPRs before they are written.
static bool ps_input_read(const Translator& t, PsInput input) {
    u32 first = t.ps_input_vgpr[input];
    if (first == ~0u)
        return false;
    for (u32 r = first; r < first + k_ps_input_vgprs[input] && r < 256; r++)
        if (t.vgpr_used[r] && vgpr_live_in(t, r))
            return true;
    return false;
}

// The interpolation an I/J VGPR pair stands for.
static Interpolation interpolation_of(const Translator& t, u32 reg) {
    static const Interpolation k_modes[7] = {
        INTERP_PERSPECTIVE_SAMPLE, INTERP_PERSPECTIVE, INTERP_PERSPECTIVE_CENTROID, INTERP_NONE,
        INTERP_LINEAR_SAMPLE,      INTERP_LINEAR,      INTERP_LINEAR_CENTROID};
    for (u32 i = 0; i < 7; i++) {
        u32 first = t.ps_input_vgpr[i];
        if (first != ~0u && (reg == first || reg == first + 1))
            return k_modes[i];
    }
    return INTERP_NONE;
}

static const char* const k_mode_names[8] = {"none",   "center",          "centroid",      "sample",
                                            "linear", "linear_centroid", "linear_sample", "flat"};

// Attribute interpolation. The desktop interpolates the attributes as declared (see
// write_attribute_declarations), so the first step (P1: P0 + I * P10) is folded into the second (P2:
// + J * P20): only the final value is observable. The 16-bit forms interpolate one half of each
// attribute dword, which holds two 16-bit values: the vertex shader packed them.
static bool emit_interpolation(Translator& t, const RdnaInst& in) {
    if (t.info->stage != SHADER_PIXEL) {
        translator_fail(t, "interpolation outside a pixel shader");
        return false;
    }
    bool half = in.enc == ENC_VOP3;
    if (half) {
        // The P1LV form takes P0 from a VGPR (16-bank LDS): not what folding the first step assumes.
        if (in.op == OP_V_INTERP_P1LV_F16) {
            translator_fail(t, "v_interp_p1lv_f16 at 0x%x", in.offset);
            return false;
        }
        for (u32 i = 0; i < in.num_src; i++) {
            if (in.src[i].abs || in.src[i].neg) {
                translator_fail(t, "%s with source modifiers at 0x%x", rdna_op_name(in.op), in.offset);
                return false;
            }
        }
        if (in.omod || in.op_sel) {
            translator_fail(t, "%s with an output modifier or op_sel at 0x%x", rdna_op_name(in.op),
                            in.offset);
            return false;
        }
    }
    Interpolation mode = INTERP_FLAT;
    if (in.op != OP_V_INTERP_MOV_F32) {
        mode = interpolation_of(t, in.src[0].reg);
        if (mode == INTERP_NONE) {
            translator_fail(t, "interpolation with unknown barycentrics v%u", in.src[0].reg);
            return false;
        }
    } else if (in.src[0].reg != 2) {
        translator_fail(t, "v_interp_mov_f32 from a vertex other than P0");
        return false;
    }
    u32 attr = in.attr & 31;
    u32 slot = (mode == INTERP_FLAT && t.info->flat_remap[attr]) ? (u32)(t.info->flat_remap[attr] - 1) : attr;
    if (slot >= 32)
        slot = attr;
    t.attribute_modes[slot] |= (u8)(1u << mode);
    if (half)
        t.attribute_halves |= 1u << slot;
    t.out->ps_attribute_mask |= 1u << slot;
    if (in.op == OP_V_INTERP_P1_F32 || in.op == OP_V_INTERP_P1LL_F16)
        return true;
    char chan = "xyzw"[in.attr_chan & 3];
    if (half) {
        // The result goes to the low half; the high half is kept.
        const char* v =
            fmt(t, "attr%u_%s_half(%c, %u)", slot, k_mode_names[mode], chan, in.attr_high ? 16u : 0u);
        if (in.clamp)
            v = fmt(t, "saturate(%s)", v);
        const char* d = vgpr(t, in.dst[0].reg);
        write_vgpr(t, in, in.dst[0].reg, fmt(t, "((%s & 0xffff0000u) | f32tof16(%s))", d, v));
        return true;
    }
    // Read through a per-mode name the declarations define (see write_attribute_declarations).
    write_vgpr(t, in, in.dst[0].reg, fmt(t, "asuint(attr%u_%s.%c)", slot, k_mode_names[mode], chan));
    return true;
}

// --- exports -----------------------------------------------------------------------------------------

static bool emit_export(Translator& t, const RdnaInst& in) {
    const RdnaExport& e = in.exp;
    u32 target = e.target;
    const char* c[4];
    for (u32 i = 0; i < 4; i++) {
        u32 source = e.compressed ? i / 2 : i;
        const char* reg = vgpr(t, in.src[source].reg);
        if (e.compressed)
            c[i] = fmt(t, "f16tof32(%s >> %u)", reg, (i & 1) * 16);
        else
            c[i] = fmt(t, "asfloat(%s)", reg);
        if (!(e.enable & (1u << i)))
            c[i] = "0.0f";
    }
    if (t.info->stage == SHADER_VERTEX) {
        if (target == EXP_POS0) {
            t.position_exported = 1;
            emit(t, "output.position = float4(%s, %s, %s, %s);", c[0], c[1], c[2], c[3]);
        } else if (target == EXP_POS0 + 1) {
            t.vs_clip_mask |= e.enable & 0xF;
            for (u32 i = 0; i < 4; i++)
                if (e.enable & (1u << i))
                    emit(t, "output.clip.%c = %s;", "xyzw"[i], c[i]);
        } else if (target >= EXP_PARAM0) {
            u32 n = target - EXP_PARAM0;
            if (n >= 32) {
                translator_fail(t, "vertex export to param%u", n);
                return false;
            }
            t.out->vs_param_mask |= 1u << n;
            emit(t, "output.p%u = float4(%s, %s, %s, %s);", n, c[0], c[1], c[2], c[3]);
            if (t.info->flat_remap[n]) {
                u32 d = t.info->flat_remap[n] - 1;
                if (d < 32) {
                    t.out->vs_param_mask |= 1u << d;
                    emit(t, "output.p%u = output.p%u;", d, n);
                }
            }
        } else if (target > EXP_POS0 && target < EXP_POS0 + 5) {
            translator_fail(t, "vertex export to pos%u (clip distances, point size)", target - EXP_POS0);
            return false;
        } else {
            translator_fail(t, "vertex export to target %u", target);
            return false;
        }
        return true;
    }
    if (t.info->stage != SHADER_PIXEL) {
        translator_fail(t, "export from a compute shader");
        return false;
    }
    // Lanes without EXEC at the last export are killed when it carries the valid mask.
    if (e.done && e.valid_mask) {
        t.out->ps_discards = true;
        emit(t, "if (!EXEC) discard;");
    }
    if (target < 8) {
        t.out->ps_target_mask |= 1u << target;
        t.target_compressed[target] = e.compressed;
        emit(t, "output.t%u = float4(%s, %s, %s, %s);", target, c[0], c[1], c[2], c[3]);
    } else if (target == EXP_MRTZ) {
        if (e.enable & 1) {
            t.out->ps_writes_depth = true;
            emit(t, "output.depth = %s;", c[0]);
        }
        if (e.enable & ~1u) {
            translator_fail(t, "depth export with stencil or sample mask");
            return false;
        }
    } else if (target != EXP_NULL) {
        translator_fail(t, "pixel export to target %u", target);
        return false;
    }
    return true;
}

// --- instructions ------------------------------------------------------------------------------------

static bool emit_instruction(Translator& t, const RdnaInst& in) {
    switch (in.enc) {
    case ENC_SOPP:
        switch (in.op) {
        case OP_S_NOP:
        case OP_S_WAITCNT:
        case OP_S_INST_PREFETCH:
        case OP_S_CLAUSE:
        case OP_S_SETPRIO:
        case OP_S_SLEEP:
        case OP_S_WAITCNT_DEPCTR:
        case OP_S_ICACHE_INV:
        case OP_S_TTRACEDATA:
        case OP_S_TTRACEDATA_IMM:
        case OP_S_ROUND_MODE:
        case OP_S_DENORM_MODE:
        case OP_S_INCPERFLEVEL:
        case OP_S_DECPERFLEVEL:
            return true; // scheduling, waits and modes: nothing to do on the desktop
        case OP_S_BARRIER:
            if (t.info->stage == SHADER_COMPUTE)
                emit(t, "GroupMemoryBarrierWithGroupSync();");
            return true;
        default:
            break; // branches and ends are handled with the blocks
        }
        if (in.op == OP_S_ENDPGM || in.op == OP_S_BRANCH ||
            (in.op >= OP_S_CBRANCH_SCC0 && in.op <= OP_S_CBRANCH_EXECNZ))
            return true;
        translator_fail(t, "unsupported %s", rdna_op_name(in.op));
        return false;
    case ENC_SOPK:
        if (in.op >= OP_S_WAITCNT_VSCNT && in.op <= OP_S_WAITCNT_LGKMCNT)
            return true;
        return emit_alu(t, in);
    case ENC_VINTRP:
        return emit_interpolation(t, in);
    case ENC_VOP3:
        if (in.op == OP_V_INTERP_P1LL_F16 || in.op == OP_V_INTERP_P1LV_F16 || in.op == OP_V_INTERP_P2_F16)
            return emit_interpolation(t, in);
        return emit_alu(t, in);
    case ENC_EXP:
        return emit_export(t, in);
    case ENC_SMEM:
    case ENC_MUBUF:
    case ENC_MTBUF:
    case ENC_MIMG:
    case ENC_DS:
    case ENC_FLAT:
    case ENC_GLOBAL:
        return emit_memory(t, in);
    default:
        return emit_alu(t, in);
    }
}

// Exact lanes: instructions emitted once for the console wave. Scalar ones, and vector ones reaching
// across lanes in ways one half at a time can't (they name both halves themselves).
static bool whole_wave(const Translator& t, const RdnaInst& in) {
    switch (in.enc) {
    case ENC_SOP1:
    case ENC_SOP2:
    case ENC_SOPC:
    case ENC_SOPK:
    case ENC_SOPP:
    case ENC_SMEM:
        return true;
    default:
        break;
    }
    switch (in.op) {
    case OP_V_READLANE_B32:
    case OP_V_WRITELANE_B32:
    case OP_V_READFIRSTLANE_B32:
        return true;
    case OP_DS_APPEND:
    case OP_DS_CONSUME:
    case OP_DS_BPERMUTE_B32:
        return t.lanes == 2;
    default:
        return false;
    }
}

static bool is_valu(const RdnaInst& in) {
    RdnaEncoding base = rdna_op_encoding(in.op);
    return base == ENC_VOP1 || base == ENC_VOP2 || base == ENC_VOPC || base == ENC_VOP3 || base == ENC_VOP3P;
}

// Operands vector instructions read as lane masks (the lane's bit): selects and carries in.
static bool mask_position(const RdnaInst& in, u32 i) {
    return i == 2 && (in.op == OP_V_CNDMASK_B32 || in.op == OP_V_ADD_CO_CI_U32 ||
                      in.op == OP_V_SUB_CO_CI_U32 || in.op == OP_V_SUBREV_CO_CI_U32);
}

static bool writes_sgpr(const RdnaInst& in, u32 reg) {
    for (u32 d = 0; d < 2; d++) {
        const RdnaOperand& o = in.dst[d];
        if (is_scalar_register(o) && ((reg - o.reg) & 127) < (o.count ? o.count : 1u))
            return true;
    }
    return false;
}

// Masks a vector instruction reads as data, as bits: ballots before the instruction, where every lane
// takes part. Returns whether it opened a scope for them.
static bool hoist_mask_reads(Translator& t, const RdnaInst& in) {
    t.hoisted_count = 0;
    if (!is_valu(in))
        return false;
    bool opened = false;
    for (u32 j = 0; j < in.num_src && !t.failed; j++) {
        const RdnaOperand& o = in.src[j];
        if (!is_scalar_register(o) || mask_position(in, j))
            continue;
        for (u32 k = 0; k < (o.count ? o.count : 1u); k++) {
            u32 reg = (o.reg + k) & 127;
            bool mask = sgpr_is_mask(t, reg);
            // Two lanes per thread: the first half's mask result would be read by the second half.
            if (t.lanes == 2 && !mask && writes_sgpr(in, reg)) {
                translator_fail(t, "%s at 0x%x reads s%u, which its lane mask result writes",
                                rdna_op_name(in.op), in.offset, reg);
                return opened;
            }
            bool known = false;
            for (u32 h = 0; h < t.hoisted_count; h++)
                known = known || t.hoisted[h] == reg;
            if (!mask || known)
                continue;
            if (t.hoisted_count == SHADER_MAX_HOISTED) {
                translator_fail(t, "too many lane masks read as data at 0x%x", in.offset);
                return opened;
            }
            if (!opened) {
                emit(t, "{");
                t.depth++;
                opened = true;
            }
            emit(t, "uint mb%u_ = mask_bits(%s);", reg, sgpr(t, reg));
            t.hoisted[t.hoisted_count++] = (u8)reg;
        }
    }
    return opened;
}

// A per-lane instruction with exact lanes: once per half.
static void emit_lanes(Translator& t, const RdnaInst& in) {
    bool opened = hoist_mask_reads(t, in);
    t.in_lane_op = true;
    for (u32 h = 0; h < t.lanes && !t.failed; h++) {
        t.half = (u8)h;
        emit_instruction(t, in);
    }
    t.in_lane_op = false;
    t.half = 0;
    t.hoisted_count = 0;
    if (opened) {
        t.depth--;
        emit(t, "}");
    }
}

void emit_block(Translator& t, u32 b) {
    const Block& blk = t.program.blocks[b];
    t.tags = t.block_entry[b];
    for (u32 i = blk.first; i < blk.end && !t.failed; i++) {
        const RdnaInst& in = t.program.insts[i];
        t.inst_offset = in.offset;
        t.copying = copies_tags(t, t.tags, in, i);
        t.write_all_lanes = t.lane_write[i] != 0;
        t.lane_written = 0;
        if (!t.exact || whole_wave(t, in))
            emit_instruction(t, in);
        else
            emit_lanes(t, in);
        if (!lane_write_complete(t, in, t.lane_written))
            t.lane_check_failed = true;
        tags_transfer(t, t.tags, in, i);
    }
    t.write_all_lanes = false;
}

const char* branch_condition(Translator& t, const RdnaInst& in, bool negate) {
    const char* test = "true";
    bool taken_if_set = true;
    switch (in.op) {
    case OP_S_CBRANCH_SCC0:
    case OP_S_CBRANCH_SCC1:
        t.scc_used = true;
        test = "scc";
        taken_if_set = in.op == OP_S_CBRANCH_SCC1;
        break;
    case OP_S_CBRANCH_VCCZ:
    case OP_S_CBRANCH_VCCNZ:
        test = mask_nonzero(t, SRC_VCC_LO);
        taken_if_set = in.op == OP_S_CBRANCH_VCCNZ;
        break;
    case OP_S_CBRANCH_EXECZ:
    case OP_S_CBRANCH_EXECNZ:
        test = mask_nonzero(t, SRC_EXEC_LO);
        taken_if_set = in.op == OP_S_CBRANCH_EXECNZ;
        break;
    default:
        return negate ? "false" : "true";
    }
    return taken_if_set != negate ? test : fmt(t, "!%s", test);
}

// --- the program -------------------------------------------------------------------------------------

static const char k_preamble_head[] = "// Translated console shader.\n";
static const char k_exec_one[] = "#define EXEC (s126 != 0u)\n";
// Two lanes per thread: an EXEC test per half (see translate.h).
static const char k_exec_two[] = "#define EXEC0 (s126 != 0u)\n#define EXEC1 (s127 != 0u)\n";

// Exact lanes (desktop waves of 32 lanes): masks between broadcast form and bits.
static const char k_exact_helpers[] =
    "uint mask_bits(uint m) { return WaveActiveBallot(m != 0u).x; }\n"
    "bool mask_any(uint lo, uint hi) { return WaveActiveAnyTrue(lo != 0u || hi != 0u); }\n"
    "uint lane_mask(uint bits) { return mask_of(((bits >> WaveGetLaneIndex()) & 1u) != 0u); }\n";

static const char k_preamble[] =
    "uint mask_of(bool b) { return b ? 0xffffffffu : 0u; }\n"
    "bool wave_any(uint m) { return WaveActiveAnyTrue(m != 0u); }\n"
    "uint wave_count(uint m) { uint4 b = WaveActiveBallot(m != 0u); return countbits(b.x) + countbits(b.y) + "
    "countbits(b.z) + countbits(b.w); }\n"
    "uint wave_first(uint m) { uint4 b = WaveActiveBallot(m != 0u); return b.x ? firstbitlow(b.x) : b.y ? "
    "32u + "
    "firstbitlow(b.y) : b.z ? 64u + firstbitlow(b.z) : b.w ? 96u + firstbitlow(b.w) : 0xffffffffu; }\n"
    "uint wave_last(uint m) { uint4 b = WaveActiveBallot(m != 0u); return b.w ? 96u + firstbithigh(b.w) : "
    "b.z ? "
    "64u + firstbithigh(b.z) : b.y ? 32u + firstbithigh(b.y) : b.x ? firstbithigh(b.x) : 0xffffffffu; }\n"
    // The first clear bit of a console mask `width` bits wide: lanes the desktop wave lacks count as clear.
    "uint wave_first_zero(uint m, uint width) { uint4 b = WaveActiveBallot(m != 0u); uint lo = ~b.x, hi = "
    "width > 32u ? ~b.y : 0u; return lo ? firstbitlow(lo) : hi ? 32u + firstbitlow(hi) : 0xffffffffu; }\n"
    "uint read_first(uint v, uint m) { uint lane = WaveActiveMin(m != 0u ? WaveGetLaneIndex() : "
    "0xffffffffu); "
    "return WaveActiveBitOr(WaveGetLaneIndex() == (lane == 0xffffffffu ? 0u : lane) ? v : 0u); }\n"
    "float mul_legacy(float a, float b) { return (a == 0.0f || b == 0.0f) ? 0.0f : a * b; }\n"
    "uint f32_to_u32(float f) { return f != f ? 0u : f >= 4294967040.0f ? 0xffffffffu : f <= 0.0f ? 0u : "
    "(uint)f; }\n"
    "int f32_to_i32(float f) { return f != f ? 0 : f >= 2147483520.0f ? 2147483647 : f <= -2147483648.0f ? "
    "(-2147483647 - 1) : (int)f; }\n"
    "uint bfe_u32(uint v, uint o, uint w) { o &= 31u; w &= 31u; return w == 0u ? 0u : (v >> o) & ((1u << w) "
    "- "
    "1u); }\n"
    "int bfe_i32(uint v, uint o, uint w) { o &= 31u; w &= 31u; return w == 0u ? 0 : ((int)(v << (32u - o - "
    "w))) "
    ">> (32u - w); }\n"
    "uint med3_u32(uint a, uint b, uint c) { return max(min(a, b), min(max(a, b), c)); }\n"
    "int med3_i32(int a, int b, int c) { return max(min(a, b), min(max(a, b), c)); }\n"
    "float med3_f32(float a, float b, float c) { return max(min(a, b), min(max(a, b), c)); }\n"
    "uint ffbh_u32(uint v) { return v == 0u ? 0xffffffffu : 31u - firstbithigh(v); }\n"
    "uint ffbh_i32(int v) { uint h = firstbithigh(v); return h == 0xffffffffu ? 0xffffffffu : 31u - h; }\n"
    "uint perm_byte(uint hi, uint lo, uint sel) { sel &= 0xffu; if (sel < 8u) return ((sel < 4u ? lo : hi) "
    ">> "
    "((sel & 3u) * 8u)) & 0xffu; if (sel < 12u) { uint b = sel == 8u ? (lo >> 15) : sel == 9u ? (lo >> 31) : "
    "sel "
    "== 10u ? (hi >> 15) : (hi >> 31); return (b & 1u) ? 0xffu : 0u; } return sel == 12u ? 0u : 0xffu; }\n"
    "uint perm_b32(uint hi, uint lo, uint s) { return perm_byte(hi, lo, s) | (perm_byte(hi, lo, s >> 8) << "
    "8) | "
    "(perm_byte(hi, lo, s >> 16) << 16) | (perm_byte(hi, lo, s >> 24) << 24); }\n"
    "float sin_rev(float x) { return sin(x * 6.28318530718f); }\n"
    "float cos_rev(float x) { return cos(x * 6.28318530718f); }\n"
    "uint cvt_pkrtz(float a, float b) { return f32tof16(a) | (f32tof16(b) << 16); }\n"
    "uint quad_any(uint m) { uint q = m != 0u ? 1u : 0u; q |= QuadReadAcrossX(q) | QuadReadAcrossY(q) | "
    "QuadReadAcrossDiagonal(q); return q != 0u ? 0xffffffffu : 0u; }\n"
    "bool cmp_class(float f, uint c) { uint u = asuint(f); bool neg = (u >> 31) != 0u; uint e = (u >> 23) & "
    "0xffu; "
    "uint m = u & 0x7fffffu; uint bit; if (e == 0xffu) bit = m != 0u ? ((m & 0x400000u) ? 1u : 0u) : (neg ? "
    "2u : "
    "9u); else if (e == 0u) bit = m == 0u ? (neg ? 5u : 6u) : (neg ? 4u : 7u); else bit = neg ? 3u : 8u; "
    "return "
    "((c >> bit) & 1u) != 0u; }\n"
    "bool cmp_class_f16(uint u, uint c) { u &= 0xffffu; bool neg = (u >> 15) != 0u; uint e = (u >> 10) & "
    "0x1fu; uint m = u & 0x3ffu; uint bit; if (e == 0x1fu) bit = m != 0u ? ((m & 0x200u) ? 1u : 0u) : (neg ? "
    "2u : 9u); else if (e == 0u) bit = m == 0u ? (neg ? 5u : 6u) : (neg ? 4u : 7u); else bit = neg ? 3u : "
    "8u; return ((c >> bit) & 1u) != 0u; }\n"
    "float frexp_mant(float f) { float e; float m = frexp(f, e); return m; }\n"
    "int frexp_exp(float f) { float e; frexp(f, e); return (int)e; }\n"
    "float cube_ma(float3 v) { float3 a = abs(v); return 2.0f * (a.z >= a.x && a.z >= a.y ? v.z : a.y >= a.x "
    "? v.y : "
    "v.x); }\n"
    "float cube_id(float3 v) { float3 a = abs(v); return a.z >= a.x && a.z >= a.y ? (v.z < 0.0f ? 5.0f : "
    "4.0f) : "
    "a.y >= a.x ? (v.y < 0.0f ? 3.0f : 2.0f) : (v.x < 0.0f ? 1.0f : 0.0f); }\n"
    "float cube_sc(float3 v) { float3 a = abs(v); return a.z >= a.x && a.z >= a.y ? (v.z < 0.0f ? -v.x : "
    "v.x) : "
    "a.y >= a.x ? v.x : (v.x < 0.0f ? v.z : -v.z); }\n"
    "float cube_tc(float3 v) { float3 a = abs(v); return a.z >= a.x && a.z >= a.y ? -v.y : a.y >= a.x ? (v.y "
    "< "
    "0.0f ? -v.z : v.z) : -v.y; }\n"
    // Cube sampling takes face coordinates on console (s, t in [1, 2] across the face, then
    // slice * 8 + face), desktop a direction: cube_coord turns them back into one, the inverse of
    // cube_sc/cube_tc above with the major axis at 1. Face-space gradients stay in the face's plane.
    "uint cube_face(float f) { return (uint)round(max(f, 0.0f)); }\n"
    "float3 cube_plane(uint id, float sc, float tc) { return id == 0u ? float3(0.0f, -tc, -sc) : id == 1u ? "
    "float3(0.0f, -tc, sc) : id == 2u ? float3(sc, 0.0f, tc) : id == 3u ? float3(sc, 0.0f, -tc) : id == 4u ? "
    "float3(sc, -tc, 0.0f) : float3(-sc, -tc, 0.0f); }\n"
    "float3 cube_major(uint id) { float m = (id & 1u) != 0u ? -1.0f : 1.0f; return id < 2u ? float3(m, 0.0f, "
    "0.0f) : id < 4u ? float3(0.0f, m, 0.0f) : float3(0.0f, 0.0f, m); }\n"
    "float4 cube_coord(float s, float t, float face) { uint f = cube_face(face); uint id = min(f & 7u, 5u); "
    "return float4(cube_major(id) + cube_plane(id, 2.0f * s - 3.0f, 2.0f * t - 3.0f), (float)(f >> 3)); }\n"
    "float3 cube_grad(float face, float ds, float dt) { return cube_plane(min(cube_face(face) & 7u, 5u), "
    "2.0f * ds, 2.0f * dt); }\n";

static const char* interpolation_keyword(Interpolation mode) {
    switch (mode) {
    case INTERP_PERSPECTIVE_CENTROID:
        return "centroid ";
    case INTERP_PERSPECTIVE_SAMPLE:
        return "sample ";
    case INTERP_LINEAR:
        return "noperspective ";
    case INTERP_LINEAR_CENTROID:
        return "noperspective centroid ";
    case INTERP_LINEAR_SAMPLE:
        return "noperspective sample ";
    case INTERP_FLAT:
        return "nointerpolation ";
    default:
        return "";
    }
}

static const char* element_hlsl_type(u32 components, bool typed, u8 value_class) {
    static const char* const k_float[5] = {"float", "float", "float2", "float3", "float4"};
    static const char* const k_uint[5] = {"uint", "uint", "uint2", "uint3", "uint4"};
    static const char* const k_int[5] = {"int", "int", "int2", "int3", "int4"};
    if (!typed || value_class == 1)
        return k_uint[components > 4 ? 4 : components];
    return value_class == 2 ? k_int[components > 4 ? 4 : components]
                            : k_float[components > 4 ? 4 : components];
}

// Raw dword fetches read the element as 32-bit unsigned integers.
static u32 raw_element_format(u32 components) {
    static const u32 k_formats[5] = {0, 42 /* R32_UINT */, 17 /* R32G32_UINT */, 7 /* R32G32B32_UINT */,
                                     3 /* R32G32B32A32_UINT */};
    return k_formats[components > 4 ? 4 : components];
}

// Pixel shader inputs. An attribute read with one interpolation is declared with it. Console
// shaders may also read some components flat and others interpolated (the hardware interpolates
// per instruction); such attributes are declared flat and interpolated with the barycentrics. So are
// attributes holding pairs of 16-bit values: each half is interpolated on its own.
static bool write_attribute_declarations(Translator& t, Text& h) {
    static const PsInput k_untranslated[] = {PS_PERSP_PULL_MODEL, PS_LINE_STIPPLE, PS_ANCILLARY};
    static const char* const k_untranslated_names[] = {"pull-model interpolation", "line stipple",
                                                       "ancillary"};
    for (u32 i = 0; i < ARRAY_COUNT(k_untranslated); i++) {
        if (ps_input_read(t, k_untranslated[i])) {
            translator_fail(t, "pixel shader reads the %s input, which is not translated",
                            k_untranslated_names[i]);
            return false;
        }
    }
    u8 interpolated_modes = 0; // modes mixed attributes need barycentrics for
    for (u32 i = 0; i < 32; i++) {
        u8 modes = t.attribute_modes[i];
        if ((modes & (modes - 1)) || (t.attribute_halves & (1u << i))) {
            t.out->ps_mixed_mask |= 1u << i;
            interpolated_modes |= (u8)(modes & ~(1u << INTERP_FLAT));
        }
    }
    if (interpolated_modes & (interpolated_modes - 1)) {
        translator_fail(t, "flat and several interpolated reads of the same attributes");
        return false;
    }
    Interpolation bary_mode = INTERP_NONE;
    for (u32 m = 0; m < 8; m++)
        if (interpolated_modes & (1u << m))
            bary_mode = (Interpolation)m;
    h.put("struct Input {\n    float4 position : SV_Position;\n");
    for (u32 i = 0; i < 32; i++) {
        u8 modes = t.attribute_modes[i];
        if (!modes)
            continue;
        bool mixed = (t.out->ps_mixed_mask & (1u << i)) != 0;
        Interpolation single = INTERP_NONE;
        for (u32 m = 0; m < 8; m++)
            if (modes & (1u << m))
                single = (Interpolation)m;
        h.put("    %sfloat4 p%u : P%u;\n", interpolation_keyword(mixed ? INTERP_FLAT : single), i, i);
    }
    if (bary_mode != INTERP_NONE)
        h.put("    %sfloat3 barycentrics : SV_Barycentrics;\n", interpolation_keyword(bary_mode));
    if (ps_input_read(t, PS_SAMPLE_COVERAGE))
        h.put("    uint coverage : SV_Coverage;\n");
    h.put("    bool front_face : SV_IsFrontFace;\n};\n");
    for (u32 i = 0; i < 32; i++) {
        u8 modes = t.attribute_modes[i];
        bool mixed = (t.out->ps_mixed_mask & (1u << i)) != 0;
        for (u32 m = 1; m < 8; m++) {
            if (!(modes & (1u << m)))
                continue;
            // A 16-bit value: the half of channel c starting at bit s, as a float.
            if ((t.attribute_halves & (1u << i)) && m != INTERP_FLAT)
                h.put("#define attr%u_%s_half(c, s) ("
                      "f16tof32(asuint(GetAttributeAtVertex(input.p%u, 0).c) >> s) * input.barycentrics.x + "
                      "f16tof32(asuint(GetAttributeAtVertex(input.p%u, 1).c) >> s) * input.barycentrics.y + "
                      "f16tof32(asuint(GetAttributeAtVertex(input.p%u, 2).c) >> s) * input.barycentrics.z)\n",
                      i, k_mode_names[m], i, i, i);
            if (!mixed)
                h.put("#define attr%u_%s input.p%u\n", i, k_mode_names[m], i);
            else if (m == INTERP_FLAT)
                h.put("#define attr%u_flat input.p%u\n", i, i);
            else
                h.put("#define attr%u_%s (GetAttributeAtVertex(input.p%u, 0) * input.barycentrics.x + "
                      "GetAttributeAtVertex(input.p%u, 1) * input.barycentrics.y + "
                      "GetAttributeAtVertex(input.p%u, 2) "
                      "* input.barycentrics.z)\n",
                      i, k_mode_names[m], i, i, i);
        }
    }
    return true;
}

// The console thread ID VGPRs (x, y, z), from the flat index.
static const char* thread_id(Translator& t, u32 axis, u32 half) {
    const ShaderInfo& info = *t.info;
    u32 tx = info.threads[0] ? info.threads[0] : 1u;
    u32 ty = info.threads[1] ? info.threads[1] : 1u;
    u32 tz = info.threads[2] ? info.threads[2] : 1u;
    const char* c = half ? "thread1" : "thread0"; // the console flat thread index (write_compute_head)
    if (axis == 0)
        return ty * tz == 1 ? c : fmt(t, "(%s %% %uu)", c, tx);
    if (axis == 1)
        return ty == 1   ? "0u"
               : tz == 1 ? fmt(t, "(%s / %uu)", c, tx)
                         : fmt(t, "((%s / %uu) %% %uu)", c, tx, ty);
    return tz == 1 ? "0u" : fmt(t, "(%s / %uu)", c, tx * ty);
}

// Desktop groups get 32 KiB of LDS, console ones up to 64 KiB.
#define DESKTOP_MAX_LDS_BYTES 32768

// The LDS the program's group allocates (all of it where the pipeline doesn't say), or 0 without LDS
// accesses.
static u32 program_lds_bytes(const Translator& t) {
    if (!t.uses_lds)
        return 0;
    return t.info->lds_bytes ? t.info->lds_bytes : 65536;
}

// Compute with exact lanes (see translate.h): desktop groups of whole desktop waves of 32, one console
// wave each. Which threads a driver puts in a wave is its business, so waves take console wave numbers
// from a counter, and console thread IDs follow from the wave and lane.
static void write_compute_head(Translator& t, Text& h) {
    u32 lds_bytes = program_lds_bytes(t);
    // Without room for the counter next to the program's LDS, its first dword holds it at the start:
    // console LDS starts undefined.
    bool borrow = lds_bytes + 4 > DESKTOP_MAX_LDS_BYTES;
    bool counted = t.group_waves > 1;
    if (counted && !borrow)
        h.put("groupshared uint g_wave_counter;\n");
    h.put("[WaveSize(32)]\n[numthreads(%u, 1, 1)]\n", t.group_waves * 32);
    h.put("void main(%suint3 group_id : SV_GroupID) {\n", counted ? "uint gi : SV_GroupIndex, " : "");
    if (counted) {
        const char* counter = borrow ? "g_lds[0]" : "g_wave_counter";
        h.put("    if (gi == 0u) %s = 0u;\n    GroupMemoryBarrierWithGroupSync();\n", counter);
        h.put("    uint wave_index = 0u;\n    if (WaveIsFirstLane()) InterlockedAdd(%s, 1u, wave_index);\n",
              counter);
        h.put("    wave_index = WaveReadLaneFirst(wave_index);\n");
        if (borrow)
            h.put("    GroupMemoryBarrierWithGroupSync();\n");
        h.put("    uint thread0 = wave_index * %uu + WaveGetLaneIndex();\n", t.lanes * 32u);
    } else {
        h.put("    uint thread0 = WaveGetLaneIndex();\n");
    }
    if (t.lanes == 2)
        h.put("    uint thread1 = thread0 + 32u;\n");
}

static void write_declarations(Translator& t, Text& h) {
    const ShaderInfo& info = *t.info;
    h.put("%s%s", k_preamble_head, t.lanes == 2 ? k_exec_two : k_exec_one);
    h.put("%s", k_preamble);
    if (t.exact)
        h.put("%s", k_exact_helpers);
    u32 user_vec4 = (info.user_sgprs + 3) / 4;
    if (user_vec4)
        h.put("cbuffer UserData : register(b0, space100) { uint4 g_user[%u]; };\n", user_vec4);
    for (u32 i = 0; i < info.parameter_count; i++) {
        if (!(t.root_used & (1ull << i)))
            continue;
        const UserDataParameter& p = info.parameters[i];
        if (p.kind == USER_DATA_ROOT_UAV)
            h.put("RWByteAddressBuffer g_root%u : register(u%u, space100);\n", i, p.root_register);
        else
            h.put("ByteAddressBuffer g_root%u : register(t%u, space100);\n", i, p.root_register);
    }
    for (u32 i = 0; i < t.pc_pool_count; i++) {
        const PcPool& p = t.pc_pools[i];
        // One more element, zero, for reads past the end (see register_pc_pool).
        h.put("static const uint g_pc_%u_end = %uu;\n", i, p.dwords);
        h.put("static const uint g_pc_%u[%u] = {", i, p.dwords + 1);
        for (u32 w = 0; w < p.dwords; w++) {
            u32 v = 0;
            u32 off = p.base_off + w * 4;
            if (off + 4 <= t.code_size)
                memcpy(&v, t.code + off, 4);
            h.put("0x%08xu, ", v);
        }
        h.put("0u};\n");
    }
    if (t.needs_descriptor_info) {
        // A record holds a slot's descriptor, then its kind (see SHADER_SHADOW_RECORD_BYTES): the dword
        // at heap byte address a comes after the kinds of the a / 32 slots before it.
        h.put("ByteAddressBuffer g_descriptor_info : register(t0, space%d);\n",
              SHADER_DESCRIPTOR_SHADOW_SPACE);
        h.put("uint shadow_at(uint a) { return a + a / %uu * %uu; }\n", SHADER_SHADOW_DESCRIPTOR_BYTES,
              SHADER_SHADOW_RECORD_BYTES - SHADER_SHADOW_DESCRIPTOR_BYTES);
    }
    for (u32 i = 0; i < t.out->sampler_count; i++)
        h.put("%s g_sampler%u : register(s%u, space%d);\n",
              t.sampler_compare & (1u << i) ? "SamplerComparisonState" : "SamplerState", i, i,
              SHADER_INLINE_SAMPLER_SPACE);
    if (t.uses_lds)
        h.put("groupshared uint g_lds[%u];\n", program_lds_bytes(t) / 4);
    h.put("struct CbArray { uint4 c[4096]; };\n");

    if (info.stage == SHADER_VERTEX) {
        h.put("struct Input {\n");
        for (u32 i = 0; i < info.element_count; i++)
            if (t.out->vertex_element_mask & (1u << i))
                h.put("    %s e%u : E%u;\n",
                      element_hlsl_type(t.vs_fetch_components[i], t.fetch_typed[i], t.fetch_class[i]), i, i);
        h.put("    uint vertex_id : SV_VertexID;\n    uint instance_id : SV_InstanceID;\n};\n");
        h.put("struct Output {\n    float4 position : SV_Position;\n");
        if (t.vs_clip_mask)
            h.put("    float4 clip : SV_ClipDistance0;\n");
        for (u32 i = 0; i < 32; i++)
            if (t.out->vs_param_mask & (1u << i))
                h.put("    float4 p%u : P%u;\n", i, i);
        h.put("};\n");
        h.put("Output main(Input input) {\n    Output output = (Output)0;\n");
    } else if (info.stage == SHADER_PIXEL) {
        if (!write_attribute_declarations(t, h))
            return;
        h.put("struct Output {\n");
        for (u32 i = 0; i < 8; i++)
            if (t.out->ps_target_mask & (1u << i))
                h.put("    float4 t%u : SV_Target%u;\n", i, i);
        if (t.out->ps_writes_depth)
            h.put("    float depth : SV_Depth;\n");
        if (!t.out->ps_target_mask && !t.out->ps_writes_depth)
            h.put("    float4 unused : SV_Target7;\n"); // an empty struct cannot be a return type
        h.put("};\n");
        h.put("Output main(Input input) {\n    Output output = (Output)0;\n");
    } else {
        write_compute_head(t, h);
    }

    // Registers: user SGPRs from the root constants, system values, the rest zero.
    for (u32 r = 0; r < 128; r++) {
        if (!t.sgpr_used[r] && r != SRC_EXEC_LO && r != SRC_EXEC_HI)
            continue;
        if (r < info.user_sgprs)
            h.put("    uint s%u = g_user[%u].%c;\n", r, r / 4, "xyzw"[r % 4]);
        else
            h.put("    uint s%u = 0u;\n", r);
    }
    if (info.stage == SHADER_COMPUTE) {
        u32 at = info.user_sgprs;
        for (u32 axis = 0; axis < 3; axis++)
            if (info.group_id_mask & (1u << axis)) {
                if (t.sgpr_used[at])
                    h.put("    s%u = group_id.%c;\n", at, "xyz"[axis]);
                at++;
            }
    }
    // EXEC starts with the lanes that run; in pixel shaders, helper lanes are off until WQM. Compute
    // groups that don't fill their last console wave have its last lanes off.
    bool partial = info.stage == SHADER_COMPUTE && t.wave_threads % (t.lanes * 32u) != 0;
    if (info.stage == SHADER_PIXEL)
        h.put("    s126 = mask_of(!IsHelperLane());\n    s127 = s126;\n");
    else if (!partial)
        h.put("    s126 = 0xffffffffu;\n    s127 = 0xffffffffu;\n");
    else if (t.lanes == 2)
        h.put("    s126 = mask_of(thread0 < %uu);\n    s127 = mask_of(thread1 < %uu);\n", t.wave_threads,
              t.wave_threads);
    else
        h.put("    s126 = mask_of(thread0 < %uu);\n    s127 = s126;\n", t.wave_threads);
    if (t.scc_used)
        h.put("    bool scc = false;\n");
    if (t.far_jumps)
        h.put("    uint go_ = 0u;\n");
    for (u32 r = 0; r < 256; r++) {
        if (!t.vgpr_used[r])
            continue;
        if (info.stage == SHADER_COMPUTE) {
            bool id = r < info.thread_id_count;
            if (t.lanes == 2)
                h.put("    uint v%u_0 = %s, v%u_1 = %s;\n", r, id ? thread_id(t, r, 0) : "0u", r,
                      id ? thread_id(t, r, 1) : "0u");
            else
                h.put("    uint v%u = %s;\n", r, id ? thread_id(t, r, 0) : "0u");
            continue;
        }
        const char* init = "0u";
        if (info.stage == SHADER_VERTEX) {
            if (r == 0)
                init = "input.vertex_id";
            else if (r == 3)
                init = "input.instance_id";
        } else {
            static const char* const k_pos[4] = {"asuint(input.position.x)", "asuint(input.position.y)",
                                                 "asuint(input.position.z)", "asuint(input.position.w)"};
            for (u32 i = PS_POS_X; i <= PS_POS_W; i++)
                if (t.ps_input_vgpr[i] == r)
                    init = k_pos[i - PS_POS_X];
            if (t.ps_input_vgpr[PS_FRONT_FACE] == r)
                init = "(input.front_face ? 0x3f800000u : 0xbf800000u)";
            // The pixel's integer position, x in the low half and y in the high half.
            if (t.ps_input_vgpr[PS_POS_FIXED_PT] == r)
                init = "((uint)input.position.x | ((uint)input.position.y << 16))";
            if (t.ps_input_vgpr[PS_SAMPLE_COVERAGE] == r)
                init = "input.coverage";
        }
        h.put("    uint v%u = %s;\n", r, init);
    }
    for (u32 slot = 0; slot < t.spill_vgpr_count; slot++)
        for (u32 lane = 0; lane < 64; lane++)
            if (t.spill_lanes[slot] & (1ull << lane))
                h.put("    uint spill%u_%u = 0u;\n", slot, lane);
}

static bool is_exec64_operand(const RdnaOperand& o) {
    return o.kind == OPND_SPECIAL && ((o.reg == SRC_EXEC_LO && o.count == 2) || o.reg == SRC_EXEC_HI);
}

static bool is_exec32_operand(const RdnaOperand& o) {
    return o.kind == OPND_SPECIAL && o.reg == SRC_EXEC_LO && o.count == 1;
}

void detect_wave_size(Translator& t) {
    bool has_exec64 = false;
    bool has_exec32 = false;
    for (u32 i = 0; i < t.program.count; i++) {
        const RdnaInst& in = t.program.insts[i];
        if ((in.op >= OP_S_AND_SAVEEXEC_B64 && in.op <= OP_S_XNOR_SAVEEXEC_B64) ||
            (in.op >= OP_S_ANDN1_SAVEEXEC_B64 && in.op <= OP_S_ANDN2_WREXEC_B64) || in.op == OP_S_WQM_B64)
            has_exec64 = true;
        if ((in.op >= OP_S_AND_SAVEEXEC_B32 && in.op <= OP_S_ANDN2_WREXEC_B32) || in.op == OP_S_WQM_B32)
            has_exec32 = true;
        RdnaEncoding base = rdna_op_encoding(in.op);
        if (base == ENC_SOP1 || base == ENC_SOP2 || base == ENC_SOPC || base == ENC_SOPK) {
            for (u32 d = 0; d < 2; d++) {
                if (is_exec64_operand(in.dst[d]))
                    has_exec64 = true;
                else if (is_exec32_operand(in.dst[d]))
                    has_exec32 = true;
            }
            for (u32 s = 0; s < in.num_src; s++) {
                if (is_exec64_operand(in.src[s]))
                    has_exec64 = true;
                else if (is_exec32_operand(in.src[s]))
                    has_exec32 = true;
            }
        }
        // Wave64 requires even-aligned SGPR pairs for VOPC / VOP3 carry-out masks; odd SGPRs prove Wave32.
        if ((base == ENC_VOPC && in.dst[0].kind == OPND_SGPR && (in.dst[0].reg & 1u)) ||
            (in.enc == ENC_VOP3 && in.dst[1].kind == OPND_SGPR && (in.dst[1].reg & 1u)))
            has_exec32 = true;
    }
    // What the code proves wins (the pipeline's wave size is not verified yet); the pipeline's decides
    // the programs whose code works either way, and wave64 if it doesn't say either.
    u32 code = has_exec64 ? 64 : has_exec32 ? 32 : 0;
    t.out->code_wave_size = (u8)code;
    t.wave32 = code ? code == 32 : t.info->wave_size == 32;
}

void emit_program(Translator& t) {
    if (t.info->stage == SHADER_PIXEL)
        layout_ps_inputs(t);
    t.depth = 1;
    t.sgpr_used[SRC_EXEC_LO] = t.sgpr_used[SRC_EXEC_HI] = 1;
    emit_control_flow(t);
    if (t.failed)
        return;
    if (t.info->stage == SHADER_VERTEX && !t.position_exported) {
        translator_fail(t, "vertex shader without a position export");
        return;
    }
    if (program_lds_bytes(t) > DESKTOP_MAX_LDS_BYTES) {
        translator_fail(t, "%u bytes of LDS, more than a desktop group has", program_lds_bytes(t));
        return;
    }
    // Desktop input formats for the fetched elements.
    for (u32 i = 0; i < t.info->element_count; i++)
        if (t.out->vertex_element_mask & (1u << i))
            t.out->element_format[i] =
                t.fetch_typed[i] ? t.info->elements[i].format : raw_element_format(t.vs_fetch_components[i]);
    Text& h = t.out->hlsl;
    write_declarations(t, h);
    h.put_raw(t.body.data ? t.body.data : "", t.body.length);
    if (t.info->stage != SHADER_COMPUTE)
        h.put("    return output;\n");
    h.put("}\n");
}
