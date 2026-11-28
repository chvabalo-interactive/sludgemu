// Shader translation on the GPU: console wave64 compute programs (assembled from the RDNA 2 source in
// the comments) are translated, compiled and run on the desktop device, and what they store is checked
// against what the console computes. Covers the lane mapping of wave64 programs onto desktop waves
// (see shader/translate.h): cross-lane reads and writes across the halves, whole-mask queries, masks
// written as data, DPP row masks, LDS append and atomics, 16-bit three-operand minimum, maximum and
// median, console wave numbering in groups of several waves. And
// control flow as the translator structures it: EXEC skip-overs, if/else on EXEC, scalar loops,
// waterfall loops, loops lanes leave one by one, jumps out of several loops, and the dispatcher that
// takes the graphs it can't structure; random graphs of scalar jumps against a walk on the CPU. And the
// lanes outside EXEC: which writes keep them (a later read sees them) and which write every lane.
// Skipped without a D3D12 device that runs waves of 32 lanes.

#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include <d3d12.h>
#include <ole2.h> // dxcapi.h needs COM types the lean Windows headers leave out
#include <dxcapi.h>

#include "shader/rdna.h"
#include "shader/translate.h"
#include "test.h"

// Program A: one console wave of 64 lanes. Stores 16 regions of 64 dwords (one per lane, at lane * 4
// plus 256 * region) through a root UAV whose address is in s[0:1].
static const u32 k_lanes_program[] = {
    0xbe8203c1,             // s_mov_b32 s2, -1
    0xbe830380,             // s_mov_b32 s3, 0
    0xd7650001, 0x020100c1, // v_mbcnt_lo_u32_b32 v1, -1, 0
    0xd7660001, 0x020202c1, // v_mbcnt_hi_u32_b32 v1, -1, v1
    0x34040282,             // v_lshlrev_b32_e32 v2, 2, v1
    0x16060283,             // v_mul_u32_u24_e32 v3, 3, v1
    0xe0701000, 0x80000102, // buffer_store_dword v1, v2, s[0:3], 0 offen
    0xd7600004, 0x02017f03, // v_readlane_b32 s4, v3, 63
    0xd7600005, 0x02014103, // v_readlane_b32 s5, v3, 32
    0x7e080204,             // v_mov_b32_e32 v4, s4
    0xe0701100, 0x80000402, // buffer_store_dword v4, v2, s[0:3], 0 offen offset:256
    0x7e080205,             // v_mov_b32_e32 v4, s5
    0xe0701200, 0x80000402, // buffer_store_dword v4, v2, s[0:3], 0 offen offset:512
    0xbe880481,             // s_mov_b64 s[8:9], 1
    0xbe8a2408,             // s_and_saveexec_b64 s[10:11], s[8:9]
    0xd7600007, 0x02017f03, // v_readlane_b32 s7, v3, 63
    0xbefe040a,             // s_mov_b64 exec, s[10:11]
    0x7e080207,             // v_mov_b32_e32 v4, s7
    0xe0701300, 0x80000402, // buffer_store_dword v4, v2, s[0:3], 0 offen offset:768
    0x7d8802a8,             // v_cmp_gt_u32_e32 vcc, 40, v1
    0xbe8c106a,             // s_bcnt1_i32_b64 s12, vcc
    0x7d8202a4,             // v_cmp_lt_u32_e32 vcc, 36, v1
    0xbe8d146a,             // s_ff1_i32_b64 s13, vcc
    0x7e08020c,             // v_mov_b32_e32 v4, s12
    0xe0701400, 0x80000402, // buffer_store_dword v4, v2, s[0:3], 0 offen offset:1024
    0x7e08020d,             // v_mov_b32_e32 v4, s13
    0xe0701500, 0x80000402, // buffer_store_dword v4, v2, s[0:3], 0 offen offset:1280
    0x4a0802ff, 0x000003e8, // v_add_nc_u32_e32 v4, 0x3e8, v1
    0x92fe80a0,             // s_bfm_b64 exec, 32, 0
    0xe0701600, 0x80000402, // buffer_store_dword v4, v2, s[0:3], 0 offen offset:1536
    0xbefe04c1,             // s_mov_b64 exec, -1
    0xbefe0391,             // s_mov_b32 exec_lo, 17
    0xbeff0391,             // s_mov_b32 exec_hi, 17
    0xe0701700, 0x80000102, // buffer_store_dword v1, v2, s[0:3], 0 offen offset:1792
    0xbefe04c1,             // s_mov_b64 exec, -1
    0x7e0a0280,             // v_mov_b32_e32 v5, 0
    0xbefe0480,             // s_mov_b64 exec, 0
    0xd7610005, 0x02015004, // v_writelane_b32 v5, s4, 40
    0xbefe04c1,             // s_mov_b64 exec, -1
    0xe0701800, 0x80000502, // buffer_store_dword v5, v2, s[0:3], 0 offen offset:2048
    0x92fea188,             // s_bfm_b64 exec, 8, 33
    0x7e1c0503,             // v_readfirstlane_b32 s14, v3
    0xbefe04c1,             // s_mov_b64 exec, -1
    0x7e08020e,             // v_mov_b32_e32 v4, s14
    0xe0701900, 0x80000402, // buffer_store_dword v4, v2, s[0:3], 0 offen offset:2304
    0x7e0c0301,             // v_mov_b32_e32 v6, v1
    0x4a0c02fa, 0xcf011101, // v_add_nc_u32_dpp v6, v1, v1 row_shr:1 row_mask:0xc bank_mask:0xf
    0xe0701a00, 0x80000602, // buffer_store_dword v6, v2, s[0:3], 0 offen offset:2560
    0x4c1002bf,             // v_sub_nc_u32_e32 v8, 63, v1
    0x34101082,             // v_lshlrev_b32_e32 v8, 2, v8
    0xdacc0000, 0x09000308, // ds_bpermute_b32 v9, v8, v3
    0xe0701b00, 0x80000902, // buffer_store_dword v9, v2, s[0:3], 0 offen offset:2816
    0xbefe03ff, 0x0000ffff, // s_mov_b32 exec_lo, 0xffff
    0xbeff03ff, 0x0000ff00, // s_mov_b32 exec_hi, 0xff00
    0xd765000a, 0x0201007e, // v_mbcnt_lo_u32_b32 v10, exec_lo, 0
    0xd766000a, 0x0202147f, // v_mbcnt_hi_u32_b32 v10, exec_hi, v10
    0xe0701c00, 0x80000a02, // buffer_store_dword v10, v2, s[0:3], 0 offen offset:3072
    0xbefe04c1,             // s_mov_b64 exec, -1
    0xb06a0060,             // s_movk_i32 vcc_lo, 0x60
    0xbeeb0387,             // s_mov_b32 vcc_hi, 7
    0x800f6b6a,             // s_add_u32 s15, vcc_lo, vcc_hi
    0x7e08020f,             // v_mov_b32_e32 v4, s15
    0xe0701d00, 0x80000402, // buffer_store_dword v4, v2, s[0:3], 0 offen offset:3328
    0xbefc0380,             // s_mov_b32 m0, 0
    0x7e1602ff, 0x00000064, // v_mov_b32_e32 v11, 0x64
    0x7e1a0280,             // v_mov_b32_e32 v13, 0
    0xd8340000, 0x00000b0d, // ds_write_b32 v13, v11
    0xbefe03ff, 0x000003ff, // s_mov_b32 exec_lo, 0x3ff
    0xbeff03ff, 0x0ffc0000, // s_mov_b32 exec_hi, 0xffc0000
    0xd8f80000, 0x0c000000, // ds_append v12
    0xe0701e00, 0x80000c02, // buffer_store_dword v12, v2, s[0:3], 0 offen offset:3584
    0xbefe04c1,             // s_mov_b64 exec, -1
    0xb010002d,             // s_movk_i32 s16, 0x2d
    0xd7600011, 0x02002103, // v_readlane_b32 s17, v3, s16
    0x7e080211,             // v_mov_b32_e32 v4, s17
    0xe0701f00, 0x80000402, // buffer_store_dword v4, v2, s[0:3], 0 offen offset:3840
    0xbf810000,             // s_endpgm
};
// Program B: stores thread ID + 1000 * (lane 0's thread ID) at thread ID * 4. Run in groups that hold
// more than one console wave.
static const u32 k_waves_program[] = {
    0xbe8203c1,                         // s_mov_b32 s2, -1
    0xbe830380,                         // s_mov_b32 s3, 0
    0x34040082,                         // v_lshlrev_b32_e32 v2, 2, v0
    0xd7600004, 0x02010100,             // v_readlane_b32 s4, v0, 0
    0xd50b0003, 0x0201fe04, 0x000003e8, // v_mul_u32_u24_e64 v3, s4, 0x3e8
    0x4a060103,                         // v_add_nc_u32_e32 v3, v3, v0
    0xe0701000, 0x80000302,             // buffer_store_dword v3, v2, s[0:3], 0 offen
    0xbf810000,                         // s_endpgm
};

// Program C: one console wave of 64 lanes, regions as in program A. Masks read as data by scalar
// instructions that name them as their destination, data written to EXEC by a 64-bit shift (and its
// SCC), a uniform boolean changed by s_addk and used as a mask, a mask joined with data and kept by a
// conditional move.
static const u32 k_forms_program[] = {
    0xbe8203c1,             // s_mov_b32 s2, -1
    0xbe830380,             // s_mov_b32 s3, 0
    0xd7650001, 0x020100c1, // v_mbcnt_lo_u32_b32 v1, -1, 0
    0xd7660001, 0x020202c1, // v_mbcnt_hi_u32_b32 v1, -1, v1
    0x34040282,             // v_lshlrev_b32_e32 v2, 2, v1
    0xd4c4000a, 0x02020284, // v_cmp_gt_u32_e64 s[10:11], 4, v1
    0xb48a000f,             // s_cmpk_eq_u32 s10, 0xf
    0x850c8987,             // s_cselect_b32 s12, 7, 9
    0xd5010004, 0x00290280, // v_cndmask_b32_e64 v4, 0, 1, s[10:11]
    0xe0701000, 0x80000402, // buffer_store_dword v4, v2, s[0:3], 0 offen
    0x7e08020c,             // v_mov_b32_e32 v4, s12
    0xe0701100, 0x80000402, // buffer_store_dword v4, v2, s[0:3], 0 offen offset:256
    0xbf068080,             // s_cmp_eq_u32 0, 0
    0x859480c1,             // s_cselect_b64 s[20:21], -1, 0
    0xb7940006,             // s_addk_i32 s20, 0x6
    0xbe962414,             // s_and_saveexec_b64 s[22:23], s[20:21]
    0xe0701200, 0x80000102, // buffer_store_dword v1, v2, s[0:3], 0 offen offset:512
    0xbefe04c1,             // s_mov_b64 exec, -1
    0xbe9803a1,             // s_mov_b32 s24, 33
    0x8ffe1883,             // s_lshl_b64 exec, 3, s24
    0x85198d8b,             // s_cselect_b32 s25, 11, 13
    0xe0701300, 0x80000102, // buffer_store_dword v1, v2, s[0:3], 0 offen offset:768
    0xbefe04c1,             // s_mov_b64 exec, -1
    0x7e080219,             // v_mov_b32_e32 v4, s25
    0xe0701400, 0x80000402, // buffer_store_dword v4, v2, s[0:3], 0 offen offset:1024
    0xbe9e0381,             // s_mov_b32 s30, 1
    0xbf06801e,             // s_cmp_eq_u32 s30, 0
    0xbf850003,             // s_cbranch_scc1 3 (to the s_movk)
    0xd4c4001a, 0x02020284, // v_cmp_gt_u32_e64 s[26:27], 4, v1
    0xbf820002,             // s_branch 2 (past the s_movk)
    0xb01a0055,             // s_movk_i32 s26, 0x55
    0xb01b0000,             // s_movk_i32 s27, 0x0
    0xbf06821e,             // s_cmp_eq_u32 s30, 2
    0xbe9a051e,             // s_cmov_b32 s26, s30
    0x7e08021a,             // v_mov_b32_e32 v4, s26
    0xe0701500, 0x80000402, // buffer_store_dword v4, v2, s[0:3], 0 offen offset:1280
    0xbf810000,             // s_endpgm
};

// Program D: control flow, one console wave of 64 lanes, regions as in program A (see flow_expected).
static const u32 k_flow_program[] = {
    0xbe8203c1,             // 0x0: s_mov_b32 s2, -1
    0xbe830380,             // 0x4: s_mov_b32 s3, 0
    0xd7650001, 0x020100c1, // 0x8: v_mbcnt_lo_u32_b32 v1, -1, 0
    0xd7660001, 0x020202c1, // 0x10: v_mbcnt_hi_u32_b32 v1, -1, v1
    0x34040282,             // 0x18: v_lshlrev_b32 v2, 2, v1
    0xd7600017, 0x02010b01, // 0x1c: v_readlane_b32 s23, v1, 5
    0x7e080287,             // 0x24: v_mov_b32 v4, 7
    0x7d880294,             // 0x28: v_cmp_gt_u32 vcc, 20, v1
    0xbe8a246a,             // 0x2c: s_and_saveexec_b64 s[10:11], vcc
    0xbf880002,             // 0x30: s_cbranch_execz (to 0x3c)
    0x4a0802ff, 0x00000064, // 0x34: v_add_nc_u32 v4, 0x64, v1
    0x88fe0a7e,             // 0x3c: s_or_b64 exec, exec, s[10:11]
    0xe0701000, 0x80000402, // 0x40: buffer_store_dword v4, v2, s[0:3], 0 offen
    0xbe94038b,             // 0x48: s_mov_b32 s20, 11
    0x7d880280,             // 0x4c: v_cmp_gt_u32 vcc, 0, v1
    0xbe8a246a,             // 0x50: s_and_saveexec_b64 s[10:11], vcc
    0xbf880001,             // 0x54: s_cbranch_execz (to 0x5c)
    0xbe9403b7,             // 0x58: s_mov_b32 s20, 55
    0x88fe0a7e,             // 0x5c: s_or_b64 exec, exec, s[10:11]
    0x7e080214,             // 0x60: v_mov_b32 v4, s20
    0xe0701100, 0x80000402, // 0x64: buffer_store_dword v4, v2, s[0:3], 0 offen offset:256
    0x7e080280,             // 0x6c: v_mov_b32 v4, 0
    0x7d8802a8,             // 0x70: v_cmp_gt_u32 vcc, 40, v1
    0xbe8a246a,             // 0x74: s_and_saveexec_b64 s[10:11], vcc
    0x898a0a7e,             // 0x78: s_xor_b64 s[10:11], exec, s[10:11]
    0xbf880001,             // 0x7c: s_cbranch_execz (to 0x84)
    0x7e080281,             // 0x80: v_mov_b32 v4, 1
    0xbe8a250a,             // 0x84: s_or_saveexec_b64 s[10:11], s[10:11]
    0x89fe0a7e,             // 0x88: s_xor_b64 exec, exec, s[10:11]
    0xbf880001,             // 0x8c: s_cbranch_execz (to 0x94)
    0x7e080282,             // 0x90: v_mov_b32 v4, 2
    0x88fe0a7e,             // 0x94: s_or_b64 exec, exec, s[10:11]
    0xe0701200, 0x80000402, // 0x98: buffer_store_dword v4, v2, s[0:3], 0 offen offset:512
    0xbe940380,             // 0xa0: s_mov_b32 s20, 0
    0xbe950380,             // 0xa4: s_mov_b32 s21, 0
    0x80151415,             // 0xa8: s_add_u32 s21, s21, s20
    0x80148114,             // 0xac: s_add_u32 s20, s20, 1
    0xbf0a8a14,             // 0xb0: s_cmp_lt_u32 s20, 10
    0xbf85fffc,             // 0xb4: s_cbranch_scc1 (to 0xa8)
    0x7e080215,             // 0xb8: v_mov_b32 v4, s21
    0xe0701300, 0x80000402, // 0xbc: buffer_store_dword v4, v2, s[0:3], 0 offen offset:768
    0x360a0283,             // 0xc4: v_and_b32 v5, 3, v1
    0x7e080280,             // 0xc8: v_mov_b32 v4, 0
    0xbe960380,             // 0xcc: s_mov_b32 s22, 0
    0xbe8c047e,             // 0xd0: s_mov_b64 s[12:13], exec
    0x7e280505,             // 0xd4: v_readfirstlane_b32 s20, v5
    0x7d840a14,             // 0xd8: v_cmp_eq_u32 vcc, s20, v5
    0xbe8e246a,             // 0xdc: s_and_saveexec_b64 s[14:15], vcc
    0x93158a14,             // 0xe0: s_mul_i32 s21, s20, 10
    0xd5250004, 0x02010215, // 0xe4: v_add_nc_u32 v4, s21, 1
    0x80168116,             // 0xec: s_add_u32 s22, s22, 1
    0x89fe0e7e,             // 0xf0: s_xor_b64 exec, exec, s[14:15]
    0xbf89fff7,             // 0xf4: s_cbranch_execnz (to 0xd4)
    0xbefe040c,             // 0xf8: s_mov_b64 exec, s[12:13]
    0xe0701400, 0x80000402, // 0xfc: buffer_store_dword v4, v2, s[0:3], 0 offen offset:1024
    0x7e080216,             // 0x104: v_mov_b32 v4, s22
    0xe0701500, 0x80000402, // 0x108: buffer_store_dword v4, v2, s[0:3], 0 offen offset:1280
    0xbe940380,             // 0x110: s_mov_b32 s20, 0
    0xbe9803ff, 0x000003e8, // 0x114: s_mov_b32 s24, 0x3e8
    0x80148114,             // 0x11c: s_add_u32 s20, s20, 1
    0xbf061714,             // 0x120: s_cmp_eq_u32 s20, s23
    0xbf850004,             // 0x124: s_cbranch_scc1 (to 0x138)
    0xbf0aff14, 0x00000064, // 0x128: s_cmp_lt_u32 s20, 0x64
    0xbf85fffa,             // 0x130: s_cbranch_scc1 (to 0x11c)
    0xbe980382,             // 0x134: s_mov_b32 s24, 2
    0x80181418,             // 0x138: s_add_u32 s24, s24, s20
    0x7e080218,             // 0x13c: v_mov_b32 v4, s24
    0xe0701600, 0x80000402, // 0x140: buffer_store_dword v4, v2, s[0:3], 0 offen offset:1536
    0xbe940380,             // 0x148: s_mov_b32 s20, 0
    0xbe990380,             // 0x14c: s_mov_b32 s25, 0
    0xbf098414,             // 0x150: s_cmp_ge_u32 s20, 4
    0xbf850007,             // 0x154: s_cbranch_scc1 (to 0x174)
    0x80148114,             // 0x158: s_add_u32 s20, s20, 1
    0xbe950380,             // 0x15c: s_mov_b32 s21, 0
    0x80158115,             // 0x160: s_add_u32 s21, s21, 1
    0x80198119,             // 0x164: s_add_u32 s25, s25, 1
    0xbf061415,             // 0x168: s_cmp_eq_u32 s21, s20
    0xbf85fff8,             // 0x16c: s_cbranch_scc1 (to 0x150)
    0xbf82fffb,             // 0x170: s_branch (to 0x160)
    0x7e080219,             // 0x174: v_mov_b32 v4, s25
    0xe0701700, 0x80000402, // 0x178: buffer_store_dword v4, v2, s[0:3], 0 offen offset:1792
    0x7e0c0280,             // 0x180: v_mov_b32 v6, 0
    0xbe900480,             // 0x184: s_mov_b64 s[16:17], 0
    0x4a0c0c81,             // 0x188: v_add_nc_u32 v6, 1, v6
    0x7d8c0306,             // 0x18c: v_cmp_ge_u32 vcc, v6, v1
    0x8890106a,             // 0x190: s_or_b64 s[16:17], vcc, s[16:17]
    0x8afe107e,             // 0x194: s_andn2_b64 exec, exec, s[16:17]
    0xbf89fffb,             // 0x198: s_cbranch_execnz (to 0x188)
    0x88fe107e,             // 0x19c: s_or_b64 exec, exec, s[16:17]
    0xe0701800, 0x80000602, // 0x1a0: buffer_store_dword v6, v2, s[0:3], 0 offen offset:2048
    0xbf810000,             // 0x1a8: s_endpgm
};
// Program D for wave32: the same with 32-bit masks, its if/else splitting at lane 12.
static const u32 k_flow32_program[] = {
    0xbe8203c1,             // 0x0: s_mov_b32 s2, -1
    0xbe830380,             // 0x4: s_mov_b32 s3, 0
    0xd7650001, 0x020100c1, // 0x8: v_mbcnt_lo_u32_b32 v1, -1, 0
    0x34040282,             // 0x10: v_lshlrev_b32 v2, 2, v1
    0xd7600017, 0x02010b01, // 0x14: v_readlane_b32 s23, v1, 5
    0x7e080287,             // 0x1c: v_mov_b32 v4, 7
    0x7d880294,             // 0x20: v_cmp_gt_u32 vcc_lo, 20, v1
    0xbe8a3c6a,             // 0x24: s_and_saveexec_b32 s10, vcc_lo
    0xbf880002,             // 0x28: s_cbranch_execz (to 0x34)
    0x4a0802ff, 0x00000064, // 0x2c: v_add_nc_u32 v4, 0x64, v1
    0x887e0a7e,             // 0x34: s_or_b32 exec_lo, exec_lo, s10
    0xe0701000, 0x80000402, // 0x38: buffer_store_dword v4, v2, s[0:3], 0 offen
    0xbe94038b,             // 0x40: s_mov_b32 s20, 11
    0x7d880280,             // 0x44: v_cmp_gt_u32 vcc_lo, 0, v1
    0xbe8a3c6a,             // 0x48: s_and_saveexec_b32 s10, vcc_lo
    0xbf880001,             // 0x4c: s_cbranch_execz (to 0x54)
    0xbe9403b7,             // 0x50: s_mov_b32 s20, 55
    0x887e0a7e,             // 0x54: s_or_b32 exec_lo, exec_lo, s10
    0x7e080214,             // 0x58: v_mov_b32 v4, s20
    0xe0701100, 0x80000402, // 0x5c: buffer_store_dword v4, v2, s[0:3], 0 offen offset:256
    0x7e080280,             // 0x64: v_mov_b32 v4, 0
    0x7d88028c,             // 0x68: v_cmp_gt_u32 vcc_lo, 12, v1
    0xbe8a3c6a,             // 0x6c: s_and_saveexec_b32 s10, vcc_lo
    0x890a0a7e,             // 0x70: s_xor_b32 s10, exec_lo, s10
    0xbf880001,             // 0x74: s_cbranch_execz (to 0x7c)
    0x7e080281,             // 0x78: v_mov_b32 v4, 1
    0xbe8a3d0a,             // 0x7c: s_or_saveexec_b32 s10, s10
    0x897e0a7e,             // 0x80: s_xor_b32 exec_lo, exec_lo, s10
    0xbf880001,             // 0x84: s_cbranch_execz (to 0x8c)
    0x7e080282,             // 0x88: v_mov_b32 v4, 2
    0x887e0a7e,             // 0x8c: s_or_b32 exec_lo, exec_lo, s10
    0xe0701200, 0x80000402, // 0x90: buffer_store_dword v4, v2, s[0:3], 0 offen offset:512
    0xbe940380,             // 0x98: s_mov_b32 s20, 0
    0xbe950380,             // 0x9c: s_mov_b32 s21, 0
    0x80151415,             // 0xa0: s_add_u32 s21, s21, s20
    0x80148114,             // 0xa4: s_add_u32 s20, s20, 1
    0xbf0a8a14,             // 0xa8: s_cmp_lt_u32 s20, 10
    0xbf85fffc,             // 0xac: s_cbranch_scc1 (to 0xa0)
    0x7e080215,             // 0xb0: v_mov_b32 v4, s21
    0xe0701300, 0x80000402, // 0xb4: buffer_store_dword v4, v2, s[0:3], 0 offen offset:768
    0x360a0283,             // 0xbc: v_and_b32 v5, 3, v1
    0x7e080280,             // 0xc0: v_mov_b32 v4, 0
    0xbe960380,             // 0xc4: s_mov_b32 s22, 0
    0xbe8c037e,             // 0xc8: s_mov_b32 s12, exec_lo
    0x7e280505,             // 0xcc: v_readfirstlane_b32 s20, v5
    0x7d840a14,             // 0xd0: v_cmp_eq_u32 vcc_lo, s20, v5
    0xbe8e3c6a,             // 0xd4: s_and_saveexec_b32 s14, vcc_lo
    0x93158a14,             // 0xd8: s_mul_i32 s21, s20, 10
    0xd5250004, 0x02010215, // 0xdc: v_add_nc_u32 v4, s21, 1
    0x80168116,             // 0xe4: s_add_u32 s22, s22, 1
    0x897e0e7e,             // 0xe8: s_xor_b32 exec_lo, exec_lo, s14
    0xbf89fff7,             // 0xec: s_cbranch_execnz (to 0xcc)
    0xbefe030c,             // 0xf0: s_mov_b32 exec_lo, s12
    0xe0701400, 0x80000402, // 0xf4: buffer_store_dword v4, v2, s[0:3], 0 offen offset:1024
    0x7e080216,             // 0xfc: v_mov_b32 v4, s22
    0xe0701500, 0x80000402, // 0x100: buffer_store_dword v4, v2, s[0:3], 0 offen offset:1280
    0xbe940380,             // 0x108: s_mov_b32 s20, 0
    0xbe9803ff, 0x000003e8, // 0x10c: s_mov_b32 s24, 0x3e8
    0x80148114,             // 0x114: s_add_u32 s20, s20, 1
    0xbf061714,             // 0x118: s_cmp_eq_u32 s20, s23
    0xbf850004,             // 0x11c: s_cbranch_scc1 (to 0x130)
    0xbf0aff14, 0x00000064, // 0x120: s_cmp_lt_u32 s20, 0x64
    0xbf85fffa,             // 0x128: s_cbranch_scc1 (to 0x114)
    0xbe980382,             // 0x12c: s_mov_b32 s24, 2
    0x80181418,             // 0x130: s_add_u32 s24, s24, s20
    0x7e080218,             // 0x134: v_mov_b32 v4, s24
    0xe0701600, 0x80000402, // 0x138: buffer_store_dword v4, v2, s[0:3], 0 offen offset:1536
    0xbe940380,             // 0x140: s_mov_b32 s20, 0
    0xbe990380,             // 0x144: s_mov_b32 s25, 0
    0xbf098414,             // 0x148: s_cmp_ge_u32 s20, 4
    0xbf850007,             // 0x14c: s_cbranch_scc1 (to 0x16c)
    0x80148114,             // 0x150: s_add_u32 s20, s20, 1
    0xbe950380,             // 0x154: s_mov_b32 s21, 0
    0x80158115,             // 0x158: s_add_u32 s21, s21, 1
    0x80198119,             // 0x15c: s_add_u32 s25, s25, 1
    0xbf061415,             // 0x160: s_cmp_eq_u32 s21, s20
    0xbf85fff8,             // 0x164: s_cbranch_scc1 (to 0x148)
    0xbf82fffb,             // 0x168: s_branch (to 0x158)
    0x7e080219,             // 0x16c: v_mov_b32 v4, s25
    0xe0701700, 0x80000402, // 0x170: buffer_store_dword v4, v2, s[0:3], 0 offen offset:1792
    0x7e0c0280,             // 0x178: v_mov_b32 v6, 0
    0xbe900380,             // 0x17c: s_mov_b32 s16, 0
    0x4a0c0c81,             // 0x180: v_add_nc_u32 v6, 1, v6
    0x7d8c0306,             // 0x184: v_cmp_ge_u32 vcc_lo, v6, v1
    0x8810106a,             // 0x188: s_or_b32 s16, vcc_lo, s16
    0x8a7e107e,             // 0x18c: s_andn2_b32 exec_lo, exec_lo, s16
    0xbf89fffb,             // 0x190: s_cbranch_execnz (to 0x180)
    0x887e107e,             // 0x194: s_or_b32 exec_lo, exec_lo, s16
    0xe0701800, 0x80000602, // 0x198: buffer_store_dword v6, v2, s[0:3], 0 offen offset:2048
    0xbf810000,             // 0x1a0: s_endpgm
};
// Program E: a loop entered at two blocks, which only the dispatcher takes.
static const u32 k_irreducible_program[] = {
    0xbe8203c1,             // 0x0: s_mov_b32 s2, -1
    0xbe830380,             // 0x4: s_mov_b32 s3, 0
    0xd7650001, 0x020100c1, // 0x8: v_mbcnt_lo_u32_b32 v1, -1, 0
    0xd7660001, 0x020202c1, // 0x10: v_mbcnt_hi_u32_b32 v1, -1, v1
    0x34040282,             // 0x18: v_lshlrev_b32 v2, 2, v1
    0xd7600017, 0x02010b01, // 0x1c: v_readlane_b32 s23, v1, 5
    0xbe9a0380,             // 0x24: s_mov_b32 s26, 0
    0xbf068517,             // 0x28: s_cmp_eq_u32 s23, 5
    0xbf850001,             // 0x2c: s_cbranch_scc1 (to 0x34)
    0x801a8a1a,             // 0x30: s_add_u32 s26, s26, 10
    0x801a811a,             // 0x34: s_add_u32 s26, s26, 1
    0xbf0a9e1a,             // 0x38: s_cmp_lt_u32 s26, 30
    0xbf85fffc,             // 0x3c: s_cbranch_scc1 (to 0x30)
    0x7e08021a,             // 0x40: v_mov_b32 v4, s26
    0xe0701000, 0x80000402, // 0x44: buffer_store_dword v4, v2, s[0:3], 0 offen
    0xbf810000,             // 0x4c: s_endpgm
};

// Program G: writes under EXEC that skip lanes later reads see, and ones whose skipped lanes nothing reads
// (temporaries, translated to write every lane; see shader/translate_lanes.cpp). Regions as in program A.
static const u32 k_lane_writes_program[] = {
    0xbe8203c1,             // 0x0: s_mov_b32 s2, -1
    0xbe830380,             // 0x4: s_mov_b32 s3, 0
    0xd7650001, 0x020100c1, // 0x8: v_mbcnt_lo_u32_b32 v1, -1, 0
    0xd7660001, 0x020202c1, // 0x10: v_mbcnt_hi_u32_b32 v1, -1, v1
    0x34040282,             // 0x18: v_lshlrev_b32 v2, 2, v1
    0x4a0802ff, 0x00000064, // 0x1c: v_add_nc_u32 v4, 0x64, v1
    0x7d880294,             // 0x24: v_cmp_gt_u32 vcc, 20, v1
    0xbe8a246a,             // 0x28: s_and_saveexec_b64 s[10:11], vcc
    0x7e080287,             // 0x2c: v_mov_b32 v4, 7 (the else lanes read the old value)
    0x7e0a0283,             // 0x30: v_mov_b32 v5, 3 (temporaries)
    0x4a0c0b05,             // 0x34: v_add_nc_u32 v6, v5, v5
    0x4a080d04,             // 0x38: v_add_nc_u32 v4, v4, v6
    0x8afe7e0a,             // 0x3c: s_andn2_b64 exec, s[10:11], exec
    0x4a080881,             // 0x40: v_add_nc_u32 v4, 1, v4
    0xbefe040a,             // 0x44: s_mov_b64 exec, s[10:11]
    0xe0701000, 0x80000402, // 0x48: buffer_store_dword v4, v2, s[0:3], 0 offen
    0x340e0281,             // 0x50: v_lshlrev_b32 v7, 1, v1
    0xbefe0481,             // 0x54: s_mov_b64 exec, 1
    0x7e0e02b7,             // 0x58: v_mov_b32 v7, 55 (lane 1 is read below)
    0xbefe04c1,             // 0x5c: s_mov_b64 exec, -1
    0xd7600005, 0x02010307, // 0x60: v_readlane_b32 s5, v7, 1
    0xd7600006, 0x02010107, // 0x68: v_readlane_b32 s6, v7, 0
    0x80050605,             // 0x70: s_add_u32 s5, s5, s6
    0x7e100205,             // 0x74: v_mov_b32 v8, s5
    0xe0701100, 0x80000802, // 0x78: buffer_store_dword v8, v2, s[0:3], 0 offen offset:256
    0x4a1202ff, 0x000001f4, // 0x80: v_add_nc_u32 v9, 0x1f4, v1
    0xbefc03c1,             // 0x88: s_mov_b32 m0, -1
    0x7e140280,             // 0x8c: v_mov_b32 v10, 0
    0x7e1602ff, 0x0000004d, // 0x90: v_mov_b32 v11, 0x4d
    0xd8340000, 0x00000b0a, // 0x98: ds_write_b32 v10, v11
    0x7d88028a,             // 0xa0: v_cmp_gt_u32 vcc, 10, v1
    0xbe8c246a,             // 0xa4: s_and_saveexec_b64 s[12:13], vcc
    0xd8d80000, 0x0900000a, // 0xa8: ds_read_b32 v9, v10 (the other lanes are read below)
    0x88fe0c7e,             // 0xb0: s_or_b64 exec, exec, s[12:13]
    0xe0701200, 0x80000902, // 0xb4: buffer_store_dword v9, v2, s[0:3], 0 offen offset:512
    0x7e180281,             // 0xbc: v_mov_b32 v12, 1
    0x7d8802a0,             // 0xc0: v_cmp_gt_u32 vcc, 32, v1
    0xbe8e246a,             // 0xc4: s_and_saveexec_b64 s[14:15], vcc
    0x7e180282,             // 0xc8: v_mov_b32 v12, 2 (whole-wave code reads lane 40)
    0xbe9025c1,             // 0xcc: s_or_saveexec_b64 s[16:17], -1
    0xd7600007, 0x0201510c, // 0xd0: v_readlane_b32 s7, v12, 40
    0xd7600008, 0x0201070c, // 0xd8: v_readlane_b32 s8, v12, 3
    0xbefe0410,             // 0xe0: s_mov_b64 exec, s[16:17]
    0x88fe0e7e,             // 0xe4: s_or_b64 exec, exec, s[14:15]
    0x8f078407,             // 0xe8: s_lshl_b32 s7, s7, 4
    0x80070807,             // 0xec: s_add_u32 s7, s7, s8
    0x7e1a0207,             // 0xf0: v_mov_b32 v13, s7
    0xe0701300, 0x80000d02, // 0xf4: buffer_store_dword v13, v2, s[0:3], 0 offen offset:768
    0x361c0283,             // 0xfc: v_and_b32 v14, 3, v1
    0x7e1e0280,             // 0x100: v_mov_b32 v15, 0
    0xbe92047e,             // 0x104: s_mov_b64 s[18:19], exec
    0xbf820001,             // 0x108: s_branch (to 0x110)
    0xbefe0416,             // 0x10c: s_mov_b64 exec, s[22:23]
    0x7e28050e,             // 0x110: v_readfirstlane_b32 s20, v14
    0x7d841c14,             // 0x114: v_cmp_eq_u32 vcc, s20, v14
    0xbe96246a,             // 0x118: s_and_saveexec_b64 s[22:23], vcc
    0x16200283,             // 0x11c: v_mul_u32_u24 v16, 3, v1 (a temporary)
    0x4a1e2014,             // 0x120: v_add_nc_u32 v15, s20, v16 (each pass writes some lanes)
    0x8a967e16,             // 0x124: s_andn2_b64 s[22:23], s[22:23], exec
    0xbf85fff8,             // 0x128: s_cbranch_scc1 (to 0x10c)
    0xbefe0412,             // 0x12c: s_mov_b64 exec, s[18:19]
    0xe0701400, 0x80000f02, // 0x130: buffer_store_dword v15, v2, s[0:3], 0 offen offset:1024
    0xbf810000,             // 0x138: s_endpgm
};

// Program I: one console wave of 64 lanes, regions as in program A. Three-operand 16-bit minimum,
// maximum and median (floats, signed and unsigned, a high half source and destination with op_sel) and
// LDS atomics: subtraction with and without the old value, compare-and-store with and without it
// (DATA0 compared, DATA1 stored).
static const u32 k_alu16_lds_program[] = {
    0xbe8203c1,                         // 0x0: s_mov_b32 s2, -1
    0xbe830380,                         // 0x4: s_mov_b32 s3, 0
    0xd7650001, 0x020100c1,             // 0x8: v_mbcnt_lo_u32_b32 v1, -1, 0
    0xd7660001, 0x020202c1,             // 0x10: v_mbcnt_hi_u32_b32 v1, -1, v1
    0x34040282,                         // 0x18: v_lshlrev_b32 v2, 2, v1
    0x7e06a101,                         // 0x1c: v_cvt_f16_u16 v3, v1
    0x360606ff, 0x0000ffff,             // 0x20: v_and_b32 v3, 0xffff, v3
    0x4c0802bf,                         // 0x28: v_sub_nc_u32 v4, 63, v1
    0x7e08a104,                         // 0x2c: v_cvt_f16_u16 v4, v4
    0x360808ff, 0x0000ffff,             // 0x30: v_and_b32 v4, 0xffff, v4
    0x7e0a02ff, 0x00005000,             // 0x38: v_mov_b32 v5, 0x5000 (32.0)
    0xd7570006, 0x04160903,             // 0x40: v_med3_f16 v6, v3, v4, v5
    0xe0701000, 0x80000602,             // 0x48: buffer_store_dword v6, v2, s[0:3], 0 offen
    0xd7510007, 0x04160903,             // 0x50: v_min3_f16 v7, v3, v4, v5
    0xe0701100, 0x80000702,             // 0x58: buffer_store_dword v7, v2, s[0:3], 0 offen offset:256
    0xd7540008, 0x04160903,             // 0x60: v_max3_f16 v8, v3, v4, v5
    0xe0701200, 0x80000802,             // 0x68: buffer_store_dword v8, v2, s[0:3], 0 offen offset:512
    0xd76f0009, 0x04152103,             // 0x70: v_lshl_or_b32 v9, v3, 16, v5
    0x7e1402ff, 0x00001234,             // 0x78: v_mov_b32 v10, 0x1234
    0xd757480a, 0x04160909,             // 0x80: v_med3_f16 v10, v9, v4, v5 op_sel:[1,0,0,1]
    0xe0701300, 0x80000a02,             // 0x88: buffer_store_dword v10, v2, s[0:3], 0 offen offset:768
    0x4e1602a0,                         // 0x90: v_subrev_nc_u32 v11, 32, v1
    0x7e1802ff, 0xabcd0000,             // 0x94: v_mov_b32 v12, 0xabcd0000
    0xd758000c, 0x021d8b0b,             // 0x9c: v_med3_i16 v12, v11, -5, 7
    0xe0701400, 0x80000c02,             // 0xa4: buffer_store_dword v12, v2, s[0:3], 0 offen offset:1024
    0x7e1a0280,                         // 0xac: v_mov_b32 v13, 0
    0xd753000d, 0x04115101,             // 0xb0: v_min3_u16 v13, v1, 40, v4
    0xe0701500, 0x80000d02,             // 0xb8: buffer_store_dword v13, v2, s[0:3], 0 offen offset:1280
    0x7e1c02ff, 0x77770000,             // 0xc0: v_mov_b32 v14, 0x77770000
    0xd755000e, 0x0215ff0b, 0xffffffec, // 0xc8: v_max3_i16 v14, v11, -20, 5
    0xe0701600, 0x80000e02,             // 0xd4: buffer_store_dword v14, v2, s[0:3], 0 offen offset:1536
    0xbefc03c1,                         // 0xdc: s_mov_b32 m0, -1
    0x4a1e02ff, 0x000003e8,             // 0xe0: v_add_nc_u32 v15, 0x3e8, v1
    0xd8340100, 0x00000f02,             // 0xe8: ds_write_b32 v2, v15 offset:256
    0x7e320280,                         // 0xf0: v_mov_b32 v25, 0
    0x7e3402ff, 0x00000064,             // 0xf4: v_mov_b32 v26, 0x64
    0xd8340000, 0x00001a19,             // 0xfc: ds_write_b32 v25, v26
    0x36280281,                         // 0x104: v_and_b32 v20, 1, v1
    0x7d842881,                         // 0x108: v_cmp_eq_u32 vcc, 1, v20
    0x022402ff, 0x00000063,             // 0x10c: v_cndmask_b32 v18, 0x63, v1, vcc (odd lanes: v1)
    0x4a2602ff, 0x000001f4,             // 0x114: v_add_nc_u32 v19, 0x1f4, v1
    0xd8340200, 0x00000102,             // 0x11c: ds_write_b32 v2, v1 offset:512
    0xbf8cc07f,                         // 0x124: s_waitcnt lgkmcnt(0)
    0xbf8a0000,                         // 0x128: s_barrier
    0xd8840100, 0x10000102,             // 0x12c: ds_sub_rtn_u32 v16, v2, v1 offset:256
    0x7e360281,                         // 0x134: v_mov_b32 v27, 1
    0xd8040000, 0x00001b19,             // 0x138: ds_sub_u32 v25, v27
    0xd8c00200, 0x11131202,             // 0x140: ds_cmpst_rtn_b32 v17, v2, v18, v19 offset:512
    0xbf8cc07f,                         // 0x148: s_waitcnt lgkmcnt(0)
    0xbf8a0000,                         // 0x14c: s_barrier
    0xd8d80100, 0x15000002,             // 0x150: ds_read_b32 v21, v2 offset:256
    0xd8d80000, 0x16000019,             // 0x158: ds_read_b32 v22, v25
    0xd8d80200, 0x17000002,             // 0x160: ds_read_b32 v23, v2 offset:512
    0xbf8cc07f,                         // 0x168: s_waitcnt lgkmcnt(0)
    0xe0701700, 0x80001002,             // 0x16c: buffer_store_dword v16, v2, s[0:3], 0 offen offset:1792
    0xe0701800, 0x80001502,             // 0x174: buffer_store_dword v21, v2, s[0:3], 0 offen offset:2048
    0xe0701900, 0x80001602,             // 0x17c: buffer_store_dword v22, v2, s[0:3], 0 offen offset:2304
    0xe0701a00, 0x80001102,             // 0x184: buffer_store_dword v17, v2, s[0:3], 0 offen offset:2560
    0xe0701b00, 0x80001702,             // 0x18c: buffer_store_dword v23, v2, s[0:3], 0 offen offset:2816
    0x7e380287,                         // 0x194: v_mov_b32 v28, 7
    0xd8400200, 0x001c1302,             // 0x198: ds_cmpst_b32 v2, v19, v28 offset:512
    0xbf8cc07f,                         // 0x1a0: s_waitcnt lgkmcnt(0)
    0xd8d80200, 0x18000002,             // 0x1a4: ds_read_b32 v24, v2 offset:512
    0xbf8cc07f,                         // 0x1ac: s_waitcnt lgkmcnt(0)
    0xe0701c00, 0x80001802,             // 0x1b0: buffer_store_dword v24, v2, s[0:3], 0 offen offset:3072
    0xbf810000,                         // 0x1b8: s_endpgm
};

#define UNWRITTEN    0xDEADBEEFu // what the output buffer starts with
#define OUTPUT_BYTES 4096

// What program A stores in region `r` for lane `i`.
static u32 lanes_expected(u32 r, u32 i) {
    switch (r) {
    case 0: // v_mbcnt_lo/hi with -1: the lane
        return i;
    case 1: // v_readlane 63
        return 63 * 3;
    case 2: // v_readlane 32
        return 32 * 3;
    case 3: // v_readlane 63 while EXEC holds lane 0 only
        return 63 * 3;
    case 4: // s_bcnt1_i32_b64 of a compare true in lanes 0-39
        return 40;
    case 5: // s_ff1_i32_b64 of a compare true in lanes 37-63
        return 37;
    case 6: // EXEC = s_bfm_b64 32, 0: lanes 0-31
        return i < 32 ? 1000 + i : UNWRITTEN;
    case 7: // exec_lo = exec_hi = 17: lanes 0, 4, 32, 36
        return (i & 31) == 0 || (i & 31) == 4 ? i : UNWRITTEN;
    case 8: // v_writelane lane 40 with EXEC empty
        return i == 40 ? 63 * 3 : 0;
    case 9: // v_readfirstlane with EXEC = lanes 33-40
        return 33 * 3;
    case 10: // DPP row_shr:1 with row_mask 0xc: rows 2 and 3 only, the first lane of a row unwritten
        return i < 32 || (i & 15) == 0 ? i : i + (i - 1);
    case 11: // ds_bpermute from lane 63 - i
        return (63 - i) * 3;
    case 12: // v_mbcnt_lo/hi of EXEC = lanes 0-15 and 40-47
        return i < 16 ? i : i >= 40 && i < 48 ? 16 + (i - 40) : UNWRITTEN;
    case 13: // VCC as data: vcc_lo = 0x60, vcc_hi = 7, added
        return 0x67;
    case 14: // ds_append in lanes 0-9 and 50-59 on a counter at 100: the old value, in every lane
        return i < 10 || (i >= 50 && i < 60) ? 100 : UNWRITTEN;
    case 15: // v_readlane with the lane in an SGPR (45)
        return 45 * 3;
    default:
        return UNWRITTEN;
    }
}

// What program D stores in region `r` for lane `i` of a wave of `lanes`, its if/else taking the lanes
// below `then_lanes`.
static u32 flow_expected(u32 r, u32 i, u32 lanes, u32 then_lanes) {
    if (i >= lanes)
        return UNWRITTEN;
    switch (r) {
    case 0: // an EXEC skip-over: lanes 0-19 add 100
        return i < 20 ? i + 100 : 7;
    case 1: // a skip-over no lane takes: its scalar write doesn't happen
        return 11;
    case 2: // if/else on EXEC
        return i < then_lanes ? 1 : 2;
    case 3: // a counted scalar loop: 0 + 1 + ... + 9
        return 45;
    case 4: // a waterfall loop over lane & 3
        return (i & 3) * 10 + 1;
    case 5: // its trip count
        return 4;
    case 6: // a loop leaving straight to the join at s20 = 5 (not through the block setting 2)
        return 1005;
    case 7: // the inner loop going on with the outer one: 1 + 2 + 3 + 4
        return 10;
    case 8: // lanes leaving a loop one by one
        return i > 1 ? i : 1;
    default:
        return UNWRITTEN;
    }
}

// What program C stores in region `r` for lane `i`.
static u32 forms_expected(u32 r, u32 i) {
    switch (r) {
    case 0: // the compare (lanes 0-3) is still a mask after s_cmpk read it
        return i < 4 ? 1 : 0;
    case 1: // s_cmpk compared the compare's bits (0xf): the same SCC in every lane
        return 7;
    case 2: // EXEC = -1 + 6 in the low dword (lanes 0 and 2), all set in the high one
        return i == 0 || i == 2 || i >= 32 ? i : UNWRITTEN;
    case 3: // EXEC = 3 << 33: lanes 33 and 34
        return i == 33 || i == 34 ? i : UNWRITTEN;
    case 4: // that shift's SCC (nonzero) in every lane
        return 11;
    case 5: // the compare's bits, joined with data and not moved over
        return 0xF;
    default:
        return UNWRITTEN;
    }
}

// What program G stores in region `r` for lane `i`.
static u32 lane_writes_expected(u32 r, u32 i) {
    switch (r) {
    case 0: // then (lanes 0-19) 7 + 6, else the value from before plus 1
        return i < 20 ? 13 : 101 + i;
    case 1: // lane 1 keeps 2 * 1 while lane 0 gets 55
        return 57;
    case 2: // the LDS value in lanes 0-9, the others keep theirs
        return i < 10 ? 0x4d : 500 + i;
    case 3: // whole-wave reads: lane 40 kept 1, lane 3 got 2
        return 18;
    case 4: // a waterfall loop over lane & 3, each pass writing its lanes
        return (i & 3) + 3 * i;
    default:
        return UNWRITTEN;
    }
}

// The half float bits of a whole number below 2048 (all exact).
static u32 half_of(u32 n) {
    if (n == 0)
        return 0;
    u32 e = 0;
    while ((n >> (e + 1)) != 0)
        e++;
    return (e + 15) << 10 | ((n << (10 - e)) & 0x3FF);
}

static s32 med3(s32 a, s32 b, s32 c) {
    s32 lo = a < b ? a : b, hi = a < b ? b : a;
    return c < lo ? lo : c > hi ? hi : c;
}

// What program I stores in region `r` for lane `i`.
static u32 alu16_lds_expected(u32 r, u32 i) {
    s32 a = (s32)i, b = 63 - (s32)i, s = (s32)i - 32;
    switch (r) {
    case 0: // v_med3_f16 of i, 63 - i, 32
        return half_of((u32)med3(a, b, 32));
    case 1: // v_min3_f16
        return half_of((u32)(a < b ? a : b));
    case 2: // v_max3_f16
        return half_of((u32)(a > b ? a : b));
    case 3: // the median from a high half into the high half, the low half kept
        return half_of((u32)med3(a, b, 32)) << 16 | 0x1234;
    case 4: // v_med3_i16 of i - 32, -5, 7; the high half kept
        return 0xABCD0000u | ((u32)med3(s, -5, 7) & 0xFFFF);
    case 5: { // v_min3_u16 of i, 40 and the half float bits of 63 - i
        u32 m = i < 40 ? i : 40;
        return m < half_of((u32)b) ? m : half_of((u32)b);
    }
    case 6: { // v_max3_i16 of i - 32, -20, 5
        s32 m = s > 5 ? s : 5;
        return 0x77770000u | ((u32)m & 0xFFFF);
    }
    case 7: // ds_sub_rtn_u32: the old value
        return 1000 + i;
    case 8: // and what it left
        return 1000;
    case 9: // 64 lanes' ds_sub_u32 of 1 from 100
        return 36;
    case 10: // ds_cmpst_rtn_b32: the old value
        return i;
    case 11: // odd lanes compared equal and stored 500 + i
        return (i & 1) ? 500 + i : i;
    case 12: // ds_cmpst_b32 comparing with 500 + i: the odd lanes store 7
        return (i & 1) ? 7 : i;
    default:
        return UNWRITTEN;
    }
}

// Program B, in groups of 100 threads (two console waves, the second with 36 lanes): thread t stores
// t + 1000 * (thread ID of lane 0 of its wave).
static u32 waves_expected(u32 t) {
    return t < 100 ? t + 1000 * (t & ~63u) : UNWRITTEN;
}

struct GpuBench {
    ID3D12Device* device;
    ID3D12CommandQueue* queue;
    ID3D12CommandAllocator* allocator;
    ID3D12GraphicsCommandList* list;
    ID3D12Fence* fence;
    u64 fence_value;
    ID3D12RootSignature* root;
    ID3D12Resource* output;   // UAV
    ID3D12Resource* upload;   // the initial contents
    ID3D12Resource* readback; // what the program stored
    IDxcCompiler3* compiler;
    ShaderTarget target;
};

static void release(IUnknown* object) {
    if (object)
        object->Release();
}

static ID3D12Resource* make_buffer(ID3D12Device* device, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_FLAGS flags,
                                   D3D12_RESOURCE_STATES state) {
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = heap;
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = OUTPUT_BYTES;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = flags;
    ID3D12Resource* r = nullptr;
    if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                               IID_PPV_ARGS(&r))))
        return nullptr;
    return r;
}

// The device and what every run needs. False (and a reason) to skip.
static bool bench_init(GpuBench& b, const char** why) {
    memset(&b, 0, sizeof(b));
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&b.device)))) {
        *why = "no D3D12 device";
        return false;
    }
    D3D12_FEATURE_DATA_SHADER_MODEL sm = {D3D_SHADER_MODEL_6_6};
    D3D12_FEATURE_DATA_D3D12_OPTIONS1 o1 = {};
    if (FAILED(b.device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm))) ||
        sm.HighestShaderModel < D3D_SHADER_MODEL_6_6 ||
        FAILED(b.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &o1, sizeof(o1))) || !o1.WaveOps) {
        *why = "no shader model 6.6 with wave operations";
        return false;
    }
    b.target.wave_min = (u8)(o1.WaveLaneCountMin < 255 ? o1.WaveLaneCountMin : 255);
    b.target.wave_max = (u8)(o1.WaveLaneCountMax < 255 ? o1.WaveLaneCountMax : 255);
    if (b.target.wave_min > 32 || b.target.wave_max < 32) {
        *why = "no waves of 32 lanes";
        return false;
    }
    HMODULE dxc = LoadLibraryW(L"dxcompiler.dll");
    DxcCreateInstanceProc create =
        dxc ? (DxcCreateInstanceProc)GetProcAddress(dxc, "DxcCreateInstance") : nullptr;
    if (!create || FAILED(create(CLSID_DxcCompiler, IID_PPV_ARGS(&b.compiler)))) {
        *why = "no dxcompiler.dll";
        return false;
    }
    // The translated programs' root signature: user data (root constants b0) and the root UAV u0, both
    // in space 100 (see shader/translate.h).
    D3D12_ROOT_PARAMETER1 params[2] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0;
    params[0].Constants.RegisterSpace = 100;
    params[0].Constants.Num32BitValues = 4;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[1].Descriptor.ShaderRegister = 0;
    params[1].Descriptor.RegisterSpace = 100;
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC rs = {};
    rs.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    rs.Desc_1_1.NumParameters = 2;
    rs.Desc_1_1.pParameters = params;
    ID3DBlob* blob = nullptr;
    if (FAILED(D3D12SerializeVersionedRootSignature(&rs, &blob, nullptr)) ||
        FAILED(b.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                             IID_PPV_ARGS(&b.root)))) {
        release(blob);
        *why = "cannot create the root signature";
        return false;
    }
    release(blob);
    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    b.output = make_buffer(b.device, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                           D3D12_RESOURCE_STATE_COMMON);
    b.upload = make_buffer(b.device, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                           D3D12_RESOURCE_STATE_GENERIC_READ);
    b.readback = make_buffer(b.device, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE,
                             D3D12_RESOURCE_STATE_COPY_DEST);
    if (!b.output || !b.upload || !b.readback ||
        FAILED(b.device->CreateCommandQueue(&qd, IID_PPV_ARGS(&b.queue))) ||
        FAILED(
            b.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&b.allocator))) ||
        FAILED(b.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, b.allocator, nullptr,
                                           IID_PPV_ARGS(&b.list))) ||
        FAILED(b.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&b.fence)))) {
        *why = "cannot create D3D12 objects";
        return false;
    }
    b.list->Close();
    u32* p = nullptr;
    if (FAILED(b.upload->Map(0, nullptr, (void**)&p))) {
        *why = "cannot map the upload buffer";
        return false;
    }
    for (u32 i = 0; i < OUTPUT_BYTES / 4; i++)
        p[i] = UNWRITTEN;
    b.upload->Unmap(0, nullptr);
    return true;
}

static void bench_release(GpuBench& b) {
    release(b.compiler);
    release(b.readback);
    release(b.upload);
    release(b.output);
    release(b.fence);
    release(b.list);
    release(b.allocator);
    release(b.queue);
    release(b.root);
    release(b.device);
}

static void barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* r, D3D12_RESOURCE_STATES before,
                    D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER rb = {};
    rb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    rb.Transition.pResource = r;
    rb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    rb.Transition.StateBefore = before;
    rb.Transition.StateAfter = after;
    list->ResourceBarrier(1, &rb);
}

// Translates `code` with `info` and runs one group of it; `out` gets the output buffer. With `expect`
// (a list ending in nullptr), the HLSL must hold each text (with a leading '!': must not). False if any
// step failed (reported).
static bool run_program(GpuBench& b, const u32* code, u32 size, const ShaderInfo& info, u32* out,
                        const char* const* expect = nullptr) {
    TranslatedShader t = {};
    if (!shader_translate(code, size, info, b.target, &t)) {
        printf("FAIL shader lanes: translation failed: %s\n", t.error);
        t.hlsl.release();
        return false;
    }
    for (u32 e = 0; expect && expect[e]; e++) {
        bool absent = expect[e][0] == '!';
        const char* text = absent ? expect[e] + 1 : expect[e];
        if ((strstr(t.hlsl.data, text) != nullptr) == absent) {
            printf("FAIL shader lanes: the translation %s \"%s\"\n%s\n", absent ? "has" : "lacks", text,
                   t.hlsl.data);
            t.hlsl.release();
            return false;
        }
    }
    DxcBuffer source = {t.hlsl.data, t.hlsl.length, DXC_CP_UTF8};
    const wchar_t* args[] = {L"-E", L"main", L"-T", L"cs_6_6", L"-O3", L"-HV", L"2021"};
    IDxcResult* result = nullptr;
    HRESULT status = E_FAIL;
    if (SUCCEEDED(b.compiler->Compile(&source, args, ARRAY_COUNT(args), nullptr, IID_PPV_ARGS(&result))))
        result->GetStatus(&status);
    IDxcBlob* object = nullptr;
    if (SUCCEEDED(status))
        result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&object), nullptr);
    if (!object) {
        IDxcBlobUtf8* errors = nullptr;
        if (result)
            result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr);
        printf("FAIL shader lanes: compiling failed: %.400s\n%s\n", errors ? errors->GetStringPointer() : "?",
               t.hlsl.data);
        release(errors);
        release(result);
        t.hlsl.release();
        return false;
    }
    t.hlsl.release();
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = b.root;
    pd.CS.pShaderBytecode = object->GetBufferPointer();
    pd.CS.BytecodeLength = object->GetBufferSize();
    ID3D12PipelineState* pso = nullptr;
    HRESULT hr = b.device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso));
    release(object);
    release(result);
    if (FAILED(hr)) {
        printf("FAIL shader lanes: pipeline creation failed (0x%08lx)\n", (unsigned long)hr);
        return false;
    }
    b.allocator->Reset();
    b.list->Reset(b.allocator, pso);
    barrier(b.list, b.output, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    b.list->CopyResource(b.output, b.upload);
    barrier(b.list, b.output, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    b.list->SetComputeRootSignature(b.root);
    u32 user[4] = {}; // s[0:1]: the root UAV, at offset 0 without a stride
    b.list->SetComputeRoot32BitConstants(0, 4, user, 0);
    b.list->SetComputeRootUnorderedAccessView(1, b.output->GetGPUVirtualAddress());
    b.list->Dispatch(1, 1, 1);
    barrier(b.list, b.output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    b.list->CopyResource(b.readback, b.output);
    barrier(b.list, b.output, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    b.list->Close();
    ID3D12CommandList* lists[] = {b.list};
    b.queue->ExecuteCommandLists(1, lists);
    b.queue->Signal(b.fence, ++b.fence_value);
    HANDLE done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    b.fence->SetEventOnCompletion(b.fence_value, done);
    WaitForSingleObject(done, 10000);
    CloseHandle(done);
    release(pso);
    if (b.fence->GetCompletedValue() < b.fence_value) {
        printf("FAIL shader lanes: the GPU did not finish\n");
        return false;
    }
    u32* p = nullptr;
    D3D12_RANGE range = {0, OUTPUT_BYTES};
    if (FAILED(b.readback->Map(0, &range, (void**)&p)))
        return false;
    memcpy(out, p, OUTPUT_BYTES);
    D3D12_RANGE none = {0, 0};
    b.readback->Unmap(0, &none);
    return true;
}

// Programs F: random control flow graphs, run on the GPU and walked on the CPU. Each block mixes its
// number into a path hash (s12 = s12 * 31 + block + 1), steps a random sequence (s11) and sets SCC from
// one of its bits, then ends in a jump (unconditional, on SCC clear or set, to any block before or after
// it, or to the end), the end of the program, or falls through. The program stores both registers at its
// end: any block reached out of order changes the hash. Graphs come reducible or not, with loops nested
// and left in all ways, so they reach every placement the structured emission has (and the dispatcher).
#define GRAPH_MAX_BLOCKS  40
#define GRAPH_BLOCK_WORDS 12   // fixed, so that jump offsets follow from block numbers
#define GRAPH_MAX_STEPS   3000 // blocks the CPU walk takes before it gives a graph up as endless
#define GRAPH_PROGRAMS    200

enum GraphEnd : u8 { END_BRANCH, END_SCC0, END_SCC1, END_PROGRAM, END_FALL };

struct RandomGraph {
    u32 blocks;
    u32 seed; // s11 at the start
    GraphEnd end[GRAPH_MAX_BLOCKS];
    u32 target[GRAPH_MAX_BLOCKS]; // jumps: the block, or `blocks` for the program's end
};

static u32 g_graph_random = 0x2545F491u;

static u32 graph_random(u32 range) {
    g_graph_random = g_graph_random * 1103515245u + 12345u;
    return (g_graph_random >> 8) % range;
}

static void make_graph(RandomGraph& g) {
    g.blocks = 2 + graph_random(GRAPH_MAX_BLOCKS - 1);
    g.seed = graph_random(0x7FFFFFFF) | 1;
    u32 back = graph_random(5); // in 8: how often a jump goes back
    for (u32 b = 0; b < g.blocks; b++) {
        static const GraphEnd k_ends[10] = {END_BRANCH, END_BRANCH, END_SCC0, END_SCC0,    END_SCC0,
                                            END_SCC1,   END_SCC1,   END_SCC1, END_PROGRAM, END_FALL};
        g.end[b] = k_ends[graph_random(10)];
        if (b + 1 == g.blocks && g.end[b] == END_FALL)
            g.end[b] = END_PROGRAM;
        g.target[b] = graph_random(8) < back ? graph_random(b + 1) : b + 1 + graph_random(g.blocks - b);
    }
}

// The bit of s11 block b sets SCC from.
static u32 graph_bit(u32 b) {
    return 16 + b % 8;
}

// What the program stores: false if it doesn't end within GRAPH_MAX_STEPS blocks.
static bool walk_graph(const RandomGraph& g, u32* hash, u32* sequence) {
    u32 h = 0, s = g.seed, b = 0;
    for (u32 step = 0; step < GRAPH_MAX_STEPS; step++) {
        h = h * 31 + b + 1;
        s = s * 1103515245u + 12345u;
        bool scc = ((s >> graph_bit(b)) & 1) != 0;
        u32 next = b + 1;
        switch (g.end[b]) {
        case END_BRANCH:
            next = g.target[b];
            break;
        case END_SCC0:
            next = scc ? b + 1 : g.target[b];
            break;
        case END_SCC1:
            next = scc ? g.target[b] : b + 1;
            break;
        case END_PROGRAM:
            next = g.blocks;
            break;
        case END_FALL:
            break;
        }
        if (next >= g.blocks) {
            *hash = h;
            *sequence = s;
            return true;
        }
        b = next;
    }
    return false;
}

// Assembles the graph; returns the size in bytes.
static u32 assemble_graph(const RandomGraph& g, u32* code) {
    u32 n = 0;
    code[n++] = 0xbe8203c1; // s_mov_b32 s2, -1
    code[n++] = 0xbe830380; // s_mov_b32 s3, 0
    code[n++] = 0xd7650001; // v_mbcnt_lo_u32_b32 v1, -1, 0
    code[n++] = 0x020100c1;
    code[n++] = 0xd7660001; // v_mbcnt_hi_u32_b32 v1, -1, v1
    code[n++] = 0x020202c1;
    code[n++] = 0x34040282; // v_lshlrev_b32 v2, 2, v1
    code[n++] = 0xbe8b03ff; // s_mov_b32 s11, seed
    code[n++] = g.seed;
    code[n++] = 0xbe8c0380; // s_mov_b32 s12, 0
    u32 first = n;
    u32 end = first + g.blocks * GRAPH_BLOCK_WORDS; // the store at the end
    for (u32 b = 0; b < g.blocks; b++) {
        code[n++] = 0x930cff0c; // s_mul_i32 s12, s12, 31
        code[n++] = 31;
        code[n++] = 0x800cff0c; // s_add_u32 s12, s12, b + 1
        code[n++] = b + 1;
        code[n++] = 0x930bff0b; // s_mul_i32 s11, s11, 1103515245
        code[n++] = 1103515245u;
        code[n++] = 0x800bff0b; // s_add_u32 s11, s11, 12345
        code[n++] = 12345;
        code[n++] = 0x900d000b | (128 + graph_bit(b)) << 8; // s_lshr_b32 s13, s11, bit
        code[n++] = 0x870d810d;                             // s_and_b32 s13, s13, 1
        code[n++] = 0xbf06810d;                             // s_cmp_eq_u32 s13, 1
        u32 to = g.end[b] == END_PROGRAM || g.target[b] >= g.blocks ? end
                                                                    : first + g.target[b] * GRAPH_BLOCK_WORDS;
        u32 offset = (u32)((s32)to - (s32)(n + 1)) & 0xFFFF; // in dwords from the next instruction
        switch (g.end[b]) {
        case END_BRANCH:
        case END_PROGRAM:
            code[n++] = 0xbf820000 | offset; // s_branch
            break;
        case END_SCC0:
            code[n++] = 0xbf840000 | offset; // s_cbranch_scc0
            break;
        case END_SCC1:
            code[n++] = 0xbf850000 | offset; // s_cbranch_scc1
            break;
        case END_FALL:
            code[n++] = 0xbf800000; // s_nop 0
            break;
        }
    }
    code[n++] = 0x7e08020c; // v_mov_b32 v4, s12
    code[n++] = 0xe0701000; // buffer_store_dword v4, v2, s[0:3], 0 offen
    code[n++] = 0x80000402;
    code[n++] = 0x7e08020b; // v_mov_b32 v4, s11
    code[n++] = 0xe0701100; // buffer_store_dword v4, v2, s[0:3], 0 offen offset:256
    code[n++] = 0x80000402;
    code[n++] = 0xbf810000; // s_endpgm
    return n * 4;
}

// Programs H: random masking code, run on the GPU and simulated on the CPU. Vector ALU writes to v4-v11
// under nested ifs (s_and_saveexec, a skip-over branch or not, an else side or not, the end restoring the
// saved mask with s_or or s_mov), v_cmpx narrowing, loops lanes leave one by one, whole-wave code and
// plain lane reads; the saved masks are used where s_and_saveexec put them, or copied, swapped with three
// XORs or parked in spill lanes first. Every lane of v4-v13 is stored at the end, so a write that skips
// its EXEC guard while some later read sees the lanes outside EXEC changes the stores (see
// shader/translate_lanes.cpp). Skip-over branches jump over code that does nothing with EXEC empty (but
// for whole-wave writes, left out there), so the CPU runs every instruction in order.
#define MASKING_PROGRAMS  150 // of each wave size
#define MASKING_MAX_WORDS 1024
#define MASKING_DEPTH     4
#define MASKING_REGIONS   10 // v4-v13
#define MASKING_EXEC      126
#define MASKING_VCC       106
#define MASKING_SPILL     30 // the spill VGPR

enum MaskHome : u8 { HOME_SAVED, HOME_COPY, HOME_SPILL };

struct MaskFrame {
    u64 saved;     // EXEC before the if
    MaskHome home; // where the saved mask is now
    bool else_done;
    u32 skip;     // the skip-over branch still to point at the next else or end, or ~0u
    bool skipped; // that branch jumps (EXEC is empty there)
};

struct MaskingProgram {
    bool wave64;
    u32 lanes;
    u32 code[MASKING_MAX_WORDS];
    u32 n;
    u64 exec;
    u32 v[16][64]; // the CPU's vector registers
    MaskFrame frames[MASKING_DEPTH];
    u32 depth;
};

// A vector ALU instruction: v_mov_b32 dst, k, or v_add_nc_u32 / v_xor_b32 / v_and_b32 dst, a or k, b.
struct MaskAlu {
    u32 op; // VOP2 opcode, or 0 for the move
    u32 dst, a, b;
    bool literal;
    u32 k;
};

static void mask_put(MaskingProgram& m, u32 word) {
    if (m.n < MASKING_MAX_WORDS)
        m.code[m.n++] = word;
}

static u32 mask_sop1(const MaskingProgram& m, u32 op64, u32 op32, u32 dst, u32 src) {
    return 0xBE800000u | dst << 16 | (m.wave64 ? op64 : op32) << 8 | src;
}

static u32 mask_sop2(const MaskingProgram& m, u32 op64, u32 op32, u32 dst, u32 a, u32 b) {
    return 0x80000000u | (m.wave64 ? op64 : op32) << 23 | dst << 16 | b << 8 | a;
}

static u64 mask_all(const MaskingProgram& m) {
    return m.wave64 ? ~0ull : 0xFFFFFFFFull;
}

static bool mask_on(u64 mask, u32 lane) {
    return ((mask >> lane) & 1) != 0;
}

// SGPRs of nesting level d: where s_and_saveexec saves, and where the saved mask is copied or swapped.
static u32 mask_save_reg(u32 d) {
    return 20 + 2 * d;
}

static u32 mask_copy_reg(u32 d) {
    return 40 + 2 * d;
}

// A vector register sources read: the lane number (v1) or v4-v13.
static u32 mask_source() {
    u32 r = graph_random(11);
    return r == 0 ? 1 : 3 + r;
}

static MaskAlu mask_alu_random() {
    static const u32 k_ops[4] = {0, 37, 29, 27}; // move, v_add_nc_u32, v_xor_b32, v_and_b32
    MaskAlu a;
    a.op = k_ops[graph_random(4)];
    a.dst = 4 + graph_random(8);
    a.a = mask_source();
    a.b = mask_source();
    a.literal = a.op == 0 || graph_random(2) == 0;
    a.k = graph_random(0x7FFFFFFF);
    return a;
}

static void mask_alu_emit(MaskingProgram& m, const MaskAlu& a) {
    if (a.op == 0)
        mask_put(m, 0x7E000000u | a.dst << 17 | 1u << 9 | 0xFF); // v_mov_b32 dst, k
    else
        mask_put(m, a.op << 25 | a.dst << 17 | a.b << 9 | (a.literal ? 0xFF : 256 + a.a));
    if (a.literal)
        mask_put(m, a.k);
}

static void mask_alu_run(MaskingProgram& m, const MaskAlu& a) {
    for (u32 l = 0; l < m.lanes; l++) {
        if (!mask_on(m.exec, l))
            continue;
        u32 x = a.literal ? a.k : m.v[a.a][l], y = m.v[a.b][l];
        m.v[a.dst][l] = a.op == 0 ? a.k : a.op == 37 ? x + y : a.op == 29 ? x ^ y : x & y;
    }
}

static void mask_alu(MaskingProgram& m) {
    MaskAlu a = mask_alu_random();
    mask_alu_emit(m, a);
    mask_alu_run(m, a);
}

// v_readlane_b32 sdst, vsrc, lane; v_writelane_b32 vdst, ssrc, lane.
static void mask_readlane(MaskingProgram& m, u32 sdst, u32 vsrc, u32 lane) {
    mask_put(m, 0xD7600000u | sdst);
    mask_put(m, (256 + vsrc) | (128 + lane) << 9 | 0x80u << 18);
}

static void mask_writelane(MaskingProgram& m, u32 vdst, u32 ssrc, u32 lane) {
    mask_put(m, 0xD7610000u | vdst);
    mask_put(m, ssrc | (128 + lane) << 9 | 0x80u << 18);
}

// v_add_nc_u32 dst, s50, dst after s50 = lane `lane` of `src`: a lane read's value used by the lanes in EXEC.
static void mask_add_lane(MaskingProgram& m, u32 src, u32 lane) {
    u32 dst = 4 + graph_random(8);
    u32 value = m.v[src][lane];
    mask_readlane(m, 50, src, lane);
    mask_put(m, 37u << 25 | dst << 17 | dst << 9 | 50);
    for (u32 l = 0; l < m.lanes; l++)
        if (mask_on(m.exec, l))
            m.v[dst][l] += value;
}

// A compare into VCC (clear outside EXEC): on the lane number, or on a bit of a register.
static u64 mask_condition(MaskingProgram& m, u32 gt_op, u32 lt_op) {
    u64 result = 0;
    if (graph_random(3) != 0) {
        u32 k = graph_random(m.lanes + 1);
        bool gt = graph_random(2) == 0;
        mask_put(m, 0x7C000000u | (gt ? gt_op : lt_op) << 17 | 1u << 9 | (128 + k));
        for (u32 l = 0; l < m.lanes; l++)
            if (mask_on(m.exec, l) && (gt ? k > l : k < l))
                result |= 1ull << l;
        return result;
    }
    u32 src = mask_source();
    u32 bit = 1u << graph_random(6);
    mask_put(m, 27u << 25 | 14u << 17 | src << 9 | (128 + bit)); // v_and_b32 v14, bit, src
    for (u32 l = 0; l < m.lanes; l++)
        if (mask_on(m.exec, l))
            m.v[14][l] = m.v[src][l] & bit;
    mask_put(m, 0x7C000000u | lt_op << 17 | 14u << 9 | 128); // 0 < v14
    for (u32 l = 0; l < m.lanes; l++)
        if (mask_on(m.exec, l) && m.v[14][l] != 0)
            result |= 1ull << l;
    return result;
}

static void mask_skip_over(MaskingProgram& m, MaskFrame& f) {
    if (graph_random(2) == 0)
        return;
    f.skip = m.n;
    f.skipped = m.exec == 0;
    mask_put(m, 0xBF880000u); // s_cbranch_execz, pointed later
}

static void mask_skip_here(MaskingProgram& m, MaskFrame& f) {
    if (f.skip != ~0u)
        m.code[f.skip] |= (u32)(m.n - (f.skip + 1)) & 0xFFFF;
    f.skip = ~0u;
    f.skipped = false;
}

static void mask_if(MaskingProgram& m) {
    u32 d = m.depth++;
    MaskFrame& f = m.frames[d];
    u64 vcc = mask_condition(m, 0xc4, 0xc1); // v_cmp_gt_u32, v_cmp_lt_u32
    f.saved = m.exec;
    f.home = HOME_SAVED;
    f.else_done = false;
    f.skip = ~0u;
    f.skipped = false;
    u32 save = mask_save_reg(d), copy = mask_copy_reg(d);
    mask_put(m, mask_sop1(m, 36, 60, save, MASKING_VCC)); // s_and_saveexec
    m.exec &= vcc;
    switch (graph_random(5)) {
    case 0: // s_mov to another register
        mask_put(m, mask_sop1(m, 4, 3, copy, save));
        f.home = HOME_COPY;
        break;
    case 1: // swapped with another register by three XORs
        mask_put(m, mask_sop2(m, 19, 18, save, save, copy));
        mask_put(m, mask_sop2(m, 19, 18, copy, copy, save));
        mask_put(m, mask_sop2(m, 19, 18, save, save, copy));
        f.home = HOME_COPY;
        break;
    case 2: // parked in spill lanes, the register then cleared
        mask_writelane(m, MASKING_SPILL, save, 2 * d);
        if (m.wave64)
            mask_writelane(m, MASKING_SPILL, save + 1, 2 * d + 1);
        mask_put(m, mask_sop1(m, 4, 3, save, 128));
        f.home = HOME_SPILL;
        break;
    default:
        break;
    }
    mask_skip_over(m, f);
}

// The register holding level d's saved mask, reloaded from its spill lanes if parked there.
static u32 mask_saved(MaskingProgram& m, MaskFrame& f, u32 d) {
    if (f.home == HOME_COPY)
        return mask_copy_reg(d);
    if (f.home == HOME_SPILL) {
        mask_readlane(m, mask_save_reg(d), MASKING_SPILL, 2 * d);
        if (m.wave64)
            mask_readlane(m, mask_save_reg(d) + 1, MASKING_SPILL, 2 * d + 1);
        f.home = HOME_SAVED;
    }
    return mask_save_reg(d);
}

static void mask_else(MaskingProgram& m) {
    u32 d = m.depth - 1;
    MaskFrame& f = m.frames[d];
    mask_skip_here(m, f);
    u32 saved = mask_saved(m, f, d);
    mask_put(m, mask_sop2(m, 21, 20, MASKING_EXEC, saved, MASKING_EXEC)); // s_andn2 exec, saved, exec
    m.exec = f.saved & ~m.exec;
    f.else_done = true;
    mask_skip_over(m, f);
}

static void mask_end_if(MaskingProgram& m) {
    u32 d = m.depth - 1;
    MaskFrame& f = m.frames[d];
    mask_skip_here(m, f);
    u32 saved = mask_saved(m, f, d);
    if (graph_random(2) == 0)
        mask_put(m, mask_sop2(m, 17, 16, MASKING_EXEC, MASKING_EXEC, saved)); // s_or exec, exec, saved
    else
        mask_put(m, mask_sop1(m, 4, 3, MASKING_EXEC, saved)); // s_mov exec, saved
    m.exec = f.saved;
    m.depth--;
}

// Whole-wave code: s_or_saveexec -1, maybe a write of every lane, a lane read, EXEC back. The write only
// where no skip-over branch jumps past it (the only code here that does something with EXEC empty).
static void mask_whole_wave(MaskingProgram& m) {
    bool skipped = false;
    for (u32 d = 0; d < m.depth; d++)
        skipped = skipped || m.frames[d].skipped;
    u64 saved = m.exec;
    mask_put(m, mask_sop1(m, 37, 61, 30, 193));
    m.exec = mask_all(m);
    if (!skipped && graph_random(2) == 0)
        mask_alu(m);
    u32 src = mask_source(), lane = graph_random(m.lanes);
    u32 value = m.v[src][lane];
    mask_readlane(m, 50, src, lane);
    mask_put(m, mask_sop1(m, 4, 3, MASKING_EXEC, 30));
    m.exec = saved;
    u32 dst = 4 + graph_random(8);
    mask_put(m, 37u << 25 | dst << 17 | dst << 9 | 50); // v_add_nc_u32 dst, s50, dst
    for (u32 l = 0; l < m.lanes; l++)
        if (mask_on(m.exec, l))
            m.v[dst][l] += value;
}

// A loop lanes leave one by one: v12 counts the passes, a lane leaves once it passes v13 = lane & k.
static void mask_loop(MaskingProgram& m) {
    u64 saved = m.exec;
    u32 k = 1 + graph_random(7);
    mask_put(m, mask_sop1(m, 4, 3, 60, MASKING_EXEC));
    mask_put(m, 0x7E000000u | 12u << 17 | 1u << 9 | 128);     // v_mov_b32 v12, 0
    mask_put(m, 27u << 25 | 13u << 17 | 1u << 9 | (128 + k)); // v_and_b32 v13, k, v1
    for (u32 l = 0; l < m.lanes; l++)
        if (mask_on(m.exec, l)) {
            m.v[12][l] = 0;
            m.v[13][l] = l & k;
        }
    u32 top = m.n;
    MaskAlu body[3];
    u32 count = 1 + graph_random(3);
    for (u32 i = 0; i < count; i++) {
        body[i] = mask_alu_random();
        mask_alu_emit(m, body[i]);
    }
    mask_put(m, 37u << 25 | 12u << 17 | 12u << 9 | 129);                        // v_add_nc_u32 v12, 1, v12
    mask_put(m, 0x7C000000u | 0xc4u << 17 | 13u << 9 | (256 + 12));             // v_cmp_gt_u32 vcc, v12, v13
    mask_put(m, mask_sop2(m, 21, 20, MASKING_EXEC, MASKING_EXEC, MASKING_VCC)); // s_andn2 exec, exec, vcc
    mask_put(m, 0xBF890000u | ((u32)(top - (m.n + 1)) & 0xFFFF));               // s_cbranch_execnz top
    mask_put(m, mask_sop1(m, 4, 3, MASKING_EXEC, 60));
    do {
        for (u32 i = 0; i < count; i++)
            mask_alu_run(m, body[i]);
        u64 leave = 0;
        for (u32 l = 0; l < m.lanes; l++)
            if (mask_on(m.exec, l) && ++m.v[12][l] > m.v[13][l])
                leave |= 1ull << l;
        m.exec &= ~leave;
    } while (m.exec != 0);
    m.exec = saved;
}

static void make_masking(MaskingProgram& m, bool wave64) {
    m.wave64 = wave64;
    m.lanes = wave64 ? 64 : 32;
    m.n = 0;
    m.depth = 0;
    m.exec = mask_all(m);
    mask_put(m, 0xbe8203c1); // s_mov_b32 s2, -1
    mask_put(m, 0xbe830380); // s_mov_b32 s3, 0
    mask_put(m, 0xd7650001); // v_mbcnt_lo_u32_b32 v1, -1, 0
    mask_put(m, 0x020100c1);
    if (wave64) {
        mask_put(m, 0xd7660001); // v_mbcnt_hi_u32_b32 v1, -1, v1
        mask_put(m, 0x020202c1);
    }
    mask_put(m, 0x34040282); // v_lshlrev_b32 v2, 2, v1
    for (u32 l = 0; l < m.lanes; l++)
        m.v[1][l] = l;
    for (u32 r = 4; r <= 14; r++) {
        u32 k = graph_random(0x7FFFFFFF);
        mask_put(m, 37u << 25 | r << 17 | 1u << 9 | 0xFF); // v_add_nc_u32 vr, k, v1
        mask_put(m, k);
        for (u32 l = 0; l < m.lanes; l++)
            m.v[r][l] = k + l;
    }
    u32 steps = 20 + graph_random(50);
    for (u32 s = 0; s < steps && m.n + 64 < MASKING_MAX_WORDS; s++) {
        u32 pick = graph_random(16);
        if (pick < 6 || (pick >= 9 && pick < 13 && m.depth == 0))
            mask_alu(m);
        else if (pick < 9)
            m.depth < MASKING_DEPTH ? mask_if(m) : mask_alu(m);
        else if (pick == 9)
            m.frames[m.depth - 1].else_done ? mask_end_if(m) : mask_else(m);
        else if (pick < 12)
            mask_end_if(m);
        else if (pick == 12)
            m.exec &= mask_condition(m, 0xd4, 0xd1); // v_cmpx_gt_u32, v_cmpx_lt_u32 (inside an if)
        else if (pick == 13)
            mask_whole_wave(m);
        else if (pick == 14)
            mask_add_lane(m, mask_source(), graph_random(m.lanes));
        else
            mask_loop(m);
    }
    while (m.depth)
        mask_end_if(m);
    for (u32 r = 0; r < MASKING_REGIONS; r++) {
        mask_put(m, 0xE0701000u | r * 256); // buffer_store_dword v(4 + r), v2, s[0:3], 0 offen offset:256r
        mask_put(m, 0x80000002u | (4 + r) << 8);
    }
    mask_put(m, 0xbf810000); // s_endpgm
}

static ShaderInfo compute_info(u16 threads, u32 lds_bytes) {
    ShaderInfo info;
    memset(&info, 0, sizeof(info));
    info.stage = SHADER_COMPUTE;
    info.user_sgprs = 2;
    info.parameter_count = 1;
    info.parameters[0].kind = USER_DATA_ROOT_UAV;
    info.parameters[0].sgpr = 0;
    info.parameters[0].dwords = 2;
    info.parameters[0].root_register = 0;
    info.resource_heap_sgpr = NO_HEAP_SGPR;
    info.sampler_heap_sgpr = NO_HEAP_SGPR;
    info.threads[0] = threads;
    info.threads[1] = 1;
    info.threads[2] = 1;
    info.thread_id_count = 1;
    info.lds_bytes = lds_bytes;
    info.wave_size = 64;
    return info;
}

void test_shader_lanes() {
    rdna_init();
    GpuBench b;
    const char* why = nullptr;
    if (!bench_init(b, &why)) {
        printf("skipped shader lane tests: %s\n", why);
        bench_release(b);
        return;
    }
    static u32 out[OUTPUT_BYTES / 4];
    if (run_program(b, k_lanes_program, sizeof(k_lanes_program), compute_info(64, 1024), out)) {
        for (u32 r = 0; r < 16; r++)
            for (u32 i = 0; i < 64; i++) {
                u32 want = lanes_expected(r, i);
                if (out[r * 64 + i] != want) {
                    printf("FAIL shader lanes: region %u lane %u: 0x%x, expected 0x%x\n", r, i,
                           out[r * 64 + i], want);
                    g_failures++;
                    break; // one report per region
                }
            }
    } else {
        g_failures++;
    }
    if (run_program(b, k_waves_program, sizeof(k_waves_program), compute_info(100, 0), out)) {
        for (u32 t = 0; t < 128; t++)
            if (out[t] != waves_expected(t)) {
                printf("FAIL shader lanes: group thread %u: 0x%x, expected 0x%x\n", t, out[t],
                       waves_expected(t));
                g_failures++;
                break;
            }
    } else {
        g_failures++;
    }
    if (run_program(b, k_forms_program, sizeof(k_forms_program), compute_info(64, 0), out)) {
        for (u32 r = 0; r < 16; r++)
            for (u32 i = 0; i < 64; i++) {
                u32 want = forms_expected(r, i);
                if (out[r * 64 + i] != want) {
                    printf("FAIL shader lanes: program C region %u lane %u: 0x%x, expected 0x%x\n", r, i,
                           out[r * 64 + i], want);
                    g_failures++;
                    break;
                }
            }
    } else {
        g_failures++;
    }
    // Program D, wave64 and wave32: structured, with jumps out of several statements (go_; the
    // dispatcher has none).
    for (u32 w = 0; w < 2; w++) {
        bool wave64 = w == 0;
        ShaderInfo info = compute_info(wave64 ? 64 : 32, 0);
        info.wave_size = wave64 ? 64 : 32;
        const u32* code = wave64 ? k_flow_program : k_flow32_program;
        u32 size = wave64 ? sizeof(k_flow_program) : sizeof(k_flow32_program);
        static const char* const k_go[] = {"go_ = ", nullptr};
        if (!run_program(b, code, size, info, out, k_go)) {
            g_failures++;
            continue;
        }
        for (u32 r = 0; r < 16; r++)
            for (u32 i = 0; i < 64; i++) {
                u32 want = flow_expected(r, i, wave64 ? 64 : 32, wave64 ? 40 : 12);
                if (out[r * 64 + i] != want) {
                    printf("FAIL shader lanes: program D (wave%u) region %u lane %u: 0x%x, expected 0x%x\n",
                           wave64 ? 64 : 32, r, i, out[r * 64 + i], want);
                    g_failures++;
                    break;
                }
            }
    }
    static const char* const k_dispatched[] = {"blocks dispatched", nullptr};
    if (run_program(b, k_irreducible_program, sizeof(k_irreducible_program), compute_info(64, 0), out,
                    k_dispatched)) {
        for (u32 i = 0; i < 64; i++)
            if (out[i] != 34) {
                printf("FAIL shader lanes: program E lane %u: 0x%x, expected 34\n", i, out[i]);
                g_failures++;
                break;
            }
    } else {
        g_failures++;
    }
    // Program G: which writes keep the lanes outside EXEC (a select) and which write every lane (both
    // halves are emitted alike; the first is checked).
    static const char* const k_guards[] = {
        "v4_0 = select(EXEC0, 0x00000007u, v4_0);", // the else lanes read the old value
        "v5_0 = 0x00000003u;",                      // temporaries of the then side
        "!v5_0 = select(",
        "!if (EXEC0) v5_0",
        "!v6_0 = select(",
        "!if (EXEC0) v6_0",
        "v7_0 = select(EXEC0, 0x00000037u, v7_0);",   // v_readlane reads a lane outside EXEC
        "v9_0 = select(EXEC0, g_lds[",                // a load whose other lanes are read after the join
        "v12_0 = select(EXEC0, 0x00000002u, v12_0);", // whole-wave code reads the other lanes
        "v15_0 = select(EXEC0, ",                     // waterfall passes each write their lanes
        "!v16_0 = select(",                           // a temporary of the passes
        "!if (EXEC0) v16_0",
        nullptr,
    };
    if (run_program(b, k_lane_writes_program, sizeof(k_lane_writes_program), compute_info(64, 64), out,
                    k_guards)) {
        for (u32 r = 0; r < 16; r++)
            for (u32 i = 0; i < 64; i++) {
                u32 want = lane_writes_expected(r, i);
                if (out[r * 64 + i] != want) {
                    printf("FAIL shader lanes: program G region %u lane %u: 0x%x, expected 0x%x\n", r, i,
                           out[r * 64 + i], want);
                    g_failures++;
                    break;
                }
            }
    } else {
        g_failures++;
    }
    static const char* const k_atomics[] = {"InterlockedCompareExchange(", "InterlockedCompareStore(",
                                            nullptr};
    if (run_program(b, k_alu16_lds_program, sizeof(k_alu16_lds_program), compute_info(64, 1024), out,
                    k_atomics)) {
        for (u32 r = 0; r < 16; r++)
            for (u32 i = 0; i < 64; i++) {
                u32 want = alu16_lds_expected(r, i);
                if (out[r * 64 + i] != want) {
                    printf("FAIL shader lanes: program I region %u lane %u: 0x%x, expected 0x%x\n", r, i,
                           out[r * 64 + i], want);
                    g_failures++;
                    break;
                }
            }
    } else {
        g_failures++;
    }
    // Programs F: the first GRAPH_PROGRAMS random graphs that end.
    static u32 code[16 + GRAPH_MAX_BLOCKS * GRAPH_BLOCK_WORDS + 8];
    u32 graphs = 0;
    while (graphs < GRAPH_PROGRAMS) {
        RandomGraph g;
        make_graph(g);
        u32 hash, sequence;
        if (!walk_graph(g, &hash, &sequence))
            continue;
        graphs++;
        if (!run_program(b, code, assemble_graph(g, code), compute_info(64, 0), out)) {
            g_failures++;
            break;
        }
        bool same = true;
        for (u32 i = 0; i < 64 && same; i++)
            same = out[i] == hash && out[64 + i] == sequence;
        if (!same) {
            printf(
                "FAIL shader lanes: random graph %u (%u blocks, seed 0x%x): 0x%x 0x%x, expected 0x%x 0x%x\n",
                graphs - 1, g.blocks, g.seed, out[0], out[64], hash, sequence);
            g_failures++;
            break;
        }
    }
    // Programs H: random masking code of both wave sizes against the CPU.
    static MaskingProgram masking;
    u32 maskings = 0;
    for (u32 i = 0; i < 2 * MASKING_PROGRAMS; i++) {
        bool wave64 = i < MASKING_PROGRAMS;
        make_masking(masking, wave64);
        ShaderInfo info = compute_info(wave64 ? 64 : 32, 0);
        info.wave_size = wave64 ? 64 : 32;
        if (!run_program(b, masking.code, masking.n * 4, info, out)) {
            g_failures++;
            break;
        }
        maskings++;
        bool same = true;
        for (u32 r = 0; r < MASKING_REGIONS && same; r++)
            for (u32 l = 0; l < masking.lanes && same; l++)
                if (out[r * 64 + l] != masking.v[4 + r][l]) {
                    printf(
                        "FAIL shader lanes: masking program %u (wave%u) v%u lane %u: 0x%x, expected 0x%x\n",
                        i, masking.lanes, 4 + r, l, out[r * 64 + l], masking.v[4 + r][l]);
                    same = false;
                }
        if (!same) {
            g_failures++;
            break;
        }
    }
    printf("shader lanes: %u programs run on the GPU (waves of %u-%u lanes)\n", 8 + graphs + maskings,
           b.target.wave_min, b.target.wave_max);
    bench_release(b);
}
