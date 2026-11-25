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
    TAG_MASK,     // a lane mask (broadcast form)
    TAG_HEAP,     // a byte address in a desktop descriptor heap; payload: heap | parameter << 1
    TAG_ROOT_HI,  // the high dword of a root descriptor (maybe with stride bits); payload: parameter
    TAG_VB_TABLE, // the vertex buffer table address
    TAG_VB_DESC,  // a vertex buffer descriptor dword; payload: slot
    TAG_PC,       // code-relative constant data offset (s_getpc_b64 + offset)
};

enum : u16 {
    HEAP_RESOURCES = 0,
    HEAP_SAMPLERS = 1,
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
    // Constant 32-bit values carried in vector registers (for MIMG texel offsets).
    u32 vgpr_val[256];
    bool vgpr_const[256];
    // Scalar tags and descriptors spilled to lanes of tracked VGPRs via v_writelane_b32.
    Tag spill_tag[SHADER_MAX_SPILL_VGPRS][64];
    u16 spill_desc[SHADER_MAX_SPILL_VGPRS][64];
};

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

struct Translator {
    const ShaderInfo* info;
    TranslatedShader* out;
    const u8* code;
    u32 code_size;
    Program program;
    TagState* block_entry; // tags at each block's start
    u8* xor_swap_role;     // per-instruction: 1 = start of 3-xor swap, 2 = swap continuation
    TagState tags;         // tags before the instruction being emitted
    Text body;             // the program's statements; declarations are written around them
    u32 depth;             // indentation of emitted statements
    bool failed;

    u8 sgpr_used[128];
    u8 vgpr_used[256];
    bool scc_used;
    u32 ps_input_vgpr[16];  // pixel shaders: first VGPR of each SPI_PS_INPUT_ADDR input, or ~0u
    u8 attribute_modes[32]; // pixel shaders: interpolations each attribute is read with (1 << mode)
    u8 vs_fetch_components[SHADER_MAX_VERTEX_ELEMENTS]; // vertex shaders: dwords fetched per element
    bool fetch_typed[SHADER_MAX_VERTEX_ELEMENTS];
    u8 fetch_class[SHADER_MAX_VERTEX_ELEMENTS]; // typed fetches: 0 float, 1 uint, 2 sint
    u8 param_components[32];
    u8 position_exported;
    u8 vs_clip_mask;
    u8 target_compressed[8];
    bool needs_descriptor_info;
    bool uses_lds;
    bool wave32;
    bool dpp_active;
    bool dpp_has_mask;
    u64 root_used;       // root descriptor parameters read
    u32 sampler_compare; // inline samplers used for comparisons, by index
    PcPool pc_pools[SHADER_MAX_PC_POOLS];
    u32 pc_pool_count;
    u8 spill_vgprs[SHADER_MAX_SPILL_VGPRS];
    u8 spill_vgpr_count;
    u8 spill_slot[256];

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
void emit_program(Translator& t);

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
// Writes a vector destination (EXEC-guarded, honoring SDWA destination selects).
void write_vgpr(Translator& t, const RdnaInst& in, u32 reg, const char* value);
// Writes a scalar destination dword; writes to null are dropped.
void write_sgpr(Translator& t, const RdnaOperand& dst, u32 part, const char* value);
// A float result with the instruction's output modifier and clamp, as uint bits.
const char* float_result(Translator& t, const RdnaInst& in, const char* value);
// Lane mask registers in this wave size.
u32 mask_dwords(const Translator& t);

bool emit_alu(Translator& t, const RdnaInst& in);    // translate_alu.cpp
bool emit_memory(Translator& t, const RdnaInst& in); // translate_memory.cpp
