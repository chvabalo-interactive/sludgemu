// Console shader translation: program analysis and HLSL emission (see translate.h for the model).

#include "shader/translate.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "shader/rdna.h"
#include "shader/translate_internal.h"

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
    } else if (o.kind == OPND_VGPR) {
        u32 reg = (o.reg + part) & 255;
        s.vgpr[reg] = t;
        s.vgpr_const[reg] = false;
    }
}

// Scalar registers whose values the analysis tracks: SGPRs and VCC (compilers build descriptors in VCC
// when SGPRs run short; translated code holds VCC as a plain value unless a compare writes it, which
// forgets the constant). EXEC, held as a broadcast mask, and M0 are left out: nothing reads their
// constants. `part` picks a dword of a wider operand.
static bool is_scalar_value_reg(const RdnaOperand& o, u32 part = 0) {
    if (o.kind == OPND_SGPR)
        return true;
    u32 reg = o.reg + part;
    return o.kind == OPND_SPECIAL && reg >= SRC_VCC_LO && reg <= SRC_VCC_HI;
}

// The value of an integer operand known at translation time: literals (zero-extended for 64-bit
// operations), inline integers (sign-extended), and SGPRs and VCC holding constants.
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
    case OPND_SPECIAL:
        return is_scalar_value_reg(o, part) && sgpr_constant(s, o.reg + part, value);
    default:
        return false;
    }
}

static void set_constant(TagState& s, const RdnaOperand& o, u32 part, u32 value) {
    if (!is_scalar_value_reg(o, part))
        return;
    u32 reg = (o.reg + part) & 127;
    s.constant[reg] = value;
    s.constant_known[reg / 64] |= 1ull << (reg % 64);
}

static u32 descriptor_of(const TagState& s, const RdnaOperand& o, u32 part = 0) {
    if (o.kind == OPND_SGPR || (o.kind == OPND_SPECIAL && o.reg < 128 && o.reg != SRC_NULL))
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

// EXEC is always a lane mask; VCC is one when its tag says so (compilers also park other values in it,
// descriptors among them).
static bool mask_source(const TagState& s, const RdnaOperand& o) {
    return is_exec(o) || tag_kind(tag_of(s, o)) == TAG_MASK;
}

// Address-like results keep their source's tag (heap offsets, root buffer high dwords). A loaded
// descriptor dword is read as its value (register_data), so arithmetic on one gives a plain value: a
// descriptor rebuilt from such values is no longer the slot's (see descriptor_origin).
static Tag carried(Tag a, Tag b) {
    Tag r = TAG_NONE;
    if (a != TAG_NONE && b == TAG_NONE)
        r = a;
    else if (b != TAG_NONE && a == TAG_NONE)
        r = b;
    else if (a == b)
        r = a;
    return heap_loaded(r) ? TAG_NONE : r;
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
    if (!is_scalar_value_reg(in.dst[0]) || in.dst[0].count != 1)
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
    if (is_scalar_value_reg(o, part)) {
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

static void transfer(const Translator& t, TagState& s, const RdnaInst& in, u32 inst_idx);

bool join_converts(Tag tag) {
    return heap_loaded(tag) && (tag_payload(tag) & (1 | HEAP_MODIFIED | HEAP_IMAGE)) == HEAP_RESOURCES;
}

// Whether two tags don't name different tables (or heaps), which would make their slot indices
// incomparable.
static bool same_table(Tag a, Tag b) {
    if (tag_kind(a) != TAG_HEAP || tag_kind(b) != TAG_HEAP)
        return true;
    return ((tag_payload(a) ^ tag_payload(b)) & 1) == 0 && heap_parameter(a) == heap_parameter(b);
}

// A register's tag where two paths join (or where an instruction leaves either of two values): what both
// agree on, or nothing. Loaded descriptor dwords are held as heap addresses, which must not pass for
// values: a loaded dword on both paths stays one, of its table (of a table not known, 0x3F, where the
// paths' tables differ). On one path only, a scalar register at a join of blocks becomes a value
// (`converting`: the path holding the address reads the dword from the shadow at the jump, see
// emit_join_conversions); otherwise it is marked HEAP_MODIFIED, which nothing can use.
//
// A dword that can't be converted (one the shadow doesn't hold, an image's or a sampler's, or one
// already an address on some paths only) joins as a value all the same, holding its heap address on
// that path, as before the shadow: compilers leave such dwords in registers on paths where they are
// dead, and liveness over the block graph can't tell (exec-masked skips of a loop's body join with
// stale registers).
static Tag merged_tag(Tag a, Tag b, bool converting = false) {
    if (a == b)
        return a;
    bool la = heap_loaded(a), lb = heap_loaded(b);
    if (!la && !lb)
        return TAG_NONE;
    if (converting && la != lb)
        return TAG_NONE;
    u32 pa = la ? tag_payload(a) : 0, pb = lb ? tag_payload(b) : 0;
    u32 parameter = la ? heap_parameter(a) : heap_parameter(b);
    if (la && lb && !same_table(a, b))
        parameter = 0x3F;
    u32 payload = ((pa | pb) & 1) | parameter << 1 | HEAP_LOADED | ((pa | pb) & (HEAP_MODIFIED | HEAP_IMAGE));
    if (!la || !lb || ((pa ^ pb) & 1))
        payload |= HEAP_MODIFIED;
    return make_tag(TAG_HEAP, payload);
}

// A conditional move (s_cmov) of a value with `tag`, `descriptor` and maybe a constant into one dword of
// `dst`: the register then holds either value, so it keeps only what both have, as at a join.
static void conditional_move(TagState& s, const RdnaOperand& dst, u32 part, Tag tag, u32 descriptor,
                             bool known, u32 value) {
    u32 old_value = 0;
    bool old_known = constant_of(s, dst, part, &old_value);
    u32 old_descriptor = descriptor_of(s, dst, part);
    Tag old_tag = tag_of(s, dst, part);
    set_tag(s, dst, part, merged_tag(old_tag, tag));
    set_descriptor(s, dst, part,
                   old_descriptor == descriptor && same_table(old_tag, tag) ? descriptor : NO_DESCRIPTOR);
    if (known && old_known && value == old_value)
        set_constant(s, dst, part, value);
}

// A vector move without modifiers: the value as it is.
static bool plain_vector_move(const RdnaInst& in) {
    return !in.has_sdwa && !in.has_dpp && !in.src[0].abs && !in.src[0].neg && !in.omod && !in.clamp;
}

bool copies_tags(const Translator& t, const RdnaInst& in, u32 inst_idx) {
    if (inst_idx != ~0u && t.xor_swap_role && t.xor_swap_role[inst_idx])
        return true;
    u32 lane;
    switch (in.op) {
    case OP_S_MOV_B32:
    case OP_S_CMOV_B32:
    case OP_S_MOV_B64:
    case OP_S_CMOV_B64:
    case OP_V_READLANE_B32:
        return true;
    case OP_V_MOV_B32:
    case OP_V_READFIRSTLANE_B32:
        return plain_vector_move(in);
    case OP_V_WRITELANE_B32: // into a spill slot, which keeps the tag; elsewhere one lane of a value
        return in.dst[0].kind == OPND_VGPR && t.spill_slot[in.dst[0].reg & 255] != 0xFF &&
               constant_lane(t, in.src[1], &lane);
    default:
        return false;
    }
}

void tags_transfer(const Translator& t, TagState& s, const RdnaInst& in, u32 inst_idx) {
    // A three-XOR swap moves tags and constants as a whole; folding its steps one by one would undo it.
    bool swap = inst_idx != ~0u && t.xor_swap_role && t.xor_swap_role[inst_idx] != 0;
    u32 folded = 0;
    bool fold = !swap && fold_scalar(s, in, &folded);
    transfer(t, s, in, inst_idx);
    if (fold)
        set_constant(s, in.dst[0], 0, folded);
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
            // The whole swap happens here: the source that is not the destination is the register
            // exchanged with it, dword by dword.
            const RdnaOperand& other = in.src[0].reg == in.dst[0].reg ? in.src[1] : in.src[0];
            u32 width = in.op == OP_S_XOR_B64 ? 2 : 1;
            for (u32 part = 0; part < width; part++) {
                u32 a = (in.dst[0].reg + part) & 127, b = (other.reg + part) & 127;
                Tag st = s.sgpr[a];
                s.sgpr[a] = s.sgpr[b];
                s.sgpr[b] = st;
                u16 sd = s.descriptor[a];
                s.descriptor[a] = s.descriptor[b];
                s.descriptor[b] = sd;
                u32 sv = s.constant[a];
                s.constant[a] = s.constant[b];
                s.constant[b] = sv;
                u64 ka = (s.constant_known[a / 64] >> (a % 64)) & 1ull;
                u64 kb = (s.constant_known[b / 64] >> (b % 64)) & 1ull;
                s.constant_known[a / 64] =
                    (s.constant_known[a / 64] & ~(1ull << (a % 64))) | (kb << (a % 64));
                s.constant_known[b / 64] =
                    (s.constant_known[b / 64] & ~(1ull << (b % 64))) | (ka << (b % 64));
            }
            return;
        }
        if (role == 2)
            return;
    }
    switch (in.op) {
    case OP_S_MOV_B32:
    case OP_S_CMOV_B32: {
        Tag tag = is_exec(in.src[0]) ? make_tag(TAG_MASK, 0) : tag_of(s, in.src[0]);
        u32 descriptor = descriptor_of(s, in.src[0]);
        bool known = constant_of(s, in.src[0], 0, &value);
        if (in.op == OP_S_CMOV_B32) {
            conditional_move(s, in.dst[0], 0, tag, descriptor, known, value);
            return;
        }
        set_tag(s, in.dst[0], 0, tag);
        set_descriptor(s, in.dst[0], 0, descriptor);
        if (known)
            set_constant(s, in.dst[0], 0, value);
        return;
    }
    case OP_V_MOV_B32:
    case OP_V_READFIRSTLANE_B32: {
        bool raw = plain_vector_move(in);
        u32 cv = 0;
        clear_spill(t, s, in.dst[0], 0);
        set_tag(s, in.dst[0], 0, raw ? tag_of(s, in.src[0]) : TAG_NONE);
        set_descriptor(s, in.dst[0], 0, raw ? descriptor_of(s, in.src[0]) : NO_DESCRIPTOR);
        if (is_exec(in.src[0]))
            set_tag(s, in.dst[0], 0, make_tag(TAG_MASK, 0));
        if (raw && eval_const_u32(s, in.src[0], TYPE_32, 0, &cv))
            set_any_constant(s, in.dst[0], 0, cv);
        return;
    }
    case OP_V_SWAP_B32: {
        // Both registers change; under a partial EXEC neither holds one value, so forget both. Swapped
        // as they are, a loaded dword's address may end up in either (see merged_tag).
        Tag a = tag_of(s, in.dst[0]), b = tag_of(s, in.src[0]);
        Tag either = heap_loaded(a) || heap_loaded(b) ? merged_tag(a, b) : TAG_NONE;
        clear_spill(t, s, in.dst[0], 0);
        clear_spill(t, s, in.src[0], 0);
        set_tag(s, in.dst[0], 0, either);
        set_tag(s, in.src[0], 0, either);
        return;
    }
    case OP_S_MOVK_I32:
        set_tag(s, in.dst[0], 0, TAG_NONE);
        set_descriptor(s, in.dst[0], 0, NO_DESCRIPTOR);
        set_constant(s, in.dst[0], 0, (u32)(s32)in.simm16);
        return;
    case OP_S_CMOVK_I32:
        conditional_move(s, in.dst[0], 0, TAG_NONE, NO_DESCRIPTOR, true, (u32)(s32)in.simm16);
        return;
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
    case OP_S_CMPK_LE_U32:
    case OP_S_SETREG_B32:
    case OP_S_WAITCNT_VSCNT:
    case OP_S_WAITCNT_VMCNT:
    case OP_S_WAITCNT_EXPCNT:
    case OP_S_WAITCNT_LGKMCNT:
        return; // their register (decoded as the destination) is only read
    case OP_S_ADDK_I32: {
        // Adds to the register in place: a constant stays one, anything else is forgotten.
        set_descriptor(s, in.dst[0], 0, NO_DESCRIPTOR);
        bool known = constant_of(s, in.dst[0], 0, &value);
        set_tag(s, in.dst[0], 0, carried(tag_of(s, in.dst[0]), TAG_NONE));
        if (known)
            set_constant(s, in.dst[0], 0, value + (u32)(s32)in.simm16);
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
        bool constant = constant_of(s, in.src[0], 0, &values[0]) && constant_of(s, in.src[0], 1, &values[1]);
        bool mask = mask_source(s, in.src[0]);
        Tag tags[2];
        u32 descriptors[2];
        for (u32 i = 0; i < 2; i++) {
            tags[i] = mask ? make_tag(TAG_MASK, 0) : tag_of(s, in.src[0], i);
            descriptors[i] = descriptor_of(s, in.src[0], i);
        }
        for (u32 i = 0; i < 2; i++) {
            if (in.op == OP_S_CMOV_B64) {
                conditional_move(s, in.dst[0], i, tags[i], descriptors[i], constant, values[i]);
                continue;
            }
            set_tag(s, in.dst[0], i, tags[i]);
            set_descriptor(s, in.dst[0], i, descriptors[i]);
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
    case OP_S_NOT_B32:
    case OP_S_CSELECT_B32:
    case OP_S_WQM_B32: {
        bool mask = mask_source(s, in.src[0]) || (in.num_src > 1 && mask_source(s, in.src[1])) ||
                    is_exec_or_vcc(in.dst[0]) || in.op == OP_S_WQM_B64 || in.op == OP_S_WQM_B32 ||
                    in.op == OP_S_BITREPLICATE_B64_B32;
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
        // Loads through a loaded descriptor read the memory it points to: plain values (emission fails
        // on them).
        bool null_soffset = in.src[1].kind == OPND_SPECIAL && in.src[1].reg == SRC_NULL;
        if (tag_kind(base) == TAG_HEAP && !heap_loaded(base)) {
            // Descriptors from the heap: each register holds its own byte address.
            result = (Tag)(base | HEAP_LOADED);
            // At a known place when loaded from the table's start with a constant offset.
            slot = (tag_payload(base) & 1) == HEAP_SAMPLERS ? 16 : 32;
            u32 at = descriptor_of(s, in.src[0]);
            if (at != NO_DESCRIPTOR && null_soffset && in.mem.offset >= 0)
                first = at * slot + (u32)in.mem.offset;
            // Image descriptors, whose bits the shadow doesn't hold: what is loaded 8 dwords or more at a
            // time, and what is in the second half of a 32-byte slot. Programs address descriptors by
            // whole slots, so the base and a register offset are taken to be multiples of the slot size.
            if (slot == 32) {
                u32 soffset = 0;
                constant_of(s, in.src[1], 0, &soffset);
                bool image = in.dst[0].count >= 8;
                for (u32 i = 0; i < in.dst[0].count; i++) {
                    u32 at_byte = (u32)in.mem.offset + soffset + 4 * i;
                    Tag tag = !image && at_byte % 32 < 16 ? result : (Tag)(result | HEAP_IMAGE);
                    set_tag(s, in.dst[0], i, tag);
                    set_descriptor(s, in.dst[0], i,
                                   first == NO_DESCRIPTOR ? NO_DESCRIPTOR : (first + 4 * i) / slot);
                }
                return;
            }
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
    RdnaEncoding base = rdna_op_encoding(in.op);
    for (u32 d = 0; d < 2; d++) {
        const RdnaOperand& dst = in.dst[d];
        if (dst.kind == OPND_NONE)
            continue;
        bool mask = base == ENC_VOPC || (d == 1 && in.enc == ENC_VOP3) ||
                    (in.op >= OP_S_AND_SAVEEXEC_B64 && in.op <= OP_S_XNOR_SAVEEXEC_B64) ||
                    (in.op >= OP_S_ANDN1_SAVEEXEC_B64 && in.op <= OP_S_ANDN2_WREXEC_B64) ||
                    (in.op >= OP_S_AND_SAVEEXEC_B32 && in.op <= OP_S_ANDN2_WREXEC_B32) ||
                    (base == ENC_VOP2 && d == 1);
        u32 count = (mask && t.wave32) ? 1 : dst.count;
        for (u32 i = 0; i < count; i++) {
            clear_spill(t, s, dst, i);
            set_tag(s, dst, i, mask ? make_tag(TAG_MASK, 0) : TAG_NONE);
            set_descriptor(s, dst, i, NO_DESCRIPTOR);
        }
    }
}

static bool merge_tag(Tag& into, Tag from, bool converting) {
    Tag m = merged_tag(into, from, converting);
    if (m == into)
        return false;
    into = m;
    return true;
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
    // Slot indices first, while the tags still tell which table each is in.
    for (u32 i = 0; i < 128; i++)
        if ((into.descriptor[i] != from.descriptor[i] || !same_table(into.sgpr[i], from.sgpr[i])) &&
            into.descriptor[i] != NO_DESCRIPTOR) {
            into.descriptor[i] = NO_DESCRIPTOR;
            changed = true;
        }
    for (u32 i = 0; i < 128; i++)
        changed |= merge_tag(into.sgpr[i], from.sgpr[i], true);
    // Vector registers aren't converted: lanes outside EXEC may hold anything, the shadow has no room
    // for reads at stray addresses.
    for (u32 i = 0; i < 256; i++)
        changed |= merge_tag(into.vgpr[i], from.vgpr[i], false);
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
            if ((into.spill_desc[slot][lane] != from.spill_desc[slot][lane] ||
                 !same_table(into.spill_tag[slot][lane], from.spill_tag[slot][lane])) &&
                into.spill_desc[slot][lane] != NO_DESCRIPTOR) {
                into.spill_desc[slot][lane] = NO_DESCRIPTOR;
                changed = true;
            }
            changed |= merge_tag(into.spill_tag[slot][lane], from.spill_tag[slot][lane], true);
        }
    return changed;
}

static void tags_entry(const ShaderInfo& info, TagState& s) {
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
    s.sgpr[SRC_VCC_LO] = s.sgpr[SRC_VCC_HI] = make_tag(TAG_MASK, 0);
}

// An operand of a scalar XOR that can take part in a swap: SGPRs or VCC (never EXEC), `width` dwords.
static bool xor_register(const RdnaOperand& o, u32 width, u32* reg) {
    bool vcc = o.kind == OPND_SPECIAL && (o.reg == SRC_VCC_LO || (o.reg == SRC_VCC_HI && width == 1));
    if (o.kind != OPND_SGPR && !vcc)
        return false;
    *reg = o.reg;
    return true;
}

// A scalar XOR of registers: destination, sources and width in dwords.
static bool xor_operands(const RdnaInst& in, u32* dst, u32* s0, u32* s1, u32* width) {
    if (in.op != OP_S_XOR_B32 && in.op != OP_S_XOR_B64)
        return false;
    *width = in.op == OP_S_XOR_B64 ? 2 : 1;
    return xor_register(in.dst[0], *width, dst) && xor_register(in.src[0], *width, s0) &&
           xor_register(in.src[1], *width, s1);
}

static bool overlaps(const RdnaOperand& o, u32 reg, u32 width) {
    if (o.kind != OPND_SGPR && !(o.kind == OPND_SPECIAL && o.reg < 128))
        return false;
    u32 first = o.reg, count = o.count ? o.count : 1u; // special registers may come with count 0
    return first < reg + width && reg < first + count;
}

// Whether an instruction reads or writes any of the scalar registers [reg, reg + width), including the
// VCC reads the decoder leaves implicit.
static bool touches(const RdnaInst& in, u32 reg, u32 width) {
    for (u32 d = 0; d < 2; d++)
        if (overlaps(in.dst[d], reg, width))
            return true;
    bool vcc = reg <= SRC_VCC_HI && SRC_VCC_LO < reg + width;
    for (u32 k = 0; k < in.num_src; k++)
        if (overlaps(in.src[k], reg, width) ||
            (vcc && in.src[k].kind == OPND_SPECIAL && in.src[k].reg == SRC_VCCZ))
            return true;
    return vcc && (in.op == OP_S_CBRANCH_VCCZ || in.op == OP_S_CBRANCH_VCCNZ || in.op == OP_V_DIV_FMAS_F32 ||
                   in.op == OP_V_DIV_FMAS_F64);
}

static bool picks_registers_at_run_time(RdnaOp op) {
    return op == OP_S_MOVRELS_B32 || op == OP_S_MOVRELS_B64 || op == OP_S_MOVRELD_B32 ||
           op == OP_S_MOVRELD_B64 || op == OP_S_MOVRELSD_2_B32 || op == OP_V_MOVRELD_B32 ||
           op == OP_V_MOVRELS_B32 || op == OP_V_MOVRELSD_B32 || op == OP_V_MOVRELSD_2_B32;
}

// Three-XOR register swaps (a ^= b; b ^= a; a ^= b), which compilers emit to exchange two registers
// without a spare one. Folded step by step the tags would be lost, so the first XOR swaps the tags and
// the other two leave them alone (xor_swap_role 1 and 2); the XORs themselves are still emitted, on
// plain values. The steps may be interleaved with other instructions as long as none of those touches
// the two registers, and all three must be in one block.
static void find_xor_swaps(Translator& t) {
    const Program& p = t.program;
    for (u32 i = 0; i < p.count; i++)
        if (picks_registers_at_run_time(p.insts[i].op))
            return; // any register may change anywhere: no swap is certain
    for (u32 b = 0; b < p.block_count; b++) {
        for (u32 i = p.blocks[b].first; i < p.blocks[b].end; i++) {
            u32 d, s0, s1, w;
            if (t.xor_swap_role[i] || !xor_operands(p.insts[i], &d, &s0, &s1, &w) || s0 == s1 ||
                (d != s0 && d != s1))
                continue;
            u32 other = d == s0 ? s1 : s0;
            if (d < other + w && other < d + w)
                continue; // partly the same registers
            u32 steps[3] = {i, 0, 0};
            u32 n = 1;
            for (u32 j = i + 1; j < p.blocks[b].end && n < 3; j++) {
                const RdnaInst& in = p.insts[j];
                if (in.op == OP_S_SETPC_B64 || in.op == OP_S_SWAPPC_B64 || in.op == OP_S_GETPC_B64)
                    break;
                if (!touches(in, d, w) && !touches(in, other, w))
                    continue;
                // The next step: an XOR of the same two registers into `other`, then into `d`.
                u32 jd, j0, j1, jw;
                if (t.xor_swap_role[j] || !xor_operands(in, &jd, &j0, &j1, &jw) || jw != w ||
                    !((j0 == d && j1 == other) || (j0 == other && j1 == d)) || jd != (n == 1 ? other : d))
                    break;
                steps[n++] = j;
            }
            if (n < 3)
                continue;
            t.xor_swap_role[steps[0]] = 1;
            t.xor_swap_role[steps[1]] = 2;
            t.xor_swap_role[steps[2]] = 2;
        }
    }
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

// Scalar registers an instruction reads and writes, as 128-bit sets. Reads err on the side of more:
// registers read implicitly (VCC, M0) count as read by everything, and conditional or accumulating
// scalar writes as reads too.
static void add_registers(u64 set[2], u32 first, u32 count) {
    for (u32 r = first; r < first + count && r < 128; r++)
        set[r / 64] |= 1ull << (r % 64);
}

static void scalar_registers(const RdnaInst& in, u64 reads[2], u64 writes[2]) {
    reads[0] = reads[1] = writes[0] = writes[1] = 0;
    for (u32 k = 0; k < in.num_src; k++) {
        const RdnaOperand& o = in.src[k];
        if (o.kind == OPND_SGPR)
            add_registers(reads, o.reg, o.count ? o.count : 1u);
        else if (o.kind == OPND_SPECIAL && o.reg < 128 && o.reg != SRC_NULL)
            add_registers(reads, o.reg, o.count > 2 ? o.count : 2u);
    }
    bool reads_dst = in.op == OP_S_CMOV_B32 || in.op == OP_S_CMOV_B64 || in.op == OP_S_CMOVK_I32 ||
                     (in.enc == ENC_SOPK && in.op != OP_S_MOVK_I32);
    for (u32 d = 0; d < 2; d++) {
        const RdnaOperand& o = in.dst[d];
        if (o.kind != OPND_SGPR && !(o.kind == OPND_SPECIAL && o.reg < 128 && o.reg != SRC_NULL))
            continue;
        add_registers(writes, o.reg, o.count ? o.count : 1u);
        if (reads_dst)
            add_registers(reads, o.reg, o.count ? o.count : 1u);
    }
    add_registers(reads, SRC_VCC_LO, 2);
    add_registers(reads, SRC_M0, 1);
}

// Scalar registers live where each block starts (read before written on some path from there), which
// tells joins what is worth converting (see emit_join_conversions). Everything, where the program
// picks registers at run time.
static bool find_live_sgprs(Translator& t) {
    const Program& p = t.program;
    t.live_sgprs = (u64*)calloc((usize)p.block_count * 2, sizeof(u64));
    if (!t.live_sgprs)
        return false;
    for (u32 i = 0; i < p.count; i++) {
        if (picks_registers_at_run_time(p.insts[i].op)) {
            memset(t.live_sgprs, 0xFF, (usize)p.block_count * 2 * sizeof(u64));
            return true;
        }
    }
    for (bool changed = true; changed;) {
        changed = false;
        for (u32 b = p.block_count; b-- > 0;) {
            u64 live[2] = {0, 0};
            for (u32 k = 0; k < 2; k++) {
                s32 n = p.blocks[b].next[k];
                if (n >= 0) {
                    live[0] |= t.live_sgprs[2 * n];
                    live[1] |= t.live_sgprs[2 * n + 1];
                }
            }
            for (u32 i = p.blocks[b].end; i-- > p.blocks[b].first;) {
                u64 reads[2], writes[2];
                scalar_registers(p.insts[i], reads, writes);
                for (u32 w = 0; w < 2; w++)
                    live[w] = (live[w] & ~writes[w]) | reads[w];
            }
            for (u32 w = 0; w < 2; w++) {
                if (live[w] != t.live_sgprs[2 * b + w]) {
                    t.live_sgprs[2 * b + w] = live[w];
                    changed = true;
                }
            }
        }
    }
    return true;
}

static bool analyze_tags(Translator& t) {
    Program& p = t.program;
    find_spill_vgprs(t);
    t.xor_swap_role = (u8*)calloc(p.count + 1, 1);
    if (!t.xor_swap_role)
        return false;
    find_xor_swaps(t);
    t.block_entry = (TagState*)calloc(p.block_count, sizeof(TagState));
    bool* reached = (bool*)calloc(p.block_count, 1);
    if (!t.block_entry || !reached) {
        free(reached);
        return false;
    }
    tags_entry(*t.info, t.block_entry[0]);
    reached[0] = true;
    // Iterate to a fixed point; joins only ever lower tags (to modified heap tags, then to TAG_NONE), so
    // this terminates.
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
    return find_live_sgprs(t);
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
    free(t.live_sgprs);
    t.body.release();
}

bool shader_translate(const void* code, u32 size, const ShaderInfo& info, TranslatedShader* out) {
    Text keep = out->hlsl;
    memset(out, 0, sizeof(*out));
    out->hlsl = keep;
    out->hlsl.clear();
    Translator t = {};
    t.info = &info;
    t.out = out;
    t.code = (const u8*)code;
    t.code_size = size;
    bool ok = decode_program(code, size, t.program, out->error, sizeof(out->error)) &&
              build_blocks(t.program, out->error, sizeof(out->error));
    if (ok) {
        detect_wave_size(t);
        ok = analyze_tags(t);
    }
    if (ok)
        find_slot_uses(t);
    if (ok) {
        emit_program(t);
        ok = !t.failed && !t.body.failed && !out->hlsl.failed;
    }
    release(t);
    return ok;
}
