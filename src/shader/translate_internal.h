// Shader translation internals shared by the translator's source files.

#pragma once

#include <stdarg.h>

#include "shader/rdna.h"
#include "shader/translate.h"

struct Block {
    u32 first, end; // instruction range
    s32 next[2];    // successors: branch target (or fallthrough), then fallthrough of a conditional
};

struct Program {
    RdnaInst* insts;
    u32 count;
    u32 capacity;
    Block* blocks;
    u32 block_count;
    u32* block_of; // instruction -> block
};

// What the translator knows statically about a register's value.
typedef u16 Tag;

enum TagKind : u16 {
    TAG_NONE = 0,
    TAG_MASK,      // a lane mask (broadcast form)
    TAG_HEAP,      // a byte address in a desktop descriptor heap; payload: see HEAP_* below
    TAG_ROOT_HI,   // the high dword of a root descriptor (maybe with stride bits); payload: parameter
    TAG_VB_TABLE,  // the vertex buffer table address
    TAG_VB_DESC,   // a vertex buffer descriptor dword; payload: slot
    TAG_PC,        // code-relative constant data offset (s_getpc_b64 + offset)
    TAG_CUBE_FACE, // a v_cubeid face (VGPR); payload 1 once a cube index is folded in (index * 8 + face)
};

// TAG_HEAP payload bits:
//   bit 0      which heap (HEAP_RESOURCES or HEAP_SAMPLERS)
//   bits 1-6   the root parameter of the table the address is in (HEAP_NO_TABLE: a directly indexed
//              heap, or a table not known)
//   bit 7      HEAP_LOADED: a descriptor dword loaded from the heap (translated code holds its byte
//              address), as opposed to a table address. Arithmetic on table addresses indexes the table;
//              reading a loaded dword as a value reads the descriptor shadow at that address instead
//              (see register_data), and the result is a plain value.
//   bit 8      HEAP_MODIFIED: the register holds a loaded dword's address on some paths only (set only
//              with HEAP_LOADED, by joins): neither a descriptor nor a value translated code can use
//   bits 9-11  free
// The bits travel with the tag through moves, spills and swaps (see tags_merge for joins).
enum : u16 {
    HEAP_RESOURCES = 0,
    HEAP_SAMPLERS = 1,
    HEAP_NO_TABLE = 0x3F, // the parameter field's value for no table
    HEAP_LOADED = 0x80,
    HEAP_MODIFIED = 0x100,
};

inline Tag make_tag(TagKind kind, u32 payload) {
    return (Tag)((kind << 12) | (payload & 0xFFF));
}
inline TagKind tag_kind(Tag t) {
    return (TagKind)(t >> 12);
}
inline u32 tag_payload(Tag t) {
    return t & 0xFFF;
}
inline u32 heap_parameter(Tag t) {
    return (tag_payload(t) >> 1) & 0x3F;
}
inline bool heap_loaded(Tag t) {
    return tag_kind(t) == TAG_HEAP && (tag_payload(t) & HEAP_LOADED);
}

#define SHADER_MAX_SPILL_VGPRS 16

struct TagState {
    Tag sgpr[128];
    Tag vgpr[256];
    // Registers loaded from a descriptor table at a known place: the descriptor's index in the table
    // (by heap slot), or NO_DESCRIPTOR. Decides the HLSL resource type from the table's ranges.
    u16 descriptor[128];
    // SGPRs holding a value known at translation time (moved from constants).
    u32 constant[128];
    u64 constant_known[2];
    // Exact lanes: SGPRs nothing has written yet (0 in the translation; see merge_tag), and SGPRs
    // holding data that is all clear or all set (uniform booleans): the same in mask form.
    u64 unwritten[2];
    u64 neutral[2];
    // Constant 32-bit values carried in vector registers (for MIMG texel offsets). Vector writes skip
    // lanes outside EXEC, so a value holds in every lane only if written while all lanes ran
    // (vgpr_exec == EXEC_ANY), or while reading under the EXEC it was written with.
    u32 vgpr_val[256];
    u32 vgpr_exec[256];
    bool vgpr_const[256];
    bool exec_full;   // EXEC is still the starting one (no instruction has written it)
    u32 exec_version; // which EXEC: 0 at the start, then where it was last written (see tags_transfer)
    // Scalar tags and descriptors parked in lanes of spill VGPRs via v_writelane_b32.
    Tag spill_tag[SHADER_MAX_SPILL_VGPRS][64];
    u16 spill_desc[SHADER_MAX_SPILL_VGPRS][64];
};

#define EXEC_ANY 0xFFFFFFFFu

inline bool vgpr_constant(const TagState& s, u32 reg, u32* value) {
    reg &= 255;
    if (!s.vgpr_const[reg] || (s.vgpr_exec[reg] != EXEC_ANY && s.vgpr_exec[reg] != s.exec_version))
        return false;
    *value = s.vgpr_val[reg];
    return true;
}

inline bool sgpr_constant(const TagState& s, u32 reg, u32* value) {
    reg &= 127;
    if (!(s.constant_known[reg / 64] & (1ull << (reg % 64))))
        return false;
    *value = s.constant[reg];
    return true;
}

#define NO_DESCRIPTOR 0xFFFF

// A pixel shader attribute read: which components, and how it is interpolated.
enum Interpolation : u8 {
    INTERP_NONE,
    INTERP_PERSPECTIVE,
    INTERP_PERSPECTIVE_CENTROID,
    INTERP_PERSPECTIVE_SAMPLE,
    INTERP_LINEAR,
    INTERP_LINEAR_CENTROID,
    INTERP_LINEAR_SAMPLE,
    INTERP_FLAT,
};

#define SHADER_MAX_PC_POOLS 16

struct PcPool {
    u32 base_off;
    u32 dwords;
};

// What a program does with the descriptor at a known place in a table (see find_slot_uses).
enum : u8 {
    SLOT_WRITTEN = 1, // stores or atomics through it: it must be an unordered access view
    SLOT_SAMPLED = 2, // sampled through it: it must be a shader resource view
};

struct SlotUse {
    u8 parameter;
    u8 flags;
    u16 index;
};

#define SHADER_MAX_SLOT_USES 256

// Masks a vector instruction reads as data: their bits are ballots, computed before the instruction
// (outside its EXEC guards, where every lane takes part).
#define SHADER_MAX_HOISTED 8

struct Translator {
    const ShaderInfo* info;
    const ShaderTarget* target;
    TranslatedShader* out;
    const u8* code;
    u32 code_size;
    Program program;
    TagState* block_entry; // tags at each block's start
    bool* block_reached;   // per block: control can get there (the others aren't emitted)
    u8* xor_swap_role;     // per-instruction: 1 = start of 3-xor swap, 2 = swap continuation
    // SGPRs each block may read before writing them (directly or in blocks after it).
    u64 (*live_in)[2];
    TagState tags;   // tags before the instruction being emitted
    u32 inst_offset; // the instruction being emitted, for messages
    bool copying;    // it copies registers as they are (copies_tags): descriptor addresses too
    Text body;       // the program's statements; declarations are written around them
    u32 depth;       // indentation of emitted statements
    bool failed;

    u8 sgpr_used[128];
    u8 vgpr_used[256];
    bool scc_used;
    bool far_jumps;         // the control flow jumps through go_ (see translate_flow.cpp)
    u32 ps_input_vgpr[16];  // pixel shaders: first VGPR of each SPI_PS_INPUT_ADDR input, or ~0u
    u8 attribute_modes[32]; // pixel shaders: interpolations each attribute is read with (1 << mode)
    u32 attribute_halves;   // pixel shaders: attributes interpolated as 16-bit halves of their dwords
    u8 vs_fetch_components[SHADER_MAX_VERTEX_ELEMENTS]; // vertex shaders: dwords fetched per element
    bool fetch_typed[SHADER_MAX_VERTEX_ELEMENTS];
    u8 fetch_class[SHADER_MAX_VERTEX_ELEMENTS]; // typed fetches: 0 float, 1 uint, 2 sint
    u8 param_components[32];
    u8 position_exported;
    bool clip_written; // vertex shaders: clip (cull) distances exported, declared from vs_out_cntl
    bool cull_written;
    u8 target_compressed[8];
    // Pixel shaders without a color or depth export: main returns nothing (see emit_program).
    bool ps_without_outputs;
    bool needs_descriptor_info;
    bool uses_lds;
    bool wave32; // the console wave size
    // Console lanes map exactly onto desktop lanes (compute, see translate.h): masks are tracked as
    // masks or data and converted between, cross-lane operations reach the whole console wave. Without
    // it (graphics) each desktop thread runs a console lane of whatever desktop wave it lands in.
    bool exact;
    u8 lanes; // console lanes per desktop thread: 2 for wave64 programs in exact mode, else 1
    u8 half;  // the half (console lanes 32 * half + desktop lane) a per-lane instruction is emitted for
    bool in_lane_op;  // emitting a per-lane instruction (inside its EXEC guards)
    bool exec_bits;   // the scalar result being written is data: written to EXEC, it is converted
    u32 wave_threads; // exact mode: threads per group, console waves per group
    u32 group_waves;
    u8 hoisted[SHADER_MAX_HOISTED]; // SGPRs (mask dwords) read as data by the instruction being emitted
    u32 hoisted_count;
    bool dpp_active;
    bool dpp_has_mask;
    u64 root_used;       // root descriptor parameters read
    u32 sampler_compare; // inline samplers used for comparisons, by index
    PcPool pc_pools[SHADER_MAX_PC_POOLS];
    u32 pc_pool_count;
    // Spill VGPRs (see analyze_tags): slot of each VGPR (0xFF: none) and the lanes each slot uses.
    u8 spill_vgprs[SHADER_MAX_SPILL_VGPRS];
    u8 spill_vgpr_count;
    u8 spill_slot[256];
    u64 spill_lanes[SHADER_MAX_SPILL_VGPRS];
    // Descriptors at known places that the program writes or samples through (find_slot_uses); with
    // more than fit, the rest count as unknown.
    SlotUse slot_uses[SHADER_MAX_SLOT_USES];
    u32 slot_use_count;

    // Vector writes that may write every lane (see translate_lanes.cpp): per instruction, and for the
    // instruction being emitted. keep_exec_guards turns the analysis off; lane_check_failed: emission
    // found an instruction the analysis modelled wrongly (translation starts over without it).
    u8* lane_write;
    bool keep_exec_guards;
    bool write_all_lanes;
    bool lane_check_failed;
    u32 lane_written; // destination registers write_vgpr wrote for the instruction being emitted

    // Scratch strings for operand expressions (a ring: an expression stays valid for a while).
    char ring[32][320];
    u32 ring_at;
};

// Value types of operands, which decide how inline constants read.
enum OperandType : u8 {
    TYPE_32,
    TYPE_16,
    TYPE_64,
};

void translator_fail(Translator& t, const char* format, ...);
void detect_wave_size(Translator& t);
void tags_transfer(const Translator& t, TagState& s, const RdnaInst& in, u32 inst_idx = ~0u);
// Whether an instruction copies register values as they are, so that its results keep their sources'
// tags (moves, spills, recognized swaps). Everything else computes values: its sources are read as
// values (register_data). tags_transfer and emission both decide by this.
bool copies_tags(const Translator& t, const TagState& s, const RdnaInst& in, u32 inst_idx);
// Whether a scalar register holding a loaded descriptor dword on one path into a block only becomes a
// value there: the path reads the dword from the descriptor shadow as it jumps (see tags_merge).
bool join_converts(Tag tag);
// A register read as a value by the instruction being emitted: `raw` (its HLSL variable), or, for a
// loaded descriptor dword (which translated code holds the heap address of), the dword from the
// descriptor shadow. Fails translation where neither is the register's value. `vector`: a vector
// register, whose lanes outside EXEC hold no address to read.
const char* register_data(Translator& t, Tag tag, const char* raw, bool vector);
// An SGPR read as a value (register_data); with exact lanes, a mask reads as its bits.
const char* sgpr_data(Translator& t, u32 reg);
const char* vgpr_data(Translator& t, u32 reg);
// Records which descriptors at known places the program writes or samples through (SLOT_* flags), which
// decides the desktop view of accesses through them. translate_memory.cpp
void find_slot_uses(Translator& t);
// A v_readlane/v_writelane lane operand known at translation time (a literal or inline integer), in the
// wave's lane range.
bool constant_lane(const Translator& t, const RdnaOperand& o, u32* lane);
// Exact lanes: whether a register holding `out` where a block ends goes into a block starting with
// `entry` as a mask converted to its bits (see merge_tag in translate.cpp).
bool edge_converts(const Translator& t, Tag out, Tag entry);
bool sgpr_live_in(const Translator& t, u32 block, u32 reg);
// Exact lanes: whether an SGPR holds data that reads the same as a mask (all bits clear or set).
bool sgpr_neutral(const TagState& s, u32 reg);
bool picks_registers_at_run_time(RdnaOp op);
// Whether destination `d` is a lane mask result (compares, carries, saveexec): one dword in wave32,
// whatever the operand says.
bool writes_lane_mask(const RdnaInst& in, u32 d);
// The registers [a, a + width) and [b, b + width) a recognized three-XOR swap exchanges, at its first
// XOR (see find_xor_swaps); false for any other instruction.
bool xor_swap_registers(const Translator& t, u32 inst_idx, u32* a, u32* b, u32* width);
// Decides which vector writes may write every lane: those whose lanes outside EXEC nothing reads before
// they are overwritten. translate_lanes.cpp
bool analyze_lane_writes(Translator& t);
// Whether the analysis takes an instruction's vector write to replace its destination outright (not
// reading the old value): emission must not read it then (see lane_check_failed).
bool lane_write_replaces(const Translator& t, const RdnaInst& in);
// Whether emission wrote every destination register the analysis takes an instruction to write
// (`written`: a bit per register, from write_vgpr).
bool lane_write_complete(const Translator& t, const RdnaInst& in, u32 written);
// Marks (a byte per register) the vector registers an instruction reads, destinations it keeps part of
// (or stores from) included.
void vector_registers_read(const RdnaInst& in, u8* regs);
void emit_program(Translator& t);
// A block's instructions; t.tags ends as the state where it ends. translate_emit.cpp
void emit_block(Translator& t, u32 b);
// The condition under which the jump ending a block is taken (with `negate`: not taken).
const char* branch_condition(Translator& t, const RdnaInst& in, bool negate);
// The program's blocks as structured control flow (or the dispatcher). translate_flow.cpp
void emit_control_flow(Translator& t);
// Join conversions on the edge to block `next` (see translate_flow.cpp); with `emitting` false, only
// whether there are any.
bool emit_join_conversions(Translator& t, s32 next, bool emitting);
// For failure messages: why register `reg` (a VGPR if `vector`) holds nothing the translator can place
// where the instruction being emitted reads it: the instruction that last wrote it in the block, or
// what each path into the block brings. Failure paths only (it repeats analysis).
void describe_tag_loss(const Translator& t, bool vector, u32 reg, char* out, usize size);

// Formatting into the scratch ring.
const char* fmt(Translator& t, const char* format, ...);
// Statements at the current depth.
void emit(Translator& t, const char* format, ...);
void use_sgpr(Translator& t, u32 reg);
void use_vgpr(Translator& t, u32 reg);
// Source operand `i` as a uint expression (dword `part` of a wider operand), without modifiers.
const char* src_u32(Translator& t, const RdnaInst& in, u32 i, OperandType type = TYPE_32, u32 part = 0);
// Source operand `i` as a float expression with its abs/neg modifiers.
const char* src_f32(Translator& t, const RdnaInst& in, u32 i);
const char* sgpr(Translator& t, u32 reg);
const char* vgpr(Translator& t, u32 reg);
// A vector register's variable for one half (two lanes per thread; the half is ignored otherwise).
const char* vgpr_half(Translator& t, u32 reg, u32 half);
// Writes a vector destination (EXEC-guarded, honoring SDWA destination selects).
void write_vgpr(Translator& t, const RdnaInst& in, u32 reg, const char* value);
// Writes a scalar destination dword; writes to null are dropped.
void write_sgpr(Translator& t, const RdnaOperand& dst, u32 part, const char* value);
// A float result with the instruction's output modifier and clamp, as uint bits.
const char* float_result(Translator& t, const RdnaInst& in, const char* value);
// Lane mask registers in this wave size.
u32 mask_dwords(const Translator& t);

// The EXEC test for the lanes being emitted: one per half for two lanes per thread.
const char* exec_cond(const Translator& t);
// Operand `i` (dword `part`) in mask form: masks as they are, data bits converted to this lane's bit
// broadcast.
const char* mask_operand(Translator& t, const RdnaInst& in, u32 i, u32 part);
// Operand `i` (dword `part`) as it is held, mask or data (moves keep the form, the tags follow).
const char* raw_operand(Translator& t, const RdnaInst& in, u32 i, u32 part);
// The value of a constant expression as operands are written ("0u", "0x%08xu").
bool literal_value(const char* expr, u32* value);
// A dword of bits in mask form (constants fold).
const char* lane_mask_of(Translator& t, const char* bits);
// Whether an SGPR holds a mask in this exact-mode program (EXEC always does).
bool sgpr_is_mask(Translator& t, u32 reg);
// The mask dword a per-lane instruction's lane mask result goes to (0 for one lane per thread).
u32 lane_mask_part(const Translator& t);

bool emit_alu(Translator& t, const RdnaInst& in);    // translate_alu.cpp
bool emit_memory(Translator& t, const RdnaInst& in); // translate_memory.cpp
// Whether an image instruction's translation takes derivatives across the quad (an implicit LOD): it
// reads its address registers in the neighbouring lanes too. translate_memory.cpp
bool image_reads_neighbours(const Translator& t, RdnaOp op);
// A vertex shader's call of the driver's fetch subroutine (see ShaderInfo::fetch_vgpr): its loads, in
// line. translate_memory.cpp
bool emit_fetch_subroutine(Translator& t, const RdnaInst& in);
