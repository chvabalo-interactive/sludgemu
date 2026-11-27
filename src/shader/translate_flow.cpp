// Console shader translation: control flow. A program is a graph of blocks, each ending in a jump on a
// wave-uniform condition (SCC, VCCZ, EXECZ), in an unconditional jump or in the program's end. The
// graph is emitted as nested if/else, loops and labelled exits rebuilt from its dominator tree, the way
// "Beyond Relooper" (N. Ramsey, ICFP 2022) rebuilds structured control flow:
// - a block reached by one forward edge is emitted in line, where that edge leaves the block before it;
// - a block reached by several (a merge point), or by an edge leaving a loop, is labelled: a
//   `switch (0u) { default: ... }` goes around the code of the block that dominates it, a jump to it is
//   a `break`, and its own code follows the switch. Labels of blocks outside a loop go around the loop,
//   so that its exits are `break`s too;
// - a loop header wraps its code and the labels inside the loop in `[loop] for (;;)`: a jump back to
//   it is `continue`, and so is falling off the end of the body;
// - a jump to where control goes anyway (the code right after) is left out, and so is a label nothing
//   breaks to.
// A jump that has to leave more than the innermost loop or label sets `go_` and breaks; each statement
// it leaves passes it on. Conditions are evaluated where the blocks end, as with the dispatcher, so the
// lanes of a wave all go the same way. Graphs that are not reducible (a loop entered at more than one
// block), which compilers don't produce, fall back to the dispatcher: a loop over a switch on the block
// number. So do graphs with a loop that nothing leaves, which the desktop compiler rejects.

#include <stdlib.h>
#include <string.h>

#include "shader/translate_internal.h"

#define FLOW_NONE 0xFFFFFFFFu
#define FLOW_EXIT 0xFFFFFFFEu // the program's end as a jump target
// Loops, labels and arms open at once, and blocks emitted in line inside each other. Deeper programs
// fall back to the dispatcher: the desktop compiler takes 256 levels of brackets, and instructions add
// a few levels of their own.
#define FLOW_MAX_FRAMES  160
#define FLOW_MAX_NESTING 256

// --- the graph ---------------------------------------------------------------------------------------

struct Flow {
    u32 count;    // blocks
    u32 reached;  // blocks the program can get to, all in `rpo`
    u32* rpo;     // reached blocks in reverse postorder: each before the blocks it jumps forward to
    u32* order;   // each block's place in `rpo` (FLOW_NONE: never reached)
    u32* pred_at; // the predecessors of block b: preds[pred_at[b] .. pred_at[b + 1])
    u32* preds;
    u32* idom;        // immediate dominator (the first block: itself)
    u32* loop;        // the innermost loop holding the block, by header, or FLOW_NONE
    u32* loop_parent; // headers: the loop around theirs, or FLOW_NONE
    bool* header;
    bool* leaves;     // headers: some edge leaves their loop
    bool* labeled;    // reached through a label (see the top of the file)
    u32* place;       // labeled blocks: the block whose code their label goes around...
    bool* after_loop; // ... or whose loop, for blocks outside it
    u32* child_at;    // the labels placed at block b: children[child_at[b] .. child_at[b + 1]), those
    u32* children;    // around its loop first, then the others, each part in reverse postorder
    bool* wrapped;    // labeled blocks some jump breaks to (the others need no switch)
    bool* flatten;    // blocks whose jump to where control goes anyway is written out (see flatten)
    bool* emitted;
};

static void flow_release(Flow& f) {
    free(f.rpo);
    free(f.order);
    free(f.pred_at);
    free(f.preds);
    free(f.idom);
    free(f.loop);
    free(f.loop_parent);
    free(f.header);
    free(f.leaves);
    free(f.labeled);
    free(f.place);
    free(f.after_loop);
    free(f.child_at);
    free(f.children);
    free(f.wrapped);
    free(f.flatten);
    free(f.emitted);
}

static bool flow_alloc(Flow& f, u32 count) {
    memset(&f, 0, sizeof(f));
    f.count = count;
    usize n = count + 1;
    f.rpo = (u32*)calloc(n, sizeof(u32));
    f.order = (u32*)calloc(n, sizeof(u32));
    f.pred_at = (u32*)calloc(n + 1, sizeof(u32));
    f.preds = (u32*)calloc(2 * n, sizeof(u32));
    f.idom = (u32*)calloc(n, sizeof(u32));
    f.loop = (u32*)calloc(n, sizeof(u32));
    f.loop_parent = (u32*)calloc(n, sizeof(u32));
    f.header = (bool*)calloc(n, 1);
    f.leaves = (bool*)calloc(n, 1);
    f.labeled = (bool*)calloc(n, 1);
    f.place = (u32*)calloc(n, sizeof(u32));
    f.after_loop = (bool*)calloc(n, 1);
    f.child_at = (u32*)calloc(n + 1, sizeof(u32));
    f.children = (u32*)calloc(n, sizeof(u32));
    f.wrapped = (bool*)calloc(n, 1);
    f.flatten = (bool*)calloc(n, 1);
    f.emitted = (bool*)calloc(n, 1);
    return f.rpo && f.order && f.pred_at && f.preds && f.idom && f.loop && f.loop_parent && f.header &&
           f.leaves && f.labeled && f.place && f.after_loop && f.child_at && f.children && f.wrapped &&
           f.flatten && f.emitted;
}

// Block b's successors other than the program's end: up to two, a jump taken both ways counted once.
static u32 successors(const Program& p, u32 b, u32 out[2]) {
    const Block& blk = p.blocks[b];
    u32 n = 0;
    if (blk.next[0] >= 0)
        out[n++] = (u32)blk.next[0];
    if (blk.next[1] >= 0 && blk.next[1] != blk.next[0])
        out[n++] = (u32)blk.next[1];
    return n;
}

// Depth-first from the first block. Of two successors the later one is visited first, so that the
// reverse postorder keeps the program's own order where it can.
static bool flow_order(Flow& f, const Program& p) {
    u32* stack = (u32*)malloc(sizeof(u32) * f.count);
    u8* step = (u8*)calloc(f.count, 1);
    u32* post = f.order; // postorder first, in the order array's space
    if (!stack || !step) {
        free(stack);
        free(step);
        return false;
    }
    u32 depth = 0, done = 0;
    stack[depth++] = 0;
    step[0] = 1;
    while (depth) {
        u32 b = stack[depth - 1];
        u32 succ[2];
        u32 n = successors(p, b, succ);
        if (n == 2 && succ[0] < succ[1]) {
            u32 later = succ[1];
            succ[1] = succ[0];
            succ[0] = later;
        }
        u32 k = step[b] - 1u;
        if (k >= n) {
            depth--;
            post[done++] = b;
            continue;
        }
        step[b]++;
        u32 s = succ[k];
        if (!step[s]) {
            step[s] = 1;
            stack[depth++] = s;
        }
    }
    f.reached = done;
    for (u32 i = 0; i < done; i++)
        f.rpo[i] = post[done - 1 - i];
    for (u32 b = 0; b < f.count; b++)
        f.order[b] = FLOW_NONE;
    for (u32 i = 0; i < done; i++)
        f.order[f.rpo[i]] = i;
    free(stack);
    free(step);
    return true;
}

static void flow_predecessors(Flow& f, const Program& p) {
    for (u32 i = 0; i < f.reached; i++) {
        u32 succ[2];
        u32 n = successors(p, f.rpo[i], succ);
        for (u32 k = 0; k < n; k++)
            f.pred_at[succ[k] + 1]++;
    }
    for (u32 b = 0; b < f.count; b++)
        f.pred_at[b + 1] += f.pred_at[b];
    u32* fill = f.child_at; // scratch until the children are placed
    memcpy(fill, f.pred_at, sizeof(u32) * f.count);
    for (u32 i = 0; i < f.reached; i++) {
        u32 b = f.rpo[i];
        u32 succ[2];
        u32 n = successors(p, b, succ);
        for (u32 k = 0; k < n; k++)
            f.preds[fill[succ[k]]++] = b;
    }
    memset(f.child_at, 0, sizeof(u32) * (f.count + 1));
}

// Dominators by the iterative algorithm of Cooper, Harvey and Kennedy ("A Simple, Fast Dominance
// Algorithm"), over the reverse postorder.
static void flow_dominators(Flow& f) {
    for (u32 b = 0; b < f.count; b++)
        f.idom[b] = FLOW_NONE;
    f.idom[0] = 0;
    for (bool changed = true; changed;) {
        changed = false;
        for (u32 i = 1; i < f.reached; i++) {
            u32 b = f.rpo[i];
            u32 dom = FLOW_NONE;
            for (u32 k = f.pred_at[b]; k < f.pred_at[b + 1]; k++) {
                u32 q = f.preds[k];
                if (f.idom[q] == FLOW_NONE)
                    continue;
                if (dom == FLOW_NONE) {
                    dom = q;
                    continue;
                }
                u32 a = q;
                while (a != dom) {
                    while (f.order[a] > f.order[dom])
                        a = f.idom[a];
                    while (f.order[dom] > f.order[a])
                        dom = f.idom[dom];
                }
            }
            if (f.idom[b] != dom) {
                f.idom[b] = dom;
                changed = true;
            }
        }
    }
}

static bool dominates(const Flow& f, u32 a, u32 b) {
    while (b != a && b != 0)
        b = f.idom[b];
    return b == a;
}

static bool is_back_edge(const Flow& f, u32 from, u32 to) {
    return f.order[to] <= f.order[from];
}

// Whether every edge going back in the reverse postorder goes to a block dominating where it starts:
// every loop has one entry, its header.
static bool flow_reducible(const Flow& f) {
    for (u32 b = 0; b < f.count; b++) {
        if (f.order[b] == FLOW_NONE)
            continue;
        for (u32 k = f.pred_at[b]; k < f.pred_at[b + 1]; k++) {
            u32 q = f.preds[k];
            if (is_back_edge(f, q, b) && !dominates(f, b, q))
                return false;
        }
    }
    return true;
}

static bool in_loop(const Flow& f, u32 b, u32 header) {
    for (u32 l = f.loop[b]; l != FLOW_NONE; l = f.loop_parent[l])
        if (l == header)
            return true;
    return false;
}

// Natural loops: a header and the blocks reaching a jump back to it without passing it. Headers come in
// reverse postorder, outer ones first, so each block ends up with its innermost loop.
static bool flow_loops(Flow& f) {
    u32* work = (u32*)malloc(sizeof(u32) * f.count);
    u32* mark = (u32*)malloc(sizeof(u32) * f.count);
    if (!work || !mark) {
        free(work);
        free(mark);
        return false;
    }
    for (u32 b = 0; b < f.count; b++) {
        f.loop[b] = f.loop_parent[b] = FLOW_NONE;
        mark[b] = FLOW_NONE;
    }
    for (u32 i = 0; i < f.reached; i++) {
        u32 h = f.rpo[i];
        bool loops = false;
        for (u32 k = f.pred_at[h]; k < f.pred_at[h + 1]; k++)
            loops = loops || is_back_edge(f, f.preds[k], h);
        if (!loops)
            continue;
        f.header[h] = true;
        f.loop_parent[h] = f.loop[h];
        f.loop[h] = h;
        // The walk stops at the header (marked first): what comes before it isn't in the loop.
        mark[h] = h;
        u32 n = 0;
        for (u32 k = f.pred_at[h]; k < f.pred_at[h + 1]; k++) {
            u32 q = f.preds[k];
            if (is_back_edge(f, q, h) && mark[q] != h) {
                mark[q] = h;
                work[n++] = q;
            }
        }
        while (n) {
            u32 b = work[--n];
            f.loop[b] = h;
            for (u32 k = f.pred_at[b]; k < f.pred_at[b + 1]; k++) {
                u32 q = f.preds[k];
                if (mark[q] != h) {
                    mark[q] = h;
                    work[n++] = q;
                }
            }
        }
    }
    free(work);
    free(mark);
    return true;
}

// Whether every loop has a way out: an edge leaving it, or the program's end. The desktop compiler
// rejects a loop without one (a console program that got there would never end, but may never get
// there), while the dispatcher's loop always has one.
static bool flow_loops_leave(Flow& f, const Program& p) {
    for (u32 i = 0; i < f.reached; i++) {
        u32 b = f.rpo[i];
        if (f.loop[b] == FLOW_NONE)
            continue;
        const Block& blk = p.blocks[b];
        for (u32 k = 0; k < 2; k++) {
            s32 next = blk.next[k];
            if (next < 0 && k == 1)
                continue; // no second way (a first one below zero is the program's end)
            // Loops nest: the first one around b that holds `next` holds it in all those around it too.
            for (u32 l = f.loop[b]; l != FLOW_NONE; l = f.loop_parent[l]) {
                if (next >= 0 && in_loop(f, (u32)next, l))
                    break;
                f.leaves[l] = true;
            }
        }
    }
    for (u32 i = 0; i < f.reached; i++)
        if (f.header[f.rpo[i]] && !f.leaves[f.rpo[i]])
            return false;
    return true;
}

// Which blocks get labels, and where the labels go: around the code of the block's immediate dominator,
// or, for a block outside loops that dominator is in, around the outermost of those loops.
static void flow_labels(Flow& f) {
    for (u32 i = 1; i < f.reached; i++) {
        u32 y = f.rpo[i];
        u32 forward = 0;
        bool exits = false;
        for (u32 k = f.pred_at[y]; k < f.pred_at[y + 1]; k++) {
            u32 q = f.preds[k];
            if (is_back_edge(f, q, y))
                continue;
            forward++;
            exits = exits || (f.loop[q] != FLOW_NONE && !in_loop(f, y, f.loop[q]));
        }
        f.labeled[y] = forward >= 2 || exits;
        if (!f.labeled[y])
            continue;
        u32 dom = f.idom[y];
        u32 outside = FLOW_NONE;
        for (u32 l = f.loop[dom]; l != FLOW_NONE && !in_loop(f, y, l); l = f.loop_parent[l])
            outside = l;
        f.place[y] = outside != FLOW_NONE ? outside : dom;
        f.after_loop[y] = outside != FLOW_NONE;
    }
    // How many labels each block gets (flow_children fills them in).
    for (u32 i = 1; i < f.reached; i++) {
        u32 y = f.rpo[i];
        if (f.labeled[y])
            f.child_at[f.place[y] + 1]++;
    }
    for (u32 b = 0; b < f.count; b++)
        f.child_at[b + 1] += f.child_at[b];
}

// Fills the children lists (counts already in child_at): two passes over the reverse postorder, the
// labels around loops first.
static bool flow_children(Flow& f) {
    u32* fill = (u32*)malloc(sizeof(u32) * (f.count + 1));
    if (!fill)
        return false;
    memcpy(fill, f.child_at, sizeof(u32) * (f.count + 1));
    for (u32 pass = 0; pass < 2; pass++)
        for (u32 i = 1; i < f.reached; i++) {
            u32 y = f.rpo[i];
            if (f.labeled[y] && f.after_loop[y] == (pass == 0))
                f.children[fill[f.place[y]]++] = y;
        }
    free(fill);
    return true;
}

// --- join conversions and the dispatcher -----------------------------------------------------------

#define PC_EXIT 0xFFFFFFFFu

// Scalar registers whose form changes where block `next` starts, converted on the way there (t.tags holds
// the state where the current block ends), where something may still read them:
// - loaded descriptor dwords, held as heap addresses here but values there (another way in holds a
//   value, see tags_merge): read from the shadow;
// - with exact lanes, masks here that are data there (see merge_tag): their bits.
// With `emitting` false, only whether there are any.
bool emit_join_conversions(Translator& t, s32 next, bool emitting) {
    if (next < 0)
        return false;
    const TagState& entry = t.block_entry[next];
    bool any = false;
    for (u32 r = 0; r < 128; r++) {
        if (!sgpr_live_in(t, (u32)next, r))
            continue;
        if (join_converts(t.tags.sgpr[r]) && !heap_loaded(entry.sgpr[r])) {
            any = true;
            if (emitting) {
                t.needs_descriptor_info = true;
                emit(t, "%s = g_descriptor_info.Load(%s);", sgpr(t, r), sgpr(t, r));
            }
        } else if (r != SRC_EXEC_LO && r != SRC_EXEC_HI && edge_converts(t, t.tags.sgpr[r], entry.sgpr[r])) {
            any = true;
            if (emitting)
                emit(t, "%s = mask_bits(%s);", sgpr(t, r), sgpr(t, r));
        }
    }
    for (u32 slot = 0; slot < t.spill_vgpr_count; slot++)
        for (u32 lane = 0; lane < 64; lane++) {
            if (!(t.spill_lanes[slot] & (1ull << lane)))
                continue;
            Tag out = t.tags.spill_tag[slot][lane], in = entry.spill_tag[slot][lane];
            if (join_converts(out) && !heap_loaded(in)) {
                any = true;
                if (emitting) {
                    t.needs_descriptor_info = true;
                    emit(t, "spill%u_%u = g_descriptor_info.Load(spill%u_%u);", slot, lane, slot, lane);
                }
            } else if (edge_converts(t, out, in)) {
                any = true;
                if (emitting)
                    emit(t, "spill%u_%u = mask_bits(spill%u_%u);", slot, lane, slot, lane);
            }
        }
    return any;
}

// The dispatcher: a loop over a switch on the block number. Branch conditions are uniform, so the whole
// wave always takes the same case.
static void emit_dispatcher(Translator& t) {
    const Program& p = t.program;
    emit(t, "uint pc = 0;");
    emit(t, "[loop] while (pc != 0x%08xu) {", PC_EXIT);
    t.depth++;
    emit(t, "switch (pc) {");
    for (u32 b = 0; b < p.block_count && !t.failed; b++) {
        // Blocks nothing jumps to (padding after a long branch's trampoline, say) get no case: no tags
        // reach them, and neither can the program.
        if (!t.block_reached[b])
            continue;
        emit(t, "case %u: {", b);
        t.depth++;
        emit_block(t, b);
        const Block& blk = p.blocks[b];
        const RdnaInst& last = p.insts[blk.end - 1];
        u32 next0 = blk.next[0] < 0 ? PC_EXIT : (u32)blk.next[0];
        bool convert0 = emit_join_conversions(t, blk.next[0], false);
        bool convert1 = emit_join_conversions(t, blk.next[1], false);
        if (blk.next[1] >= 0 && (convert0 || convert1)) {
            emit(t, "{ bool taken_ = %s;", branch_condition(t, last, false));
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
            emit(t, "pc = %s ? %uu : %uu;", branch_condition(t, last, false), next0, (u32)blk.next[1]);
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

// --- structured emission -----------------------------------------------------------------------------

enum FrameKind : u8 {
    FRAME_ROOT,  // the program: falling off its end ends it
    FRAME_LOOP,  // [loop] for (;;) around a header's code
    FRAME_LABEL, // a labeled block's switch (when something breaks to it), its code after
    FRAME_ARM,   // an arm of an if/else: falls off where the if does
};

struct Frame {
    FrameKind kind;
    bool breakable; // `break` leaves it: loops, and labels something breaks to
    u32 block;      // loops: the header; labels: the labeled block
};

enum JumpKind : u8 {
    JUMP_NONE,     // no way there: not a structure this emission knows
    JUMP_NATURAL,  // control goes there anyway
    JUMP_RETURN,   // the program's end
    JUMP_BREAK,    // out of the innermost loop or label
    JUMP_CONTINUE, // back to the innermost loop's header
    JUMP_FAR,      // further out: through `go_`
};

struct Jump {
    JumpKind kind;
    u32 code;   // JUMP_FAR: the value of go_ (see far_code)
    u32 holder; // JUMP_FAR: the frame the first break leaves
    u32 to;     // the frame that breaking out of arrives, or FLOW_NONE
};

struct Structurer {
    Translator* t;
    Flow* f;
    bool emitting; // pass 2: writing the program; pass 1 only decides which labels need a switch
    bool ok;
    const char* why; // what made it give up
    Frame frames[FLOW_MAX_FRAMES];
    u32 depth;
    u32 nesting;
    // Far jumps on their way out: (frame index << 16) | code, for each breakable frame they still have
    // to leave, handled where that frame closes.
    u32* pending;
    u32 pending_count;
    u32 pending_capacity;
};

// A far jump's go_ value: the frame it goes to, and whether it continues that frame's loop.
static u32 far_code(u32 frame, bool to_continue) {
    return frame * 2u + (to_continue ? 2u : 1u);
}

// Where control goes from the end of frame i: a loop's body continues, a label's switch goes to the
// labeled block's code, an arm goes wherever its if statement does, the program ends.
static u32 fall_target(const Structurer& s, u32 i) {
    for (;; i--) {
        const Frame& fr = s.frames[i];
        if (fr.kind == FRAME_ROOT)
            return FLOW_EXIT;
        if (fr.kind != FRAME_ARM)
            return fr.block;
    }
}

// Where `break` out of breakable frame i goes: past a loop (to where its statement falls), or to the
// labeled block.
static u32 break_target(const Structurer& s, u32 i) {
    return s.frames[i].kind == FRAME_LOOP ? fall_target(s, i - 1) : s.frames[i].block;
}

static void give_up(Structurer& s, const char* why) {
    if (s.ok)
        s.why = why;
    s.ok = false;
}

static bool push_frame(Structurer& s, FrameKind kind, bool breakable, u32 block) {
    if (s.depth == FLOW_MAX_FRAMES) {
        give_up(s, "nested too deep");
        return false;
    }
    s.frames[s.depth++] = {kind, breakable, block};
    return true;
}

static void innermost(const Structurer& s, u32 below, u32* breakable, u32* loop) {
    *breakable = *loop = FLOW_NONE;
    for (u32 i = below; i-- > 1;) {
        if (*breakable == FLOW_NONE && s.frames[i].breakable)
            *breakable = i;
        if (*loop == FLOW_NONE && s.frames[i].kind == FRAME_LOOP)
            *loop = i;
    }
}

// How a jump from the current position (the end of the innermost frame) to `target` is written. With
// `natural` false, also a jump to where control goes anyway. Kind JUMP_NONE: no way there.
static Jump find_jump(const Structurer& s, u32 target, bool natural) {
    Jump j = {JUMP_NATURAL, 0, 0, FLOW_NONE};
    if (natural && target == fall_target(s, s.depth - 1))
        return j;
    if (target == FLOW_EXIT) {
        j.kind = JUMP_RETURN;
        return j;
    }
    u32 inner, loop;
    innermost(s, s.depth, &inner, &loop);
    if (inner != FLOW_NONE && break_target(s, inner) == target) {
        j.kind = JUMP_BREAK;
        j.to = inner;
        return j;
    }
    // `continue` goes through labels' switches to the loop around them.
    if (loop != FLOW_NONE && s.frames[loop].block == target) {
        j.kind = JUMP_CONTINUE;
        return j;
    }
    for (u32 i = inner == FLOW_NONE ? 0 : inner; i-- > 1;) {
        const Frame& fr = s.frames[i];
        bool to_break = fr.breakable && break_target(s, i) == target;
        bool to_continue = fr.kind == FRAME_LOOP && fr.block == target;
        if (to_break || to_continue) {
            j.kind = JUMP_FAR;
            j.code = far_code(i, !to_break);
            j.holder = inner;
            j.to = to_break ? i : FLOW_NONE;
            return j;
        }
    }
    j.kind = JUMP_NONE;
    return j;
}

// A jump the program takes: pass 1 learns from these which labels need their switch.
static Jump resolve(Structurer& s, u32 target, bool natural = true) {
    Jump j = find_jump(s, target, natural);
    if (j.kind == JUMP_NONE)
        give_up(s, "a jump to no loop or label around it");
    if (j.to != FLOW_NONE && s.frames[j.to].kind == FRAME_LABEL)
        s.f->wrapped[s.frames[j.to].block] = true;
    return j;
}

static void add_pending(Structurer& s, u32 frame, u32 code) {
    u32 entry = frame << 16 | code;
    for (u32 i = 0; i < s.pending_count; i++)
        if (s.pending[i] == entry)
            return;
    if (s.pending_count == s.pending_capacity) {
        u32 capacity = s.pending_capacity ? s.pending_capacity * 2 : 16;
        u32* grown = (u32*)realloc(s.pending, sizeof(u32) * capacity);
        if (!grown) {
            give_up(s, "out of memory");
            return;
        }
        s.pending = grown;
        s.pending_capacity = capacity;
    }
    s.pending[s.pending_count++] = entry;
}

// The statement for a jump that leaves (not JUMP_NATURAL).
static const char* jump_statement(Structurer& s, const Jump& j) {
    Translator& t = *s.t;
    switch (j.kind) {
    case JUMP_RETURN:
        return t.info->stage == SHADER_COMPUTE ? "return;" : "return output;";
    case JUMP_BREAK:
        return "break;";
    case JUMP_CONTINUE:
        return "continue;";
    case JUMP_FAR:
        t.far_jumps = true;
        add_pending(s, j.holder, j.code);
        return fmt(t, "{ go_ = %uu; break; }", j.code);
    default:
        return "";
    }
}

// After the closing brace of the breakable frame that was at index `frame`: the far jumps that left it
// either arrive (their frame is now the innermost one to break or continue) or leave the next one too.
static void pass_far_jumps(Structurer& s, u32 frame) {
    if (!s.emitting)
        return;
    Translator& t = *s.t;
    u32 inner, loop;
    innermost(s, s.depth, &inner, &loop);
    bool onward = false;
    for (u32 i = 0; i < s.pending_count; i++) {
        u32 entry = s.pending[i];
        if (entry >> 16 != frame)
            continue;
        u32 code = entry & 0xFFFF;
        u32 target = (code - 1) / 2;
        bool to_continue = ((code - 1) & 1) != 0;
        if (to_continue ? loop == target : inner == target) {
            emit(t, "if (go_ == %uu) { go_ = 0u; %s }", code, to_continue ? "continue;" : "break;");
            s.pending[i] = FLOW_NONE;
        } else if (inner != FLOW_NONE && inner > target) {
            s.pending[i] = inner << 16 | code;
            onward = true;
        } else {
            s.pending[i] = FLOW_NONE;
            give_up(s, "a far jump with nowhere to go");
        }
    }
    u32 kept = 0;
    for (u32 i = 0; i < s.pending_count; i++) {
        u32 entry = s.pending[i];
        bool known = entry == FLOW_NONE;
        for (u32 k = 0; k < kept && !known; k++)
            known = s.pending[k] == entry;
        if (!known)
            s.pending[kept++] = entry;
    }
    s.pending_count = kept;
    if (onward)
        emit(t, "if (go_ != 0u) break;");
}

static void emit_tree(Structurer& s, u32 x);

static void restore_exit_tags(Translator& t, u32 b) {
    const Block& blk = t.program.blocks[b];
    t.tags = t.block_entry[b];
    for (u32 i = blk.first; i < blk.end; i++)
        tags_transfer(t, t.tags, t.program.insts[i], i);
}

// One way out of a block: the join conversions on the edge, then the jump, or the target's own code
// when the edge is the only way there.
struct Arm {
    u32 target;
    bool in_line;  // the target's code goes here
    bool converts; // join conversions on the edge
    Jump jump;
};

static Arm arm_of(Structurer& s, u32 x, u32 target) {
    const Flow& f = *s.f;
    Arm a = {};
    a.target = target;
    a.in_line = target != FLOW_EXIT && !f.labeled[target] && !is_back_edge(f, x, target);
    a.converts = s.emitting && emit_join_conversions(*s.t, target == FLOW_EXIT ? -1 : (s32)target, false);
    if (!a.in_line)
        a.jump = resolve(s, target);
    return a;
}

// Nothing to write: the target's code comes next anyway.
static bool arm_empty(const Arm& a) {
    return !a.in_line && !a.converts && a.jump.kind == JUMP_NATURAL;
}

// Jumps away (after its conversions): what follows its if statement is only for the other arm.
static bool arm_leaves(const Arm& a) {
    return !a.in_line && a.jump.kind != JUMP_NATURAL;
}

// Arm `a` goes where control goes anyway, and the other arm's code comes in line. Unless that is a
// skip-over within the block's own code (to a label placed at it), `a` is written as a jump all the
// same, so that the code in line (often the rest of a loop or of the program) doesn't nest inside an if:
// `if (c) break;` and the code after it. Pass 1 decides, and pass 2 repeats its decision.
static bool flatten(Structurer& s, u32 x, Arm& a) {
    Flow& f = *s.f;
    u32 target = a.target;
    if (!s.emitting) {
        bool own = target != FLOW_EXIT && f.labeled[target] && f.place[target] == x && !f.after_loop[target];
        JumpKind kind = own ? JUMP_NONE : find_jump(s, target, false).kind;
        f.flatten[x] = kind == JUMP_RETURN || kind == JUMP_BREAK || kind == JUMP_CONTINUE;
    }
    if (!f.flatten[x])
        return false;
    a.jump = resolve(s, target, false);
    if (a.jump.kind != JUMP_RETURN && a.jump.kind != JUMP_BREAK && a.jump.kind != JUMP_CONTINUE)
        give_up(s, "a jump the first pass wrote differently");
    return true;
}

static void emit_arm(Structurer& s, u32 x, const Arm& a) {
    Translator& t = *s.t;
    if (s.emitting && a.converts) {
        restore_exit_tags(t, x);
        emit_join_conversions(t, (s32)a.target, true);
    }
    if (a.in_line) {
        if (s.f->idom[a.target] != x) {
            give_up(s, "a block in line away from its dominator");
            return;
        }
        emit_tree(s, a.target);
    } else if (s.emitting && a.jump.kind != JUMP_NATURAL) {
        emit(t, "%s", jump_statement(s, a.jump));
    }
}

// An arm inside braces of its own.
static void emit_braced_arm(Structurer& s, u32 x, const Arm& a) {
    if (!push_frame(s, FRAME_ARM, false, 0))
        return;
    if (s.emitting)
        s.t->depth++;
    emit_arm(s, x, a);
    if (!s.ok)
        return;
    if (s.emitting)
        s.t->depth--;
    s.depth--;
}

static void emit_if_else(Structurer& s, u32 x, const RdnaInst& last, const Arm arm[2]) {
    Translator& t = *s.t;
    if (s.emitting)
        emit(t, "if (%s) {", branch_condition(t, last, false));
    emit_braced_arm(s, x, arm[0]);
    if (s.emitting)
        emit(t, "} else {");
    emit_braced_arm(s, x, arm[1]);
    if (s.emitting)
        emit(t, "}");
}

// A block's instructions, then where it goes.
static void emit_node(Structurer& s, u32 x) {
    Translator& t = *s.t;
    const Program& p = t.program;
    const Block& blk = p.blocks[x];
    if (s.emitting)
        emit_block(t, x);
    u32 taken = blk.next[0] < 0 ? FLOW_EXIT : (u32)blk.next[0];
    if (blk.next[1] < 0 || blk.next[1] == blk.next[0]) {
        Arm a = arm_of(s, x, taken);
        emit_arm(s, x, a);
        return;
    }
    // Arm k is taken where branch_condition(k == 1) holds.
    const RdnaInst& last = p.insts[blk.end - 1];
    Arm arm[2] = {arm_of(s, x, taken), arm_of(s, x, (u32)blk.next[1])};
    if (!s.ok || (arm_empty(arm[0]) && arm_empty(arm[1])))
        return;
    u32 k = 0; // the arm written first
    if (arm[0].in_line && arm[1].in_line) {
        emit_if_else(s, x, last, arm);
        return;
    }
    if (arm[0].in_line || arm[1].in_line) {
        k = arm[0].in_line ? 1 : 0;
        if (arm[k].jump.kind == JUMP_NATURAL && !flatten(s, x, arm[k])) {
            if (arm[k].converts) {
                emit_if_else(s, x, last, arm);
            } else {
                if (s.emitting)
                    emit(t, "if (%s) {", branch_condition(t, last, k == 0));
                emit_braced_arm(s, x, arm[k ^ 1]);
                if (s.emitting)
                    emit(t, "}");
            }
            return;
        }
    } else if (!arm_leaves(arm[0])) {
        k = 1; // both jump, at most one of them where control goes anyway
    }
    // Arm k leaves: `if (c) jump;`, and the other arm after it at the end of the current frame.
    if (s.emitting) {
        const char* condition = branch_condition(t, last, k == 1);
        if (arm[k].converts) {
            emit(t, "if (%s) {", condition);
            t.depth++;
            emit_arm(s, x, arm[k]);
            t.depth--;
            emit(t, "}");
        } else {
            emit(t, "if (%s) %s", condition, jump_statement(s, arm[k].jump));
        }
    }
    emit_arm(s, x, arm[k ^ 1]);
}

static u32 block_offset(const Translator& t, u32 b) {
    return t.program.insts[t.program.blocks[b].first].offset;
}

static void open_label(Structurer& s, u32 y) {
    bool wrap = !s.emitting || s.f->wrapped[y];
    if (!push_frame(s, FRAME_LABEL, wrap, y))
        return;
    if (s.emitting && wrap) {
        emit(*s.t, "switch (0u) { default: // break: to 0x%x", block_offset(*s.t, y));
        s.t->depth++;
    }
}

static void close_label(Structurer& s) {
    Frame fr = s.frames[--s.depth];
    if (s.emitting && fr.breakable) {
        s.t->depth--;
        emit(*s.t, "}");
        pass_far_jumps(s, s.depth);
    }
}

// A block, the labels placed at it and their blocks, inside its loop if it heads one.
static void emit_tree(Structurer& s, u32 x) {
    Flow& f = *s.f;
    Translator& t = *s.t;
    if (!s.ok)
        return;
    if (f.emitted[x]) {
        give_up(s, "a block reached twice");
        return;
    }
    if (s.nesting == FLOW_MAX_NESTING) {
        give_up(s, "nested too deep");
        return;
    }
    f.emitted[x] = true;
    s.nesting++;
    u32 first = f.child_at[x], end = f.child_at[x + 1];
    u32 split = first;
    while (split < end && f.after_loop[f.children[split]])
        split++;
    for (u32 i = split; i-- > first && s.ok;)
        open_label(s, f.children[i]);
    if (f.header[x] && push_frame(s, FRAME_LOOP, true, x) && s.emitting) {
        emit(t, "[loop] for (;;) { // 0x%x", block_offset(t, x));
        t.depth++;
    }
    for (u32 i = end; i-- > split && s.ok;)
        open_label(s, f.children[i]);
    if (s.ok)
        emit_node(s, x);
    for (u32 i = split; i < end && s.ok; i++) {
        close_label(s);
        emit_tree(s, f.children[i]);
    }
    if (f.header[x] && s.ok) {
        s.depth--;
        if (s.emitting) {
            t.depth--;
            emit(t, "}");
            pass_far_jumps(s, s.depth);
        }
    }
    for (u32 i = first; i < split && s.ok; i++) {
        close_label(s);
        emit_tree(s, f.children[i]);
    }
    s.nesting--;
}

// One pass over the tree from the first block. False where the graph isn't a structure this knows.
static bool structure_pass(Structurer& s, bool emitting) {
    Flow& f = *s.f;
    s.emitting = emitting;
    s.ok = true;
    s.why = nullptr;
    s.depth = 0;
    s.nesting = 0;
    s.pending_count = 0;
    memset(f.emitted, 0, f.count);
    push_frame(s, FRAME_ROOT, false, 0);
    emit_tree(s, 0);
    if (s.ok && (s.depth != 1 || s.pending_count))
        give_up(s, "unbalanced");
    for (u32 i = 0; i < f.reached && s.ok; i++)
        if (!f.emitted[f.rpo[i]])
            give_up(s, "a block left out");
    return s.ok;
}

// --- the program's control flow ----------------------------------------------------------------------

void emit_control_flow(Translator& t) {
    const Program& p = t.program;
    if (p.block_count == 1) {
        emit_block(t, 0);
        return;
    }
    Flow f;
    const char* why = nullptr;
    if (!flow_alloc(f, p.block_count) || !flow_order(f, p)) {
        why = "out of memory";
    } else {
        flow_predecessors(f, p);
        flow_dominators(f);
        if (!flow_reducible(f))
            why = "a loop with several entries";
        else if (!flow_loops(f))
            why = "out of memory";
        else if (!flow_loops_leave(f, p))
            why = "a loop with no way out";
        else {
            flow_labels(f);
            if (!flow_children(f))
                why = "out of memory";
        }
    }
    Structurer s = {};
    s.t = &t;
    s.f = &f;
    if (!why && !structure_pass(s, false))
        why = s.why;
    if (!why) {
        if (!structure_pass(s, true) && !t.failed)
            translator_fail(t, "control flow: %s, in a structure the first pass took", s.why ? s.why : "?");
    }
    free(s.pending);
    flow_release(f);
    if (why) {
        emit(t, "// blocks dispatched: %s", why);
        emit_dispatcher(t);
    }
}
