// Console shader translation: program analysis and HLSL emission (see translate.h for the model).

#include "shader/translate.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "shader/rdna.h"
#include "shader/translate_internal.h"

// The wave size went into what used to be padding, so that dumps of older builds still load.
static_assert(offsetof(ShaderInfo, wave_size) == 3 && offsetof(ShaderInfo, parameters) == 4,
              "ShaderInfo layout");

// --- program structure -------------------------------------------------------------------------------

static bool is_branch(RdnaOp op) {
    switch (op) {
    case OP_S_BRANCH:
    case OP_S_CBRANCH_SCC0:
    case OP_S_CBRANCH_SCC1:
    case OP_S_CBRANCH_VCCZ:
    case OP_S_CBRANCH_VCCNZ:
    case OP_S_CBRANCH_EXECZ:
    case OP_S_CBRANCH_EXECNZ:
        return true;
    default:
        return false;
    }
}

static bool ends_program(RdnaOp op) {
    return op == OP_S_ENDPGM || op == OP_S_ENDPGM_SAVED || op == OP_S_ENDPGM_ORDERED_PS_DONE;
}

static s32 find_instruction(const Program& p, u32 offset) {
    u32 lo = 0, hi = p.count;
    while (lo < hi) {
        u32 mid = (lo + hi) / 2;
        if (p.insts[mid].offset < offset)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < p.count && p.insts[lo].offset == offset ? (s32)lo : -1;
}

static bool decode_program(const void* code, u32 size, Program& p, char* error, usize error_size) {
    const u32* words = (const u32*)code;
    u32 dwords = size / 4;
    p.capacity = dwords + 1;
    p.insts = (RdnaInst*)malloc(sizeof(RdnaInst) * p.capacity);
    if (!p.insts) {
        snprintf(error, error_size, "out of memory");
        return false;
    }
    p.count = 0;
    u32 offset = 0;
    u32 furthest_target = 0;
    while (offset / 4 < dwords) {
        RdnaInst& in = p.insts[p.count];
        if (!rdna_decode(words, dwords, offset, &in)) {
            snprintf(error, error_size, "undecodable instruction at 0x%x: %08x", offset, words[offset / 4]);
            return false;
        }
        if (in.op == OP_S_CODE_END)
            break;
        p.count++;
        offset += in.dwords * 4u;
        if (is_branch(in.op) && in.branch_target > furthest_target)
            furthest_target = in.branch_target;
        // The program ends at an S_ENDPGM no branch jumps past.
        if (ends_program(in.op) && offset > furthest_target)
            break;
    }
    return true;
}

static bool build_blocks(Program& p, char* error, usize error_size) {
    bool* leader = (bool*)calloc(p.count + 1, 1);
    if (!leader)
        return false;
    leader[0] = true;
    for (u32 i = 0; i < p.count; i++) {
        const RdnaInst& in = p.insts[i];
        if (is_branch(in.op)) {
            s32 target = find_instruction(p, in.branch_target);
            if (target < 0) {
                snprintf(error, error_size, "branch at 0x%x into the middle of an instruction", in.offset);
                free(leader);
                return false;
            }
            leader[target] = true;
            leader[i + 1] = true;
        } else if (ends_program(in.op)) {
            leader[i + 1] = true;
        }
    }
    p.block_count = 0;
    for (u32 i = 0; i < p.count; i++)
        if (leader[i])
            p.block_count++;
    p.blocks = (Block*)calloc(p.block_count, sizeof(Block));
    p.block_of = (u32*)malloc(sizeof(u32) * (p.count + 1));
    if (!p.blocks || !p.block_of) {
        free(leader);
        return false;
    }
    u32 b = 0;
    for (u32 i = 0; i < p.count; i++) {
        if (leader[i] && i > 0)
            b++;
        if (leader[i])
            p.blocks[b].first = i;
        p.blocks[b].end = i + 1;
        p.block_of[i] = b;
    }
    free(leader);
    for (u32 i = 0; i < p.block_count; i++) {
        Block& blk = p.blocks[i];
        const RdnaInst& last = p.insts[blk.end - 1];
        blk.next[0] = blk.next[1] = -1;
        if (ends_program(last.op))
            continue;
        if (is_branch(last.op)) {
            s32 target = (s32)p.block_of[find_instruction(p, last.branch_target)];
            blk.next[0] = target;
            if (last.op != OP_S_BRANCH)
                blk.next[1] = i + 1 < p.block_count ? (s32)(i + 1) : -1;
        } else {
            blk.next[0] = i + 1 < p.block_count ? (s32)(i + 1) : -1;
        }
    }
    return true;
}

// --- static register tags ----------------------------------------------------------------------------

static Tag tag_of(const TagState& s, const RdnaOperand& o, u32 part = 0) {
    if (o.kind == OPND_SGPR || (o.kind == OPND_SPECIAL && o.reg < 128))
        return s.sgpr[(o.reg + part) & 127];
    if (o.kind == OPND_VGPR)
        return s.vgpr[(o.reg + part) & 255];
    return TAG_NONE;
}

// Every scalar write goes through here, which also forgets the register's constant value.
static void set_tag(TagState& s, const RdnaOperand& o, u32 part, Tag t) {
    if (o.kind == OPND_SGPR || (o.kind == OPND_SPECIAL && o.reg < 128 && o.reg != SRC_NULL)) {
        u32 reg = (o.reg + part) & 127;
        s.sgpr[reg] = t;
        s.constant_known[reg / 64] &= ~(1ull << (reg % 64));
        s.unwritten[reg / 64] &= ~(1ull << (reg % 64));
        s.neutral[reg / 64] &= ~(1ull << (reg % 64));
    } else if (o.kind == OPND_VGPR) {
        u32 reg = (o.reg + part) & 255;
        s.vgpr[reg] = t;
        s.vgpr_const[reg] = false;
    }
}

// The value of an integer operand known at translation time: literals (zero-extended for 64-bit
// operations), inline integers (sign-extended), and SGPRs holding constants.
static bool constant_of(const TagState& s, const RdnaOperand& o, u32 part, u32* value) {
    switch (o.kind) {
    case OPND_LITERAL:
        *value = part ? 0 : o.value;
        return true;
    case OPND_INLINE:
        if (o.reg > SRC_INT_NEG_MAX)
            return false; // float constants
        {
            s32 v = o.reg <= SRC_INT_MAX ? (s32)o.reg - SRC_ZERO : SRC_INT_MAX - (s32)o.reg;
            *value = part ? (v < 0 ? 0xFFFFFFFFu : 0) : (u32)v;
        }
        return true;
    case OPND_SGPR:
        return sgpr_constant(s, o.reg + part, value);
    default:
        return false;
    }
}

// Constants are known in SGPRs and in the special scalar registers (VCC: a cleared mask, see
// merge_tag), except EXEC: compares and saveexec write it without naming it.
static void set_constant(TagState& s, const RdnaOperand& o, u32 part, u32 value) {
    if (o.kind != OPND_SGPR && !(o.kind == OPND_SPECIAL && o.reg < 128 && o.reg != SRC_NULL))
        return;
    u32 reg = (o.reg + part) & 127;
    if (reg == SRC_EXEC_LO || reg == SRC_EXEC_HI)
        return;
    s.constant[reg] = value;
    s.constant_known[reg / 64] |= 1ull << (reg % 64);
}

static u32 descriptor_of(const TagState& s, const RdnaOperand& o, u32 part = 0) {
    if (o.kind == OPND_SGPR)
        return s.descriptor[(o.reg + part) & 127];
    return NO_DESCRIPTOR;
}

static void set_descriptor(TagState& s, const RdnaOperand& o, u32 part, u32 index) {
    if (o.kind == OPND_SGPR || (o.kind == OPND_SPECIAL && o.reg < 128 && o.reg != SRC_NULL))
        s.descriptor[(o.reg + part) & 127] = (u16)index;
}

static bool is_exec_or_vcc(const RdnaOperand& o) {
    return o.kind == OPND_SPECIAL &&
           (o.reg == SRC_EXEC_LO || o.reg == SRC_EXEC_HI || o.reg == SRC_VCC_LO || o.reg == SRC_VCC_HI);
}

static bool is_exec(const RdnaOperand& o) {
    return o.kind == OPND_SPECIAL && (o.reg == SRC_EXEC_LO || o.reg == SRC_EXEC_HI);
}

// Registers that hold masks by what they are: EXEC; without exact lanes VCC too (with exact lanes, VCC
// is a mask only where compares or carries wrote it: programs also keep data in it).
static bool mask_register(const Translator& t, const RdnaOperand& o) {
    return t.exact ? is_exec(o) : is_exec_or_vcc(o);
}

static bool mask_source(const Translator& t, const TagState& s, const RdnaOperand& o) {
    return mask_register(t, o) || tag_kind(tag_of(s, o)) == TAG_MASK;
}

// A vector destination never holds a mask with exact lanes: masks moved into vector registers are their
// bits (see translate.h).
static Tag vector_tag(const Translator& t, Tag tag) {
    if (t.exact && tag_kind(tag) == TAG_MASK)
        return TAG_NONE;
    return tag;
}

// Address-like results keep their source's tag (heap offsets, root buffer high dwords).
static Tag carried(Tag a, Tag b) {
    if (a != TAG_NONE && b == TAG_NONE)
        return a;
    if (b != TAG_NONE && a == TAG_NONE)
        return b;
    return a == b ? a : TAG_NONE;
}

static const u32 k_const_f32[9] = {0x3f000000, 0xbf000000, 0x3f800000, 0xbf800000, 0x40000000,
                                   0xc0000000, 0x40800000, 0xc0800000, 0x3e22f983};
static const u32 k_const_f64_hi[9] = {0x3fe00000, 0xbfe00000, 0x3ff00000, 0xbff00000, 0x40000000,
                                      0xc0000000, 0x40100000, 0xc0100000, 0x3fc45f30};

// The value of an operand known at translation time: what constant_of knows, plus inline float
// constants, vector registers holding a constant in every lane that runs, and the null register.
static bool eval_const_u32(const TagState& s, const RdnaOperand& o, OperandType type, u32 part, u32* out) {
    if (constant_of(s, o, part, out))
        return true;
    switch (o.kind) {
    case OPND_VGPR:
        return vgpr_constant(s, o.reg + part, out);
    case OPND_INLINE:
        if (o.reg >= SRC_HALF && o.reg - SRC_HALF < 9) {
            u32 index = o.reg - SRC_HALF;
            if (type == TYPE_64)
                *out = part ? k_const_f64_hi[index] : (index == 8 ? 0x6dc9c882u : 0u);
            else
                *out = part ? 0u : k_const_f32[index];
            return true;
        }
        return false;
    case OPND_SPECIAL:
        if (o.reg == SRC_NULL) {
            *out = 0u;
            return true;
        }
        return o.reg < 128 && sgpr_constant(s, o.reg + part, out);
    default:
        return false;
    }
}

// Scalar integer results of constant operands, evaluated at translation time (programs build sampler
// descriptors and the like from constants with arithmetic, not only with moves).
static bool fold_scalar(const TagState& s, const RdnaInst& in, u32* out) {
    if (in.dst[0].kind != OPND_SGPR || in.dst[0].count != 1)
        return false;
    u32 a = 0, b = 0;
    if (in.num_src < 1 || !eval_const_u32(s, in.src[0], TYPE_32, 0, &a))
        return false;
    bool two = in.num_src > 1;
    if (two && !eval_const_u32(s, in.src[1], TYPE_32, 0, &b))
        return false;
    switch (in.op) {
    case OP_S_NOT_B32:
        *out = ~a;
        return true;
    case OP_S_BREV_B32: {
        u32 r = 0;
        for (u32 i = 0; i < 32; i++)
            r |= ((a >> i) & 1u) << (31 - i);
        *out = r;
        return true;
    }
    default:
        break;
    }
    if (!two)
        return false;
    switch (in.op) {
    case OP_S_ADD_U32:
    case OP_S_ADD_I32:
        *out = a + b;
        return true;
    case OP_S_SUB_U32:
    case OP_S_SUB_I32:
        *out = a - b;
        return true;
    case OP_S_MIN_U32:
        *out = a < b ? a : b;
        return true;
    case OP_S_MAX_U32:
        *out = a > b ? a : b;
        return true;
    case OP_S_MIN_I32:
        *out = (s32)a < (s32)b ? a : b;
        return true;
    case OP_S_MAX_I32:
        *out = (s32)a > (s32)b ? a : b;
        return true;
    case OP_S_AND_B32:
        *out = a & b;
        return true;
    case OP_S_OR_B32:
        *out = a | b;
        return true;
    case OP_S_XOR_B32:
        *out = a ^ b;
        return true;
    case OP_S_ANDN2_B32:
        *out = a & ~b;
        return true;
    case OP_S_ORN2_B32:
        *out = a | ~b;
        return true;
    case OP_S_NAND_B32:
        *out = ~(a & b);
        return true;
    case OP_S_NOR_B32:
        *out = ~(a | b);
        return true;
    case OP_S_XNOR_B32:
        *out = ~(a ^ b);
        return true;
    case OP_S_LSHL_B32:
        *out = a << (b & 31);
        return true;
    case OP_S_LSHR_B32:
        *out = a >> (b & 31);
        return true;
    case OP_S_ASHR_I32:
        *out = (u32)((s32)a >> (b & 31));
        return true;
    case OP_S_BFM_B32:
        *out = ((1u << (a & 31)) - 1u) << (b & 31);
        return true;
    case OP_S_MUL_I32:
        *out = a * b;
        return true;
    case OP_S_MUL_HI_U32:
        *out = (u32)(((u64)a * b) >> 32);
        return true;
    case OP_S_BFE_U32:
    case OP_S_BFE_I32: {
        u32 offset = b & 31, width = (b >> 16) & 127;
        if (width == 0) {
            *out = 0;
            return true;
        }
        u32 v = a >> offset;
        if (width < 32) {
            v &= (1u << width) - 1u;
            if (in.op == OP_S_BFE_I32 && (v >> (width - 1)) & 1u)
                v |= ~((1u << width) - 1u);
        }
        *out = v;
        return true;
    }
    case OP_S_LSHL1_ADD_U32:
    case OP_S_LSHL2_ADD_U32:
    case OP_S_LSHL3_ADD_U32:
    case OP_S_LSHL4_ADD_U32:
        *out = (a << (in.op - OP_S_LSHL1_ADD_U32 + 1)) + b;
        return true;
    case OP_S_PACK_LL_B32_B16:
        *out = (a & 0xFFFFu) | (b << 16);
        return true;
    case OP_S_PACK_LH_B32_B16:
        *out = (a & 0xFFFFu) | (b & 0xFFFF0000u);
        return true;
    case OP_S_PACK_HH_B32_B16:
        *out = (a >> 16) | (b & 0xFFFF0000u);
        return true;
    default:
        return false;
    }
}

static void set_any_constant(TagState& s, const RdnaOperand& o, u32 part, u32 value) {
    if (o.kind == OPND_SGPR) {
        set_constant(s, o, part, value);
    } else if (o.kind == OPND_VGPR) {
        // Lanes outside EXEC keep their old value: the value holds everywhere only if written while
        // all lanes ran, otherwise only while EXEC stays as it is.
        u32 r = (o.reg + part) & 255;
        s.vgpr_const[r] = true;
        s.vgpr_val[r] = value;
        s.vgpr_exec[r] = s.exec_full ? EXEC_ANY : s.exec_version;
    }
}

static bool writes_exec(const RdnaInst& in) {
    for (u32 d = 0; d < 2; d++) {
        const RdnaOperand& o = in.dst[d];
        if (o.kind == OPND_SPECIAL && (o.reg == SRC_EXEC_LO || o.reg == SRC_EXEC_HI))
            return true;
    }
    if ((in.op >= OP_S_AND_SAVEEXEC_B64 && in.op <= OP_S_XNOR_SAVEEXEC_B64) ||
        (in.op >= OP_S_ANDN1_SAVEEXEC_B64 && in.op <= OP_S_ANDN2_WREXEC_B64) ||
        (in.op >= OP_S_AND_SAVEEXEC_B32 && in.op <= OP_S_ANDN2_WREXEC_B32))
        return true;
    return rdna_op_encoding(in.op) == ENC_VOPC && strstr(rdna_op_name(in.op), "_cmpx_") != nullptr;
}

bool constant_lane(const Translator& t, const RdnaOperand& o, u32* lane) {
    u32 value;
    if (o.kind == OPND_LITERAL)
        value = o.value;
    else if (o.kind == OPND_INLINE && o.reg >= SRC_ZERO && o.reg <= SRC_INT_MAX)
        value = o.reg - SRC_ZERO;
    else
        return false;
    *lane = value & (t.wave32 ? 31u : 63u);
    return true;
}

static void clear_spill(const Translator& t, TagState& s, const RdnaOperand& o, u32 part) {
    if (o.kind != OPND_VGPR)
        return;
    u8 slot = t.spill_slot[(o.reg + part) & 255];
    if (slot == 0xFF)
        return;
    memset(s.spill_tag[slot], 0, sizeof(s.spill_tag[slot]));
    memset(s.spill_desc[slot], 0xFF, sizeof(s.spill_desc[slot]));
}

// Whether an instruction writes its destination: all do but the SOPK forms whose register field is only
// read (compares, hardware register and counter writes) or unused.
static bool writes_destination(const RdnaInst& in) {
    if (in.enc != ENC_SOPK)
        return true;
    switch (in.op) {
    case OP_S_MOVK_I32:
    case OP_S_CMOVK_I32:
    case OP_S_ADDK_I32:
    case OP_S_MULK_I32:
    case OP_S_GETREG_B32:
    case OP_S_CALL_B64:
        return true;
    default:
        return false;
    }
}

// Instructions that read their destination: conditional moves (which may keep it), bit sets, SOPK
// arithmetic and compares, hardware register and counter writes.
static bool reads_destination(const RdnaInst& in) {
    if (in.op >= OP_S_CMPK_EQ_I32 && in.op <= OP_S_CMPK_LE_U32)
        return true;
    if (in.op >= OP_S_WAITCNT_VSCNT && in.op <= OP_S_WAITCNT_LGKMCNT)
        return true;
    switch (in.op) {
    case OP_S_CMOV_B32:
    case OP_S_CMOV_B64:
    case OP_S_CMOVK_I32:
    case OP_S_ADDK_I32:
    case OP_S_MULK_I32:
    case OP_S_BITSET0_B32:
    case OP_S_BITSET0_B64:
    case OP_S_BITSET1_B32:
    case OP_S_BITSET1_B64:
    case OP_S_SETREG_B32:
        return true;
    default:
        return false;
    }
}

// Lane mask results: compares, carries (VOP2 and VOP3 second destinations) and saveexec.
static bool writes_lane_mask(const RdnaInst& in, u32 d) {
    RdnaEncoding base = rdna_op_encoding(in.op);
    return base == ENC_VOPC || (d == 1 && (in.enc == ENC_VOP3 || base == ENC_VOP2)) ||
           (in.op >= OP_S_AND_SAVEEXEC_B64 && in.op <= OP_S_XNOR_SAVEEXEC_B64) ||
           (in.op >= OP_S_ANDN1_SAVEEXEC_B64 && in.op <= OP_S_ANDN2_WREXEC_B64) ||
           (in.op >= OP_S_AND_SAVEEXEC_B32 && in.op <= OP_S_ANDN2_WREXEC_B32);
}

static void transfer(const Translator& t, TagState& s, const RdnaInst& in, u32 inst_idx);

// Exact lanes: a scalar operand dword that is all clear or all set, the same as data and as a mask.
static bool operand_neutral(const TagState& s, const RdnaOperand& o, u32 part) {
    u32 value;
    if (constant_of(s, o, part, &value))
        return value == 0 || value == 0xFFFFFFFFu;
    bool scalar = o.kind == OPND_SGPR || (o.kind == OPND_SPECIAL && o.reg < 128 && o.reg != SRC_NULL);
    return scalar && tag_kind(tag_of(s, o, part)) != TAG_MASK && sgpr_neutral(s, o.reg + part);
}

// Moves, selects and bitwise operations of such dwords make more of them (uniform booleans: s_cselect
// of -1 and 0 and the like).
static bool result_neutral(const TagState& s, const RdnaInst& in, u32 part) {
    bool forms = in.op == OP_S_MOV_B32 || in.op == OP_S_MOV_B64 || in.op == OP_S_CSELECT_B32 ||
                 in.op == OP_S_CSELECT_B64 || in.op == OP_S_NOT_B32 || in.op == OP_S_NOT_B64 ||
                 (in.op >= OP_S_AND_B32 && in.op <= OP_S_XNOR_B64);
    if (!forms || in.num_src == 0)
        return false;
    for (u32 i = 0; i < in.num_src; i++)
        if (!operand_neutral(s, in.src[i], part))
            return false;
    return true;
}

void tags_transfer(const Translator& t, TagState& s, const RdnaInst& in, u32 inst_idx) {
    // A three-XOR swap moves tags and constants as a whole; folding its steps one by one would undo it.
    bool swap = inst_idx != ~0u && t.xor_swap_role && t.xor_swap_role[inst_idx] != 0;
    u32 folded = 0;
    bool fold = !swap && fold_scalar(s, in, &folded);
    bool neutral[2] = {t.exact && !swap && result_neutral(s, in, 0),
                       t.exact && !swap && result_neutral(s, in, 1)};
    transfer(t, s, in, inst_idx);
    if (fold)
        set_constant(s, in.dst[0], 0, folded);
    // Exact lanes: EXEC is a mask whatever was written to it (data is converted on the way in), and
    // masks have no constant value (the broadcast form differs from lane to lane).
    if (t.exact) {
        for (u32 d = 0; d < 2; d++) {
            const RdnaOperand& o = in.dst[d];
            if (o.kind != OPND_SGPR && !(o.kind == OPND_SPECIAL && o.reg < 128 && o.reg != SRC_NULL))
                continue;
            for (u32 k = 0; k < (o.count ? o.count : 1u); k++) {
                u32 reg = (o.reg + k) & 127;
                if (reg == SRC_EXEC_LO || reg == SRC_EXEC_HI)
                    s.sgpr[reg] = make_tag(TAG_MASK, 0);
                if (tag_kind(s.sgpr[reg]) == TAG_MASK)
                    s.constant_known[reg / 64] &= ~(1ull << (reg % 64));
                else if (d == 0 && k < 2 && neutral[k])
                    s.neutral[reg / 64] |= 1ull << (reg % 64);
            }
        }
    }
    // A new EXEC: vector constants written from now on hold only in its lanes (see vgpr_constant).
    if (writes_exec(in)) {
        s.exec_full = false;
        s.exec_version = inst_idx != ~0u ? inst_idx + 1 : 0x7FFFFFFFu;
    }
}

static void transfer(const Translator& t, TagState& s, const RdnaInst& in, u32 inst_idx) {
    u32 value;
    if (inst_idx != ~0u && t.xor_swap_role) {
        u8 role = t.xor_swap_role[inst_idx];
        if (role == 1) {
            u32 a = in.dst[0].reg & 127, b = in.src[1].reg & 127;
            Tag st = s.sgpr[a];
            s.sgpr[a] = s.sgpr[b];
            s.sgpr[b] = st;
            u16 sd = s.descriptor[a];
            s.descriptor[a] = s.descriptor[b];
            s.descriptor[b] = sd;
            u32 sv = s.constant[a];
            s.constant[a] = s.constant[b];
            s.constant[b] = sv;
            s.unwritten[a / 64] &= ~(1ull << (a % 64));
            s.unwritten[b / 64] &= ~(1ull << (b % 64));
            u64 ka = (s.constant_known[a / 64] >> (a % 64)) & 1ull;
            u64 kb = (s.constant_known[b / 64] >> (b % 64)) & 1ull;
            s.constant_known[a / 64] = (s.constant_known[a / 64] & ~(1ull << (a % 64))) | (kb << (a % 64));
            s.constant_known[b / 64] = (s.constant_known[b / 64] & ~(1ull << (b % 64))) | (ka << (b % 64));
            u64 na = (s.neutral[a / 64] >> (a % 64)) & 1ull;
            u64 nb = (s.neutral[b / 64] >> (b % 64)) & 1ull;
            s.neutral[a / 64] = (s.neutral[a / 64] & ~(1ull << (a % 64))) | (nb << (a % 64));
            s.neutral[b / 64] = (s.neutral[b / 64] & ~(1ull << (b % 64))) | (na << (b % 64));
            return;
        }
        if (role == 2)
            return;
    }
    // A register only read keeps what is known about it.
    if (!writes_destination(in))
        return;
    switch (in.op) {
    case OP_S_MOV_B32:
    case OP_S_CMOV_B32:
        // (With exact lanes, the emitter refuses conditional moves between masks and data.)
        set_tag(s, in.dst[0], 0, tag_of(s, in.src[0]));
        set_descriptor(s, in.dst[0], 0, descriptor_of(s, in.src[0]));
        if (mask_register(t, in.src[0]))
            set_tag(s, in.dst[0], 0, make_tag(TAG_MASK, 0));
        if (in.op == OP_S_MOV_B32 && constant_of(s, in.src[0], 0, &value))
            set_constant(s, in.dst[0], 0, value);
        return;
    case OP_V_MOV_B32:
    case OP_V_READFIRSTLANE_B32: {
        bool raw = !in.has_sdwa && !in.has_dpp && !in.src[0].abs && !in.src[0].neg && !in.omod && !in.clamp;
        u32 cv = 0;
        clear_spill(t, s, in.dst[0], 0);
        set_tag(s, in.dst[0], 0, raw ? vector_tag(t, tag_of(s, in.src[0])) : TAG_NONE);
        set_descriptor(s, in.dst[0], 0, raw ? descriptor_of(s, in.src[0]) : NO_DESCRIPTOR);
        if (!t.exact && is_exec_or_vcc(in.src[0]))
            set_tag(s, in.dst[0], 0, make_tag(TAG_MASK, 0));
        if (raw && eval_const_u32(s, in.src[0], TYPE_32, 0, &cv))
            set_any_constant(s, in.dst[0], 0, cv);
        return;
    }
    case OP_V_SWAP_B32:
        // Both registers change; under a partial EXEC neither holds one value, so forget both.
        clear_spill(t, s, in.dst[0], 0);
        clear_spill(t, s, in.src[0], 0);
        set_tag(s, in.dst[0], 0, TAG_NONE);
        set_tag(s, in.src[0], 0, TAG_NONE);
        return;
    case OP_S_MOVK_I32:
        set_tag(s, in.dst[0], 0, TAG_NONE);
        set_descriptor(s, in.dst[0], 0, NO_DESCRIPTOR);
        set_constant(s, in.dst[0], 0, (u32)(s32)in.simm16);
        return;
    case OP_S_ADDK_I32: {
        // The sum keeps the register's tag (an offset stays one), but is data, not a mask.
        Tag tag = tag_of(s, in.dst[0]);
        u32 base = 0;
        bool known = in.dst[0].kind == OPND_SGPR && sgpr_constant(s, in.dst[0].reg, &base);
        set_tag(s, in.dst[0], 0, tag_kind(tag) == TAG_MASK ? TAG_NONE : tag);
        set_descriptor(s, in.dst[0], 0, NO_DESCRIPTOR);
        if (known)
            set_constant(s, in.dst[0], 0, base + (u32)(s32)in.simm16);
        return;
    }
    case OP_S_SWAPPC_B64: // the fetch subroutine: the return address, and the inputs it loads
        for (u32 i = 0; i < 2; i++) {
            set_tag(s, in.dst[0], i, TAG_NONE);
            set_descriptor(s, in.dst[0], i, NO_DESCRIPTOR);
        }
        set_constant(s, in.dst[0], 0, in.offset + 4);
        set_constant(s, in.dst[0], 1, 0u);
        for (u32 i = 0; i < t.info->element_count; i++)
            for (u32 c = 0; c < t.info->fetch_components[i]; c++) {
                u32 r = (t.info->fetch_vgpr[i] + c) & 255;
                s.vgpr[r] = TAG_NONE;
                s.vgpr_const[r] = false;
            }
        return;
    case OP_S_GETPC_B64:
        set_tag(s, in.dst[0], 0, make_tag(TAG_PC, 0));
        set_tag(s, in.dst[0], 1, TAG_NONE);
        set_descriptor(s, in.dst[0], 0, NO_DESCRIPTOR);
        set_descriptor(s, in.dst[0], 1, NO_DESCRIPTOR);
        set_constant(s, in.dst[0], 0, in.offset + 4);
        set_constant(s, in.dst[0], 1, 0u);
        return;
    case OP_V_READLANE_B32: {
        Tag rt = TAG_NONE;
        u32 rd = NO_DESCRIPTOR;
        u32 lane = 0;
        if (in.src[0].kind == OPND_VGPR) {
            u8 slot = t.spill_slot[in.src[0].reg & 255];
            if (slot != 0xFF && constant_lane(t, in.src[1], &lane)) {
                rt = s.spill_tag[slot][lane];
                rd = s.spill_desc[slot][lane];
            } else {
                rt = tag_of(s, in.src[0]);
            }
        }
        set_tag(s, in.dst[0], 0, rt);
        set_descriptor(s, in.dst[0], 0, rd);
        return;
    }
    case OP_V_WRITELANE_B32: {
        if (in.dst[0].kind == OPND_VGPR) {
            u32 r = in.dst[0].reg & 255;
            u8 slot = t.spill_slot[r];
            u32 lane = 0;
            if (slot != 0xFF && constant_lane(t, in.src[1], &lane)) {
                s.spill_tag[slot][lane] = tag_of(s, in.src[0]);
                s.spill_desc[slot][lane] = (u16)descriptor_of(s, in.src[0]);
            } else {
                clear_spill(t, s, in.dst[0], 0);
            }
            s.vgpr[r] = TAG_NONE;
            s.vgpr_const[r] = false;
        }
        return;
    }
    case OP_S_MOV_B64:
    case OP_S_CMOV_B64: {
        u32 values[2];
        bool constant = in.op == OP_S_MOV_B64 && constant_of(s, in.src[0], 0, &values[0]) &&
                        constant_of(s, in.src[0], 1, &values[1]);
        for (u32 i = 0; i < 2; i++) {
            Tag moved = t.exact                        ? tag_of(s, in.src[0], i)
                        : mask_source(t, s, in.src[0]) ? make_tag(TAG_MASK, 0)
                                                       : tag_of(s, in.src[0], i);
            set_tag(s, in.dst[0], i, moved);
            set_descriptor(s, in.dst[0], i, descriptor_of(s, in.src[0], i));
            if (constant)
                set_constant(s, in.dst[0], i, values[i]);
        }
        return;
    }
    case OP_S_BFM_B64: {
        u32 a = 0, b = 0;
        bool ok =
            eval_const_u32(s, in.src[0], TYPE_32, 0, &a) && eval_const_u32(s, in.src[1], TYPE_32, 0, &b);
        u64 v = ok ? ((((u64)1 << (a & 63u)) - 1ull) << (b & 63u)) : 0ull;
        for (u32 i = 0; i < 2; i++) {
            set_tag(s, in.dst[0], i, TAG_NONE);
            set_descriptor(s, in.dst[0], i, NO_DESCRIPTOR);
            if (ok)
                set_constant(s, in.dst[0], i, (u32)(v >> (32 * i)));
        }
        return;
    }
    case OP_S_ADD_U32:
    case OP_S_ADD_I32:
    case OP_S_OR_B32:
    case OP_S_AND_B32:
    case OP_S_BFE_U32:
    case OP_S_LSHL1_ADD_U32:
    case OP_S_LSHL2_ADD_U32:
    case OP_S_LSHL3_ADD_U32:
    case OP_S_LSHL4_ADD_U32:
    case OP_S_ADDC_U32:
    case OP_S_SUB_U32:
    case OP_S_SUB_I32:
    case OP_V_ADD_NC_U32:
    case OP_V_OR_B32:
    case OP_V_LSHL_ADD_U32:
    case OP_V_ADD3_U32: {
        clear_spill(t, s, in.dst[0], 0);
        Tag a = tag_of(s, in.src[0]), b = tag_of(s, in.src[1]);
        if (tag_kind(a) == TAG_MASK || tag_kind(b) == TAG_MASK) {
            set_tag(s, in.dst[0], 0,
                    in.op == OP_S_AND_B32 || in.op == OP_S_OR_B32 ? make_tag(TAG_MASK, 0) : TAG_NONE);
            return;
        }
        set_tag(s, in.dst[0], 0, carried(a, b));
        set_descriptor(s, in.dst[0], 0, NO_DESCRIPTOR);
        // Scalar results are folded in tags_transfer; vector ones here (texel offsets are built so).
        u32 va = 0, vb = 0;
        bool vector = in.op == OP_V_ADD_NC_U32 || in.op == OP_V_OR_B32;
        if (vector && !in.has_sdwa && !in.has_dpp && !in.clamp &&
            eval_const_u32(s, in.src[0], TYPE_32, 0, &va) && eval_const_u32(s, in.src[1], TYPE_32, 0, &vb))
            set_any_constant(s, in.dst[0], 0, in.op == OP_V_ADD_NC_U32 ? va + vb : va | vb);
        return;
    }
    case OP_S_LSHL_B32:
    case OP_S_LSHR_B32:
    case OP_S_PACK_LL_B32_B16:
    case OP_S_PACK_LH_B32_B16:
    case OP_S_PACK_HH_B32_B16:
        set_tag(s, in.dst[0], 0, TAG_NONE);
        set_descriptor(s, in.dst[0], 0, NO_DESCRIPTOR);
        return;
    case OP_S_AND_B64:
    case OP_S_OR_B64:
    case OP_S_XOR_B64:
    case OP_S_ANDN2_B64:
    case OP_S_ORN2_B64:
    case OP_S_NAND_B64:
    case OP_S_NOR_B64:
    case OP_S_XNOR_B64:
    case OP_S_NOT_B64:
    case OP_S_WQM_B64:
    case OP_S_CSELECT_B64:
    case OP_S_BITREPLICATE_B64_B32:
    case OP_S_ANDN2_B32:
    case OP_S_ORN2_B32:
    case OP_S_XOR_B32:
    case OP_S_NAND_B32:
    case OP_S_NOR_B32:
    case OP_S_XNOR_B32:
    case OP_S_NOT_B32:
    case OP_S_CSELECT_B32:
    case OP_S_WQM_B32: {
        // With exact lanes, bit replication (of wave32 masks into wave64 ones) works on data: a mask
        // source is read as its bits, and the result is bits.
        bool sources = mask_source(t, s, in.src[0]) || (in.num_src > 1 && mask_source(t, s, in.src[1]));
        bool mask = mask_register(t, in.dst[0]) || in.op == OP_S_WQM_B64 || in.op == OP_S_WQM_B32 ||
                    (in.op == OP_S_BITREPLICATE_B64_B32 ? !t.exact : sources);
        u32 parts = in.dst[0].count;
        for (u32 i = 0; i < parts; i++) {
            set_tag(s, in.dst[0], i, mask ? make_tag(TAG_MASK, 0) : TAG_NONE);
            set_descriptor(s, in.dst[0], i, NO_DESCRIPTOR);
        }
        return;
    }
    case OP_S_LOAD_DWORD:
    case OP_S_LOAD_DWORDX2:
    case OP_S_LOAD_DWORDX4:
    case OP_S_LOAD_DWORDX8:
    case OP_S_LOAD_DWORDX16: {
        Tag base = tag_of(s, in.src[0], 0);
        Tag hi = tag_of(s, in.src[0], 1);
        Tag result = TAG_NONE;
        u32 first = NO_DESCRIPTOR, slot = 32;
        if (tag_kind(base) == TAG_HEAP) {
            result = base; // descriptors from the heap: each register holds its own byte address
            // At a known place when loaded from the table's start with a constant offset.
            slot = (tag_payload(base) & 1) == HEAP_SAMPLERS ? 16 : 32;
            u32 at = descriptor_of(s, in.src[0]);
            bool constant = in.src[1].kind == OPND_SPECIAL && in.src[1].reg == SRC_NULL;
            if (at != NO_DESCRIPTOR && constant && in.mem.offset >= 0)
                first = at * slot + (u32)in.mem.offset;
        } else if (tag_kind(base) == TAG_VB_TABLE && in.src[1].kind == OPND_SPECIAL &&
                   in.src[1].reg == SRC_NULL && in.mem.offset % 16 == 0)
            result = make_tag(TAG_VB_DESC, (u32)in.mem.offset / 16);
        (void)hi;
        for (u32 i = 0; i < in.dst[0].count; i++) {
            set_tag(s, in.dst[0], i, result);
            set_descriptor(s, in.dst[0], i, first == NO_DESCRIPTOR ? NO_DESCRIPTOR : (first + 4 * i) / slot);
        }
        return;
    }
    default:
        break;
    }
    // Compares and saveexec write masks; everything else writes plain values.
    for (u32 d = 0; d < 2; d++) {
        const RdnaOperand& dst = in.dst[d];
        if (dst.kind == OPND_NONE)
            continue;
        bool mask = writes_lane_mask(in, d);
        u32 count = (mask && t.wave32) ? 1 : dst.count;
        for (u32 i = 0; i < count; i++) {
            clear_spill(t, s, dst, i);
            set_tag(s, dst, i, mask ? make_tag(TAG_MASK, 0) : TAG_NONE);
            set_descriptor(s, dst, i, NO_DESCRIPTOR);
        }
    }
}

// A scalar register's tag where paths join: what both agree on, else nothing known. With exact lanes,
// a mask on one path and data on another join as data: the paths bringing the mask convert it to its
// bits on the way (edge_converts; conversions either way lose nothing in whole desktop waves of 32
// lanes, and data is what other uses read registers as). Except where the data path has all bits
// clear or all set (`neutral`), which is the same value in both forms: then the join is a mask.
static Tag merge_tag(const Translator& t, Tag into, Tag from, bool into_neutral, bool from_neutral) {
    if (into == from)
        return into;
    if (t.exact) {
        bool into_mask = tag_kind(into) == TAG_MASK, from_mask = tag_kind(from) == TAG_MASK;
        if (into_mask && !from_mask && from_neutral)
            return into;
        if (from_mask && !into_mask && into_neutral)
            return from;
    }
    return TAG_NONE;
}

bool edge_converts(const Translator& t, Tag out, Tag entry) {
    return t.exact && tag_kind(out) == TAG_MASK && tag_kind(entry) != TAG_MASK;
}

// Registers with all bits clear or all set, or none written yet (0 in the translation).
bool sgpr_neutral(const TagState& s, u32 reg) {
    u32 value;
    reg &= 127;
    if ((s.unwritten[reg / 64] | s.neutral[reg / 64]) & (1ull << (reg % 64)))
        return true;
    return sgpr_constant(s, reg, &value) && (value == 0 || value == 0xFFFFFFFFu);
}

static bool tags_merge(const Translator& t, TagState& into, const TagState& from, u32 block) {
    bool changed = false;
    // EXEC: still the starting one only if on both paths; a version of its own where they differ.
    if (into.exec_full && !from.exec_full) {
        into.exec_full = false;
        changed = true;
    }
    u32 joined = 0x80000000u | block;
    if (into.exec_version != from.exec_version && into.exec_version != joined) {
        into.exec_version = joined;
        changed = true;
    }
    for (u32 i = 0; i < 128; i++) {
        bool into_neutral = sgpr_neutral(into, i), from_neutral = sgpr_neutral(from, i);
        Tag joined_tag = merge_tag(t, into.sgpr[i], from.sgpr[i], into_neutral, from_neutral);
        if (joined_tag != into.sgpr[i]) {
            into.sgpr[i] = joined_tag;
            changed = true;
        }
        // Neutral on both paths (if different constants) stays neutral.
        u64 bit = 1ull << (i % 64);
        bool neutral = into_neutral && from_neutral && tag_kind(joined_tag) != TAG_MASK;
        if (((into.neutral[i / 64] & bit) != 0) != neutral) {
            into.neutral[i / 64] ^= bit;
            changed = true;
        }
    }
    for (u32 i = 0; i < 256; i++)
        if (into.vgpr[i] != from.vgpr[i] && into.vgpr[i] != TAG_NONE) {
            into.vgpr[i] = TAG_NONE;
            changed = true;
        }
    for (u32 i = 0; i < 128; i++)
        if (into.descriptor[i] != from.descriptor[i] && into.descriptor[i] != NO_DESCRIPTOR) {
            into.descriptor[i] = NO_DESCRIPTOR;
            changed = true;
        }
    for (u32 w = 0; w < 2; w++) {
        u64 unwritten = into.unwritten[w] & from.unwritten[w];
        if (unwritten != into.unwritten[w]) {
            into.unwritten[w] = unwritten;
            changed = true;
        }
    }
    // Constants survive a join only where both paths agree.
    for (u32 w = 0; w < 2; w++) {
        u64 keep = into.constant_known[w] & from.constant_known[w];
        for (u32 b = 0; b < 64; b++)
            if ((keep & (1ull << b)) && into.constant[w * 64 + b] != from.constant[w * 64 + b])
                keep &= ~(1ull << b);
        if (keep != into.constant_known[w]) {
            into.constant_known[w] = keep;
            changed = true;
        }
    }
    for (u32 i = 0; i < 256; i++)
        if (into.vgpr_const[i] && (!from.vgpr_const[i] || into.vgpr_val[i] != from.vgpr_val[i] ||
                                   into.vgpr_exec[i] != from.vgpr_exec[i])) {
            into.vgpr_const[i] = false;
            changed = true;
        }
    for (u32 slot = 0; slot < t.spill_vgpr_count; slot++)
        for (u32 lane = 0; lane < 64; lane++) {
            Tag joined_tag =
                merge_tag(t, into.spill_tag[slot][lane], from.spill_tag[slot][lane], false, false);
            if (joined_tag != into.spill_tag[slot][lane]) {
                into.spill_tag[slot][lane] = joined_tag;
                changed = true;
            }
            if (into.spill_desc[slot][lane] != from.spill_desc[slot][lane] &&
                into.spill_desc[slot][lane] != NO_DESCRIPTOR) {
                into.spill_desc[slot][lane] = NO_DESCRIPTOR;
                changed = true;
            }
        }
    return changed;
}

static void tags_entry(const Translator& t, const ShaderInfo& info, TagState& s) {
    memset(&s, 0, sizeof(s));
    memset(s.descriptor, 0xFF, sizeof(s.descriptor));
    memset(s.spill_desc, 0xFF, sizeof(s.spill_desc));
    s.exec_full = true;
    for (u32 i = 0; i < info.parameter_count; i++) {
        const UserDataParameter& p = info.parameters[i];
        switch (p.kind) {
        case USER_DATA_RESOURCE_TABLE:
            s.sgpr[p.sgpr] = make_tag(TAG_HEAP, HEAP_RESOURCES | (i << 1));
            s.descriptor[p.sgpr] = 0;
            break;
        case USER_DATA_SAMPLER_TABLE:
            s.sgpr[p.sgpr] = make_tag(TAG_HEAP, HEAP_SAMPLERS | (i << 1));
            s.descriptor[p.sgpr] = 0;
            break;
        case USER_DATA_ROOT_CBV:
        case USER_DATA_ROOT_SRV:
        case USER_DATA_ROOT_UAV:
            s.sgpr[p.sgpr + 1] = make_tag(TAG_ROOT_HI, i);
            break;
        default:
            break;
        }
    }
    // Heap bases: parameter 0x3F stands for no table (the place in the heap is unknown).
    if (info.resource_heap_sgpr != NO_HEAP_SGPR)
        s.sgpr[info.resource_heap_sgpr & 127] = make_tag(TAG_HEAP, HEAP_RESOURCES | (0x3F << 1));
    if (info.sampler_heap_sgpr != NO_HEAP_SGPR)
        s.sgpr[info.sampler_heap_sgpr & 127] = make_tag(TAG_HEAP, HEAP_SAMPLERS | (0x3F << 1));
    if (info.stage == SHADER_VERTEX) {
        s.sgpr[0] = make_tag(TAG_VB_TABLE, 0);
        s.sgpr[1] = make_tag(TAG_VB_TABLE, 1);
    }
    s.sgpr[SRC_EXEC_LO] = s.sgpr[SRC_EXEC_HI] = make_tag(TAG_MASK, 0);
    if (!t.exact)
        s.sgpr[SRC_VCC_LO] = s.sgpr[SRC_VCC_HI] = make_tag(TAG_MASK, 0);
    // Exact lanes: the registers the program starts without a value in are 0 in the translation, the
    // same as a mask or as data (see merge_tag). Code joins paths that wrote a mask with paths that left
    // the register alone (and masks it with an empty EXEC there).
    if (t.exact) {
        u32 inputs = info.user_sgprs;
        for (u32 axis = 0; axis < 3; axis++)
            inputs += (info.group_id_mask >> axis) & 1u;
        for (u32 r = inputs; r < SRC_EXEC_LO; r++)
            s.unwritten[r / 64] |= 1ull << (r % 64);
    }
}

static bool is_sgpr_xor_pair(const RdnaInst& in, u32* dst, u32* s0, u32* s1) {
    if (in.op != OP_S_XOR_B32 || in.dst[0].kind != OPND_SGPR || in.src[0].kind != OPND_SGPR ||
        in.src[1].kind != OPND_SGPR)
        return false;
    *dst = in.dst[0].reg & 127;
    *s0 = in.src[0].reg & 127;
    *s1 = in.src[1].reg & 127;
    return true;
}

// Spill VGPRs: registers only v_writelane_b32 writes and only v_readlane_b32 reads, always at constant
// lanes. Scalar code parks SGPRs in their lanes; the translation keeps one variable per lane used
// instead of cross-lane operations (exact whatever the desktop wave size and whichever lanes run, and
// masks keep each lane's own bit), and tracks the parked registers' tags and descriptors through them.
static void find_spill_vgprs(Translator& t) {
    const Program& p = t.program;
    memset(t.spill_slot, 0xFF, sizeof(t.spill_slot));
    memset(t.spill_lanes, 0, sizeof(t.spill_lanes));
    t.spill_vgpr_count = 0;
    bool written[256] = {}, other[256] = {};
    for (u32 i = 0; i < p.count; i++) {
        const RdnaInst& in = p.insts[i];
        if (in.op == OP_V_MOVRELD_B32 || in.op == OP_V_MOVRELS_B32 || in.op == OP_V_MOVRELSD_B32 ||
            in.op == OP_V_MOVRELSD_2_B32)
            return; // registers picked at run time: any VGPR may be touched
        u32 lane;
        bool writelane = in.op == OP_V_WRITELANE_B32 && constant_lane(t, in.src[1], &lane);
        bool readlane = in.op == OP_V_READLANE_B32 && constant_lane(t, in.src[1], &lane);
        for (u32 d = 0; d < 2; d++) {
            const RdnaOperand& o = in.dst[d];
            if (o.kind != OPND_VGPR)
                continue;
            if (writelane && d == 0 && o.count <= 1) {
                written[o.reg & 255] = true;
                continue;
            }
            for (u32 k = 0; k < (o.count ? o.count : 1u); k++)
                other[(o.reg + k) & 255] = true;
        }
        for (u32 j = 0; j < in.num_src; j++) {
            const RdnaOperand& o = in.src[j];
            if (o.kind != OPND_VGPR || (readlane && j == 0 && o.count <= 1))
                continue;
            for (u32 k = 0; k < (o.count ? o.count : 1u); k++)
                other[(o.reg + k) & 255] = true;
        }
        if (rdna_op_encoding(in.op) == ENC_MIMG)
            for (u32 k = 0; k < in.mem.nsa_count && k < sizeof(in.mem.nsa_addr); k++)
                other[in.mem.nsa_addr[k]] = true;
    }
    for (u32 r = 0; r < 256; r++) {
        if (!written[r] || other[r] || t.spill_vgpr_count >= SHADER_MAX_SPILL_VGPRS)
            continue;
        t.spill_slot[r] = t.spill_vgpr_count;
        t.spill_vgprs[t.spill_vgpr_count++] = (u8)r;
    }
    for (u32 i = 0; i < p.count; i++) {
        const RdnaInst& in = p.insts[i];
        if (in.op != OP_V_WRITELANE_B32 && in.op != OP_V_READLANE_B32)
            continue;
        const RdnaOperand& v = in.op == OP_V_WRITELANE_B32 ? in.dst[0] : in.src[0];
        u32 lane;
        if (v.kind == OPND_VGPR && t.spill_slot[v.reg & 255] != 0xFF && constant_lane(t, in.src[1], &lane))
            t.spill_lanes[t.spill_slot[v.reg & 255]] |= 1ull << lane;
    }
}

// --- scalar register liveness ------------------------------------------------------------------------

static void mark_sgprs(u64 set[2], const RdnaOperand& o, u32 count) {
    if (o.kind == OPND_SPECIAL && o.reg == SRC_VCCZ) {
        set[SRC_VCC_LO / 64] |= 3ull << (SRC_VCC_LO % 64);
        return;
    }
    bool scalar = o.kind == OPND_SGPR || (o.kind == OPND_SPECIAL && o.reg < 128 && o.reg != SRC_NULL);
    if (!scalar)
        return;
    for (u32 k = 0; k < count; k++) {
        u32 reg = (o.reg + k) & 127;
        set[reg / 64] |= 1ull << (reg % 64);
    }
}

static u32 operand_count(const RdnaOperand& o) {
    return o.count ? o.count : 1u;
}

// Where SGPRs are live: only there do masks need converting at joins (see emit_edge_conversions).
// Reads not named by operands (VCC by its branches, M0 by LDS and interpolation, the destination of
// conditional moves, bit sets and SOPK arithmetic and compares) count as reads of everything that might
// be one; unmodelled writes just keep registers live longer, and writes that may not happen
// (conditional moves) are reads as well.
static bool analyze_liveness(Translator& t) {
    const Program& p = t.program;
    t.live_in = (u64(*)[2])calloc(p.block_count, sizeof(u64[2]));
    u64(*use)[2] = (u64(*)[2])calloc(p.block_count, sizeof(u64[2]));
    u64(*def)[2] = (u64(*)[2])calloc(p.block_count, sizeof(u64[2]));
    if (!t.live_in || !use || !def) {
        free(use);
        free(def);
        return false;
    }
    for (u32 b = 0; b < p.block_count; b++) {
        for (u32 i = p.blocks[b].first; i < p.blocks[b].end; i++) {
            const RdnaInst& in = p.insts[i];
            u64 reads[2] = {}, writes[2] = {};
            for (u32 j = 0; j < in.num_src; j++)
                mark_sgprs(reads, in.src[j], operand_count(in.src[j]));
            if (in.op == OP_S_CBRANCH_VCCZ || in.op == OP_S_CBRANCH_VCCNZ)
                reads[SRC_VCC_LO / 64] |= 3ull << (SRC_VCC_LO % 64);
            if (in.enc == ENC_DS || in.enc == ENC_VINTRP)
                reads[SRC_M0 / 64] |= 1ull << (SRC_M0 % 64);
            if (reads_destination(in))
                mark_sgprs(reads, in.dst[0], operand_count(in.dst[0]));
            for (u32 d = 0; d < 2 && writes_destination(in); d++) {
                // Wave32 lane masks are one dword, whatever the operand says.
                u32 count = operand_count(in.dst[d]);
                if (t.wave32 && writes_lane_mask(in, d))
                    count = 1;
                mark_sgprs(writes, in.dst[d], count);
            }
            for (u32 w = 0; w < 2; w++) {
                use[b][w] |= reads[w] & ~def[b][w];
                def[b][w] |= writes[w];
            }
        }
    }
    for (bool changed = true; changed;) {
        changed = false;
        for (u32 b = p.block_count; b-- > 0;) {
            u64 out[2] = {};
            for (u32 k = 0; k < 2; k++) {
                s32 n = p.blocks[b].next[k];
                if (n >= 0)
                    for (u32 w = 0; w < 2; w++)
                        out[w] |= t.live_in[n][w];
            }
            for (u32 w = 0; w < 2; w++) {
                u64 live = use[b][w] | (out[w] & ~def[b][w]);
                if (live != t.live_in[b][w]) {
                    t.live_in[b][w] = live;
                    changed = true;
                }
            }
        }
    }
    free(use);
    free(def);
    return true;
}

bool sgpr_live_in(const Translator& t, u32 block, u32 reg) {
    reg &= 127;
    return (t.live_in[block][reg / 64] >> (reg % 64)) & 1ull;
}

static bool analyze_tags(Translator& t) {
    Program& p = t.program;
    find_spill_vgprs(t);
    t.xor_swap_role = (u8*)calloc(p.count + 1, 1);
    if (!t.xor_swap_role)
        return false;
    for (u32 b = 0; b < p.block_count; b++) {
        for (u32 i = p.blocks[b].first; i + 2 < p.blocks[b].end; i++) {
            u32 d0, a0, b0, d1, a1, b1, d2, a2, b2;
            if (!is_sgpr_xor_pair(p.insts[i], &d0, &a0, &b0) ||
                !is_sgpr_xor_pair(p.insts[i + 1], &d1, &a1, &b1) ||
                !is_sgpr_xor_pair(p.insts[i + 2], &d2, &a2, &b2))
                continue;
            if (a0 == b0 || d0 != a0 || d2 != a0 || d1 != b0)
                continue;
            if (!((a1 == a0 && b1 == b0) || (a1 == b0 && b1 == a0)))
                continue;
            if (!((a2 == a0 && b2 == b0) || (a2 == b0 && b2 == a0)))
                continue;
            t.xor_swap_role[i] = 1;
            t.xor_swap_role[i + 1] = 2;
            t.xor_swap_role[i + 2] = 2;
            i += 2;
        }
    }
    t.block_entry = (TagState*)calloc(p.block_count, sizeof(TagState));
    bool* reached = (bool*)calloc(p.block_count, 1);
    if (!t.block_entry || !reached) {
        free(reached);
        return false;
    }
    tags_entry(t, *t.info, t.block_entry[0]);
    reached[0] = true;
    // Iterate to a fixed point; tags only ever fall to TAG_NONE, so this terminates.
    for (bool changed = true; changed;) {
        changed = false;
        for (u32 b = 0; b < p.block_count; b++) {
            if (!reached[b])
                continue;
            TagState s = t.block_entry[b];
            for (u32 i = p.blocks[b].first; i < p.blocks[b].end; i++)
                tags_transfer(t, s, p.insts[i], i);
            for (u32 k = 0; k < 2; k++) {
                s32 n = p.blocks[b].next[k];
                if (n < 0)
                    continue;
                if (!reached[n]) {
                    t.block_entry[n] = s;
                    reached[n] = true;
                    changed = true;
                } else if (tags_merge(t, t.block_entry[n], s, (u32)n)) {
                    changed = true;
                }
            }
        }
    }
    free(reached);
    return true;
}

// --- driver ------------------------------------------------------------------------------------------

void translator_fail(Translator& t, const char* format, ...) {
    if (t.failed)
        return;
    t.failed = true;
    va_list args;
    va_start(args, format);
    vsnprintf(t.out->error, sizeof(t.out->error), format, args);
    va_end(args);
}

static void release(Translator& t) {
    free(t.program.insts);
    free(t.program.blocks);
    free(t.program.block_of);
    free(t.block_entry);
    free(t.xor_swap_role);
    free(t.live_in);
    t.body.release();
}

// Exact lanes for compute (see translate.h): desktop waves of 32 lanes, one console wave each.
static bool choose_lanes(Translator& t) {
    const ShaderInfo& info = *t.info;
    t.lanes = 1;
    t.exact = info.stage == SHADER_COMPUTE;
    if (!t.exact)
        return true;
    u32 console_wave = t.wave32 ? 32u : 64u;
    if (t.target->wave_min > 32 || t.target->wave_max < 32) {
        translator_fail(t, "desktop compute waves of %u-%u lanes cannot run a wave%u program lane for lane",
                        t.target->wave_min, t.target->wave_max, console_wave);
        return false;
    }
    t.lanes = t.wave32 ? 1 : 2;
    u32 threads = 1;
    for (u32 i = 0; i < 3; i++)
        threads *= info.threads[i] ? info.threads[i] : 1u;
    t.wave_threads = threads;
    t.group_waves = (threads + console_wave - 1) / console_wave;
    if (t.group_waves * 32u > 1024u) {
        translator_fail(t, "%u threads per group", threads);
        return false;
    }
    return true;
}

bool shader_translate(const void* code, u32 size, const ShaderInfo& info, const ShaderTarget& target,
                      TranslatedShader* out) {
    Text keep = out->hlsl;
    memset(out, 0, sizeof(*out));
    out->hlsl = keep;
    out->hlsl.clear();
    Translator t = {};
    t.info = &info;
    t.target = &target;
    t.out = out;
    t.code = (const u8*)code;
    t.code_size = size;
    bool ok = decode_program(code, size, t.program, out->error, sizeof(out->error)) &&
              build_blocks(t.program, out->error, sizeof(out->error));
    if (ok) {
        detect_wave_size(t);
        ok = choose_lanes(t) && analyze_tags(t) && (!t.exact || analyze_liveness(t));
    }
    if (ok) {
        emit_program(t);
        ok = !t.failed && !t.body.failed && !out->hlsl.failed;
    }
    release(t);
    return ok;
}
