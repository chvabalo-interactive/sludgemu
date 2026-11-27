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
    use_vgpr(t, reg);
    return fmt(t, "v%u", reg & 255);
}

u32 mask_dwords(const Translator& t) {
    return t.wave32 ? 1 : 2;
}

static const u32 k_inline_f32[9] = {0x3f000000, 0xbf000000, 0x3f800000, 0xbf800000, 0x40000000,
                                    0xc0000000, 0x40800000, 0xc0800000, 0x3e22f983};
static const u32 k_inline_f16[9] = {0x3800, 0xb800, 0x3c00, 0xbc00, 0x4000, 0xc000, 0x4400, 0xc400, 0x3118};
static const u32 k_inline_f64_hi[9] = {0x3fe00000, 0xbfe00000, 0x3ff00000, 0xbff00000, 0x40000000,
                                       0xc0000000, 0x40100000, 0xc0100000, 0x3fc45f30};

static const char* operand_u32(Translator& t, const RdnaOperand& o, OperandType type, u32 part) {
    switch (o.kind) {
    case OPND_SGPR:
        return sgpr(t, o.reg + part);
    case OPND_VGPR:
        return vgpr(t, o.reg + part);
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
            return sgpr(t, o.reg + part);
        case SRC_NULL:
            return "0u";
        case SRC_SCC:
            t.scc_used = true;
            return "(scc ? 1u : 0u)";
        case SRC_VCCZ:
            return "(wave_any(s106) ? 0u : 1u)";
        case SRC_EXECZ:
            return "(wave_any(s126) ? 0u : 1u)";
        default:
            translator_fail(t, "unsupported operand %u", o.reg);
            return "0u";
        }
    default:
        translator_fail(t, "missing operand");
        return "0u";
    }
}

const char* register_data(Translator& t, Tag tag, const char* raw, u16 descriptor, bool vector) {
    if (!heap_loaded(tag) || t.copying)
        return raw;
    u32 payload = tag_payload(tag);
    if (payload & HEAP_MODIFIED)
        translator_fail(t, "register read as a value at 0x%x holds a descriptor dword on some paths only",
                        t.inst_offset);
    else if ((payload & 1) == HEAP_SAMPLERS)
        translator_fail(t, "sampler descriptor dword read as a value at 0x%x", t.inst_offset);
    else if ((payload & HEAP_IMAGE) || (slot_use(t, tag, descriptor) & SLOT_IMAGE))
        translator_fail(t, "image descriptor dword read as a value at 0x%x (the shadow has buffers' only)",
                        t.inst_offset);
    // The shadow lays out each desktop slot's descriptor bits like console heap memory: the dword at
    // heap byte address A is word A of it. Lanes outside EXEC may hold anything in a vector register,
    // and the shadow, a root descriptor, has no bounds: they read the first record instead.
    t.needs_descriptor_info = true;
    if (vector)
        return fmt(t, "g_descriptor_info.Load(EXEC ? %s : 0u)", raw);
    return fmt(t, "g_descriptor_info.Load(%s)", raw);
}

const char* sgpr_data(Translator& t, u32 reg) {
    return register_data(t, t.tags.sgpr[reg & 127], sgpr(t, reg), t.tags.descriptor[reg & 127], false);
}

const char* vgpr_data(Translator& t, u32 reg) {
    return register_data(t, t.tags.vgpr[reg & 255], vgpr(t, reg), NO_DESCRIPTOR, true);
}

// A register operand (dword `part`) read as a value; `raw` for anything else. Scalar registers are the
// ones tags_transfer tracks (tag_of): SGPRs, VCC, M0 and EXEC.
static const char* operand_data(Translator& t, const RdnaOperand& o, u32 part, const char* raw) {
    if (o.kind == OPND_SGPR || (o.kind == OPND_SPECIAL && o.reg < 128 && o.reg != SRC_NULL)) {
        u32 reg = (o.reg + part) & 127;
        return register_data(t, t.tags.sgpr[reg], raw, t.tags.descriptor[reg], false);
    }
    if (o.kind == OPND_VGPR)
        return register_data(t, t.tags.vgpr[(o.reg + part) & 255], raw, NO_DESCRIPTOR, true);
    return raw;
}

const char* src_u32(Translator& t, const RdnaInst& in, u32 i, OperandType type, u32 part) {
    if (t.dpp_active && i == 0 && part == 0)
        return "dpp0_";
    const RdnaOperand& o = in.src[i];
    if (o.kind == OPND_SPECIAL && (o.reg == SRC_VCC_LO || o.reg == SRC_EXEC_LO))
        t.sgpr_used[o.reg] = 1;
    const char* value = operand_data(t, o, part, operand_u32(t, o, type, part));
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

void write_vgpr(Translator& t, const RdnaInst& in, u32 reg, const char* value) {
    const char* dst = vgpr(t, reg);
    if (in.has_sdwa && in.sdwa.dst_sel != SDWA_DWORD) {
        u32 sel = in.sdwa.dst_sel;
        u32 shift = sel <= SDWA_BYTE_3 ? sel * 8 : (sel - SDWA_WORD_0) * 16;
        u32 width = sel <= SDWA_BYTE_3 ? 8 : 16;
        u32 field = ((1u << width) - 1) << shift;
        const char* bits = fmt(t, "((%s << %u) & 0x%08xu)", value, shift, field);
        if (in.sdwa.dst_unused == 2) // preserve the other bits
            value = fmt(t, "((%s & 0x%08xu) | %s)", dst, ~field, bits);
        else
            value = bits;
    }
    emit(t, t.dpp_has_mask ? "if (EXEC && dpp_en_) %s = %s;" : "if (EXEC) %s = %s;", dst, value);
}

void write_sgpr(Translator& t, const RdnaOperand& dst, u32 part, const char* value) {
    if (dst.kind == OPND_SPECIAL && dst.reg == SRC_NULL)
        return;
    if (dst.kind != OPND_SGPR && dst.kind != OPND_SPECIAL) {
        translator_fail(t, "bad scalar destination");
        return;
    }
    emit(t, "%s = %s;", sgpr(t, dst.reg + part), value);
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

static bool emit_interpolation(Translator& t, const RdnaInst& in) {
    if (t.info->stage != SHADER_PIXEL) {
        translator_fail(t, "interpolation outside a pixel shader");
        return false;
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
    t.out->ps_attribute_mask |= 1u << slot;
    // The P1 step is folded into P2: only the final value is observable.
    if (in.op == OP_V_INTERP_P1_F32)
        return true;
    // Read through a per-mode name the declarations define (see write_attribute_declarations).
    write_vgpr(t, in, in.dst[0].reg,
               fmt(t, "asuint(attr%u_%s.%c)", slot, k_mode_names[mode], "xyzw"[in.attr_chan & 3]));
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

static void emit_block(Translator& t, u32 b) {
    const Block& blk = t.program.blocks[b];
    t.tags = t.block_entry[b];
    for (u32 i = blk.first; i < blk.end && !t.failed; i++) {
        const RdnaInst& in = t.program.insts[i];
        t.inst_offset = in.offset;
        t.copying = copies_tags(t, in, i);
        emit_instruction(t, in);
        tags_transfer(t, t.tags, in, i);
    }
}

// The condition under which a block's last instruction branches.
static const char* branch_condition(Translator& t, const RdnaInst& in) {
    switch (in.op) {
    case OP_S_CBRANCH_SCC0:
        t.scc_used = true;
        return "!scc";
    case OP_S_CBRANCH_SCC1:
        t.scc_used = true;
        return "scc";
    case OP_S_CBRANCH_VCCZ:
        use_sgpr(t, SRC_VCC_LO);
        return "!wave_any(s106)";
    case OP_S_CBRANCH_VCCNZ:
        use_sgpr(t, SRC_VCC_LO);
        return "wave_any(s106)";
    case OP_S_CBRANCH_EXECZ:
        return "!wave_any(s126)";
    case OP_S_CBRANCH_EXECNZ:
        return "wave_any(s126)";
    default:
        return "true";
    }
}

#define PC_EXIT 0xFFFFFFFFu

// Scalar registers that hold a loaded descriptor dword's address at the end of a block but a value where
// block `next` starts (another way in holds a value there, see tags_merge): read from the shadow before
// the jump. With `emitting` false, only whether there are any.
static bool emit_join_conversions(Translator& t, s32 next, bool emitting) {
    if (next < 0)
        return false;
    const TagState& entry = t.block_entry[next];
    bool any = false;
    const u64* live = &t.live_sgprs[2 * next];
    for (u32 r = 0; r < 128; r++) {
        if (!join_converts(t.tags.sgpr[r]) || heap_loaded(entry.sgpr[r]) || !((live[r / 64] >> (r % 64)) & 1))
            continue;
        any = true;
        if (emitting) {
            t.needs_descriptor_info = true;
            emit(t, "%s = g_descriptor_info.Load(%s);", sgpr(t, r), sgpr(t, r));
        }
    }
    for (u32 slot = 0; slot < t.spill_vgpr_count; slot++)
        for (u32 lane = 0; lane < 64; lane++) {
            if (!join_converts(t.tags.spill_tag[slot][lane]) || heap_loaded(entry.spill_tag[slot][lane]))
                continue;
            any = true;
            if (emitting) {
                t.needs_descriptor_info = true;
                emit(t, "spill%u_%u = g_descriptor_info.Load(spill%u_%u);", slot, lane, slot, lane);
            }
        }
    return any;
}

// Several blocks: a loop over a switch on the block number. Branch conditions are uniform, so the
// whole wave always takes the same case.
static void emit_blocks(Translator& t) {
    const Program& p = t.program;
    if (p.block_count == 1) {
        emit_block(t, 0);
        return;
    }
    emit(t, "uint pc = 0;");
    emit(t, "[loop] while (pc != 0x%08xu) {", PC_EXIT);
    t.depth++;
    emit(t, "switch (pc) {");
    for (u32 b = 0; b < p.block_count && !t.failed; b++) {
        emit(t, "case %u: {", b);
        t.depth++;
        emit_block(t, b);
        const Block& blk = p.blocks[b];
        const RdnaInst& last = p.insts[blk.end - 1];
        u32 next0 = blk.next[0] < 0 ? PC_EXIT : (u32)blk.next[0];
        bool convert0 = emit_join_conversions(t, blk.next[0], false);
        bool convert1 = emit_join_conversions(t, blk.next[1], false);
        if (blk.next[1] >= 0 && (convert0 || convert1)) {
            emit(t, "{ bool taken_ = %s;", branch_condition(t, last));
            t.depth++;
            emit(t, "if (taken_) {");
            t.depth++;
            emit_join_conversions(t, blk.next[0], true);
            t.depth--;
            emit(t, "} else {");
            t.depth++;
            emit_join_conversions(t, blk.next[1], true);
            t.depth--;
            emit(t, "}");
            emit(t, "pc = taken_ ? %uu : %uu;", next0, (u32)blk.next[1]);
            t.depth--;
            emit(t, "}");
        } else if (blk.next[1] >= 0) {
            emit(t, "pc = %s ? %uu : %uu;", branch_condition(t, last), next0, (u32)blk.next[1]);
        } else {
            emit_join_conversions(t, blk.next[0], true);
            emit(t, "pc = %uu;", next0);
        }
        emit(t, "break;");
        t.depth--;
        emit(t, "}");
    }
    emit(t, "default: pc = 0x%08xu; break;", PC_EXIT);
    emit(t, "}");
    t.depth--;
    emit(t, "}");
}

// --- the program -------------------------------------------------------------------------------------

static const char k_preamble[] =
    "// Translated console shader.\n"
    "#define EXEC (s126 != 0u)\n"
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
    "uint read_first(uint v, uint m) { uint lane = WaveActiveMin(m != 0u ? WaveGetLaneIndex() : 0xffffffffu); "
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
    "0.0f ? -v.z : v.z) : -v.y; }\n";

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
// per instruction); such attributes are declared flat and interpolated with the barycentrics.
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
        if (modes & (modes - 1)) {
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
        bool mixed = (modes & (modes - 1)) != 0;
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
        bool mixed = (modes & (modes - 1)) != 0;
        for (u32 m = 1; m < 8; m++) {
            if (!(modes & (1u << m)))
                continue;
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

static void write_declarations(Translator& t, Text& h) {
    const ShaderInfo& info = *t.info;
    h.put("%s", k_preamble);
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
    if (t.needs_descriptor_info)
        h.put("ByteAddressBuffer g_descriptor_info : register(t0, space%d);\n",
              SHADER_DESCRIPTOR_SHADOW_SPACE);
    for (u32 i = 0; i < t.out->sampler_count; i++)
        h.put("%s g_sampler%u : register(s%u, space%d);\n",
              t.sampler_compare & (1u << i) ? "SamplerComparisonState" : "SamplerState", i, i,
              SHADER_INLINE_SAMPLER_SPACE);
    if (t.uses_lds)
        h.put("groupshared uint g_lds[%u];\n", (info.lds_bytes ? info.lds_bytes : 65536) / 4);
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
        h.put("[numthreads(%u, %u, %u)]\n", info.threads[0] ? info.threads[0] : 1,
              info.threads[1] ? info.threads[1] : 1, info.threads[2] ? info.threads[2] : 1);
        h.put("void main(uint3 thread_id : SV_GroupThreadID, uint3 group_id : SV_GroupID) {\n");
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
    // EXEC starts with the lanes that run; in pixel shaders, helper lanes are off until WQM.
    if (info.stage == SHADER_PIXEL)
        h.put("    s126 = mask_of(!IsHelperLane());\n    s127 = s126;\n");
    else
        h.put("    s126 = 0xffffffffu;\n    s127 = 0xffffffffu;\n");
    if (t.scc_used)
        h.put("    bool scc = false;\n");
    for (u32 r = 0; r < 256; r++) {
        if (!t.vgpr_used[r])
            continue;
        const char* init = "0u";
        if (info.stage == SHADER_VERTEX) {
            if (r == 0)
                init = "input.vertex_id";
            else if (r == 3)
                init = "input.instance_id";
        } else if (info.stage == SHADER_COMPUTE) {
            if (r < info.thread_id_count)
                init = r == 0 ? "thread_id.x" : r == 1 ? "thread_id.y" : "thread_id.z";
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
    t.wave32 = !has_exec64 && has_exec32;
}

void emit_program(Translator& t) {
    if (t.info->stage == SHADER_PIXEL)
        layout_ps_inputs(t);
    t.depth = 1;
    t.sgpr_used[SRC_EXEC_LO] = t.sgpr_used[SRC_EXEC_HI] = 1;
    emit_blocks(t);
    if (t.failed)
        return;
    if (t.info->stage == SHADER_VERTEX && !t.position_exported) {
        translator_fail(t, "vertex shader without a position export");
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
