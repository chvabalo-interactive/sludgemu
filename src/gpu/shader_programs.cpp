#include "gpu/shader_programs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include <ole2.h> // dxcapi.h needs COM types the lean Windows headers leave out
#include <dxcapi.h>

#include "common/log.h"
#include "generated/shader_translator_hash.h"
#include "shader/rdna.h"

// --- jobs --------------------------------------------------------------------------------------------

// A job is a program to build or a function to run.
struct Job {
    ShaderProgram* program;
    void (*run)(void* context);
    void* context;
};

// Two lanes: work something waits for, served first, and work nothing waits for yet: programs titles
// merely created (often thousands at once, at startup) and jobs queued behind them.
#define JOB_CAPACITY 65536

struct JobLane {
    Job* jobs;
    u32 head, count;
};

// Each worker has its own compiler: DXC compiler objects are not thread safe.
struct Worker {
    IDxcCompiler3* compiler;
    HANDLE thread;
    DWORD thread_id;
    u32 index;
    bool urgent_only;      // takes urgent work alone, so that waits never sit behind background work
    volatile long boosted; // raised to normal priority: something waits for its job (gpu_job_hurry)
};

#define MAX_GENERAL_WORKERS 8
#define MAX_WORKERS         (MAX_GENERAL_WORKERS + 1) // and the urgent-only one

static JobLane g_lanes[2]; // urgent, background
static SRWLOCK g_job_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE g_job_ready = CONDITION_VARIABLE_INIT;    // general workers
static CONDITION_VARIABLE g_urgent_ready = CONDITION_VARIABLE_INIT; // the urgent-only worker
static Worker g_workers[MAX_WORKERS];
static u32 g_worker_count;
static u32 g_active; // workers running a job (under g_job_lock)
static bool g_ready;

// Programs done, for the report when the queue drains.
static struct {
    volatile long cached, built, failed;
    long reported;       // cached + built + failed at the last report (under g_job_lock)
    LARGE_INTEGER start; // when the queue last went from idle to busy (under g_job_lock)
} g_stats;

static bool queue(const Job& job, bool urgent) {
    if (!g_ready)
        return false;
    AcquireSRWLockExclusive(&g_job_lock);
    JobLane& lane = g_lanes[urgent ? 0 : 1];
    if (lane.count == JOB_CAPACITY) {
        ReleaseSRWLockExclusive(&g_job_lock);
        LOG_E("gpu", "GPU job queue full; job dropped");
        return false;
    }
    if (!g_active && !g_lanes[0].count && !g_lanes[1].count)
        QueryPerformanceCounter(&g_stats.start);
    lane.jobs[(lane.head + lane.count) % JOB_CAPACITY] = job;
    lane.count++;
    ReleaseSRWLockExclusive(&g_job_lock);
    WakeConditionVariable(&g_job_ready);
    if (urgent)
        WakeConditionVariable(&g_urgent_ready);
    return true;
}

bool gpu_job_queue(void (*run)(void* context), void* context) {
    return queue({nullptr, run, context}, true);
}

bool gpu_job_queue_background(void (*run)(void* context), void* context) {
    return queue({nullptr, run, context}, false);
}

static void hurry(ShaderProgram* p) {
    if (p->state == PROGRAM_PENDING && InterlockedExchange(&p->urgent, 1) == 0 &&
        !queue({p, nullptr, nullptr}, true))
        InterlockedExchange(&p->urgent, 0); // asked again by the next wait
}

u32 gpu_job_worker() {
    DWORD id = GetCurrentThreadId();
    for (u32 i = 0; i < g_worker_count; i++)
        if (g_workers[i].thread_id == id)
            return i + 1;
    return 0;
}

// The waiting thread would otherwise wait for what busy cores leave over.
void gpu_job_hurry(u32 worker) {
    if (worker > 0 && worker <= g_worker_count && InterlockedExchange(&g_workers[worker - 1].boosted, 1) == 0)
        SetThreadPriority(g_workers[worker - 1].thread, THREAD_PRIORITY_NORMAL);
}

static bool is_final(u32 state) {
    return state == PROGRAM_READY || state == PROGRAM_FAILED;
}

bool shader_program_wait(ShaderProgram* p) {
    u32 state = p->state;
    if (is_final(state))
        return state == PROGRAM_READY;
    hurry(p);
    u32 waited_s = 0;
    bool boosted = false;
    for (;;) {
        state = p->state;
        if (is_final(state))
            return state == PROGRAM_READY;
        if (state == PROGRAM_BUILDING && !boosted && p->worker) {
            gpu_job_hurry((u32)p->worker);
            boosted = true;
        }
        // Builders wake every waiter when they start (to be hurried along) and when the state becomes
        // final; the timeout only asks again where the queue was full, and tells about a wait that
        // looks stuck.
        if (!WaitOnAddress(&p->state, &state, sizeof(state), 1000) && GetLastError() == ERROR_TIMEOUT) {
            hurry(p);
            if (++waited_s == 10)
                LOG_W("gpu", "still waiting for shader %016llx after 10 s (state %u)",
                      (unsigned long long)p->key, p->state);
        }
    }
}

bool shader_program_wait_started(ShaderProgram* p) {
    for (;;) {
        u32 state = p->state;
        if (is_final(state))
            return true;
        if (state == PROGRAM_PENDING)
            return false;
        WaitOnAddress(&p->state, &state, sizeof(state), INFINITE);
    }
}

// --- building ----------------------------------------------------------------------------------------

static DxcCreateInstanceProc g_dxc_create;
static bool g_cache_open;         // the disk cache is in use
static ShaderTarget g_target;     // what the device runs, for the translator
static char g_dump_dir[MAX_PATH]; // SLUDGE_SHADER_DUMP, or empty

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
    if (!g_dump_dir[0])
        return;
    char path[MAX_PATH + 64];
    snprintf(path, sizeof(path), "%s\\%016llx.bin", g_dump_dir, (unsigned long long)p->key);
    FILE* f = fopen(path, "wb");
    if (f) {
        fwrite(p->code, 1, p->code_size, f);
        fclose(f);
    }
    snprintf(path, sizeof(path), "%s\\%016llx.info", g_dump_dir, (unsigned long long)p->key);
    f = fopen(path, "wb");
    if (f) {
        fwrite(&p->info, sizeof(p->info), 1, f);
        fclose(f);
    }
    snprintf(path, sizeof(path), "%s\\%016llx.hlsl", g_dump_dir, (unsigned long long)p->key);
    f = fopen(path, "w");
    if (f) {
        if (reason)
            fprintf(f, "// %s\n", reason);
        if (hlsl)
            fputs(hlsl, f);
        fclose(f);
    }
}

// Translates and compiles the program into `out`; false (logged) if it can't be.
static bool translate_program(Worker& w, const ShaderProgram* p, ShaderBinary* out) {
    TranslatedShader t = {};
    bool ok = false;
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
            out->dxil = malloc(object->GetBufferSize());
            if (out->dxil) {
                memcpy(out->dxil, object->GetBufferPointer(), object->GetBufferSize());
                out->dxil_size = object->GetBufferSize();
                out->vertex_element_mask = t.vertex_element_mask;
                memcpy(out->element_format, t.element_format, sizeof(out->element_format));
                out->ps_target_mask = t.ps_target_mask;
                out->ps_writes_depth = t.ps_writes_depth;
                out->sampler_count = t.sampler_count;
                memcpy(out->samplers, t.samplers, sizeof(out->samplers));
                memcpy(out->table_reach, t.table_reach, sizeof(out->table_reach));
                out->vs_param_mask = t.vs_param_mask;
                ok = true;
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
    return ok;
}

static void build_program(Worker& w, ShaderProgram* p) {
    // Programs can be queued in both lanes; the first worker to get one builds it.
    if (InterlockedCompareExchange((volatile LONG*)&p->state, PROGRAM_BUILDING, PROGRAM_PENDING) !=
        PROGRAM_PENDING)
        return;
    InterlockedExchange(&p->worker, (long)w.index + 1);
    // A thread waiting since before the start hurries this worker: it may be a background one, and
    // it was asked first by the program's creation.
    WakeByAddressAll((void*)&p->state);
    ShaderBinary b;
    memset(&b, 0, sizeof(b));
    // Dumps are made by building, so they bypass the cache (and refresh it).
    bool ready = !g_dump_dir[0] && shader_cache_load(p->key, p->code_size, &b);
    if (ready) {
        InterlockedIncrement(&g_stats.cached);
    } else {
        ready = translate_program(w, p, &b);
        InterlockedIncrement(ready ? &g_stats.built : &g_stats.failed);
        if (ready && g_cache_open && !shader_cache_store(p->key, p->code_size, b))
            LOG_W_ONCE("gpu", "shader cache: cannot write to %ls; programs are translated again next run",
                       shader_cache_folder());
    }
    p->binary = b;
    free(p->code);
    p->code = nullptr;
    InterlockedExchange((volatile LONG*)&p->state, ready ? PROGRAM_READY : PROGRAM_FAILED);
    WakeByAddressAll((void*)&p->state);
}

// Logs what the workers did since the last report, once the queue drains after enough programs (a
// report per pipeline titles create mid-game would only be noise).
static void report_drained(const LARGE_INTEGER& start) {
    long cached = g_stats.cached, built = g_stats.built, failed = g_stats.failed;
    long total = cached + built + failed;
    AcquireSRWLockExclusive(&g_job_lock);
    long since = total - g_stats.reported;
    bool report = since >= 64;
    if (report)
        g_stats.reported = total;
    ReleaseSRWLockExclusive(&g_job_lock);
    if (!report)
        return;
    LARGE_INTEGER now, frequency;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    LOG_I("gpu",
          "shader queue drained (busy %.1f s): %ld programs since the last report; in all %ld from the disk "
          "cache, %ld translated, %ld failed",
          (f64)(now.QuadPart - start.QuadPart) / (f64)frequency.QuadPart, since, cached, built, failed);
}

static DWORD WINAPI worker_main(void* param) {
    Worker& w = *(Worker*)param;
    int resting = w.urgent_only ? THREAD_PRIORITY_NORMAL : THREAD_PRIORITY_BELOW_NORMAL;
    for (;;) {
        AcquireSRWLockExclusive(&g_job_lock);
        while (g_lanes[0].count == 0 && (w.urgent_only || g_lanes[1].count == 0))
            SleepConditionVariableSRW(w.urgent_only ? &g_urgent_ready : &g_job_ready, &g_job_lock, INFINITE,
                                      0);
        bool urgent = g_lanes[0].count != 0;
        JobLane& lane = g_lanes[urgent ? 0 : 1];
        Job job = lane.jobs[lane.head];
        lane.head = (lane.head + 1) % JOB_CAPACITY;
        lane.count--;
        g_active++;
        ReleaseSRWLockExclusive(&g_job_lock);
        // Urgent work runs at normal priority: something is waiting for it.
        bool raised = urgent && !w.urgent_only;
        if (raised)
            SetThreadPriority(w.thread, THREAD_PRIORITY_NORMAL);
        if (job.program)
            build_program(w, job.program);
        else
            job.run(job.context);
        if (InterlockedExchange(&w.boosted, 0))
            raised = true;
        if (raised && !w.urgent_only)
            SetThreadPriority(w.thread, resting);
        AcquireSRWLockExclusive(&g_job_lock);
        bool drained = --g_active == 0 && !g_lanes[0].count && !g_lanes[1].count;
        LARGE_INTEGER start = g_stats.start;
        ReleaseSRWLockExclusive(&g_job_lock);
        if (drained)
            report_drained(start);
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

// The cache generation: everything besides a program's code and inputs (its key) that decides what
// its translation comes out as. The translator's sources (hashed at build time), what the device runs,
// and the compiler's version.
static u64 cache_generation(IDxcCompiler3* compiler) {
    u64 h = hash_bytes("sludgemu shader cache", 21, 0xcbf29ce484222325ull);
    u64 translator = SHADER_TRANSLATOR_HASH;
    h = hash_bytes(&translator, sizeof(translator), h);
    h = hash_bytes(&g_target, sizeof(g_target), h);
    bool int64_atomics = gpu_int64_image_atomics();
    h = hash_bytes(&int64_atomics, sizeof(int64_atomics), h);
    IDxcVersionInfo2* version = nullptr;
    if (SUCCEEDED(compiler->QueryInterface(IID_PPV_ARGS(&version)))) {
        UINT32 numbers[3] = {};
        char* commit = nullptr;
        version->GetVersion(&numbers[0], &numbers[1]);
        if (SUCCEEDED(version->GetCommitInfo(&numbers[2], &commit)) && commit) {
            h = hash_bytes(commit, strlen(commit), h);
            CoTaskMemFree(commit);
        }
        h = hash_bytes(numbers, sizeof(numbers), h);
        version->Release();
    } else {
        LOG_W("gpu",
              "the shader compiler doesn't tell its version; the shader cache can't tell its builds apart");
    }
    return h;
}

static void prune_cache(void*) {
    // A generation per translator build: keep the few most recently used (switching between builds
    // stays quick), none older than two weeks.
    u32 removed = shader_cache_prune(14, 4);
    if (removed)
        LOG_I("gpu", "shader cache: removed %u older generations", removed);
}

static void open_cache(const wchar_t* root, IDxcCompiler3* compiler) {
    char setting[8];
    if (GetEnvironmentVariableA("SLUDGE_SHADER_CACHE", setting, sizeof(setting)) && setting[0] == '0') {
        LOG_I("gpu", "shader cache: off (SLUDGE_SHADER_CACHE=0)");
        return;
    }
    if (!root)
        return;
    u64 generation = cache_generation(compiler);
    if (!shader_cache_open(root, generation)) {
        LOG_W("gpu", "shader cache: cannot use %ls; programs are translated on every run", root);
        return;
    }
    g_cache_open = true;
    LOG_I("gpu", "shader cache: %ls", shader_cache_folder());
    gpu_job_queue_background(prune_cache, nullptr);
}

bool shader_programs_init(const ShaderTarget& target, const wchar_t* cache_root) {
    rdna_init();
    g_target = target;
    if (!GetEnvironmentVariableA("SLUDGE_SHADER_DUMP", g_dump_dir, sizeof(g_dump_dir)))
        g_dump_dir[0] = 0;
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
    u32 general = si.dwNumberOfProcessors > 4 ? si.dwNumberOfProcessors - 2 : 2;
    if (general > MAX_GENERAL_WORKERS)
        general = MAX_GENERAL_WORKERS;
    // Compilers first: a pool without them would leave waits for programs hanging.
    g_worker_count = general + 1;
    for (u32 i = 0; i < g_worker_count; i++) {
        Worker& w = g_workers[i];
        w.index = i;
        w.urgent_only = i == general;
        if (FAILED(g_dxc_create(CLSID_DxcCompiler, IID_PPV_ARGS(&w.compiler)))) {
            LOG_E("gpu", "cannot create a shader compiler: shaders cannot be translated");
            return false;
        }
    }
    g_ready = true; // jobs can be queued from here on; the workers take them once started
    open_cache(cache_root, g_workers[0].compiler);
    for (u32 i = 0; i < g_worker_count; i++) {
        Worker& w = g_workers[i];
        w.thread = CreateThread(nullptr, 1 << 20, worker_main, &w, CREATE_SUSPENDED, &w.thread_id);
        if (!w.thread)
            FATAL("cannot start shader workers");
        SetThreadPriority(w.thread, w.urgent_only ? THREAD_PRIORITY_NORMAL : THREAD_PRIORITY_BELOW_NORMAL);
        SetThreadDescription(w.thread,
                             w.urgent_only ? L"sludgemu shader worker (urgent)" : L"sludgemu shader worker");
        ResumeThread(w.thread);
    }
    LOG_I("gpu", "shader translation: %u workers and one for work draws wait for", general);
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
    // A program no queue holds would never be built, and a draw waiting for it would wait forever.
    if (p && !queue({p, nullptr, nullptr}, false)) {
        InterlockedExchange((volatile LONG*)&p->state, PROGRAM_FAILED);
        WakeByAddressAll((void*)&p->state);
    }
    return p;
}
