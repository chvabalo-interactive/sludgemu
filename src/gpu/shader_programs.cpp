#include "gpu/shader_programs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include <ole2.h> // dxcapi.h needs COM types the lean Windows headers leave out
#include <dxcapi.h>

#include "common/log.h"
#include "common/thread_slot.h"
#include "shader/rdna.h"

// --- jobs --------------------------------------------------------------------------------------------

// A job is a program to build or a function to run.
struct Job {
    ShaderProgram* program;
    void (*run)(void* context);
    void* context;
};

// Two lanes: work a draw is waiting for, served first, and programs titles merely created (often
// thousands at once, at startup).
#define JOB_CAPACITY 65536

struct JobLane {
    Job* jobs;
    u32 head, count;
};

static JobLane g_lanes[2]; // urgent, background
static SRWLOCK g_job_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE g_job_ready = CONDITION_VARIABLE_INIT;
static bool g_ready;

static bool queue(const Job& job, bool urgent) {
    AcquireSRWLockExclusive(&g_job_lock);
    JobLane& lane = g_lanes[urgent ? 0 : 1];
    if (lane.count == JOB_CAPACITY) {
        ReleaseSRWLockExclusive(&g_job_lock);
        LOG_E("gpu", "GPU job queue full; job dropped");
        return false;
    }
    lane.jobs[(lane.head + lane.count) % JOB_CAPACITY] = job;
    lane.count++;
    ReleaseSRWLockExclusive(&g_job_lock);
    WakeConditionVariable(&g_job_ready);
    return true;
}

void gpu_job_queue(void (*run)(void* context), void* context) {
    queue({nullptr, run, context}, true);
}

void shader_program_hurry(ShaderProgram* p) {
    if (p && p->state == PROGRAM_PENDING && InterlockedExchange(&p->urgent, 1) == 0)
        queue({p, nullptr, nullptr}, true);
}

// --- compiler ----------------------------------------------------------------------------------------

static DxcCreateInstanceProc g_dxc_create;
static ShaderTarget g_target; // what the device runs, for the translator

// Each worker has its own compiler: DXC compiler objects are not thread safe.
struct Worker {
    IDxcCompiler3* compiler;
    u32 index;
};

static const wchar_t* profile_of(ShaderStage stage) {
    switch (stage) {
    case SHADER_VERTEX:
        return L"vs_6_6";
    case SHADER_PIXEL:
        return L"ps_6_6";
    default:
        return L"cs_6_6";
    }
}

// Programs (code, what the translator was told about them, and HLSL with the error for failures) are
// written to SLUDGE_SHADER_DUMP=<directory>; shader_tool retranslate translates them again.
static void dump(const ShaderProgram* p, const char* hlsl, const char* reason) {
    char dir[MAX_PATH];
    if (!GetEnvironmentVariableA("SLUDGE_SHADER_DUMP", dir, sizeof(dir)))
        return;
    char path[MAX_PATH + 64];
    snprintf(path, sizeof(path), "%s\\%016llx.bin", dir, (unsigned long long)p->key);
    FILE* f = fopen(path, "wb");
    if (f) {
        fwrite(p->code, 1, p->code_size, f);
        fclose(f);
    }
    snprintf(path, sizeof(path), "%s\\%016llx.info", dir, (unsigned long long)p->key);
    f = fopen(path, "wb");
    if (f) {
        fwrite(&p->info, sizeof(p->info), 1, f);
        fclose(f);
    }
    snprintf(path, sizeof(path), "%s\\%016llx.hlsl", dir, (unsigned long long)p->key);
    f = fopen(path, "w");
    if (f) {
        if (reason)
            fprintf(f, "// %s\n", reason);
        if (hlsl)
            fputs(hlsl, f);
        fclose(f);
    }
}

static void build_program(Worker& w, ShaderProgram* p) {
    // Programs can be queued in both lanes; the first worker to get one builds it.
    if (InterlockedCompareExchange((volatile LONG*)&p->state, PROGRAM_BUILDING, PROGRAM_PENDING) !=
        PROGRAM_PENDING)
        return;
    TranslatedShader t = {};
    u32 state = PROGRAM_FAILED;
    bool translated = shader_translate(p->code, p->code_size, p->info, g_target, &t);
    // The pipeline's wave size (unverified) against what the code proves, where both tell; the code wins.
    if (p->info.wave_size && t.code_wave_size && t.code_wave_size != p->info.wave_size)
        LOG_W("gpu", "shader %016llx: launched as wave%u, but its code is wave%u", (unsigned long long)p->key,
              p->info.wave_size, t.code_wave_size);
    if (!translated) {
        LOG_W("gpu", "shader %016llx: translation failed: %s", (unsigned long long)p->key, t.error);
        dump(p, t.hlsl.data, t.error);
    } else if (t.uses_int64_atomics && !gpu_int64_image_atomics()) {
        // The pipeline would fail to create (or worse); the adapter's lack was logged at startup.
        LOG_W("gpu", "shader %016llx: needs 64-bit atomics on typed views, which the adapter lacks",
              (unsigned long long)p->key);
        dump(p, t.hlsl.data, "needs 64-bit atomics on typed views");
    } else {
        DxcBuffer source = {t.hlsl.data, t.hlsl.length, DXC_CP_UTF8};
        const wchar_t* args[] = {L"-E",
                                 L"main",
                                 L"-T",
                                 profile_of(p->stage),
                                 L"-O3",
                                 L"-HV",
                                 L"2021",
                                 L"-Qstrip_debug",
                                 L"-Qstrip_reflect"};
        IDxcResult* result = nullptr;
        HRESULT status = E_FAIL;
        if (SUCCEEDED(w.compiler->Compile(&source, args, ARRAY_COUNT(args), nullptr, IID_PPV_ARGS(&result))))
            result->GetStatus(&status);
        IDxcBlob* object = nullptr;
        if (SUCCEEDED(status))
            result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&object), nullptr);
        if (object && object->GetBufferSize()) {
            p->dxil = malloc(object->GetBufferSize());
            if (p->dxil) {
                memcpy(p->dxil, object->GetBufferPointer(), object->GetBufferSize());
                p->dxil_size = object->GetBufferSize();
                p->vertex_element_mask = t.vertex_element_mask;
                memcpy(p->element_format, t.element_format, sizeof(p->element_format));
                p->ps_target_mask = t.ps_target_mask;
                p->ps_writes_depth = t.ps_writes_depth;
                p->sampler_count = t.sampler_count;
                memcpy(p->samplers, t.samplers, sizeof(p->samplers));
                memcpy(p->table_reach, t.table_reach, sizeof(p->table_reach));
                p->vs_param_mask = t.vs_param_mask;
                state = PROGRAM_READY;
                dump(p, t.hlsl.data, nullptr);
            }
        } else {
            IDxcBlobUtf8* errors = nullptr;
            if (result)
                result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr);
            const char* text =
                errors && errors->GetStringLength() ? errors->GetStringPointer() : "unknown error";
            LOG_W("gpu", "shader %016llx: compiling the translation failed: %.300s",
                  (unsigned long long)p->key, text);
            dump(p, t.hlsl.data, text);
            if (errors)
                errors->Release();
        }
        if (object)
            object->Release();
        if (result)
            result->Release();
    }
    t.hlsl.release();
    free(p->code);
    p->code = nullptr;
    InterlockedExchange((volatile LONG*)&p->state, (LONG)state);
}

static DWORD WINAPI worker_main(void* param) {
    Worker w = {};
    w.index = (u32)(usize)param;
    if (FAILED(g_dxc_create(CLSID_DxcCompiler, IID_PPV_ARGS(&w.compiler)))) {
        LOG_E("gpu", "shader worker %u: cannot create a compiler", w.index);
        return 1;
    }
    for (;;) {
        AcquireSRWLockExclusive(&g_job_lock);
        while (g_lanes[0].count == 0 && g_lanes[1].count == 0)
            SleepConditionVariableSRW(&g_job_ready, &g_job_lock, INFINITE, 0);
        JobLane& lane = g_lanes[g_lanes[0].count ? 0 : 1];
        Job job = lane.jobs[lane.head];
        lane.head = (lane.head + 1) % JOB_CAPACITY;
        lane.count--;
        ReleaseSRWLockExclusive(&g_job_lock);
        if (job.program)
            build_program(w, job.program);
        else
            job.run(job.context);
    }
}

// --- rectangle lists ---------------------------------------------------------------------------------

// Console rectangle lists give three corners per rectangle. The corner between the other two is the
// one facing the longest edge (the diagonal), whatever the order; the fourth corner, and its
// attributes, are the other two minus it. Drawn as a strip: the corner, its neighbours, the fourth.
#define RECT_GS_MAX 64

static struct {
    SRWLOCK lock;
    IDxcCompiler3* compiler;
    u32 count;
    struct {
        u32 param_mask;
        D3D12_SHADER_BYTECODE code;
    } entries[RECT_GS_MAX];
} g_rect = {SRWLOCK_INIT};

static bool compile_rect_gs(u32 param_mask, D3D12_SHADER_BYTECODE* out) {
    char hlsl[4096];
    usize n = (usize)snprintf(hlsl, sizeof(hlsl), "struct V {\n    float4 position : SV_Position;\n");
    for (u32 i = 0; i < 32; i++)
        if (param_mask & (1u << i))
            n += (usize)snprintf(hlsl + n, sizeof(hlsl) - n, "    float4 p%u : P%u;\n", i, i);
    n += (usize)snprintf(hlsl + n, sizeof(hlsl) - n,
                         "};\n[maxvertexcount(4)]\n"
                         "void main(triangle V v[3], inout TriangleStream<V> stream) {\n"
                         "    float2 a = v[0].position.xy / v[0].position.w;\n"
                         "    float2 b = v[1].position.xy / v[1].position.w;\n"
                         "    float2 c = v[2].position.xy / v[2].position.w;\n"
                         "    float da = dot(b - c, b - c), db = dot(a - c, a - c), dc = dot(a - b, a - b);\n"
                         "    uint k = da >= db && da >= dc ? 0 : (db >= dc ? 1 : 2);\n"
                         "    V corner = v[k], p = v[(k + 1) %% 3], q = v[(k + 2) %% 3], r;\n"
                         "    r.position = p.position + q.position - corner.position;\n");
    for (u32 i = 0; i < 32; i++)
        if (param_mask & (1u << i))
            n += (usize)snprintf(hlsl + n, sizeof(hlsl) - n, "    r.p%u = p.p%u + q.p%u - corner.p%u;\n", i,
                                 i, i, i);
    n += (usize)snprintf(hlsl + n, sizeof(hlsl) - n,
                         "    stream.Append(corner);\n    stream.Append(p);\n    stream.Append(q);\n"
                         "    stream.Append(r);\n}\n");
    if (n >= sizeof(hlsl))
        return false;
    if (!g_rect.compiler && FAILED(g_dxc_create(CLSID_DxcCompiler, IID_PPV_ARGS(&g_rect.compiler))))
        return false;
    DxcBuffer source = {hlsl, n, DXC_CP_UTF8};
    const wchar_t* args[] = {L"-E", L"main", L"-T", L"gs_6_6", L"-O3", L"-Qstrip_debug", L"-Qstrip_reflect"};
    IDxcResult* result = nullptr;
    HRESULT status = E_FAIL;
    if (SUCCEEDED(g_rect.compiler->Compile(&source, args, ARRAY_COUNT(args), nullptr, IID_PPV_ARGS(&result))))
        result->GetStatus(&status);
    IDxcBlob* object = nullptr;
    if (SUCCEEDED(status))
        result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&object), nullptr);
    bool ok = false;
    if (object && object->GetBufferSize()) {
        void* code = malloc(object->GetBufferSize());
        if (code) {
            memcpy(code, object->GetBufferPointer(), object->GetBufferSize());
            *out = {code, object->GetBufferSize()};
            ok = true;
        }
    }
    if (object)
        object->Release();
    if (result)
        result->Release();
    return ok;
}

const D3D12_SHADER_BYTECODE* shader_rect_list_gs(u32 param_mask) {
    if (!g_ready)
        return nullptr;
    AcquireSRWLockExclusive(&g_rect.lock);
    const D3D12_SHADER_BYTECODE* found = nullptr;
    for (u32 i = 0; i < g_rect.count && !found; i++)
        if (g_rect.entries[i].param_mask == param_mask)
            found = &g_rect.entries[i].code;
    if (!found && g_rect.count < RECT_GS_MAX) {
        D3D12_SHADER_BYTECODE code;
        if (compile_rect_gs(param_mask, &code)) {
            g_rect.entries[g_rect.count] = {param_mask, code};
            found = &g_rect.entries[g_rect.count++].code;
        } else {
            LOG_E("gpu", "cannot compile the rectangle list geometry shader for parameters 0x%x", param_mask);
        }
    }
    ReleaseSRWLockExclusive(&g_rect.lock);
    return found;
}

// --- the program table -------------------------------------------------------------------------------

#define PROGRAM_TABLE_SIZE 65536 // power of two

static ShaderProgram** g_programs;
static SRWLOCK g_program_lock = SRWLOCK_INIT;
static u32 g_program_count;

static u64 hash_bytes(const void* data, usize size, u64 h) {
    const u8* p = (const u8*)data;
    for (usize i = 0; i < size; i++) {
        h ^= p[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

bool shader_programs_init(const ShaderTarget& target) {
    rdna_init();
    g_target = target;
    HMODULE dxc = LoadLibraryW(L"dxcompiler.dll");
    g_dxc_create = dxc ? (DxcCreateInstanceProc)GetProcAddress(dxc, "DxcCreateInstance") : nullptr;
    if (!g_dxc_create) {
        LOG_E("gpu", "dxcompiler.dll is missing: shaders cannot be translated");
        return false;
    }
    for (JobLane& lane : g_lanes)
        lane.jobs = (Job*)calloc(JOB_CAPACITY, sizeof(Job));
    g_programs = (ShaderProgram**)calloc(PROGRAM_TABLE_SIZE, sizeof(ShaderProgram*));
    if (!g_lanes[0].jobs || !g_lanes[1].jobs || !g_programs)
        return false;
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    u32 workers = si.dwNumberOfProcessors > 4 ? si.dwNumberOfProcessors - 2 : 2;
    if (workers > 8)
        workers = 8;
    for (u32 i = 0; i < workers; i++) {
        HANDLE thread = CreateThread(nullptr, 1 << 20, worker_main, (void*)(usize)i, 0, nullptr);
        if (!thread)
            return false;
        SetThreadPriority(thread, THREAD_PRIORITY_BELOW_NORMAL);
        SetThreadDescription(thread, L"sludgemu shader worker");
        CloseHandle(thread);
    }
    g_ready = true;
    LOG_I("gpu", "shader translation: %u workers", workers);
    return true;
}

ShaderProgram* shader_program_get(const void* code, u32 code_size, const ShaderInfo& info) {
    if (!g_ready)
        return nullptr;
    u64 key = hash_bytes(code, code_size, 0xcbf29ce484222325ull);
    key = hash_bytes(&info, sizeof(info), key);
    u32 slot = (u32)key & (PROGRAM_TABLE_SIZE - 1);
    AcquireSRWLockExclusive(&g_program_lock);
    while (g_programs[slot]) {
        if (g_programs[slot]->key == key) {
            ShaderProgram* found = g_programs[slot];
            ReleaseSRWLockExclusive(&g_program_lock);
            return found;
        }
        slot = (slot + 1) & (PROGRAM_TABLE_SIZE - 1);
    }
    ShaderProgram* p = nullptr;
    if (g_program_count < PROGRAM_TABLE_SIZE / 2) {
        p = (ShaderProgram*)calloc(1, sizeof(ShaderProgram));
        u8* copy = p ? (u8*)malloc(code_size) : nullptr;
        if (p && copy) {
            memcpy(copy, code, code_size);
            p->code = copy;
            p->code_size = code_size;
            p->info = info;
            p->stage = info.stage;
            p->key = key;
            g_programs[slot] = p;
            g_program_count++;
        } else {
            free(p);
            p = nullptr;
        }
    } else {
        LOG_W_ONCE("gpu", "shader program table full; more programs are not translated");
    }
    ReleaseSRWLockExclusive(&g_program_lock);
    if (p)
        queue({p, nullptr, nullptr}, false);
    return p;
}
