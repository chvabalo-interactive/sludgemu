// Console CPU instructions on hosts without them: CLZERO, MONITORX, MWAITX, CLWB and SSE4a's
// EXTRQ and INSERTQ.
//
// EXTRQ and INSERTQ extract or insert a bit field in the low 64 bits of an XMM register. Compilers
// emit them for packing (half floats, for one). The handler carries out each one on the registers in
// the exception context and resumes after it.
//
// MONITORX/MWAITX wait for a store to a watched address, but may also return early for any reason,
// so callers always re-check what they wait for. A MONITORX becomes a no-op and an MWAITX a PAUSE:
// an early return every time, which turns the wait into a polite spin. Those are patched in place
// on first use, as the replacement is exact.
//
// CLWB (66 [REX] 0F AE /6) writes back a modified cache line without invalidating it. Desktop
// memory is coherent across CPU caches and devices, so CLWB is patched in place on first use to an
// exact-length NOP.
//
// CLZERO:
//
// CLZERO (AMD only) zeroes the 64-byte cache line rAX points into. Titles clear large structures
// with loops around it, one line per iteration, e.g.:
//
//     L: clzero                        L: mov rax, rP
//        add rax, 64                      add rP, 64
//        cmp rax, rEnd                    clzero
//        jb  L                            cmp rP, rEnd
//                                         jb  L
//
// Trapping once per line would cost an exception per 64 bytes. Instead the handler runs the loop
// to its exit: it interprets the faulting CLZERO and the instructions after it for as long as they
// are among the few forms these loops use, and stops when the loop's branch falls through or an
// instruction outside those forms comes up. The title then continues natively from there with the
// registers and flags the loop left. One exception per loop.

#include "core/cpu_compat.h"

#include <windows.h>
#include <intrin.h>
#include <string.h>

#include "common/log.h"
#include "loader/loader.h"

#define CACHE_LINE 64

// x86 flag bits.
#define FLAG_CF     (1u << 0)
#define FLAG_PF     (1u << 2)
#define FLAG_AF     (1u << 4)
#define FLAG_ZF     (1u << 6)
#define FLAG_SF     (1u << 7)
#define FLAG_OF     (1u << 11)
#define ARITH_FLAGS (FLAG_CF | FLAG_PF | FLAG_AF | FLAG_ZF | FLAG_SF | FLAG_OF)

// A loop that never exits would also hang on the console; the cap only keeps the handler from
// running unbounded, and the next CLZERO traps again.
#define MAX_INSTRUCTIONS (1u << 24)

// The longest instruction we decode, to keep every fetch inside the image.
#define MAX_INSTRUCTION_BYTES 4

static volatile long g_reported = 0;
static volatile long g_reported_clwb = 0;
static volatile long g_reported_sse4a = 0;
static volatile LONG64 g_sse4a_traps = 0;
static bool g_emulate_clzero = false;
static bool g_emulate_sse4a = false;
static bool g_patch_monitorx = false;
static bool g_patch_clwb = false;
static SRWLOCK g_patch_lock = SRWLOCK_INIT;

// Host CPUID: CLZERO is extended leaf 0x80000008 EBX bit 0, MONITORX/MWAITX leaf 0x80000001 ECX bit 29.
static bool host_has_clzero() {
    int r[4];
    __cpuid(r, 0x80000000);
    if ((u32)r[0] < 0x80000008u)
        return false;
    __cpuid(r, 0x80000008);
    return (r[1] & 1) != 0;
}

// SSE4a: extended leaf 0x80000001 ECX bit 6.
static bool host_has_sse4a() {
    int r[4];
    __cpuid(r, 0x80000000);
    if ((u32)r[0] < 0x80000001u)
        return false;
    __cpuid(r, 0x80000001);
    return (r[2] & (1 << 6)) != 0;
}

static bool host_has_monitorx() {
    int r[4];
    __cpuid(r, 0x80000000);
    if ((u32)r[0] < 0x80000001u)
        return false;
    __cpuid(r, 0x80000001);
    return (r[2] & (1 << 29)) != 0;
}

// CLWB: standard leaf 7 subleaf 0 EBX bit 24.
static bool host_has_clwb() {
    int r[4];
    __cpuid(r, 0);
    if ((u32)r[0] < 7u)
        return false;
    __cpuidex(r, 7, 0);
    return ((u32)r[1] & (1u << 24)) != 0;
}

// Flags of a 64-bit add (r = a + b) or subtract/compare (r = a - b).
static u32 arith_flags(u64 a, u64 b, u64 r, bool sub) {
    u32 f = 0;
    if (sub ? a < b : r < a)
        f |= FLAG_CF;
    if (!(__popcnt((u32)(r & 0xff)) & 1))
        f |= FLAG_PF;
    if ((a ^ b ^ r) & 0x10)
        f |= FLAG_AF;
    if (r == 0)
        f |= FLAG_ZF;
    if (r >> 63)
        f |= FLAG_SF;
    u64 overflow = sub ? (a ^ b) & (a ^ r) : ~(a ^ b) & (a ^ r);
    if (overflow >> 63)
        f |= FLAG_OF;
    return f;
}

// Zeroes the line; false if the memory isn't writable. CLZERO would fault there too.
static bool zero_line(u64 address) {
    __try {
        memset((void*)(address & ~(u64)(CACHE_LINE - 1)), 0, CACHE_LINE);
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER
                                                                 : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
    return true;
}

static bool is_clzero(const u8* ip) {
    return ip[0] == 0x0f && ip[1] == 0x01 && ip[2] == 0xfc;
}

// Interprets from the faulting CLZERO until the loop exits. The context holds the general purpose
// registers in encoding order, rAX first, so a register number indexes them directly.
static LONG run_clzero_loop(EXCEPTION_POINTERS* ep, const PeImage* image) {
    CONTEXT* ctx = ep->ContextRecord;
    DWORD64* regs = &ctx->Rax;
    const u8* end = image->base + image->size - MAX_INSTRUCTION_BYTES;
    const u8* ip = (const u8*)ctx->Rip;
    u32 flags = ctx->EFlags;

    for (u32 executed = 0; executed < MAX_INSTRUCTIONS; executed++) {
        if (ip < image->base || ip > end)
            break;
        if (is_clzero(ip)) {
            if (!zero_line(regs[0])) {
                // Report what the console would: a write fault at the instruction.
                EXCEPTION_RECORD* rec = ep->ExceptionRecord;
                ctx->Rip = (DWORD64)ip;
                ctx->EFlags = (ctx->EFlags & ~ARITH_FLAGS) | (flags & ARITH_FLAGS);
                rec->ExceptionCode = EXCEPTION_ACCESS_VIOLATION;
                rec->ExceptionAddress = (void*)ip;
                rec->NumberParameters = 2;
                rec->ExceptionInformation[0] = 1;
                rec->ExceptionInformation[1] = (ULONG_PTR)(regs[0] & ~(u64)(CACHE_LINE - 1));
                return EXCEPTION_CONTINUE_SEARCH;
            }
            ip += 3;
            continue;
        }
        if (ip[0] == 0x72) { // jb rel8: taken while the last compare came out below
            if (!(flags & FLAG_CF)) {
                ip += 2;
                break;
            }
            ip += 2 + (s8)ip[1];
            continue;
        }
        // The rest are 64-bit register forms: REX.W, opcode, ModRM with mod 11.
        u8 rex = ip[0];
        u8 modrm = ip[2];
        if ((rex & 0xfa) != 0x48 || (modrm & 0xc0) != 0xc0)
            break;
        u32 rm = (modrm & 7) | ((rex & 1) << 3);
        u32 reg = ((modrm >> 3) & 7) | ((rex & 4) << 1);
        u8 op = ip[1];
        if (op == 0x83 && reg == 0 && rm != 4) { // add r64, imm8
            u64 a = regs[rm], b = (u64)(s64)(s8)ip[3];
            regs[rm] = a + b;
            flags = arith_flags(a, b, a + b, false);
            ip += 4;
        } else if ((op == 0x89 || op == 0x8b) && rm != 4 && reg != 4) { // mov r64, r64
            if (op == 0x89)
                regs[rm] = regs[reg];
            else
                regs[reg] = regs[rm];
            ip += 3;
        } else if (op == 0x39 || op == 0x3b) { // cmp r64, r64
            u64 a = op == 0x39 ? regs[rm] : regs[reg];
            u64 b = op == 0x39 ? regs[reg] : regs[rm];
            flags = arith_flags(a, b, a - b, true);
            ip += 3;
        } else {
            break;
        }
    }

    ctx->Rip = (DWORD64)ip;
    ctx->EFlags = (ctx->EFlags & ~ARITH_FLAGS) | (flags & ARITH_FLAGS);
    return EXCEPTION_CONTINUE_EXECUTION;
}

// --- SSE4a ---------------------------------------------------------------------------------------------

// Bits [len-1:0]; a length of 0 means all 64.
static u64 field_mask(u32 len) {
    return len == 0 || len >= 64 ? ~0ull : (1ull << len) - 1;
}

// EXTRQ   66 [REX] 0F 78 /0 ib ib    dst = dst >> idx, masked to len bits
//         66 [REX] 0F 79 /r          the same with len and idx in bits 5:0 and 13:8 of src
// INSERTQ F2 [REX] 0F 78 /r ib ib    the low len bits of src replace bits idx.. of dst
//         F2 [REX] 0F 79 /r          the same with len and idx in bits 69:64 and 77:72 of src
// Register operands only (mod = 3). Both leave the upper half of dst undefined; it is kept. Returns
// the instruction's length, or 0 if `ip` holds neither.
static u32 emulate_sse4a(const u8* ip, const u8* end, CONTEXT* ctx) {
    if (ip + 4 > end)
        return 0;
    u8 prefix = ip[0];
    if (prefix != 0x66 && prefix != 0xf2)
        return 0;
    u32 at = 1;
    u8 rex = 0;
    if ((ip[at] & 0xf0) == 0x40)
        rex = ip[at++];
    if (ip + at + 3 > end || ip[at] != 0x0f || (ip[at + 1] != 0x78 && ip[at + 1] != 0x79))
        return 0;
    bool immediate = ip[at + 1] == 0x78;
    u8 modrm = ip[at + 2];
    at += 3;
    if ((modrm >> 6) != 3)
        return 0;
    u32 reg = ((modrm >> 3) & 7) | ((rex & 4) ? 8 : 0);
    u32 rm = (modrm & 7) | ((rex & 1) ? 8 : 0);
    u32 len = 0, idx = 0; // from the immediate here, or from the source register below
    if (immediate) {
        if (ip + at + 2 > end)
            return 0;
        len = ip[at] & 63;
        idx = ip[at + 1] & 63;
        at += 2;
    }
    M128A* xmm = &ctx->Xmm0;
    if (prefix == 0x66) { // EXTRQ: the destination is the r/m register (and the source, for the reg form)
        if (immediate && ((modrm >> 3) & 7) != 0)
            return 0;
        M128A& dst = xmm[immediate ? rm : reg];
        if (!immediate) {
            len = xmm[rm].Low & 63;
            idx = (xmm[rm].Low >> 8) & 63;
        }
        dst.Low = (dst.Low >> idx) & field_mask(len);
    } else { // INSERTQ: destination reg, source rm
        M128A& dst = xmm[reg];
        const M128A& src = xmm[rm];
        if (!immediate) {
            len = (u32)src.High & 63;
            idx = (u32)(src.High >> 8) & 63;
        }
        u64 mask = field_mask(len) << idx;
        dst.Low = (dst.Low & ~mask) | ((src.Low << idx) & mask);
    }
    return at;
}

// MONITORX (0F 01 FA) becomes a 3-byte NOP, MWAITX (0F 01 FB) a PAUSE and a NOP.
static const u8 k_monitorx_patch[3] = {0x0f, 0x1f, 0x00};
static const u8 k_mwaitx_patch[3] = {0xf3, 0x90, 0x90};

// Canonical Intel/AMD multi-byte NOP encodings of 1..9 bytes.
static const u8 k_nop1[] = {0x90};
static const u8 k_nop2[] = {0x66, 0x90};
static const u8 k_nop3[] = {0x0f, 0x1f, 0x00};
static const u8 k_nop4[] = {0x0f, 0x1f, 0x40, 0x00};
static const u8 k_nop5[] = {0x0f, 0x1f, 0x44, 0x00, 0x00};
static const u8 k_nop6[] = {0x66, 0x0f, 0x1f, 0x44, 0x00, 0x00};
static const u8 k_nop7[] = {0x0f, 0x1f, 0x80, 0x00, 0x00, 0x00, 0x00};
static const u8 k_nop8[] = {0x0f, 0x1f, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00};
static const u8 k_nop9[] = {0x66, 0x0f, 0x1f, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00};

static const u8* const k_nop_table[10] = {
    nullptr, k_nop1, k_nop2, k_nop3, k_nop4, k_nop5, k_nop6, k_nop7, k_nop8, k_nop9,
};

// Overwrites `len` bytes of executable code at `ip` with `patch`. Caller holds `g_patch_lock`.
static bool patch_code(u8* ip, const u8* patch, u32 len) {
    DWORD old;
    if (!VirtualProtect(ip, len, PAGE_EXECUTE_READWRITE, &old))
        return false;
    memcpy(ip, patch, len);
    VirtualProtect(ip, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), ip, len);
    return true;
}

// Rewrites a MONITORX/MWAITX at `ip` (or finds another thread already did).
static bool patch_wait_instruction(u8* ip) {
    AcquireSRWLockExclusive(&g_patch_lock);
    bool ok;
    if (ip[0] == 0x0f && ip[1] == 0x01 && (ip[2] == 0xfa || ip[2] == 0xfb)) {
        const u8* patch = ip[2] == 0xfa ? k_monitorx_patch : k_mwaitx_patch;
        ok = patch_code(ip, patch, 3);
    } else {
        ok = memcmp(ip, k_monitorx_patch, 3) == 0 || memcmp(ip, k_mwaitx_patch, 3) == 0;
    }
    ReleaseSRWLockExclusive(&g_patch_lock);
    return ok;
}

// Returns the byte length of a CLWB (66 [REX] 0F AE /6, mod != 3) instruction at `ip`, or 0.
static u32 clwb_length(const u8* ip, const u8* end) {
    if (ip >= end)
        return 0;
    u32 limit = (u32)((end - ip) > 15 ? 15 : (end - ip));
    u32 at = 0;
    bool has_66 = false;
    while (at < limit) {
        u8 b = ip[at];
        if (b == 0x66) {
            has_66 = true;
            at++;
        } else if (b == 0x67 || b == 0x26 || b == 0x2e || b == 0x36 || b == 0x3e || b == 0x64 ||
                   b == 0x65) {
            at++;
        } else {
            break;
        }
    }
    if (!has_66)
        return 0;
    if (at < limit && (ip[at] & 0xf0) == 0x40)
        at++;
    if (at + 3 > limit || ip[at] != 0x0f || ip[at + 1] != 0xae)
        return 0;
    u8 modrm = ip[at + 2];
    at += 3;
    u8 mod = (modrm >> 6) & 3;
    u8 reg = (modrm >> 3) & 7;
    u8 rm = modrm & 7;
    if (mod == 3 || reg != 6)
        return 0;
    if (rm == 4) {
        if (at >= limit)
            return 0;
        u8 sib = ip[at++];
        if (mod == 0 && (sib & 7) == 5)
            at += 4;
    } else if (mod == 0 && rm == 5) {
        at += 4;
    }
    if (mod == 1)
        at += 1;
    else if (mod == 2)
        at += 4;
    return at <= limit ? at : 0;
}

static void fill_nop(u8* dst, u32 len) {
    while (len > 0) {
        u32 chunk = len > 9 ? 9 : len;
        memcpy(dst, k_nop_table[chunk], chunk);
        dst += chunk;
        len -= chunk;
    }
}

static bool is_nop_patch(const u8* ip, const u8* end) {
    for (u32 len = 4; len <= 9; len++) {
        if (ip + len <= end && memcmp(ip, k_nop_table[len], len) == 0)
            return true;
    }
    return false;
}

// Rewrites a CLWB at `ip` to an exact-length NOP (or finds another thread already did).
static bool patch_clwb(u8* ip, const PeImage* image) {
    const u8* end = image->base + image->size;
    AcquireSRWLockExclusive(&g_patch_lock);
    bool ok = false;
    u32 len = clwb_length(ip, end);
    if (len != 0) {
        u8 nop[15];
        fill_nop(nop, len);
        ok = patch_code(ip, nop, len);
        if (ok && !InterlockedExchange(&g_reported_clwb, 1))
            LOG_I("cpu", "host CPU lacks CLWB: patching it to NOP, first at %s+0x%x", image->name,
                  image->rva_of(ip));
    } else {
        ok = is_nop_patch(ip, end);
    }
    ReleaseSRWLockExclusive(&g_patch_lock);
    return ok;
}

static LONG CALLBACK compat_handler(EXCEPTION_POINTERS* ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ILLEGAL_INSTRUCTION)
        return EXCEPTION_CONTINUE_SEARCH;
    // Only title code: its bytes are mapped and readable, and nothing else is built for the console.
    u8* ip = (u8*)ep->ContextRecord->Rip;
    const PeImage* image = g_loader.find_containing(ip);
    if (!image || ip + 3 > image->base + image->size)
        return EXCEPTION_CONTINUE_SEARCH;
    if (g_emulate_sse4a) {
        if (u32 length = emulate_sse4a(ip, image->base + image->size, ep->ContextRecord)) {
            LONG64 traps = InterlockedIncrement64(&g_sse4a_traps);
            if (!InterlockedExchange(&g_reported_sse4a, 1))
                LOG_I("cpu", "host CPU lacks SSE4a: emulating it, first at %s+0x%x", image->name,
                      image->rva_of(ip));
            if ((traps & (traps - 1)) == 0 && traps >= 4096) // how hot it is, at powers of two
                LOG_I("cpu", "%lld SSE4a instructions emulated so far", traps);
            ep->ContextRecord->Rip += length;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    if (g_emulate_clzero && is_clzero(ip)) {
        if (!InterlockedExchange(&g_reported, 1))
            LOG_I("cpu", "host CPU lacks CLZERO: emulating it, first at %s+0x%x", image->name,
                  image->rva_of(ip));
        return run_clzero_loop(ep, image);
    }
    if (g_patch_clwb && patch_clwb(ip, image))
        return EXCEPTION_CONTINUE_EXECUTION;
    if (g_patch_monitorx && patch_wait_instruction(ip))
        return EXCEPTION_CONTINUE_EXECUTION;
    return EXCEPTION_CONTINUE_SEARCH;
}

void cpu_compat_init() {
    g_emulate_clzero = !host_has_clzero();
    g_patch_monitorx = !host_has_monitorx();
    g_patch_clwb = !host_has_clwb();
    g_emulate_sse4a = !host_has_sse4a();
    if (!g_emulate_clzero && !g_patch_monitorx && !g_patch_clwb && !g_emulate_sse4a)
        return;
    LOG_I("cpu", "host CPU lacks%s%s%s%s; handling them for console code",
          g_emulate_clzero ? " CLZERO" : "", g_patch_monitorx ? " MONITORX/MWAITX" : "",
          g_patch_clwb ? " CLWB" : "", g_emulate_sse4a ? " SSE4a" : "");
    if (!AddVectoredExceptionHandler(1, compat_handler))
        LOG_E("cpu", "could not install the CPU compatibility handler");
}
