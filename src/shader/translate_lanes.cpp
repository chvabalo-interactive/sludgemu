// Console shader translation: which vector writes may skip their EXEC guard.
//
// A vector instruction only writes the lanes whose EXEC bit is set; the others keep their old value.
// Translated literally (`if (EXEC) v = x;`), every vector register written while some lanes are off
// carries its old value along, so to the desktop compiler nearly every register is live across the
// whole program: long programs then take minutes to compile (its loop and jump threading passes scale
// with the values live across loops), and the extra values cost registers on the GPU as well.
//
// Most of those old values are never seen again: a temporary written and read under the same EXEC,
// then overwritten. The analysis here finds the writes whose skipped lanes nobody reads before they
// are overwritten; those are emitted unguarded, writing every lane. It is a backward liveness of the
// vector registers that knows which lanes a later read can see:
//
// - EXEC versions (forward). Every instruction that writes EXEC, and every join where the paths bring
//   different EXECs, starts a version of EXEC. The usual masking code nests: a version made by ANDing
//   EXEC (s_and_saveexec, s_and/s_andn2 of EXEC, v_cmpx) holds a subset of the lanes of the one before,
//   and moving a saved copy back (s_mov exec, s[n:n+1], s_or exec, exec, s[n:n+1]) returns to the saved
//   version. So at each point the analysis keeps a chain of versions, outermost first, each a subset of
//   the one before and the last one EXEC itself, and which SGPRs hold a copy of which version. A
//   version's name stands for its latest instance: making a version again drops the chain entries and
//   copies of the previous one. Whole-wave code (EXEC = -1) keeps the chain to return to.
// - Liveness (backward). Per vector register, the lanes later reads may see of its current value:
//   none (dead), those within version Z of the chain (a read under EXEC Z, or under a version inside
//   it), or any. A read outside its EXEC (lane reads, DPP, permutes, image sampling with derivatives
//   from neighbouring lanes, exports) sees any lane. A write under EXEC X whose register is then dead
//   or seen within X only may write every lane: the lanes outside X are never looked at again. Moving
//   back over a narrowing that made version X, "within X" becomes "within the version it narrowed".
//
// Instructions that keep part of their destination (16-bit halves, accumulators, SDWA preserving the
// other bits) read the destination as well. Anything not modelled exactly keeps its EXEC guards, which
// is always correct: only the writes the analysis marks change. Emission double-checks that a write
// the analysis treated as replacing its destination doesn't read it (see lane_write_replaces), and
// translation starts over without the analysis if one did.

#include <stdlib.h>
#include <string.h>

#include "shader/translate_internal.h"

#define CHAIN_MAX 16

// Version names: the start of the program, every lane of the wave, instructions (FIRST_MADE + index)
// and joins (JOIN_VERSION | block). Compute waves start with every lane (in whole waves).
enum : u32 {
    NO_VERSION = 0,
    START_VERSION = 1,
    ALL_VERSION = 2,
    FIRST_MADE = 3,
    JOIN_VERSION = 0x80000000u,
};

// Liveness of a register's value: DEAD, ANY_LANE, or the version its lanes seen are within.
enum : u32 {
    DEAD = 0,
    ANY_LANE = 0xFFFFFFFFu,
};

struct ExecChain {
    u32 version[CHAIN_MAX]; // outermost first; the last is EXEC's own version
    u8 depth;
    bool all; // EXEC holds every lane (whole-wave code); `version` is the chain to go back to
};

// What an SGPR pair holds (one SGPR for wave32): a copy of a version's EXEC, or with HELD_INSIDE a mask
// inside it (EXEC & something, compare results, which are clear outside EXEC), or NO_VERSION.
enum : u32 {
    HELD_INSIDE = 0x40000000u,
};

struct ExecState {
    ExecChain chain;
    u32 held[128]; // per SGPR
};

static u32 held_version(u32 held) {
    return held & ~HELD_INSIDE;
}

static bool chain_has(const ExecChain& c, u32 v, u32* at = nullptr) {
    for (u32 i = 0; i < c.depth; i++)
        if (c.version[i] == v) {
            if (at)
                *at = i;
            return true;
        }
    return false;
}

static bool same_chain(const ExecChain& a, const ExecChain& b) {
    return a.depth == b.depth && a.all == b.all && memcmp(a.version, b.version, a.depth * sizeof(u32)) == 0;
}

// The version EXEC is, for reads and writes: NO_VERSION in whole-wave code.
static u32 current(const ExecChain& c) {
    return c.all || c.depth == 0 ? NO_VERSION : c.version[c.depth - 1];
}

// Forgets the previous instance of `v`, about to be made again.
static void drop_version(ExecState& s, u32 v) {
    u32 at;
    if (chain_has(s.chain, v, &at)) {
        memmove(&s.chain.version[at], &s.chain.version[at + 1], (s.chain.depth - at - 1) * sizeof(u32));
        s.chain.depth--;
    }
    for (u32 r = 0; r < 128; r++)
        if (s.held[r] != NO_VERSION && held_version(s.held[r]) == v)
            s.held[r] = NO_VERSION;
}

// EXEC becomes version `v`, a subset of the current one (in whole-wave code: of every lane).
static void narrow(ExecState& s, u32 v) {
    bool inside = !s.chain.all;
    drop_version(s, v);
    if (!inside)
        s.chain.depth = s.chain.depth && s.chain.version[0] == ALL_VERSION ? 1 : 0;
    s.chain.all = false;
    if (s.chain.depth == CHAIN_MAX) {
        memmove(&s.chain.version[0], &s.chain.version[1], (CHAIN_MAX - 1) * sizeof(u32));
        s.chain.depth--;
    }
    s.chain.version[s.chain.depth++] = v;
}

// EXEC becomes version `v`, related to none before (but inside every lane).
static void restart(ExecState& s, u32 v) {
    drop_version(s, v);
    s.chain.all = false;
    s.chain.depth = s.chain.depth && s.chain.version[0] == ALL_VERSION ? 1 : 0;
    s.chain.version[s.chain.depth++] = v;
}

// EXEC goes back to version `v` of the chain.
static void return_to(ExecState& s, u32 at) {
    s.chain.depth = (u8)(at + 1);
    s.chain.all = false;
}

static bool is_exec_operand(const RdnaOperand& o) {
    return o.kind == OPND_SPECIAL && o.reg == SRC_EXEC_LO;
}

static bool is_minus_one(const RdnaOperand& o) {
    return o.kind == OPND_INLINE && o.reg == 193;
}

static bool is_mask_register(const RdnaOperand& o) {
    return o.kind == OPND_SGPR || (o.kind == OPND_SPECIAL && o.reg == SRC_VCC_LO);
}

static u32 held_of(const Translator& t, const ExecState& s, const RdnaOperand& o) {
    if (!is_mask_register(o))
        return NO_VERSION;
    u32 reg = o.reg & 127;
    u32 v = s.held[reg];
    if (!t.wave32 && (reg + 1 >= 128 || s.held[reg + 1] != v))
        return NO_VERSION;
    return v;
}

static void set_held(const Translator& t, ExecState& s, const RdnaOperand& o, u32 v) {
    if (!is_mask_register(o))
        return;
    u32 reg = o.reg & 127;
    s.held[reg] = v;
    if (!t.wave32 && reg + 1 < 128)
        s.held[reg + 1] = v;
}

static bool writes_exec_register(const RdnaInst& in) {
    for (u32 d = 0; d < 2; d++) {
        const RdnaOperand& o = in.dst[d];
        if (o.kind == OPND_SPECIAL && (o.reg == SRC_EXEC_LO || o.reg == SRC_EXEC_HI))
            return true;
    }
    return false;
}

static bool is_cmpx(const RdnaInst& in) {
    return rdna_op_encoding(in.op) == ENC_VOPC && strstr(rdna_op_name(in.op), "_cmpx_") != nullptr;
}

static bool is_saveexec(RdnaOp op) {
    return (op >= OP_S_AND_SAVEEXEC_B64 && op <= OP_S_XNOR_SAVEEXEC_B64) ||
           (op >= OP_S_ANDN1_SAVEEXEC_B64 && op <= OP_S_ANDN2_WREXEC_B64) ||
           (op >= OP_S_AND_SAVEEXEC_B32 && op <= OP_S_ANDN2_WREXEC_B32);
}

static bool is_scalar_alu(const RdnaInst& in) {
    RdnaEncoding base = rdna_op_encoding(in.op);
    return base == ENC_SOP1 || base == ENC_SOP2 || base == ENC_SOPK || base == ENC_SOPC || base == ENC_SOPP;
}

// The masking forms of this wave size: 64-bit operations for wave64, 32-bit ones for wave32.
static bool mask_op(const Translator& t, RdnaOp op, RdnaOp op64, RdnaOp op32) {
    return op == (t.wave32 ? op32 : op64);
}

static u32 inside(u32 held) {
    return held == NO_VERSION ? NO_VERSION : held | HELD_INSIDE;
}

// What an instruction leaves in its scalar destination (see held_of), from the state before it.
static u32 result_held(const Translator& t, const ExecState& s, const RdnaInst& in, u32 cur) {
    const RdnaOperand& a = in.src[0];
    const RdnaOperand& b = in.src[1];
    if (mask_op(t, in.op, OP_S_AND_SAVEEXEC_B64, OP_S_AND_SAVEEXEC_B32) ||
        mask_op(t, in.op, OP_S_OR_SAVEEXEC_B64, OP_S_OR_SAVEEXEC_B32))
        return cur;
    if (mask_op(t, in.op, OP_S_MOV_B64, OP_S_MOV_B32))
        return is_exec_operand(a) ? cur : held_of(t, s, a);
    if (mask_op(t, in.op, OP_S_AND_B64, OP_S_AND_B32)) {
        if (is_exec_operand(a) || is_exec_operand(b))
            return inside(cur);
        u32 v = held_of(t, s, a);
        return inside(v != NO_VERSION ? v : held_of(t, s, b));
    }
    if (mask_op(t, in.op, OP_S_ANDN2_B64, OP_S_ANDN2_B32))
        return is_exec_operand(a) ? inside(cur) : inside(held_of(t, s, a));
    // Compares write the running lanes' results and clear the others.
    if (rdna_op_encoding(in.op) == ENC_VOPC && !is_cmpx(in))
        return inside(cur);
    return NO_VERSION;
}

// EXEC becomes what `held` says: a version of the chain again, or a mask inside one. False if it names
// none of the chain.
static bool set_exec_from(ExecState& s, u32 held, u32 made) {
    u32 at;
    if (held == NO_VERSION || !chain_has(s.chain, held_version(held), &at))
        return false;
    return_to(s, at);
    if (held & HELD_INSIDE)
        narrow(s, made);
    return true;
}

// Moves the EXEC state over one instruction (which is instruction `index`).
static void exec_transfer(const Translator& t, ExecState& s, const RdnaInst& in, u32 index) {
    u32 made = FIRST_MADE + index;
    u32 cur = current(s.chain);
    // A call: nothing is known about what it leaves.
    if (in.op == OP_S_SWAPPC_B64 || in.op == OP_S_CALL_B64) {
        for (u32 r = 0; r < 128; r++)
            s.held[r] = NO_VERSION;
        restart(s, made);
        return;
    }
    // What EXEC's sources hold, before the destinations are written.
    const RdnaOperand& a = in.src[0];
    const RdnaOperand& b = in.src[1];
    u32 held_a = in.num_src > 0 ? held_of(t, s, a) : NO_VERSION;
    u32 held_b = in.num_src > 1 ? held_of(t, s, b) : NO_VERSION;
    u32 result = result_held(t, s, in, cur);
    // Destination SGPRs lose what they held (VCC too: vector instructions write it without naming it).
    for (u32 d = 0; d < 2; d++) {
        const RdnaOperand& o = in.dst[d];
        if (o.kind != OPND_SGPR && !(o.kind == OPND_SPECIAL && o.reg < 128))
            continue;
        for (u32 k = 0; k < (o.count ? o.count : 1u); k++)
            s.held[(o.reg + k) & 127] = NO_VERSION;
    }
    if (!is_scalar_alu(in)) {
        s.held[SRC_VCC_LO] = NO_VERSION;
        s.held[SRC_VCC_HI] = NO_VERSION;
    }
    bool exec_dst = writes_exec_register(in);
    if (result != NO_VERSION && !exec_dst)
        set_held(t, s, in.dst[0], result);
    if (!exec_dst && !is_saveexec(in.op) && !is_cmpx(in))
        return;

    // A whole EXEC write of this wave size, or anything else (a dword of a wave64 EXEC, other forms).
    const RdnaOperand& d = in.dst[0];
    bool whole =
        is_saveexec(in.op) || is_cmpx(in) || (is_exec_operand(d) && (t.wave32 ? d.count <= 1 : d.count == 2));
    bool exec_a = is_exec_operand(a), exec_b = in.num_src > 1 && is_exec_operand(b);
    if (!whole) {
        restart(s, made);
    } else if (mask_op(t, in.op, OP_S_AND_SAVEEXEC_B64, OP_S_AND_SAVEEXEC_B32) || is_cmpx(in)) {
        narrow(s, made); // EXEC & source
    } else if (mask_op(t, in.op, OP_S_OR_SAVEEXEC_B64, OP_S_OR_SAVEEXEC_B32) && is_minus_one(a)) {
        s.chain.all = true;
    } else if (mask_op(t, in.op, OP_S_MOV_B64, OP_S_MOV_B32)) {
        if (is_minus_one(a))
            s.chain.all = true;
        else if (!set_exec_from(s, held_a, made))
            restart(s, made);
    } else if (mask_op(t, in.op, OP_S_OR_B64, OP_S_OR_B32) && (exec_a || exec_b)) {
        // EXEC | (a mask inside a version EXEC is inside of) is inside that version; | a copy is it.
        if (!s.chain.all && !set_exec_from(s, exec_a ? held_b : held_a, made))
            restart(s, made);
    } else if (mask_op(t, in.op, OP_S_AND_B64, OP_S_AND_B32)) {
        if (exec_a || exec_b)
            narrow(s, made);
        else if (!set_exec_from(s, inside(held_a != NO_VERSION ? held_a : held_b), made))
            restart(s, made);
    } else if (mask_op(t, in.op, OP_S_ANDN2_B64, OP_S_ANDN2_B32)) {
        // EXEC & ~x is inside EXEC; saved & ~EXEC is inside the saved version (the other side of an if).
        if (exec_a)
            narrow(s, made);
        else if (!set_exec_from(s, inside(held_a), made))
            restart(s, made);
    } else {
        restart(s, made);
    }
}

// What an SGPR holding `held` on a path with chain `own` holds as far as the joined chain `joined_chain`
// can tell: the same, or a mask inside the innermost version of the joined chain that holds it (the
// join's own version is a new instance: the path's one is a different mask).
static u32 held_after_join(const ExecChain& own, const ExecChain& joined_chain, u32 held, u32 joined) {
    if (held == NO_VERSION)
        return NO_VERSION;
    u32 v = held_version(held), at;
    if (v != joined && chain_has(joined_chain, v))
        return held;
    if (!chain_has(own, v, &at))
        return NO_VERSION;
    while (at-- > 0)
        if (own.version[at] != joined && chain_has(joined_chain, own.version[at]))
            return own.version[at] | HELD_INSIDE;
    return NO_VERSION;
}

// What an SGPR holds after a join: the same on both paths, or a mask inside the outer of the versions
// each path's value is inside of.
static u32 merged_held(const ExecChain& c, const ExecChain& own_a, u32 a, const ExecChain& own_b, u32 b,
                       u32 joined) {
    a = held_after_join(own_a, c, a, joined);
    b = held_after_join(own_b, c, b, joined);
    if (a == NO_VERSION || b == NO_VERSION)
        return NO_VERSION;
    if (a == b)
        return a;
    u32 at_a, at_b;
    chain_has(c, held_version(a), &at_a);
    chain_has(c, held_version(b), &at_b);
    return (at_a < at_b ? held_version(a) : held_version(b)) | HELD_INSIDE;
}

// Joins the EXEC state of another path into a block's entry state. Returns whether it changed.
static bool exec_merge(ExecState& into, const ExecState& from, u32 block) {
    ExecState old = into;
    u32 joined = JOIN_VERSION | block;
    bool same_exec = into.chain.all == from.chain.all && current(into.chain) == current(from.chain) &&
                     (into.chain.all || current(into.chain) != NO_VERSION);
    if (same_exec && !into.chain.all) {
        // The same version on both paths: it stays, inside the versions both chains share.
        u32 top = current(into.chain);
        u32 n = 0;
        while (n + 1 < into.chain.depth && n + 1 < from.chain.depth &&
               into.chain.version[n] == from.chain.version[n])
            n++;
        into.chain.version[n] = top;
        into.chain.depth = (u8)(n + 1);
    } else if (same_exec) {
        u32 n = 0;
        while (n < into.chain.depth && n < from.chain.depth && into.chain.version[n] == from.chain.version[n])
            n++;
        into.chain.depth = (u8)n;
    } else if (into.chain.all || from.chain.all) {
        // Every lane on one path, some on the other: a version of its own, inside every lane only.
        bool both_all = into.chain.depth && into.chain.version[0] == ALL_VERSION && from.chain.depth &&
                        from.chain.version[0] == ALL_VERSION;
        into.chain.all = false;
        into.chain.depth = 0;
        if (both_all)
            into.chain.version[into.chain.depth++] = ALL_VERSION;
        into.chain.version[into.chain.depth++] = joined;
    } else {
        // Different versions: one of the join's own, inside what both chains share (its previous
        // instance, which a loop may bring around, ends the shared part).
        u32 into_depth = into.chain.depth;
        if (into_depth && into.chain.version[into_depth - 1] == joined)
            into_depth--;
        u32 n = 0;
        while (n < into_depth && n < from.chain.depth && n + 1 < CHAIN_MAX &&
               into.chain.version[n] == from.chain.version[n] && from.chain.version[n] != joined)
            n++;
        into.chain.version[n] = joined;
        into.chain.depth = (u8)(n + 1);
    }
    for (u32 r = 0; r < 128; r++)
        into.held[r] = merged_held(into.chain, old.chain, into.held[r], from.chain, from.held[r], joined);
    return !same_chain(old.chain, into.chain) || memcmp(old.held, into.held, sizeof(old.held)) != 0;
}

// --- what instructions read and write ----------------------------------------------------------------

// How an instruction writes its vector destination.
enum VectorWrite : u8 {
    WRITE_NONE,    // no vector destination
    WRITE_REPLACE, // every bit of the destination registers, in the lanes running
    WRITE_KEEP,    // part of the bits (or reads the destination): the old value is read too
    WRITE_OTHER,   // anything else: not modelled as a write, the destination counts as read
};

static bool is_vector_alu(const RdnaInst& in) {
    RdnaEncoding base = rdna_op_encoding(in.op);
    return base == ENC_VOP1 || base == ENC_VOP2 || base == ENC_VOP3 || base == ENC_VOP3P;
}

// Memory loads that write every destination register outright: not 16-bit halves (d16), packed image
// loads, loads with a status register (TFE/LWE) or into LDS.
static bool loads_whole_destination(const RdnaInst& in) {
    const char* name = rdna_op_name(in.op);
    if (strstr(name, "d16") || strstr(name, "pck") || in.mem.tfe || in.mem.lwe || in.mem.lds)
        return false;
    switch (rdna_op_encoding(in.op)) {
    case ENC_DS:
        return strncmp(name, "ds_read", 7) == 0;
    case ENC_MUBUF:
    case ENC_MTBUF:
    case ENC_FLAT:
    case ENC_GLOBAL:
        return strstr(name, "_load_") != nullptr;
    case ENC_MIMG:
        return !in.mem.d16 && strncmp(name, "image_store", 11) != 0 && strncmp(name, "image_atomic", 12) != 0;
    default:
        return false;
    }
}

static VectorWrite vector_write(const Translator& t, const RdnaInst& in) {
    const RdnaOperand& d = in.dst[0];
    if (d.kind != OPND_VGPR)
        return WRITE_NONE;
    if (t.spill_slot[d.reg & 255] != 0xFF)
        return WRITE_OTHER;
    if (!is_vector_alu(in))
        return loads_whole_destination(in) ? WRITE_REPLACE : WRITE_OTHER;
    if (in.has_dpp)
        return WRITE_OTHER;
    switch (in.op) {
    case OP_V_WRITELANE_B32:
    case OP_V_SWAP_B32:
    case OP_V_SWAPREL_B32:
    case OP_V_PERMLANE16_B32:
    case OP_V_PERMLANEX16_B32:
    case OP_V_MOVRELD_B32:
    case OP_V_MOVRELS_B32:
    case OP_V_MOVRELSD_B32:
    case OP_V_MOVRELSD_2_B32:
        return WRITE_OTHER;
    default:
        break;
    }
    // Accumulators, 16-bit results (half writes, keeping the other half) and mixed precision FMAs.
    const char* name = rdna_op_name(in.op);
    if (strstr(name, "mac") || strstr(name, "mix") || strstr(name, "16"))
        return WRITE_KEEP;
    if (in.enc == ENC_VOP3 && (in.op_sel & 8))
        return WRITE_KEEP;
    if (in.has_sdwa && in.sdwa.dst_sel != SDWA_DWORD && in.sdwa.dst_unused == 2)
        return WRITE_KEEP;
    return WRITE_REPLACE;
}

// Reads that may see other lanes than the reader's own, or lanes outside EXEC: DPP, lane reads and
// permutes, exports, and image sampling and LOD queries with an implicit LOD (derivatives come from
// the neighbouring lanes).
static bool reads_any_lane(const Translator& t, const RdnaInst& in) {
    if (in.has_dpp || in.enc == ENC_EXP)
        return true;
    switch (in.op) {
    case OP_V_READLANE_B32:
    case OP_V_PERMLANE16_B32:
    case OP_V_PERMLANEX16_B32:
    case OP_DS_SWIZZLE_B32:
    case OP_DS_PERMUTE_B32:
    case OP_DS_BPERMUTE_B32:
        return true;
    default:
        break;
    }
    return rdna_op_encoding(in.op) == ENC_MIMG && image_reads_neighbours(t, in.op);
}

// Calls: code the analysis doesn't see, which may read any register.
static bool leaves_the_program(const RdnaInst& in) {
    return in.op == OP_S_SWAPPC_B64 || in.op == OP_S_CALL_B64;
}

static bool changes_exec(const RdnaInst& in) {
    return writes_exec_register(in) || is_saveexec(in.op) || is_cmpx(in) || leaves_the_program(in);
}

static void mark_range(u8* regs, u32 first, u32 count) {
    for (u32 k = 0; k < count; k++)
        regs[(first + k) & 255] = 1;
}

// The vector registers an instruction reads (destinations counted where vector_write says so).
static void vector_reads(const RdnaInst& in, VectorWrite write, u8* regs) {
    for (u32 j = 0; j < in.num_src; j++)
        if (in.src[j].kind == OPND_VGPR)
            mark_range(regs, in.src[j].reg, in.src[j].count ? in.src[j].count : 1u);
    if (rdna_op_encoding(in.op) == ENC_MIMG) {
        if (in.mem.nsa_count)
            for (u32 k = 0; k < in.mem.nsa_count && k < sizeof(in.mem.nsa_addr); k++)
                regs[in.mem.nsa_addr[k]] = 1;
        else if (in.src[0].kind == OPND_VGPR)
            mark_range(regs, in.src[0].reg, rdna_image_address_count(in.op, in.mem.dim));
    }
    // Stores name their data as the destination; partial writes keep part of it.
    if (in.dst[0].kind == OPND_VGPR && (write == WRITE_KEEP || write == WRITE_OTHER))
        mark_range(regs, in.dst[0].reg, in.dst[0].count ? in.dst[0].count : 1u);
    if (in.dst[1].kind == OPND_VGPR)
        mark_range(regs, in.dst[1].reg, in.dst[1].count ? in.dst[1].count : 1u);
}

// --- the analysis ------------------------------------------------------------------------------------

struct LaneAnalysis {
    ExecState* entry;  // per block: EXEC state where it starts
    bool* reached;     // per block
    ExecChain* chains; // snapshots of the chain
    u32 chain_count;
    u32* chain_before; // per instruction: its snapshots before and after it
    u32* chain_after;
    u32 (*live)[256]; // per block: liveness where it starts
};

// Two liveness values at a point with chain `c`: the wider.
static u32 live_join(u32 a, u32 b, const ExecChain& c) {
    if (a == DEAD || a == b)
        return b;
    if (b == DEAD)
        return a;
    if (a == ANY_LANE || b == ANY_LANE)
        return ANY_LANE;
    u32 ia, ib;
    if (!chain_has(c, a, &ia) || !chain_has(c, b, &ib))
        return ANY_LANE;
    return ia < ib ? a : b;
}

// A liveness value at the start of block `next`, seen at the end of a block whose chain is `c`: the
// join's own version is the EXEC this path brings.
static u32 live_on_edge(u32 v, u32 next, const ExecChain& c) {
    if (v == DEAD || v == ANY_LANE)
        return v;
    if (v == (JOIN_VERSION | next))
        return current(c) == NO_VERSION ? ANY_LANE : current(c);
    return chain_has(c, v) ? v : ANY_LANE;
}

// Moves liveness `live` backward over instruction `i`. With `decide`, records whether its vector write
// may write every lane.
static void live_transfer(Translator& t, const LaneAnalysis& a, u32 i, u32* live, bool decide) {
    const RdnaInst& in = t.program.insts[i];
    const ExecChain& before = a.chains[a.chain_before[i]];
    const ExecChain& after = a.chains[a.chain_after[i]];
    if (leaves_the_program(in)) {
        for (u32 r = 0; r < 256; r++)
            live[r] = ANY_LANE;
        return;
    }
    if (changes_exec(in)) {
        u32 made = FIRST_MADE + i;
        for (u32 r = 0; r < 256; r++) {
            u32 v = live[r];
            if (v == DEAD || v == ANY_LANE)
                continue;
            if (v == made) {
                // Within the version made here: within the one it narrowed, if it did.
                bool narrowed = !after.all && after.depth >= 2 && after.version[after.depth - 1] == made;
                u32 parent = narrowed ? after.version[after.depth - 2] : NO_VERSION;
                live[r] = narrowed && chain_has(before, parent) ? parent : ANY_LANE;
            } else if (!chain_has(before, v)) {
                live[r] = ANY_LANE;
            }
        }
    }
    u32 cur = current(before);
    VectorWrite write = vector_write(t, in);
    if (write == WRITE_REPLACE || write == WRITE_KEEP) {
        const RdnaOperand& d = in.dst[0];
        u32 count = d.count ? d.count : 1u;
        bool every_lane = true;
        for (u32 k = 0; k < count; k++) {
            u32 r = (d.reg + k) & 255;
            bool unseen = before.all || cur == ALL_VERSION || live[r] == DEAD || live[r] == cur;
            every_lane = every_lane && unseen;
            // Its lanes outside EXEC are never seen again: the old value is dead, written there or not.
            if (unseen)
                live[r] = DEAD;
        }
        if (decide)
            t.lane_write[i] = every_lane ? 1 : 0;
    }
    u8 regs[256] = {};
    vector_reads(in, write, regs);
    u32 seen = reads_any_lane(t, in) || cur == NO_VERSION ? ANY_LANE : cur;
    for (u32 r = 0; r < 256; r++)
        if (regs[r])
            live[r] = live_join(live[r], seen, before);
}

static u32 add_chain(LaneAnalysis& a, const ExecChain& c) {
    if (a.chain_count && same_chain(a.chains[a.chain_count - 1], c))
        return a.chain_count - 1;
    a.chains[a.chain_count] = c;
    return a.chain_count++;
}

static bool forward(const Translator& t, LaneAnalysis& a) {
    const Program& p = t.program;
    ExecState& start = a.entry[0];
    memset(&start, 0, sizeof(start));
    bool whole_waves = t.exact && t.wave_threads % (t.wave32 ? 32u : 64u) == 0;
    start.chain.version[0] = whole_waves ? ALL_VERSION : START_VERSION;
    start.chain.depth = 1;
    a.reached[0] = true;
    ExecState* s = (ExecState*)malloc(sizeof(ExecState));
    if (!s)
        return false;
    // Joins only shorten chains, turn versions into the join's own and forget copies: this settles.
    for (bool changed = true; changed;) {
        changed = false;
        for (u32 b = 0; b < p.block_count; b++) {
            if (!a.reached[b])
                continue;
            *s = a.entry[b];
            for (u32 i = p.blocks[b].first; i < p.blocks[b].end; i++)
                exec_transfer(t, *s, p.insts[i], i);
            for (u32 k = 0; k < 2; k++) {
                s32 n = p.blocks[b].next[k];
                if (n < 0)
                    continue;
                if (!a.reached[n]) {
                    a.entry[n] = *s;
                    a.reached[n] = true;
                    changed = true;
                } else if (exec_merge(a.entry[n], *s, (u32)n)) {
                    changed = true;
                }
            }
        }
    }
    // Snapshots of the chain around every instruction.
    for (u32 b = 0; b < p.block_count; b++) {
        if (!a.reached[b])
            continue;
        *s = a.entry[b];
        u32 at = add_chain(a, s->chain);
        for (u32 i = p.blocks[b].first; i < p.blocks[b].end; i++) {
            a.chain_before[i] = at;
            exec_transfer(t, *s, p.insts[i], i);
            if (changes_exec(p.insts[i]))
                at = add_chain(a, s->chain);
            a.chain_after[i] = at;
        }
    }
    free(s);
    return true;
}

// Liveness where block `b` ends: what its successors need, or nothing after the program's end.
static void live_out(const Translator& t, const LaneAnalysis& a, u32 b, u32* live) {
    const Program& p = t.program;
    const Block& blk = p.blocks[b];
    const ExecChain& c = a.chains[a.chain_after[blk.end - 1]];
    bool any_next = false;
    for (u32 r = 0; r < 256; r++)
        live[r] = DEAD;
    for (u32 k = 0; k < 2; k++) {
        s32 n = blk.next[k];
        if (n < 0 || !a.reached[n])
            continue;
        any_next = true;
        for (u32 r = 0; r < 256; r++)
            live[r] = live_join(live[r], live_on_edge(a.live[n][r], (u32)n, c), c);
    }
    RdnaOp last = p.insts[blk.end - 1].op;
    bool ends = last == OP_S_ENDPGM || last == OP_S_ENDPGM_SAVED || last == OP_S_ENDPGM_ORDERED_PS_DONE;
    if (!any_next && !ends)
        for (u32 r = 0; r < 256; r++)
            live[r] = ANY_LANE; // runs off somewhere the analysis can't see
}

static bool backward(Translator& t, LaneAnalysis& a) {
    const Program& p = t.program;
    u32 live[256];
    // Liveness only widens, through a lattice a few chain entries high: a handful of rounds settle it.
    for (u32 round = 0;; round++) {
        if (round == 64)
            return false;
        bool changed = false;
        for (u32 b = p.block_count; b-- > 0;) {
            if (!a.reached[b])
                continue;
            live_out(t, a, b, live);
            for (u32 i = p.blocks[b].end; i-- > p.blocks[b].first;)
                live_transfer(t, a, i, live, false);
            if (memcmp(live, a.live[b], sizeof(live)) != 0) {
                memcpy(a.live[b], live, sizeof(live));
                changed = true;
            }
        }
        if (!changed)
            break;
    }
    for (u32 b = 0; b < p.block_count; b++) {
        if (!a.reached[b])
            continue;
        live_out(t, a, b, live);
        for (u32 i = p.blocks[b].end; i-- > p.blocks[b].first;)
            live_transfer(t, a, i, live, true);
    }
    return true;
}

bool analyze_lane_writes(Translator& t) {
    const Program& p = t.program;
    t.lane_write = (u8*)calloc(p.count + 1, 1);
    if (!t.lane_write)
        return false;
    if (t.keep_exec_guards)
        return true;
    for (u32 i = 0; i < p.count; i++)
        if (picks_registers_at_run_time(p.insts[i].op))
            return true; // any register may be read anywhere: every write keeps its guard
    LaneAnalysis a = {};
    u32 snapshots = p.block_count + 1;
    for (u32 i = 0; i < p.count; i++)
        snapshots += changes_exec(p.insts[i]) ? 1u : 0u;
    a.entry = (ExecState*)calloc(p.block_count, sizeof(ExecState));
    a.reached = (bool*)calloc(p.block_count, 1);
    a.chains = (ExecChain*)calloc(snapshots, sizeof(ExecChain));
    a.chain_before = (u32*)calloc(p.count, sizeof(u32));
    a.chain_after = (u32*)calloc(p.count, sizeof(u32));
    a.live = (u32(*)[256])calloc(p.block_count, sizeof(u32[256]));
    bool ok = a.entry && a.reached && a.chains && a.chain_before && a.chain_after && a.live;
    if (ok)
        ok = forward(t, a);
    if (ok && !backward(t, a))
        memset(t.lane_write, 0, p.count + 1); // didn't settle: every write keeps its guard
    free(a.entry);
    free(a.reached);
    free(a.chains);
    free(a.chain_before);
    free(a.chain_after);
    free(a.live);
    return ok;
}

bool lane_write_replaces(const Translator& t, const RdnaInst& in) {
    return vector_write(t, in) == WRITE_REPLACE;
}

bool lane_write_complete(const Translator& t, const RdnaInst& in, u32 written) {
    VectorWrite write = vector_write(t, in);
    if (write != WRITE_REPLACE && write != WRITE_KEEP)
        return true;
    u32 count = in.dst[0].count ? in.dst[0].count : 1u;
    return count < 32 && written == (1u << count) - 1;
}

