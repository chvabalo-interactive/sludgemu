// RDNA 2 (gfx10.3) machine code: decoding and disassembly.
//
// Console shaders arrive as GPU machine code. The decoder turns it into a flat list of instructions
// with their operands spelled out, which the translator walks; the disassembler prints that list in
// the usual LLVM-style syntax for logs and analysis.

#pragma once

#include "common/types.h"

enum RdnaEncoding : u8 {
    ENC_INVALID,
    ENC_SOP2,
    ENC_SOPK,
    ENC_SOP1,
    ENC_SOPC,
    ENC_SOPP,
    ENC_SMEM,
    ENC_VOP2,
    ENC_VOP1,
    ENC_VOPC,
    ENC_VOP3,
    ENC_VOP3P,
    ENC_VINTRP,
    ENC_DS,
    ENC_MUBUF,
    ENC_MTBUF,
    ENC_MIMG,
    ENC_EXP,
    ENC_FLAT, // the FLAT encoding with its segment: flat, scratch or global
    ENC_GLOBAL,
    ENC_COUNT,
};

enum RdnaOp : u16 {
    OP_INVALID,
#define RDNA_OP(name, enc, opcode) OP_##name,
#include "shader/rdna_ops.inc"
#undef RDNA_OP
    OP_COUNT,
};

// Operand register files and special values, from the 9-bit source field (scalar fields use the
// low 8 bits).
enum RdnaOperandKind : u8 {
    OPND_NONE,
    OPND_SGPR,    // s0..s105, also ttmp (reg 108..123) and the special registers below
    OPND_VGPR,    // v0..v255
    OPND_INLINE,  // inline constant: `code` holds the 9-bit encoding (128..248)
    OPND_LITERAL, // 32-bit literal following the instruction: `value`
    OPND_SPECIAL, // vcc, exec, m0, scc, null, ...: `code` holds the encoding
};

// Special operand encodings.
enum : u16 {
    SRC_VCC_LO = 106,
    SRC_VCC_HI = 107,
    SRC_TTMP0 = 108,
    SRC_M0 = 124,
    SRC_NULL = 125,
    SRC_EXEC_LO = 126,
    SRC_EXEC_HI = 127,
    SRC_ZERO = 128,
    SRC_INT_MAX = 192,     // 64
    SRC_INT_NEG_MAX = 208, // -16
    SRC_DPP8 = 233,
    SRC_DPP8_FI = 234,
    SRC_SHARED_BASE = 235,
    SRC_SHARED_LIMIT = 236,
    SRC_PRIVATE_BASE = 237,
    SRC_PRIVATE_LIMIT = 238,
    SRC_POPS_EXITING_WAVE_ID = 239,
    SRC_HALF = 240, // 0.5, -0.5, 1.0, -1.0, 2.0, -2.0, 4.0, -4.0, 1/(2*pi) through 248
    SRC_INV_2PI = 248,
    SRC_SDWA = 249,
    SRC_DPP16 = 250,
    SRC_VCCZ = 251,
    SRC_EXECZ = 252,
    SRC_SCC = 253,
    SRC_LDS_DIRECT = 254,
    SRC_LITERAL = 255,
    SRC_VGPR0 = 256,
};

struct RdnaOperand {
    RdnaOperandKind kind;
    u8 count; // registers covered (1 for 32 bits, 2 for 64, ...)
    bool neg;
    bool abs;
    u16 reg;   // SGPR or VGPR index; the encoding for OPND_INLINE and OPND_SPECIAL
    u32 value; // literal
};

// DPP16 (data parallel primitives): the lane source pattern for src0.
struct RdnaDpp {
    u16 ctrl;
    u8 row_mask;
    u8 bank_mask;
    bool bound_ctrl;
    bool fetch_inactive;
    u8 sel8[8]; // DPP8: source lane within each group of 8
    bool dpp8;
};

// SDWA (sub-dword addressing): which bytes or words the sources and destination use.
struct RdnaSdwa {
    u8 dst_sel;
    u8 dst_unused;
    u8 src_sel[2];
    bool src_sext[2];
};

enum RdnaSdwaSel : u8 {
    SDWA_BYTE_0,
    SDWA_BYTE_1,
    SDWA_BYTE_2,
    SDWA_BYTE_3,
    SDWA_WORD_0,
    SDWA_WORD_1,
    SDWA_DWORD,
};

// Memory and export fields, by encoding.
struct RdnaMemory {
    s32 offset;         // SMEM, MUBUF, MTBUF, FLAT: immediate byte offset; DS: offset0
    u8 offset1;         // DS: second offset (read2/write2)
    bool offen, idxen;  // MUBUF/MTBUF: the vaddr VGPRs hold an offset and/or index
    bool glc, slc, dlc; // cache policy
    bool lds;           // MUBUF: load into LDS
    bool tfe, lwe;      // texture fail enable, LOD warning enable
    bool gds;           // DS: global data share
    u8 format;          // MTBUF: unified buffer format
    u8 dmask;           // MIMG: components read or written
    u8 dim;             // MIMG: resource dimension (RdnaImageDim)
    bool unorm;         // MIMG: unnormalized coordinates
    bool r128;          // MIMG: 128-bit resource descriptor
    bool a16, d16;      // MIMG: 16-bit addresses, 16-bit data
    u8 nsa_count;       // MIMG: extra address VGPRs in non-sequential-address form
    u8 nsa_addr[13];    // MIMG: address VGPRs after the first, in NSA form
    u8 segment;         // FLAT: 0 flat, 1 scratch, 2 global
};

enum RdnaImageDim : u8 {
    DIM_1D,
    DIM_2D,
    DIM_3D,
    DIM_CUBE,
    DIM_1D_ARRAY,
    DIM_2D_ARRAY,
    DIM_2D_MSAA,
    DIM_2D_MSAA_ARRAY,
};

struct RdnaExport {
    u8 target; // 0..7 mrt, 8 mrtz, 9 null, 12..16 pos, 20 prim, 32..63 param
    u8 enable; // component mask
    bool compressed;
    bool done;
    bool valid_mask;
};

enum : u8 {
    EXP_MRT0 = 0,
    EXP_MRTZ = 8,
    EXP_NULL = 9,
    EXP_POS0 = 12,
    EXP_PRIM = 20,
    EXP_PARAM0 = 32,
};

struct RdnaInst {
    RdnaOp op;
    RdnaEncoding enc; // as encoded (VOP3 for promoted VOP1/VOP2/VOPC forms)
    u8 dwords;        // length including literal and DPP/SDWA dwords
    u32 offset;       // byte offset in the program

    RdnaOperand dst[2]; // dst[1]: VOP3b carry-out / compare result SGPRs
    RdnaOperand src[4];
    u8 num_src;

    // VALU modifiers.
    bool clamp;
    u8 omod;      // output modifier: 0 none, 1 *2, 2 *4, 3 /2
    u8 op_sel;    // VOP3/VOP3P: per-operand high-half select
    u8 op_sel_hi; // VOP3P
    u8 neg_hi;    // VOP3P
    bool has_dpp, has_sdwa;
    RdnaDpp dpp;
    RdnaSdwa sdwa;

    s32 simm16; // SOPP/SOPK immediate; for branches the target byte offset (see branch_target)
    u32 branch_target;
    // VINTRP, and the 16-bit interpolation instructions (VOP3): the attribute and channel, and for
    // 16-bit ones which half of each attribute dword holds the value
    u8 attr, attr_chan;
    bool attr_high;

    RdnaMemory mem;
    RdnaExport exp;
};

const char* rdna_op_name(RdnaOp op);
RdnaEncoding rdna_op_encoding(RdnaOp op);

// Decodes one instruction at byte `offset`. Returns false on an invalid or truncated encoding.
bool rdna_decode(const u32* code, u32 code_dwords, u32 offset, RdnaInst* out);

// Address VGPRs an image instruction reads (32-bit addresses, sequential form).
u32 rdna_image_address_count(RdnaOp op, u8 dim);

// Writes the instruction as text (no trailing newline). Returns the length written.
usize rdna_disassemble(const RdnaInst& inst, char* out, usize size);

// Must run before the functions above.
void rdna_init();
