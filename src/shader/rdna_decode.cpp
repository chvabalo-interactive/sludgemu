// RDNA 2 instruction decoding.
//
// Each encoding is recognized by its fixed high bits; the opcode field then indexes a per-encoding
// table built from rdna_ops.inc. Operand widths (how many registers a source or destination
// covers) follow from the instruction's data types.

#include <string.h>

#include "shader/rdna.h"

struct OpInfo {
    const char* name; // lower case mnemonic
    RdnaEncoding enc;
    u16 opcode;
};

static OpInfo g_ops[OP_COUNT];
static char g_names[OP_COUNT][32];

// Opcode -> op, per encoding. VOP3 covers the promoted VOPC, VOP2 and VOP1 forms as well.
static RdnaOp g_sop2[128], g_sopk[32], g_sop1[256], g_sopc[128], g_sopp[128], g_smem[256];
static RdnaOp g_vop2[64], g_vop1[256], g_vopc[256], g_vop3[1024], g_vop3p[128], g_vintrp[4];
static RdnaOp g_ds[256], g_mubuf[256], g_mtbuf[16], g_mimg[256], g_global[128];

static const struct {
    const char* name;
    RdnaEncoding enc;
    u16 opcode;
} k_op_table[] = {
#define RDNA_OP(name, enc, opcode) {#name, ENC_##enc, opcode},
#include "shader/rdna_ops.inc"
#undef RDNA_OP
};
static_assert(sizeof(k_op_table) / sizeof(k_op_table[0]) == OP_COUNT - 1, "op table");

void rdna_init() {
    g_ops[OP_INVALID] = {"<invalid>", ENC_INVALID, 0};
    for (u32 i = 0; i < OP_COUNT - 1; i++) {
        RdnaOp op = (RdnaOp)(i + 1);
        const char* src = k_op_table[i].name;
        char* name = g_names[op];
        usize n = 0;
        for (; src[n] && n + 1 < sizeof(g_names[op]); n++)
            name[n] = (src[n] >= 'A' && src[n] <= 'Z') ? (char)(src[n] - 'A' + 'a') : src[n];
        name[n] = 0;
        g_ops[op] = {name, k_op_table[i].enc, k_op_table[i].opcode};
        u16 code = k_op_table[i].opcode;
        switch (k_op_table[i].enc) {
        case ENC_SOP2:
            g_sop2[code] = op;
            break;
        case ENC_SOPK:
            g_sopk[code] = op;
            break;
        case ENC_SOP1:
            g_sop1[code] = op;
            break;
        case ENC_SOPC:
            g_sopc[code] = op;
            break;
        case ENC_SOPP:
            g_sopp[code] = op;
            break;
        case ENC_SMEM:
            g_smem[code] = op;
            break;
        case ENC_VOP2:
            g_vop2[code] = op;
            g_vop3[0x100 + code] = op;
            break;
        case ENC_VOP1:
            g_vop1[code] = op;
            g_vop3[0x180 + code] = op;
            break;
        case ENC_VOPC:
            g_vopc[code] = op;
            g_vop3[code] = op;
            break;
        case ENC_VOP3:
            g_vop3[code] = op;
            break;
        case ENC_VOP3P:
            g_vop3p[code] = op;
            break;
        case ENC_VINTRP:
            g_vintrp[code] = op;
            break;
        case ENC_DS:
            g_ds[code] = op;
            break;
        case ENC_MUBUF:
            g_mubuf[code] = op;
            break;
        case ENC_MTBUF:
            g_mtbuf[code] = op;
            break;
        case ENC_MIMG:
            g_mimg[code] = op;
            break;
        case ENC_GLOBAL:
            g_global[code] = op;
            break;
        default:
            break;
        }
    }
}

const char* rdna_op_name(RdnaOp op) {
    return op < OP_COUNT ? g_ops[op].name : "<invalid>";
}

RdnaEncoding rdna_op_encoding(RdnaOp op) {
    return op < OP_COUNT ? g_ops[op].enc : ENC_INVALID;
}

// --- operand widths ----------------------------------------------------------------------------------

static bool name_has(RdnaOp op, const char* part) {
    return strstr(g_ops[op].name, part) != nullptr;
}

// Registers covered by each operand: destination, then sources.
struct Widths {
    u8 dst;
    u8 src[3];
};

static Widths op_widths(RdnaOp op) {
    Widths w = {1, {1, 1, 1}};
    bool wide = name_has(op, "_b64") || name_has(op, "_i64") || name_has(op, "_u64") || name_has(op, "_f64");
    RdnaEncoding enc = g_ops[op].enc;

    if (enc == ENC_VOPC) {
        // Compares write a lane mask (an SGPR pair in wave64).
        w.dst = 2;
        if (wide)
            w.src[0] = w.src[1] = 2;
        if (op == OP_V_CMP_CLASS_F64 || op == OP_V_CMPX_CLASS_F64)
            w.src[1] = 1;
        return w;
    }
    if (enc == ENC_SOP1 || enc == ENC_SOP2 || enc == ENC_SOPC) {
        if (wide)
            w.dst = w.src[0] = w.src[1] = 2;
        switch (op) {
        case OP_S_LSHL_B64:
        case OP_S_LSHR_B64:
        case OP_S_ASHR_I64:
        case OP_S_BFE_U64:
        case OP_S_BFE_I64:
        case OP_S_BITCMP0_B64:
        case OP_S_BITCMP1_B64:
            w.src[1] = 1;
            break;
        case OP_S_BFM_B64:
            w.src[0] = w.src[1] = 1;
            break;
        case OP_S_BCNT0_I32_B64:
        case OP_S_BCNT1_I32_B64:
        case OP_S_FF0_I32_B64:
        case OP_S_FF1_I32_B64:
        case OP_S_FLBIT_I32_B64:
        case OP_S_FLBIT_I32_I64:
            w.dst = 1;
            break;
        case OP_S_BITSET0_B64:
        case OP_S_BITSET1_B64:
        case OP_S_BITREPLICATE_B64_B32:
            w.src[0] = 1;
            break;
        case OP_S_GETPC_B64:
            w.dst = 2;
            break;
        case OP_S_SETPC_B64:
        case OP_S_SWAPPC_B64:
        case OP_S_RFE_B64:
            w.dst = 2;
            w.src[0] = 2;
            break;
        case OP_S_CSELECT_B64:
            w.dst = w.src[0] = w.src[1] = 2;
            break;
        default:
            break;
        }
        return w;
    }
    if (enc == ENC_VOP1 || enc == ENC_VOP2 || enc == ENC_VOP3) {
        if (wide)
            w.dst = w.src[0] = w.src[1] = w.src[2] = 2;
        switch (op) {
        case OP_V_CVT_I32_F64:
        case OP_V_CVT_U32_F64:
        case OP_V_CVT_F32_F64:
        case OP_V_FREXP_EXP_I32_F64:
            w.dst = 1;
            break;
        case OP_V_CVT_F64_I32:
        case OP_V_CVT_F64_U32:
        case OP_V_CVT_F64_F32:
            w.src[0] = 1;
            break;
        case OP_V_LDEXP_F64:
        case OP_V_TRIG_PREOP_F64:
            w.src[1] = 1;
            break;
        case OP_V_LSHLREV_B64:
        case OP_V_LSHRREV_B64:
        case OP_V_ASHRREV_I64:
            w.src[0] = 1;
            break;
        case OP_V_MAD_U64_U32:
        case OP_V_MAD_I64_I32:
            w.dst = 2;
            w.src[0] = w.src[1] = 1;
            w.src[2] = 2;
            break;
        default:
            break;
        }
        return w;
    }
    return w;
}

// Number of sources for VOP3-only instructions (VOP3-encoded VOP1/VOP2/VOPC use their own counts).
static u8 vop3_sources(RdnaOp op) {
    switch (op) {
    case OP_V_ADD_F64:
    case OP_V_MUL_F64:
    case OP_V_MIN_F64:
    case OP_V_MAX_F64:
    case OP_V_LDEXP_F64:
    case OP_V_MUL_LO_U32:
    case OP_V_MUL_HI_U32:
    case OP_V_MUL_HI_I32:
    case OP_V_TRIG_PREOP_F64:
    case OP_V_LSHLREV_B64:
    case OP_V_LSHRREV_B64:
    case OP_V_ASHRREV_I64:
    case OP_V_ADD_NC_U16:
    case OP_V_SUB_NC_U16:
    case OP_V_MUL_LO_U16:
    case OP_V_LSHRREV_B16:
    case OP_V_ASHRREV_I16:
    case OP_V_MAX_U16:
    case OP_V_MAX_I16:
    case OP_V_MIN_U16:
    case OP_V_MIN_I16:
    case OP_V_ADD_NC_I16:
    case OP_V_SUB_NC_I16:
    case OP_V_ADD_CO_U32:
    case OP_V_SUB_CO_U32:
    case OP_V_SUBREV_CO_U32:
    case OP_V_PACK_B32_F16:
    case OP_V_CVT_PKNORM_I16_F16:
    case OP_V_CVT_PKNORM_U16_F16:
    case OP_V_LSHLREV_B16:
    case OP_V_READLANE_B32:
    case OP_V_WRITELANE_B32:
    case OP_V_LDEXP_F32:
    case OP_V_BFM_B32:
    case OP_V_BCNT_U32_B32:
    case OP_V_MBCNT_LO_U32_B32:
    case OP_V_MBCNT_HI_U32_B32:
    case OP_V_CVT_PKNORM_I16_F32:
    case OP_V_CVT_PKNORM_U16_F32:
    case OP_V_CVT_PK_U16_U32:
    case OP_V_CVT_PK_I16_I32:
    case OP_V_SUB_NC_I32:
    case OP_V_ADD_NC_I32:
        return 2;
    default:
        return 3;
    }
}

static u8 vop2_sources(RdnaOp op) {
    switch (op) {
    case OP_V_CNDMASK_B32:
    case OP_V_ADD_CO_CI_U32:
    case OP_V_SUB_CO_CI_U32:
    case OP_V_SUBREV_CO_CI_U32:
        return 3; // the carry-in / select mask is the third source (VCC unless VOP3)
    default:
        return 2;
    }
}

static bool vop1_no_source(RdnaOp op) {
    return op == OP_V_NOP || op == OP_V_PIPEFLUSH || op == OP_V_CLREXCP;
}

// VOP3b: a scalar carry-out (or the like) in place of abs/op_sel/clamp... bits.
static bool is_vop3b(RdnaOp op) {
    switch (op) {
    case OP_V_ADD_CO_U32:
    case OP_V_SUB_CO_U32:
    case OP_V_SUBREV_CO_U32:
    case OP_V_ADD_CO_CI_U32:
    case OP_V_SUB_CO_CI_U32:
    case OP_V_SUBREV_CO_CI_U32:
    case OP_V_DIV_SCALE_F32:
    case OP_V_DIV_SCALE_F64:
    case OP_V_MAD_U64_U32:
    case OP_V_MAD_I64_I32:
        return true;
    default:
        return false;
    }
}

// The 16-bit interpolation instructions: VOP3 with the attribute in the src0 field.
static bool is_vop3_interpolation(RdnaOp op) {
    return op == OP_V_INTERP_P1LL_F16 || op == OP_V_INTERP_P1LV_F16 || op == OP_V_INTERP_P2_F16;
}

// --- operands ----------------------------------------------------------------------------------------

struct Reader {
    const u32* code;
    u32 count;
    u32 at;    // dword index of the instruction
    u32 extra; // dwords consumed after the base encoding (literal)
    bool has_literal;
    u32 literal_at;
    bool ok;
};

static RdnaOperand source(Reader& r, u32 field, u8 count) {
    RdnaOperand o = {};
    o.count = count;
    if (field >= SRC_VGPR0) {
        o.kind = OPND_VGPR;
        o.reg = (u16)(field - SRC_VGPR0);
    } else if (field <= 105 || (field >= SRC_TTMP0 && field < SRC_M0)) {
        o.kind = OPND_SGPR;
        o.reg = (u16)field;
    } else if (field == SRC_LITERAL) {
        o.kind = OPND_LITERAL;
        if (!r.has_literal) {
            r.has_literal = true;
            r.literal_at = r.extra++;
        }
        u32 at = r.at + r.literal_at;
        if (at < r.count)
            o.value = r.code[at];
        else
            r.ok = false;
    } else if ((field >= SRC_ZERO && field <= SRC_INT_NEG_MAX) ||
               (field >= SRC_HALF && field <= SRC_INV_2PI)) {
        o.kind = OPND_INLINE;
        o.reg = (u16)field;
    } else {
        o.kind = OPND_SPECIAL;
        o.reg = (u16)field;
        if (field == SRC_VCC_LO || field == SRC_EXEC_LO)
            o.count = count;
    }
    return o;
}

static RdnaOperand sgpr(u32 field, u8 count) {
    RdnaOperand o = {};
    o.count = count;
    if (field <= 105 || (field >= SRC_TTMP0 && field < SRC_M0)) {
        o.kind = OPND_SGPR;
        o.reg = (u16)field;
    } else {
        o.kind = OPND_SPECIAL;
        o.reg = (u16)field;
    }
    return o;
}

static RdnaOperand vgpr(u32 index, u8 count) {
    RdnaOperand o = {};
    o.kind = OPND_VGPR;
    o.reg = (u16)index;
    o.count = count;
    return o;
}

static RdnaOperand special(u16 code, u8 count) {
    RdnaOperand o = {};
    o.kind = OPND_SPECIAL;
    o.reg = code;
    o.count = count;
    return o;
}

static u32 bits(u32 v, u32 lo, u32 n) {
    return (v >> lo) & ((1u << n) - 1);
}

static s32 sign_extend(u32 v, u32 n) {
    u32 m = 1u << (n - 1);
    return (s32)((v ^ m) - m);
}

// VOP1/VOP2/VOPC src0 may name a DPP or SDWA dword that follows.
static void vop_extension(Reader& r, RdnaInst* in, u32 src0_field) {
    if (src0_field != SRC_DPP16 && src0_field != SRC_SDWA && src0_field != SRC_DPP8 &&
        src0_field != SRC_DPP8_FI)
        return;
    u32 at = r.at + 1;
    if (at >= r.count) {
        r.ok = false;
        return;
    }
    u32 x = r.code[at];
    r.extra = 2;
    RdnaOperand& s0 = in->src[0];
    if (src0_field == SRC_DPP16) {
        in->has_dpp = true;
        s0 = vgpr(bits(x, 0, 8), s0.count);
        in->dpp.ctrl = (u16)bits(x, 8, 9);
        in->dpp.fetch_inactive = bits(x, 18, 1);
        in->dpp.bound_ctrl = bits(x, 19, 1);
        s0.neg = bits(x, 20, 1);
        s0.abs = bits(x, 21, 1);
        in->src[1].neg = bits(x, 22, 1);
        in->src[1].abs = bits(x, 23, 1);
        in->dpp.bank_mask = (u8)bits(x, 24, 4);
        in->dpp.row_mask = (u8)bits(x, 28, 4);
    } else if (src0_field == SRC_DPP8 || src0_field == SRC_DPP8_FI) {
        in->has_dpp = true;
        in->dpp.dpp8 = true;
        in->dpp.fetch_inactive = src0_field == SRC_DPP8_FI;
        s0 = vgpr(bits(x, 0, 8), s0.count);
        for (u32 i = 0; i < 8; i++)
            in->dpp.sel8[i] = (u8)bits(x, 8 + 3 * i, 3);
    } else {
        in->has_sdwa = true;
        bool s0_sgpr = bits(x, 23, 1);
        s0 = s0_sgpr ? source(r, bits(x, 0, 8), s0.count)
                     : vgpr(bits(x, 0, 8), s0.count); // scalar: SGPR or constant
        if (in->enc == ENC_VOPC) {
            if (bits(x, 15, 1))
                in->dst[0] = sgpr(bits(x, 8, 7), 2);
        } else {
            in->sdwa.dst_sel = (u8)bits(x, 8, 3);
            in->sdwa.dst_unused = (u8)bits(x, 11, 2);
            in->clamp = bits(x, 13, 1);
            in->omod = (u8)bits(x, 14, 2);
        }
        in->sdwa.src_sel[0] = (u8)bits(x, 16, 3);
        in->sdwa.src_sext[0] = bits(x, 19, 1);
        s0.neg = bits(x, 20, 1);
        s0.abs = bits(x, 21, 1);
        in->sdwa.src_sel[1] = (u8)bits(x, 24, 3);
        in->sdwa.src_sext[1] = bits(x, 27, 1);
        in->src[1].neg = bits(x, 28, 1);
        in->src[1].abs = bits(x, 29, 1);
        if (bits(x, 31, 1) && in->src[1].kind == OPND_VGPR) {
            u16 reg = in->src[1].reg;
            in->src[1] = source(r, reg, in->src[1].count);
        }
    }
}

// --- encodings ---------------------------------------------------------------------------------------

static bool decode_scalar(Reader& r, u32 w, RdnaInst* in) {
    u32 top9 = w >> 23;
    if (top9 == 0x17F) { // SOPP
        in->enc = ENC_SOPP;
        in->op = g_sopp[bits(w, 16, 7)];
        in->simm16 = (s16)(w & 0xFFFF);
        return true;
    }
    if (top9 == 0x17E) { // SOPC
        in->enc = ENC_SOPC;
        in->op = g_sopc[bits(w, 16, 7)];
        if (!in->op)
            return false;
        Widths wd = op_widths(in->op);
        in->src[0] = source(r, bits(w, 0, 8), wd.src[0]);
        in->src[1] = source(r, bits(w, 8, 8), wd.src[1]);
        in->num_src = 2;
        return true;
    }
    if (top9 == 0x17D) { // SOP1
        in->enc = ENC_SOP1;
        in->op = g_sop1[bits(w, 8, 8)];
        if (!in->op)
            return false;
        Widths wd = op_widths(in->op);
        if (in->op != OP_S_SETPC_B64)
            in->dst[0] = sgpr(bits(w, 16, 7), wd.dst);
        if (in->op != OP_S_GETPC_B64) {
            in->src[0] = source(r, bits(w, 0, 8), wd.src[0]);
            in->num_src = 1;
        }
        return true;
    }
    if ((w >> 28) == 0xB) { // SOPK
        in->enc = ENC_SOPK;
        in->op = g_sopk[bits(w, 23, 5)];
        if (!in->op)
            return false;
        in->simm16 = (s16)(w & 0xFFFF);
        in->dst[0] = sgpr(bits(w, 16, 7), 1);
        if (in->op == OP_S_SETREG_IMM32_B32) {
            r.extra = 2;
            if (r.at + 1 >= r.count)
                return false;
            in->src[0].kind = OPND_LITERAL;
            in->src[0].count = 1;
            in->src[0].value = r.code[r.at + 1];
            in->num_src = 1;
        }
        if (in->op == OP_S_CALL_B64)
            in->dst[0].count = 2;
        return true;
    }
    // SOP2
    in->enc = ENC_SOP2;
    in->op = g_sop2[bits(w, 23, 7)];
    if (!in->op)
        return false;
    Widths wd = op_widths(in->op);
    in->dst[0] = sgpr(bits(w, 16, 7), wd.dst);
    in->src[0] = source(r, bits(w, 0, 8), wd.src[0]);
    in->src[1] = source(r, bits(w, 8, 8), wd.src[1]);
    in->num_src = 2;
    return true;
}

static void decode_smem(Reader& r, u32 w, u32 w1, RdnaInst* in) {
    in->enc = ENC_SMEM;
    in->op = g_smem[bits(w, 18, 8)];
    u8 dwords = 1;
    switch (in->op) {
    case OP_S_LOAD_DWORDX2:
    case OP_S_BUFFER_LOAD_DWORDX2:
    case OP_S_MEMTIME:
    case OP_S_MEMREALTIME:
        dwords = 2;
        break;
    case OP_S_LOAD_DWORDX4:
    case OP_S_BUFFER_LOAD_DWORDX4:
        dwords = 4;
        break;
    case OP_S_LOAD_DWORDX8:
    case OP_S_BUFFER_LOAD_DWORDX8:
        dwords = 8;
        break;
    case OP_S_LOAD_DWORDX16:
    case OP_S_BUFFER_LOAD_DWORDX16:
        dwords = 16;
        break;
    default:
        break;
    }
    bool buffer = in->op >= OP_S_BUFFER_LOAD_DWORD && in->op <= OP_S_BUFFER_LOAD_DWORDX16;
    if (in->op != OP_S_GL1_INV && in->op != OP_S_DCACHE_INV)
        in->dst[0] = sgpr(bits(w, 6, 7), dwords);
    in->src[0] = sgpr(bits(w, 0, 6) * 2, buffer ? 4 : 2); // base address or buffer descriptor
    in->src[1] = sgpr(bits(w1, 25, 7), 1);                // soffset (null when unused)
    in->num_src = 2;
    in->mem.dlc = bits(w, 14, 1);
    in->mem.glc = bits(w, 16, 1);
    in->mem.offset = sign_extend(bits(w1, 0, 21), 21);
}

// The src0 field holds the attribute (bits 0-5), its channel (6-7) and the half of each attribute
// dword the value is in (8). Sources are numbered as for VINTRP: src[0] is the I or J VGPR (the src1
// field), src[1] the src2 field (P0 values for v_interp_p1lv_f16, the first step's result for
// v_interp_p2_f16).
static void decode_vop3_interpolation(Reader& r, u32 w, u32 w1, RdnaInst* in) {
    u32 attribute = bits(w1, 0, 9);
    in->attr = (u8)(attribute & 63);
    in->attr_chan = (u8)((attribute >> 6) & 3);
    in->attr_high = (attribute >> 8) != 0;
    in->num_src = in->op == OP_V_INTERP_P1LL_F16 ? 1 : 2;
    for (u8 i = 0; i < in->num_src; i++) {
        in->src[i] = source(r, bits(w1, 9 + 9 * i, 9), 1);
        in->src[i].neg = bits(w1, 30 + i, 1);
        in->src[i].abs = bits(w, 9 + i, 1);
    }
    in->dst[0] = vgpr(bits(w, 0, 8), 1);
    in->omod = (u8)bits(w1, 27, 2);
    in->clamp = bits(w, 15, 1);
    in->op_sel = (u8)bits(w, 11, 4);
}

static void decode_vop3(Reader& r, u32 w, u32 w1, RdnaInst* in) {
    in->enc = ENC_VOP3;
    u32 opcode = bits(w, 16, 10);
    in->op = g_vop3[opcode];
    if (!in->op)
        return;
    if (is_vop3_interpolation(in->op)) {
        decode_vop3_interpolation(r, w, w1, in);
        return;
    }
    RdnaEncoding base = g_ops[in->op].enc;
    Widths wd = op_widths(in->op);
    u8 n = 3;
    if (base == ENC_VOP1)
        n = vop1_no_source(in->op) ? 0 : 1;
    else if (base == ENC_VOP2)
        n = vop2_sources(in->op);
    else if (base == ENC_VOPC)
        n = 2;
    else
        n = vop3_sources(in->op);
    u32 fields[3] = {bits(w1, 0, 9), bits(w1, 9, 9), bits(w1, 18, 9)};
    for (u8 i = 0; i < n; i++) {
        u8 count = (base == ENC_VOP2 && i == 2) ? 2 : wd.src[i]; // cndmask / carry-in: a lane mask
        in->src[i] = source(r, fields[i], count);
        in->src[i].neg = bits(w1, 29 + i, 1);
    }
    in->num_src = n;
    // A literal named by an unused source field still follows the instruction. Console compilers
    // put a code hash there in V_NOPs ("xbox data"); it is kept as src[0] for the disassembly.
    for (u8 i = n; i < 3; i++) {
        if (fields[i] == SRC_LITERAL) {
            RdnaOperand literal = source(r, SRC_LITERAL, 1);
            if (n == 0)
                in->src[0] = literal;
        }
    }
    in->omod = (u8)bits(w1, 27, 2);
    in->clamp = bits(w, 15, 1);
    if (base == ENC_VOPC) {
        in->dst[0] = sgpr(bits(w, 0, 8), 2);
    } else if (is_vop3b(in->op)) {
        in->dst[0] = vgpr(bits(w, 0, 8), wd.dst);
        in->dst[1] = sgpr(bits(w, 8, 7), 2);
    } else if (in->op == OP_V_READLANE_B32 || in->op == OP_V_READFIRSTLANE_B32) {
        in->dst[0] = sgpr(bits(w, 0, 8), 1);
    } else if (n > 0 || base != ENC_VOP1) {
        in->dst[0] = vgpr(bits(w, 0, 8), wd.dst);
    }
    if (!is_vop3b(in->op)) {
        for (u8 i = 0; i < n; i++)
            in->src[i].abs = bits(w, 8 + i, 1);
        in->op_sel = (u8)bits(w, 11, 4);
    }
}

static void decode_vop3p(Reader& r, u32 w, u32 w1, RdnaInst* in) {
    in->enc = ENC_VOP3P;
    in->op = g_vop3p[bits(w, 16, 7)];
    if (!in->op)
        return;
    u8 n = 3;
    switch (in->op) {
    case OP_V_PK_MAD_I16:
    case OP_V_PK_MAD_U16:
    case OP_V_PK_FMA_F16:
    case OP_V_DOT2_F32_F16:
    case OP_V_DOT2_I32_I16:
    case OP_V_DOT2_U32_U16:
    case OP_V_DOT4_I32_I8:
    case OP_V_DOT4_U32_U8:
    case OP_V_DOT8_I32_I4:
    case OP_V_DOT8_U32_U4:
    case OP_V_FMA_MIX_F32:
    case OP_V_FMA_MIXLO_F16:
    case OP_V_FMA_MIXHI_F16:
        n = 3;
        break;
    default:
        n = 2;
        break;
    }
    u32 fields[3] = {bits(w1, 0, 9), bits(w1, 9, 9), bits(w1, 18, 9)};
    for (u8 i = 0; i < n; i++) {
        in->src[i] = source(r, fields[i], 1);
        in->src[i].neg = bits(w1, 29 + i, 1);
    }
    in->num_src = n;
    in->dst[0] = vgpr(bits(w, 0, 8), 1);
    in->neg_hi = (u8)bits(w, 8, 3);
    in->op_sel = (u8)bits(w, 11, 3);
    in->op_sel_hi = (u8)(bits(w1, 27, 2) | (bits(w, 14, 1) << 2));
    in->clamp = bits(w, 15, 1);
}

static u8 popcount4(u32 v) {
    return (u8)((v & 1) + ((v >> 1) & 1) + ((v >> 2) & 1) + ((v >> 3) & 1));
}

static void decode_mimg(Reader& r, u32 w, u32 w1, RdnaInst* in) {
    in->enc = ENC_MIMG;
    u32 opcode = bits(w, 18, 7) | (bits(w, 0, 1) << 7);
    in->op = g_mimg[opcode];
    RdnaMemory& m = in->mem;
    u32 nsa = bits(w, 1, 2);
    m.dim = (u8)bits(w, 3, 3);
    m.dlc = bits(w, 7, 1);
    m.dmask = (u8)bits(w, 8, 4);
    m.unorm = bits(w, 12, 1);
    m.glc = bits(w, 13, 1);
    m.r128 = bits(w, 15, 1);
    m.tfe = bits(w, 16, 1);
    m.lwe = bits(w, 17, 1);
    m.slc = bits(w, 25, 1);
    m.a16 = bits(w1, 30, 1);
    m.d16 = bits(w1, 31, 1);
    u8 data = popcount4(m.dmask);
    if (in->op >= OP_IMAGE_GATHER4 && in->op <= OP_IMAGE_GATHER4_C_LZ_O)
        data = 4;
    if (data == 0)
        data = 1;
    if (m.d16)
        data = (u8)((data + 1) / 2);
    if (m.tfe || m.lwe)
        data++;
    in->dst[0] = vgpr(bits(w1, 8, 8), data); // vdata: loaded, or the data stored
    in->src[0] = vgpr(bits(w1, 0, 8), 1);    // first address VGPR
    in->src[1] = sgpr(bits(w1, 16, 5) * 4, m.r128 ? 4 : 8);
    in->src[2] = sgpr(bits(w1, 21, 5) * 4, 4);
    in->num_src = 3;
    m.nsa_count = 0;
    if (nsa) {
        r.extra = 2 + nsa;
        for (u32 i = 0; i < nsa * 4 && i < sizeof(m.nsa_addr); i++) {
            u32 at = r.at + 2 + i / 4;
            if (at >= r.count) {
                r.ok = false;
                return;
            }
            m.nsa_addr[i] = (u8)bits(r.code[at], 8 * (i % 4), 8);
        }
        m.nsa_count = (u8)(nsa * 4);
    }
}

static void decode_buffer(Reader& r, u32 w, u32 w1, RdnaInst* in, bool typed) {
    RdnaMemory& m = in->mem;
    m.offset = (s32)bits(w, 0, 12);
    m.offen = bits(w, 12, 1);
    m.idxen = bits(w, 13, 1);
    m.glc = bits(w, 14, 1);
    m.dlc = bits(w, 15, 1);
    m.slc = bits(w1, 22, 1);
    m.tfe = bits(w1, 23, 1);
    u8 data = 1;
    if (typed) {
        in->enc = ENC_MTBUF;
        in->op = g_mtbuf[bits(w, 16, 3) | (bits(w1, 21, 1) << 3)];
        m.format = (u8)bits(w, 19, 7);
        data = (u8)((in->op - OP_TBUFFER_LOAD_FORMAT_X) % 4 + 1);
        if (in->op >= OP_TBUFFER_LOAD_FORMAT_D16_X)
            data = (u8)((data + 1) / 2); // two 16-bit components per register
    } else {
        in->enc = ENC_MUBUF;
        in->op = g_mubuf[bits(w, 18, 7) | (bits(w, 25, 1) << 7)]; // bit 25 extends the opcode
        m.lds = bits(w, 16, 1);
        switch (in->op) {
        case OP_BUFFER_LOAD_FORMAT_D16_XYZ:
        case OP_BUFFER_LOAD_FORMAT_D16_XYZW:
        case OP_BUFFER_STORE_FORMAT_D16_XYZ:
        case OP_BUFFER_STORE_FORMAT_D16_XYZW:
            data = 2; // two 16-bit components per register
            break;
        case OP_BUFFER_LOAD_FORMAT_XY:
        case OP_BUFFER_STORE_FORMAT_XY:
        case OP_BUFFER_LOAD_DWORDX2:
        case OP_BUFFER_STORE_DWORDX2:
            data = 2;
            break;
        case OP_BUFFER_LOAD_FORMAT_XYZ:
        case OP_BUFFER_STORE_FORMAT_XYZ:
        case OP_BUFFER_LOAD_DWORDX3:
        case OP_BUFFER_STORE_DWORDX3:
            data = 3;
            break;
        case OP_BUFFER_LOAD_FORMAT_XYZW:
        case OP_BUFFER_STORE_FORMAT_XYZW:
        case OP_BUFFER_LOAD_DWORDX4:
        case OP_BUFFER_STORE_DWORDX4:
            data = 4;
            break;
        case OP_BUFFER_ATOMIC_CMPSWAP:
        case OP_BUFFER_ATOMIC_FCMPSWAP:
            data = 2;
            break;
        default:
            break;
        }
    }
    if (m.tfe)
        data++;
    in->dst[0] = vgpr(bits(w1, 8, 8), data);
    in->src[0] = vgpr(bits(w1, 0, 8), (m.offen && m.idxen) ? 2 : 1);
    in->src[1] = sgpr(bits(w1, 16, 5) * 4, 4);
    in->src[2] = source(r, bits(w1, 24, 8), 1); // soffset
    in->num_src = 3;
}

static void decode_ds(u32 w, u32 w1, RdnaInst* in) {
    in->enc = ENC_DS;
    in->op = g_ds[bits(w, 18, 8)];
    in->mem.offset = (s32)bits(w, 0, 8);
    in->mem.offset1 = (u8)bits(w, 8, 8);
    in->mem.gds = bits(w, 17, 1);
    u8 data = 1, ret = 1;
    switch (in->op) {
    case OP_DS_WRITE_B64:
    case OP_DS_WRITE2_B64:
    case OP_DS_WRITE2ST64_B64:
        data = 2;
        break;
    case OP_DS_WRITE_B96:
        data = 3;
        break;
    case OP_DS_WRITE_B128:
        data = 4;
        break;
    case OP_DS_READ2_B32:
    case OP_DS_READ2ST64_B32:
    case OP_DS_READ_B64:
    case OP_DS_WRXCHG2_RTN_B32:
    case OP_DS_WRXCHG2ST64_RTN_B32:
        ret = 2;
        break;
    case OP_DS_READ2_B64:
    case OP_DS_READ2ST64_B64:
    case OP_DS_READ_B128:
        ret = 4;
        break;
    case OP_DS_READ_B96:
        ret = 3;
        break;
    default:
        break;
    }
    in->dst[0] = vgpr(bits(w1, 24, 8), ret);
    in->src[0] = vgpr(bits(w1, 0, 8), 1);
    in->src[1] = vgpr(bits(w1, 8, 8), data);
    in->src[2] = vgpr(bits(w1, 16, 8), data);
    in->num_src = 3;
}

static void decode_flat(u32 w, u32 w1, RdnaInst* in) {
    in->mem.segment = (u8)bits(w, 14, 2);
    in->enc = in->mem.segment == 2 ? ENC_GLOBAL : ENC_FLAT;
    in->op = in->mem.segment == 2 ? g_global[bits(w, 18, 7)] : OP_INVALID;
    in->mem.offset = sign_extend(bits(w, 0, 12), 12);
    in->mem.dlc = bits(w, 12, 1);
    in->mem.lds = bits(w, 13, 1);
    in->mem.glc = bits(w, 16, 1);
    in->mem.slc = bits(w, 17, 1);
    u8 data = 1;
    switch (in->op) {
    case OP_GLOBAL_LOAD_DWORDX2:
    case OP_GLOBAL_STORE_DWORDX2:
    case OP_GLOBAL_ATOMIC_CMPSWAP:
        data = 2;
        break;
    case OP_GLOBAL_LOAD_DWORDX3:
    case OP_GLOBAL_STORE_DWORDX3:
        data = 3;
        break;
    case OP_GLOBAL_LOAD_DWORDX4:
    case OP_GLOBAL_STORE_DWORDX4:
        data = 4;
        break;
    default:
        break;
    }
    u32 saddr = bits(w1, 16, 7);
    bool has_saddr = saddr != 0x7D && saddr != 0x7F;
    in->dst[0] = vgpr(bits(w1, 24, 8), data);
    in->src[0] = vgpr(bits(w1, 0, 8), has_saddr ? 1 : 2); // 32-bit offset with saddr, else 64-bit address
    in->src[1] = vgpr(bits(w1, 8, 8), data);
    in->src[2] = has_saddr ? sgpr(saddr, 2) : special(SRC_NULL, 1);
    in->num_src = 3;
}

bool rdna_decode(const u32* code, u32 code_dwords, u32 offset, RdnaInst* in) {
    memset(in, 0, sizeof(*in));
    u32 at = offset / 4;
    if (at >= code_dwords)
        return false;
    Reader r = {code, code_dwords, at, 1, false, 0, true};
    u32 w = code[at];
    u32 w1 = at + 1 < code_dwords ? code[at + 1] : 0;
    in->offset = offset;
    bool ok = true;

    if ((w >> 30) == 2) { // all scalar ALU encodings start with binary 10
        ok = decode_scalar(r, w, in);
    } else if ((w >> 31) == 0) {
        u32 t7 = w >> 25;
        if (t7 == 0x3F) { // VOP1
            in->enc = ENC_VOP1;
            in->op = g_vop1[bits(w, 9, 8)];
            if (in->op) {
                Widths wd = op_widths(in->op);
                if (!vop1_no_source(in->op)) {
                    in->src[0] = source(r, bits(w, 0, 9), wd.src[0]);
                    in->num_src = 1;
                    in->dst[0] = in->op == OP_V_READFIRSTLANE_B32 ? sgpr(bits(w, 17, 8), 1)
                                                                  : vgpr(bits(w, 17, 8), wd.dst);
                }
                vop_extension(r, in, bits(w, 0, 9));
            }
        } else if (t7 == 0x3E) { // VOPC
            in->enc = ENC_VOPC;
            in->op = g_vopc[bits(w, 17, 8)];
            if (in->op) {
                Widths wd = op_widths(in->op);
                bool cmpx = name_has(in->op, "_cmpx_");
                in->dst[0] = special(cmpx ? SRC_EXEC_LO : SRC_VCC_LO, 2);
                in->src[0] = source(r, bits(w, 0, 9), wd.src[0]);
                in->src[1] = vgpr(bits(w, 9, 8), wd.src[1]);
                in->num_src = 2;
                vop_extension(r, in, bits(w, 0, 9));
            }
        } else { // VOP2
            in->enc = ENC_VOP2;
            in->op = g_vop2[bits(w, 25, 6)];
            if (in->op) {
                Widths wd = op_widths(in->op);
                in->dst[0] = vgpr(bits(w, 17, 8), wd.dst);
                in->src[0] = source(r, bits(w, 0, 9), wd.src[0]);
                in->src[1] = vgpr(bits(w, 9, 8), wd.src[1]);
                in->num_src = 2;
                switch (in->op) {
                case OP_V_MADMK_F32:
                case OP_V_FMAMK_F32:
                case OP_V_FMAMK_F16: { // dst = src0 * K + src1
                    RdnaOperand k = source(r, SRC_LITERAL, 1);
                    in->src[2] = in->src[1];
                    in->src[1] = k;
                    in->num_src = 3;
                    break;
                }
                case OP_V_MADAK_F32:
                case OP_V_FMAAK_F32:
                case OP_V_FMAAK_F16: // dst = src0 * src1 + K
                    in->src[2] = source(r, SRC_LITERAL, 1);
                    in->num_src = 3;
                    break;
                case OP_V_CNDMASK_B32:
                case OP_V_ADD_CO_CI_U32:
                case OP_V_SUB_CO_CI_U32:
                case OP_V_SUBREV_CO_CI_U32:
                    in->src[2] = special(SRC_VCC_LO, 2);
                    in->num_src = 3;
                    if (in->op != OP_V_CNDMASK_B32)
                        in->dst[1] = special(SRC_VCC_LO, 2);
                    break;
                default:
                    break;
                }
                vop_extension(r, in, bits(w, 0, 9));
            }
        }
    } else {
        u32 t6 = w >> 26;
        r.extra = 2;
        switch (t6) {
        case 0x3D:
            decode_smem(r, w, w1, in);
            break;
        case 0x35:
            decode_vop3(r, w, w1, in);
            break;
        case 0x33:
            decode_vop3p(r, w, w1, in);
            break;
        case 0x32: // VINTRP is a single dword
            r.extra = 1;
            in->enc = ENC_VINTRP;
            in->op = g_vintrp[bits(w, 16, 2)];
            in->dst[0] = vgpr(bits(w, 18, 8), 1);
            in->src[0] = vgpr(bits(w, 0, 8), 1);
            in->num_src = in->op == OP_V_INTERP_MOV_F32 ? 0 : 1;
            in->attr_chan = (u8)bits(w, 8, 2);
            in->attr = (u8)bits(w, 10, 6);
            if (in->op == OP_V_INTERP_MOV_F32)
                in->src[0].reg = (u16)bits(w, 0, 8); // 0 P10, 1 P20, 2 P0
            break;
        case 0x36:
            decode_ds(w, w1, in);
            break;
        case 0x38:
            decode_buffer(r, w, w1, in, false);
            break;
        case 0x3A:
            decode_buffer(r, w, w1, in, true);
            break;
        case 0x3C:
            decode_mimg(r, w, w1, in);
            break;
        case 0x3E: // EXP
            in->enc = ENC_EXP;
            in->op = OP_EXP;
            in->exp.enable = (u8)bits(w, 0, 4);
            in->exp.target = (u8)bits(w, 4, 6);
            in->exp.compressed = bits(w, 10, 1);
            in->exp.done = bits(w, 11, 1);
            in->exp.valid_mask = bits(w, 12, 1);
            for (u32 i = 0; i < 4; i++)
                in->src[i] = vgpr(bits(w1, 8 * i, 8), 1);
            in->num_src = 4;
            break;
        case 0x37:
            decode_flat(w, w1, in);
            break;
        default:
            ok = false;
            break;
        }
    }
    if (!ok || !r.ok || !in->op)
        return false;
    in->dwords = (u8)r.extra;
    if (at + in->dwords > code_dwords)
        return false;
    if (in->enc == ENC_SOPP) {
        switch (in->op) {
        case OP_S_BRANCH:
        case OP_S_CBRANCH_SCC0:
        case OP_S_CBRANCH_SCC1:
        case OP_S_CBRANCH_VCCZ:
        case OP_S_CBRANCH_VCCNZ:
        case OP_S_CBRANCH_EXECZ:
        case OP_S_CBRANCH_EXECNZ:
        case OP_S_CBRANCH_CDBGSYS:
        case OP_S_CBRANCH_CDBGUSER:
        case OP_S_CBRANCH_CDBGSYS_OR_USER:
        case OP_S_CBRANCH_CDBGSYS_AND_USER:
            in->branch_target = (u32)((s32)offset + 4 + in->simm16 * 4);
            break;
        default:
            break;
        }
    }
    return true;
}
