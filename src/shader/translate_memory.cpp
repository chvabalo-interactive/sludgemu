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

// The kind of descriptor at a known place in a table, from the table's ranges.
static bool range_kind(const Translator& t, u32 parameter, u32 descriptor, TableRangeKind* kind) {
    if (parameter >= t.info->parameter_count)
        return false;
    const UserDataParameter& p = t.info->parameters[parameter];
    if (p.kind != USER_DATA_RESOURCE_TABLE)
        return false;
    u32 kinds = 0;
    for (u32 i = 0; i < p.range_count; i++) {
        const TableRange& r = p.ranges[i];
        kinds |= 1u << r.kind;
        if (descriptor != NO_DESCRIPTOR && descriptor >= r.offset &&
            (r.count == ~0u || descriptor < r.offset + r.count)) {
            *kind = r.kind;
            return true;
        }
    }
    // Unknown place: decided if the whole table holds one kind.
    for (u32 k = 0; k < 3; k++)
        if (kinds == (1u << k)) {
            *kind = (TableRangeKind)k;
            return true;
        }
    return false;
}

// The descriptor kind for a heap register, or `fallback` when the root signature does not tell.
static TableRangeKind heap_kind(const Translator& t, u32 reg, TableRangeKind fallback) {
    Tag tag = sgpr_tag(t, reg);
    TableRangeKind kind = fallback;
    range_kind(t, heap_parameter(tag), t.tags.descriptor[reg & 127], &kind);
    return kind;
}

// Where the descriptor in registers [reg, reg + dwords) comes from. Translated code keeps only the heap
// address of a descriptor loaded from the heap (in its first register), so a descriptor works as one
// only if every dword was loaded from the same slot and left alone, except the dwords in `may_change`
// (by bit), whose fields the desktop view makes no use of for the access at hand.
enum DescriptorOrigin {
    ORIGIN_HEAP,            // loaded from one slot of the heap `heap`, unchanged
    ORIGIN_CONSTANT,        // every dword known at translation time
    ORIGIN_PARTLY_CONSTANT, // some dwords known, others not
    ORIGIN_MODIFIED,        // loaded from the heap, then changed
    ORIGIN_OTHER_HEAP,      // loaded from the other heap
    ORIGIN_ASSEMBLED,       // dwords from different slots, or loaded and unknown ones mixed
    ORIGIN_UNKNOWN,         // nothing the translator can place
};

static DescriptorOrigin descriptor_origin(const Translator& t, u32 reg, u32 dwords, u32 heap,
                                          u32 may_change = 0) {
    u32 constants = 0, value;
    for (u32 i = 0; i < dwords; i++)
        constants += sgpr_constant(t.tags, reg + i, &value) ? 1 : 0;
    if (constants == dwords)
        return ORIGIN_CONSTANT;
    Tag first = sgpr_tag(t, reg);
    if (!heap_loaded(first))
        return constants ? ORIGIN_PARTLY_CONSTANT : ORIGIN_UNKNOWN;
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
        // The same table and kind of tag; the same slot where known.
        if ((tag & ~HEAP_MODIFIED) != (first & ~HEAP_MODIFIED) ||
            (slot != NO_DESCRIPTOR && d != NO_DESCRIPTOR && d != slot))
            return ORIGIN_ASSEMBLED;
        if (tag_payload(tag) & HEAP_MODIFIED)
            origin = ORIGIN_MODIFIED;
    }
    return origin;
}

// Buffer descriptor dwords a raw desktop view of the buffer can ignore changes to. Compilers rewrite
// structured buffer descriptors into raw ones (record count times stride, stride cleared, raw bounds
// checking), which is what desktop views are anyway. Not the base address (dword 0); the stride (dword
// 1, with the base's high bits) only without an index, which the translation scales by the original
// stride; the record count (dword 2) bounds the console's accesses only, desktop bounds are the view's
// size; the format and swizzle (dword 3) only for untyped accesses.
static u32 buffer_fields_unused(bool indexed, bool typed) {
    return (indexed ? 0u : 1u << 1) | 1u << 2 | (typed ? 0u : 1u << 3);
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
    case ORIGIN_OTHER_HEAP:
        return "from the other heap";
    case ORIGIN_ASSEMBLED:
        return "put together from different places";
    default:
        return "of unknown origin";
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
        if (parameter < SHADER_MAX_ROOT_PARAMETERS) {
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
        TableRangeKind kind = heap_kind(t, base, RANGE_CBV);
        emit(t, "{ uint a_ = %s;", fmt(t, "(0x%xu + %s)", (u32)in.mem.offset, soffset(t, in, 1)));
        t.depth++;
        // Constant buffers too are raw buffer views on desktop.
        emit(t, "%s b_ = ResourceDescriptorHeap[%s >> 5];",
             kind == RANGE_UAV ? "RWByteAddressBuffer" : "ByteAddressBuffer", sgpr(t, base));
        for (u32 i = 0; i < dwords; i++)
            write_sgpr(t, in.dst[0], i, fmt(t, "b_.Load(a_ + %uu)", 4 * i));
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
    translator_fail(t, "%s at 0x%x from an address the translator cannot place", rdna_op_name(in.op),
                    in.offset);
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
            emit(t, "uint o_ = 0u; if (EXEC) %s.InterlockedCompareExchange(%s, %s, %s, o_);", buf, addr, d1,
                 d0);
        else
            emit(t, "if (EXEC) %s.InterlockedCompareStore(%s, %s, %s);", buf, addr, d1, d0);
    } else if (in.op == OP_BUFFER_ATOMIC_INC || in.op == OP_BUFFER_ATOMIC_DEC) {
        const char* next = in.op == OP_BUFFER_ATOMIC_INC
                               ? fmt(t, "(cur_ >= %s ? 0u : cur_ + 1u)", d0)
                               : fmt(t, "((cur_ == 0u || cur_ > %s) ? %s : cur_ - 1u)", d0, d0);
        emit(t,
             "uint o_ = 0u; if (EXEC) { uint cur_ = %s.Load(%s), prev_ = 0u; [allow_uav_condition] do { "
             "uint nxt_ = %s; %s.InterlockedCompareExchange(%s, cur_, nxt_, prev_); if (prev_ == cur_) { o_ "
             "= cur_; break; } cur_ = prev_; } while (true); }",
             buf, addr, next, buf, addr);
    } else if (in.op == OP_BUFFER_ATOMIC_SWAP) {
        emit(t, "uint o_ = 0u; if (EXEC) %s.InterlockedExchange(%s, %s, o_);", buf, addr, d0);
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
            emit(t, "%s o_ = %s; if (EXEC) %s.%s(%s, %s, o_);", is_signed ? "int" : "uint",
                 is_signed ? "0" : "0u", buf, fn, addr, val);
        else
            emit(t, "if (EXEC) %s.%s(%s, %s);", buf, fn, addr, val);
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
        emit(t, "if (EXEC) %s.%s(%s, w_);", buf, k_store_fn[dwords], addr);
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
    const char* index = in.mem.idxen ? vgpr(t, vaddr) : "0u";
    const char* voffset = in.mem.offen ? vgpr(t, vaddr + (in.mem.idxen ? 1 : 0)) : "0u";
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
                    n += (usize)snprintf(list + n, sizeof(list) - n, "%sv%u", i ? ", " : "",
                                         in.dst[0].reg + i);
                for (u32 i = 0; i < dwords; i++)
                    use_vgpr(t, in.dst[0].reg + i);
                data = fmt(t, "uint%u(%s)", dwords, list);
            }
            emit(t, "if (EXEC) g_root%u.%s(a_, %s);", parameter, k_store_fn[dwords], data);
        } else {
            write_loaded(t, in, dwords, fmt(t, "g_root%u.%s(a_)", parameter, k_load_fn[dwords]));
        }
        t.depth--;
        emit(t, "}");
        return true;
    }
    if (tag_kind(lo) == TAG_HEAP && (tag_payload(lo) & 1) == HEAP_RESOURCES) {
        DescriptorOrigin origin = descriptor_origin(
            t, rsrc, 4, HEAP_RESOURCES, buffer_fields_unused(in.mem.idxen, format && in.enc != ENC_MTBUF));
        if (origin != ORIGIN_HEAP) {
            translator_fail(t, "%s at 0x%x through a buffer descriptor %s", rdna_op_name(in.op), in.offset,
                            origin_text(origin));
            return false;
        }
        TableRangeKind kind = heap_kind(t, rsrc, (store || atomic) ? RANGE_UAV : RANGE_SRV);
        const char* heap_index = fmt(t, "(%s >> 5)", sgpr(t, rsrc));
        emit(t, "{");
        t.depth++;
        if (format && in.enc != ENC_MTBUF) {
            const RdnaOperand& so = in.src[2];
            bool no_soffset = (so.kind == OPND_SPECIAL && so.reg == SRC_NULL) ||
                              (so.kind == OPND_INLINE && so.reg == SRC_ZERO);
            if (in.mem.offen || in.mem.offset || !no_soffset) {
                translator_fail(t, "%s with an offset or format at 0x%x", rdna_op_name(in.op), in.offset);
                return false;
            }
            u32 comps =
                store
                    ? dwords
                    : (u32)(d16 ? in.op - OP_BUFFER_LOAD_FORMAT_D16_X : in.op - OP_BUFFER_LOAD_FORMAT_X) + 1;
            if (comps == 0 || comps > 4) {
                translator_fail(t, "%s at 0x%x: bad component count", rdna_op_name(in.op), in.offset);
                return false;
            }
            emit(t, "%s<float4> b_ = ResourceDescriptorHeap[%s];",
                 kind == RANGE_UAV || store ? "RWBuffer" : "Buffer", heap_index);
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
                emit(t, "if (EXEC) b_[%s] = float4(%s, %s, %s, %s);", index, c[0], c[1], c[2], c[3]);
            } else {
                emit(t, "float4 r_ = b_[%s];", index);
                static const char k_comp[4] = {'x', 'y', 'z', 'w'};
                if (d16) {
                    for (u32 i = 0; i < comps; i += 2) {
                        const char* packed = (i + 1 < comps)
                                                 ? fmt(t, "(f32tof16(r_.%c) | (f32tof16(r_.%c) << 16))",
                                                       k_comp[i], k_comp[i + 1])
                                                 : fmt(t, "f32tof16(r_.%c)", k_comp[i]);
                        write_vgpr(t, in, in.dst[0].reg + i / 2, packed);
                    }
                } else {
                    for (u32 i = 0; i < comps; i++)
                        write_vgpr(t, in, in.dst[0].reg + i, fmt(t, "asuint(r_.%c)", k_comp[i]));
                }
            }
        } else {
            if (kind == RANGE_CBV && (store || atomic)) {
                translator_fail(t, "store or atomic to a constant buffer at 0x%x", in.offset);
                return false;
            }
            const char* stride = "0u";
            if (in.mem.idxen) {
                t.needs_descriptor_info = true;
                stride = fmt(t, "g_descriptor_info.Load(%s * 16u)", heap_index);
            }
            // Stores and atomics go through UAVs whatever the root signature says (dynamically indexed
            // tables).
            emit(t, "%s b_ = ResourceDescriptorHeap[%s];",
                 kind == RANGE_UAV || store || atomic ? "RWByteAddressBuffer" : "ByteAddressBuffer",
                 heap_index);
            emit(t, "uint a_ = %s * %s + %s;", index, stride, offset);
            if (atomic) {
                if (!emit_buffer_atomic(t, in, "b_", "a_"))
                    return false;
            } else if (in.enc == ENC_MTBUF) {
                if (!emit_mtbuf_access(t, in, "b_", "a_", store))
                    return false;
            } else if (store) {
                char list[96] = {};
                usize n = 0;
                for (u32 i = 0; i < dwords; i++) {
                    n += (usize)snprintf(list + n, sizeof(list) - n, "%sv%u", i ? ", " : "",
                                         in.dst[0].reg + i);
                    use_vgpr(t, in.dst[0].reg + i);
                }
                emit(t, "if (EXEC) b_.%s(a_, %s%s%s);", k_store_fn[dwords],
                     dwords > 1 ? fmt(t, "uint%u(", dwords) : "", list, dwords > 1 ? ")" : "");
            } else {
                write_loaded(t, in, dwords, fmt(t, "b_.%s(a_)", k_load_fn[dwords]));
            }
        }
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
    translator_fail(t, "%s at 0x%x through a descriptor the translator cannot place", rdna_op_name(in.op),
                    in.offset);
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
static s32 inline_sampler(Translator& t, const RdnaInst& in, const u32 words[4], bool compare) {
    InlineSampler s;
    const char* why = nullptr;
    if (!decode_sampler(words, compare, &s, &why)) {
        translator_fail(t, "%s at 0x%x with a constant sampler using %s", rdna_op_name(in.op), in.offset,
                        why);
        return -1;
    }
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

static const char* const k_texture_types[8] = {"Texture1D",   "Texture2D",       "Texture3D",
                                               "TextureCube", "Texture1DArray",  "Texture2DArray",
                                               "Texture2DMS", "Texture2DMSArray"};
static const u8 k_dim_coords[8] = {1, 2, 3, 3, 2, 3, 2, 3};
static const u8 k_dim_grads[8] = {1, 2, 3, 3, 1, 2, 2, 2};

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
        return vgpr(t, a.regs[i]);
    u32 j = (u32)i - a.unpacked;
    const char* reg = vgpr(t, a.regs[a.unpacked + j / 2]);
    return j & 1 ? fmt(t, "(%s >> 16)", reg) : fmt(t, "(%s & 0xffffu)", reg);
}

// Component `i` as a float.
static const char* address_value(Translator& t, const ImageAddress& a, s32 i) {
    if (a.a16 && i == a.half_at)
        return fmt(t, "f16tof32(%s)", vgpr(t, a.regs[i]));
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
    if (m.dim == DIM_CUBE && (form.sample || form.gather)) {
        translator_fail(t, "cube sampling at 0x%x", in.offset);
        return false;
    }
    u32 rsrc = in.src[1].reg;
    Tag tag = sgpr_tag(t, rsrc);
    if (tag_kind(tag) != TAG_HEAP || (tag_payload(tag) & 1) != HEAP_RESOURCES) {
        translator_fail(t, "%s at 0x%x with a resource not from a descriptor table", rdna_op_name(in.op),
                        in.offset);
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
    // Texel offsets: 6-bit signed fields at bits 0, 8 and 16 of a register whose value is known. Kept
    // out of the scratch ring (", offsets" or empty), since many expressions are formatted after it.
    char offset_arg[48] = {};
    if (form.offset) {
        u32 value;
        if (!vgpr_constant(t.tags, addr.regs[offset_at], &value)) {
            translator_fail(t, "%s at 0x%x with non-constant texel offsets", rdna_op_name(in.op), in.offset);
            return false;
        }
        s32 o[3];
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

    TableRangeKind kind = heap_kind(t, rsrc, form.store ? RANGE_UAV : RANGE_SRV);
    // Sampling reads through shader resource views only; stores need unordered access views.
    bool sampled = form.sample || form.gather || in.op == OP_IMAGE_GET_LOD;
    bool uav = form.store || (kind == RANGE_UAV && !sampled);
    const char* resource = fmt(t, "%s >> 5", sgpr(t, rsrc));
    emit(t, "{");
    t.depth++;
    if (uav) {
        static const char* const k_rw[8] = {"RWTexture1D",
                                            "RWTexture2D",
                                            "RWTexture3D",
                                            "RWTexture2DArray",
                                            "RWTexture1DArray",
                                            "RWTexture2DArray",
                                            "",
                                            ""};
        if (!k_rw[m.dim][0]) {
            translator_fail(t, "storage image of dimension %u", m.dim);
            return false;
        }
        emit(t, "%s<float4> tex_ = ResourceDescriptorHeap[%s];", k_rw[m.dim], resource);
    } else {
        emit(t, "%s<float4> tex_ = ResourceDescriptorHeap[%s];", k_texture_types[m.dim], resource);
    }
    const char* value = nullptr;
    if (form.sample || form.gather) {
        if (!emit_sampler(t, in, form.compare))
            return false;
        const char* uv = address_vector(t, addr, true, coord_at, coords);
        // Implicit LOD needs derivatives. Compute groups whose shape desktop can't take them in (1D
        // groups not a multiple of 4 threads, other groups with odd X or Y) have lone lanes in their
        // quads on console too: zero derivatives, so the LOD is the bias alone.
        bool implicit_lod_ok = true;
        if (t.info->stage == SHADER_COMPUTE) {
            const u16* n = t.info->threads;
            bool one_d = n[1] == 1 && n[2] == 1;
            implicit_lod_ok = one_d ? n[0] % 4 == 0 : n[0] % 2 == 0 && n[1] % 2 == 0;
        }
        if (form.gather) {
            static const char* const k_gather[4] = {"GatherRed", "GatherGreen", "GatherBlue", "GatherAlpha"};
            u32 c = m.dmask & 2 ? 1 : m.dmask & 4 ? 2 : m.dmask & 8 ? 3 : 0;
            if (form.compare)
                value = fmt(t, "tex_.GatherCmpRed(smp_, %s, %s%s)", uv, address_value(t, addr, compare_at),
                            offset_arg);
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
            // Without usable derivatives the LOD is the bias alone (see implicit_lod_ok), at least the clamp.
            const char* level = form.bias && !form.lod_zero ? address_value(t, addr, bias_at) : "0.0f";
            if (form.clamp && !form.lod_zero)
                level = fmt(t, "max(%s, %s)", level, address_value(t, addr, clamp_at));
            value = fmt(t, "tex_.SampleLevel(smp_, %s, %s%s)", uv, level, offset_arg);
        } else if (form.lod) {
            value =
                fmt(t, "tex_.SampleLevel(smp_, %s, %s%s)", uv, address_value(t, addr, lod_at), offset_arg);
        } else if (form.bias && !form.clamp) {
            value =
                fmt(t, "tex_.SampleBias(smp_, %s, %s%s)", uv, address_value(t, addr, bias_at), offset_arg);
        } else if (form.grad && !form.clamp) {
            const char* ddx = address_vector(t, addr, true, (u32)grad_at, grads);
            const char* ddy = address_vector(t, addr, true, (u32)grad_at + grads, grads);
            value = fmt(t, "tex_.SampleGrad(smp_, %s, %s, %s%s)", uv, ddx, ddy, offset_arg);
        } else if (!form.bias && !form.grad) {
            // The clamp form takes an offset argument either way.
            value = form.clamp ? fmt(t, "tex_.Sample(smp_, %s, %s, %s)", uv,
                                     form.offset ? offset_arg + 2 : "0", address_value(t, addr, clamp_at))
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
        emit(t, "if (EXEC) tex_[%s] = float4(%s, %s, %s, %s);", xy, c[0], c[1], c[2], c[3]);
        t.depth--;
        emit(t, "}");
        return true;
    } else if (in.op == OP_IMAGE_GET_RESINFO) {
        emit(t, "uint w_ = 0u, h_ = 0u, d_ = 1u, l_ = 1u;");
        const char* mip = vgpr(t, in.src[0].reg);
        switch (m.dim) {
        case DIM_1D:
            emit(t, "tex_.GetDimensions(%s, w_, l_);", mip);
            break;
        case DIM_2D:
        case DIM_CUBE:
            emit(t, "tex_.GetDimensions(%s, w_, h_, l_);", mip);
            break;
        case DIM_3D:
        case DIM_2D_ARRAY:
            emit(t, "tex_.GetDimensions(%s, w_, h_, d_, l_);", mip);
            break;
        case DIM_1D_ARRAY:
            emit(t, "tex_.GetDimensions(%s, w_, h_, l_);", mip);
            break;
        default:
            translator_fail(t, "resource info of dimension %u", m.dim);
            return false;
        }
        value = "uint4(w_, h_, d_, l_)";
        u32 dst = in.dst[0].reg;
        for (u32 i = 0; i < 4; i++)
            if (m.dmask & (1u << i))
                write_vgpr(t, in, dst++, fmt(t, "%s.%c", value, "xyzw"[i]));
        t.depth--;
        emit(t, "}");
        return true;
    } else if (in.op == OP_IMAGE_GET_LOD) {
        if (!emit_sampler(t, in, false))
            return false;
        const char* uv = address_vector(t, addr, true, coord_at, coords);
        value = fmt(
            t,
            "float4(tex_.CalculateLevelOfDetail(smp_, %s), tex_.CalculateLevelOfDetailUnclamped(smp_, %s), "
            "0.0f, 0.0f)",
            uv, uv);
    } else {
        translator_fail(t, "unsupported %s", rdna_op_name(in.op));
        return false;
    }
    emit(t, "float4 r_ = %s;", value);
    u32 dst = in.dst[0].reg;
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
                       i + 1 < n ? fmt(t, "(f32tof16(%s) | (f32tof16(%s) << 16))", results[i], results[i + 1])
                                 : fmt(t, "f32tof16(%s)", results[i]));
    }
    t.depth--;
    emit(t, "}");
    return true;
}

// --- LDS ---------------------------------------------------------------------------------------------

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
        emit(t, "if (EXEC) g_lds[(%s + %uu) >> 2] = %s;", addr, offset, d0);
        return true;
    case OP_DS_WRITE_B64:
        emit(t, "if (EXEC) { g_lds[(%s + %uu) >> 2] = %s; g_lds[(%s + %uu) >> 2] = %s; }", addr, offset, d0,
             addr, offset + 4, vgpr(t, in.src[1].reg + 1));
        return true;
    case OP_DS_WRITE_B96:
        emit(t,
             "if (EXEC) { uint a_ = (%s + %uu) >> 2; g_lds[a_] = %s; g_lds[a_ + 1u] = %s; g_lds[a_ + 2u] = "
             "%s; }",
             addr, offset, d0, vgpr(t, in.src[1].reg + 1), vgpr(t, in.src[1].reg + 2));
        return true;
    case OP_DS_WRITE_B128:
        emit(t,
             "if (EXEC) { uint a_ = (%s + %uu) >> 2; g_lds[a_] = %s; g_lds[a_ + 1u] = %s; g_lds[a_ + 2u] = "
             "%s; "
             "g_lds[a_ + 3u] = %s; }",
             addr, offset, d0, vgpr(t, in.src[1].reg + 1), vgpr(t, in.src[1].reg + 2),
             vgpr(t, in.src[1].reg + 3));
        return true;
    case OP_DS_WRITE2_B32:
    case OP_DS_WRITE2ST64_B32: {
        u32 unit = in.op == OP_DS_WRITE2ST64_B32 ? 256 : 4;
        emit(t, "if (EXEC) { g_lds[(%s + %uu) >> 2] = %s; g_lds[(%s + %uu) >> 2] = %s; }", addr, off0 * unit,
             d0, addr, off1 * unit, vgpr(t, in.src[2].reg));
        return true;
    }
    case OP_DS_WRITE2_B64:
    case OP_DS_WRITE2ST64_B64: {
        u32 unit = in.op == OP_DS_WRITE2ST64_B64 ? 512 : 8;
        emit(t,
             "if (EXEC) { uint a0_ = (%s + %uu) >> 2, a1_ = (%s + %uu) >> 2; g_lds[a0_] = %s; g_lds[a0_ + "
             "1u] = %s; g_lds[a1_] = %s; g_lds[a1_ + 1u] = %s; }",
             addr, off0 * unit, addr, off1 * unit, d0, vgpr(t, in.src[1].reg + 1), vgpr(t, in.src[2].reg),
             vgpr(t, in.src[2].reg + 1));
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
             "if (EXEC) { uint ba_ = %s + %uu; uint i_ = ba_ >> 2; uint sh_ = (ba_ & %uu) * 8u; uint m_ = "
             "0x%xu << "
             "sh_; uint v_ = ((%s%s) & 0x%xu) << sh_; uint o_ = g_lds[i_]; [allow_uav_condition] for (;;) { "
             "uint p_; "
             "InterlockedCompareExchange(g_lds[i_], o_, (o_ & ~m_) | v_, p_); if (p_ == o_) break; o_ = p_; "
             "} }",
             addr, offset, is_16 ? 2u : 3u, mask, d0, hi ? " >> 16" : "", mask);
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
            emit(t, "{ uint o_ = 0u; if (EXEC) %s(g_lds[(%s + %uu) >> 2], %s, o_);", fn, addr, offset, d0);
            t.depth++;
            write_vgpr(t, in, in.dst[0].reg, "o_");
            t.depth--;
            emit(t, "}");
        } else {
            emit(t, "if (EXEC) %s(g_lds[(%s + %uu) >> 2], %s);", fn, addr, offset, d0);
        }
        return true;
    }
    case OP_DS_BPERMUTE_B32:
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

static bool emit_global(Translator& t, const RdnaInst& in) {
    const RdnaOperand& saddr = in.src[2];
    u32 parameter = ~0u;
    const char* base = "0u";
    if (saddr.kind == OPND_SGPR) {
        Tag hi = sgpr_tag(t, saddr.reg + 1);
        if (tag_kind(hi) == TAG_ROOT_HI) {
            parameter = tag_payload(hi);
            base = fmt(t, "(%s + %s)", sgpr(t, saddr.reg), vgpr(t, in.src[0].reg));
        }
    } else {
        Tag hi = t.tags.vgpr[(in.src[0].reg + 1) & 255];
        if (tag_kind(hi) == TAG_ROOT_HI) {
            parameter = tag_payload(hi);
            base = vgpr(t, in.src[0].reg);
        }
    }
    if (parameter == ~0u) {
        translator_fail(t, "%s at 0x%x from an address the translator cannot place", rdna_op_name(in.op),
                        in.offset);
        return false;
    }
    t.root_used |= 1ull << parameter;
    u32 dwords = in.dst[0].count;
    const char* a = fmt(t, "(%s + 0x%xu)", base, (u32)in.mem.offset);
    if (in.op >= OP_GLOBAL_LOAD_DWORD && in.op <= OP_GLOBAL_LOAD_DWORDX3) {
        write_loaded(t, in, dwords, fmt(t, "g_root%u.%s(%s)", parameter, k_load_fn[dwords], a));
        return true;
    }
    translator_fail(t, "unsupported %s", rdna_op_name(in.op));
    return false;
}

bool emit_memory(Translator& t, const RdnaInst& in) {
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
