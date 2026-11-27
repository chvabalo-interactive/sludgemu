// Console shader translation: memory instructions (scalar and buffer loads, vertex fetches, images,
// LDS, global memory).

#include <stdio.h>
#include <string.h>

#include "shader/translate_internal.h"

static Tag sgpr_tag(const Translator& t, u32 reg) {
    return t.tags.sgpr[reg & 127];
}

static const char* soffset(Translator& t, const RdnaInst& in, u32 i) {
    const RdnaOperand& o = in.src[i];
    if (o.kind == OPND_SPECIAL && o.reg == SRC_NULL)
        return "0u";
    return src_u32(t, in, i);
}

static const char* const k_load_fn[5] = {"", "Load", "Load2", "Load3", "Load4"};
static const char* const k_store_fn[5] = {"", "Store", "Store2", "Store3", "Store4"};

// Where the descriptor in registers [reg, reg + dwords) comes from. Translated code keeps only the heap
// address of a descriptor loaded from the heap (in its first register), so a descriptor works as one
// only if every dword was loaded from the same slot and left alone, except the dwords in `may_change`
// (by bit), which the access at hand reads as values or not at all.
enum DescriptorOrigin {
    ORIGIN_HEAP,            // loaded from one slot of the heap `heap`, unchanged
    ORIGIN_CONSTANT,        // every dword known at translation time
    ORIGIN_PARTLY_CONSTANT, // some dwords known, others not
    ORIGIN_MODIFIED,        // loaded from the heap, then changed
    ORIGIN_MIXED,           // loaded from the heap on some paths only
    ORIGIN_OTHER_HEAP,      // loaded from the other heap
    ORIGIN_ASSEMBLED,       // dwords from different slots or tables
    ORIGIN_UNKNOWN,         // nothing the translator can place
};

static DescriptorOrigin descriptor_origin(const Translator& t, u32 reg, u32 dwords, u32 heap,
                                          u32 may_change = 0) {
    u32 constants = 0, loaded = 0, value;
    for (u32 i = 0; i < dwords; i++) {
        constants += sgpr_constant(t.tags, reg + i, &value) ? 1 : 0;
        loaded += heap_loaded(sgpr_tag(t, reg + i)) ? 1 : 0;
    }
    if (constants == dwords)
        return ORIGIN_CONSTANT;
    Tag first = sgpr_tag(t, reg);
    if (!heap_loaded(first)) {
        // A value in place of the first dword, next to loaded ones: the base address was changed.
        return constants ? ORIGIN_PARTLY_CONSTANT : loaded ? ORIGIN_MODIFIED : ORIGIN_UNKNOWN;
    }
    if ((tag_payload(first) & 1) != heap)
        return ORIGIN_OTHER_HEAP;
    u16 slot = t.tags.descriptor[reg & 127];
    DescriptorOrigin origin = ORIGIN_HEAP;
    for (u32 i = 0; i < dwords; i++) {
        if ((may_change >> i) & 1)
            continue; // whatever it holds
        Tag tag = sgpr_tag(t, reg + i);
        u16 d = t.tags.descriptor[(reg + i) & 127];
        if (sgpr_constant(t.tags, reg + i, &value))
            return ORIGIN_PARTLY_CONSTANT;
        // Arithmetic on a loaded dword gives a plain value (see carried).
        if (!heap_loaded(tag))
            return ORIGIN_MODIFIED;
        // The same table; the same slot where known.
        u32 compared = HEAP_MODIFIED | HEAP_IMAGE;
        if ((tag & ~compared) != (first & ~compared) ||
            (slot != NO_DESCRIPTOR && d != NO_DESCRIPTOR && d != slot))
            return ORIGIN_ASSEMBLED;
        if (tag_payload(tag) & HEAP_MODIFIED)
            origin = ORIGIN_MIXED;
    }
    return origin;
}

// Buffer descriptor dwords that may hold anything for an access through a desktop view of the slot's
// buffer. Compilers rewrite structured buffer descriptors into raw ones (record count times stride,
// stride cleared, raw bounds checking), which is what desktop views are anyway. Not the base address
// (dword 0), which names the slot. The record count (dword 2) bounds the console's accesses only,
// desktop bounds are the view's size. Untyped accesses read the stride (dword 1, with the base's high
// bits) as a value where an index needs it, and make no use of the format and swizzle (dword 3); typed
// accesses go through the desktop view's format, and indexed ones through its elements, which the
// descriptor must then still describe.
static u32 buffer_fields_unused(bool typed, bool indexed) {
    return 1u << 2 | (typed && indexed ? 0u : 1u << 1) | (typed ? 0u : 1u << 3);
}

static const char* origin_text(DescriptorOrigin origin) {
    switch (origin) {
    case ORIGIN_HEAP:
        return "from the heap";
    case ORIGIN_CONSTANT:
        return "made of constants";
    case ORIGIN_PARTLY_CONSTANT:
        return "partly made of constants";
    case ORIGIN_MODIFIED:
        return "changed after loading";
    case ORIGIN_MIXED:
        return "loaded on some paths only";
    case ORIGIN_OTHER_HEAP:
        return "from the other heap";
    case ORIGIN_ASSEMBLED:
        return "put together from different places";
    default:
        return "of unknown origin";
    }
}

// --- view kinds --------------------------------------------------------------------------------------

// How an access views a descriptor from the heap on desktop: read-only (shader resource views and
// constant buffers), read-write (unordered access views), or by the kind the shadow records for the
// slot, which translated code branches on (uniformly: descriptors are in scalar registers).
enum ViewAccess {
    VIEW_READ_ONLY,
    VIEW_READ_WRITE,
    VIEW_BY_SHADOW,
};

u8 slot_use(const Translator& t, Tag tag, u16 index) {
    if (index == NO_DESCRIPTOR || !heap_loaded(tag) || (tag_payload(tag) & 1) != HEAP_RESOURCES)
        return 0;
    for (u32 i = 0; i < t.slot_use_count; i++)
        if (t.slot_uses[i].parameter == heap_parameter(tag) && t.slot_uses[i].index == index)
            return t.slot_uses[i].flags;
    return 0;
}

// What the program does through the descriptor in `reg` elsewhere, if it is at a known place.
static u8 slot_flags(const Translator& t, const TagState& s, u32 reg) {
    return slot_use(t, s.sgpr[reg & 127], s.descriptor[reg & 127]);
}

// The view for an access through the descriptor in `reg` that writes, samples or only reads: writing
// takes an unordered access view and sampling a shader resource view (which a program doing both
// through one slot can't have on desktop); reads follow what the program does through the slot
// elsewhere, or else what the slot holds.
static bool view_access(Translator& t, const RdnaInst& in, u32 reg, bool writes, bool samples,
                        ViewAccess* out) {
    u8 flags = slot_flags(t, t.tags, reg);
    if ((writes || (flags & SLOT_WRITTEN)) && (samples || (flags & SLOT_SAMPLED))) {
        translator_fail(t, "%s at 0x%x through a descriptor the program both writes and samples through",
                        rdna_op_name(in.op), in.offset);
        return false;
    }
    if (writes || (flags & SLOT_WRITTEN))
        *out = VIEW_READ_WRITE;
    else if (samples || (flags & SLOT_SAMPLED))
        *out = VIEW_READ_ONLY;
    else
        *out = VIEW_BY_SHADOW;
    return true;
}

// The kind the shadow records for the slot of the loaded descriptor in `reg`: a uniform value.
static const char* shadow_kind(Translator& t, u32 reg) {
    t.needs_descriptor_info = true;
    return fmt(t, "g_descriptor_info.Load((%s & ~%uu) + %uu)", sgpr(t, reg), SHADER_SHADOW_RECORD_BYTES - 1,
               SHADER_SHADOW_KIND_OFFSET);
}

// Emitting an access for each view it may take: once for a known one; for VIEW_BY_SHADOW a branch on
// the slot's kind, read-write first. Usage: for (ViewPass v = view_begin(...); view_next(t, v);) {...}
struct ViewPass {
    ViewAccess access;
    u32 reg;
    u32 pass;
    bool read_write; // the view of the current pass
};

static ViewPass view_begin(ViewAccess access, u32 reg) {
    return {access, reg, 0, false};
}

static bool view_next(Translator& t, ViewPass& v) {
    u32 passes = v.access == VIEW_BY_SHADOW ? 2 : 1;
    if (v.access == VIEW_BY_SHADOW && v.pass > 0)
        t.depth--;
    if (v.pass == passes) {
        if (v.access == VIEW_BY_SHADOW)
            emit(t, "}");
        return false;
    }
    if (v.access == VIEW_BY_SHADOW) {
        if (v.pass == 0)
            emit(t, "if (%s == %uu) {", shadow_kind(t, v.reg), SHADOW_VIEW_UAV);
        else
            emit(t, "} else {");
        t.depth++;
    }
    v.read_write = v.access == VIEW_READ_WRITE || (v.access == VIEW_BY_SHADOW && v.pass == 0);
    v.pass++;
    return !t.failed;
}

// What an instruction does through its resource descriptor (SLOT_* flags), for find_slot_uses.
static u8 access_flags(const RdnaInst& in);

void find_slot_uses(Translator& t) {
    t.slot_use_count = 0;
    const Program& p = t.program;
    for (u32 b = 0; b < p.block_count; b++) {
        if (!t.block_reached[b])
            continue;
        TagState s = t.block_entry[b];
        for (u32 i = p.blocks[b].first; i < p.blocks[b].end; i++) {
            const RdnaInst& in = p.insts[i];
            u8 flags = access_flags(in);
            u32 reg = in.src[1].reg & 127;
            Tag tag = s.sgpr[reg];
            u16 index = s.descriptor[reg];
            if (flags && heap_loaded(tag) && (tag_payload(tag) & 1) == HEAP_RESOURCES &&
                index != NO_DESCRIPTOR) {
                u32 k = 0;
                while (k < t.slot_use_count &&
                       !(t.slot_uses[k].parameter == heap_parameter(tag) && t.slot_uses[k].index == index))
                    k++;
                if (k == t.slot_use_count && k < SHADER_MAX_SLOT_USES)
                    t.slot_uses[t.slot_use_count++] = {(u8)heap_parameter(tag), 0, index};
                // Beyond the table's capacity a slot's other accesses decide alone.
                if (k < t.slot_use_count)
                    t.slot_uses[k].flags |= flags;
            }
            tags_transfer(t, s, in, i);
        }
    }
}

// Constant data in the code (a buffer descriptor based on s_getpc_b64 plus an offset) becomes a static
// array of the code's dwords from the base to the end of the buffer: what the descriptor's size allows,
// at most the rest of the code. Reads past the end read 0 like out-of-range buffer reads do: the array
// has one more element, zero, and every read clamps its index to it (g_pc_<i>_end).
static s32 register_pc_pool(Translator& t, u32 rsrc_reg) {
    u32 base_off = 0;
    if (!sgpr_constant(t.tags, rsrc_reg, &base_off))
        return -1;
    if (base_off >= t.code_size)
        return -1;
    u32 avail = (t.code_size - base_off) / 4u;
    u32 dwords = avail;
    // Records are bytes, or elements of the stride when the descriptor has one, so without the stride
    // (the word holding it usually has the code address in it too) the size is unknown.
    u32 records = 0, word1 = 0;
    if (sgpr_constant(t.tags, rsrc_reg + 2, &records) && sgpr_constant(t.tags, rsrc_reg + 1, &word1)) {
        u64 bytes = records;
        u32 stride = (word1 >> 16) & 0x3FFFu;
        if (stride)
            bytes *= stride;
        u64 d = (bytes + 3u) / 4u;
        if (d < avail)
            dwords = (u32)d;
    }
    for (u32 i = 0; i < t.pc_pool_count; i++) {
        if (t.pc_pools[i].base_off == base_off) {
            if (dwords > t.pc_pools[i].dwords)
                t.pc_pools[i].dwords = dwords;
            return (s32)i;
        }
    }
    if (t.pc_pool_count >= SHADER_MAX_PC_POOLS)
        return -1;
    u32 idx = t.pc_pool_count++;
    t.pc_pools[idx] = {base_off, dwords};
    return (s32)idx;
}

static void write_loaded(Translator& t, const RdnaInst& in, u32 dwords, const char* value) {
    static const char k_comp[4] = {'x', 'y', 'z', 'w'};
    for (u32 i = 0; i < dwords; i++)
        write_vgpr(t, in, in.dst[0].reg + i, dwords == 1 ? value : fmt(t, "%s.%c", value, k_comp[i]));
}

// --- scalar memory -----------------------------------------------------------------------------------

static bool emit_scalar_memory(Translator& t, const RdnaInst& in) {
    if (in.op == OP_S_DCACHE_INV || in.op == OP_S_GL1_INV)
        return true;
    if (in.op == OP_S_MEMTIME || in.op == OP_S_MEMREALTIME) {
        write_sgpr(t, in.dst[0], 0, "0u");
        write_sgpr(t, in.dst[0], 1, "0u");
        return true;
    }
    u32 base = in.src[0].reg;
    u32 dwords = in.dst[0].count;
    const char* offset = fmt(t, "(%s + 0x%xu + %s)", sgpr(t, base), (u32)in.mem.offset, soffset(t, in, 1));
    bool buffer = in.op >= OP_S_BUFFER_LOAD_DWORD && in.op <= OP_S_BUFFER_LOAD_DWORDX16;
    Tag lo = sgpr_tag(t, base), hi = sgpr_tag(t, base + 1);

    if (!buffer && heap_loaded(lo)) {
        // Through the address in a descriptor (a buffer's base, say): the memory there isn't placed.
        translator_fail(t, "%s at 0x%x through a descriptor's address", rdna_op_name(in.op), in.offset);
        return false;
    }
    if (!buffer && tag_kind(lo) == TAG_HEAP) {
        // How far into its table the program reads: console tables are plain memory, and programs may
        // read past the ranges their root signature declares.
        u32 parameter = heap_parameter(lo);
        if (parameter != HEAP_NO_TABLE && parameter < SHADER_MAX_ROOT_PARAMETERS) {
            u32 slot = (tag_payload(lo) & 1) == HEAP_SAMPLERS ? 16 : 32;
            u32 at = t.tags.descriptor[base & 127];
            bool constant = in.src[1].kind == OPND_SPECIAL && in.src[1].reg == SRC_NULL;
            u16& reach = t.out->table_reach[parameter];
            if (at != NO_DESCRIPTOR && constant && in.mem.offset >= 0) {
                u32 end = (at * slot + (u32)in.mem.offset + 4 * dwords + slot - 1) / slot;
                if (reach != TABLE_REACH_UNKNOWN && end > reach)
                    reach = (u16)(end < TABLE_REACH_UNKNOWN ? end : TABLE_REACH_UNKNOWN);
            } else {
                reach = TABLE_REACH_UNKNOWN;
            }
        }
        // Descriptors: each destination dword holds its own heap byte address.
        emit(t, "{ uint a_ = %s;", offset);
        t.depth++;
        for (u32 i = 0; i < dwords; i++)
            write_sgpr(t, in.dst[0], i, fmt(t, "a_ + %uu", 4 * i));
        t.depth--;
        emit(t, "}");
        return true;
    }
    if (!buffer && tag_kind(lo) == TAG_VB_TABLE) {
        for (u32 i = 0; i < dwords; i++)
            write_sgpr(t, in.dst[0], i, "0u"); // vertex buffer descriptors: tracked statically
        return true;
    }
    if (tag_kind(hi) == TAG_ROOT_HI) {
        u32 parameter = tag_payload(hi);
        t.root_used |= 1ull << parameter;
        emit(t, "{ uint a_ = %s;", offset);
        t.depth++;
        for (u32 i = 0; i < dwords; i += 4) {
            u32 n = dwords - i < 4 ? dwords - i : 4;
            emit(t, "uint%s l%u_ = g_root%u.%s(a_ + %uu);", n == 1 ? "" : fmt(t, "%u", n), i, parameter,
                 k_load_fn[n], 4 * i);
            for (u32 k = 0; k < n; k++)
                write_sgpr(t, in.dst[0], i + k, n == 1 ? fmt(t, "l%u_", i) : fmt(t, "l%u_.%c", i, "xyzw"[k]));
        }
        t.depth--;
        emit(t, "}");
        return true;
    }
    if (buffer && tag_kind(lo) == TAG_HEAP && (tag_payload(lo) & 1) == HEAP_RESOURCES) {
        DescriptorOrigin origin =
            descriptor_origin(t, base, 4, HEAP_RESOURCES, buffer_fields_unused(false, false));
        if (origin != ORIGIN_HEAP) {
            translator_fail(t, "%s at 0x%x through a buffer descriptor %s", rdna_op_name(in.op), in.offset,
                            origin_text(origin));
            return false;
        }
        ViewAccess access;
        if (!view_access(t, in, base, false, false, &access))
            return false;
        emit(t, "{ uint a_ = %s;", fmt(t, "(0x%xu + %s)", (u32)in.mem.offset, soffset(t, in, 1)));
        t.depth++;
        // Constant buffers too are raw buffer views on desktop.
        for (ViewPass v = view_begin(access, base); view_next(t, v);) {
            emit(t, "%s b_ = ResourceDescriptorHeap[%s >> 5];",
                 v.read_write ? "RWByteAddressBuffer" : "ByteAddressBuffer", sgpr(t, base));
            for (u32 i = 0; i < dwords; i++)
                write_sgpr(t, in.dst[0], i, fmt(t, "b_.Load(a_ + %uu)", 4 * i));
        }
        t.depth--;
        emit(t, "}");
        return true;
    }
    if (buffer && tag_kind(lo) == TAG_PC) {
        s32 pid = register_pc_pool(t, base);
        if (pid >= 0) {
            emit(t, "{ uint wi_ = (0x%xu + %s) >> 2;", (u32)in.mem.offset, soffset(t, in, 1));
            t.depth++;
            for (u32 i = 0; i < dwords; i++)
                write_sgpr(t, in.dst[0], i, fmt(t, "g_pc_%d[min(wi_ + %uu, g_pc_%d_end)]", pid, i, pid));
            t.depth--;
            emit(t, "}");
            return true;
        }
    }
    char why[96];
    describe_tag_loss(t, false, base, why, sizeof(why));
    translator_fail(t, "%s at 0x%x from an address the translator cannot place%s", rdna_op_name(in.op),
                    in.offset, why);
    return false;
}

// --- buffers -----------------------------------------------------------------------------------------

static u32 buffer_dwords(const RdnaInst& in) {
    return in.dst[0].count - (in.mem.tfe ? 1 : 0);
}

// Formatted accesses with two 16-bit components per register.
static bool is_d16_format(RdnaOp op) {
    return (op >= OP_BUFFER_LOAD_FORMAT_D16_X && op <= OP_BUFFER_STORE_FORMAT_D16_XYZW) ||
           (op >= OP_TBUFFER_LOAD_FORMAT_D16_X && op <= OP_TBUFFER_STORE_FORMAT_D16_XYZW);
}

// (The 16-bit format opcodes come after the others of their encoding, not next to them.)
static bool is_store(RdnaOp op) {
    return (op >= OP_BUFFER_STORE_FORMAT_X && op <= OP_BUFFER_STORE_FORMAT_XYZW) ||
           (op >= OP_BUFFER_STORE_BYTE && op <= OP_BUFFER_STORE_DWORDX3) ||
           (op >= OP_BUFFER_STORE_FORMAT_D16_X && op <= OP_BUFFER_STORE_FORMAT_D16_XYZW) ||
           (op >= OP_TBUFFER_STORE_FORMAT_X && op <= OP_TBUFFER_STORE_FORMAT_XYZW) ||
           (op >= OP_TBUFFER_STORE_FORMAT_D16_X && op <= OP_TBUFFER_STORE_FORMAT_D16_XYZW);
}

static bool is_format_access(RdnaOp op) {
    return op <= OP_BUFFER_STORE_FORMAT_XYZW || is_d16_format(op) ||
           (op >= OP_TBUFFER_LOAD_FORMAT_X && op <= OP_TBUFFER_STORE_FORMAT_XYZW);
}

static bool is_buffer_atomic(RdnaOp op) {
    return op >= OP_BUFFER_ATOMIC_SWAP && op <= OP_BUFFER_ATOMIC_DEC;
}

static bool is_image_atomic(RdnaOp op) {
    return op >= OP_IMAGE_ATOMIC_SWAP && op <= OP_IMAGE_ATOMIC_DEC;
}

enum MtbufNum : u8 {
    MTBUF_UNORM,
    MTBUF_SNORM,
    MTBUF_USCALED,
    MTBUF_SSCALED,
    MTBUF_UINT,
    MTBUF_SINT,
    MTBUF_FLOAT,
};

struct MtbufFmt {
    u8 channels;
    u8 bits[4];
    MtbufNum num;
};

static bool mtbuf_set(MtbufFmt* out, u8 channels, u8 b0, u8 b1, u8 b2, u8 b3, MtbufNum num) {
    out->channels = channels;
    out->bits[0] = b0;
    out->bits[1] = b1;
    out->bits[2] = b2;
    out->bits[3] = b3;
    out->num = num;
    return true;
}

// RDNA 2 (gfx10) unified buffer formats (LLVM UfmtSymbolicGFX10). Format names list the fields from the
// most significant bits down; bits[] lists the channels from bit 0 (10_11_11 is R11G11B10, 2_10_10_10 is
// R10G10B10A2).
static bool mtbuf_format_info(u32 fmt_id, MtbufFmt* out) {
    static const MtbufNum k_6[6] = {MTBUF_UNORM,   MTBUF_SNORM, MTBUF_USCALED,
                                    MTBUF_SSCALED, MTBUF_UINT,  MTBUF_SINT};
    static const MtbufNum k_7[7] = {MTBUF_UNORM, MTBUF_SNORM, MTBUF_USCALED, MTBUF_SSCALED,
                                    MTBUF_UINT,  MTBUF_SINT,  MTBUF_FLOAT};
    static const MtbufNum k_3[3] = {MTBUF_UINT, MTBUF_SINT, MTBUF_FLOAT};
    if (fmt_id >= 1 && fmt_id <= 6)
        return mtbuf_set(out, 1, 8, 0, 0, 0, k_6[fmt_id - 1]);
    if (fmt_id >= 7 && fmt_id <= 13)
        return mtbuf_set(out, 1, 16, 0, 0, 0, k_7[fmt_id - 7]);
    if (fmt_id >= 14 && fmt_id <= 19)
        return mtbuf_set(out, 2, 8, 8, 0, 0, k_6[fmt_id - 14]);
    if (fmt_id >= 20 && fmt_id <= 22)
        return mtbuf_set(out, 1, 32, 0, 0, 0, k_3[fmt_id - 20]);
    if (fmt_id >= 23 && fmt_id <= 29)
        return mtbuf_set(out, 2, 16, 16, 0, 0, k_7[fmt_id - 23]);
    if (fmt_id >= 30 && fmt_id <= 36)
        return mtbuf_set(out, 3, 11, 11, 10, 0, k_7[fmt_id - 30]);
    if (fmt_id >= 37 && fmt_id <= 43)
        return mtbuf_set(out, 3, 10, 11, 11, 0, k_7[fmt_id - 37]);
    if (fmt_id >= 44 && fmt_id <= 49)
        return mtbuf_set(out, 4, 2, 10, 10, 10, k_6[fmt_id - 44]);
    if (fmt_id >= 50 && fmt_id <= 55)
        return mtbuf_set(out, 4, 10, 10, 10, 2, k_6[fmt_id - 50]);
    if (fmt_id >= 56 && fmt_id <= 61)
        return mtbuf_set(out, 4, 8, 8, 8, 8, k_6[fmt_id - 56]);
    if (fmt_id >= 62 && fmt_id <= 64)
        return mtbuf_set(out, 2, 32, 32, 0, 0, k_3[fmt_id - 62]);
    if (fmt_id >= 65 && fmt_id <= 71)
        return mtbuf_set(out, 4, 16, 16, 16, 16, k_7[fmt_id - 65]);
    if (fmt_id >= 72 && fmt_id <= 74)
        return mtbuf_set(out, 3, 32, 32, 32, 0, k_3[fmt_id - 72]);
    if (fmt_id >= 75 && fmt_id <= 77)
        return mtbuf_set(out, 4, 32, 32, 32, 32, k_3[fmt_id - 75]);
    return false;
}

static bool emit_buffer_atomic(Translator& t, const RdnaInst& in, const char* buf, const char* addr) {
    bool rtn = in.mem.glc;
    const char* d0 = vgpr(t, in.dst[0].reg);
    bool is_signed = in.op == OP_BUFFER_ATOMIC_SMIN || in.op == OP_BUFFER_ATOMIC_SMAX;
    if (in.op == OP_BUFFER_ATOMIC_CMPSWAP) {
        const char* d1 = vgpr(t, in.dst[0].reg + 1);
        if (rtn)
            emit(t, "uint o_ = 0u; if (%s) %s.InterlockedCompareExchange(%s, %s, %s, o_);", exec_cond(t), buf,
                 addr, d1, d0);
        else
            emit(t, "if (%s) %s.InterlockedCompareStore(%s, %s, %s);", exec_cond(t), buf, addr, d1, d0);
    } else if (in.op == OP_BUFFER_ATOMIC_INC || in.op == OP_BUFFER_ATOMIC_DEC) {
        const char* next = in.op == OP_BUFFER_ATOMIC_INC
                               ? fmt(t, "(cur_ >= %s ? 0u : cur_ + 1u)", d0)
                               : fmt(t, "((cur_ == 0u || cur_ > %s) ? %s : cur_ - 1u)", d0, d0);
        emit(t,
             "uint o_ = 0u; if (%s) { uint cur_ = %s.Load(%s), prev_ = 0u; [allow_uav_condition] do { "
             "uint nxt_ = %s; %s.InterlockedCompareExchange(%s, cur_, nxt_, prev_); if (prev_ == cur_) { o_ "
             "= cur_; break; } cur_ = prev_; } while (true); }",
             exec_cond(t), buf, addr, next, buf, addr);
    } else if (in.op == OP_BUFFER_ATOMIC_SWAP) {
        emit(t, "uint o_ = 0u; if (%s) %s.InterlockedExchange(%s, %s, o_);", exec_cond(t), buf, addr, d0);
    } else {
        const char* fn = nullptr;
        const char* val = d0;
        switch (in.op) {
        case OP_BUFFER_ATOMIC_ADD:
            fn = "InterlockedAdd";
            break;
        case OP_BUFFER_ATOMIC_SUB:
            fn = "InterlockedAdd";
            val = fmt(t, "(uint)(-(int)%s)", d0);
            break;
        case OP_BUFFER_ATOMIC_SMIN:
            fn = "InterlockedMin";
            val = fmt(t, "(int)%s", d0);
            break;
        case OP_BUFFER_ATOMIC_UMIN:
            fn = "InterlockedMin";
            break;
        case OP_BUFFER_ATOMIC_SMAX:
            fn = "InterlockedMax";
            val = fmt(t, "(int)%s", d0);
            break;
        case OP_BUFFER_ATOMIC_UMAX:
            fn = "InterlockedMax";
            break;
        case OP_BUFFER_ATOMIC_AND:
            fn = "InterlockedAnd";
            break;
        case OP_BUFFER_ATOMIC_OR:
            fn = "InterlockedOr";
            break;
        case OP_BUFFER_ATOMIC_XOR:
            fn = "InterlockedXor";
            break;
        default:
            translator_fail(t, "unsupported %s", rdna_op_name(in.op));
            return false;
        }
        if (rtn)
            emit(t, "%s o_ = %s; if (%s) %s.%s(%s, %s, o_);", is_signed ? "int" : "uint",
                 is_signed ? "0" : "0u", exec_cond(t), buf, fn, addr, val);
        else
            emit(t, "if (%s) %s.%s(%s, %s);", exec_cond(t), buf, fn, addr, val);
    }
    if (rtn)
        write_vgpr(t, in, in.dst[0].reg, is_signed ? "(uint)o_" : "o_");
    return true;
}

static bool emit_mtbuf_access(Translator& t, const RdnaInst& in, const char* buf, const char* addr,
                              bool store) {
    MtbufFmt f;
    if (!mtbuf_format_info(in.mem.format, &f)) {
        translator_fail(t, "%s with unsupported format %u at 0x%x", rdna_op_name(in.op), in.mem.format,
                        in.offset);
        return false;
    }
    bool d16 = is_d16_format(in.op);
    u32 comps =
        store ? (u32)(d16 ? in.op - OP_TBUFFER_STORE_FORMAT_D16_X : in.op - OP_TBUFFER_STORE_FORMAT_X) + 1
              : (u32)(d16 ? in.op - OP_TBUFFER_LOAD_FORMAT_D16_X : in.op - OP_TBUFFER_LOAD_FORMAT_X) + 1;
    if (comps == 0 || comps > 4) {
        translator_fail(t, "%s at 0x%x: bad component count", rdna_op_name(in.op), in.offset);
        return false;
    }
    u32 total_bits = 0;
    for (u32 i = 0; i < f.channels; i++)
        total_bits += f.bits[i];
    u32 dwords = (total_bits + 31) / 32;
    if (store) {
        if (total_bits % 32 != 0) {
            translator_fail(t, "sub-dword %s at 0x%x", rdna_op_name(in.op), in.offset);
            return false;
        }
        const char* in_c[4] = {};
        for (u32 i = 0; i < 4; i++) {
            if (i < comps) {
                if (d16) {
                    const char* r = vgpr(t, in.dst[0].reg + i / 2);
                    const char* half = (i & 1) ? fmt(t, "(%s >> 16)", r) : fmt(t, "(%s & 0xffffu)", r);
                    in_c[i] =
                        (f.num == MTBUF_UINT || f.num == MTBUF_SINT) ? half : fmt(t, "f16tof32(%s)", half);
                } else {
                    const char* r = vgpr(t, in.dst[0].reg + i);
                    in_c[i] = (f.num == MTBUF_UINT || f.num == MTBUF_SINT) ? r : fmt(t, "asfloat(%s)", r);
                }
            } else {
                in_c[i] = (f.num == MTBUF_UINT || f.num == MTBUF_SINT) ? "0u" : "0.0f";
            }
        }
        if (dwords == 1)
            emit(t, "uint w_ = 0u;");
        else
            emit(t, "uint%u w_ = (uint%u)0u;", dwords, dwords);
        u32 bit_at = 0;
        for (u32 i = 0; i < f.channels; i++) {
            u32 w = f.bits[i];
            const char* enc = nullptr;
            u32 max_u = w == 32 ? 0xffffffffu : ((1u << w) - 1u);
            s32 max_s = (s32)((1u << (w - 1)) - 1u);
            switch (f.num) {
            case MTBUF_FLOAT:
                // 11- and 10-bit floats are unsigned halves without the low mantissa bits (truncated).
                if (w == 32)
                    enc = fmt(t, "asuint(%s)", in_c[i]);
                else if (w == 16)
                    enc = fmt(t, "f32tof16(%s)", in_c[i]);
                else
                    enc = fmt(t, "((f32tof16(max(%s, 0.0f)) >> %uu) & 0x%xu)", in_c[i], 15u - w, max_u);
                break;
            case MTBUF_UNORM:
                enc = fmt(t, "(uint)round(saturate(%s) * %u.0f)", in_c[i], max_u);
                break;
            case MTBUF_SNORM:
                enc = fmt(t, "((uint)(int)round(clamp(%s, -1.0f, 1.0f) * %d.0f) & 0x%xu)", in_c[i], max_s,
                          max_u);
                break;
            case MTBUF_USCALED:
                enc = fmt(t, "(f32_to_u32(%s) & 0x%xu)", in_c[i], max_u);
                break;
            case MTBUF_SSCALED:
                enc = fmt(t, "((uint)f32_to_i32(%s) & 0x%xu)", in_c[i], max_u);
                break;
            default:
                enc = w == 32 ? in_c[i] : fmt(t, "(%s & 0x%xu)", in_c[i], max_u);
                break;
            }
            u32 dw = bit_at / 32, sh = bit_at % 32;
            const char* dst_w = dwords == 1 ? "w_" : fmt(t, "w_.%c", "xyzw"[dw]);
            emit(t, "%s |= (%s << %uu);", dst_w, enc, sh);
            bit_at += w;
        }
        emit(t, "if (%s) %s.%s(%s, w_);", exec_cond(t), buf, k_store_fn[dwords], addr);
        return true;
    }
    emit(t, "uint%s w_ = %s.%s(%s);", dwords == 1 ? "" : fmt(t, "%u", dwords), buf, k_load_fn[dwords], addr);
    const char* out_c[4] = {};
    u32 bit_at = 0;
    bool int_fmt = f.num == MTBUF_UINT || f.num == MTBUF_SINT;
    for (u32 i = 0; i < comps; i++) {
        if (i >= f.channels) {
            out_c[i] = (i == 3) ? (int_fmt ? "1u" : (d16 ? "0x3c00u" : "0x3f800000u")) : "0u";
            continue;
        }
        u32 w = f.bits[i], dw = bit_at / 32, sh = bit_at % 32;
        bit_at += w;
        const char* src_w = dwords == 1 ? "w_" : fmt(t, "w_.%c", "xyzw"[dw]);
        u32 mask = w == 32 ? 0xffffffffu : ((1u << w) - 1u);
        const char* raw = w == 32 ? src_w : fmt(t, "((%s >> %uu) & 0x%xu)", src_w, sh, mask);
        const char* sraw = w == 32 ? fmt(t, "(int)%s", src_w)
                                   : fmt(t, "((int)(%s << %uu) >> %uu)", src_w, 32u - sh - w, 32u - w);
        const char* flt = nullptr;
        switch (f.num) {
        case MTBUF_FLOAT:
            if (d16 && w == 16)
                out_c[i] = raw;
            else if (w == 32)
                flt = fmt(t, "asfloat(%s)", raw);
            else if (w == 16)
                flt = fmt(t, "f16tof32(%s)", raw);
            else // 11- and 10-bit floats: unsigned halves without the low mantissa bits
                flt = fmt(t, "f16tof32(%s << %uu)", raw, 15u - w);
            break;
        case MTBUF_UNORM:
            flt = fmt(t, "((float)%s / %u.0f)", raw, mask);
            break;
        case MTBUF_SNORM:
            flt = fmt(t, "max((float)%s / %d.0f, -1.0f)", sraw, (s32)((1u << (w - 1)) - 1u));
            break;
        case MTBUF_USCALED:
            flt = fmt(t, "(float)%s", raw);
            break;
        case MTBUF_SSCALED:
            flt = fmt(t, "(float)%s", sraw);
            break;
        case MTBUF_UINT:
            out_c[i] = d16 ? fmt(t, "(%s & 0xffffu)", raw) : raw;
            break;
        case MTBUF_SINT:
            out_c[i] = d16 ? fmt(t, "((uint)%s & 0xffffu)", sraw) : fmt(t, "(uint)%s", sraw);
            break;
        }
        if (flt)
            out_c[i] = d16 ? fmt(t, "f32tof16(%s)", flt) : fmt(t, "asuint(%s)", flt);
    }
    if (d16) {
        for (u32 i = 0; i < comps; i += 2) {
            const char* packed =
                (i + 1 < comps) ? fmt(t, "(%s | (%s << 16))", out_c[i], out_c[i + 1]) : out_c[i];
            write_vgpr(t, in, in.dst[0].reg + i / 2, packed);
        }
    } else {
        for (u32 i = 0; i < comps; i++)
            write_vgpr(t, in, in.dst[0].reg + i, out_c[i]);
    }
    return true;
}

// Vertex element formats: components and bytes per component (0 for packed formats), and which
// input assembler component each component in memory becomes.
struct FormatLayout {
    u8 components;
    u8 component_bytes;
    u8 order[4];
    u8 value_class; // 0 float (also unorm/snorm), 1 uint, 2 sint
};

static bool vertex_format_layout(u32 format, FormatLayout* out) {
    static const u8 k_rgba[4] = {0, 1, 2, 3};
    u8 components = 0, bytes = 0;
    switch (format) {
    case 2:
    case 3:
    case 4:
        components = 4;
        bytes = 4;
        break; // R32G32B32A32
    case 6:
    case 7:
    case 8:
        components = 3;
        bytes = 4;
        break; // R32G32B32
    case 10:
    case 11:
    case 12:
    case 13:
    case 14:
        components = 4;
        bytes = 2;
        break; // R16G16B16A16
    case 16:
    case 17:
    case 18:
        components = 2;
        bytes = 4;
        break; // R32G32
    case 24:
    case 25:
    case 26:
        components = 1;
        bytes = 4;
        break; // packed 32-bit: one fetch
    case 28:
    case 29:
    case 30:
    case 31:
    case 32:
    case 87:
        components = 4;
        bytes = 1;
        break; // RGBA8, BGRA8
    case 34:
    case 35:
    case 36:
    case 37:
    case 38:
        components = 2;
        bytes = 2;
        break; // R16G16
    case 41:
    case 42:
    case 43:
        components = 1;
        bytes = 4;
        break; // R32
    case 49:
    case 50:
    case 51:
    case 52:
        components = 2;
        bytes = 1;
        break; // R8G8
    case 54:
    case 56:
    case 57:
    case 58:
    case 59:
        components = 1;
        bytes = 2;
        break; // R16
    case 61:
    case 62:
    case 63:
    case 64:
        components = 1;
        bytes = 1;
        break; // R8
    default:
        return false;
    }
    out->components = components;
    out->component_bytes = bytes;
    memcpy(out->order, k_rgba, 4);
    switch (format) {
    case 3:
    case 7:
    case 12:
    case 17:
    case 25:
    case 30:
    case 36:
    case 42:
    case 50:
    case 57:
    case 62:
        out->value_class = 1;
        break;
    case 4:
    case 8:
    case 14:
    case 18:
    case 32:
    case 38:
    case 43:
    case 52:
    case 59:
    case 64:
        out->value_class = 2;
        break;
    default:
        out->value_class = 0;
        break;
    }
    if (format == 87) { // B8G8R8A8: blue first in memory
        out->order[0] = 2;
        out->order[2] = 0;
    }
    return true;
}

static bool emit_vertex_fetch(Translator& t, const RdnaInst& in, u32 slot) {
    if (t.info->stage != SHADER_VERTEX || is_store(in.op) || in.mem.offen || is_d16_format(in.op)) {
        translator_fail(t, "unexpected access through a vertex buffer descriptor at 0x%x", in.offset);
        return false;
    }
    const RdnaOperand& so = in.src[2];
    if (!(so.kind == OPND_INLINE && so.reg == SRC_ZERO) && !(so.kind == OPND_SPECIAL && so.reg == SRC_NULL)) {
        translator_fail(t, "vertex fetch with a register offset at 0x%x", in.offset);
        return false;
    }
    // The element holding the fetched bytes; fetches may read some of its components only.
    u32 element = ~0u;
    FormatLayout layout = {};
    u32 offset = (u32)in.mem.offset;
    for (u32 i = 0; i < t.info->element_count; i++) {
        const VertexElement& e = t.info->elements[i];
        FormatLayout l;
        if (e.slot != slot || !vertex_format_layout(e.format, &l))
            continue;
        if (offset >= e.offset && offset < e.offset + (u32)l.components * l.component_bytes) {
            element = i;
            layout = l;
        }
    }
    if (element == ~0u) {
        translator_fail(t, "vertex fetch of slot %u offset %d matches no input element", slot, in.mem.offset);
        return false;
    }
    bool typed = is_format_access(in.op);
    u32 dwords = buffer_dwords(in);
    u32 first = offset - t.info->elements[element].offset; // bytes into the element
    if (typed)
        first = layout.component_bytes ? first / layout.component_bytes : 0;
    else if (first % 4) {
        translator_fail(t, "raw vertex fetch inside a dword at 0x%x", in.offset);
        return false;
    } else
        first /= 4;
    if ((t.out->vertex_element_mask & (1u << element)) && t.fetch_typed[element] != typed) {
        translator_fail(t, "vertex element %u fetched both raw and formatted", element);
        return false;
    }
    t.out->vertex_element_mask |= 1u << element;
    t.fetch_typed[element] = typed;
    t.fetch_class[element] = layout.value_class;
    // Typed elements always read all four components (missing ones read as 0, 0, 0, 1).
    u32 needed = typed ? 4 : first + dwords;
    if (needed > t.vs_fetch_components[element])
        t.vs_fetch_components[element] = (u8)needed;
    if (first + dwords > 4) {
        translator_fail(t, "vertex fetch past its element at 0x%x", in.offset);
        return false;
    }
    static const char k_comp[4] = {'x', 'y', 'z', 'w'};
    for (u32 i = 0; i < dwords; i++) {
        u32 c = typed ? (first + i < layout.components ? layout.order[first + i] : first + i) : first + i;
        const char* value = fmt(t, "input.e%u.%c", element, k_comp[c & 3]);
        write_vgpr(t, in, in.dst[0].reg + i,
                   typed && layout.value_class != 1 ? fmt(t, "asuint(%s)", value) : value);
    }
    return true;
}

bool emit_fetch_subroutine(Translator& t, const RdnaInst& in) {
    bool any = false;
    for (u32 i = 0; i < t.info->element_count; i++)
        any |= t.info->fetch_components[i] != 0;
    if (t.info->stage != SHADER_VERTEX || !any) {
        translator_fail(t, "s_swappc_b64 at 0x%x without a fetch subroutine to call", in.offset);
        return false;
    }
    static const char k_comp[4] = {'x', 'y', 'z', 'w'};
    for (u32 i = 0; i < t.info->element_count; i++) {
        u32 count = t.info->fetch_components[i];
        if (!count)
            continue;
        FormatLayout layout;
        if (!vertex_format_layout(t.info->elements[i].format, &layout)) {
            translator_fail(t, "vertex element %u in format %u", i, t.info->elements[i].format);
            return false;
        }
        if ((t.out->vertex_element_mask & (1u << i)) && !t.fetch_typed[i]) {
            translator_fail(t, "vertex element %u fetched both raw and formatted", i);
            return false;
        }
        t.out->vertex_element_mask |= 1u << i;
        t.fetch_typed[i] = true;
        t.fetch_class[i] = layout.value_class;
        t.vs_fetch_components[i] = 4; // typed: all four components, missing ones read as 0, 0, 0, 1
        for (u32 c = 0; c < count; c++) {
            u32 component = c < layout.components ? layout.order[c] : c;
            const char* value = fmt(t, "input.e%u.%c", i, k_comp[component & 3]);
            write_vgpr(t, in, t.info->fetch_vgpr[i] + c,
                       layout.value_class != 1 ? fmt(t, "asuint(%s)", value) : value);
        }
    }
    // The return address, which the subroutine jumps back to.
    write_sgpr(t, in.dst[0], 0, fmt(t, "0x%08xu", in.offset + 4));
    write_sgpr(t, in.dst[0], 1, "0u");
    return true;
}

// One access through the desktop view of a heap buffer descriptor (read-write or read-only) as `b_`:
// formatted through a typed view (`comps` components at index `element`), or raw at the byte address
// `address` computes into a_.
static bool emit_heap_buffer_access(Translator& t, const RdnaInst& in, bool read_write,
                                    const char* heap_index, const char* element, const char* address,
                                    u32 dwords, u32 comps) {
    bool store = is_store(in.op);
    bool d16 = is_d16_format(in.op);
    if (comps) {
        emit(t, "%s<float4> b_ = ResourceDescriptorHeap[%s];", read_write ? "RWBuffer" : "Buffer",
             heap_index);
        if (store) {
            const char* c[4];
            for (u32 i = 0; i < 4; i++) {
                if (i < comps) {
                    if (d16) {
                        const char* r = vgpr(t, in.dst[0].reg + i / 2);
                        c[i] = (i & 1) ? fmt(t, "f16tof32(%s >> 16)", r) : fmt(t, "f16tof32(%s)", r);
                    } else {
                        c[i] = fmt(t, "asfloat(%s)", vgpr(t, in.dst[0].reg + i));
                    }
                } else {
                    c[i] = "0.0f";
                }
            }
            emit(t, "if (%s) b_[%s] = float4(%s, %s, %s, %s);", exec_cond(t), element, c[0], c[1], c[2],
                 c[3]);
            return true;
        }
        emit(t, "float4 r_ = b_[%s];", element);
        static const char k_comp[4] = {'x', 'y', 'z', 'w'};
        if (d16) {
            for (u32 i = 0; i < comps; i += 2) {
                const char* packed = (i + 1 < comps) ? fmt(t, "(f32tof16(r_.%c) | (f32tof16(r_.%c) << 16))",
                                                           k_comp[i], k_comp[i + 1])
                                                     : fmt(t, "f32tof16(r_.%c)", k_comp[i]);
                write_vgpr(t, in, in.dst[0].reg + i / 2, packed);
            }
        } else {
            for (u32 i = 0; i < comps; i++)
                write_vgpr(t, in, in.dst[0].reg + i, fmt(t, "asuint(r_.%c)", k_comp[i]));
        }
        return true;
    }
    emit(t, "%s b_ = ResourceDescriptorHeap[%s];", read_write ? "RWByteAddressBuffer" : "ByteAddressBuffer",
         heap_index);
    emit(t, "%s", address);
    if (is_buffer_atomic(in.op))
        return emit_buffer_atomic(t, in, "b_", "a_");
    if (in.enc == ENC_MTBUF)
        return emit_mtbuf_access(t, in, "b_", "a_", store);
    if (store) {
        char list[96] = {};
        usize n = 0;
        for (u32 i = 0; i < dwords; i++)
            n += (usize)snprintf(list + n, sizeof(list) - n, "%s%s", i ? ", " : "",
                                 vgpr(t, in.dst[0].reg + i));
        emit(t, "if (%s) b_.%s(a_, %s%s%s);", exec_cond(t), k_store_fn[dwords],
             dwords > 1 ? fmt(t, "uint%u(", dwords) : "", list, dwords > 1 ? ")" : "");
        return true;
    }
    write_loaded(t, in, dwords, fmt(t, "b_.%s(a_)", k_load_fn[dwords]));
    return true;
}

static bool emit_buffer(Translator& t, const RdnaInst& in) {
    if (in.op == OP_BUFFER_GL0_INV || in.op == OP_BUFFER_GL1_INV)
        return true;
    u32 rsrc = in.src[1].reg;
    Tag lo = sgpr_tag(t, rsrc), hi = sgpr_tag(t, rsrc + 1);
    if (tag_kind(lo) == TAG_VB_DESC)
        return emit_vertex_fetch(t, in, tag_payload(lo));
    if (in.mem.lds || in.mem.tfe) {
        translator_fail(t, "buffer access with LDS or TFE at 0x%x", in.offset);
        return false;
    }
    bool store = is_store(in.op);
    bool format = is_format_access(in.op);
    bool d16 = is_d16_format(in.op);
    bool atomic = is_buffer_atomic(in.op);
    u32 dwords = store ? in.src[1].count : buffer_dwords(in);
    if (store)
        dwords = (u32)((in.op >= OP_TBUFFER_STORE_FORMAT_D16_X)  ? in.op - OP_TBUFFER_STORE_FORMAT_D16_X + 1
                       : (in.op >= OP_TBUFFER_STORE_FORMAT_X)    ? in.op - OP_TBUFFER_STORE_FORMAT_X + 1
                       : (in.op >= OP_BUFFER_STORE_FORMAT_D16_X) ? in.op - OP_BUFFER_STORE_FORMAT_D16_X + 1
                       : (in.op <= OP_BUFFER_STORE_FORMAT_XYZW)  ? in.op - OP_BUFFER_STORE_FORMAT_X + 1
                       : in.op == OP_BUFFER_STORE_DWORDX2        ? 2
                       : in.op == OP_BUFFER_STORE_DWORDX3        ? 3
                       : in.op == OP_BUFFER_STORE_DWORDX4        ? 4
                                                                 : 1);
    bool sub_dword = (in.op >= OP_BUFFER_LOAD_UBYTE && in.op <= OP_BUFFER_LOAD_SSHORT) ||
                     (in.op >= OP_BUFFER_STORE_BYTE && in.op <= OP_BUFFER_STORE_SHORT_D16_HI) ||
                     in.op >= OP_BUFFER_LOAD_UBYTE_D16 && in.op <= OP_BUFFER_LOAD_SHORT_D16_HI;
    if (sub_dword || (in.op >= OP_BUFFER_ATOMIC_FCMPSWAP && in.op <= OP_BUFFER_ATOMIC_FMAX)) {
        translator_fail(t, "unsupported %s", rdna_op_name(in.op));
        return false;
    }
    u32 vaddr = in.src[0].reg;
    u32 voffset_reg = vaddr + (in.mem.idxen ? 1 : 0);
    const char* index = in.mem.idxen ? vgpr_data(t, vaddr) : "0u";
    const char* voffset = in.mem.offen ? vgpr_data(t, voffset_reg) : "0u";
    const char* offset = fmt(t, "(%s + 0x%xu + %s)", voffset, (u32)in.mem.offset, soffset(t, in, 2));

    if (tag_kind(hi) == TAG_ROOT_HI) {
        if (format && in.enc != ENC_MTBUF) {
            translator_fail(t, "formatted access to a root buffer at 0x%x", in.offset);
            return false;
        }
        u32 parameter = tag_payload(hi);
        t.root_used |= 1ull << parameter;
        // Stride comes from the descriptor's second dword, which the program built itself.
        emit(t, "{ uint a_ = %s + %s * ((%s >> 16) & 0x3fffu) + %s;", sgpr(t, rsrc), index, sgpr(t, rsrc + 1),
             offset);
        t.depth++;
        if (atomic) {
            if (t.info->parameters[parameter].kind != USER_DATA_ROOT_UAV) {
                translator_fail(t, "atomic on a read-only root buffer at 0x%x", in.offset);
                return false;
            }
            if (!emit_buffer_atomic(t, in, fmt(t, "g_root%u", parameter), "a_"))
                return false;
        } else if (in.enc == ENC_MTBUF) {
            if (store && t.info->parameters[parameter].kind != USER_DATA_ROOT_UAV) {
                translator_fail(t, "store to a read-only root buffer at 0x%x", in.offset);
                return false;
            }
            if (!emit_mtbuf_access(t, in, fmt(t, "g_root%u", parameter), "a_", store))
                return false;
        } else if (store) {
            if (t.info->parameters[parameter].kind != USER_DATA_ROOT_UAV) {
                translator_fail(t, "store to a read-only root buffer at 0x%x", in.offset);
                return false;
            }
            const char* data = vgpr(t, in.dst[0].reg);
            if (dwords > 1) {
                char list[96] = {};
                usize n = 0;
                for (u32 i = 0; i < dwords; i++)
                    n += (usize)snprintf(list + n, sizeof(list) - n, "%s%s", i ? ", " : "",
                                         vgpr(t, in.dst[0].reg + i));
                data = fmt(t, "uint%u(%s)", dwords, list);
            }
            emit(t, "if (%s) g_root%u.%s(a_, %s);", exec_cond(t), parameter, k_store_fn[dwords], data);
        } else {
            write_loaded(t, in, dwords, fmt(t, "g_root%u.%s(a_)", parameter, k_load_fn[dwords]));
        }
        t.depth--;
        emit(t, "}");
        return true;
    }
    if (tag_kind(lo) == TAG_HEAP && (tag_payload(lo) & 1) == HEAP_RESOURCES) {
        bool typed = format && in.enc != ENC_MTBUF; // through a typed desktop view
        DescriptorOrigin origin =
            descriptor_origin(t, rsrc, 4, HEAP_RESOURCES, buffer_fields_unused(typed, in.mem.idxen));
        if (origin != ORIGIN_HEAP) {
            translator_fail(t, "%s at 0x%x through a buffer descriptor %s", rdna_op_name(in.op), in.offset,
                            origin_text(origin));
            return false;
        }
        ViewAccess access;
        if (!view_access(t, in, rsrc, store || atomic, false, &access))
            return false;
        // Kept out of the scratch ring: a branch on the view kind emits the access twice.
        char heap_index[64], element[160], address[320] = {};
        snprintf(heap_index, sizeof(heap_index), "(%s >> 5)", sgpr(t, rsrc));
        snprintf(element, sizeof(element), "%s", index);
        u32 comps = 0;
        if (typed) {
            const RdnaOperand& so = in.src[2];
            bool no_soffset = (so.kind == OPND_SPECIAL && so.reg == SRC_NULL) ||
                              (so.kind == OPND_INLINE && so.reg == SRC_ZERO);
            if (in.mem.offen || in.mem.offset || !no_soffset) {
                translator_fail(t, "%s with an offset or format at 0x%x", rdna_op_name(in.op), in.offset);
                return false;
            }
            comps =
                store
                    ? dwords
                    : (u32)(d16 ? in.op - OP_BUFFER_LOAD_FORMAT_D16_X : in.op - OP_BUFFER_LOAD_FORMAT_X) + 1;
            if (comps == 0 || comps > 4) {
                translator_fail(t, "%s at 0x%x: bad component count", rdna_op_name(in.op), in.offset);
                return false;
            }
        }
        if (!typed) {
            // Indexed: the console's stride, from the descriptor's second dword as the program has it
            // (the shadow's copy of the slot's while it is unchanged).
            const char* stride = "0u";
            if (in.mem.idxen)
                stride = fmt(t, "((%s >> 16) & 0x3fffu)", sgpr_data(t, rsrc + 1));
            if (snprintf(address, sizeof(address), "uint a_ = %s * %s + %s;", index, stride, offset) >=
                (int)sizeof(address))
                translator_fail(t, "expression too long");
        }
        emit(t, "{");
        t.depth++;
        for (ViewPass v = view_begin(access, rsrc); view_next(t, v);)
            if (!emit_heap_buffer_access(t, in, v.read_write, heap_index, element, address, dwords, comps))
                return false;
        t.depth--;
        emit(t, "}");
        return true;
    }
    if (!store && !atomic && !format && tag_kind(lo) == TAG_PC) {
        s32 pid = register_pc_pool(t, rsrc);
        if (pid >= 0) {
            emit(t, "{ uint wi_ = (%s * ((%s >> 16) & 0x3fffu) + %s) >> 2;", index, sgpr(t, rsrc + 1),
                 offset);
            t.depth++;
            for (u32 i = 0; i < dwords; i++)
                write_vgpr(t, in, in.dst[0].reg + i,
                           fmt(t, "g_pc_%d[min(wi_ + %uu, g_pc_%d_end)]", pid, i, pid));
            t.depth--;
            emit(t, "}");
            return true;
        }
    }
    DescriptorOrigin origin = descriptor_origin(t, rsrc, 4, HEAP_RESOURCES);
    if (origin == ORIGIN_MODIFIED || origin == ORIGIN_MIXED)
        translator_fail(t, "%s at 0x%x through a buffer descriptor %s", rdna_op_name(in.op), in.offset,
                        origin_text(origin));
    else {
        char why[96];
        describe_tag_loss(t, false, rsrc, why, sizeof(why));
        translator_fail(t, "%s at 0x%x through a descriptor the translator cannot place%s",
                        rdna_op_name(in.op), in.offset, why);
    }
    return false;
}

// --- images ------------------------------------------------------------------------------------------

// Samplers built from constants: the descriptor's fields in D3D12 terms, for a sampling instruction
// that compares (an _c one) or not: the descriptor's comparison function applies only to those. False
// (with the reason) for what a desktop static sampler can't express.
static bool decode_sampler(const u32 w[4], bool compare, InlineSampler* out, const char** why) {
    static const u8 k_address[8] = {1, 2, 3, 5, 4, 5, 4, 5}; // wrap, mirror, clamp, mirror once, border
    memset(out, 0, sizeof(*out));
    for (u32 a = 0; a < 3; a++)
        out->address[a] = k_address[(w[0] >> (3 * a)) & 7];
    u32 aniso_ratio = (w[0] >> 9) & 7;
    u32 compare_func = (w[0] >> 12) & 7;
    out->unnormalized = (w[0] >> 15) & 1;
    u32 filter_mode = (w[0] >> 29) & 3; // blend, min, max
    out->min_lod = (f32)(w[1] & 0xFFF) / 256.0f;
    out->max_lod = (f32)((w[1] >> 12) & 0xFFF) / 256.0f;
    out->mip_bias = (f32)((s32)(w[2] << 18) >> 18) / 256.0f;
    // Filters: point, bilinear, then anisotropic over point or bilinear; mip none, point, linear, and
    // point with an anisotropic adjustment.
    u32 mag = (w[2] >> 20) & 3, min = (w[2] >> 22) & 3, mip = (w[2] >> 26) & 3;
    out->border_color = (u8)((w[3] >> 30) & 3);
    if (out->border_color == 3) {
        *why = "a border color from the border color table";
        return false;
    }
    if (filter_mode == 3) {
        *why = "an unknown reduction mode";
        return false;
    }
    if (compare && filter_mode) {
        *why = "a comparison with a min/max reduction";
        return false;
    }
    // D3D12's reduction field takes one of standard, comparison, minimum, maximum.
    u32 reduction = compare ? 1 : filter_mode == 1 ? 2 : filter_mode == 2 ? 3 : 0;
    bool aniso = mag >= 2 || min >= 2;
    if (aniso)
        out->filter = mip == 2 ? 0x55 : 0x54; // anisotropic, with linear or point mips
    else
        out->filter = (min == 1 ? 0x10 : 0) | (mag == 1 ? 0x4 : 0) | (mip == 2 ? 0x1 : 0);
    out->filter |= reduction << 7;
    out->comparison = (u8)(compare ? compare_func + 1 : 1); // D3D12's functions are the console's + 1
    out->max_anisotropy = (u8)(1u << (aniso_ratio > 4 ? 4 : aniso_ratio));
    if (out->max_lod >= 15.99f)
        out->max_lod = 3.402823466e+38f; // saturated: no limit
    if (mip == 0) {
        // No mipmapping: always the view's first level, whatever the LOD.
        out->min_lod = 0.0f;
        out->max_lod = 0.0f;
    }
    if (out->unnormalized) {
        // What desktop samplers with unnormalized coordinates allow.
        bool clamped =
            (out->address[0] == 3 || out->address[0] == 4) && (out->address[1] == 3 || out->address[1] == 4);
        if (!clamped || aniso || min != mag || compare || out->max_lod != 0.0f || out->min_lod != 0.0f) {
            *why = "unnormalized coordinates with wrapping, mips, anisotropy, comparison or unequal filters";
            return false;
        }
        // With the level held at 0 the mip filter and the bias change nothing: point mips and no bias,
        // the form the desktop rules for unnormalized samplers (Vulkan's) are written for.
        out->filter &= ~1u;
        out->mip_bias = 0.0f;
    }
    return true;
}

// The program's inline sampler for a descriptor, added on first use; -1 (failed) if impossible. A
// descriptor used both with and without comparison makes two samplers (their filters differ).
static s32 add_inline_sampler(Translator& t, const InlineSampler& s, bool compare) {
    TranslatedShader& out = *t.out;
    u32 i = 0;
    while (i < out.sampler_count && memcmp(&out.samplers[i], &s, sizeof(s)) != 0)
        i++;
    if (i == out.sampler_count) {
        if (i == SHADER_MAX_INLINE_SAMPLERS) {
            translator_fail(t, "more than %d constant samplers", SHADER_MAX_INLINE_SAMPLERS);
            return -1;
        }
        out.samplers[out.sampler_count++] = s;
        if (compare)
            t.sampler_compare |= 1u << i;
    }
    return (s32)i;
}

static s32 inline_sampler(Translator& t, const RdnaInst& in, const u32 words[4], bool compare) {
    InlineSampler s;
    const char* why = nullptr;
    if (!decode_sampler(words, compare, &s, &why)) {
        translator_fail(t, "%s at 0x%x with a constant sampler using %s", rdna_op_name(in.op), in.offset,
                        why);
        return -1;
    }
    return add_inline_sampler(t, s, compare);
}

// The sampler of a sampling instruction as `smp_`: a static sampler for a descriptor made of constants,
// the heap's sampler for one loaded from a sampler table.
static bool emit_sampler(Translator& t, const RdnaInst& in, bool compare) {
    u32 samp = in.src[2].reg;
    const char* type = compare ? "SamplerComparisonState" : "SamplerState";
    DescriptorOrigin origin = descriptor_origin(t, samp, 4, HEAP_SAMPLERS);
    if (origin == ORIGIN_CONSTANT) {
        u32 words[4];
        for (u32 i = 0; i < 4; i++)
            sgpr_constant(t.tags, samp + i, &words[i]);
        s32 index = inline_sampler(t, in, words, compare);
        if (index < 0)
            return false;
        emit(t, "%s smp_ = g_sampler%d;", type, index);
        return true;
    }
    if (origin == ORIGIN_HEAP) {
        emit(t, "%s smp_ = SamplerDescriptorHeap[%s >> 4];", type, sgpr(t, samp));
        return true;
    }
    translator_fail(t, "%s at 0x%x with a sampler %s", rdna_op_name(in.op), in.offset, origin_text(origin));
    return false;
}

struct ImageForm {
    bool sample, gather, load, store, mip, lod, bias, lod_zero, compare, grad, clamp, offset;
};

static ImageForm image_form(RdnaOp op) {
    ImageForm f = {};
    const char* name = rdna_op_name(op);
    if (op >= OP_IMAGE_SAMPLE && op <= OP_IMAGE_SAMPLE_C_LZ_O)
        f.sample = true;
    else if (op >= OP_IMAGE_GATHER4 && op <= OP_IMAGE_GATHER4_C_LZ_O)
        f.gather = true;
    else if (op <= OP_IMAGE_LOAD_MIP_PCK_SGN || op == OP_IMAGE_MSAA_LOAD)
        f.load = true;
    else if (op >= OP_IMAGE_STORE && op <= OP_IMAGE_STORE_MIP_PCK)
        f.store = true;
    f.mip = op == OP_IMAGE_LOAD_MIP || op == OP_IMAGE_STORE_MIP;
    if (f.sample || f.gather) {
        // Modifiers follow the operation name: "image_sample" or "image_gather4", then "_c_lz_o" etc.
        const char* suffix = strchr(name + 6, '_');
        for (const char* s = suffix; s && *s; s = strchr(s + 1, '_')) {
            if (!strncmp(s, "_lz", 3))
                f.lod_zero = true;
            else if (!strncmp(s, "_l", 2) && (s[2] == '_' || !s[2]))
                f.lod = true;
            else if (!strncmp(s, "_b", 2) && (s[2] == '_' || !s[2]))
                f.bias = true;
            else if (!strncmp(s, "_c", 2) && (s[2] == '_' || !s[2]))
                f.compare = true;
            else if (!strncmp(s, "_cl", 3))
                f.clamp = true;
            else if (!strncmp(s, "_d", 2) && (s[2] == '_' || !s[2]))
                f.grad = true;
            else if (!strncmp(s, "_o", 2) && (s[2] == '_' || !s[2]))
                f.offset = true;
        }
    }
    return f;
}

// Every cube view is a cube array on desktop (the runtime makes TEXTURECUBE views TEXTURECUBEARRAY):
// console shaders read either kind through one cube instruction. Cube gradients are face-space 2D.
static const char* const k_texture_types[8] = {"Texture1D",        "Texture2D",       "Texture3D",
                                               "TextureCubeArray", "Texture1DArray",  "Texture2DArray",
                                               "Texture2DMS",      "Texture2DMSArray"};
// Read-write views by dimension: a cube's is an array of its faces; multisampled images have none.
static const char* const k_rw_texture_types[8] = {"RWTexture1D",
                                                  "RWTexture2D",
                                                  "RWTexture3D",
                                                  "RWTexture2DArray",
                                                  "RWTexture1DArray",
                                                  "RWTexture2DArray",
                                                  "",
                                                  ""};
static const u8 k_dim_coords[8] = {1, 2, 3, 3, 2, 3, 2, 3};
static const u8 k_dim_grads[8] = {1, 2, 3, 2, 1, 2, 2, 2};

// Image address registers. With 16-bit addresses, components after the first `unpacked` share
// registers two by two (low half first); the unpacked ones keep a whole register each, and the bias
// among them is a half in the low 16 bits of its register.
struct ImageAddress {
    u32 regs[16];
    u32 unpacked;
    s32 half_at; // the bias component, or -1
    bool a16;
};

// Component `i` as uint bits.
static const char* address_bits(Translator& t, const ImageAddress& a, s32 i) {
    if (!a.a16 || (u32)i < a.unpacked)
        return vgpr_data(t, a.regs[i]);
    u32 j = (u32)i - a.unpacked;
    const char* reg = vgpr_data(t, a.regs[a.unpacked + j / 2]);
    return j & 1 ? fmt(t, "(%s >> 16)", reg) : fmt(t, "(%s & 0xffffu)", reg);
}

// Component `i` as a float.
static const char* address_value(Translator& t, const ImageAddress& a, s32 i) {
    if (a.a16 && i == a.half_at)
        return fmt(t, "f16tof32(%s)", vgpr_data(t, a.regs[i]));
    if (!a.a16 || (u32)i < a.unpacked)
        return fmt(t, "asfloat(%s)", address_bits(t, a, i));
    return fmt(t, "f16tof32(%s)", address_bits(t, a, i));
}

// Components [first, first + n) as a float or uint vector.
static const char* address_vector(Translator& t, const ImageAddress& a, bool floats, u32 first, u32 n) {
    char list[240] = {};
    usize len = 0;
    for (u32 i = 0; i < n; i++)
        len += (usize)snprintf(list + len, sizeof(list) - len, "%s%s", i ? ", " : "",
                               floats ? address_value(t, a, (s32)(first + i))
                                      : address_bits(t, a, (s32)(first + i)));
    if (n == 1)
        return fmt(t, "%s", list);
    return fmt(t, "%s%u(%s)", floats ? "float" : "uint", n, list);
}

// An image size query (image_get_resinfo) into w_, h_, d_ and l_: read-only views have mips to ask
// about, read-write views are of one mip.
static bool emit_image_size(Translator& t, const RdnaInst& in, bool read_write) {
    const char* mip = vgpr_data(t, in.src[0].reg);
    switch (in.mem.dim) {
    case DIM_1D:
        if (read_write)
            emit(t, "tex_.GetDimensions(w_);");
        else
            emit(t, "tex_.GetDimensions(%s, w_, l_);", mip);
        return true;
    case DIM_2D:
        if (read_write)
            emit(t, "tex_.GetDimensions(w_, h_);");
        else
            emit(t, "tex_.GetDimensions(%s, w_, h_, l_);", mip);
        return true;
    case DIM_CUBE:
        if (read_write) {
            emit(t, "tex_.GetDimensions(w_, h_, d_); d_ = 1u;"); // a cube's read-write view is an array
        } else {
            // Desktop counts cubes; the hardware counts faces, one per slice as for 2D arrays.
            emit(t, "tex_.GetDimensions(%s, w_, h_, d_, l_);", mip);
            emit(t, "d_ *= 6u;");
        }
        return true;
    case DIM_3D:
    case DIM_2D_ARRAY:
        if (read_write)
            emit(t, "tex_.GetDimensions(w_, h_, d_);");
        else
            emit(t, "tex_.GetDimensions(%s, w_, h_, d_, l_);", mip);
        return true;
    case DIM_1D_ARRAY:
        if (read_write)
            emit(t, "tex_.GetDimensions(w_, h_);");
        else
            emit(t, "tex_.GetDimensions(%s, w_, h_, l_);", mip);
        return true;
    default:
        translator_fail(t, "resource info of dimension %u", in.mem.dim);
        return false;
    }
}

// Implicit LOD needs derivatives. Compute groups whose shape desktop can't take them in (1D groups not a
// multiple of 4 threads, other groups with odd X or Y) have lone lanes in their quads on console too:
// zero derivatives, so the LOD is the bias alone.
static bool implicit_lod_usable(const Translator& t) {
    if (t.info->stage != SHADER_COMPUTE)
        return true;
    const u16* n = t.info->threads;
    bool one_d = n[1] == 1 && n[2] == 1;
    return one_d ? n[0] % 4 == 0 : n[0] % 2 == 0 && n[1] % 2 == 0;
}

bool image_reads_neighbours(const Translator& t, RdnaOp op) {
    if (op == OP_IMAGE_GET_LOD)
        return implicit_lod_usable(t);
    ImageForm f = image_form(op);
    return (f.sample || f.gather) && !f.lod && !f.lod_zero && !f.grad && implicit_lod_usable(t);
}

// A finite float as an HLSL literal.
static const char* float_literal(Translator& t, f32 value) {
    char digits[32];
    snprintf(digits, sizeof(digits), "%.9g", value);
    bool point = strpbrk(digits, ".e") != nullptr;
    return fmt(t, "%s%sf", digits, point ? "" : ".0");
}

// A gather that picks a mip (an explicit LOD, an implicit one, a bias or a clamp), as `tex_` is declared.
// Desktop gathers read the view's most detailed mip only, and no shader model has one that takes a LOD:
// four point samples at the centres of the 2x2 texels the console gathers from that mip, which keep the
// sampler's address modes and border. Returns the result's expression, or nullptr (failed).
static const char* emit_gather_at_mip(Translator& t, const RdnaInst& in, const ImageForm& form,
                                      const ImageAddress& addr, u32 coord_at, s32 lod_at, s32 bias_at,
                                      s32 clamp_at, const s32 texel_offset[3]) {
    const RdnaMemory& m = in.mem;
    if (form.compare) {
        translator_fail(t, "%s at 0x%x: a comparing gather from a mip it picks", rdna_op_name(in.op),
                        in.offset);
        return nullptr;
    }
    if (m.dim != DIM_2D && m.dim != DIM_2D_ARRAY) {
        translator_fail(t, "%s at 0x%x of dimension %u", rdna_op_name(in.op), in.offset, m.dim);
        return nullptr;
    }
    bool array = m.dim == DIM_2D_ARRAY;
    // A sampler made of constants applies its bias and LOD clamps here, to pick the mip whose size the
    // taps are placed by; the taps go through a copy without them (and without filtering), so that they
    // read exactly that mip's texels. A heap sampler is used as it is: exact unless it biases or clamps
    // the LOD itself, which translated code can't see.
    u32 samp = in.src[2].reg;
    DescriptorOrigin origin = descriptor_origin(t, samp, 4, HEAP_SAMPLERS);
    InlineSampler constant = {};
    if (origin == ORIGIN_CONSTANT) {
        u32 words[4];
        for (u32 i = 0; i < 4; i++)
            sgpr_constant(t.tags, samp + i, &words[i]);
        const char* why = nullptr;
        if (!decode_sampler(words, false, &constant, &why)) {
            translator_fail(t, "%s at 0x%x with a constant sampler using %s", rdna_op_name(in.op), in.offset,
                            why);
            return nullptr;
        }
        InlineSampler tap = constant;
        tap.filter = 0; // point min, mag and mip filters, no min/max reduction
        tap.max_anisotropy = 1;
        tap.mip_bias = 0.0f;
        tap.min_lod = 0.0f;
        tap.max_lod = 3.402823466e+38f;
        tap.unnormalized = false; // taps are placed in normalized coordinates
        s32 index = add_inline_sampler(t, tap, false);
        if (index < 0)
            return nullptr;
        emit(t, "SamplerState smp_ = g_sampler%d;", index);
    } else if (origin == ORIGIN_HEAP) {
        emit(t, "SamplerState smp_ = SamplerDescriptorHeap[%s >> 4]; // its LOD bias and clamps aren't seen",
             sgpr(t, samp));
    } else {
        translator_fail(t, "%s at 0x%x with a sampler %s", rdna_op_name(in.op), in.offset,
                        origin_text(origin));
        return nullptr;
    }
    const char* uv = address_vector(t, addr, true, coord_at, 2);
    const char* level;
    if (form.lod) {
        level = address_value(t, addr, lod_at);
    } else {
        level =
            implicit_lod_usable(t) ? fmt(t, "tex_.CalculateLevelOfDetailUnclamped(smp_, %s)", uv) : "0.0f";
        if (form.bias)
            level = fmt(t, "%s + %s", level, address_value(t, addr, bias_at));
    }
    if (origin == ORIGIN_CONSTANT)
        level = fmt(t, "clamp(%s + %s, %s, %s)", level, float_literal(t, constant.mip_bias),
                    float_literal(t, constant.min_lod), float_literal(t, constant.max_lod));
    if (form.clamp)
        level = fmt(t, "max(%s, %s)", level, address_value(t, addr, clamp_at));
    const char* elements = array ? "e_, " : "";
    emit(t, "float l_ = %s;", level);
    emit(t, "uint w_, h_, e_, n_; tex_.GetDimensions(0u, w_, h_, %sn_);", elements);
    emit(t, "uint m_ = (uint)clamp(floor(l_ + 0.5f), 0.0f, (float)(max(n_, 1u) - 1u));");
    emit(t, "tex_.GetDimensions(m_, w_, h_, %sn_);", elements);
    emit(t, "float2 sz_ = float2(w_, h_);");
    // The top-left texel of the footprint (texel coordinates of its centre); unnormalized coordinates
    // are in texels already, and keep to mip 0 (see decode_sampler).
    emit(t, "float2 b_ = floor(%s - 0.5f) + float2(%.1ff, %.1ff);",
         constant.unnormalized ? uv : fmt(t, "%s * sz_", uv), texel_offset[0] + 0.5f, texel_offset[1] + 0.5f);
    const char* slice = array ? fmt(t, ", %s", address_value(t, addr, coord_at + 2)) : "";
    // Gather order: (0, 1), (1, 1), (1, 0), (0, 0) from the top-left texel.
    static const u32 k_corner[4][2] = {{0, 1}, {1, 1}, {1, 0}, {0, 0}};
    for (u32 i = 0; i < 4; i++)
        emit(t, "float4 t%u_ = tex_.SampleLevel(smp_, %s((b_ + float2(%u, %u)) / sz_%s), (float)m_);", i,
             array ? "float3" : "", k_corner[i][0], k_corner[i][1], slice);
    char c = "xyzw"[m.dmask & 2 ? 1 : m.dmask & 4 ? 2 : m.dmask & 8 ? 3 : 0];
    return fmt(t, "float4(t0_.%c, t1_.%c, t2_.%c, t3_.%c)", c, c, c, c);
}

// An image atomic through a read-write view of 32- or 64-bit integer texels (`coords` uint components
// of `addr` from `coord_at`). The data registers hold one value, or for a compare-swap the value and
// then what the texel is compared with; dmask has a bit per dword of it. With glc the texel's old value
// comes back in the first registers.
static bool emit_image_atomic(Translator& t, const RdnaInst& in, const ImageAddress& addr, u32 coord_at,
                              u32 coords, const char* resource) {
    const RdnaMemory& m = in.mem;
    bool cmpswap = in.op == OP_IMAGE_ATOMIC_CMPSWAP;
    u32 data_dwords = m.dmask == 0x1 ? 1 : m.dmask == 0x3 ? 2 : m.dmask == 0xF ? 4 : 0;
    u32 dwords = cmpswap ? data_dwords / 2 : data_dwords; // per value
    if ((dwords != 1 && dwords != 2) || m.d16 || !k_rw_texture_types[m.dim][0]) {
        translator_fail(t, "%s at 0x%x with dmask 0x%x%s, dimension %u", rdna_op_name(in.op), in.offset,
                        m.dmask, m.d16 ? " (16-bit)" : "", m.dim);
        return false;
    }
    bool wide = dwords == 2;
    bool is_signed = in.op == OP_IMAGE_ATOMIC_SMIN || in.op == OP_IMAGE_ATOMIC_SMAX;
    bool inc_dec = in.op == OP_IMAGE_ATOMIC_INC || in.op == OP_IMAGE_ATOMIC_DEC;
    if (wide && inc_dec) {
        translator_fail(t, "64-bit %s at 0x%x", rdna_op_name(in.op), in.offset);
        return false;
    }
    // Typed atomics take their signedness from the view's element type.
    const char* type = wide ? (is_signed ? "int64_t" : "uint64_t") : (is_signed ? "int" : "uint");
    u32 data = in.dst[0].reg;
    const char* value =
        wide ? fmt(t, "((uint64_t)%s << 32 | %s)", vgpr(t, data + 1), vgpr(t, data)) : vgpr(t, data);
    if (is_signed)
        value = fmt(t, "(%s)%s", type, value);
    if (wide)
        t.out->uses_int64_atomics = true;
    bool returns = m.glc;
    emit(t, "{ %s<%s> tex_ = ResourceDescriptorHeap[%s];", k_rw_texture_types[m.dim], type, resource);
    t.depth++;
    emit(t, "uint%s at_ = %s;", coords > 1 ? fmt(t, "%u", coords) : "",
         address_vector(t, addr, false, coord_at, coords));
    emit(t, "%s o_ = 0;", type);
    if (cmpswap) {
        const char* compare = wide ? fmt(t, "((uint64_t)%s << 32 | %s)", vgpr(t, data + 3), vgpr(t, data + 2))
                                   : vgpr(t, data + 1);
        if (returns)
            emit(t, "if (%s) InterlockedCompareExchange(tex_[at_], %s, %s, o_);", exec_cond(t), compare,
                 value);
        else
            emit(t, "if (%s) InterlockedCompareStore(tex_[at_], %s, %s);", exec_cond(t), compare, value);
    } else if (inc_dec) {
        // No typed atomic does these: a compare-exchange loop, which needs no typed load to start from
        // (the first exchange returns the texel unless it already holds the guess).
        const char* next = in.op == OP_IMAGE_ATOMIC_INC
                               ? fmt(t, "c_ >= %s ? 0u : c_ + 1u", value)
                               : fmt(t, "(c_ == 0u || c_ > %s) ? %s : c_ - 1u", value, value);
        emit(t,
             "if (%s) { uint c_ = 0u, p_; [allow_uav_condition] for (;;) { "
             "InterlockedCompareExchange(tex_[at_], c_, %s, p_); if (p_ == c_) break; c_ = p_; } o_ = c_; }",
             exec_cond(t), next);
    } else {
        const char* fn = nullptr;
        switch (in.op) {
        case OP_IMAGE_ATOMIC_SWAP:
            fn = "InterlockedExchange";
            break;
        case OP_IMAGE_ATOMIC_ADD:
            fn = "InterlockedAdd";
            break;
        case OP_IMAGE_ATOMIC_SUB:
            fn = "InterlockedAdd";
            value = fmt(t, "(%s)0 - %s", type, value);
            break;
        case OP_IMAGE_ATOMIC_SMIN:
        case OP_IMAGE_ATOMIC_UMIN:
            fn = "InterlockedMin";
            break;
        case OP_IMAGE_ATOMIC_SMAX:
        case OP_IMAGE_ATOMIC_UMAX:
            fn = "InterlockedMax";
            break;
        case OP_IMAGE_ATOMIC_AND:
            fn = "InterlockedAnd";
            break;
        case OP_IMAGE_ATOMIC_OR:
            fn = "InterlockedOr";
            break;
        case OP_IMAGE_ATOMIC_XOR:
            fn = "InterlockedXor";
            break;
        default:
            translator_fail(t, "unsupported %s", rdna_op_name(in.op));
            return false;
        }
        // The exchange always returns the old value.
        if (returns || in.op == OP_IMAGE_ATOMIC_SWAP)
            emit(t, "if (%s) %s(tex_[at_], %s, o_);", exec_cond(t), fn, value);
        else
            emit(t, "if (%s) %s(tex_[at_], %s);", exec_cond(t), fn, value);
    }
    if (returns) {
        write_vgpr(t, in, data, "(uint)o_");
        if (wide)
            write_vgpr(t, in, data + 1, "(uint)((uint64_t)o_ >> 32)");
    }
    t.depth--;
    emit(t, "}");
    return true;
}

// Cube coordinates (s, t, face from `first` on) as the desktop direction and cube index in c_ (see
// cube_coord in the preamble).
static void emit_cube_coord(Translator& t, const ImageAddress& a, u32 first) {
    emit(t, "float4 c_ = cube_coord(%s, %s, %s);", address_value(t, a, (s32)first),
         address_value(t, a, (s32)first + 1), address_value(t, a, (s32)first + 2));
}

static bool emit_image(Translator& t, const RdnaInst& in) {
    const RdnaMemory& m = in.mem;
    ImageForm form = image_form(in.op);
    if (in.op == OP_IMAGE_BVH_INTERSECT_RAY || in.op == OP_IMAGE_BVH64_INTERSECT_RAY) {
        translator_fail(t, "unsupported %s", rdna_op_name(in.op));
        return false;
    }
    if (m.tfe || m.lwe) {
        translator_fail(t, "%s with residency feedback at 0x%x", rdna_op_name(in.op), in.offset);
        return false;
    }
    // Desktop cube sampling takes no texel offsets (no title uses them).
    if (m.dim == DIM_CUBE && form.offset) {
        translator_fail(t, "%s at 0x%x: cube sampling with texel offsets", rdna_op_name(in.op), in.offset);
        return false;
    }
    u32 rsrc = in.src[1].reg;
    Tag tag = sgpr_tag(t, rsrc);
    if (tag_kind(tag) != TAG_HEAP || (tag_payload(tag) & 1) != HEAP_RESOURCES) {
        DescriptorOrigin origin =
            descriptor_origin(t, rsrc, in.src[1].count ? in.src[1].count : 8, HEAP_RESOURCES);
        char why[96];
        describe_tag_loss(t, false, rsrc, why, sizeof(why));
        if (origin == ORIGIN_MODIFIED)
            translator_fail(t, "%s at 0x%x with an image descriptor %s%s", rdna_op_name(in.op), in.offset,
                            origin_text(origin), why);
        else
            translator_fail(t, "%s at 0x%x with a resource not from a descriptor table%s",
                            rdna_op_name(in.op), in.offset, why);
        return false;
    }
    DescriptorOrigin origin =
        descriptor_origin(t, rsrc, in.src[1].count ? in.src[1].count : 8, HEAP_RESOURCES);
    if (origin != ORIGIN_HEAP) {
        translator_fail(t, "%s at 0x%x with an image descriptor %s", rdna_op_name(in.op), in.offset,
                        origin_text(origin));
        return false;
    }
    // Address components in order: texel offsets, bias, comparison reference, gradients, then
    // coordinates, LOD (or mip, or sample index) and clamp. With 16-bit addresses only the components
    // from the coordinates on are packed two to a register (low half first): offsets, bias, reference
    // and gradients keep a register each (16-bit gradients come with opcodes of their own).
    u32 components = rdna_image_address_count(in.op, m.dim);
    u32 grads = k_dim_grads[m.dim];
    u32 unpacked =
        (form.offset ? 1 : 0) + (form.bias ? 1 : 0) + (form.compare ? 1 : 0) + (form.grad ? grads * 2 : 0);
    if (components > 16 || unpacked > components) {
        translator_fail(t, "%s at 0x%x: too many image address registers", rdna_op_name(in.op), in.offset);
        return false;
    }
    u32 registers = m.a16 ? unpacked + (components - unpacked + 1) / 2 : components;
    if (m.nsa_count && registers - 1 > m.nsa_count) {
        translator_fail(t, "%s at 0x%x: fewer address registers than the address needs", rdna_op_name(in.op),
                        in.offset);
        return false;
    }
    ImageAddress addr = {};
    for (u32 i = 0; i < registers; i++)
        addr.regs[i] = m.nsa_count && i > 0 ? m.nsa_addr[i - 1] : in.src[0].reg + i;
    addr.unpacked = unpacked;
    addr.a16 = m.a16;
    addr.half_at = -1;
    u32 at = 0;
    s32 offset_at = -1, bias_at = -1, compare_at = -1, lod_at = -1, clamp_at = -1, grad_at = -1;
    if (form.offset)
        offset_at = (s32)at++;
    if (form.bias) {
        bias_at = (s32)at++;
        addr.half_at = bias_at;
    }
    if (form.compare)
        compare_at = (s32)at++;
    if (form.grad) {
        grad_at = (s32)at;
        at += grads * 2;
    }
    u32 coords = k_dim_coords[m.dim];
    u32 coord_at = at;
    at += coords;
    if (form.lod || form.mip || in.op == OP_IMAGE_MSAA_LOAD)
        lod_at = (s32)at++;
    if (form.clamp)
        clamp_at = (s32)at++;
    if (at > components) {
        translator_fail(t, "%s at 0x%x: address layout mismatch", rdna_op_name(in.op), in.offset);
        return false;
    }
    // Cube arrays can be sampled with the 2D array dimension, the layer being cube index * 8 + face. A
    // Texture2DArray reading a cube array view would be wrong twice over; only a v_cubeid face with an
    // index folded in says so for certain (a bare face may be a layer of a real 2D array).
    if (m.dim == DIM_2D_ARRAY && !m.a16 && (form.sample || form.gather || in.op == OP_IMAGE_GET_LOD)) {
        Tag layer = t.tags.vgpr[addr.regs[coord_at + 2] & 255];
        if (tag_kind(layer) == TAG_CUBE_FACE && tag_payload(layer) == 1) {
            translator_fail(t, "%s at 0x%x: cube array sampling through the 2D array dimension",
                            rdna_op_name(in.op), in.offset);
            return false;
        }
    }
    // Texel offsets: 6-bit signed fields at bits 0, 8 and 16 of a register whose value is known. Kept
    // out of the scratch ring (", offsets" or empty), since many expressions are formatted after it.
    char offset_arg[48] = {};
    s32 o[3] = {0, 0, 0};
    if (form.offset) {
        u32 value;
        if (!vgpr_constant(t.tags, addr.regs[offset_at], &value)) {
            translator_fail(t, "%s at 0x%x with non-constant texel offsets", rdna_op_name(in.op), in.offset);
            return false;
        }
        for (u32 i = 0; i < 3; i++)
            o[i] = (s32)(((value >> (8 * i)) & 0x3Fu) << 26) >> 26;
        // Desktop sampling takes offsets in [-8, 7] only; gathers take the whole console range.
        for (u32 i = 0; i < grads && !form.gather; i++) {
            if (o[i] < -8 || o[i] > 7) {
                translator_fail(t, "%s at 0x%x with texel offset %d outside [-8, 7]", rdna_op_name(in.op),
                                in.offset, o[i]);
                return false;
            }
        }
        if (grads == 1)
            snprintf(offset_arg, sizeof(offset_arg), ", %d", o[0]);
        else if (grads == 2)
            snprintf(offset_arg, sizeof(offset_arg), ", int2(%d, %d)", o[0], o[1]);
        else
            snprintf(offset_arg, sizeof(offset_arg), ", int3(%d, %d, %d)", o[0], o[1], o[2]);
    }

    // Sampling reads through shader resource views only; stores need unordered access views.
    bool sampled = form.sample || form.gather || in.op == OP_IMAGE_GET_LOD;
    ViewAccess access;
    if (!view_access(t, in, rsrc, form.store || is_image_atomic(in.op), sampled, &access))
        return false;
    if (access == VIEW_BY_SHADOW && !k_rw_texture_types[m.dim][0])
        access = VIEW_READ_ONLY; // no read-write views of multisampled images
    if (m.dim == DIM_CUBE && form.load && access != VIEW_READ_WRITE) {
        // Cube views (see k_texture_types) take no texel loads on desktop, so neither can a slot whose
        // view the shadow decides.
        translator_fail(t, "%s at 0x%x: image load from a cube view%s", rdna_op_name(in.op), in.offset,
                        access == VIEW_BY_SHADOW ? " (or a view of a kind the program doesn't say)" : "");
        return false;
    }
    // Loads and size queries work through either kind of view: with the shadow's kind deciding, both are
    // emitted, and the results are written after.
    bool by_shadow = access == VIEW_BY_SHADOW;
    bool resinfo = in.op == OP_IMAGE_GET_RESINFO;
    char resource[64];
    snprintf(resource, sizeof(resource), "%s >> 5", sgpr(t, rsrc));
    if (is_image_atomic(in.op)) // always through a read-write view (see view_access)
        return emit_image_atomic(t, in, addr, coord_at, coords, resource);
    emit(t, "{");
    t.depth++;
    if (by_shadow)
        emit(t, resinfo ? "uint w_ = 0u, h_ = 0u, d_ = 1u, l_ = 1u;" : "float4 r_;");
    for (ViewPass v = view_begin(access, rsrc); view_next(t, v);) {
        bool uav = v.read_write;
        if (uav) {
            if (!k_rw_texture_types[m.dim][0]) {
                translator_fail(t, "storage image of dimension %u", m.dim);
                return false;
            }
            emit(t, "%s<float4> tex_ = ResourceDescriptorHeap[%s];", k_rw_texture_types[m.dim], resource);
        } else {
            emit(t, "%s<float4> tex_ = ResourceDescriptorHeap[%s];", k_texture_types[m.dim], resource);
        }
        const char* value = nullptr;
        if (form.gather && !form.lod_zero) {
            value = emit_gather_at_mip(t, in, form, addr, coord_at, lod_at, bias_at, clamp_at, o);
            if (!value)
                return false;
        } else if (form.sample || form.gather) {
            if (!emit_sampler(t, in, form.compare))
                return false;
            const char* uv;
            if (m.dim == DIM_CUBE) {
                emit_cube_coord(t, addr, coord_at);
                uv = "c_";
            } else {
                uv = address_vector(t, addr, true, coord_at, coords);
            }
            // The clamp forms take an offset argument before the clamp, except on cubes (which have none).
            const char* clamp_offset = m.dim == DIM_CUBE ? "" : form.offset ? offset_arg : ", 0";
            bool implicit_lod_ok = implicit_lod_usable(t);
            if (form.gather) {
                static const char* const k_gather[4] = {"GatherRed", "GatherGreen", "GatherBlue",
                                                        "GatherAlpha"};
                u32 c = m.dmask & 2 ? 1 : m.dmask & 4 ? 2 : m.dmask & 8 ? 3 : 0;
                if (form.compare)
                    value = fmt(t, "tex_.GatherCmpRed(smp_, %s, %s%s)", uv,
                                address_value(t, addr, compare_at), offset_arg);
                else
                    value = fmt(t, "tex_.%s(smp_, %s%s)", k_gather[c], uv, offset_arg);
                if (form.lod || form.bias || form.grad || form.clamp) {
                    translator_fail(t, "%s variant at 0x%x", rdna_op_name(in.op), in.offset);
                    return false;
                }
            } else if (form.compare) {
                const char* ref = address_value(t, addr, compare_at);
                // Comparisons with an explicit LOD, bias or gradients need shader model 6.7/6.8 on desktop:
                // those variants fail below. Without usable derivatives the plain one samples level 0.
                bool plain = !form.lod && !form.bias && !form.grad && !form.clamp;
                if (form.lod_zero || (plain && !implicit_lod_ok))
                    value = fmt(t, "tex_.SampleCmpLevelZero(smp_, %s, %s%s).xxxx", uv, ref, offset_arg);
                else if (plain)
                    value = fmt(t, "tex_.SampleCmp(smp_, %s, %s%s).xxxx", uv, ref, offset_arg);
            } else if (form.lod_zero || (!form.lod && !form.grad && !implicit_lod_ok)) {
                // Without usable derivatives the LOD is the bias alone (see implicit_lod_ok), at least the
                // clamp.
                const char* level = form.bias && !form.lod_zero ? address_value(t, addr, bias_at) : "0.0f";
                if (form.clamp && !form.lod_zero)
                    level = fmt(t, "max(%s, %s)", level, address_value(t, addr, clamp_at));
                value = fmt(t, "tex_.SampleLevel(smp_, %s, %s%s)", uv, level, offset_arg);
            } else if (form.lod) {
                value = fmt(t, "tex_.SampleLevel(smp_, %s, %s%s)", uv, address_value(t, addr, lod_at),
                            offset_arg);
            } else if (form.bias) {
                const char* bias = address_value(t, addr, bias_at);
                value = form.clamp ? fmt(t, "tex_.SampleBias(smp_, %s, %s%s, %s)", uv, bias, clamp_offset,
                                         address_value(t, addr, clamp_at))
                                   : fmt(t, "tex_.SampleBias(smp_, %s, %s%s)", uv, bias, offset_arg);
            } else if (form.grad) {
                const char* ddx;
                const char* ddy;
                if (m.dim == DIM_CUBE) {
                    // Face-space gradients (ds, dt per screen axis) lie in the face's plane of the direction.
                    const char* face = address_value(t, addr, (s32)coord_at + 2);
                    emit(t, "float3 dx_ = cube_grad(%s, %s, %s);", face, address_value(t, addr, grad_at),
                         address_value(t, addr, grad_at + 1));
                    emit(t, "float3 dy_ = cube_grad(%s, %s, %s);", face, address_value(t, addr, grad_at + 2),
                         address_value(t, addr, grad_at + 3));
                    ddx = "dx_";
                    ddy = "dy_";
                } else {
                    ddx = address_vector(t, addr, true, (u32)grad_at, grads);
                    ddy = address_vector(t, addr, true, (u32)grad_at + grads, grads);
                }
                value = form.clamp ? fmt(t, "tex_.SampleGrad(smp_, %s, %s, %s%s, %s)", uv, ddx, ddy,
                                         clamp_offset, address_value(t, addr, clamp_at))
                                   : fmt(t, "tex_.SampleGrad(smp_, %s, %s, %s%s)", uv, ddx, ddy, offset_arg);
            } else {
                value = form.clamp ? fmt(t, "tex_.Sample(smp_, %s%s, %s)", uv, clamp_offset,
                                         address_value(t, addr, clamp_at))
                                   : fmt(t, "tex_.Sample(smp_, %s%s)", uv, offset_arg);
            }
            if (!value) {
                translator_fail(t, "%s variant at 0x%x", rdna_op_name(in.op), in.offset);
                return false;
            }
        } else if (form.load) {
            const char* xy = address_vector(t, addr, false, coord_at, coords);
            if (in.op == OP_IMAGE_MSAA_LOAD)
                value = fmt(t, "tex_.Load(%s, %s)", xy, address_bits(t, addr, lod_at));
            else if (uav)
                value = fmt(t, "tex_[%s]", xy);
            else
                value = fmt(t, "tex_.Load(uint%u(%s, %s))", coords + 1, xy,
                            form.mip ? address_bits(t, addr, lod_at) : "0u");
        } else if (form.store) {
            const char* xy = address_vector(t, addr, false, coord_at, coords);
            const char* c[4] = {"0.0f", "0.0f", "0.0f", "0.0f"};
            u32 source = 0;
            for (u32 i = 0; i < 4; i++) {
                if (!(m.dmask & (1u << i)))
                    continue;
                // 16-bit data: two components to a register, low half first.
                const char* reg = vgpr(t, in.dst[0].reg + (m.d16 ? source / 2 : source));
                if (!m.d16)
                    c[i] = fmt(t, "asfloat(%s)", reg);
                else
                    c[i] = fmt(t, source & 1 ? "f16tof32(%s >> 16)" : "f16tof32(%s & 0xffffu)", reg);
                source++;
            }
            emit(t, "if (%s) tex_[%s] = float4(%s, %s, %s, %s);", exec_cond(t), xy, c[0], c[1], c[2], c[3]);
            continue;
        } else if (resinfo) {
            if (!by_shadow)
                emit(t, "uint w_ = 0u, h_ = 0u, d_ = 1u, l_ = 1u;");
            if (!emit_image_size(t, in, uav))
                return false;
            continue;
        } else if (in.op == OP_IMAGE_GET_LOD) {
            if (!emit_sampler(t, in, false))
                return false;
            const char* uv;
            if (m.dim == DIM_CUBE) {
                emit_cube_coord(t, addr, coord_at);
                uv = "c_.xyz";
            } else {
                uv = address_vector(t, addr, true, coord_at, coords);
            }
            value = fmt(t,
                        "float4(tex_.CalculateLevelOfDetail(smp_, %s), "
                        "tex_.CalculateLevelOfDetailUnclamped(smp_, %s), "
                        "0.0f, 0.0f)",
                        uv, uv);
        } else {
            translator_fail(t, "unsupported %s", rdna_op_name(in.op));
            return false;
        }
        emit(t, by_shadow ? "r_ = %s;" : "float4 r_ = %s;", value);
    }
    if (t.failed)
        return false;
    u32 dst = in.dst[0].reg;
    if (resinfo) {
        for (u32 i = 0; i < 4; i++)
            if (m.dmask & (1u << i))
                write_vgpr(t, in, dst++, fmt(t, "uint4(w_, h_, d_, l_).%c", "xyzw"[i]));
    } else if (!form.store) {
        u32 mask = form.gather ? 0xF : m.dmask;
        const char* results[4];
        u32 n = 0;
        for (u32 i = 0; i < 4; i++)
            if (mask & (1u << i))
                results[n++] = fmt(t, "r_.%c", "xyzw"[i]);
        if (!m.d16) {
            for (u32 i = 0; i < n; i++)
                write_vgpr(t, in, dst + i, fmt(t, "asuint(%s)", results[i]));
        } else {
            // 16-bit data: two components to a register, low half first.
            for (u32 i = 0; i < n; i += 2)
                write_vgpr(t, in, dst + i / 2,
                           i + 1 < n
                               ? fmt(t, "(f32tof16(%s) | (f32tof16(%s) << 16))", results[i], results[i + 1])
                               : fmt(t, "f32tof16(%s)", results[i]));
        }
    }
    t.depth--;
    emit(t, "}");
    return true;
}

static u8 access_flags(const RdnaInst& in) {
    if (in.enc == ENC_MUBUF || in.enc == ENC_MTBUF)
        return is_store(in.op) || is_buffer_atomic(in.op) ? SLOT_WRITTEN : 0;
    if (in.enc != ENC_MIMG)
        return 0;
    ImageForm form = image_form(in.op);
    if (form.store || is_image_atomic(in.op))
        return SLOT_IMAGE | SLOT_WRITTEN;
    return SLOT_IMAGE | (form.sample || form.gather || in.op == OP_IMAGE_GET_LOD ? SLOT_SAMPLED : 0);
}

// --- LDS ---------------------------------------------------------------------------------------------

// ds_bpermute_b32 with exact lanes: each lane reads data0 of the lane its address names (in bytes);
// lanes outside EXEC read as zero. The reads happen in every lane, before any lane writes (the
// destination may be the data). With two lanes per thread both halves are done here: a lane may name
// either half, so each reads both halves' value of the named desktop lane.
static bool emit_bpermute(Translator& t, const RdnaInst& in, u32 offset) {
    u32 addr = in.src[0].reg, data = in.src[1].reg, dst = in.dst[0].reg;
    if (t.lanes == 1) {
        emit(t,
             "{ uint i_ = ((%s + %uu) >> 2) & 31u; uint r_ = WaveReadLaneAt(%s, i_); bool e_ = "
             "WaveReadLaneAt(EXEC ? 1u : 0u, i_) != 0u;",
             vgpr(t, addr), offset, vgpr(t, data));
        t.depth++;
        write_vgpr(t, in, dst, "(e_ ? r_ : 0u)");
        t.depth--;
        emit(t, "}");
        return true;
    }
    emit(t, "{");
    t.depth++;
    for (u32 h = 0; h < 2; h++) {
        emit(t, "uint i%u_ = ((%s + %uu) >> 2) & 63u;", h, vgpr_half(t, addr, h), offset);
        emit(t,
             "uint r%u_ = WaveReadLaneAt((i%u_ & 32u) != 0u ? %s : %s, i%u_ & 31u); bool e%u_ = "
             "WaveReadLaneAt((i%u_ & 32u) != 0u ? s127 : s126, i%u_ & 31u) != 0u;",
             h, h, vgpr_half(t, data, 1), vgpr_half(t, data, 0), h, h, h, h);
    }
    emit(t, "if (EXEC0) %s = e0_ ? r0_ : 0u; if (EXEC1) %s = e1_ ? r1_ : 0u;", vgpr_half(t, dst, 0),
         vgpr_half(t, dst, 1));
    t.depth--;
    emit(t, "}");
    return true;
}

static bool emit_lds(Translator& t, const RdnaInst& in) {
    if (in.mem.gds) {
        translator_fail(t, "GDS access at 0x%x", in.offset);
        return false;
    }
    t.uses_lds = true;
    const char* addr = vgpr(t, in.src[0].reg);
    u32 off0 = (u32)in.mem.offset, off1 = in.mem.offset1;
    u32 offset = off0 | (off1 << 8);
    const char* d0 = vgpr(t, in.src[1].reg);
    switch (in.op) {
    case OP_DS_WRITE_B32:
        emit(t, "if (%s) g_lds[(%s + %uu) >> 2] = %s;", exec_cond(t), addr, offset, d0);
        return true;
    case OP_DS_WRITE_B64:
        emit(t, "if (%s) { g_lds[(%s + %uu) >> 2] = %s; g_lds[(%s + %uu) >> 2] = %s; }", exec_cond(t), addr,
             offset, d0, addr, offset + 4, vgpr(t, in.src[1].reg + 1));
        return true;
    case OP_DS_WRITE_B96:
        emit(t,
             "if (%s) { uint a_ = (%s + %uu) >> 2; g_lds[a_] = %s; g_lds[a_ + 1u] = %s; g_lds[a_ + 2u] = "
             "%s; }",
             exec_cond(t), addr, offset, d0, vgpr(t, in.src[1].reg + 1), vgpr(t, in.src[1].reg + 2));
        return true;
    case OP_DS_WRITE_B128:
        emit(t,
             "if (%s) { uint a_ = (%s + %uu) >> 2; g_lds[a_] = %s; g_lds[a_ + 1u] = %s; g_lds[a_ + 2u] = "
             "%s; "
             "g_lds[a_ + 3u] = %s; }",
             exec_cond(t), addr, offset, d0, vgpr(t, in.src[1].reg + 1), vgpr(t, in.src[1].reg + 2),
             vgpr(t, in.src[1].reg + 3));
        return true;
    case OP_DS_WRITE2_B32:
    case OP_DS_WRITE2ST64_B32: {
        u32 unit = in.op == OP_DS_WRITE2ST64_B32 ? 256 : 4;
        emit(t, "if (%s) { g_lds[(%s + %uu) >> 2] = %s; g_lds[(%s + %uu) >> 2] = %s; }", exec_cond(t), addr,
             off0 * unit, d0, addr, off1 * unit, vgpr(t, in.src[2].reg));
        return true;
    }
    case OP_DS_WRITE2_B64:
    case OP_DS_WRITE2ST64_B64: {
        u32 unit = in.op == OP_DS_WRITE2ST64_B64 ? 512 : 8;
        emit(t,
             "if (%s) { uint a0_ = (%s + %uu) >> 2, a1_ = (%s + %uu) >> 2; g_lds[a0_] = %s; g_lds[a0_ + "
             "1u] = %s; g_lds[a1_] = %s; g_lds[a1_ + 1u] = %s; }",
             exec_cond(t), addr, off0 * unit, addr, off1 * unit, d0, vgpr(t, in.src[1].reg + 1),
             vgpr(t, in.src[2].reg), vgpr(t, in.src[2].reg + 1));
        return true;
    }
    case OP_DS_WRITE_B8:
    case OP_DS_WRITE_B16:
    case OP_DS_WRITE_B8_D16_HI:
    case OP_DS_WRITE_B16_D16_HI: {
        bool is_16 = in.op == OP_DS_WRITE_B16 || in.op == OP_DS_WRITE_B16_D16_HI;
        bool hi = in.op == OP_DS_WRITE_B8_D16_HI || in.op == OP_DS_WRITE_B16_D16_HI;
        u32 mask = is_16 ? 0xffffu : 0xffu;
        // One compare-exchange per lane makes the store a single update: lanes writing the same bytes
        // leave one lane's value, and no other wave sees the bytes cleared halfway. Every round lets one
        // of the conflicting lanes through.
        emit(t,
             "if (%s) { uint ba_ = %s + %uu; uint i_ = ba_ >> 2; uint sh_ = (ba_ & %uu) * 8u; uint m_ = "
             "0x%xu << "
             "sh_; uint v_ = ((%s%s) & 0x%xu) << sh_; uint o_ = g_lds[i_]; [allow_uav_condition] for (;;) { "
             "uint p_; "
             "InterlockedCompareExchange(g_lds[i_], o_, (o_ & ~m_) | v_, p_); if (p_ == o_) break; o_ = p_; "
             "} }",
             exec_cond(t), addr, offset, is_16 ? 2u : 3u, mask, d0, hi ? " >> 16" : "", mask);
        return true;
    }
    case OP_DS_READ_B32:
        write_vgpr(t, in, in.dst[0].reg, fmt(t, "g_lds[(%s + %uu) >> 2]", addr, offset));
        return true;
    case OP_DS_READ_B64:
    case OP_DS_READ_B96:
    case OP_DS_READ_B128: {
        u32 n = in.op == OP_DS_READ_B64 ? 2 : in.op == OP_DS_READ_B96 ? 3 : 4;
        emit(t, "{ uint a_ = (%s + %uu) >> 2;", addr, offset);
        t.depth++;
        // Read every dword before writing any: the destination may overlap the address.
        for (u32 i = 0; i < n; i++)
            emit(t, "uint r%u_ = g_lds[a_ + %uu];", i, i);
        for (u32 i = 0; i < n; i++)
            write_vgpr(t, in, in.dst[0].reg + i, fmt(t, "r%u_", i));
        t.depth--;
        emit(t, "}");
        return true;
    }
    case OP_DS_READ2_B32:
    case OP_DS_READ2ST64_B32: {
        u32 unit = in.op == OP_DS_READ2ST64_B32 ? 256 : 4;
        emit(t, "{ uint r0_ = g_lds[(%s + %uu) >> 2]; uint r1_ = g_lds[(%s + %uu) >> 2];", addr, off0 * unit,
             addr, off1 * unit);
        t.depth++;
        write_vgpr(t, in, in.dst[0].reg, "r0_");
        write_vgpr(t, in, in.dst[0].reg + 1, "r1_");
        t.depth--;
        emit(t, "}");
        return true;
    }
    case OP_DS_READ2_B64:
    case OP_DS_READ2ST64_B64: {
        u32 unit = in.op == OP_DS_READ2ST64_B64 ? 512 : 8;
        emit(t,
             "{ uint a0_ = (%s + %uu) >> 2, a1_ = (%s + %uu) >> 2; uint r0_ = g_lds[a0_], r1_ = g_lds[a0_ "
             "+ 1u], r2_ = g_lds[a1_], r3_ = g_lds[a1_ + 1u];",
             addr, off0 * unit, addr, off1 * unit);
        t.depth++;
        write_vgpr(t, in, in.dst[0].reg, "r0_");
        write_vgpr(t, in, in.dst[0].reg + 1, "r1_");
        write_vgpr(t, in, in.dst[0].reg + 2, "r2_");
        write_vgpr(t, in, in.dst[0].reg + 3, "r3_");
        t.depth--;
        emit(t, "}");
        return true;
    }
    case OP_DS_READ_I8:
    case OP_DS_READ_U8:
    case OP_DS_READ_I16:
    case OP_DS_READ_U16:
    case OP_DS_READ_U8_D16:
    case OP_DS_READ_U8_D16_HI:
    case OP_DS_READ_I8_D16:
    case OP_DS_READ_I8_D16_HI:
    case OP_DS_READ_U16_D16:
    case OP_DS_READ_U16_D16_HI: {
        bool is_16 = in.op == OP_DS_READ_I16 || in.op == OP_DS_READ_U16 || in.op == OP_DS_READ_U16_D16 ||
                     in.op == OP_DS_READ_U16_D16_HI;
        bool is_signed = in.op == OP_DS_READ_I8 || in.op == OP_DS_READ_I16 || in.op == OP_DS_READ_I8_D16 ||
                         in.op == OP_DS_READ_I8_D16_HI;
        bool d16_lo = in.op == OP_DS_READ_U8_D16 || in.op == OP_DS_READ_I8_D16 || in.op == OP_DS_READ_U16_D16;
        bool d16_hi =
            in.op == OP_DS_READ_U8_D16_HI || in.op == OP_DS_READ_I8_D16_HI || in.op == OP_DS_READ_U16_D16_HI;
        u32 bits = is_16 ? 16u : 8u;
        emit(t, "{ uint ba_ = %s + %uu; uint w_ = g_lds[ba_ >> 2]; uint sh_ = (ba_ & %uu) * 8u;", addr,
             offset, is_16 ? 2u : 3u);
        t.depth++;
        const char* val = is_signed
                              ? fmt(t, "(uint)((int)(w_ << (32u - %uu - sh_)) >> %uu)", bits, 32u - bits)
                              : fmt(t, "((w_ >> sh_) & 0x%xu)", (1u << bits) - 1u);
        if (d16_lo) {
            const char* dst = vgpr(t, in.dst[0].reg);
            write_vgpr(t, in, in.dst[0].reg, fmt(t, "((%s & 0xffff0000u) | (%s & 0xffffu))", dst, val));
        } else if (d16_hi) {
            const char* dst = vgpr(t, in.dst[0].reg);
            write_vgpr(t, in, in.dst[0].reg, fmt(t, "((%s & 0xffffu) | ((%s & 0xffffu) << 16))", dst, val));
        } else {
            write_vgpr(t, in, in.dst[0].reg, val);
        }
        t.depth--;
        emit(t, "}");
        return true;
    }
    case OP_DS_APPEND:
    case OP_DS_CONSUME: {
        use_sgpr(t, SRC_M0);
        // The active lanes are counted before one of them does the add for the wave.
        const char* delta = in.op == OP_DS_APPEND ? "n_" : "(uint)(-(int)n_)";
        if (t.lanes == 2) {
            // Both halves: one add for the console wave's active lanes, by its first one; every lane gets
            // the counter's old value (programs add their own position, with v_mbcnt).
            emit(t, "{ uint b0_ = mask_bits(s126), b1_ = mask_bits(s127); uint n_ = countbits(b0_) + "
                    "countbits(b1_); "
                    "uint f_ = b0_ != 0u ? firstbitlow(b0_) : firstbitlow(b1_); uint o_ = 0u;");
            t.depth++;
            emit(t,
                 "if (n_ != 0u && WaveGetLaneIndex() == (f_ & 31u)) InterlockedAdd(g_lds[((s124 & 0xffffu) + "
                 "%uu) >> 2], %s, o_);",
                 offset, delta);
            emit(t, "o_ = WaveReadLaneAt(o_, f_ & 31u);");
            emit(t, "if (EXEC0) %s = o_; if (EXEC1) %s = o_;", vgpr_half(t, in.dst[0].reg, 0),
                 vgpr_half(t, in.dst[0].reg, 1));
            t.depth--;
            emit(t, "}");
            return true;
        }
        emit(t,
             "{ uint o_ = 0u; if (EXEC) { uint n_ = wave_count(s126); uint lane_ = wave_first(s126); "
             "if (WaveGetLaneIndex() == lane_) InterlockedAdd(g_lds[((s124 & 0xffffu) + %uu) >> 2], %s, o_); "
             "o_ = WaveReadLaneAt(o_, lane_); }",
             offset, delta);
        t.depth++;
        write_vgpr(t, in, in.dst[0].reg, "o_");
        t.depth--;
        emit(t, "}");
        return true;
    }
    case OP_DS_ADD_U32:
    case OP_DS_MIN_U32:
    case OP_DS_MAX_U32:
    case OP_DS_MIN_I32:
    case OP_DS_MAX_I32:
    case OP_DS_AND_B32:
    case OP_DS_OR_B32:
    case OP_DS_XOR_B32:
    case OP_DS_ADD_RTN_U32:
    case OP_DS_MIN_RTN_U32:
    case OP_DS_MAX_RTN_U32:
    case OP_DS_AND_RTN_B32:
    case OP_DS_OR_RTN_B32:
    case OP_DS_XOR_RTN_B32:
    case OP_DS_WRXCHG_RTN_B32: {
        const char* fn;
        bool rtn = in.op >= OP_DS_ADD_RTN_U32;
        switch (in.op) {
        case OP_DS_ADD_U32:
        case OP_DS_ADD_RTN_U32:
            fn = "InterlockedAdd";
            break;
        case OP_DS_MIN_U32:
        case OP_DS_MIN_RTN_U32:
        case OP_DS_MIN_I32:
            fn = "InterlockedMin";
            break;
        case OP_DS_MAX_U32:
        case OP_DS_MAX_RTN_U32:
        case OP_DS_MAX_I32:
            fn = "InterlockedMax";
            break;
        case OP_DS_AND_B32:
        case OP_DS_AND_RTN_B32:
            fn = "InterlockedAnd";
            break;
        case OP_DS_OR_B32:
        case OP_DS_OR_RTN_B32:
            fn = "InterlockedOr";
            break;
        case OP_DS_WRXCHG_RTN_B32:
            fn = "InterlockedExchange";
            break;
        default:
            fn = "InterlockedXor";
            break;
        }
        if (in.op == OP_DS_MIN_I32 || in.op == OP_DS_MAX_I32) {
            translator_fail(t, "signed LDS atomics");
            return false;
        }
        if (rtn) {
            emit(t, "{ uint o_ = 0u; if (%s) %s(g_lds[(%s + %uu) >> 2], %s, o_);", exec_cond(t), fn, addr,
                 offset, d0);
            t.depth++;
            write_vgpr(t, in, in.dst[0].reg, "o_");
            t.depth--;
            emit(t, "}");
        } else {
            emit(t, "if (%s) %s(g_lds[(%s + %uu) >> 2], %s);", exec_cond(t), fn, addr, offset, d0);
        }
        return true;
    }
    case OP_DS_BPERMUTE_B32:
        if (t.exact)
            return emit_bpermute(t, in, offset);
        // Read data0 from the lane addressed (in bytes) by this lane's address.
        write_vgpr(
            t, in, in.dst[0].reg,
            fmt(t, "WaveReadLaneAt(%s, ((%s + %uu) >> 2) & %su)", d0, addr, offset, t.wave32 ? "31" : "63"));
        return true;
    default:
        translator_fail(t, "unsupported %s", rdna_op_name(in.op));
        return false;
    }
}

// --- global memory -----------------------------------------------------------------------------------

// A global load from `buffer` at the byte address `address`. Results are written a register at a time, and
// the address may be in those registers: with more than one, it is taken first.
static void write_global_loaded(Translator& t, const RdnaInst& in, const char* buffer, const char* address) {
    u32 dwords = in.dst[0].count;
    if (dwords == 1) {
        write_loaded(t, in, 1, fmt(t, "%s.Load(%s)", buffer, address));
        return;
    }
    emit(t, "{ uint a_ = %s;", address);
    t.depth++;
    write_loaded(t, in, dwords, fmt(t, "%s.%s(a_)", buffer, k_load_fn[dwords]));
    t.depth--;
    emit(t, "}");
}

// Descriptor heap memory read as data (programs read fields of descriptors they pick per lane): from the
// descriptor shadow, which holds dwords 0-3 of each slot. The address is a heap address (a table's or
// the heap's base plus an index scaled by the slot size, which translated code holds as a desktop heap
// byte offset) with a high dword of zero: console heaps are below 4 GiB.
static bool emit_heap_global_load(Translator& t, const RdnaInst& in, const char* address) {
    u32 dwords = in.dst[0].count;
    u32 at = (u32)in.mem.offset % SHADER_SHADOW_RECORD_BYTES;
    if (!(in.op >= OP_GLOBAL_LOAD_DWORD && in.op <= OP_GLOBAL_LOAD_DWORDX3)) {
        translator_fail(t, "%s at 0x%x to descriptor heap memory", rdna_op_name(in.op), in.offset);
        return false;
    }
    // Programs address slots whole, so the offset tells the dwords' place in the slot.
    if (at + 4 * dwords > SHADER_SHADOW_KIND_OFFSET) {
        translator_fail(t, "%s at 0x%x reads image descriptor dwords 4-7, which the shadow doesn't hold",
                        rdna_op_name(in.op), in.offset);
        return false;
    }
    t.needs_descriptor_info = true;
    write_global_loaded(t, in, "g_descriptor_info", fmt(t, "%s + 0x%xu", address, (u32)in.mem.offset));
    return true;
}

static bool emit_global(Translator& t, const RdnaInst& in) {
    const RdnaOperand& saddr = in.src[2];
    u32 parameter = ~0u;
    const char* base = "0u";
    u32 hi_value = ~0u;
    if (saddr.kind == OPND_SGPR) {
        Tag lo = sgpr_tag(t, saddr.reg), hi = sgpr_tag(t, saddr.reg + 1);
        if (tag_kind(hi) == TAG_ROOT_HI) {
            parameter = tag_payload(hi);
            base = fmt(t, "(%s + %s)", sgpr(t, saddr.reg), vgpr(t, in.src[0].reg));
        } else if (tag_kind(lo) == TAG_HEAP && !heap_loaded(lo) && (tag_payload(lo) & 1) == HEAP_RESOURCES &&
                   sgpr_constant(t.tags, saddr.reg + 1, &hi_value) && hi_value == 0) {
            const char* offset = vgpr_data(t, in.src[0].reg);
            return emit_heap_global_load(t, in, fmt(t, "%s + %s", sgpr(t, saddr.reg), offset));
        }
    } else {
        Tag lo = t.tags.vgpr[in.src[0].reg & 255], hi = t.tags.vgpr[(in.src[0].reg + 1) & 255];
        if (tag_kind(hi) == TAG_ROOT_HI) {
            parameter = tag_payload(hi);
            base = vgpr(t, in.src[0].reg);
        } else if (tag_kind(lo) == TAG_HEAP && !heap_loaded(lo) && (tag_payload(lo) & 1) == HEAP_RESOURCES &&
                   vgpr_constant(t.tags, in.src[0].reg + 1, &hi_value) && hi_value == 0) {
            return emit_heap_global_load(t, in, vgpr(t, in.src[0].reg));
        }
    }
    if (parameter == ~0u) {
        char why[96];
        if (saddr.kind == OPND_SGPR)
            describe_tag_loss(t, false, saddr.reg, why, sizeof(why));
        else
            describe_tag_loss(t, true, in.src[0].reg, why, sizeof(why));
        translator_fail(t, "%s at 0x%x from an address the translator cannot place%s", rdna_op_name(in.op),
                        in.offset, why);
        return false;
    }
    t.root_used |= 1ull << parameter;
    const char* a = fmt(t, "(%s + 0x%xu)", base, (u32)in.mem.offset);
    if (in.op >= OP_GLOBAL_LOAD_DWORD && in.op <= OP_GLOBAL_LOAD_DWORDX3) {
        write_global_loaded(t, in, fmt(t, "g_root%u", parameter), a);
        return true;
    }
    translator_fail(t, "unsupported %s", rdna_op_name(in.op));
    return false;
}

// Stores and atomics write their data registers as translated code holds them: a loaded descriptor
// dword's heap address would reach memory in place of the dword. None seen; fail where one would.
static bool check_stored_registers(Translator& t, const RdnaInst& in, const RdnaOperand& o) {
    if (o.kind != OPND_VGPR)
        return true;
    for (u32 i = 0; i < (o.count ? o.count : 1u); i++) {
        if (heap_loaded(t.tags.vgpr[(o.reg + i) & 255])) {
            translator_fail(t, "%s at 0x%x stores a descriptor dword", rdna_op_name(in.op), in.offset);
            return false;
        }
    }
    return true;
}

static bool stores_data(const RdnaInst& in) {
    if (in.enc == ENC_MUBUF || in.enc == ENC_MTBUF)
        return is_store(in.op) || is_buffer_atomic(in.op);
    if (in.enc == ENC_MIMG)
        return image_form(in.op).store || is_image_atomic(in.op);
    return false;
}

bool emit_memory(Translator& t, const RdnaInst& in) {
    if (stores_data(in) && !check_stored_registers(t, in, in.dst[0]))
        return false;
    if (in.enc == ENC_DS &&
        !(check_stored_registers(t, in, in.src[1]) && check_stored_registers(t, in, in.src[2])))
        return false;
    switch (in.enc) {
    case ENC_SMEM:
        return emit_scalar_memory(t, in);
    case ENC_MUBUF:
    case ENC_MTBUF:
        return emit_buffer(t, in);
    case ENC_MIMG:
        return emit_image(t, in);
    case ENC_DS:
        return emit_lds(t, in);
    case ENC_GLOBAL:
        return emit_global(t, in);
    default:
        translator_fail(t, "unsupported %s", rdna_op_name(in.op));
        return false;
    }
}
