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
//                                                    <key>_vs/ps/cs/ms.hlsl; -v lists every failure
//   shader_tool pipelines <dir>                      what the graphics packets of a dump say: stages,
//                                                    user SGPRs, clip and cull distances, mesh shapes
//
// Translations are for a desktop device running compute waves of 32 lanes; --wave <min> <max> (before
// the command) names another range of wave sizes.
//
// Dumps hold, per pipeline pNNNNN: _g_packet.bin, _g_meta.bin, _rs.txt (root signature as text) and
// _vs_code.bin / _ps_code.bin. `pipelines` also reads dumps naming pipelines pNNN_mSS (SS: the stages
// with code, a bit per stage in the order PS, VS, GS, ES, HS, LS, FS) with _packet.bin, _meta.bin and
// _stage<i>_code.bin.

#include <stdarg.h>
#include <stddef.h>
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
        info->vs_out_cntl = pixel ? 0 : g.vs_out_cntl;
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

// A translation error without the instruction offsets in it (" at 0x1234"), so that failures group by
// what they are.
static void failure_reason(const char* error, char* out, usize size) {
    usize n = 0;
    for (const char* p = error; *p && n + 1 < size;) {
        if (!strncmp(p, " at 0x", 6)) {
            p += 6;
            while ((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F'))
                p++;
            continue;
        }
        out[n++] = *p++;
    }
    out[n] = 0;
}

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
            u64 h = (hash_bytes(code, size) ^ (stage ? 0x9e3779b97f4a7c15ull : 0)) | 1;
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
                failure_reason(out.error, reason, sizeof(reason));
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
// from <key>.info (a ShaderInfo of this build). Each failure's error goes to <out dir>.failures.txt (and
// with `verbose` to the console), and the inline samplers of the programs that translate (which the HLSL
// doesn't show) to <out dir>.samplers.txt.
static int retranslate(const char* dir, const char* out_dir, bool verbose) {
    static FailureCount failures[512];
    u32 failure_kinds = 0, programs = 0, translated = 0, older_infos = 0;
    char pattern[MAX_PATH];
    snprintf(pattern, sizeof(pattern), "%s\\*.info", dir);
    WIN32_FIND_DATAA fd;
    HANDLE find = FindFirstFileA(pattern, &fd);
    if (find == INVALID_HANDLE_VALUE)
        return 1;
    CreateDirectoryA(out_dir, nullptr);
    char list_path[MAX_PATH];
    snprintf(list_path, sizeof(list_path), "%s.failures.txt", out_dir);
    FILE* list = fopen(list_path, "w");
    snprintf(list_path, sizeof(list_path), "%s.samplers.txt", out_dir);
    FILE* sampler_list = fopen(list_path, "w");
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
        // Fields are only ever appended to ShaderInfo: older builds' dumps are shorter, and what they
        // lack is zero.
        if (!info_bytes || !code || info_size < offsetof(ShaderInfo, flat_remap) ||
            info_size > sizeof(ShaderInfo)) {
            if (info_bytes && info_size > sizeof(ShaderInfo))
                fprintf(stderr, "%s: dumped by a newer build (info of %u bytes)\n", key, info_size);
            free(info_bytes);
            free(code);
            continue;
        }
        if (info_size < sizeof(ShaderInfo))
            older_infos++;
        ShaderInfo info;
        memset(&info, 0, sizeof(info));
        memcpy(&info, info_bytes, info_size);
        free(info_bytes);
        programs++;
        static const char* const k_stage_names[4] = {"vs", "ps", "cs", "ms"};
        const char* stage = info.stage < 4 ? k_stage_names[info.stage] : "unknown";
        if (shader_translate(code, size, info, g_target, &out)) {
            translated++;
            snprintf(path, sizeof(path), "%s\\%s_%s.hlsl", out_dir, key, stage);
            FILE* f = fopen(path, "w");
            if (f) {
                fputs(out.hlsl.data, f);
                fclose(f);
            }
            for (u32 i = 0; i < out.sampler_count && sampler_list; i++) {
                const InlineSampler& smp = out.samplers[i];
                fprintf(
                    sampler_list,
                    "%s_%s: s%u filter 0x%x address %u %u %u aniso %u compare %u border %u unnormalized %u "
                    "bias %g lod %g..%g\n",
                    key, stage, i, smp.filter, smp.address[0], smp.address[1], smp.address[2],
                    smp.max_anisotropy, smp.comparison, smp.border_color, smp.unnormalized ? 1 : 0,
                    smp.mip_bias, smp.min_lod, smp.max_lod);
            }
        } else {
            if (list)
                fprintf(list, "%s_%s: %s\n", key, stage, out.error);
            if (verbose)
                printf("%s_%s: %s\n", key, stage, out.error);
            char reason[160];
            failure_reason(out.error, reason, sizeof(reason));
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
    if (list)
        fclose(list);
    if (sampler_list)
        fclose(sampler_list);
    out.hlsl.release();
    printf("%u programs, %u translated\n", programs, translated);
    if (older_infos)
        printf("%u programs dumped by an older build (what their info lacks is zero)\n", older_infos);
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

// --- pipelines ---------------------------------------------------------------------------------------

// Distinct values (as text) with how many pipelines have each, for the pipelines report.
struct Tally {
    const char* title;
    u32 used;
    u32 dropped; // values past the table's capacity
    struct Entry {
        char text[128];
        char example[40];
        u32 count;
    } entries[256];
};

static void tally(Tally* t, const char* example, const char* format, ...) {
    char text[128];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    u32 i = 0;
    while (i < t->used && strcmp(t->entries[i].text, text) != 0)
        i++;
    if (i == t->used) {
        if (t->used == ARRAY_COUNT(t->entries)) {
            t->dropped++;
            return;
        }
        Tally::Entry& e = t->entries[t->used++];
        snprintf(e.text, sizeof(e.text), "%s", text);
        snprintf(e.example, sizeof(e.example), "%s", example);
        e.count = 0;
    }
    t->entries[i].count++;
}

// Most common first; empties the tally.
static void print_tally(Tally* t) {
    if (!t->used)
        return;
    printf("\n%s:\n", t->title);
    for (;;) {
        u32 best = ~0u;
        for (u32 i = 0; i < t->used; i++)
            if (t->entries[i].count && (best == ~0u || t->entries[i].count > t->entries[best].count))
                best = i;
        if (best == ~0u)
            break;
        printf("%8u  %-90s %s\n", t->entries[best].count, t->entries[best].text, t->entries[best].example);
        t->entries[best].count = 0;
    }
    if (t->dropped)
        printf("%8u  (more distinct values than listed)\n", t->dropped);
}

static bool file_exists(const char* path) {
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

// Which hardware stages of a dumped pipeline have code (a bit per stage: PS, VS, GS, ES, HS, LS, FS).
// `base` is the pipeline's name: pNNNNN for _g_packet.bin dumps (vertex and pixel shaders only), pNNN_mSS
// for _packet.bin dumps.
static u32 dumped_stages(const char* dir, const char* base, bool g_dump) {
    char path[MAX_PATH];
    u32 mask = 0;
    if (g_dump) {
        static const char* const k_names[2] = {"ps", "vs"};
        for (u32 i = 0; i < 2; i++) {
            snprintf(path, sizeof(path), "%s\\%s_%s_code.bin", dir, base, k_names[i]);
            if (file_exists(path))
                mask |= 1u << i;
        }
        return mask;
    }
    for (u32 i = 0; i < 7; i++) {
        snprintf(path, sizeof(path), "%s\\%s_stage%u_code.bin", dir, base, i);
        if (file_exists(path))
            mask |= 1u << i;
    }
    return mask;
}

static u32 packet_u32(const u8* packet, u32 size, u32 offset) {
    u32 v = 0;
    if (offset + 4 <= size)
        memcpy(&v, packet + offset, 4);
    return v;
}

static int pipelines(const char* dir) {
    static Tally failures, configs, stages, swaps, vertex_out, mesh_shapes, mesh_lds, mesh_unknown,
        geometry_other;
    failures.title = "not parsed";
    configs.title = "configurations (PACKET_CONFIG bits 1:0)";
    stages.title = "stages with code";
    swaps.title = "vertex pipelines whose +60 and +80 user SGPR counts differ";
    vertex_out.title = "PA_CL_VS_OUT_CNTL by kind of pipeline";
    mesh_shapes.title =
        "mesh pipelines: threads, vertices, primitives, user SGPRs (GS, PS), PA_CL_VS_OUT_CNTL, "
        "topology type";
    mesh_lds.title = "mesh pipelines: LDS bytes";
    mesh_unknown.title = "mesh pipelines: VGT_GS_ONCHIP_CNTL and packet dwords +116 +120 +168";
    geometry_other.title = "geometry-stage pipelines that aren't mesh pipelines (stages, input elements)";
    char pattern[MAX_PATH];
    snprintf(pattern, sizeof(pattern), "%s\\*_packet.bin", dir);
    WIN32_FIND_DATAA fd;
    HANDLE find = FindFirstFileA(pattern, &fd);
    if (find == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "no files match %s\n", pattern);
        return 1;
    }
    u32 total = 0, parsed = 0, mesh = 0, vs_changed = 0, ps_changed = 0, with_vs = 0, with_ps = 0;
    do {
        // pNNNNN_g_packet.bin or pNNN_mSS_packet.bin; compute pipelines (_c_packet.bin) are skipped.
        char base[64];
        usize n = strlen(fd.cFileName) - strlen("_packet.bin");
        if (n < 2 || n >= sizeof(base))
            continue;
        memcpy(base, fd.cFileName, n);
        base[n] = 0;
        if (strcmp(base + n - 2, "_c") == 0)
            continue;
        bool g_dump = strcmp(base + n - 2, "_g") == 0;
        if (g_dump)
            base[n - 2] = 0;
        char path[MAX_PATH];
        u32 packet_size = 0, meta_size = 0;
        snprintf(path, sizeof(path), "%s\\%s", dir, fd.cFileName);
        u8* packet = read_file(path, &packet_size);
        snprintf(path, sizeof(path), g_dump ? "%s\\%s_g_meta.bin" : "%s\\%s_meta.bin", dir, base);
        u8* meta = read_file(path, &meta_size);
        total++;
        ConsoleGraphicsShaders g = {};
        if (!packet || !meta || !console_parse_graphics(packet, packet_size, meta, meta_size, &g)) {
            tally(&failures, base, "%s", !packet || !meta ? "packet or metadata missing" : g.error);
            free(packet);
            free(meta);
            continue;
        }
        parsed++;
        u32 mask = dumped_stages(dir, base, g_dump);
        bool has_ps = mask & 1, has_vs = (mask >> 1) & 1, has_gs = (mask >> 2) & 1;
        tally(&configs, base, "config %u", g.config);
        tally(&stages, base, "%s%s%s%s", has_ps ? "PS " : "", has_vs ? "VS " : "", has_gs ? "GS " : "",
              mask & ~7u ? "others" : "");
        // Vertex pipelines were read with +60 as the pixel shader's user SGPR count and +80 as the vertex
        // shader's before: only those where the two differ change.
        u32 at60 = (packet_u32(packet, packet_size, PACKET_VS_RSRC2) >> 1) & 31;
        u32 at80 = (packet_u32(packet, packet_size, PACKET_PS_RSRC2) >> 1) & 31;
        with_vs += has_vs && !g.geometry;
        with_ps += has_ps && !g.geometry;
        if (at60 != at80 && !g.geometry) {
            vs_changed += has_vs;
            ps_changed += has_ps;
            tally(&swaps, base, "+60 %2u +80 %2u, %s%s%s", at60, at80, has_vs ? "VS " : "",
                  has_ps ? "PS " : "", has_gs ? "GS" : "");
        }
        bool is_mesh = console_mesh_pipeline(g, has_gs, (mask & ~5u) != 0);
        tally(&vertex_out, base, "%-10s 0x%08x",
              is_mesh      ? "mesh"
              : g.geometry ? "geometry"
                           : "vertex",
              g.vs_out_cntl);
        if (is_mesh) {
            mesh++;
            tally(&mesh_shapes, base, "%3u %3u %3u  gs %2u ps %2u  0x%08x  topology %u", g.mesh_threads,
                  g.mesh_max_vertices, g.mesh_max_primitives, g.gs_user_sgprs, g.ps_user_sgprs, g.vs_out_cntl,
                  g.primitive_topology_type);
            tally(&mesh_lds, base, "%6u", g.mesh_lds_bytes);
            tally(&mesh_unknown, base, "0x%08x  0x%08x 0x%08x 0x%08x", g.gs_onchip, g.gs_unknown[0],
                  g.gs_unknown[1], g.gs_unknown[2]);
        } else if (g.geometry) {
            tally(&geometry_other, base, "stages 0x%02x, %u input elements", mask, g.element_count);
        }
        free(packet);
        free(meta);
    } while (FindNextFileA(find, &fd));
    FindClose(find);
    printf("%u graphics pipelines, %u parsed, %u mesh pipelines\n", total, parsed, mesh);
    printf("vertex pipelines' user SGPR counts that changed register (+60 and +80 differ): %u of %u vertex "
           "shaders, %u of %u pixel shaders\n",
           vs_changed, with_vs, ps_changed, with_ps);
    Tally* all[] = {&failures,    &configs,  &stages,       &swaps,         &vertex_out,
                    &mesh_shapes, &mesh_lds, &mesh_unknown, &geometry_other};
    for (u32 i = 0; i < ARRAY_COUNT(all); i++)
        print_tally(all[i]);
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
    if (argc >= 3 && strcmp(argv[1], "pipelines") == 0)
        return pipelines(argv[2]);
    fprintf(stderr,
            "usage: shader_tool [--wave <min> <max>] disasm <code.bin> | stats <dir> [filter] | "
            "translate <dir> <pipeline> <vs|ps> | translate-all <dir> <out> | retranslate <dir> <out> | "
            "pipelines <dir>\n");
    return 1;
}
