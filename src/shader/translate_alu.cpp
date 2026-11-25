// Console shader translation: scalar and vector ALU instructions.

#include <string.h>

#include "shader/translate_internal.h"

// --- scalar ------------------------------------------------------------------------------------------

static const char* src(Translator& t, const RdnaInst& in, u32 i, u32 part = 0) {
    return src_u32(t, in, i, TYPE_32, part);
}

static const char* src64(Translator& t, const RdnaInst& in, u32 i) {
    return fmt(t, "((uint64_t)%s | ((uint64_t)%s << 32))", src(t, in, i, 0), src(t, in, i, 1));
}

static void set_scc(Translator& t, const char* condition) {
    t.scc_used = true;
    emit(t, "scc = %s;", condition);
}

static void write64(Translator& t, const RdnaOperand& dst, const char* value) {
    emit(t, "{ uint64_t r_ = %s;", value);
    t.depth++;
    write_sgpr(t, dst, 0, "(uint)r_");
    write_sgpr(t, dst, 1, "(uint)(r_ >> 32)");
    t.depth--;
    emit(t, "}");
}

// The result register's SCC for logical operations: masks count lanes across the wave.
static bool result_is_mask(const Translator& t, const RdnaInst& in) {
    const RdnaOperand& d = in.dst[0];
    if (d.kind == OPND_SPECIAL && (d.reg == SRC_EXEC_LO || d.reg == SRC_VCC_LO))
        return true;
    for (u32 i = 0; i < in.num_src; i++) {
        const RdnaOperand& s = in.src[i];
        if (s.kind == OPND_SPECIAL && (s.reg == SRC_EXEC_LO || s.reg == SRC_VCC_LO))
            return true;
        if ((s.kind == OPND_SGPR) && tag_kind(t.tags.sgpr[s.reg & 127]) == TAG_MASK)
            return true;
    }
    return false;
}

static const char* logic_expr(Translator& t, RdnaOp op, const char* a, const char* b) {
    switch (op) {
    case OP_S_AND_B32:
    case OP_S_AND_B64:
        return fmt(t, "(%s & %s)", a, b);
    case OP_S_OR_B32:
    case OP_S_OR_B64:
        return fmt(t, "(%s | %s)", a, b);
    case OP_S_XOR_B32:
    case OP_S_XOR_B64:
        return fmt(t, "(%s ^ %s)", a, b);
    case OP_S_ANDN2_B32:
    case OP_S_ANDN2_B64:
        return fmt(t, "(%s & ~%s)", a, b);
    case OP_S_ORN2_B32:
    case OP_S_ORN2_B64:
        return fmt(t, "(%s | ~%s)", a, b);
    case OP_S_NAND_B32:
    case OP_S_NAND_B64:
        return fmt(t, "~(%s & %s)", a, b);
    case OP_S_NOR_B32:
    case OP_S_NOR_B64:
        return fmt(t, "~(%s | %s)", a, b);
    case OP_S_XNOR_B32:
    case OP_S_XNOR_B64:
        return fmt(t, "~(%s ^ %s)", a, b);
    default:
        return "0u";
    }
}

// The SAVEEXEC and WREXEC families: SAVEEXEC writes the old EXEC to dst, WREXEC writes the new EXEC.
static bool emit_saveexec(Translator& t, const RdnaInst& in) {
    const char* e = "s126";
    const char* s = src(t, in, 0);
    const char* value;
    bool wrexec = false;
    switch (in.op) {
    case OP_S_AND_SAVEEXEC_B64:
    case OP_S_AND_SAVEEXEC_B32:
        value = fmt(t, "(%s & %s)", s, e);
        break;
    case OP_S_OR_SAVEEXEC_B64:
    case OP_S_OR_SAVEEXEC_B32:
        value = fmt(t, "(%s | %s)", s, e);
        break;
    case OP_S_XOR_SAVEEXEC_B64:
    case OP_S_XOR_SAVEEXEC_B32:
        value = fmt(t, "(%s ^ %s)", s, e);
        break;
    case OP_S_ANDN2_SAVEEXEC_B64:
    case OP_S_ANDN2_SAVEEXEC_B32:
        value = fmt(t, "(%s & ~%s)", s, e);
        break;
    case OP_S_ORN2_SAVEEXEC_B64:
    case OP_S_ORN2_SAVEEXEC_B32:
        value = fmt(t, "(%s | ~%s)", s, e);
        break;
    case OP_S_NAND_SAVEEXEC_B64:
    case OP_S_NAND_SAVEEXEC_B32:
        value = fmt(t, "~(%s & %s)", s, e);
        break;
    case OP_S_NOR_SAVEEXEC_B64:
    case OP_S_NOR_SAVEEXEC_B32:
        value = fmt(t, "~(%s | %s)", s, e);
        break;
    case OP_S_XNOR_SAVEEXEC_B64:
    case OP_S_XNOR_SAVEEXEC_B32:
        value = fmt(t, "~(%s ^ %s)", s, e);
        break;
    case OP_S_ANDN1_SAVEEXEC_B64:
    case OP_S_ANDN1_SAVEEXEC_B32:
        value = fmt(t, "(~%s & %s)", s, e);
        break;
    case OP_S_ORN1_SAVEEXEC_B64:
    case OP_S_ORN1_SAVEEXEC_B32:
        value = fmt(t, "(~%s | %s)", s, e);
        break;
    case OP_S_ANDN1_WREXEC_B64:
    case OP_S_ANDN1_WREXEC_B32:
        value = fmt(t, "(~%s & %s)", s, e);
        wrexec = true;
        break;
    case OP_S_ANDN2_WREXEC_B64:
    case OP_S_ANDN2_WREXEC_B32:
        value = fmt(t, "(%s & ~%s)", s, e);
        wrexec = true;
        break;
    default:
        translator_fail(t, "unsupported %s", rdna_op_name(in.op));
        return false;
    }
    emit(t, "{ uint old_ = s126; uint new_ = %s;", value);
    t.depth++;
    emit(t, "s126 = new_; s127 = new_;");
    const char* dst_val = wrexec ? "new_" : "old_";
    write_sgpr(t, in.dst[0], 0, dst_val);
    if (!t.wave32)
        write_sgpr(t, in.dst[0], 1, dst_val);
    t.depth--;
    emit(t, "}");
    set_scc(t, "wave_any(s126)");
    return true;
}

static bool emit_scalar(Translator& t, const RdnaInst& in) {
    const RdnaOperand& d = in.dst[0];
    switch (in.op) {
    case OP_S_MOV_B32:
        write_sgpr(t, d, 0, src(t, in, 0));
        return true;
    case OP_S_MOV_B64:
        write_sgpr(t, d, 0, src(t, in, 0));
        write_sgpr(t, d, 1, src(t, in, 0, 1));
        return true;
    case OP_S_CMOV_B32:
        t.scc_used = true;
        emit(t, "if (scc) %s = %s;", sgpr(t, d.reg), src(t, in, 0));
        return true;
    case OP_S_CMOV_B64:
        t.scc_used = true;
        emit(t, "if (scc) { %s = %s; %s = %s; }", sgpr(t, d.reg), src(t, in, 0), sgpr(t, d.reg + 1),
             src(t, in, 0, 1));
        return true;
    case OP_S_MOVK_I32:
        write_sgpr(t, d, 0, fmt(t, "0x%08xu", (u32)in.simm16));
        return true;
    case OP_S_CMOVK_I32:
        t.scc_used = true;
        emit(t, "if (scc) %s = 0x%08xu;", sgpr(t, d.reg), (u32)in.simm16);
        return true;
    case OP_S_NOT_B32:
    case OP_S_NOT_B64: {
        bool mask = result_is_mask(t, in);
        for (u32 i = 0; i < d.count; i++)
            write_sgpr(t, d, i, fmt(t, "~%s", src(t, in, 0, i)));
        set_scc(t, mask ? fmt(t, "wave_any(%s)", sgpr(t, d.reg)) : fmt(t, "%s != 0u", sgpr(t, d.reg)));
        return true;
    }
    case OP_S_WQM_B32:
    case OP_S_WQM_B64:
        for (u32 i = 0; i < d.count; i++)
            write_sgpr(t, d, i, fmt(t, "quad_any(%s)", src(t, in, 0, i)));
        set_scc(t, fmt(t, "wave_any(%s)", sgpr(t, d.reg)));
        return true;
    case OP_S_AND_B32:
    case OP_S_OR_B32:
    case OP_S_XOR_B32:
    case OP_S_ANDN2_B32:
    case OP_S_ORN2_B32:
    case OP_S_NAND_B32:
    case OP_S_NOR_B32:
    case OP_S_XNOR_B32:
    case OP_S_AND_B64:
    case OP_S_OR_B64:
    case OP_S_XOR_B64:
    case OP_S_ANDN2_B64:
    case OP_S_ORN2_B64:
    case OP_S_NAND_B64:
    case OP_S_NOR_B64:
    case OP_S_XNOR_B64: {
        bool mask = result_is_mask(t, in);
        emit(t, "{");
        t.depth++;
        for (u32 i = 0; i < d.count; i++)
            emit(t, "uint r%u_ = %s;", i, logic_expr(t, in.op, src(t, in, 0, i), src(t, in, 1, i)));
        for (u32 i = 0; i < d.count; i++)
            write_sgpr(t, d, i, fmt(t, "r%u_", i));
        if (mask)
            set_scc(t, "wave_any(r0_)");
        else
            set_scc(t, d.count == 2 ? "(r0_ | r1_) != 0u" : "r0_ != 0u");
        t.depth--;
        emit(t, "}");
        return true;
    }
    case OP_S_CSELECT_B32:
    case OP_S_CSELECT_B64:
        t.scc_used = true;
        emit(t, "{");
        t.depth++;
        for (u32 i = 0; i < d.count; i++)
            emit(t, "uint r%u_ = scc ? %s : %s;", i, src(t, in, 0, i), src(t, in, 1, i));
        for (u32 i = 0; i < d.count; i++)
            write_sgpr(t, d, i, fmt(t, "r%u_", i));
        t.depth--;
        emit(t, "}");
        return true;
    case OP_S_ADD_U32:
        emit(t, "{ uint a_ = %s, b_ = %s, r_ = a_ + b_; scc = r_ < a_;", src(t, in, 0), src(t, in, 1));
        break;
    case OP_S_SUB_U32:
        emit(t, "{ uint a_ = %s, b_ = %s, r_ = a_ - b_; scc = b_ > a_;", src(t, in, 0), src(t, in, 1));
        break;
    case OP_S_ADDC_U32:
        emit(t,
             "{ uint a_ = %s, b_ = %s, c_ = scc ? 1u : 0u, r_ = a_ + b_ + c_; scc = r_ < a_ || (c_ != 0u && "
             "r_ "
             "== a_);",
             src(t, in, 0), src(t, in, 1));
        break;
    case OP_S_SUBB_U32:
        emit(t,
             "{ uint a_ = %s, b_ = %s, c_ = scc ? 1u : 0u, r_ = a_ - b_ - c_; scc = (uint64_t)b_ + c_ > a_;",
             src(t, in, 0), src(t, in, 1));
        break;
    case OP_S_ADD_I32:
        emit(t, "{ uint a_ = %s, b_ = %s, r_ = a_ + b_; scc = ((~(a_ ^ b_)) & (a_ ^ r_)) >> 31 != 0u;",
             src(t, in, 0), src(t, in, 1));
        break;
    case OP_S_SUB_I32:
        emit(t, "{ uint a_ = %s, b_ = %s, r_ = a_ - b_; scc = ((a_ ^ b_) & (a_ ^ r_)) >> 31 != 0u;",
             src(t, in, 0), src(t, in, 1));
        break;
    case OP_S_ADDK_I32:
        emit(t, "{ uint a_ = %s, b_ = 0x%08xu, r_ = a_ + b_; scc = ((~(a_ ^ b_)) & (a_ ^ r_)) >> 31 != 0u;",
             sgpr(t, d.reg), (u32)in.simm16);
        break;
    case OP_S_MULK_I32:
        emit(t, "{ uint r_ = %s * 0x%08xu;", sgpr(t, d.reg), (u32)in.simm16);
        break;
    case OP_S_MIN_I32:
    case OP_S_MAX_I32:
    case OP_S_MIN_U32:
    case OP_S_MAX_U32: {
        bool is_signed = in.op == OP_S_MIN_I32 || in.op == OP_S_MAX_I32;
        bool is_min = in.op == OP_S_MIN_I32 || in.op == OP_S_MIN_U32;
        const char* cast = is_signed ? "(int)" : "";
        emit(t, "{ uint a_ = %s, b_ = %s; scc = %sa_ %s %sb_; uint r_ = scc ? a_ : b_;", src(t, in, 0),
             src(t, in, 1), cast, is_min ? "<" : ">", cast);
        break;
    }
    case OP_S_ABSDIFF_I32:
        emit(t, "{ uint r_ = (uint)abs((int)%s - (int)%s); scc = r_ != 0u;", src(t, in, 0), src(t, in, 1));
        break;
    case OP_S_LSHL_B32:
        emit(t, "{ uint r_ = %s << (%s & 31u); scc = r_ != 0u;", src(t, in, 0), src(t, in, 1));
        break;
    case OP_S_LSHR_B32:
        emit(t, "{ uint r_ = %s >> (%s & 31u); scc = r_ != 0u;", src(t, in, 0), src(t, in, 1));
        break;
    case OP_S_ASHR_I32:
        emit(t, "{ uint r_ = (uint)((int)%s >> (%s & 31u)); scc = r_ != 0u;", src(t, in, 0), src(t, in, 1));
        break;
    case OP_S_LSHL1_ADD_U32:
    case OP_S_LSHL2_ADD_U32:
    case OP_S_LSHL3_ADD_U32:
    case OP_S_LSHL4_ADD_U32: {
        u32 shift = (u32)(in.op - OP_S_LSHL1_ADD_U32) + 1;
        emit(t, "{ uint64_t w_ = ((uint64_t)%s << %u) + %s; uint r_ = (uint)w_; scc = (w_ >> 32) != 0u;",
             src(t, in, 0), shift, src(t, in, 1));
        break;
    }
    case OP_S_BFE_U32:
        emit(t, "{ uint c_ = %s; uint r_ = bfe_u32(%s, c_, c_ >> 16); scc = r_ != 0u;", src(t, in, 1),
             src(t, in, 0));
        break;
    case OP_S_BFE_I32:
        emit(t, "{ uint c_ = %s; uint r_ = (uint)bfe_i32(%s, c_, c_ >> 16); scc = r_ != 0u;", src(t, in, 1),
             src(t, in, 0));
        break;
    case OP_S_BFM_B32:
        emit(t, "{ uint r_ = ((1u << (%s & 31u)) - 1u) << (%s & 31u);", src(t, in, 0), src(t, in, 1));
        break;
    case OP_S_MUL_I32:
        emit(t, "{ uint r_ = %s * %s;", src(t, in, 0), src(t, in, 1));
        break;
    case OP_S_MUL_HI_U32:
        emit(t, "{ uint r_ = (uint)(((uint64_t)%s * %s) >> 32);", src(t, in, 0), src(t, in, 1));
        break;
    case OP_S_MUL_HI_I32:
        emit(t, "{ uint r_ = (uint)(((int64_t)(int)%s * (int)%s) >> 32);", src(t, in, 0), src(t, in, 1));
        break;
    case OP_S_PACK_LL_B32_B16:
        emit(t, "{ uint r_ = (%s & 0xffffu) | (%s << 16);", src(t, in, 0), src(t, in, 1));
        break;
    case OP_S_PACK_LH_B32_B16:
        emit(t, "{ uint r_ = (%s & 0xffffu) | (%s & 0xffff0000u);", src(t, in, 0), src(t, in, 1));
        break;
    case OP_S_PACK_HH_B32_B16:
        emit(t, "{ uint r_ = (%s >> 16) | (%s & 0xffff0000u);", src(t, in, 0), src(t, in, 1));
        break;
    case OP_S_BREV_B32:
        emit(t, "{ uint r_ = reversebits(%s);", src(t, in, 0));
        break;
    case OP_S_SEXT_I32_I8:
        emit(t, "{ uint r_ = (uint)((int)(%s << 24) >> 24);", src(t, in, 0));
        break;
    case OP_S_SEXT_I32_I16:
        emit(t, "{ uint r_ = (uint)((int)(%s << 16) >> 16);", src(t, in, 0));
        break;
    case OP_S_ABS_I32:
        emit(t, "{ uint r_ = (uint)abs((int)%s); scc = r_ != 0u;", src(t, in, 0));
        break;
    case OP_S_BITSET0_B32:
        emit(t, "{ uint r_ = %s & ~(1u << (%s & 31u));", sgpr(t, d.reg), src(t, in, 0));
        break;
    case OP_S_BITSET1_B32:
        emit(t, "{ uint r_ = %s | (1u << (%s & 31u));", sgpr(t, d.reg), src(t, in, 0));
        break;
    case OP_S_BCNT0_I32_B32:
    case OP_S_BCNT0_I32_B64:
        if (result_is_mask(t, in) || in.op == OP_S_BCNT0_I32_B64)
            emit(t, "{ uint r_ = %s; scc = r_ != 0u;",
                 result_is_mask(t, in)
                     ? fmt(t, "wave_count(~%s)", src(t, in, 0))
                     : fmt(t, "countbits(~%s) + countbits(~%s)", src(t, in, 0), src(t, in, 0, 1)));
        else
            emit(t, "{ uint r_ = countbits(~%s); scc = r_ != 0u;", src(t, in, 0));
        break;
    case OP_S_BCNT1_I32_B32:
    case OP_S_BCNT1_I32_B64:
        if (result_is_mask(t, in) || in.op == OP_S_BCNT1_I32_B64)
            emit(t, "{ uint r_ = %s; scc = r_ != 0u;",
                 result_is_mask(t, in)
                     ? fmt(t, "wave_count(%s)", src(t, in, 0))
                     : fmt(t, "countbits(%s) + countbits(%s)", src(t, in, 0), src(t, in, 0, 1)));
        else
            emit(t, "{ uint r_ = countbits(%s); scc = r_ != 0u;", src(t, in, 0));
        break;
    case OP_S_FF0_I32_B32:
    case OP_S_FF0_I32_B64:
        if (result_is_mask(t, in))
            emit(t, "{ uint r_ = wave_first(~%s);", src(t, in, 0));
        else if (in.op == OP_S_FF0_I32_B64)
            emit(t,
                 "{ uint lo_ = ~%s, hi_ = ~%s; uint r_ = lo_ ? firstbitlow(lo_) : hi_ ? 32u + firstbitlow(hi_) "
                 ": 0xffffffffu;",
                 src(t, in, 0), src(t, in, 0, 1));
        else
            emit(t, "{ uint r_ = firstbitlow(~%s);", src(t, in, 0));
        break;
    case OP_S_FF1_I32_B32:
    case OP_S_FF1_I32_B64:
        if (result_is_mask(t, in))
            emit(t, "{ uint r_ = wave_first(%s);", src(t, in, 0));
        else if (in.op == OP_S_FF1_I32_B64)
            emit(t,
                 "{ uint lo_ = %s, hi_ = %s; uint r_ = lo_ ? firstbitlow(lo_) : hi_ ? 32u + firstbitlow(hi_) "
                 ": "
                 "0xffffffffu;",
                 src(t, in, 0), src(t, in, 0, 1));
        else
            emit(t, "{ uint r_ = firstbitlow(%s);", src(t, in, 0));
        break;
    case OP_S_FLBIT_I32_B32:
        emit(t, "{ uint r_ = ffbh_u32(%s);", src(t, in, 0));
        break;
    case OP_S_FLBIT_I32_B64:
        emit(t,
             "{ uint lo_ = %s, hi_ = %s; uint r_ = hi_ ? ffbh_u32(hi_) : lo_ ? 32u + ffbh_u32(lo_) : "
             "0xffffffffu;",
             src(t, in, 0), src(t, in, 0, 1));
        break;
    case OP_S_FLBIT_I32:
        emit(t, "{ uint r_ = ffbh_i32((int)%s);", src(t, in, 0));
        break;
    case OP_S_FLBIT_I32_I64:
        emit(t,
             "{ uint lo_ = %s, hi_ = %s; bool neg_ = (int)hi_ < 0; uint s_ = neg_ ? ~hi_ : hi_, l_ = neg_ ? "
             "~lo_ : lo_; uint r_ = s_ ? ffbh_u32(s_) : l_ ? 32u + ffbh_u32(l_) : 0xffffffffu;",
             src(t, in, 0), src(t, in, 0, 1));
        break;
    case OP_S_LSHL_B64:
    case OP_S_LSHR_B64:
    case OP_S_ASHR_I64: {
        const char* v = src64(t, in, 0);
        const char* s = fmt(t, "(%s & 63u)", src(t, in, 1));
        const char* r = in.op == OP_S_LSHL_B64   ? fmt(t, "(%s << %s)", v, s)
                        : in.op == OP_S_LSHR_B64 ? fmt(t, "(%s >> %s)", v, s)
                                                 : fmt(t, "(uint64_t)((int64_t)%s >> %s)", v, s);
        write64(t, d, r);
        set_scc(t, fmt(t, "(%s | %s) != 0u", sgpr(t, d.reg), sgpr(t, d.reg + 1)));
        return true;
    }
    case OP_S_BFE_U64: {
        const char* c = src(t, in, 1);
        write64(t, d,
                fmt(t,
                    "((%s >> (%s & 63u)) & ((((%s >> 16) & 127u) >= 64u) ? 0xffffffffffffffffull : "
                    "(((uint64_t)1 << ((%s >> 16) & 127u)) - 1)))",
                    src64(t, in, 0), c, c, c));
        set_scc(t, fmt(t, "(%s | %s) != 0u", sgpr(t, d.reg), sgpr(t, d.reg + 1)));
        return true;
    }
    case OP_S_BFM_B64:
        write64(t, d,
                fmt(t, "((((uint64_t)1 << (%s & 63u)) - 1) << (%s & 63u))", src(t, in, 0), src(t, in, 1)));
        return true;
    case OP_S_BITREPLICATE_B64_B32: {
        // Each bit of the source twice: used to widen wave32 masks.
        emit(t,
             "{ uint x_ = %s; uint lo_ = 0u, hi_ = 0u; for (uint i_ = 0u; i_ < 16u; i_++) { lo_ |= ((x_ >> "
             "i_) & "
             "1u) * (3u << (2u * i_)); hi_ |= ((x_ >> (i_ + 16u)) & 1u) * (3u << (2u * i_)); }",
             src(t, in, 0));
        t.depth++;
        write_sgpr(t, d, 0, "lo_");
        write_sgpr(t, d, 1, "hi_");
        t.depth--;
        emit(t, "}");
        return true;
    }
    case OP_S_GETPC_B64:
        write_sgpr(t, d, 0, fmt(t, "0x%08xu", in.offset + 4));
        write_sgpr(t, d, 1, "0u");
        return true;
    case OP_S_GETREG_B32:
        write_sgpr(t, d, 0, "0u"); // hardware registers: nothing a translated program depends on
        return true;
    case OP_S_SETREG_B32:
    case OP_S_SETREG_IMM32_B32:
        return true; // float modes and the like
    case OP_S_CMP_EQ_I32:
    case OP_S_CMP_LG_I32:
    case OP_S_CMP_GT_I32:
    case OP_S_CMP_GE_I32:
    case OP_S_CMP_LT_I32:
    case OP_S_CMP_LE_I32:
    case OP_S_CMP_EQ_U32:
    case OP_S_CMP_LG_U32:
    case OP_S_CMP_GT_U32:
    case OP_S_CMP_GE_U32:
    case OP_S_CMP_LT_U32:
    case OP_S_CMP_LE_U32: {
        static const char* const k_ops[6] = {"==", "!=", ">", ">=", "<", "<="};
        u32 k = (u32)(in.op - OP_S_CMP_EQ_I32);
        bool is_signed = k < 6;
        set_scc(t, fmt(t, "%s%s %s %s%s", is_signed ? "(int)" : "", src(t, in, 0), k_ops[k % 6],
                       is_signed ? "(int)" : "", src(t, in, 1)));
        return true;
    }
    case OP_S_CMPK_EQ_I32:
    case OP_S_CMPK_LG_I32:
    case OP_S_CMPK_GT_I32:
    case OP_S_CMPK_GE_I32:
    case OP_S_CMPK_LT_I32:
    case OP_S_CMPK_LE_I32:
    case OP_S_CMPK_EQ_U32:
    case OP_S_CMPK_LG_U32:
    case OP_S_CMPK_GT_U32:
    case OP_S_CMPK_GE_U32:
    case OP_S_CMPK_LT_U32:
    case OP_S_CMPK_LE_U32: {
        static const char* const k_ops[6] = {"==", "!=", ">", ">=", "<", "<="};
        u32 k = (u32)(in.op - OP_S_CMPK_EQ_I32);
        bool is_signed = k < 6;
        u32 imm = is_signed ? (u32)in.simm16 : (u32)(u16)in.simm16;
        set_scc(t, fmt(t, "%s%s %s %s0x%08xu", is_signed ? "(int)" : "", sgpr(t, d.reg), k_ops[k % 6],
                       is_signed ? "(int)" : "", imm));
        return true;
    }
    case OP_S_BITCMP0_B32:
        set_scc(t, fmt(t, "((%s >> (%s & 31u)) & 1u) == 0u", src(t, in, 0), src(t, in, 1)));
        return true;
    case OP_S_BITCMP1_B32:
        set_scc(t, fmt(t, "((%s >> (%s & 31u)) & 1u) != 0u", src(t, in, 0), src(t, in, 1)));
        return true;
    case OP_S_CMP_EQ_U64:
    case OP_S_CMP_LG_U64: {
        bool mask = in.src[0].kind == OPND_SGPR && tag_kind(t.tags.sgpr[in.src[0].reg & 127]) == TAG_MASK;
        if (mask && in.src[1].kind == OPND_INLINE && in.src[1].reg == SRC_ZERO)
            set_scc(t, fmt(t, "%swave_any(%s)", in.op == OP_S_CMP_EQ_U64 ? "!" : "", src(t, in, 0)));
        else
            set_scc(t, fmt(t, "%s %s %s", src64(t, in, 0),
                           in.op == OP_S_CMP_EQ_U64 ? "==" : "!=", src64(t, in, 1)));
        return true;
    }
    default:
        if ((in.op >= OP_S_AND_SAVEEXEC_B64 && in.op <= OP_S_XNOR_SAVEEXEC_B64) ||
            (in.op >= OP_S_AND_SAVEEXEC_B32 && in.op <= OP_S_ANDN2_WREXEC_B32) ||
            (in.op >= OP_S_ANDN1_SAVEEXEC_B64 && in.op <= OP_S_ANDN2_WREXEC_B64))
            return emit_saveexec(t, in);
        translator_fail(t, "unsupported %s", rdna_op_name(in.op));
        return false;
    }
    // The cases that break out opened a block computing r_.
    t.scc_used = true;
    t.depth++;
    write_sgpr(t, d, 0, "r_");
    t.depth--;
    emit(t, "}");
    return true;
}

// --- vector ------------------------------------------------------------------------------------------

static const char* f(Translator& t, const RdnaInst& in, u32 i) {
    return src_f32(t, in, i);
}

static const char* u(Translator& t, const RdnaInst& in, u32 i) {
    return src_u32(t, in, i, TYPE_32);
}

static const char* h(Translator& t, const RdnaInst& in, u32 i) {
    // A half float source: the low or (op_sel) high half, with modifiers.
    const char* bits = src_u32(t, in, i, TYPE_16);
    const char* v = (in.op_sel >> i) & 1 ? fmt(t, "f16tof32(%s >> 16)", bits) : fmt(t, "f16tof32(%s)", bits);
    if (in.src[i].abs)
        v = fmt(t, "abs(%s)", v);
    if (in.src[i].neg)
        v = fmt(t, "(-%s)", v);
    return v;
}

static const char* h_bits(Translator& t, const RdnaInst& in, u32 i) {
    const char* bits = src_u32(t, in, i, TYPE_16);
    const char* v = (in.op_sel >> i) & 1 ? fmt(t, "((%s >> 16) & 0xffffu)", bits) : fmt(t, "(%s & 0xffffu)", bits);
    if (in.src[i].abs)
        v = fmt(t, "(%s & 0x7fffu)", v);
    if (in.src[i].neg)
        v = fmt(t, "(%s ^ 0x8000u)", v);
    return v;
}

static const char* u16_src(Translator& t, const RdnaInst& in, u32 i, bool is_signed) {
    const char* bits = src_u32(t, in, i, TYPE_16);
    const char* lo = (in.op_sel >> i) & 1 ? fmt(t, "(%s >> 16)", bits) : bits;
    return is_signed ? fmt(t, "((int)(%s << 16) >> 16)", lo) : fmt(t, "(%s & 0xffffu)", lo);
}

static void vwrite(Translator& t, const RdnaInst& in, const char* value) {
    write_vgpr(t, in, in.dst[0].reg, value);
}

static void vfloat(Translator& t, const RdnaInst& in, const char* value) {
    vwrite(t, in, float_result(t, in, value));
}

// A 16-bit float result: low half, or the high half with op_sel bit 3; the other half is kept.
static void vhalf(Translator& t, const RdnaInst& in, const char* value) {
    const char* v = in.clamp ? fmt(t, "saturate(%s)", value) : value;
    const char* dst = vgpr(t, in.dst[0].reg);
    if (in.op_sel & 8)
        vwrite(t, in, fmt(t, "((%s & 0xffffu) | (f32tof16(%s) << 16))", dst, v));
    else
        vwrite(t, in, fmt(t, "((%s & 0xffff0000u) | f32tof16(%s))", dst, v));
}

// A 16-bit integer result: low half, or the high half with op_sel bit 3; the other half is kept.
static void vhalf_u16(Translator& t, const RdnaInst& in, const char* value) {
    const char* dst = vgpr(t, in.dst[0].reg);
    if (in.op_sel & 8)
        vwrite(t, in, fmt(t, "((%s & 0xffffu) | ((%s & 0xffffu) << 16))", dst, value));
    else
        vwrite(t, in, fmt(t, "((%s & 0xffff0000u) | (%s & 0xffffu))", dst, value));
}

static const char* lane_bit(Translator& t, const RdnaInst& in, u32 i) {
    return fmt(t, "(%s != 0u)", src_u32(t, in, i));
}

// Compares write EXEC && condition to the destination mask (or EXEC itself for CMPX).
static void emit_compare_result(Translator& t, const RdnaInst& in, const char* condition) {
    bool cmpx = strstr(rdna_op_name(in.op), "_cmpx_") != nullptr;
    emit(t, t.dpp_has_mask ? "{ uint m_ = mask_of(EXEC && dpp_en_ && (%s));"
                           : "{ uint m_ = mask_of(EXEC && (%s));",
         condition);
    t.depth++;
    if (cmpx) {
        emit(t, "s126 = m_;");
        if (!t.wave32)
            emit(t, "s127 = m_;");
    } else {
        RdnaOperand dst = in.dst[0];
        write_sgpr(t, dst, 0, "m_");
        if (!t.wave32)
            write_sgpr(t, dst, 1, "m_");
    }
    t.depth--;
    emit(t, "}");
}

static bool emit_compare(Translator& t, const RdnaInst& in) {
    const char* name = rdna_op_name(in.op);
    // v_cmp[x]_<cond>_<type>
    const char* cond = strstr(name, "_cmp_") ? name + 6 : name + 7;
    char c[8] = {};
    for (u32 i = 0; i < 7 && cond[i] && cond[i] != '_'; i++)
        c[i] = cond[i];
    const char* type = strrchr(name, '_') + 1;
    if (strcmp(c, "class") == 0) {
        if (strcmp(type, "f32") == 0) {
            emit_compare_result(t, in, fmt(t, "cmp_class(%s, %s)", f(t, in, 0), u(t, in, 1)));
            return true;
        }
        if (strcmp(type, "f16") == 0) {
            emit_compare_result(t, in, fmt(t, "cmp_class_f16(%s, %s)", h_bits(t, in, 0), u(t, in, 1)));
            return true;
        }
        translator_fail(t, "unsupported %s", name);
        return false;
    }
    const char *a, *b;
    bool is_float = type[0] == 'f';
    if (strcmp(type, "f32") == 0) {
        a = f(t, in, 0);
        b = f(t, in, 1);
    } else if (strcmp(type, "f16") == 0) {
        a = h(t, in, 0);
        b = h(t, in, 1);
    } else if (strcmp(type, "i32") == 0) {
        a = fmt(t, "(int)%s", u(t, in, 0));
        b = fmt(t, "(int)%s", u(t, in, 1));
    } else if (strcmp(type, "u32") == 0) {
        a = u(t, in, 0);
        b = u(t, in, 1);
    } else if (strcmp(type, "u16") == 0 || strcmp(type, "i16") == 0) {
        bool s = type[0] == 'i';
        a = s ? fmt(t, "((int)(%s << 16) >> 16)", u(t, in, 0)) : fmt(t, "(%s & 0xffffu)", u(t, in, 0));
        b = s ? fmt(t, "((int)(%s << 16) >> 16)", u(t, in, 1)) : fmt(t, "(%s & 0xffffu)", u(t, in, 1));
    } else if (strcmp(type, "u64") == 0 || strcmp(type, "i64") == 0) {
        bool s = type[0] == 'i';
        a = fmt(t, "%s(%s | ((uint64_t)%s << 32))", s ? "(int64_t)" : "(uint64_t)",
                src_u32(t, in, 0, TYPE_64, 0), src_u32(t, in, 0, TYPE_64, 1));
        b = fmt(t, "%s(%s | ((uint64_t)%s << 32))", s ? "(int64_t)" : "(uint64_t)",
                src_u32(t, in, 1, TYPE_64, 0), src_u32(t, in, 1, TYPE_64, 1));
    } else {
        translator_fail(t, "unsupported %s", name);
        return false;
    }
    const char* e = nullptr;
    // Float "n" conditions are true for unordered (NaN) operands.
    if (!strcmp(c, "f"))
        e = "false";
    else if (!strcmp(c, "t") || !strcmp(c, "tru"))
        e = "true";
    else if (!strcmp(c, "lt"))
        e = fmt(t, "%s < %s", a, b);
    else if (!strcmp(c, "eq"))
        e = fmt(t, "%s == %s", a, b);
    else if (!strcmp(c, "le"))
        e = fmt(t, "%s <= %s", a, b);
    else if (!strcmp(c, "gt"))
        e = fmt(t, "%s > %s", a, b);
    else if (!strcmp(c, "ne"))
        e = fmt(t, "%s != %s", a, b);
    else if (!strcmp(c, "lg"))
        e = fmt(t, "(%s < %s || %s > %s)", a, b, a, b);
    else if (!strcmp(c, "ge"))
        e = fmt(t, "%s >= %s", a, b);
    else if (!strcmp(c, "o"))
        e = fmt(t, "(%s == %s && %s == %s)", a, a, b, b);
    else if (!strcmp(c, "u"))
        e = fmt(t, "(%s != %s || %s != %s)", a, a, b, b);
    else if (!strcmp(c, "nge"))
        e = fmt(t, "!(%s >= %s)", a, b);
    else if (!strcmp(c, "nlg"))
        e = fmt(t, "!(%s < %s || %s > %s)", a, b, a, b);
    else if (!strcmp(c, "ngt"))
        e = fmt(t, "!(%s > %s)", a, b);
    else if (!strcmp(c, "nle"))
        e = fmt(t, "!(%s <= %s)", a, b);
    else if (!strcmp(c, "neq"))
        e = fmt(t, "!(%s == %s)", a, b);
    else if (!strcmp(c, "nlt"))
        e = fmt(t, "!(%s < %s)", a, b);
    (void)is_float;
    if (!e) {
        translator_fail(t, "unsupported %s", name);
        return false;
    }
    emit_compare_result(t, in, e);
    return true;
}

static bool emit_vector(Translator& t, const RdnaInst& in) {
    if (in.has_dpp &&
        (in.op == OP_V_SWAP_B32 || in.op == OP_V_READFIRSTLANE_B32 || in.op == OP_V_READLANE_B32 ||
         in.op == OP_V_WRITELANE_B32)) {
        translator_fail(t, "DPP on %s", rdna_op_name(in.op));
        return false;
    }
    switch (in.op) {
    case OP_V_NOP:
    case OP_V_PIPEFLUSH:
    case OP_V_CLREXCP:
        return true;
    case OP_V_MOV_B32:
        vwrite(t, in, u(t, in, 0));
        return true;
    case OP_V_CNDMASK_B32:
        if (in.enc == ENC_VOP3 && (in.src[0].abs || in.src[0].neg || in.src[1].abs || in.src[1].neg))
            vwrite(t, in,
                   fmt(t, "(%s ? asuint(%s) : asuint(%s))", lane_bit(t, in, 2), f(t, in, 1), f(t, in, 0)));
        else
            vwrite(t, in, fmt(t, "(%s ? %s : %s)", lane_bit(t, in, 2), u(t, in, 1), u(t, in, 0)));
        return true;
    case OP_V_READFIRSTLANE_B32:
        write_sgpr(t, in.dst[0], 0, fmt(t, "read_first(%s, s126)", u(t, in, 0)));
        return true;
    case OP_V_READLANE_B32:
        write_sgpr(t, in.dst[0], 0,
                   fmt(t, "WaveReadLaneAt(%s, %s & %su)", u(t, in, 0), u(t, in, 1), t.wave32 ? "31" : "63"));
        return true;
    case OP_V_WRITELANE_B32:
        emit(t, "if (WaveGetLaneIndex() == (%s & %su)) %s = %s;", u(t, in, 1), t.wave32 ? "31" : "63",
             vgpr(t, in.dst[0].reg), u(t, in, 0));
        return true;
    case OP_V_MBCNT_LO_U32_B32:
    case OP_V_MBCNT_HI_U32_B32: {
        // Lanes below this one in the mask. With an all-ones mask, the lane index; otherwise the
        // whole count happens in the LO step (it sees every lane of the desktop wave).
        bool all = in.src[0].kind == OPND_INLINE && in.src[0].reg == SRC_INT_MAX + 1; // -1
        bool lo = in.op == OP_V_MBCNT_LO_U32_B32;
        if (all)
            vwrite(t, in,
                   lo ? fmt(t, "(min(WaveGetLaneIndex(), 32u) + %s)", u(t, in, 1))
                      : fmt(t, "(max((int)WaveGetLaneIndex() - 32, 0) + %s)", u(t, in, 1)));
        else
            vwrite(t, in,
                   lo ? fmt(t, "(WavePrefixCountBits(%s != 0u) + %s)", u(t, in, 0), u(t, in, 1))
                      : u(t, in, 1));
        return true;
    }
    // Float arithmetic.
    case OP_V_ADD_F32:
        vfloat(t, in, fmt(t, "(%s + %s)", f(t, in, 0), f(t, in, 1)));
        return true;
    case OP_V_SUB_F32:
        vfloat(t, in, fmt(t, "(%s - %s)", f(t, in, 0), f(t, in, 1)));
        return true;
    case OP_V_SUBREV_F32:
        vfloat(t, in, fmt(t, "(%s - %s)", f(t, in, 1), f(t, in, 0)));
        return true;
    case OP_V_MUL_F32:
        vfloat(t, in, fmt(t, "(%s * %s)", f(t, in, 0), f(t, in, 1)));
        return true;
    case OP_V_MUL_LEGACY_F32:
        vfloat(t, in, fmt(t, "mul_legacy(%s, %s)", f(t, in, 0), f(t, in, 1)));
        return true;
    case OP_V_MIN_F32:
        vfloat(t, in, fmt(t, "min(%s, %s)", f(t, in, 0), f(t, in, 1)));
        return true;
    case OP_V_MAX_F32:
        vfloat(t, in, fmt(t, "max(%s, %s)", f(t, in, 0), f(t, in, 1)));
        return true;
    case OP_V_FMAC_F32:
    case OP_V_MAC_F32:
        vfloat(t, in, fmt(t, "mad(%s, %s, asfloat(%s))", f(t, in, 0), f(t, in, 1), vgpr(t, in.dst[0].reg)));
        return true;
    case OP_V_FMAC_LEGACY_F32:
        vfloat(
            t, in,
            fmt(t, "(mul_legacy(%s, %s) + asfloat(%s))", f(t, in, 0), f(t, in, 1), vgpr(t, in.dst[0].reg)));
        return true;
    case OP_V_FMA_F32:
    case OP_V_MAD_F32:
    case OP_V_MADMK_F32:
    case OP_V_MADAK_F32:
    case OP_V_FMAMK_F32:
    case OP_V_FMAAK_F32:
        vfloat(t, in, fmt(t, "mad(%s, %s, %s)", f(t, in, 0), f(t, in, 1), f(t, in, 2)));
        return true;
    case OP_V_MAD_LEGACY_F32:
        vfloat(t, in, fmt(t, "(mul_legacy(%s, %s) + %s)", f(t, in, 0), f(t, in, 1), f(t, in, 2)));
        return true;
    case OP_V_MIN3_F32:
        vfloat(t, in, fmt(t, "min(min(%s, %s), %s)", f(t, in, 0), f(t, in, 1), f(t, in, 2)));
        return true;
    case OP_V_MAX3_F32:
        vfloat(t, in, fmt(t, "max(max(%s, %s), %s)", f(t, in, 0), f(t, in, 1), f(t, in, 2)));
        return true;
    case OP_V_MED3_F32:
        vfloat(t, in, fmt(t, "med3_f32(%s, %s, %s)", f(t, in, 0), f(t, in, 1), f(t, in, 2)));
        return true;
    case OP_V_FRACT_F32:
        vfloat(t, in, fmt(t, "frac(%s)", f(t, in, 0)));
        return true;
    case OP_V_TRUNC_F32:
        vfloat(t, in, fmt(t, "trunc(%s)", f(t, in, 0)));
        return true;
    case OP_V_CEIL_F32:
        vfloat(t, in, fmt(t, "ceil(%s)", f(t, in, 0)));
        return true;
    case OP_V_RNDNE_F32:
        vfloat(t, in, fmt(t, "round(%s)", f(t, in, 0)));
        return true;
    case OP_V_FLOOR_F32:
        vfloat(t, in, fmt(t, "floor(%s)", f(t, in, 0)));
        return true;
    case OP_V_EXP_F32:
        vfloat(t, in, fmt(t, "exp2(%s)", f(t, in, 0)));
        return true;
    case OP_V_LOG_F32:
        vfloat(t, in, fmt(t, "log2(%s)", f(t, in, 0)));
        return true;
    case OP_V_RCP_F32:
    case OP_V_RCP_IFLAG_F32:
        vfloat(t, in, fmt(t, "rcp(%s)", f(t, in, 0)));
        return true;
    case OP_V_RSQ_F32:
        vfloat(t, in, fmt(t, "rsqrt(%s)", f(t, in, 0)));
        return true;
    case OP_V_SQRT_F32:
        vfloat(t, in, fmt(t, "sqrt(%s)", f(t, in, 0)));
        return true;
    case OP_V_SIN_F32:
        vfloat(t, in, fmt(t, "sin_rev(%s)", f(t, in, 0)));
        return true;
    case OP_V_COS_F32:
        vfloat(t, in, fmt(t, "cos_rev(%s)", f(t, in, 0)));
        return true;
    case OP_V_LDEXP_F32:
        vfloat(t, in, fmt(t, "ldexp(%s, (float)(int)%s)", f(t, in, 0), u(t, in, 1)));
        return true;
    case OP_V_FREXP_MANT_F32:
        vfloat(t, in, fmt(t, "frexp_mant(%s)", f(t, in, 0)));
        return true;
    case OP_V_FREXP_EXP_I32_F32:
        vwrite(t, in, fmt(t, "(uint)frexp_exp(%s)", f(t, in, 0)));
        return true;
    case OP_V_DIV_SCALE_F32:
        // Division is done exactly in DIV_FIXUP; scaling is the identity and never flags.
        vwrite(t, in, u(t, in, 0));
        write_sgpr(t, in.dst[1], 0, "0u");
        if (!t.wave32)
            write_sgpr(t, in.dst[1], 1, "0u");
        return true;
    case OP_V_DIV_FMAS_F32:
        vfloat(t, in, fmt(t, "mad(%s, %s, %s)", f(t, in, 0), f(t, in, 1), f(t, in, 2)));
        return true;
    case OP_V_DIV_FIXUP_F32:
        vfloat(t, in, fmt(t, "(%s / %s)", f(t, in, 2), f(t, in, 1)));
        return true;
    case OP_V_CUBEID_F32:
    case OP_V_CUBESC_F32:
    case OP_V_CUBETC_F32:
    case OP_V_CUBEMA_F32: {
        static const char* const k_fn[4] = {"cube_id", "cube_sc", "cube_tc", "cube_ma"};
        vfloat(t, in,
               fmt(t, "%s(float3(%s, %s, %s))", k_fn[in.op - OP_V_CUBEID_F32], f(t, in, 0), f(t, in, 1),
                   f(t, in, 2)));
        return true;
    }
    // Conversions.
    case OP_V_CVT_F32_I32:
        vfloat(t, in, fmt(t, "(float)(int)%s", u(t, in, 0)));
        return true;
    case OP_V_CVT_F32_U32:
        vfloat(t, in, fmt(t, "(float)%s", u(t, in, 0)));
        return true;
    case OP_V_CVT_U32_F32:
        vwrite(t, in, fmt(t, "f32_to_u32(%s)", f(t, in, 0)));
        return true;
    case OP_V_CVT_I32_F32:
        vwrite(t, in, fmt(t, "(uint)f32_to_i32(%s)", f(t, in, 0)));
        return true;
    case OP_V_CVT_RPI_I32_F32:
        vwrite(t, in, fmt(t, "(uint)f32_to_i32(floor(%s + 0.5f))", f(t, in, 0)));
        return true;
    case OP_V_CVT_FLR_I32_F32:
        vwrite(t, in, fmt(t, "(uint)f32_to_i32(floor(%s))", f(t, in, 0)));
        return true;
    case OP_V_CVT_F16_F32:
        vhalf(t, in, f(t, in, 0));
        return true;
    case OP_V_CVT_F32_F16:
        vfloat(t, in, h(t, in, 0));
        return true;
    case OP_V_CVT_PKRTZ_F16_F32:
        vwrite(t, in, fmt(t, "cvt_pkrtz(%s, %s)", f(t, in, 0), f(t, in, 1)));
        return true;
    case OP_V_CVT_F32_UBYTE0:
    case OP_V_CVT_F32_UBYTE1:
    case OP_V_CVT_F32_UBYTE2:
    case OP_V_CVT_F32_UBYTE3:
        vfloat(t, in,
               fmt(t, "(float)((%s >> %u) & 0xffu)", u(t, in, 0), 8 * (u32)(in.op - OP_V_CVT_F32_UBYTE0)));
        return true;
    case OP_V_CVT_PKNORM_I16_F32:
        vwrite(t, in,
               fmt(t,
                   "(((uint)(int)round(clamp(%s, -1.0f, 1.0f) * 32767.0f) & 0xffffu) | ((uint)(int)round("
                   "clamp(%s, -1.0f, 1.0f) * 32767.0f) << 16))",
                   f(t, in, 0), f(t, in, 1)));
        return true;
    case OP_V_CVT_PKNORM_U16_F32:
        vwrite(t, in,
               fmt(t, "((uint)round(saturate(%s) * 65535.0f) | ((uint)round(saturate(%s) * 65535.0f) << 16))",
                   f(t, in, 0), f(t, in, 1)));
        return true;
    case OP_V_CVT_PK_U16_U32:
        vwrite(t, in, fmt(t, "(min(%s, 0xffffu) | (min(%s, 0xffffu) << 16))", u(t, in, 0), u(t, in, 1)));
        return true;
    case OP_V_CVT_PK_I16_I32:
        vwrite(t, in,
               fmt(t,
                   "(((uint)clamp((int)%s, -32768, 32767) & 0xffffu) | ((uint)clamp((int)%s, -32768, "
                   "32767) << 16))",
                   u(t, in, 0), u(t, in, 1)));
        return true;
    case OP_V_CVT_PK_U8_F32: {
        const char* byte = fmt(t, "(%s & 3u) * 8u", u(t, in, 1));
        vwrite(t, in,
               fmt(t, "((%s & ~(0xffu << %s)) | ((f32_to_u32(round(%s)) & 0xffu) << %s))", u(t, in, 2), byte,
                   f(t, in, 0), byte));
        return true;
    }
    // Integer arithmetic.
    case OP_V_ADD_NC_U32:
        vwrite(t, in, fmt(t, "(%s + %s)", u(t, in, 0), u(t, in, 1)));
        return true;
    case OP_V_SUB_NC_U32:
        vwrite(t, in, fmt(t, "(%s - %s)", u(t, in, 0), u(t, in, 1)));
        return true;
    case OP_V_SUBREV_NC_U32:
        vwrite(t, in, fmt(t, "(%s - %s)", u(t, in, 1), u(t, in, 0)));
        return true;
    case OP_V_ADD_NC_I32:
        vwrite(t, in, fmt(t, "(%s + %s)", u(t, in, 0), u(t, in, 1)));
        return true;
    case OP_V_SUB_NC_I32:
        vwrite(t, in, fmt(t, "(%s - %s)", u(t, in, 0), u(t, in, 1)));
        return true;
    case OP_V_ADD3_U32:
        vwrite(t, in, fmt(t, "(%s + %s + %s)", u(t, in, 0), u(t, in, 1), u(t, in, 2)));
        return true;
    case OP_V_XAD_U32:
        vwrite(t, in, fmt(t, "((%s ^ %s) + %s)", u(t, in, 0), u(t, in, 1), u(t, in, 2)));
        return true;
    case OP_V_LSHL_ADD_U32:
        vwrite(t, in, fmt(t, "((%s << (%s & 31u)) + %s)", u(t, in, 0), u(t, in, 1), u(t, in, 2)));
        return true;
    case OP_V_ADD_LSHL_U32:
        vwrite(t, in, fmt(t, "((%s + %s) << (%s & 31u))", u(t, in, 0), u(t, in, 1), u(t, in, 2)));
        return true;
    case OP_V_LSHL_OR_B32:
        vwrite(t, in, fmt(t, "((%s << (%s & 31u)) | %s)", u(t, in, 0), u(t, in, 1), u(t, in, 2)));
        return true;
    case OP_V_AND_OR_B32:
        vwrite(t, in, fmt(t, "((%s & %s) | %s)", u(t, in, 0), u(t, in, 1), u(t, in, 2)));
        return true;
    case OP_V_OR3_B32:
        vwrite(t, in, fmt(t, "(%s | %s | %s)", u(t, in, 0), u(t, in, 1), u(t, in, 2)));
        return true;
    case OP_V_XOR3_B32:
        vwrite(t, in, fmt(t, "(%s ^ %s ^ %s)", u(t, in, 0), u(t, in, 1), u(t, in, 2)));
        return true;
    case OP_V_MUL_U32_U24:
        vwrite(t, in, fmt(t, "((%s & 0xffffffu) * (%s & 0xffffffu))", u(t, in, 0), u(t, in, 1)));
        return true;
    case OP_V_MUL_I32_I24:
        vwrite(t, in,
               fmt(t, "(uint)(((int)(%s << 8) >> 8) * ((int)(%s << 8) >> 8))", u(t, in, 0), u(t, in, 1)));
        return true;
    case OP_V_MUL_HI_U32_U24:
        vwrite(t, in,
               fmt(t, "(uint)(((uint64_t)(%s & 0xffffffu) * (%s & 0xffffffu)) >> 32)", u(t, in, 0),
                   u(t, in, 1)));
        return true;
    case OP_V_MAD_U32_U24:
        vwrite(t, in,
               fmt(t, "((%s & 0xffffffu) * (%s & 0xffffffu) + %s)", u(t, in, 0), u(t, in, 1), u(t, in, 2)));
        return true;
    case OP_V_MAD_I32_I24:
        vwrite(t, in,
               fmt(t, "(uint)(((int)(%s << 8) >> 8) * ((int)(%s << 8) >> 8) + (int)%s)", u(t, in, 0),
                   u(t, in, 1), u(t, in, 2)));
        return true;
    case OP_V_MAD_U32_U16:
        if (in.clamp)
            vwrite(t, in,
                   fmt(t, "(uint)min((uint64_t)%s * %s + %s, 0xffffffffull)", u16_src(t, in, 0, false),
                       u16_src(t, in, 1, false), u(t, in, 2)));
        else
            vwrite(t, in,
                   fmt(t, "(%s * %s + %s)", u16_src(t, in, 0, false), u16_src(t, in, 1, false), u(t, in, 2)));
        return true;
    case OP_V_MAD_I32_I16:
        if (in.clamp)
            vwrite(t, in,
                   fmt(t, "(uint)clamp((int64_t)%s * %s + (int)%s, -2147483648ll, 2147483647ll)",
                       u16_src(t, in, 0, true), u16_src(t, in, 1, true), u(t, in, 2)));
        else
            vwrite(t, in,
                   fmt(t, "(uint)(%s * %s + (int)%s)", u16_src(t, in, 0, true), u16_src(t, in, 1, true),
                       u(t, in, 2)));
        return true;
    case OP_V_MAD_U16: {
        const char* expr =
            fmt(t, "(%s * %s + %s)", u16_src(t, in, 0, false), u16_src(t, in, 1, false),
                u16_src(t, in, 2, false));
        vhalf_u16(t, in, in.clamp ? fmt(t, "min(%s, 0xffffu)", expr) : expr);
        return true;
    }
    case OP_V_MAD_I16: {
        const char* expr = fmt(t, "(%s * %s + %s)", u16_src(t, in, 0, true), u16_src(t, in, 1, true),
                               u16_src(t, in, 2, true));
        vhalf_u16(t, in, in.clamp ? fmt(t, "(uint)clamp(%s, -32768, 32767)", expr) : fmt(t, "(uint)%s", expr));
        return true;
    }
    case OP_V_MUL_LO_U32:
        vwrite(t, in, fmt(t, "(%s * %s)", u(t, in, 0), u(t, in, 1)));
        return true;
    case OP_V_MUL_HI_U32:
        vwrite(t, in, fmt(t, "(uint)(((uint64_t)%s * %s) >> 32)", u(t, in, 0), u(t, in, 1)));
        return true;
    case OP_V_MUL_HI_I32:
        vwrite(t, in, fmt(t, "(uint)(((int64_t)(int)%s * (int)%s) >> 32)", u(t, in, 0), u(t, in, 1)));
        return true;
    case OP_V_MIN_I32:
        vwrite(t, in, fmt(t, "(uint)min((int)%s, (int)%s)", u(t, in, 0), u(t, in, 1)));
        return true;
    case OP_V_MAX_I32:
        vwrite(t, in, fmt(t, "(uint)max((int)%s, (int)%s)", u(t, in, 0), u(t, in, 1)));
        return true;
    case OP_V_MIN_U32:
        vwrite(t, in, fmt(t, "min(%s, %s)", u(t, in, 0), u(t, in, 1)));
        return true;
    case OP_V_MAX_U32:
        vwrite(t, in, fmt(t, "max(%s, %s)", u(t, in, 0), u(t, in, 1)));
        return true;
    case OP_V_MIN3_I32:
        vwrite(t, in,
               fmt(t, "(uint)min(min((int)%s, (int)%s), (int)%s)", u(t, in, 0), u(t, in, 1), u(t, in, 2)));
        return true;
    case OP_V_MAX3_I32:
        vwrite(t, in,
               fmt(t, "(uint)max(max((int)%s, (int)%s), (int)%s)", u(t, in, 0), u(t, in, 1), u(t, in, 2)));
        return true;
    case OP_V_MIN3_U32:
        vwrite(t, in, fmt(t, "min(min(%s, %s), %s)", u(t, in, 0), u(t, in, 1), u(t, in, 2)));
        return true;
    case OP_V_MAX3_U32:
        vwrite(t, in, fmt(t, "max(max(%s, %s), %s)", u(t, in, 0), u(t, in, 1), u(t, in, 2)));
        return true;
    case OP_V_MED3_I32:
        vwrite(t, in,
               fmt(t, "(uint)med3_i32((int)%s, (int)%s, (int)%s)", u(t, in, 0), u(t, in, 1), u(t, in, 2)));
        return true;
    case OP_V_MED3_U32:
        vwrite(t, in, fmt(t, "med3_u32(%s, %s, %s)", u(t, in, 0), u(t, in, 1), u(t, in, 2)));
        return true;
    // Bits.
    case OP_V_AND_B32:
        vwrite(t, in, fmt(t, "(%s & %s)", u(t, in, 0), u(t, in, 1)));
        return true;
    case OP_V_OR_B32:
        vwrite(t, in, fmt(t, "(%s | %s)", u(t, in, 0), u(t, in, 1)));
        return true;
    case OP_V_XOR_B32:
        vwrite(t, in, fmt(t, "(%s ^ %s)", u(t, in, 0), u(t, in, 1)));
        return true;
    case OP_V_XNOR_B32:
        vwrite(t, in, fmt(t, "~(%s ^ %s)", u(t, in, 0), u(t, in, 1)));
        return true;
    case OP_V_NOT_B32:
        vwrite(t, in, fmt(t, "~%s", u(t, in, 0)));
        return true;
    case OP_V_BFREV_B32:
        vwrite(t, in, fmt(t, "reversebits(%s)", u(t, in, 0)));
        return true;
    case OP_V_FFBH_U32:
        vwrite(t, in, fmt(t, "ffbh_u32(%s)", u(t, in, 0)));
        return true;
    case OP_V_FFBL_B32:
        vwrite(t, in, fmt(t, "firstbitlow(%s)", u(t, in, 0)));
        return true;
    case OP_V_FFBH_I32:
        vwrite(t, in, fmt(t, "ffbh_i32((int)%s)", u(t, in, 0)));
        return true;
    case OP_V_BCNT_U32_B32:
        vwrite(t, in, fmt(t, "(countbits(%s) + %s)", u(t, in, 0), u(t, in, 1)));
        return true;
    case OP_V_LSHRREV_B32:
        vwrite(t, in, fmt(t, "(%s >> (%s & 31u))", u(t, in, 1), u(t, in, 0)));
        return true;
    case OP_V_ASHRREV_I32:
        vwrite(t, in, fmt(t, "(uint)((int)%s >> (%s & 31u))", u(t, in, 1), u(t, in, 0)));
        return true;
    case OP_V_LSHLREV_B32:
        vwrite(t, in, fmt(t, "(%s << (%s & 31u))", u(t, in, 1), u(t, in, 0)));
        return true;
    case OP_V_BFE_U32:
        vwrite(t, in, fmt(t, "bfe_u32(%s, %s, %s)", u(t, in, 0), u(t, in, 1), u(t, in, 2)));
        return true;
    case OP_V_BFE_I32:
        vwrite(t, in, fmt(t, "(uint)bfe_i32(%s, %s, %s)", u(t, in, 0), u(t, in, 1), u(t, in, 2)));
        return true;
    case OP_V_BFI_B32:
        vwrite(t, in, fmt(t, "((%s & %s) | (~%s & %s))", u(t, in, 0), u(t, in, 1), u(t, in, 0), u(t, in, 2)));
        return true;
    case OP_V_BFM_B32:
        vwrite(t, in, fmt(t, "(((1u << (%s & 31u)) - 1u) << (%s & 31u))", u(t, in, 0), u(t, in, 1)));
        return true;
    case OP_V_ALIGNBIT_B32:
        vwrite(t, in,
               fmt(t, "(uint)((((uint64_t)%s << 32) | %s) >> (%s & 31u))", u(t, in, 0), u(t, in, 1),
                   u(t, in, 2)));
        return true;
    case OP_V_ALIGNBYTE_B32:
        vwrite(t, in,
               fmt(t, "(uint)((((uint64_t)%s << 32) | %s) >> ((%s & 3u) * 8u))", u(t, in, 0), u(t, in, 1),
                   u(t, in, 2)));
        return true;
    case OP_V_PERM_B32:
        vwrite(t, in, fmt(t, "perm_b32(%s, %s, %s)", u(t, in, 0), u(t, in, 1), u(t, in, 2)));
        return true;
    case OP_V_LSHLREV_B64:
    case OP_V_LSHRREV_B64:
    case OP_V_ASHRREV_I64: {
        const char* v = fmt(t, "((uint64_t)%s | ((uint64_t)%s << 32))", src_u32(t, in, 1, TYPE_64, 0),
                            src_u32(t, in, 1, TYPE_64, 1));
        const char* s = fmt(t, "(%s & 63u)", u(t, in, 0));
        const char* r = in.op == OP_V_LSHLREV_B64   ? fmt(t, "(%s << %s)", v, s)
                        : in.op == OP_V_LSHRREV_B64 ? fmt(t, "(%s >> %s)", v, s)
                                                    : fmt(t, "(uint64_t)((int64_t)%s >> %s)", v, s);
        emit(t, "{ uint64_t r_ = %s;", r);
        t.depth++;
        write_vgpr(t, in, in.dst[0].reg, "(uint)r_");
        write_vgpr(t, in, in.dst[0].reg + 1, "(uint)(r_ >> 32)");
        t.depth--;
        emit(t, "}");
        return true;
    }
    // Carries: VOP2 forms use VCC, VOP3 forms name the masks.
    case OP_V_ADD_CO_U32:
    case OP_V_SUB_CO_U32:
    case OP_V_SUBREV_CO_U32:
    case OP_V_ADD_CO_CI_U32:
    case OP_V_SUB_CO_CI_U32:
    case OP_V_SUBREV_CO_CI_U32: {
        bool carry_in =
            in.op == OP_V_ADD_CO_CI_U32 || in.op == OP_V_SUB_CO_CI_U32 || in.op == OP_V_SUBREV_CO_CI_U32;
        bool sub = in.op == OP_V_SUB_CO_U32 || in.op == OP_V_SUB_CO_CI_U32;
        bool rev = in.op == OP_V_SUBREV_CO_U32 || in.op == OP_V_SUBREV_CO_CI_U32;
        const char* a = u(t, in, rev ? 1 : 0);
        const char* b = u(t, in, rev ? 0 : 1);
        const char* c = carry_in ? fmt(t, "(%s ? 1u : 0u)", lane_bit(t, in, 2)) : "0u";
        if (sub || rev)
            emit(t,
                 "{ uint64_t w_ = (uint64_t)%s - %s - %s; uint carry_ = mask_of(EXEC && (w_ >> 32) != 0u);",
                 a, b, c);
        else
            emit(t,
                 "{ uint64_t w_ = (uint64_t)%s + %s + %s; uint carry_ = mask_of(EXEC && (w_ >> 32) != 0u);",
                 a, b, c);
        t.depth++;
        write_vgpr(t, in, in.dst[0].reg, "(uint)w_");
        RdnaOperand carry = in.dst[1];
        if (carry.kind == OPND_NONE)
            carry = in.dst[0], carry.kind = OPND_SPECIAL, carry.reg = SRC_VCC_LO;
        write_sgpr(t, carry, 0, "carry_");
        if (!t.wave32)
            write_sgpr(t, carry, 1, "carry_");
        t.depth--;
        emit(t, "}");
        return true;
    }
    case OP_V_MAD_U64_U32: {
        emit(t, "{ uint64_t w_ = (uint64_t)%s * %s + ((uint64_t)%s | ((uint64_t)%s << 32));", u(t, in, 0),
             u(t, in, 1), src_u32(t, in, 2, TYPE_64, 0), src_u32(t, in, 2, TYPE_64, 1));
        t.depth++;
        write_vgpr(t, in, in.dst[0].reg, "(uint)w_");
        write_vgpr(t, in, in.dst[0].reg + 1, "(uint)(w_ >> 32)");
        write_sgpr(t, in.dst[1], 0, "0u");
        if (!t.wave32)
            write_sgpr(t, in.dst[1], 1, "0u");
        t.depth--;
        emit(t, "}");
        return true;
    }
    // 16-bit floats.
    case OP_V_ADD_F16:
        vhalf(t, in, fmt(t, "(%s + %s)", h(t, in, 0), h(t, in, 1)));
        return true;
    case OP_V_SUB_F16:
        vhalf(t, in, fmt(t, "(%s - %s)", h(t, in, 0), h(t, in, 1)));
        return true;
    case OP_V_SUBREV_F16:
        vhalf(t, in, fmt(t, "(%s - %s)", h(t, in, 1), h(t, in, 0)));
        return true;
    case OP_V_MUL_F16:
        vhalf(t, in, fmt(t, "(%s * %s)", h(t, in, 0), h(t, in, 1)));
        return true;
    case OP_V_MAX_F16:
        vhalf(t, in, fmt(t, "max(%s, %s)", h(t, in, 0), h(t, in, 1)));
        return true;
    case OP_V_MIN_F16:
        vhalf(t, in, fmt(t, "min(%s, %s)", h(t, in, 0), h(t, in, 1)));
        return true;
    case OP_V_FMA_F16:
        vhalf(t, in, fmt(t, "mad(%s, %s, %s)", h(t, in, 0), h(t, in, 1), h(t, in, 2)));
        return true;
    case OP_V_FMAC_F16: {
        const char* acc = fmt(t, "f16tof32(%s)", vgpr(t, in.dst[0].reg));
        vhalf(t, in, fmt(t, "mad(%s, %s, %s)", h(t, in, 0), h(t, in, 1), acc));
        return true;
    }
    case OP_V_CVT_F16_U16:
        vhalf(t, in, fmt(t, "(float)(%s & 0xffffu)", u(t, in, 0)));
        return true;
    case OP_V_CVT_F16_I16:
        vhalf(t, in, fmt(t, "(float)((int)(%s << 16) >> 16)", u(t, in, 0)));
        return true;
    case OP_V_CVT_U16_F16:
        vwrite(t, in, fmt(t, "(f32_to_u32(%s) & 0xffffu)", h(t, in, 0)));
        return true;
    case OP_V_CVT_I16_F16:
        vhalf_u16(t, in, fmt(t, "(uint)clamp(f32_to_i32(%s), -32768, 32767)", h(t, in, 0)));
        return true;
    case OP_V_RCP_F16:
        vhalf(t, in, fmt(t, "rcp(%s)", h(t, in, 0)));
        return true;
    case OP_V_SQRT_F16:
        vhalf(t, in, fmt(t, "sqrt(%s)", h(t, in, 0)));
        return true;
    case OP_V_RSQ_F16:
        vhalf(t, in, fmt(t, "rsqrt(%s)", h(t, in, 0)));
        return true;
    case OP_V_EXP_F16:
        vhalf(t, in, fmt(t, "exp2(%s)", h(t, in, 0)));
        return true;
    case OP_V_LOG_F16:
        vhalf(t, in, fmt(t, "log2(%s)", h(t, in, 0)));
        return true;
    case OP_V_FRACT_F16:
        vhalf(t, in, fmt(t, "frac(%s)", h(t, in, 0)));
        return true;
    case OP_V_FLOOR_F16:
        vhalf(t, in, fmt(t, "floor(%s)", h(t, in, 0)));
        return true;
    case OP_V_CEIL_F16:
        vhalf(t, in, fmt(t, "ceil(%s)", h(t, in, 0)));
        return true;
    case OP_V_TRUNC_F16:
        vhalf(t, in, fmt(t, "trunc(%s)", h(t, in, 0)));
        return true;
    case OP_V_RNDNE_F16:
        vhalf(t, in, fmt(t, "round(%s)", h(t, in, 0)));
        return true;
    case OP_V_SIN_F16:
        vhalf(t, in, fmt(t, "sin_rev(%s)", h(t, in, 0)));
        return true;
    case OP_V_COS_F16:
        vhalf(t, in, fmt(t, "cos_rev(%s)", h(t, in, 0)));
        return true;
    case OP_V_ADD_NC_U16:
    case OP_V_SUB_NC_U16:
    case OP_V_MUL_LO_U16:
    case OP_V_MAX_U16:
    case OP_V_MIN_U16:
    case OP_V_LSHLREV_B16:
    case OP_V_LSHRREV_B16:
    case OP_V_ADD_NC_I16:
    case OP_V_SUB_NC_I16:
    case OP_V_MAX_I16:
    case OP_V_MIN_I16:
    case OP_V_ASHRREV_I16: {
        bool is_signed = in.op == OP_V_ADD_NC_I16 || in.op == OP_V_SUB_NC_I16 || in.op == OP_V_MAX_I16 ||
                         in.op == OP_V_MIN_I16 || in.op == OP_V_ASHRREV_I16;
        const char* a = u16_src(t, in, 0, is_signed && in.op != OP_V_ASHRREV_I16);
        const char* b = u16_src(t, in, 1, is_signed);
        const char* r = nullptr;
        switch (in.op) {
        case OP_V_ADD_NC_U16:
        case OP_V_ADD_NC_I16:
            r = fmt(t, "(uint)(%s + %s)", a, b);
            break;
        case OP_V_SUB_NC_U16:
        case OP_V_SUB_NC_I16:
            r = fmt(t, "(uint)(%s - %s)", a, b);
            break;
        case OP_V_MUL_LO_U16:
            r = fmt(t, "(%s * %s)", a, b);
            break;
        case OP_V_MAX_U16:
        case OP_V_MAX_I16:
            r = fmt(t, "(uint)max(%s, %s)", a, b);
            break;
        case OP_V_MIN_U16:
        case OP_V_MIN_I16:
            r = fmt(t, "(uint)min(%s, %s)", a, b);
            break;
        case OP_V_LSHLREV_B16:
            r = fmt(t, "(%s << (%s & 15u))", b, a);
            break;
        case OP_V_LSHRREV_B16:
            r = fmt(t, "(%s >> (%s & 15u))", b, a);
            break;
        default:
            r = fmt(t, "(uint)(%s >> (%s & 15u))", b, a);
            break;
        }
        vhalf_u16(t, in, r);
        return true;
    }
    case OP_V_PERMLANE16_B32:
    case OP_V_PERMLANEX16_B32: {
        bool x16 = in.op == OP_V_PERMLANEX16_B32;
        bool fi = (in.op_sel & 1) != 0;
        bool bc = (in.op_sel & 2) != 0;
        emit(t, "{ uint l_ = WaveGetLaneIndex(); uint idx_ = l_ & 15u; uint m_ = idx_ < 8u ? %s : %s;",
             u(t, in, 2), u(t, in, 1));
        t.depth++;
        emit(t, "uint sl_ = (%s) | ((m_ >> ((idx_ & 7u) * 4u)) & 15u);",
             x16 ? "((l_ & ~15u) ^ 16u)" : "(l_ & ~15u)");
        if (fi) {
            emit(t, "uint r_ = WaveReadLaneAt(%s, sl_);", u(t, in, 0));
        } else {
            emit(t, "uint r_ = WaveReadLaneAt(EXEC ? 1u : 0u, sl_) != 0u ? WaveReadLaneAt(%s, sl_) : %s;",
                 u(t, in, 0), bc ? "0u" : vgpr(t, in.dst[0].reg));
        }
        vwrite(t, in, "r_");
        t.depth--;
        emit(t, "}");
        return true;
    }
    case OP_V_PACK_B32_F16: {
        const char* lo = fmt(t, "f32tof16(%s)", h(t, in, 0));
        const char* hi = fmt(t, "f32tof16(%s)", h(t, in, 1));
        vwrite(t, in, fmt(t, "(%s | (%s << 16))", lo, hi));
        return true;
    }
    case OP_V_PK_FMAC_F16: {
        auto half2 = [&](u32 idx) -> const char* {
            const char* bits = src_u32(t, in, idx, TYPE_16);
            if (in.src[idx].kind == OPND_INLINE && in.src[idx].reg >= SRC_HALF)
                return fmt(t, "(%s | (%s << 16))", bits, bits);
            return bits;
        };
        const char* a = half2(0);
        const char* b = half2(1);
        const char* d = vgpr(t, in.dst[0].reg);
        emit(t, "{ uint a_ = %s, b_ = %s, d_ = %s;", a, b, d);
        t.depth++;
        emit(t, "uint lo_ = f32tof16(mad(f16tof32(a_), f16tof32(b_), f16tof32(d_)));");
        emit(t, "uint hi_ = f32tof16(mad(f16tof32(a_ >> 16), f16tof32(b_ >> 16), f16tof32(d_ >> 16)));");
        vwrite(t, in, "(lo_ | (hi_ << 16))");
        t.depth--;
        emit(t, "}");
        return true;
    }
    case OP_V_SWAP_B32: {
        const char* a = vgpr(t, in.dst[0].reg);
        const char* b = vgpr(t, in.src[0].reg);
        emit(t, "if (EXEC) { uint x_ = %s; %s = %s; %s = x_; }", a, a, b, b);
        return true;
    }
    default:
        break;
    }
    translator_fail(t, "unsupported %s", rdna_op_name(in.op));
    return false;
}

// VOP3P: packed 16-bit math and mixed precision FMA.
static const char* mix_source(Translator& t, const RdnaInst& in, u32 i) {
    // op_sel_hi set: a half from the op_sel-selected half; clear: a full float.
    bool half = (in.op_sel_hi >> i) & 1;
    const char* bits = src_u32(t, in, i, half ? TYPE_16 : TYPE_32);
    const char* v = !half                    ? fmt(t, "asfloat(%s)", bits)
                    : ((in.op_sel >> i) & 1) ? fmt(t, "f16tof32(%s >> 16)", bits)
                                             : fmt(t, "f16tof32(%s)", bits);
    if ((in.neg_hi >> i) & 1)
        v = fmt(t, "abs(%s)", v); // for mix instructions, neg_hi is abs
    if (in.src[i].neg)
        v = fmt(t, "(-%s)", v);
    return v;
}

static const char* packed_half(Translator& t, const RdnaInst& in, u32 i, bool high) {
    const char* bits = src_u32(t, in, i, TYPE_16);
    bool sel = high ? (in.op_sel_hi >> i) & 1 : (in.op_sel >> i) & 1;
    const char* v = sel ? fmt(t, "f16tof32(%s >> 16)", bits) : fmt(t, "f16tof32(%s)", bits);
    bool neg = high ? (in.neg_hi >> i) & 1 : in.src[i].neg;
    return neg ? fmt(t, "(-%s)", v) : v;
}

static const char* packed_u16(Translator& t, const RdnaInst& in, u32 i, bool high, bool is_signed) {
    const char* bits = src_u32(t, in, i, TYPE_16);
    bool sel = high ? (in.op_sel_hi >> i) & 1 : (in.op_sel >> i) & 1;
    const char* lo = sel ? fmt(t, "(%s >> 16)", bits) : fmt(t, "(%s & 0xffffu)", bits);
    return is_signed ? fmt(t, "((int)(%s << 16) >> 16)", lo) : lo;
}

static bool emit_vop3p(Translator& t, const RdnaInst& in) {
    switch (in.op) {
    case OP_V_FMA_MIX_F32:
        vfloat(t, in,
               fmt(t, "mad(%s, %s, %s)", mix_source(t, in, 0), mix_source(t, in, 1), mix_source(t, in, 2)));
        return true;
    case OP_V_FMA_MIXLO_F16:
    case OP_V_FMA_MIXHI_F16: {
        const char* v =
            fmt(t, "mad(%s, %s, %s)", mix_source(t, in, 0), mix_source(t, in, 1), mix_source(t, in, 2));
        if (in.clamp)
            v = fmt(t, "saturate(%s)", v);
        const char* dst = vgpr(t, in.dst[0].reg);
        if (in.op == OP_V_FMA_MIXLO_F16)
            vwrite(t, in, fmt(t, "((%s & 0xffff0000u) | f32tof16(%s))", dst, v));
        else
            vwrite(t, in, fmt(t, "((%s & 0xffffu) | (f32tof16(%s) << 16))", dst, v));
        return true;
    }
    case OP_V_PK_ADD_F16:
    case OP_V_PK_MUL_F16:
    case OP_V_PK_MAX_F16:
    case OP_V_PK_MIN_F16:
    case OP_V_PK_FMA_F16: {
        const char* r[2];
        for (u32 hi = 0; hi < 2; hi++) {
            const char* a = packed_half(t, in, 0, hi);
            const char* b = packed_half(t, in, 1, hi);
            switch (in.op) {
            case OP_V_PK_ADD_F16:
                r[hi] = fmt(t, "(%s + %s)", a, b);
                break;
            case OP_V_PK_MUL_F16:
                r[hi] = fmt(t, "(%s * %s)", a, b);
                break;
            case OP_V_PK_MAX_F16:
                r[hi] = fmt(t, "max(%s, %s)", a, b);
                break;
            case OP_V_PK_MIN_F16:
                r[hi] = fmt(t, "min(%s, %s)", a, b);
                break;
            default:
                r[hi] = fmt(t, "mad(%s, %s, %s)", a, b, packed_half(t, in, 2, hi));
                break;
            }
            if (in.clamp)
                r[hi] = fmt(t, "saturate(%s)", r[hi]);
        }
        vwrite(t, in, fmt(t, "(f32tof16(%s) | (f32tof16(%s) << 16))", r[0], r[1]));
        return true;
    }
    case OP_V_PK_LSHLREV_B16:
    case OP_V_PK_LSHRREV_B16:
    case OP_V_PK_ASHRREV_I16:
    case OP_V_PK_ADD_I16:
    case OP_V_PK_SUB_I16:
    case OP_V_PK_MAX_I16:
    case OP_V_PK_MIN_I16:
    case OP_V_PK_ADD_U16:
    case OP_V_PK_SUB_U16:
    case OP_V_PK_MAX_U16:
    case OP_V_PK_MIN_U16:
    case OP_V_PK_MUL_LO_U16:
    case OP_V_PK_MAD_I16:
    case OP_V_PK_MAD_U16: {
        bool is_signed = in.op == OP_V_PK_ASHRREV_I16 || in.op == OP_V_PK_ADD_I16 ||
                         in.op == OP_V_PK_SUB_I16 || in.op == OP_V_PK_MAX_I16 || in.op == OP_V_PK_MIN_I16 ||
                         in.op == OP_V_PK_MAD_I16;
        const char* r[2];
        for (u32 hi = 0; hi < 2; hi++) {
            const char* a = packed_u16(t, in, 0, hi, is_signed && in.op != OP_V_PK_ASHRREV_I16);
            const char* b = packed_u16(t, in, 1, hi, is_signed);
            switch (in.op) {
            case OP_V_PK_LSHLREV_B16:
                r[hi] = fmt(t, "((%s << (%s & 15u)) & 0xffffu)", b, a);
                break;
            case OP_V_PK_LSHRREV_B16:
                r[hi] = fmt(t, "(%s >> (%s & 15u))", b, a);
                break;
            case OP_V_PK_ASHRREV_I16:
                r[hi] = fmt(t, "((uint)(%s >> (%s & 15u)) & 0xffffu)", b, a);
                break;
            case OP_V_PK_ADD_I16:
            case OP_V_PK_ADD_U16:
                r[hi] = fmt(t, "(uint)(%s + %s)", a, b);
                break;
            case OP_V_PK_SUB_I16:
            case OP_V_PK_SUB_U16:
                r[hi] = fmt(t, "(uint)(%s - %s)", a, b);
                break;
            case OP_V_PK_MUL_LO_U16:
                r[hi] = fmt(t, "(%s * %s)", a, b);
                break;
            case OP_V_PK_MAX_I16:
            case OP_V_PK_MAX_U16:
                r[hi] = fmt(t, "(uint)max(%s, %s)", a, b);
                break;
            case OP_V_PK_MIN_I16:
            case OP_V_PK_MIN_U16:
                r[hi] = fmt(t, "(uint)min(%s, %s)", a, b);
                break;
            default:
                r[hi] = fmt(t, "(uint)(%s * %s + %s)", a, b, packed_u16(t, in, 2, hi, is_signed));
                break;
            }
            if (in.clamp) {
                if (is_signed)
                    r[hi] = fmt(t, "(uint)clamp((int)%s, -32768, 32767)", r[hi]);
                else
                    r[hi] = fmt(t, "min(%s, 0xffffu)", r[hi]);
            }
        }
        vwrite(t, in, fmt(t, "((%s & 0xffffu) | ((%s & 0xffffu) << 16))", r[0], r[1]));
        return true;
    }
    case OP_V_DOT2_F32_F16: {
        const char* a0 = packed_half(t, in, 0, false);
        const char* a1 = packed_half(t, in, 0, true);
        const char* b0 = packed_half(t, in, 1, false);
        const char* b1 = packed_half(t, in, 1, true);
        vfloat(t, in, fmt(t, "(%s * %s + %s * %s + asfloat(%s))", a0, b0, a1, b1, src_u32(t, in, 2)));
        return true;
    }
    default:
        translator_fail(t, "unsupported %s", rdna_op_name(in.op));
        return false;
    }
}

static bool begin_dpp(Translator& t, const RdnaInst& in) {
    const char* v = vgpr(t, in.src[0].reg);
    u32 c = in.dpp.ctrl;
    const char* addr = nullptr;
    bool always_in_bounds = true;
    if (in.dpp.dpp8) {
        u32 packed = 0;
        for (u32 i = 0; i < 8; i++)
            packed |= (u32)(in.dpp.sel8[i] & 7u) << (3 * i);
        addr = fmt(t, "uint sl_ = (l_ & ~7u) | ((0x%06xu >> ((l_ & 7u) * 3u)) & 7u); bool b_ = true;", packed);
    } else if (c <= 0xFF) {
        addr = fmt(t, "uint sl_ = (l_ & ~3u) | ((0x%02xu >> ((l_ & 3u) * 2u)) & 3u); bool b_ = true;", c);
    } else if (c >= 0x101 && c <= 0x10F) {
        u32 k = c - 0x100;
        always_in_bounds = false;
        addr = fmt(t, "bool b_ = (l_ & 15u) < %uu; uint sl_ = l_ + %uu;", 16u - k, k);
    } else if (c >= 0x111 && c <= 0x11F) {
        u32 k = c - 0x110;
        always_in_bounds = false;
        addr = fmt(t, "bool b_ = (l_ & 15u) >= %uu; uint sl_ = l_ - %uu;", k, k);
    } else if (c >= 0x121 && c <= 0x12F) {
        u32 k = c - 0x120;
        addr = fmt(t, "uint sl_ = (l_ & ~15u) | ((l_ + %uu) & 15u); bool b_ = true;", 16u - k);
    } else if (c == 0x130) {
        always_in_bounds = false;
        addr = "bool b_ = (l_ + 1u) < WaveGetLaneCount(); uint sl_ = l_ + 1u;";
    } else if (c == 0x134) {
        addr = "uint sl_ = (l_ + 1u) & (WaveGetLaneCount() - 1u); bool b_ = true;";
    } else if (c == 0x138) {
        always_in_bounds = false;
        addr = "bool b_ = l_ > 0u; uint sl_ = l_ - 1u;";
    } else if (c == 0x13C) {
        addr = "uint sl_ = (l_ + WaveGetLaneCount() - 1u) & (WaveGetLaneCount() - 1u); bool b_ = true;";
    } else if (c == 0x140) {
        addr = "uint sl_ = l_ ^ 15u; bool b_ = true;";
    } else if (c == 0x141) {
        addr = "uint sl_ = l_ ^ 7u; bool b_ = true;";
    } else if (c == 0x142) {
        always_in_bounds = false;
        addr = "bool b_ = l_ >= 16u; uint sl_ = (l_ & ~15u) - 1u;";
    } else if (c == 0x143) {
        always_in_bounds = false;
        addr = "bool b_ = l_ >= 32u; uint sl_ = (l_ & ~31u) - 1u;";
    } else if (c >= 0x150 && c <= 0x15F) {
        addr = fmt(t, "uint sl_ = (l_ & ~15u) | %uu; bool b_ = true;", c - 0x150);
    } else if (c >= 0x160 && c <= 0x16F) {
        addr = fmt(t, "uint sl_ = l_ ^ %uu; bool b_ = true;", c - 0x160);
    } else {
        translator_fail(t, "unsupported DPP ctrl 0x%x on %s", c, rdna_op_name(in.op));
        return false;
    }
    emit(t, "{ uint l_ = WaveGetLaneIndex(); %s", addr);
    t.depth++;
    const char* src_expr = in.dpp.fetch_inactive ? v : fmt(t, "(EXEC ? %s : 0u)", v);
    emit(t, "uint dpp0_ = b_ ? WaveReadLaneAt(%s, b_ ? (sl_ & (WaveGetLaneCount() - 1u)) : 0u) : 0u;",
         src_expr);
    if (!in.dpp.dpp8 && (in.dpp.row_mask != 0xF || in.dpp.bank_mask != 0xF || (!always_in_bounds && !in.dpp.bound_ctrl))) {
        emit(t, "bool dpp_en_ = ((0x%xu >> ((l_ >> 4) & 3u)) & 1u) != 0u && ((0x%xu >> ((l_ >> 2) & 3u)) & 1u) != 0u%s;",
             in.dpp.row_mask, in.dpp.bank_mask, in.dpp.bound_ctrl ? "" : " && b_");
        t.dpp_has_mask = true;
    }
    t.dpp_active = true;
    return true;
}

bool emit_alu(Translator& t, const RdnaInst& in) {
    switch (in.enc) {
    case ENC_SOP1:
    case ENC_SOP2:
    case ENC_SOPC:
    case ENC_SOPK:
        return emit_scalar(t, in);
    case ENC_VOP3P:
        return emit_vop3p(t, in);
    default:
        break;
    }
    bool dpp = in.has_dpp;
    if (dpp && !begin_dpp(t, in))
        return false;
    bool ok = rdna_op_encoding(in.op) == ENC_VOPC ? emit_compare(t, in) : emit_vector(t, in);
    if (dpp) {
        t.dpp_active = false;
        t.dpp_has_mask = false;
        t.depth--;
        emit(t, "}");
    }
    return ok;
}
