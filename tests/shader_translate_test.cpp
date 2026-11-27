// Shader translation: hand-assembled RDNA 2 programs through shader_translate(), checking what the
// static analysis makes of them (samplers built from constants, descriptors moved through register
// swaps) and that what it can't place fails instead of translating wrong.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hle/d3d12/descriptor_shadow.h"
#include "shader/rdna.h"
#include "shader/translate.h"
#include "test.h"

// --- a small assembler ---------------------------------------------------------------------------------

// Operand encodings of the scalar source fields.
enum : u32 {
    VCC = 106,
    VCC_HI = 107,
    NUL = 125,
    EXEC_LO = 126,
    ZERO = 128, // inline integers: 128 + n for 0..64
    MINUS_ONE = 193,
    LIT = 255, // a literal follows
    V0 = 256,  // VALU sources: VGPRs from 256
};

struct Asm {
    u32 w[256];
    u32 n;

    void put(u32 word) {
        if (n < 256)
            w[n++] = word;
    }
    void sop1(u32 op, u32 sdst, u32 s0, u32 literal = 0) {
        put(0xBE800000u | sdst << 16 | op << 8 | s0);
        if (s0 == LIT)
            put(literal);
    }
    void sop2(u32 op, u32 sdst, u32 s0, u32 s1, u32 literal = 0) {
        put(0x80000000u | op << 23 | sdst << 16 | s1 << 8 | s0);
        if (s0 == LIT || s1 == LIT)
            put(literal);
    }
    void sopk(u32 op, u32 sdst, u32 imm) { put(0xB0000000u | op << 23 | sdst << 16 | (imm & 0xFFFF)); }
    void sopp(u32 op, u32 imm) { put(0xBF800000u | op << 16 | (imm & 0xFFFF)); }
    void smem(u32 op, u32 sdata, u32 sbase, u32 offset) {
        put(0xF4000000u | op << 18 | sdata << 6 | sbase / 2);
        put(NUL << 25 | offset);
    }
    // Sampling and loads: 2D, `dmask` components.
    void mimg(u32 op, u32 vdata, u32 vaddr, u32 srsrc, u32 ssamp, u32 dmask) {
        put(0xF0000000u | (op & 0x7F) << 18 | dmask << 8 | 1u << 3 | (op >> 7));
        put((ssamp / 4) << 21 | (srsrc / 4) << 16 | vdata << 8 | vaddr);
    }
    // Any MIMG access of dimension `dim`, with the glc bit.
    void mimg_full(u32 op, u32 vdata, u32 vaddr, u32 srsrc, u32 ssamp, u32 dmask, u32 dim, bool glc) {
        put(0xF0000000u | (op & 0x7F) << 18 | (glc ? 1u : 0u) << 13 | dmask << 8 | dim << 3 | (op >> 7));
        put((ssamp / 4) << 21 | (srsrc / 4) << 16 | vdata << 8 | vaddr);
    }
    void vop3(u32 op, u32 vdst, u32 s0, u32 s1, u32 s2 = 0) {
        put(0xD4000000u | op << 16 | vdst);
        put(s2 << 18 | s1 << 9 | s0);
    }
    void vopc(u32 op, u32 s0, u32 vsrc1) { put(0x7C000000u | op << 17 | vsrc1 << 9 | s0); }
    void sopc(u32 op, u32 s0, u32 s1) { put(0xBF000000u | op << 16 | s1 << 8 | s0); }
    // Untyped buffer access with a byte offset in a VGPR.
    void mubuf_offen(u32 op, u32 vdata, u32 vaddr, u32 srsrc) {
        put(0xE0000000u | op << 18 | 1u << 12);
        put(ZERO << 24 | (srsrc / 4) << 16 | vdata << 8 | vaddr);
    }
    // An indexed buffer access (the index in `vaddr`), returning the old value for atomics.
    void mubuf_idxen(u32 op, u32 vdata, u32 vaddr, u32 srsrc) {
        put(0xE0000000u | op << 18 | 1u << 14 | 1u << 13);
        put(ZERO << 24 | (srsrc / 4) << 16 | vdata << 8 | vaddr);
    }
    // A global load from the 64-bit address in v[vaddr:vaddr+1].
    void global_load(u32 op, u32 vdst, u32 vaddr, u32 offset) {
        put(0xDC000000u | op << 18 | 2u << 14 | (offset & 0xFFF));
        put(vdst << 24 | NUL << 16 | vaddr);
    }
    void vop1(u32 op, u32 vdst, u32 s0) { put(0x7E000000u | vdst << 17 | op << 9 | s0); }
    void vop2(u32 op, u32 vdst, u32 s0, u32 vsrc1) { put(op << 25 | vdst << 17 | vsrc1 << 9 | s0); }
    void end() { sopp(1, 0); }
};

// Opcodes (rdna_ops.inc).
enum : u32 {
    SOP2_ADD_U32 = 0,
    SOP2_AND_B32 = 14,
    SOP2_LSHL_B32 = 30,
    SOP2_OR_B32 = 16,
    SOP2_XOR_B32 = 18,
    SOP2_XOR_B64 = 19,
    SOP2_BFM_B32 = 36,
    SOP2_BFM_B64 = 37,
    SOP1_MOV_B32 = 3,
    SOP1_MOV_B64 = 4,
    SOP1_CMOV_B64 = 6,
    SOP1_MOVRELD_B32 = 48,
    SOPK_MOVK_I32 = 0,
    SOPK_CMPK_LG_U32 = 10,
    SOPK_ADDK_I32 = 15,
    SMEM_LOAD_DWORD = 0,
    SMEM_LOAD_DWORDX4 = 2,
    SMEM_LOAD_DWORDX8 = 3,
    MIMG_SAMPLE_LZ = 39,
    MIMG_SAMPLE_C_LZ = 47,
    VOPC_CMP_EQ_U32 = 0xC2,
    SOPC_CMP_EQ_U32 = 6,
    SOPP_NOP = 0,
    SOPP_BRANCH = 2,
    SOPP_CBRANCH_SCC0 = 4,
    SOPP_CBRANCH_EXECNZ = 9,
    SOP2_SUB_U32 = 1,
    SOP2_ADDC_U32 = 4,
    SOP2_SUBB_U32 = 5,
    SOP2_CSELECT_B32 = 10,
    SOP2_CSELECT_B64 = 11,
    MIMG_LOAD = 0,
    SOP1_GETPC_B64 = 31,
    SOP1_SETPC_B64 = 32,
    SMEM_LOAD_DWORDX2 = 1,
    MIMG_ATOMIC_CMPSWAP = 16,
    MIMG_ATOMIC_ADD = 17,
    MIMG_ATOMIC_SUB = 18,
    MIMG_ATOMIC_SMIN = 20,
    MIMG_ATOMIC_UMAX = 23,
    MIMG_ATOMIC_INC = 27,
    MIMG_GATHER4_L = 68,
    MIMG_GATHER4_B = 69,
    MIMG_GATHER4_LZ = 71,
    MIMG_GATHER4_C_L = 76,
    MIMG_GATHER4_L_O = 84,
    MUBUF_LOAD_DWORD = 12,
    MUBUF_STORE_DWORD = 28,
    MUBUF_ATOMIC_ADD = 50,
    MIMG_STORE = 8,
    GLOBAL_LOAD_DWORD = 12,
    GLOBAL_LOAD_DWORDX2 = 13,
    VOP1_MOV_B32 = 1,
    VOP2_LSHLREV_B32 = 26,
    VOP2_ADD_NC_U32 = 37,
    VOP3_READLANE = 0x360,
    VOP3_WRITELANE = 0x361,
};

// A compute program with a resource table in s0 (all SRVs) and a sampler table in s1. Launched as wave32
// (these programs don't tell): one console lane per desktop thread, one variable per register. Wave64
// lane mapping has tests of its own (shader_lanes_test.cpp).
static ShaderInfo compute_info() {
    ShaderInfo info = {};
    info.stage = SHADER_COMPUTE;
    info.user_sgprs = 2;
    info.parameter_count = 2;
    info.parameters[0].kind = USER_DATA_RESOURCE_TABLE;
    info.parameters[0].sgpr = 0;
    info.parameters[0].dwords = 1;
    info.parameters[0].range_count = 1;
    info.parameters[0].ranges[0] = {RANGE_SRV, 0, ~0u};
    info.parameters[1].kind = USER_DATA_SAMPLER_TABLE;
    info.parameters[1].sgpr = 1;
    info.parameters[1].dwords = 1;
    info.resource_heap_sgpr = NO_HEAP_SGPR;
    info.sampler_heap_sgpr = NO_HEAP_SGPR;
    info.threads[0] = 64;
    info.threads[1] = 1;
    info.threads[2] = 1;
    info.thread_id_count = 1;
    info.wave_size = 32;
    return info;
}

// Loads the texture descriptor from slot 0 of the resource table into s[8:15], and with `sampler` the
// sampler descriptor from slot 0 of the sampler table into s[16:19].
static void load_descriptors(Asm& a, bool sampler) {
    a.sop1(SOP1_MOV_B32, 2, 0);
    a.sop1(SOP1_MOV_B32, 3, ZERO);
    a.smem(SMEM_LOAD_DWORDX8, 8, 2, 0);
    if (sampler) {
        a.sop1(SOP1_MOV_B32, 4, 1);
        a.sop1(SOP1_MOV_B32, 5, ZERO);
        a.smem(SMEM_LOAD_DWORDX4, 16, 4, 0);
    }
}

// A constant sampler descriptor in s[104:107] the way compilers build it, the upper half in VCC:
// clamp, bilinear, point mips, LOD 0..12.
static void constant_sampler_in_vcc(Asm& a) {
    a.sopk(SOPK_MOVK_I32, 104, 0x92);
    a.sop2(SOP2_BFM_B32, 105, ZERO + 2, ZERO + 22);
    a.sop1(SOP1_MOV_B64, VCC, LIT, 0x14500000);
}

static void sample(Asm& a, u32 ssamp) {
    a.mimg(MIMG_SAMPLE_LZ, 0, 0, 8, ssamp, 0xF);
}

static TranslatedShader g_out;
// A desktop device running waves of 32 lanes, which compute translation needs.
static const ShaderTarget k_target = {32, 32};

// With SLUDGE_TEST_HLSL=<dir>, each translation's HLSL is also written there (<n>_cs.hlsl), to check
// that the desktop compiler takes it.
static bool translate(const Asm& a, const ShaderInfo& info) {
    static u32 count = 0;
    bool ok = shader_translate(a.w, a.n * 4, info, k_target, &g_out);
    const char* dir = getenv("SLUDGE_TEST_HLSL");
    if (ok && dir) {
        char path[300];
        snprintf(path, sizeof(path), "%s\\test%03u_%s.hlsl", dir, count++,
                 info.stage == SHADER_PIXEL    ? "ps"
                 : info.stage == SHADER_VERTEX ? "vs"
                                               : "cs");
        FILE* f = fopen(path, "w");
        if (f) {
            fputs(g_out.hlsl.data, f);
            fclose(f);
        }
    }
    return ok;
}

static bool error_has(const char* text) {
    bool found = strstr(g_out.error, text) != nullptr;
    if (!found)
        printf("  the error was: %s\n", g_out.error);
    return found;
}

static bool hlsl_has(const char* text) {
    return g_out.hlsl.data && strstr(g_out.hlsl.data, text) != nullptr;
}

// --- constant samplers ---------------------------------------------------------------------------------

static void test_constant_sampler_in_vcc() {
    ShaderInfo info = compute_info();
    Asm a = {};
    load_descriptors(a, false);
    constant_sampler_in_vcc(a);
    sample(a, 104);
    a.end();
    CHECK(translate(a, info));
    CHECK(g_out.sampler_count == 1);
    const InlineSampler& s = g_out.samplers[0];
    CHECK(s.filter == 0x14); // linear min/mag, point mips
    CHECK(s.address[0] == 3 && s.address[1] == 3 && s.address[2] == 3);
    CHECK(s.min_lod == 0.0f && s.max_lod == 12.0f);
    CHECK(s.comparison == 1 && s.border_color == 0 && !s.unnormalized);
    CHECK(hlsl_has("SamplerState g_sampler0 : register(s0, space103);"));
    CHECK(hlsl_has("SamplerState smp_ = g_sampler0;"));
}

static void test_constant_sampler_bfm64_border() {
    ShaderInfo info = compute_info();
    Asm a = {};
    load_descriptors(a, false);
    a.sop2(SOP2_BFM_B64, 104, ZERO + 2, ZERO + 54); // s105 = 0x00C00000 (LOD up to 12), s104 = 0
    a.sop1(SOP1_MOV_B32, 104, LIT, 0x1B6);          // border addressing everywhere
    a.sop1(SOP1_MOV_B64, VCC, LIT, 0x14500000);
    sample(a, 104);
    a.end();
    CHECK(translate(a, info));
    CHECK(g_out.sampler_count == 1);
    const InlineSampler& s = g_out.samplers[0];
    CHECK(s.address[0] == 4 && s.address[1] == 4 && s.address[2] == 4);
    CHECK(s.border_color == 0); // transparent black
    CHECK(s.max_lod == 12.0f);
}

// Values a constant must not outlive: a compare writing VCC, a non-foldable XOR into VCC's high half.
static void test_stale_vcc_constants() {
    ShaderInfo info = compute_info();
    {
        Asm a = {};
        load_descriptors(a, false);
        constant_sampler_in_vcc(a);
        a.vopc(VOPC_CMP_EQ_U32, V0 + 0, 1);
        sample(a, 104);
        a.end();
        CHECK(!translate(a, info));
    }
    {
        // s_addk keeps a constant a constant: the bias field picks up the 1.
        Asm a = {};
        load_descriptors(a, false);
        constant_sampler_in_vcc(a);
        a.sopk(SOPK_ADDK_I32, VCC, 1);
        sample(a, 104);
        a.end();
        CHECK(translate(a, info));
        CHECK(g_out.sampler_count == 1 && g_out.samplers[0].mip_bias == 1.0f / 256.0f);
    }
    {
        Asm a = {};
        a.sop1(SOP1_MOV_B32, EXEC_LO, MINUS_ONE); // wave32
        load_descriptors(a, false);
        constant_sampler_in_vcc(a);
        a.sop2(SOP2_XOR_B32, VCC_HI, 0, VCC_HI); // s0 isn't known
        sample(a, 104);
        a.end();
        CHECK(!translate(a, info));
    }
}

static void constant_sampler(Asm& a, u32 reg, const u32 w[4]) {
    for (u32 i = 0; i < 4; i++)
        a.sop1(SOP1_MOV_B32, reg + i, LIT, w[i]);
}

static void test_sampler_decoding() {
    ShaderInfo info = compute_info();
    // Point everything, wrap, comparison function NEVER (0), LOD 0..1.
    u32 never[4] = {0x0, 0x100000, 0x04000000, 0};
    {
        // A comparing sample honours NEVER; without comparison the function is ignored.
        Asm a = {};
        load_descriptors(a, false);
        constant_sampler(a, 20, never);
        a.mimg(MIMG_SAMPLE_C_LZ, 0, 0, 8, 20, 0x1);
        a.mimg(MIMG_SAMPLE_LZ, 4, 0, 8, 20, 0xF);
        a.end();
        CHECK(translate(a, info));
        CHECK(g_out.sampler_count == 2);
        CHECK(g_out.samplers[0].filter == 0x80 && g_out.samplers[0].comparison == 1);
        CHECK(g_out.samplers[1].filter == 0x00 && g_out.samplers[1].comparison == 1);
        CHECK(hlsl_has("SamplerComparisonState g_sampler0"));
        CHECK(hlsl_has("SamplerState g_sampler1"));
    }
    {
        // A comparison function on a sampler used without comparison doesn't make a comparison filter.
        u32 less[4] = {1u << 12, 0x100000, 0x04000000, 0};
        Asm a = {};
        load_descriptors(a, false);
        constant_sampler(a, 20, less);
        a.mimg(MIMG_SAMPLE_LZ, 4, 0, 8, 20, 0xF);
        a.end();
        CHECK(translate(a, info));
        CHECK(g_out.sampler_count == 1 && g_out.samplers[0].filter == 0x00);
    }
    {
        // Comparison together with a min reduction has no D3D12 form.
        u32 min_reduction[4] = {1u << 29, 0x100000, 0x04000000, 0};
        Asm a = {};
        load_descriptors(a, false);
        constant_sampler(a, 20, min_reduction);
        a.mimg(MIMG_SAMPLE_C_LZ, 0, 0, 8, 20, 0x1);
        a.end();
        CHECK(!translate(a, info));
        CHECK(error_has("min/max reduction"));
    }
    {
        // Min and max reductions without comparison.
        u32 max_reduction[4] = {2u << 29, 0x100000, 0x04000000, 0};
        Asm a = {};
        load_descriptors(a, false);
        constant_sampler(a, 20, max_reduction);
        sample(a, 20);
        a.end();
        CHECK(translate(a, info));
        CHECK(g_out.sampler_count == 1 && g_out.samplers[0].filter == 0x180);
    }
    {
        // No mip filter: the first level only.
        u32 no_mips[4] = {0x0, 0xC00000, 0x00500000, 0};
        Asm a = {};
        load_descriptors(a, false);
        constant_sampler(a, 20, no_mips);
        sample(a, 20);
        a.end();
        CHECK(translate(a, info));
        CHECK(g_out.sampler_count == 1 && g_out.samplers[0].filter == 0x14);
        CHECK(g_out.samplers[0].min_lod == 0.0f && g_out.samplers[0].max_lod == 0.0f);
    }
    {
        // Anisotropic with point mips.
        u32 aniso_point[4] = {4u << 9, 0xC00000, 0x04F00000, 0};
        Asm a = {};
        load_descriptors(a, false);
        constant_sampler(a, 20, aniso_point);
        sample(a, 20);
        a.end();
        CHECK(translate(a, info));
        CHECK(g_out.sampler_count == 1 && g_out.samplers[0].filter == 0x54);
        CHECK(g_out.samplers[0].max_anisotropy == 16);
    }
    {
        // Unnormalized coordinates: clamped and without mips is fine, wrapping isn't.
        // Linear mips and a bias change nothing with the level held at 0: point mips, no bias.
        u32 clamped[4] = {0x92 | 1u << 15, 0, 0x08500010, 0};
        u32 wrapped[4] = {1u << 15, 0, 0x00500000, 0};
        Asm a = {};
        load_descriptors(a, false);
        constant_sampler(a, 20, clamped);
        sample(a, 20);
        a.end();
        CHECK(translate(a, info));
        CHECK(g_out.sampler_count == 1 && g_out.samplers[0].unnormalized);
        CHECK(g_out.samplers[0].filter == 0x14 && g_out.samplers[0].mip_bias == 0.0f);
        Asm b = {};
        load_descriptors(b, false);
        constant_sampler(b, 20, wrapped);
        sample(b, 20);
        b.end();
        CHECK(!translate(b, info));
        CHECK(error_has("unnormalized"));
    }
}

// --- heap samplers through register swaps ----------------------------------------------------------------

// Copies the loaded sampler s[16:19] to s[20:23] except its first dword, which comes through a
// three-XOR swap of s16 with s20 (s20 holding something else first).
static void test_swapped_heap_sampler() {
    ShaderInfo info = compute_info();
    for (u32 commuted = 0; commuted < 2; commuted++) {
        Asm a = {};
        load_descriptors(a, true);
        a.sop1(SOP1_MOV_B32, 21, 17);
        a.sop1(SOP1_MOV_B32, 22, 18);
        a.sop1(SOP1_MOV_B32, 23, 19);
        a.sop1(SOP1_MOV_B32, 20, ZERO + 5);
        if (commuted)
            a.sop2(SOP2_XOR_B32, 16, 20, 16);
        else
            a.sop2(SOP2_XOR_B32, 16, 16, 20);
        a.sop1(SOP1_MOV_B32, 30, ZERO + 1); // unrelated work in between
        a.sop2(SOP2_XOR_B32, 20, 16, 20);
        a.sop1(SOP1_MOV_B32, 31, ZERO + 2);
        a.sop2(SOP2_XOR_B32, 16, 16, 20);
        sample(a, 20);
        a.end();
        CHECK(translate(a, info));
        CHECK(g_out.sampler_count == 0);
        CHECK(hlsl_has("SamplerDescriptorHeap[s20 >> 4]"));
    }
    {
        // 64 bits wide, with VCC: the sampler's upper half moves into VCC.
        Asm a = {};
        load_descriptors(a, true);
        a.sop1(SOP1_MOV_B64, 104, 16);
        a.sop1(SOP1_MOV_B64, VCC, ZERO);
        a.sop2(SOP2_XOR_B64, VCC, VCC, 18);
        a.sop2(SOP2_XOR_B64, 18, VCC, 18);
        a.sop2(SOP2_XOR_B64, VCC, VCC, 18);
        sample(a, 104);
        a.end();
        CHECK(translate(a, info));
        CHECK(g_out.sampler_count == 0);
        CHECK(hlsl_has("SamplerDescriptorHeap[s104 >> 4]"));
    }
    {
        // Another write to one of the registers between the steps: not a swap.
        Asm a = {};
        load_descriptors(a, true);
        a.sop1(SOP1_MOV_B32, 21, 17);
        a.sop1(SOP1_MOV_B32, 22, 18);
        a.sop1(SOP1_MOV_B32, 23, 19);
        a.sop1(SOP1_MOV_B32, 20, ZERO + 5);
        a.sop2(SOP2_XOR_B32, 16, 16, 20);
        a.sop1(SOP1_MOV_B32, 20, ZERO + 7);
        a.sop2(SOP2_XOR_B32, 20, 16, 20);
        a.sop2(SOP2_XOR_B32, 16, 16, 20);
        sample(a, 20);
        a.end();
        CHECK(!translate(a, info));
        CHECK(error_has("sampler"));
    }
    {
        // Registers picked at run time anywhere in the program: no swap is certain.
        Asm a = {};
        load_descriptors(a, true);
        a.sop1(SOP1_MOV_B32, 21, 17);
        a.sop1(SOP1_MOV_B32, 22, 18);
        a.sop1(SOP1_MOV_B32, 23, 19);
        a.sop1(SOP1_MOV_B32, 20, ZERO + 5);
        a.sop2(SOP2_XOR_B32, 16, 16, 20);
        a.sop2(SOP2_XOR_B32, 20, 16, 20);
        a.sop2(SOP2_XOR_B32, 16, 16, 20);
        sample(a, 20);
        a.sop1(SOP1_MOVRELD_B32, 40, 41);
        a.end();
        CHECK(!translate(a, info));
        CHECK(error_has("sampler"));
    }
}

// --- descriptors changed after loading ------------------------------------------------------------------

// Translated code keeps a loaded descriptor's heap address only, so what changes a descriptor (or puts
// one together) must fail rather than read the unchanged one. Reading a dword the descriptor shadow
// doesn't hold (samplers', images') as a value fails where it is read.
static void test_changed_descriptors() {
    ShaderInfo info = compute_info();
    {
        Asm a = {};
        load_descriptors(a, true);
        a.sop2(SOP2_AND_B32, 18, 18, LIT, 0xFFFFF000u); // a LOD bias override
        sample(a, 16);
        a.end();
        CHECK(!translate(a, info));
        CHECK(error_has("sampler descriptor dword read as a value"));
    }
    {
        Asm a = {};
        load_descriptors(a, true);
        a.sop2(SOP2_LSHL_B32, 18, 0, ZERO + 1); // a value from elsewhere
        sample(a, 16);
        a.end();
        CHECK(!translate(a, info));
        CHECK(error_has("sampler changed after loading"));
    }
    {
        Asm a = {};
        load_descriptors(a, true);
        a.sop1(SOP1_MOV_B32, 19, ZERO);
        sample(a, 16);
        a.end();
        CHECK(!translate(a, info));
        CHECK(error_has("sampler partly made of constants"));
    }
    {
        // Changed on one path only.
        Asm a = {};
        load_descriptors(a, true);
        a.sopc(SOPC_CMP_EQ_U32, 0, ZERO);
        a.sopp(SOPP_CBRANCH_SCC0, 1);
        a.sop2(SOP2_LSHL_B32, 18, 0, ZERO + 1);
        sample(a, 16);
        a.end();
        CHECK(!translate(a, info));
        CHECK(error_has("sampler changed after loading"));
    }
    {
        Asm a = {};
        load_descriptors(a, true);
        a.sop2(SOP2_OR_B32, 13, 13, LIT, 0x1000u);
        sample(a, 16);
        a.end();
        CHECK(!translate(a, info));
        CHECK(error_has("image descriptor dword read as a value"));
    }
    {
        Asm a = {};
        load_descriptors(a, true);
        a.sop2(SOP2_LSHL_B32, 13, 0, ZERO + 1);
        sample(a, 16);
        a.end();
        CHECK(!translate(a, info));
        CHECK(error_has("image descriptor changed after loading"));
    }
    {
        // The sampler's upper half parked in VCC and moved back keeps its origin.
        Asm a = {};
        load_descriptors(a, false);
        a.sop1(SOP1_MOV_B32, 4, 1);
        a.sop1(SOP1_MOV_B32, 5, ZERO);
        a.smem(SMEM_LOAD_DWORDX4, 104, 4, 0);
        a.sop1(SOP1_MOV_B64, 82, VCC);
        a.sop1(SOP1_MOV_B64, 80, 104);
        sample(a, 80);
        a.end();
        CHECK(translate(a, info));
        CHECK(hlsl_has("SamplerDescriptorHeap[s80 >> 4]"));
    }
    // A conditional move leaves either value: a constant sampler replaced by a heap one on some runs is
    // neither, the same heap sampler moved over itself stays one.
    for (u32 same = 0; same < 2; same++) {
        u32 clamp[4] = {0x92, 0x00C00000, 0x14500000, 0};
        Asm a = {};
        load_descriptors(a, true);
        a.sop1(SOP1_MOV_B64, 24, 16);
        a.sop1(SOP1_MOV_B64, 26, 18);
        if (!same)
            constant_sampler(a, 16, clamp);
        a.sopc(SOPC_CMP_EQ_U32, 0, ZERO);
        a.sop1(SOP1_CMOV_B64, 16, 24);
        a.sop1(SOP1_CMOV_B64, 18, 26);
        sample(a, 16);
        a.end();
        CHECK(translate(a, info) == (same != 0));
        CHECK(same ? hlsl_has("SamplerDescriptorHeap[s16 >> 4]")
                   : error_has("sampler loaded on some paths only"));
    }
    // Buffers: a raw view ignores a new record count, not a moved base address.
    for (u32 moved = 0; moved < 2; moved++) {
        Asm a = {};
        a.sop1(SOP1_MOV_B32, 2, 0);
        a.sop1(SOP1_MOV_B32, 3, ZERO);
        a.smem(SMEM_LOAD_DWORDX4, 8, 2, 0);
        if (moved)
            a.sop2(SOP2_ADD_U32, 8, 8, ZERO + 16);
        else
            a.sop2(SOP2_AND_B32, 10, 10, LIT, 0xFFFFu);
        a.mubuf_offen(MUBUF_LOAD_DWORD, 4, 0, 8);
        a.end();
        CHECK(translate(a, info) == !moved);
        CHECK(moved ? error_has("buffer descriptor changed after loading") : hlsl_has("ByteAddressBuffer"));
    }
    {
        // A scalar load through the address in a descriptor reads the memory behind it.
        Asm a = {};
        a.sop1(SOP1_MOV_B32, 2, 0);
        a.sop1(SOP1_MOV_B32, 3, ZERO);
        a.smem(SMEM_LOAD_DWORDX4, 8, 2, 0);
        a.smem(SMEM_LOAD_DWORD, 20, 8, 0);
        a.end();
        CHECK(!translate(a, info));
        CHECK(error_has("through a descriptor's address"));
    }
}

// --- view kinds by use ---------------------------------------------------------------------------------

// Loads the buffer descriptor at slot `slot` of the resource table into s[8:11].
static void load_buffer_descriptor(Asm& a, u32 slot) {
    a.sop1(SOP1_MOV_B32, 2, 0);
    a.sop1(SOP1_MOV_B32, 3, ZERO);
    a.smem(SMEM_LOAD_DWORDX4, 8, 2, slot * 32);
}

static const char k_shadow_kind_branch[] = "if (g_descriptor_info.Load((s8 & ~31u) + 16u) == 2u) {";

// Root signature ranges don't decide what a slot holds: what the program does through it does, and a
// slot it only reads takes the kind the descriptor shadow records.
static void test_view_kinds() {
    ShaderInfo info = compute_info();
    info.parameters[0].ranges[0] = {RANGE_CBV, 0, ~0u}; // labels that don't fit the use
    {
        // An atomic through a slot labeled as a constant buffer: an unordered access view, indexed with
        // the stride the descriptor holds.
        Asm a = {};
        load_buffer_descriptor(a, 10);
        a.mubuf_idxen(MUBUF_ATOMIC_ADD, 6, 5, 8);
        a.end();
        CHECK(translate(a, info));
        CHECK(hlsl_has("RWByteAddressBuffer b_ = ResourceDescriptorHeap[(s8 >> 5)];"));
        CHECK(hlsl_has("uint a_ = v5 * ((g_descriptor_info.Load(s9) >> 16) & 0x3fffu)"));
        CHECK(hlsl_has("InterlockedAdd(a_, v6, o_)"));
        CHECK(hlsl_has("ByteAddressBuffer g_descriptor_info : register(t0, space101);"));
        CHECK(!hlsl_has(k_shadow_kind_branch));
    }
    {
        // Read and written: read through the same unordered access view.
        Asm a = {};
        load_buffer_descriptor(a, 0);
        a.mubuf_offen(MUBUF_LOAD_DWORD, 4, 0, 8);
        a.mubuf_offen(MUBUF_STORE_DWORD, 4, 0, 8);
        a.end();
        CHECK(translate(a, info));
        CHECK(hlsl_has("RWByteAddressBuffer b_"));
        CHECK(!hlsl_has("    ByteAddressBuffer b_"));
        CHECK(!hlsl_has(k_shadow_kind_branch));
    }
    {
        // Only read: either view, by the slot's kind in the shadow.
        Asm a = {};
        load_buffer_descriptor(a, 0);
        a.mubuf_offen(MUBUF_LOAD_DWORD, 4, 0, 8);
        a.end();
        CHECK(translate(a, info));
        CHECK(hlsl_has(k_shadow_kind_branch));
        CHECK(hlsl_has("RWByteAddressBuffer b_"));
        CHECK(hlsl_has("    ByteAddressBuffer b_"));
    }
    {
        // Another slot written doesn't decide this one.
        Asm a = {};
        load_buffer_descriptor(a, 0);
        a.sop1(SOP1_MOV_B32, 4, 0);
        a.sop1(SOP1_MOV_B32, 5, ZERO);
        a.smem(SMEM_LOAD_DWORDX4, 12, 4, 32);
        a.mubuf_offen(MUBUF_STORE_DWORD, 4, 0, 12);
        a.mubuf_offen(MUBUF_LOAD_DWORD, 4, 0, 8);
        a.end();
        CHECK(translate(a, info));
        CHECK(hlsl_has(k_shadow_kind_branch));
    }
    {
        // Sampled through and written: no desktop view does both.
        Asm a = {};
        load_descriptors(a, true);
        sample(a, 16);
        a.mimg(MIMG_STORE, 0, 0, 8, 0, 0xF);
        a.end();
        CHECK(!translate(a, info));
        CHECK(error_has("both writes and samples"));
    }
}

// --- descriptor bits as values -------------------------------------------------------------------------

// Translated code holds the heap addresses of loaded descriptor dwords; reading one as a value reads the
// descriptor shadow there, and what is computed from it is a plain value.
static void test_descriptor_values() {
    ShaderInfo info = compute_info();
    {
        Asm a = {};
        load_buffer_descriptor(a, 0);
        a.sop2(SOP2_AND_B32, 20, 10, LIT, 0x1FFFFu); // the record count
        a.sop2(SOP2_ADD_U32, 21, 20, ZERO + 1);
        a.end();
        CHECK(translate(a, info));
        CHECK(hlsl_has("(g_descriptor_info.Load(s10) & 0x0001ffffu)"));
        CHECK(!hlsl_has("Load(s20)")); // s20 holds the value itself
    }
    {
        // Image descriptors' bits aren't in the shadow.
        Asm a = {};
        load_descriptors(a, false);
        a.sop2(SOP2_AND_B32, 20, 10, LIT, 0x3FFFu);
        a.end();
        CHECK(!translate(a, info));
        CHECK(error_has("image descriptor dword read as a value"));
    }
    {
        // A descriptor picked per lane: the address is the table's plus a scaled index, with a high
        // dword of zero (heaps are below 4 GiB), read from the shadow lane by lane.
        Asm a = {};
        a.vop2(VOP2_LSHLREV_B32, 2, ZERO + 5, 0);
        a.vop1(VOP1_MOV_B32, 3, ZERO);
        a.vop2(VOP2_ADD_NC_U32, 2, 0, 2);
        a.global_load(GLOBAL_LOAD_DWORD, 4, 2, 8);
        a.end();
        CHECK(translate(a, info));
        CHECK(hlsl_has("if (EXEC) v4 = g_descriptor_info.Load(v2 + 0x8u);"));
    }
    {
        // The second half of a slot is an image descriptor's dwords 4-7.
        Asm a = {};
        a.vop2(VOP2_LSHLREV_B32, 2, ZERO + 5, 0);
        a.vop1(VOP1_MOV_B32, 3, ZERO);
        a.vop2(VOP2_ADD_NC_U32, 2, 0, 2);
        a.global_load(GLOBAL_LOAD_DWORD, 4, 2, 20);
        a.end();
        CHECK(!translate(a, info));
        CHECK(error_has("image descriptor dwords 4-7"));
    }
    {
        // A dword loaded on one path and a value on the other, read after they join: the path holding
        // the address reads the dword before it jumps.
        Asm a = {};
        load_buffer_descriptor(a, 0);
        a.sopc(SOPC_CMP_EQ_U32, 0, ZERO);
        a.sopp(SOPP_CBRANCH_SCC0, 1);
        a.sop1(SOP1_MOV_B32, 10, ZERO + 7);
        a.sop2(SOP2_AND_B32, 20, 10, LIT, 0x1FFFFu);
        a.end();
        CHECK(translate(a, info));
        CHECK(hlsl_has("s10 = g_descriptor_info.Load(s10);"));
        CHECK(hlsl_has("(s10 & 0x0001ffffu)"));
    }
    {
        // A compare with an inline constant reads its register (decoded as the destination) and
        // leaves it a loaded dword.
        Asm a = {};
        load_buffer_descriptor(a, 0);
        a.sopk(SOPK_CMPK_LG_U32, 10, 0);
        a.sop2(SOP2_AND_B32, 20, 10, LIT, 0x1FFFFu);
        a.end();
        CHECK(translate(a, info));
        CHECK(hlsl_has("scc = g_descriptor_info.Load(s10) != 0x00000000u;"));
        CHECK(hlsl_has("(g_descriptor_info.Load(s10) & 0x0001ffffu)"));
    }
    {
        // A dword of a slot the program samples through, loaded on its own: an image descriptor's,
        // which the shadow doesn't hold.
        Asm a = {};
        load_descriptors(a, true);
        a.smem(SMEM_LOAD_DWORD, 20, 2, 4);
        sample(a, 16);
        a.sop2(SOP2_AND_B32, 21, 20, LIT, 0x3FFFu);
        a.end();
        CHECK(!translate(a, info));
        CHECK(error_has("image descriptor dword read as a value"));
    }
    {
        // A loaded dword in a vector register, read as a value: lanes outside EXEC read the first record.
        Asm a = {};
        load_buffer_descriptor(a, 0);
        a.vop1(VOP1_MOV_B32, 6, 10);
        a.vop2(VOP2_ADD_NC_U32, 7, V0 + 6, 0);
        a.end();
        CHECK(translate(a, info));
        CHECK(hlsl_has("g_descriptor_info.Load(EXEC ? v6 : 0u)"));
    }
    {
        // Stored as it is, it would reach memory as its heap address.
        Asm a = {};
        load_buffer_descriptor(a, 0);
        a.vop1(VOP1_MOV_B32, 6, 10);
        a.mubuf_offen(MUBUF_STORE_DWORD, 6, 0, 8);
        a.end();
        CHECK(!translate(a, info));
        CHECK(error_has("stores a descriptor dword"));
    }
    {
        // Two dwords of descriptors picked per lane into the address registers: the address is taken
        // before the first is written.
        Asm a = {};
        a.vop2(VOP2_LSHLREV_B32, 2, ZERO + 5, 0);
        a.vop1(VOP1_MOV_B32, 3, ZERO);
        a.vop2(VOP2_ADD_NC_U32, 2, 0, 2);
        a.global_load(GLOBAL_LOAD_DWORDX2, 2, 2, 4);
        a.end();
        CHECK(translate(a, info));
        CHECK(hlsl_has("{ uint a_ = v2 + 0x4u;"));
        CHECK(hlsl_has("if (EXEC) v3 = g_descriptor_info.Load2(a_).y;"));
    }
    {
        // A slot loaded four dwords at a time on one path and eight on the other is still the table's
        // slot after the join: a store through it there decides the view of a read elsewhere.
        Asm a = {};
        load_buffer_descriptor(a, 0);
        a.sopc(SOPC_CMP_EQ_U32, 0, ZERO);
        a.sopp(SOPP_CBRANCH_SCC0, 2);
        a.smem(SMEM_LOAD_DWORDX8, 8, 2, 0);
        a.mubuf_offen(MUBUF_STORE_DWORD, 6, 0, 8);
        a.smem(SMEM_LOAD_DWORDX4, 16, 2, 0);
        a.mubuf_offen(MUBUF_LOAD_DWORD, 7, 0, 16);
        a.end();
        CHECK(translate(a, info));
        CHECK(hlsl_has("RWByteAddressBuffer b_ = ResourceDescriptorHeap[(s16 >> 5)];"));
        CHECK(!hlsl_has("if (g_descriptor_info.Load((s16 & ~31u) + 16u) == 2u) {"));
    }
}

// --- long branches -------------------------------------------------------------------------------------

// s_getpc_b64 s[reg:reg+1]; s_add_u32 / s_sub_u32 with `delta`; s_addc_u32 / s_subb_u32; s_setpc_b64:
// a jump to the s_getpc_b64's offset + 4 + delta (negative: backwards).
static void long_branch(Asm& a, u32 reg, s32 delta) {
    a.sop1(SOP1_GETPC_B64, reg, 0);
    if (delta >= 0) {
        a.sop2(SOP2_ADD_U32, reg, reg, LIT, (u32)delta);
        a.sop2(SOP2_ADDC_U32, reg + 1, reg + 1, ZERO);
    } else {
        a.sop2(SOP2_SUB_U32, reg, reg, LIT, (u32)-delta);
        a.sop2(SOP2_SUBB_U32, reg + 1, reg + 1, ZERO);
    }
    a.sop1(SOP1_SETPC_B64, 0, reg);
}

static void test_long_branches() {
    ShaderInfo info = compute_info();
    {
        // Over a long branch to a trampoline after the last s_endpgm, which comes back with an s_branch
        // around padding nothing reaches.
        Asm a = {};
        a.sopp(SOPP_CBRANCH_EXECNZ, 5);    // 0x00 -> 0x18
        long_branch(a, 20, 0x18);          // 0x04..0x14 -> 0x20
        a.vop1(VOP1_MOV_B32, 1, ZERO + 1); // 0x18
        a.end();                           // 0x1C
        a.vop1(VOP1_MOV_B32, 1, ZERO + 2); // 0x20: the trampoline
        a.sopp(SOPP_BRANCH, 1);            // 0x24 -> 0x2C
        a.sopp(SOPP_NOP, 0);               // 0x28
        a.end();                           // 0x2C
        a.put(0xFFFFFFFFu);                // data, never decoded
        CHECK(translate(a, info));
        // Blocks: 0x00, 0x04 (the long branch), 0x18, 0x20 (the trampoline), 0x28, 0x2C.
        CHECK(hlsl_has("case 1: {"));
        CHECK(hlsl_has("pc = 3u;"));
        CHECK(hlsl_has("case 3: {"));
        CHECK(hlsl_has("pc = 5u;"));
        CHECK(!hlsl_has("case 4: {")); // the padding
    }
    {
        // A trampoline jumping back into the program with s_sub_u32 / s_subb_u32, data right after it.
        Asm a = {};
        a.sopp(SOPP_CBRANCH_EXECNZ, 5);    // 0x00 -> 0x18
        long_branch(a, 20, 0x18);          // 0x04..0x14 -> 0x20
        a.vop1(VOP1_MOV_B32, 1, ZERO + 1); // 0x18
        a.end();                           // 0x1C
        a.vop1(VOP1_MOV_B32, 1, ZERO + 2); // 0x20
        long_branch(a, 22, -0x10);         // 0x24..0x34 -> 0x18
        a.put(0xFFFFFFFFu);                // 0x38: data
        a.put(0xFFFFFFFFu);
        CHECK(translate(a, info));
        CHECK(hlsl_has("case 3: {"));
        CHECK(hlsl_has("pc = 2u;"));
    }
    {
        // A jump to an address loaded from memory isn't a long branch.
        Asm a = {};
        a.sop1(SOP1_MOV_B32, 2, 0);
        a.sop1(SOP1_MOV_B32, 3, ZERO);
        a.smem(SMEM_LOAD_DWORDX2, 20, 2, 0);
        a.sop1(SOP1_SETPC_B64, 0, 20);
        a.end();
        CHECK(!translate(a, info));
        CHECK(error_has("s_setpc_b64 at 0x10 to a computed address"));
    }
    {
        // A path joining in the middle of the sequence with other register values: the jump goes
        // somewhere else on that path, so the registers must hold the target, not only the code.
        Asm a = {};
        a.sop1(SOP1_MOV_B32, 20, ZERO + 64); // 0x00
        a.sop1(SOP1_MOV_B32, 21, ZERO);      // 0x04
        a.sopc(SOPC_CMP_EQ_U32, 0, ZERO);    // 0x08
        a.sopp(SOPP_CBRANCH_SCC0, 1);        // 0x0C -> 0x14
        long_branch(a, 20, 0x10);            // 0x10..0x20 -> 0x24
        a.end();                             // 0x24
        CHECK(!translate(a, info));
        CHECK(error_has("registers don't hold its target"));
    }
}

// --- image atomics -------------------------------------------------------------------------------------

static void test_image_atomics() {
    ShaderInfo info = compute_info();
    {
        // 32-bit, no old value wanted.
        Asm a = {};
        load_descriptors(a, false);
        a.mimg_full(MIMG_ATOMIC_UMAX, 4, 0, 8, 0, 0x1, 1, false);
        a.end();
        CHECK(translate(a, info));
        CHECK(hlsl_has("{ RWTexture2D<uint> tex_ = ResourceDescriptorHeap[s8 >> 5];"));
        CHECK(hlsl_has("uint2 at_ = uint2(v0, v1);"));
        CHECK(hlsl_has("if (EXEC) InterlockedMax(tex_[at_], v4);"));
        CHECK(!hlsl_has("if (EXEC) v4 = "));
        CHECK(!g_out.uses_int64_atomics);
    }
    {
        // 64-bit with the old value back (two registers each).
        Asm a = {};
        load_descriptors(a, false);
        a.mimg_full(MIMG_ATOMIC_UMAX, 4, 0, 8, 0, 0x3, 1, true);
        a.end();
        CHECK(translate(a, info));
        CHECK(hlsl_has("RWTexture2D<uint64_t> tex_"));
        CHECK(hlsl_has("if (EXEC) InterlockedMax(tex_[at_], ((uint64_t)v5 << 32 | v4), o_);"));
        CHECK(hlsl_has("if (EXEC) v4 = (uint)o_;"));
        CHECK(hlsl_has("if (EXEC) v5 = (uint)((uint64_t)o_ >> 32);"));
        CHECK(g_out.uses_int64_atomics);
    }
    {
        // Compare-swap: the value, then what the texel is compared with.
        Asm a = {};
        load_descriptors(a, false);
        a.mimg_full(MIMG_ATOMIC_CMPSWAP, 4, 0, 8, 0, 0x3, 1, true);
        a.mimg_full(MIMG_ATOMIC_CMPSWAP, 8, 0, 8, 0, 0xF, 1, false);
        a.end();
        CHECK(translate(a, info));
        CHECK(hlsl_has("if (EXEC) InterlockedCompareExchange(tex_[at_], v5, v4, o_);"));
        CHECK(hlsl_has("if (EXEC) v4 = (uint)o_;"));
        CHECK(!hlsl_has("if (EXEC) v5 = "));
        CHECK(hlsl_has("if (EXEC) InterlockedCompareStore(tex_[at_], ((uint64_t)v11 << 32 | v10), "
                       "((uint64_t)v9 << 32 | v8));"));
    }
    {
        // Signed minimum through a view of signed texels; subtraction as an add of the negation.
        Asm a = {};
        load_descriptors(a, false);
        a.mimg_full(MIMG_ATOMIC_SMIN, 4, 0, 8, 0, 0x1, 1, false);
        a.mimg_full(MIMG_ATOMIC_SUB, 5, 0, 8, 0, 0x1, 1, false);
        a.end();
        CHECK(translate(a, info));
        CHECK(hlsl_has("RWTexture2D<int> tex_"));
        CHECK(hlsl_has("InterlockedMin(tex_[at_], (int)v4);"));
        CHECK(hlsl_has("InterlockedAdd(tex_[at_], (uint)0 - v5);"));
    }
    {
        // Increment: a compare-exchange loop starting from a guess.
        Asm a = {};
        load_descriptors(a, false);
        a.mimg_full(MIMG_ATOMIC_INC, 4, 0, 8, 0, 0x1, 1, true);
        a.end();
        CHECK(translate(a, info));
        CHECK(hlsl_has("InterlockedCompareExchange(tex_[at_], c_, c_ >= v4 ? 0u : c_ + 1u, p_);"));
        CHECK(hlsl_has("if (EXEC) v4 = (uint)o_;"));
    }
    {
        // Three dwords are no texel width.
        Asm a = {};
        load_descriptors(a, false);
        a.mimg_full(MIMG_ATOMIC_ADD, 4, 0, 8, 0, 0x7, 1, false);
        a.end();
        CHECK(!translate(a, info));
        CHECK(error_has("with dmask 0x7"));
    }
}

// --- gathers from a picked mip -------------------------------------------------------------------------

static void test_gathers_at_a_mip() {
    ShaderInfo info = compute_info();
    // Border addressing, linear filters and mips, LOD bias +1, LOD 0.5..4.
    const u32 biased[4] = {0x1B6, 0x00400080, 0x08500100, 0};
    {
        // An explicit LOD through a constant sampler: its bias and clamps pick the mip here, the taps go
        // through a point-filtered copy without them.
        Asm a = {};
        load_descriptors(a, false);
        constant_sampler(a, 20, biased);
        a.mimg_full(MIMG_GATHER4_L, 4, 0, 8, 20, 0x2, 1, false);
        a.end();
        CHECK(translate(a, info));
        CHECK(g_out.sampler_count == 1);
        const InlineSampler& s = g_out.samplers[0];
        CHECK(s.filter == 0 && s.max_anisotropy == 1 && s.mip_bias == 0.0f && s.min_lod == 0.0f);
        CHECK(s.max_lod > 1e38f && s.address[0] == 4 && s.address[1] == 4 && !s.unnormalized);
        CHECK(hlsl_has("float l_ = clamp(asfloat(v2) + 1.0f, 0.5f, 4.0f);"));
        CHECK(hlsl_has("tex_.GetDimensions(m_, w_, h_, n_);"));
        CHECK(hlsl_has(
            "float2 b_ = floor(float2(asfloat(v0), asfloat(v1)) * sz_ - 0.5f) + float2(0.5f, 0.5f);"));
        CHECK(hlsl_has("float4 t0_ = tex_.SampleLevel(smp_, ((b_ + float2(0, 1)) / sz_), (float)m_);"));
        CHECK(hlsl_has("float4 t3_ = tex_.SampleLevel(smp_, ((b_ + float2(0, 0)) / sz_), (float)m_);"));
        CHECK(hlsl_has("float4 r_ = float4(t0_.y, t1_.y, t2_.y, t3_.y);"));
        CHECK(!hlsl_has("Gather"));
    }
    {
        // Offsets in the whole console range, from a heap sampler, in an array.
        Asm a = {};
        load_descriptors(a, true);
        a.vop1(VOP1_MOV_B32, 0, LIT);
        a.put(0x2C | 0x05 << 8); // x -20, y +5
        a.mimg_full(MIMG_GATHER4_L_O, 4, 0, 8, 16, 0x1, 5, false);
        a.end();
        CHECK(translate(a, info));
        CHECK(hlsl_has("SamplerState smp_ = SamplerDescriptorHeap[s16 >> 4];"));
        CHECK(hlsl_has("float l_ = asfloat(v4);"));
        CHECK(hlsl_has("tex_.GetDimensions(0u, w_, h_, e_, n_);"));
        CHECK(hlsl_has("+ float2(-19.5f, 5.5f);"));
        CHECK(hlsl_has("tex_.SampleLevel(smp_, float3((b_ + float2(1, 1)) / sz_, asfloat(v3)), (float)m_);"));
    }
    {
        // An implicit LOD comes from the coordinates' derivatives, plus the bias.
        Asm a = {};
        load_descriptors(a, true);
        a.mimg_full(MIMG_GATHER4_B, 4, 0, 8, 16, 0x1, 1, false);
        a.end();
        CHECK(translate(a, info));
        CHECK(hlsl_has("float l_ = tex_.CalculateLevelOfDetailUnclamped(smp_, float2(asfloat(v1), "
                       "asfloat(v2))) + asfloat(v0);"));
    }
    {
        // Gathers from the first mip stay desktop gathers.
        Asm a = {};
        load_descriptors(a, true);
        a.mimg_full(MIMG_GATHER4_LZ, 4, 0, 8, 16, 0x1, 1, false);
        a.end();
        CHECK(translate(a, info));
        CHECK(hlsl_has("tex_.GatherRed(smp_, float2(asfloat(v0), asfloat(v1)))"));
    }
    {
        // Comparing at a picked mip has no desktop form here yet.
        Asm a = {};
        load_descriptors(a, true);
        a.mimg_full(MIMG_GATHER4_C_L, 4, 0, 8, 16, 0x1, 1, false);
        a.end();
        CHECK(!translate(a, info));
        CHECK(error_has("comparing gather"));
    }
}

// --- descriptors on several paths ----------------------------------------------------------------------

// Every table indexes the same desktop heap, so where descriptors from different tables meet, they are
// still descriptors of the heap; what can't be placed says where its tag went.
static void test_descriptors_on_paths() {
    ShaderInfo info = compute_info();
    info.parameters[1].kind = USER_DATA_RESOURCE_TABLE; // a second resource table in s1
    info.parameters[1].range_count = 1;
    info.parameters[1].ranges[0] = {RANGE_SRV, 0, ~0u};
    {
        // The table address comes from either table; the descriptor is loaded after the join.
        Asm a = {};
        a.sop1(SOP1_MOV_B32, 2, 0); // 0x00
        a.sop1(SOP1_MOV_B32, 3, ZERO);
        a.sopc(SOPC_CMP_EQ_U32, 4, ZERO);
        a.sopp(SOPP_CBRANCH_SCC0, 1); // 0x0C -> 0x14
        a.sop1(SOP1_MOV_B32, 2, 1);   // 0x10
        a.smem(SMEM_LOAD_DWORDX8, 8, 2, 64);
        a.mimg(MIMG_LOAD, 4, 0, 8, 0, 0x1);
        a.end();
        CHECK(translate(a, info));
        CHECK(hlsl_has("Texture2D<float4> tex_ = ResourceDescriptorHeap[s8 >> 5];"));
        CHECK(hlsl_has("if (g_descriptor_info.Load((s8 & ~31u) + 16u) == 2u) {"));
    }
    {
        // A table address and a sampler table's are of different heaps: nothing to place.
        ShaderInfo mixed = compute_info();
        Asm a = {};
        a.sop1(SOP1_MOV_B32, 2, 0);
        a.sop1(SOP1_MOV_B32, 3, ZERO);
        a.sopc(SOPC_CMP_EQ_U32, 4, ZERO);
        a.sopp(SOPP_CBRANCH_SCC0, 1);
        a.sop1(SOP1_MOV_B32, 2, 1);
        a.smem(SMEM_LOAD_DWORDX8, 8, 2, 64); // 0x14
        a.mimg(MIMG_LOAD, 4, 0, 8, 0, 0x1);
        a.end();
        CHECK(!translate(a, mixed));
        CHECK(error_has(
            "s_load_dwordx8 at 0x14 from an address the translator cannot place (joining at 0x14: a "
            "table address at 0xc, a table address at 0x10)"));
    }
    {
        // s_cselect picking one of two descriptors keeps their heap addresses as they are.
        Asm a = {};
        a.sop1(SOP1_MOV_B32, 2, 0);
        a.sop1(SOP1_MOV_B32, 3, ZERO);
        a.smem(SMEM_LOAD_DWORDX8, 8, 2, 0);
        a.smem(SMEM_LOAD_DWORDX8, 16, 2, 32);
        a.sopc(SOPC_CMP_EQ_U32, 4, ZERO);
        for (u32 i = 0; i < 8; i += 2)
            a.sop2(SOP2_CSELECT_B64, 24 + i, 8 + i, 16 + i);
        a.mimg(MIMG_LOAD, 4, 0, 24, 0, 0x1);
        a.end();
        CHECK(translate(a, info));
        CHECK(hlsl_has("uint r0_ = scc ? s8 : s16;"));
        CHECK(hlsl_has("ResourceDescriptorHeap[s24 >> 5]"));
    }
    {
        // Between a buffer descriptor's dword and a constant it selects a value: the dword's.
        Asm a = {};
        load_buffer_descriptor(a, 0);
        a.sopc(SOPC_CMP_EQ_U32, 4, ZERO);
        a.sop2(SOP2_CSELECT_B32, 20, 10, ZERO);
        a.sop2(SOP2_ADD_U32, 21, 20, ZERO + 1);
        a.end();
        CHECK(translate(a, info));
        CHECK(hlsl_has("uint r0_ = scc ? g_descriptor_info.Load(s10) : 0x00000000u;"));
    }
    {
        // An image descriptor selected against constants reads its dwords as values, which it can't.
        Asm a = {};
        load_descriptors(a, false);
        a.sopc(SOPC_CMP_EQ_U32, 4, ZERO);
        for (u32 i = 0; i < 8; i += 2)
            a.sop2(SOP2_CSELECT_B64, 24 + i, 8 + i, ZERO);
        a.mimg(MIMG_LOAD, 4, 0, 24, 0, 0x1);
        a.end();
        CHECK(!translate(a, info));
        CHECK(error_has("image descriptor dword read as a value at 0x14"));
    }
    {
        // A descriptor on one path only says where it went.
        Asm a = {};
        load_descriptors(a, false);         // 0x00..0x08
        a.sopc(SOPC_CMP_EQ_U32, 4, ZERO);   // 0x10
        a.sopp(SOPP_CBRANCH_SCC0, 1);       // 0x14 -> 0x1C
        a.sop1(SOP1_MOV_B32, 8, ZERO);      // 0x18
        a.mimg(MIMG_LOAD, 4, 0, 8, 0, 0x1); // 0x1C
        a.end();
        CHECK(!translate(a, info));
        CHECK(error_has(
            "with an image descriptor changed after loading (joining at 0x1c: a descriptor at 0x14, "
            "a value at 0x18)"));
    }
    {
        // A compare that names its register as the destination doesn't write it.
        Asm a = {};
        a.sop1(SOP1_MOV_B32, 8, ZERO);      // 0x00
        a.sopk(SOPK_CMPK_LG_U32, 8, 0);     // 0x04
        a.mimg(MIMG_LOAD, 4, 0, 8, 0, 0x1); // 0x08
        a.end();
        CHECK(!translate(a, info));
        CHECK(error_has("not from a descriptor table (written at 0x0 by s_mov_b32)"));
    }
    {
        // The first block is entered at the start and again by a loop.
        Asm a = {};
        a.mimg(MIMG_LOAD, 4, 0, 8, 0, 0x1); // 0x00
        a.sop1(SOP1_MOV_B32, 8, ZERO);      // 0x08
        a.sopp(SOPP_CBRANCH_SCC0, 0xFFFC);  // 0x0C -> 0x00
        a.end();
        CHECK(!translate(a, info));
        CHECK(error_has("(joining at 0x0: the program's start, a value at 0xc)"));
    }
}

// --- descriptor shadow records -------------------------------------------------------------------------

// What the runtime writes into the shadow for a slot, read back the way translated programs read it.
static void test_descriptor_shadow_records() {
    u8 slot[32];
    u32 record[8];
    {
        // A raw view of 0x1234 bytes at 0x2_8000_0100: no stride, records count bytes.
        u32 bits[4];
        shadow_buffer_descriptor(0x280000100ull, 0, 0x1234, bits);
        u64 marker[2] = {DESCRIPTOR_MARKER_MAGIC | (u64)(2 | SHADOW_VIEW_UAV << 8) << 32, 0x1000};
        memcpy(slot, marker, 16);
        memcpy(slot + DESCRIPTOR_MARKER_BITS, bits, 16);
        shadow_record(slot, record);
        u64 base = record[0] | (u64)(record[1] & 0xFFFF) << 32;
        CHECK(base == 0x280000100ull);
        CHECK(((record[1] >> 16) & 0x3FFF) == 0);
        CHECK(record[2] == 0x1234 && (record[2] & ~15u) != 0);
        CHECK(record[4] == SHADOW_VIEW_UAV);
        CHECK(record[5] == 0 && record[6] == 0 && record[7] == 0);
    }
    {
        // A structured view of 100 elements of 24 bytes, rewritten the way compilers make a raw
        // descriptor of it: records times the stride, the stride cleared, the base kept.
        u32 bits[4];
        shadow_buffer_descriptor(0x7C9600040ull, 24, 100, bits);
        u32 stride = (bits[1] >> 16) & 0x3FFF;
        CHECK(stride == 24);
        CHECK(bits[2] * (stride > 1 ? stride : 1) == 2400);
        CHECK((bits[1] & 0xC000FFFFu) == 0x7);
        CHECK(bits[0] == 0xC9600040u);
    }
    {
        // A constant buffer view the title wrote: the descriptor is its own record.
        u64 native[2] = {0x80B3FF00ull | NATIVE_CBV_STRIDE_BITS << 48, 64 | NATIVE_CBV_FORMAT_BITS << 32};
        memcpy(slot, native, 16);
        memset(slot + 16, 0xEE, 16);
        shadow_record(slot, record);
        CHECK(record[0] == 0x80B3FF00u && record[1] == 0x00100000u);
        CHECK(((record[1] >> 16) & 0x3FFF) == 16 && record[2] == 64);
        CHECK(record[4] == SHADOW_VIEW_CBV);
    }
    {
        // Memory never written as a descriptor: a null view's record.
        memset(slot, 0x5A, sizeof(slot));
        shadow_record(slot, record);
        bool zero = true;
        for (u32 i = 0; i < 8; i++)
            zero &= record[i] == 0;
        CHECK(zero);
    }
}

void test_shader_translate() {
    rdna_init();
    test_constant_sampler_in_vcc();
    test_constant_sampler_bfm64_border();
    test_stale_vcc_constants();
    test_sampler_decoding();
    test_swapped_heap_sampler();
    test_changed_descriptors();
    test_view_kinds();
    test_descriptor_values();
    test_long_branches();
    test_image_atomics();
    test_gathers_at_a_mip();
    test_descriptors_on_paths();
    test_descriptor_shadow_records();
    g_out.hlsl.release();
}
