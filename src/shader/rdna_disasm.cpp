// RDNA 2 disassembly in LLVM-style syntax.

#include <stdio.h>
#include <string.h>

#include "shader/rdna.h"

struct Line {
    char* out;
    usize size;
    usize len;

    void put(const char* fmt, ...);
};

#include <stdarg.h>

void Line::put(const char* fmt, ...) {
    if (len >= size)
        return;
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(out + len, size - len, fmt, args);
    va_end(args);
    if (n > 0)
        len = len + (usize)n < size ? len + (usize)n : size - 1;
}

static const char* const k_inline_floats[] = {"0.5",  "-0.5", "1.0",  "-1.0",      "2.0",
                                              "-2.0", "4.0",  "-4.0", "0.15915494"};

static void put_register(Line& t, char file, u32 reg, u32 count) {
    if (count <= 1)
        t.put("%c%u", file, reg);
    else
        t.put("%c[%u:%u]", file, reg, reg + count - 1);
}

static void put_special(Line& t, u32 code, u32 count) {
    switch (code) {
    case SRC_VCC_LO:
        t.put(count == 2 ? "vcc" : "vcc_lo");
        return;
    case SRC_VCC_HI:
        t.put("vcc_hi");
        return;
    case SRC_M0:
        t.put("m0");
        return;
    case SRC_NULL:
        t.put("null");
        return;
    case SRC_EXEC_LO:
        t.put(count == 2 ? "exec" : "exec_lo");
        return;
    case SRC_EXEC_HI:
        t.put("exec_hi");
        return;
    case SRC_SHARED_BASE:
        t.put("src_shared_base");
        return;
    case SRC_SHARED_LIMIT:
        t.put("src_shared_limit");
        return;
    case SRC_PRIVATE_BASE:
        t.put("src_private_base");
        return;
    case SRC_PRIVATE_LIMIT:
        t.put("src_private_limit");
        return;
    case SRC_POPS_EXITING_WAVE_ID:
        t.put("src_pops_exiting_wave_id");
        return;
    case SRC_VCCZ:
        t.put("src_vccz");
        return;
    case SRC_EXECZ:
        t.put("src_execz");
        return;
    case SRC_SCC:
        t.put("src_scc");
        return;
    case SRC_LDS_DIRECT:
        t.put("src_lds_direct");
        return;
    default:
        t.put("special_%u", code);
        return;
    }
}

static void put_operand(Line& t, const RdnaOperand& o) {
    if (o.neg)
        t.put("-");
    if (o.abs)
        t.put("|");
    switch (o.kind) {
    case OPND_SGPR:
        if (o.reg >= SRC_TTMP0)
            put_register(t, 't', o.reg - SRC_TTMP0, o.count); // printed as t<n>: ttmp registers
        else
            put_register(t, 's', o.reg, o.count);
        break;
    case OPND_VGPR:
        put_register(t, 'v', o.reg, o.count);
        break;
    case OPND_INLINE:
        if (o.reg <= SRC_INT_MAX)
            t.put("%d", (int)o.reg - SRC_ZERO);
        else if (o.reg <= SRC_INT_NEG_MAX)
            t.put("%d", SRC_INT_MAX - (int)o.reg);
        else
            t.put("%s", k_inline_floats[o.reg - SRC_HALF]);
        break;
    case OPND_LITERAL:
        t.put("lit(0x%08x)", o.value);
        break;
    case OPND_SPECIAL:
        put_special(t, o.reg, o.count);
        break;
    default:
        t.put("?");
        break;
    }
    if (o.abs)
        t.put("|");
}

static void put_mnemonic(Line& t, const RdnaInst& in) {
    const char* name = rdna_op_name(in.op);
    if (in.enc == ENC_VOP3 && in.op == OP_V_NOP && in.src[0].kind == OPND_NONE)
        name = "v_nop";
    usize n = strlen(name);
    t.put("%s", name);
    usize pad = n + 2 > 14 ? 2 : 14 - n;
    t.put("%*s", (int)pad, "");
}

// Address VGPRs an image instruction reads, by operation and dimension (32-bit addresses).
u32 rdna_image_address_count(RdnaOp op, u8 dim) {
    static const u8 k_coords[8] = {1, 2, 3, 3, 2, 3, 3, 4};
    u32 coords = k_coords[dim & 7];
    if (op == OP_IMAGE_GET_RESINFO)
        return 1;
    // Node pointer (two registers for 64-bit), ray extent, origin, direction, inverse direction
    // (counted with 32-bit addresses).
    if (op == OP_IMAGE_BVH_INTERSECT_RAY)
        return 11;
    if (op == OP_IMAGE_BVH64_INTERSECT_RAY)
        return 12;
    if (op <= OP_IMAGE_ATOMIC_DEC || op == OP_IMAGE_MSAA_LOAD) {
        bool mip = op == OP_IMAGE_LOAD_MIP || op == OP_IMAGE_LOAD_MIP_PCK ||
                   op == OP_IMAGE_LOAD_MIP_PCK_SGN || op == OP_IMAGE_STORE_MIP ||
                   op == OP_IMAGE_STORE_MIP_PCK;
        return coords + (mip ? 1 : 0);
    }
    if (op == OP_IMAGE_GET_LOD)
        return coords;
    const char* name = rdna_op_name(op);
    u32 extra = 0;
    if (strstr(name, "_o"))
        extra += 1; // offsets
    if (strstr(name, "_b"))
        extra += 1; // bias
    if (strstr(name, "_c_") || (strlen(name) > 2 && strcmp(name + strlen(name) - 2, "_c") == 0))
        extra += 1; // depth compare value
    if (strstr(name, "_d"))
        extra += (dim == DIM_3D) ? 6 : (dim == DIM_1D || dim == DIM_1D_ARRAY) ? 2 : 4; // gradients
    if (strstr(name, "_l") && !strstr(name, "_lz"))
        extra += 1; // lod
    if (strstr(name, "_cl"))
        extra += 1; // lod clamp
    return coords + extra;
}

static const char* const k_dims[8] = {
    "SQ_RSRC_IMG_1D",       "SQ_RSRC_IMG_2D",       "SQ_RSRC_IMG_3D",      "SQ_RSRC_IMG_CUBE",
    "SQ_RSRC_IMG_1D_ARRAY", "SQ_RSRC_IMG_2D_ARRAY", "SQ_RSRC_IMG_2D_MSAA", "SQ_RSRC_IMG_2D_MSAA_ARRAY"};

static void put_sopp(Line& t, const RdnaInst& in) {
    switch (in.op) {
    case OP_S_ENDPGM:
    case OP_S_CODE_END:
    case OP_S_BARRIER:
    case OP_S_WAKEUP:
    case OP_S_ICACHE_INV:
    case OP_S_TTRACEDATA:
    case OP_S_ENDPGM_SAVED:
    case OP_S_ENDPGM_ORDERED_PS_DONE:
        return;
    case OP_S_WAITCNT: {
        u32 v = (u32)(u16)in.simm16;
        u32 vm = (v & 0xF) | ((v >> 10) & 0x30);
        u32 expcnt = (v >> 4) & 7;
        u32 lgkm = (v >> 8) & 0x3F;
        bool any = false;
        if (vm != 0x3F) {
            t.put("vmcnt(%u)", vm);
            any = true;
        }
        if (expcnt != 7) {
            t.put("%sexpcnt(%u)", any ? " " : "", expcnt);
            any = true;
        }
        if (lgkm != 0x3F)
            t.put("%slgkmcnt(%u)", any ? " " : "", lgkm);
        return;
    }
    default:
        break;
    }
    if (in.branch_target || in.op == OP_S_BRANCH ||
        (in.op >= OP_S_CBRANCH_SCC0 && in.op <= OP_S_CBRANCH_EXECNZ))
        t.put("label_%04X", in.branch_target);
    else
        t.put("0x%04x", (u32)(u16)in.simm16);
}

static void put_dpp(Line& t, const RdnaInst& in) {
    if (in.dpp.dpp8) {
        t.put(" dpp8:[%u,%u,%u,%u,%u,%u,%u,%u]", in.dpp.sel8[0], in.dpp.sel8[1], in.dpp.sel8[2],
              in.dpp.sel8[3], in.dpp.sel8[4], in.dpp.sel8[5], in.dpp.sel8[6], in.dpp.sel8[7]);
        return;
    }
    u32 c = in.dpp.ctrl;
    if (c <= 0xFF)
        t.put(" quad_perm:[%u,%u,%u,%u]", c & 3, (c >> 2) & 3, (c >> 4) & 3, (c >> 6) & 3);
    else if (c >= 0x101 && c <= 0x10F)
        t.put(" row_shl:%u", c - 0x100);
    else if (c >= 0x111 && c <= 0x11F)
        t.put(" row_shr:%u", c - 0x110);
    else if (c >= 0x121 && c <= 0x12F)
        t.put(" row_ror:%u", c - 0x120);
    else if (c == 0x140)
        t.put(" row_mirror");
    else if (c == 0x141)
        t.put(" row_half_mirror");
    else if (c >= 0x150 && c <= 0x15F)
        t.put(" row_share:%u", c - 0x150);
    else if (c >= 0x160 && c <= 0x16F)
        t.put(" row_xmask:%u", c - 0x160);
    else
        t.put(" dpp_ctrl:0x%x", c);
    t.put(" row_mask:0x%x bank_mask:0x%x", in.dpp.row_mask, in.dpp.bank_mask);
    if (in.dpp.bound_ctrl)
        t.put(" bound_ctrl:0");
    if (in.dpp.fetch_inactive)
        t.put(" fi:1");
}

static void put_sdwa(Line& t, const RdnaInst& in) {
    static const char* const k_sel[8] = {"BYTE_0", "BYTE_1", "BYTE_2", "BYTE_3",
                                         "WORD_0", "WORD_1", "DWORD",  "?"};
    if (in.enc != ENC_VOPC)
        t.put(" dst_sel:%s dst_unused:%u", k_sel[in.sdwa.dst_sel & 7], in.sdwa.dst_unused);
    t.put(" src0_sel:%s", k_sel[in.sdwa.src_sel[0] & 7]);
    if (in.num_src > 1)
        t.put(" src1_sel:%s", k_sel[in.sdwa.src_sel[1] & 7]);
}

static void put_export(Line& t, const RdnaInst& in) {
    u32 target = in.exp.target;
    if (target < 8)
        t.put("mrt%u", target);
    else if (target == EXP_MRTZ)
        t.put("mrtz");
    else if (target == EXP_NULL)
        t.put("null");
    else if (target >= EXP_POS0 && target < EXP_POS0 + 5)
        t.put("pos%u", target - EXP_POS0);
    else if (target == EXP_PRIM)
        t.put("prim");
    else if (target >= EXP_PARAM0)
        t.put("param%u", target - EXP_PARAM0);
    else
        t.put("target%u", target);
    for (u32 i = 0; i < 4; i++) {
        t.put(", ");
        // Compressed exports carry two 16-bit components per register: src0 for x/y, src1 for z/w.
        u32 source = in.exp.compressed ? i / 2 : i;
        if (in.exp.enable & (1u << i))
            put_operand(t, in.src[source]);
        else
            t.put("off");
    }
    if (in.exp.done)
        t.put(" done");
    if (in.exp.compressed)
        t.put(" compr");
    if (in.exp.valid_mask)
        t.put(" vm");
}

usize rdna_disassemble(const RdnaInst& in, char* out, usize size) {
    Line t = {out, size, 0};
    if (size)
        out[0] = 0;
    if (in.enc == ENC_VOP3 && in.op == OP_V_NOP && in.src[0].kind == OPND_LITERAL) {
        t.put("v_nop          lit(0x%08x)", in.src[0].value); // console code hash
        return t.len;
    }
    put_mnemonic(t, in);
    switch (in.enc) {
    case ENC_SOPP:
        put_sopp(t, in);
        break;
    case ENC_SOPK:
        if (in.op == OP_S_SETREG_IMM32_B32) {
            t.put("hwreg(0x%04x), lit(0x%08x)", (u32)(u16)in.simm16, in.src[0].value);
        } else if (in.op == OP_S_GETREG_B32 || in.op == OP_S_SETREG_B32) {
            put_operand(t, in.dst[0]);
            t.put(", hwreg(0x%04x)", (u32)(u16)in.simm16);
        } else if (in.op >= OP_S_WAITCNT_VSCNT && in.op <= OP_S_WAITCNT_LGKMCNT) {
            put_operand(t, in.dst[0]);
            t.put(", 0x%x", (u32)(u16)in.simm16);
        } else {
            put_operand(t, in.dst[0]);
            t.put(", 0x%04x", (u32)(u16)in.simm16);
        }
        break;
    case ENC_SMEM:
        if (in.dst[0].kind != OPND_NONE) {
            put_operand(t, in.dst[0]);
            t.put(", ");
        }
        put_operand(t, in.src[0]);
        t.put(", ");
        if (in.src[1].kind == OPND_SPECIAL && in.src[1].reg == SRC_NULL) {
            if (in.mem.offset)
                t.put("0x%06x", (u32)in.mem.offset & 0x1FFFFF);
            else
                t.put("null");
        } else {
            put_operand(t, in.src[1]);
            if (in.mem.offset)
                t.put(" offset:0x%06x", (u32)in.mem.offset & 0x1FFFFF);
        }
        if (in.mem.glc)
            t.put(" glc");
        if (in.mem.dlc)
            t.put(" dlc");
        break;
    case ENC_VINTRP:
        put_operand(t, in.dst[0]);
        if (in.op == OP_V_INTERP_MOV_F32) {
            static const char* const k_param[4] = {"p10", "p20", "p0", "?"};
            t.put(", %s", k_param[in.src[0].reg & 3]);
        } else {
            t.put(", ");
            put_operand(t, in.src[0]);
        }
        t.put(", attr%u.%c", in.attr, "xyzw"[in.attr_chan & 3]);
        break;
    case ENC_EXP:
        put_export(t, in);
        break;
    case ENC_MUBUF:
    case ENC_MTBUF:
        put_operand(t, in.dst[0]);
        t.put(", ");
        if (in.mem.offen || in.mem.idxen)
            put_operand(t, in.src[0]);
        else
            t.put("off");
        t.put(", ");
        put_operand(t, in.src[1]);
        t.put(", ");
        put_operand(t, in.src[2]);
        if (in.enc == ENC_MTBUF)
            t.put(" format:%u", in.mem.format);
        if (in.mem.idxen)
            t.put(" idxen");
        if (in.mem.offen)
            t.put(" offen");
        if (in.mem.offset)
            t.put(" offset:%d", in.mem.offset);
        if (in.mem.glc)
            t.put(" glc");
        if (in.mem.slc)
            t.put(" slc");
        if (in.mem.dlc)
            t.put(" dlc");
        if (in.mem.lds)
            t.put(" lds");
        if (in.mem.tfe)
            t.put(" tfe");
        break;
    case ENC_MIMG: {
        put_operand(t, in.dst[0]);
        t.put(", ");
        u32 addresses = rdna_image_address_count(in.op, in.mem.dim);
        if (in.mem.nsa_count) {
            t.put("[v%u", in.src[0].reg);
            for (u32 i = 0; i + 1 < addresses && i < in.mem.nsa_count; i++)
                t.put(", v%u", in.mem.nsa_addr[i]);
            t.put("]");
        } else {
            put_register(t, 'v', in.src[0].reg, addresses);
        }
        t.put(", ");
        put_operand(t, in.src[1]);
        bool sampler = (in.op >= OP_IMAGE_SAMPLE && in.op <= OP_IMAGE_GET_LOD);
        if (sampler) {
            t.put(", ");
            put_operand(t, in.src[2]);
        }
        t.put(" dmask:0x%x dim:%s", in.mem.dmask, k_dims[in.mem.dim & 7]);
        if (in.mem.unorm)
            t.put(" unorm");
        if (in.mem.glc)
            t.put(" glc");
        if (in.mem.slc)
            t.put(" slc");
        if (in.mem.dlc)
            t.put(" dlc");
        if (in.mem.r128)
            t.put(" r128");
        if (in.mem.a16)
            t.put(" a16");
        if (in.mem.d16)
            t.put(" d16");
        if (in.mem.tfe)
            t.put(" tfe");
        if (in.mem.lwe)
            t.put(" lwe");
        break;
    }
    case ENC_DS: {
        bool read = in.dst[0].kind == OPND_VGPR &&
                    (strstr(rdna_op_name(in.op), "read") || strstr(rdna_op_name(in.op), "rtn") ||
                     strstr(rdna_op_name(in.op), "permute") || in.op == OP_DS_SWIZZLE_B32 ||
                     in.op == OP_DS_APPEND || in.op == OP_DS_CONSUME);
        bool data = !strstr(rdna_op_name(in.op), "read") && in.op != OP_DS_SWIZZLE_B32 &&
                    in.op != OP_DS_APPEND && in.op != OP_DS_CONSUME;
        bool data2 = strstr(rdna_op_name(in.op), "2") != nullptr || in.op == OP_DS_MSKOR_B32 ||
                     in.op == OP_DS_CMPST_B32 || in.op == OP_DS_CMPST_RTN_B32;
        if (read) {
            put_operand(t, in.dst[0]);
            t.put(", ");
        }
        if (in.op != OP_DS_APPEND && in.op != OP_DS_CONSUME)
            put_operand(t, in.src[0]);
        if (data && in.op != OP_DS_NOP) {
            t.put(", ");
            put_operand(t, in.src[1]);
            if (data2 && !strstr(rdna_op_name(in.op), "read")) {
                t.put(", ");
                put_operand(t, in.src[2]);
            }
        }
        bool pair = strstr(rdna_op_name(in.op), "2_") || strstr(rdna_op_name(in.op), "2st64");
        if (pair) {
            if (in.mem.offset)
                t.put(" offset0:%d", in.mem.offset);
            if (in.mem.offset1)
                t.put(" offset1:%u", in.mem.offset1);
        } else {
            u32 offset = (u32)in.mem.offset | ((u32)in.mem.offset1 << 8);
            if (offset)
                t.put(" offset:%u", offset);
        }
        if (in.mem.gds)
            t.put(" gds");
        break;
    }
    case ENC_FLAT:
    case ENC_GLOBAL: {
        bool store = strstr(rdna_op_name(in.op), "store") != nullptr;
        if (!store) {
            put_operand(t, in.dst[0]);
            t.put(", ");
        }
        put_operand(t, in.src[0]);
        if (store || strstr(rdna_op_name(in.op), "atomic")) {
            t.put(", ");
            put_operand(t, in.src[1]);
        }
        t.put(", ");
        if (in.src[2].kind == OPND_SPECIAL && in.src[2].reg == SRC_NULL)
            t.put("off");
        else
            put_operand(t, in.src[2]);
        if (in.mem.offset)
            t.put(" offset:%d", in.mem.offset);
        if (in.mem.glc)
            t.put(" glc");
        if (in.mem.slc)
            t.put(" slc");
        if (in.mem.dlc)
            t.put(" dlc");
        break;
    }
    default: {
        // ALU: destinations then sources.
        bool first = true;
        for (u32 i = 0; i < 2; i++) {
            if (in.dst[i].kind == OPND_NONE)
                continue;
            // VOP2 carry-out to VCC and VOPC to VCC/EXEC are implicit in the 32-bit encodings.
            if (in.enc == ENC_VOP2 && i == 1)
                continue;
            if (in.enc == ENC_VOPC && in.dst[i].kind == OPND_SPECIAL && !in.has_sdwa)
                continue;
            t.put(first ? "" : ", ");
            put_operand(t, in.dst[i]);
            first = false;
        }
        bool interpolation =
            in.op == OP_V_INTERP_P1LL_F16 || in.op == OP_V_INTERP_P1LV_F16 || in.op == OP_V_INTERP_P2_F16;
        for (u32 i = 0; i < in.num_src; i++) {
            t.put(first ? "" : ", ");
            put_operand(t, in.src[i]);
            first = false;
            if (interpolation && i == 0)
                t.put(", attr%u.%c", in.attr, "xyzw"[in.attr_chan & 3]);
        }
        if (interpolation && in.attr_high)
            t.put(" high");
        if (in.has_dpp)
            put_dpp(t, in);
        if (in.has_sdwa)
            put_sdwa(t, in);
        if (in.op_sel && in.enc == ENC_VOP3)
            t.put(" op_sel:[%u,%u,%u,%u]", in.op_sel & 1, (in.op_sel >> 1) & 1, (in.op_sel >> 2) & 1,
                  (in.op_sel >> 3) & 1);
        if (in.enc == ENC_VOP3P) {
            if (in.op_sel)
                t.put(" op_sel:[%u,%u,%u]", in.op_sel & 1, (in.op_sel >> 1) & 1, (in.op_sel >> 2) & 1);
            if (in.op_sel_hi != 7)
                t.put(" op_sel_hi:[%u,%u,%u]", in.op_sel_hi & 1, (in.op_sel_hi >> 1) & 1,
                      (in.op_sel_hi >> 2) & 1);
            if (in.neg_hi)
                t.put(" neg_hi:[%u,%u,%u]", in.neg_hi & 1, (in.neg_hi >> 1) & 1, (in.neg_hi >> 2) & 1);
        }
        if (in.clamp)
            t.put(" clamp");
        static const char* const k_omod[4] = {"", " mul:2", " mul:4", " div:2"};
        t.put("%s", k_omod[in.omod & 3]);
        break;
    }
    }
    return t.len;
}
