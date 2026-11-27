// shader_tool: console shader code analysis.
//
//   shader_tool disasm <code.bin>          disassemble raw GPU code
//   shader_tool stats <dir> [name filter]  instruction statistics over the unique programs in a dump
//                                          directory; lists instructions that fail to decode
//   shader_tool translate <dir> <pipeline> <vs|ps>   translate one stage of a dumped pipeline to HLSL
//   shader_tool translate-all <dir> <out dir>        translate every unique program of a dump, write
//                                                    the HLSL and a summary of failures
//   shader_tool retranslate <dir> <out dir> [-v]     the same for a runtime dump (SLUDGE_SHADER_DUMP:
//                                                    <key>.bin code and <key>.info); HLSL goes to
//                                                    <key>_vs/ps/cs.hlsl; -v lists every failure
//
// Translations are for a desktop device running compute waves of 32 lanes; --wave <min> <max> (before
// the command) names another range of wave sizes.
//
// Dumps hold, per pipeline pNNNNN: _g_packet.bin, _g_meta.bin, _rs.txt (root signature as text) and
// _vs_code.bin / _ps_code.bin.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "shader/console_pipeline.h"
#include "shader/rdna.h"
#include "shader/translate.h"

static ShaderTarget g_target = {32, 32};

static u8* read_file(const char* path, u32* size) {
    FILE* f = fopen(path, "rb");
    if (!f)
        return nullptr;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    u8* data = (u8*)malloc((usize)n + 4);
    if (data && fread(data, 1, (usize)n, f) != (usize)n) {
        free(data);
        data = nullptr;
    }
    fclose(f);
    *size = (u32)n;
    return data;
}

static int disasm(const char* path) {
    u32 size = 0;
    u8* data = read_file(path, &size);
    if (!data) {
        fprintf(stderr, "cannot read %s\n", path);
        return 1;
    }
    const u32* code = (const u32*)data;
    u32 dwords = size / 4;
    u32 offset = 0;
    char text[256];
    while (offset / 4 < dwords) {
        RdnaInst in;
        if (!rdna_decode(code, dwords, offset, &in)) {
            printf("  %06X: %08X  <invalid>\n", offset, code[offset / 4]);
            offset += 4;
            continue;
        }
        if (in.op == OP_S_CODE_END)
            break;
        rdna_disassemble(in, text, sizeof(text));
        char words[40] = {};
        int n = 0;
        for (u32 i = 0; i < in.dwords && i < 3; i++)
            n += snprintf(words + n, sizeof(words) - n, "%08X ", code[offset / 4 + i]);
        printf("  %06X: %-27s %s\n", offset, words, text);
        offset += in.dwords * 4u;
    }
    free(data);
    return 0;
}

// --- stats -------------------------------------------------------------------------------------------

static u64 hash_bytes(const u8* p, u32 n) {
    u64 h = 0xcbf29ce484222325ull;
    for (u32 i = 0; i < n; i++) {
        h ^= p[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

static int stats(const char* dir, const char* filter) {
    static u32 counts[OP_COUNT];
    static char first_file[OP_COUNT][64];
    static u64 seen[1 << 16];
    u32 programs = 0, unique = 0, failures = 0;
    char pattern[MAX_PATH];
    snprintf(pattern, sizeof(pattern), "%s\\*%s*", dir, filter ? filter : "");
    WIN32_FIND_DATAA fd;
    HANDLE find = FindFirstFileA(pattern, &fd);
    if (find == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "no files match %s\n", pattern);
        return 1;
    }
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        char path[MAX_PATH];
        snprintf(path, sizeof(path), "%s\\%s", dir, fd.cFileName);
        u32 size = 0;
        u8* data = read_file(path, &size);
        if (!data)
            continue;
        programs++;
        u64 h = hash_bytes(data, size) | 1;
        u32 slot = (u32)h & 0xFFFF;
        bool fresh = true;
        while (seen[slot]) {
            if (seen[slot] == h) {
                fresh = false;
                break;
            }
            slot = (slot + 1) & 0xFFFF;
        }
        if (fresh) {
            seen[slot] = h;
            unique++;
            const u32* code = (const u32*)data;
            u32 dwords = size / 4;
            for (u32 offset = 0; offset / 4 < dwords;) {
                RdnaInst in;
                if (!rdna_decode(code, dwords, offset, &in)) {
                    if (failures++ < 40)
                        printf("decode failure: %s+%X: %08X %08X\n", fd.cFileName, offset, code[offset / 4],
                               offset / 4 + 1 < dwords ? code[offset / 4 + 1] : 0);
                    break;
                }
                if (in.op == OP_S_CODE_END)
                    break;
                if (!counts[in.op]++)
                    snprintf(first_file[in.op], sizeof(first_file[in.op]), "%s+%X", fd.cFileName, offset);
                offset += in.dwords * 4u;
            }
        }
        free(data);
    } while (FindNextFileA(find, &fd));
    FindClose(find);
    printf("%u programs, %u unique, %u decode failures\n", programs, unique, failures);
    // Most used first.
    for (;;) {
        u32 best = 0;
        for (u32 op = 1; op < OP_COUNT; op++)
            if (counts[op] > counts[best])
                best = op;
        if (!counts[best])
            break;
        printf("%8u  %-28s %s\n", counts[best], rdna_op_name((RdnaOp)best), first_file[best]);
        counts[best] = 0;
    }
    return 0;
}

// --- translation -------------------------------------------------------------------------------------

// The root signature dump: "flags F params N ..." then "param i type T vis V [constants N ...] [range
// type count reg space offset]...".
static bool read_root_layout(const char* path, ConsoleRootLayout* layout) {
    memset(layout, 0, sizeof(*layout));
    FILE* f = fopen(path, "r");
    if (!f)
        return false;
    char line[2048];
    unsigned flags = 0, params = 0;
    if (!fgets(line, sizeof(line), f) || sscanf(line, "flags %x params %u", &flags, &params) != 2) {
        fclose(f);
        return false;
    }
    layout->flags = flags;
    layout->parameter_count = (u8)params;
    for (u32 i = 0; i < params && fgets(line, sizeof(line), f); i++) {
        unsigned index = 0, type = 0, vis = 0;
        int used = 0;
        if (sscanf(line, "param %u type %u vis %u%n", &index, &type, &vis, &used) != 3 ||
            index >= SHADER_MAX_ROOT_PARAMETERS)
            break;
        ConsoleRootParameter& p = layout->parameters[index];
        const char* rest = line + used;
        switch (type) {
        case 0: { // table
            p.type = ROOT_TABLE;
            int n = 0;
            unsigned rt, count, reg, space, offset;
            while (sscanf(rest, " range %u %u %u %u %u%n", &rt, &count, &reg, &space, &offset, &n) == 5 &&
                   p.range_count < SHADER_MAX_TABLE_RANGES) {
                if (rt == 3)
                    p.sampler_table = true;
                TableRange& r = p.ranges[p.range_count++];
                r.kind = rt == 0 ? RANGE_SRV : rt == 1 ? RANGE_UAV : RANGE_CBV;
                r.count = count;
                r.offset = offset;
                rest += n;
            }
            break;
        }
        case 1: {
            unsigned n = 0;
            sscanf(rest, " constants %u", &n);
            p.type = ROOT_CONSTANTS;
            p.constants = (u8)n;
            break;
        }
        case 2:
            p.type = ROOT_CBV;
            break;
        case 3:
            p.type = ROOT_SRV;
            break;
        default:
            p.type = ROOT_UAV;
            break;
        }
    }
    fclose(f);
    return true;
}

// ShaderInfo for one stage of a dumped graphics pipeline.
static bool pipeline_info(const char* dir, const char* name, bool pixel, ShaderInfo* info) {
    char path[MAX_PATH];
    u32 packet_size = 0, meta_size = 0;
    snprintf(path, sizeof(path), "%s\\%s_g_packet.bin", dir, name);
    u8* packet = read_file(path, &packet_size);
    snprintf(path, sizeof(path), "%s\\%s_g_meta.bin", dir, name);
    u8* meta = read_file(path, &meta_size);
    ConsoleRootLayout layout;
    snprintf(path, sizeof(path), "%s\\%s_rs.txt", dir, name);
    bool ok = packet && meta && read_root_layout(path, &layout);
    ConsoleGraphicsShaders g = {};
    ok = ok && console_parse_graphics(packet, packet_size, meta, meta_size, &g);
    if (ok) {
        memset(info, 0, sizeof(*info));
        info->stage = pixel ? SHADER_PIXEL : SHADER_VERTEX;
        console_user_data_layout(layout, CONSOLE_FIRST_ROOT_SGPR_GRAPHICS, info);
        info->user_sgprs = pixel ? g.ps_user_sgprs : g.vs_user_sgprs;
        info->ps_input_addr = g.ps_input_ena;
        info->ps_color_format = g.ps_color_format;
        info->element_count = g.element_count;
        memcpy(info->elements, g.elements, sizeof(g.elements));
    }
    free(packet);
    free(meta);
    return ok;
}

static int translate_one(const char* dir, const char* name, const char* stage) {
    bool pixel = strcmp(stage, "ps") == 0;
    ShaderInfo info;
    if (!pipeline_info(dir, name, pixel, &info)) {
        fprintf(stderr, "cannot read pipeline %s\n", name);
        return 1;
    }
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\%s_%s_code.bin", dir, name, stage);
    u32 size = 0;
    u8* code = read_file(path, &size);
    if (!code) {
        fprintf(stderr, "cannot read %s\n", path);
        return 1;
    }
    TranslatedShader out = {};
    bool ok = shader_translate(code, size, info, g_target, &out);
    if (out.hlsl.data)
        fputs(out.hlsl.data, stdout);
    if (!ok)
        fprintf(stderr, "translation failed: %s\n", out.error);
    out.hlsl.release();
    free(code);
    return ok ? 0 : 1;
}

struct FailureCount {
    char reason[160];
    u32 count;
    char example[64];
};

static int translate_all(const char* dir, const char* out_dir) {
    static u64 seen[1 << 16];
    static FailureCount failures[512];
    u32 failure_kinds = 0, programs = 0, translated = 0;
    char pattern[MAX_PATH];
    snprintf(pattern, sizeof(pattern), "%s\\p*_g_packet.bin", dir);
    WIN32_FIND_DATAA fd;
    HANDLE find = FindFirstFileA(pattern, &fd);
    if (find == INVALID_HANDLE_VALUE)
        return 1;
    CreateDirectoryA(out_dir, nullptr);
    TranslatedShader out = {};
    do {
        char name[16] = {};
        memcpy(name, fd.cFileName, 6);
        for (u32 stage = 0; stage < 2; stage++) {
            const char* stage_name = stage ? "ps" : "vs";
            char path[MAX_PATH];
            snprintf(path, sizeof(path), "%s\\%s_%s_code.bin", dir, name, stage_name);
            u32 size = 0;
            u8* code = read_file(path, &size);
            if (!code)
                continue;
            u64 h = hash_bytes(code, size) ^ (stage ? 0x9e3779b97f4a7c15ull : 0) | 1;
            u32 slot = (u32)h & 0xFFFF;
            bool fresh = true;
            while (seen[slot]) {
                if (seen[slot] == h) {
                    fresh = false;
                    break;
                }
                slot = (slot + 1) & 0xFFFF;
            }
            ShaderInfo info;
            if (!fresh || !pipeline_info(dir, name, stage == 1, &info)) {
                free(code);
                continue;
            }
            seen[slot] = h;
            programs++;
            if (shader_translate(code, size, info, g_target, &out)) {
                translated++;
                snprintf(path, sizeof(path), "%s\\%s_%s.hlsl", out_dir, name, stage_name);
                FILE* f = fopen(path, "w");
                if (f) {
                    fputs(out.hlsl.data, f);
                    fclose(f);
                }
            } else {
                // Group by reason, ignoring offsets.
                char reason[160];
                snprintf(reason, sizeof(reason), "%s", out.error);
                char* at = strstr(reason, " at 0x");
                if (at)
                    *at = 0;
                u32 k = 0;
                while (k < failure_kinds && strcmp(failures[k].reason, reason) != 0)
                    k++;
                if (k == failure_kinds && failure_kinds < 512) {
                    snprintf(failures[k].reason, sizeof(failures[k].reason), "%s", reason);
                    snprintf(failures[k].example, sizeof(failures[k].example), "%s_%s", name, stage_name);
                    failure_kinds++;
                }
                if (k < failure_kinds)
                    failures[k].count++;
            }
            free(code);
        }
    } while (FindNextFileA(find, &fd));
    FindClose(find);
    out.hlsl.release();
    printf("%u unique programs, %u translated\n", programs, translated);
    for (;;) {
        u32 best = ~0u;
        for (u32 k = 0; k < failure_kinds; k++)
            if (failures[k].count && (best == ~0u || failures[k].count > failures[best].count))
                best = k;
        if (best == ~0u)
            break;
        printf("%6u  %-90s %s\n", failures[best].count, failures[best].reason, failures[best].example);
        failures[best].count = 0;
    }
    return 0;
}

// Translates every program of a runtime dump again: code from <key>.bin, what the translator was told
// from <key>.info (a ShaderInfo of this build).
static int retranslate(const char* dir, const char* out_dir, bool verbose) {
    static FailureCount failures[512];
    u32 failure_kinds = 0, programs = 0, translated = 0;
    char pattern[MAX_PATH];
    snprintf(pattern, sizeof(pattern), "%s\\*.info", dir);
    WIN32_FIND_DATAA fd;
    HANDLE find = FindFirstFileA(pattern, &fd);
    if (find == INVALID_HANDLE_VALUE)
        return 1;
    CreateDirectoryA(out_dir, nullptr);
    TranslatedShader out = {};
    do {
        char key[32] = {};
        snprintf(key, sizeof(key), "%.16s", fd.cFileName);
        char path[MAX_PATH];
        snprintf(path, sizeof(path), "%s\\%s.info", dir, key);
        u32 info_size = 0, size = 0;
        u8* info_bytes = read_file(path, &info_size);
        snprintf(path, sizeof(path), "%s\\%s.bin", dir, key);
        u8* code = read_file(path, &size);
        if (!info_bytes || !code || info_size != sizeof(ShaderInfo)) {
            if (info_bytes && info_size != sizeof(ShaderInfo))
                fprintf(stderr, "%s: dumped by another build (info of %u bytes)\n", key, info_size);
            free(info_bytes);
            free(code);
            continue;
        }
        ShaderInfo info;
        memcpy(&info, info_bytes, sizeof(info));
        free(info_bytes);
        programs++;
        const char* stage = info.stage == SHADER_VERTEX ? "vs" : info.stage == SHADER_PIXEL ? "ps" : "cs";
        if (shader_translate(code, size, info, g_target, &out)) {
            translated++;
            snprintf(path, sizeof(path), "%s\\%s_%s.hlsl", out_dir, key, stage);
            FILE* f = fopen(path, "w");
            if (f) {
                fputs(out.hlsl.data, f);
                fclose(f);
            }
        } else {
            if (verbose)
                printf("%s_%s: %s\n", key, stage, out.error);
            char reason[160];
            snprintf(reason, sizeof(reason), "%s", out.error);
            char* at = strstr(reason, " at 0x");
            if (at)
                *at = 0;
            u32 k = 0;
            while (k < failure_kinds && strcmp(failures[k].reason, reason) != 0)
                k++;
            if (k == failure_kinds && failure_kinds < 512) {
                snprintf(failures[k].reason, sizeof(failures[k].reason), "%s", reason);
                snprintf(failures[k].example, sizeof(failures[k].example), "%s_%s", key, stage);
                failure_kinds++;
            }
            if (k < failure_kinds)
                failures[k].count++;
        }
        free(code);
    } while (FindNextFileA(find, &fd));
    FindClose(find);
    out.hlsl.release();
    printf("%u programs, %u translated\n", programs, translated);
    for (;;) {
        u32 best = ~0u;
        for (u32 k = 0; k < failure_kinds; k++)
            if (failures[k].count && (best == ~0u || failures[k].count > failures[best].count))
                best = k;
        if (best == ~0u)
            break;
        printf("%6u  %-90s %s\n", failures[best].count, failures[best].reason, failures[best].example);
        failures[best].count = 0;
    }
    return 0;
}

int main(int argc, char** argv) {
    rdna_init();
    if (argc >= 4 && strcmp(argv[1], "--wave") == 0) {
        g_target.wave_min = (u8)atoi(argv[2]);
        g_target.wave_max = (u8)atoi(argv[3]);
        argv += 3;
        argc -= 3;
    }
    if (argc >= 3 && strcmp(argv[1], "disasm") == 0)
        return disasm(argv[2]);
    if (argc >= 5 && strcmp(argv[1], "translate") == 0)
        return translate_one(argv[2], argv[3], argv[4]);
    if (argc >= 4 && strcmp(argv[1], "translate-all") == 0)
        return translate_all(argv[2], argv[3]);
    if (argc >= 3 && strcmp(argv[1], "stats") == 0)
        return stats(argv[2], argc >= 4 ? argv[3] : nullptr);
    if (argc >= 4 && strcmp(argv[1], "retranslate") == 0)
        return retranslate(argv[2], argv[3], argc >= 5 && strcmp(argv[4], "-v") == 0);
    fprintf(stderr,
            "usage: shader_tool [--wave <min> <max>] disasm <code.bin> | stats <dir> [filter] | "
            "translate <dir> <pipeline> <vs|ps> | translate-all <dir> <out> | retranslate <dir> <out>\n");
    return 1;
}
