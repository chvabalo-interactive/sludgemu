#include "gpu/shader_programs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include <ole2.h> // dxcapi.h needs COM types the lean Windows headers leave out
#include <dxcapi.h>

#include "common/log.h"
#include "core/memory.h"
#include "generated/shader_translator_hash.h"
#include "gpu/shader_schedule.h"
#include "shader/rdna.h"

// --- jobs --------------------------------------------------------------------------------------------

// Work something waits for is served first, in the order asked. Work nothing waits for yet (programs
// titles merely created, often thousands at once at startup, and jobs queued behind them) runs cheapest
// first on part of the workers, and its compiles start only while their memory fits the budget
// (gpu/shader_schedule.h).
#define JOB_CAPACITY 65536

struct UrgentLane {
    ShaderJob* jobs;
    u32 head, count;
};

// Each worker has its own compiler: DXC compiler objects are not thread safe.
struct Worker {
    IDxcCompiler3* compiler;
    HANDLE thread;
    DWORD thread_id;
    u32 index;
    // The job running and its speed (under `lock`): urgent jobs run at full speed, background ones
    // yield to the title until something waits for them (gpu_job_hurry), for that job alone.
    SRWLOCK lock;
    u32 ticket; // counts the jobs started; with the index it names the running job (job_token)
    bool busy;
    bool full_speed;
};

static UrgentLane g_urgent;
static ShaderJobHeap g_background; // cheapest (least code) first
static ShaderJobHeap g_compiles;   // translated programs waiting for memory to compile in, least HLSL first
static SRWLOCK g_job_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE g_job_ready = CONDITION_VARIABLE_INIT;
static Worker g_workers[SHADER_MAX_WORKERS];
static ShaderSchedule g_schedule;
static u32 g_worker_count;
static u32 g_active;             // workers running a job (under g_job_lock)
static u32 g_background_running; // of them on background jobs (under g_job_lock)
static u64 g_compile_in_flight;  // the estimated memory of the compiles running (under g_job_lock)
static bool g_ready;

// The title's waits (gpu/shader_schedule.h has the budget). Under g_job_lock: the workers' decision to
// sleep (no background work while the title waits) and a wait's end, which wakes them, are ordered.
static ShaderWaitBudget g_wait_budget;
static u32 g_title_waits; // waits running
static LARGE_INTEGER g_clock_start, g_clock_frequency;

// Jobs to queue once a program is built (shader_program_then).
struct ProgramThen {
    ProgramThen* next;
    void (*run)(void* context);
    void* context;
};

// What the workers did, for the report when the queue drains.
static struct {
    volatile long cached, built, failed;
    long reported;       // cached + built + failed at the last report (under g_job_lock)
    LARGE_INTEGER start; // when the queue last went from idle to busy (under g_job_lock)
    long compile_waits;  // compiles that waited for memory (under g_job_lock)
    u64 compile_peak;    // the most compile memory estimated at once (under g_job_lock)
} g_stats;

// --- worker speed ---

// Background work yields to the title in every way that can be undone from another thread, since the
// thread that waits for a background job is the one that raises it: the lowest priority (not idle: a
// compile preempted while it holds a heap lock the title needs would hold it until the scheduler's
// anti-starvation boost, seconds later), efficiency mode (efficiency cores on hybrid processors, low
// clocks elsewhere, which leaves laptops the power for the title's threads) and low memory priority
// (its pages go first when memory runs short). Background mode (THREAD_MODE_BACKGROUND_BEGIN) would add
// very low I/O priority, which would slow down the disk cache under the title's streaming, and only
// the thread itself can leave it, so a waiting thread could not raise a worker.
static bool set_speed(Worker& w, bool full) {
    THREAD_POWER_THROTTLING_STATE throttling = {THREAD_POWER_THROTTLING_CURRENT_VERSION,
                                                THREAD_POWER_THROTTLING_EXECUTION_SPEED,
                                                full ? 0u : (ULONG)THREAD_POWER_THROTTLING_EXECUTION_SPEED};
    MEMORY_PRIORITY_INFORMATION memory = {full ? (ULONG)MEMORY_PRIORITY_NORMAL : (ULONG)MEMORY_PRIORITY_LOW};
    bool ok = SetThreadPriority(w.thread, full ? THREAD_PRIORITY_NORMAL : THREAD_PRIORITY_LOWEST) != 0;
    ok &= SetThreadInformation(w.thread, ThreadPowerThrottling, &throttling, sizeof(throttling)) != 0;
    ok &= SetThreadInformation(w.thread, ThreadMemoryPriority, &memory, sizeof(memory)) != 0;
    w.full_speed = full;
    return ok;
}

// Names the job a worker runs: what waits for it raises the worker for that job, not for whatever
// the worker runs by the time the request comes.
#define TICKET_BITS 28

static u32 job_token(const Worker& w) {
    return (w.ticket & ((1u << TICKET_BITS) - 1)) << 4 | (w.index + 1);
}

static void begin_job(Worker& w, bool urgent) {
    AcquireSRWLockExclusive(&w.lock);
    w.ticket++;
    w.busy = true;
    // Background jobs drop the speed the previous job was raised to (dropping when a job ends would
    // only raise it again before an urgent job).
    if (w.full_speed != urgent && !set_speed(w, urgent))
        LOG_W_ONCE("gpu", "cannot change the speed of shader workers (error %lu)", GetLastError());
    ReleaseSRWLockExclusive(&w.lock);
}

static void end_job(Worker& w) {
    AcquireSRWLockExclusive(&w.lock);
    w.busy = false;
    ReleaseSRWLockExclusive(&w.lock);
}

u32 gpu_job_worker() {
    DWORD id = GetCurrentThreadId();
    for (u32 i = 0; i < g_worker_count; i++)
        if (g_workers[i].thread_id == id)
            return job_token(g_workers[i]);
    return 0;
}

// The waiting thread would otherwise wait for what the title leaves over.
void gpu_job_hurry(u32 token) {
    u32 index = (token & 15) - 1;
    if (index >= g_worker_count)
        return;
    Worker& w = g_workers[index];
    AcquireSRWLockExclusive(&w.lock);
    if (w.busy && job_token(w) == token && !w.full_speed && !set_speed(w, true))
        LOG_W_ONCE("gpu", "cannot change the speed of shader workers (error %lu)", GetLastError());
    ReleaseSRWLockExclusive(&w.lock);
}

// --- queues ---

static bool queues_empty() {
    return !g_urgent.count && !g_background.count && !g_compiles.count;
}

// Under g_job_lock. False if the queue is full.
static bool push_job(const ShaderJob& job, bool urgent, u64 cost) {
    if (!g_active && queues_empty())
        QueryPerformanceCounter(&g_stats.start);
    if (!urgent)
        return shader_job_heap_push(&g_background, job, cost);
    if (g_urgent.count == JOB_CAPACITY)
        return false;
    g_urgent.jobs[(g_urgent.head + g_urgent.count) % JOB_CAPACITY] = job;
    g_urgent.count++;
    return true;
}

// Background jobs cost `cost` (programs: their code size; functions: 0, they are quick).
static bool queue(const ShaderJob& job, bool urgent, u64 cost) {
    if (!g_ready)
        return false;
    AcquireSRWLockExclusive(&g_job_lock);
    bool queued = push_job(job, urgent, cost);
    ReleaseSRWLockExclusive(&g_job_lock);
    if (!queued) {
        LOG_E("gpu", "GPU job queue full; job dropped");
        return false;
    }
    WakeConditionVariable(&g_job_ready);
    return true;
}

bool gpu_job_queue(void (*run)(void* context), void* context) {
    return queue({nullptr, run, context}, true, 0);
}

bool gpu_job_queue_background(void (*run)(void* context), void* context) {
    return queue({nullptr, run, context}, false, 0);
}

// A draw needs `p`: from here on it is built ahead of the programs merely created, at full speed, and if
// no worker has it, it is queued in the urgent lane. Its compile starts whatever the memory only while a
// title thread is blocked on it (`blocked`): draws skip what isn't built once their waits run out, and
// make every program they skip urgent, so urgent alone may mean many of the largest programs at once
// (about a gigabyte of compiler memory each). One put aside for memory is queued again only for a
// blocked thread, to be taken over; for draws that skip it, it stays put aside until it fits. Under
// g_job_lock, like the worker's decision to put a translated program aside (build_program): one of the
// two sees the other's change.
static void hurry(ShaderProgram* p) {
    AcquireSRWLockExclusive(&g_job_lock);
    p->urgent = true;
    u32 state = p->state;
    bool queued = false;
    if (!p->urgent_queued && (state == PROGRAM_PENDING || (state == PROGRAM_TRANSLATED && p->blocked)))
        queued = p->urgent_queued = push_job({p, nullptr, nullptr}, true, 0); // else asked again by the wait
    ReleaseSRWLockExclusive(&g_job_lock);
    if (queued)
        WakeConditionVariable(&g_job_ready);
}

// Compiles nothing waits for take only commit the system can spare: they leave room for the title to
// grow, as a compile of one of the largest programs takes a gigabyte and the title stops when the system
// runs out. All if Windows doesn't tell.
#define COMPILE_COMMIT_HEADROOM GiB(2)

static u64 commit_room() {
    MEMORYSTATUSEX m = {sizeof(m)};
    if (!GlobalMemoryStatusEx(&m))
        return ~0ull;
    u64 keep = COMPILE_COMMIT_HEADROOM;
    return m.ullAvailPageFile > keep ? m.ullAvailPageFile - keep : 0;
}

static ShaderQueueState queue_state() {
    ShaderQueueState s;
    ShaderJob cheapest;
    s.urgent_queued = g_urgent.count;
    s.background_queued = g_background.count;
    s.background_running = g_background_running;
    s.background_slots = g_schedule.background_slots;
    s.title_waiting = g_title_waits != 0;
    s.compile_waiting = shader_job_heap_peek(&g_compiles, &cheapest);
    s.compile_cost = s.compile_waiting ? cheapest.program->compile_cost : 0;
    s.compile_in_flight = g_compile_in_flight;
    s.compile_budget = g_schedule.compile_budget;
    s.commit_room = s.compile_waiting ? commit_room() : 0;
    return s;
}

// Programs that waited for memory and were then compiled for a draw stay in g_compiles; they are
// dropped as they come up. Under g_job_lock.
static void drop_taken_compiles() {
    ShaderJob job;
    while (shader_job_heap_peek(&g_compiles, &job) && job.program->state != PROGRAM_TRANSLATED)
        shader_job_heap_pop(&g_compiles, &job);
}

// Reserves the estimated memory of a compile. Under g_job_lock.
static void reserve_compile(u64 cost) {
    g_compile_in_flight += cost;
    if (g_compile_in_flight > g_stats.compile_peak)
        g_stats.compile_peak = g_compile_in_flight;
}

// A worker's next job, PICK_NOTHING to sleep. Under g_job_lock.
static ShaderPick take_job(ShaderJob* job) {
    drop_taken_compiles();
    ShaderPick pick = shader_pick(queue_state());
    switch (pick) {
    case PICK_URGENT:
        *job = g_urgent.jobs[g_urgent.head];
        g_urgent.head = (g_urgent.head + 1) % JOB_CAPACITY;
        g_urgent.count--;
        // Queued again from here on if it is put aside and a thread blocks on it.
        if (job->program)
            job->program->urgent_queued = false;
        break;
    case PICK_BACKGROUND:
        shader_job_heap_pop(&g_background, job);
        break;
    case PICK_COMPILE:
        shader_job_heap_pop(&g_compiles, job);
        InterlockedExchange((volatile LONG*)&job->program->state, PROGRAM_BUILDING);
        reserve_compile(job->program->compile_cost);
        break;
    case PICK_NOTHING:
        break;
    }
    return pick;
}

static bool is_final(u32 state) {
    return state == PROGRAM_READY || state == PROGRAM_FAILED;
}

// --- waits ---

u64 shader_wait_now_us() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    u64 f = (u64)g_clock_frequency.QuadPart;
    if (!f)
        return 0; // before shader_programs_init: no program exists to wait for
    u64 ticks = (u64)(now.QuadPart - g_clock_start.QuadPart);
    // In two parts: ticks times a million would overflow after days.
    return ticks / f * 1000000 + ticks % f * 1000000 / f;
}

void shader_wait_begin(ShaderWait* wait, bool dispatch) {
    wait->start_us = shader_wait_now_us();
    AcquireSRWLockExclusive(&g_job_lock);
    wait->deadline_us = shader_wait_budget_begin(&g_wait_budget, wait->start_us, dispatch);
    if (wait->deadline_us)
        g_title_waits++;
    ReleaseSRWLockExclusive(&g_job_lock);
}

void shader_wait_end(ShaderWait* wait) {
    if (!wait->deadline_us)
        return;
    u64 now = shader_wait_now_us();
    AcquireSRWLockExclusive(&g_job_lock);
    shader_wait_budget_end(&g_wait_budget, now);
    bool last = --g_title_waits == 0;
    bool background = last && shader_pick(queue_state()) != PICK_NOTHING;
    ReleaseSRWLockExclusive(&g_job_lock);
    // Background work held back meanwhile starts again (a worker that takes it wakes the next).
    if (background)
        WakeConditionVariable(&g_job_ready);
}

u32 shader_wait_ms_left(u64 deadline_us) {
    u64 now = shader_wait_now_us();
    if (!deadline_us || now >= deadline_us)
        return 0;
    return (u32)((deadline_us - now + 999) / 1000);
}

void shader_program_hurry(ShaderProgram* p) {
    u32 state = p->state;
    if (is_final(state))
        return;
    // Once urgent and queued in the urgent lane, building or put aside, whoever builds it does so at full
    // speed (start_program), and one put aside waits for memory (or for a blocked thread, which hurries
    // it itself), so asking again takes no lock. Read without it: `urgent` is only ever set, and a stale
    // `urgent_queued` names a job a worker just took, which builds the program or puts it aside.
    if (p->urgent && (p->urgent_queued || state == PROGRAM_BUILDING || state == PROGRAM_TRANSLATED))
        return;
    hurry(p);
    // A worker that had it before it was urgent runs it at background speed.
    long builder = p->worker;
    if (p->state == PROGRAM_BUILDING && builder)
        gpu_job_hurry((u32)builder);
}

// The thread blocks on `p` until it is final or `deadline_us` passes; counted in `blocked` meanwhile.
static ShaderWaitResult wait_blocked(ShaderProgram* p, u64 deadline_us) {
    long boosted = 0;
    for (;;) {
        u32 state = p->state;
        if (is_final(state))
            return state == PROGRAM_READY ? SHADER_WAIT_READY : SHADER_WAIT_FAILED;
        // A program can pass through several workers (translated on one, compiled on another).
        long builder = p->worker;
        if (state == PROGRAM_BUILDING && builder && builder != boosted) {
            gpu_job_hurry((u32)builder);
            boosted = builder;
        }
        u32 ms = shader_wait_ms_left(deadline_us);
        if (!ms) {
            p->wait_ran_out = true;
            return SHADER_WAIT_LATE;
        }
        // Builders wake every waiter when they start (to be hurried along), when they put a program
        // aside and when the state becomes final; the timeout ends the wait at its deadline, and asks
        // again where the urgent lane was full.
        if (!WaitOnAddress(&p->state, &state, sizeof(state), ms < 1000 ? ms : 1000) &&
            GetLastError() == ERROR_TIMEOUT)
            hurry(p);
    }
}

static void set_blocked(ShaderProgram* p, bool blocked) {
    AcquireSRWLockExclusive(&g_job_lock);
    if (blocked)
        p->blocked++;
    else
        p->blocked--;
    ReleaseSRWLockExclusive(&g_job_lock);
}

ShaderWaitResult shader_program_wait(ShaderProgram* p, u64 deadline_us) {
    u32 state = p->state;
    if (is_final(state))
        return state == PROGRAM_READY ? SHADER_WAIT_READY : SHADER_WAIT_FAILED;
    if (!shader_wait_ms_left(deadline_us)) {
        shader_program_hurry(p);
        if (deadline_us)
            p->wait_ran_out = true;
        return SHADER_WAIT_LATE;
    }
    // Blocked before hurried: one put aside for memory is then taken over by an urgent job, and one
    // still translating is compiled whatever the memory (build_program reads `blocked` under the lock).
    set_blocked(p, true);
    hurry(p);
    ShaderWaitResult r = wait_blocked(p, deadline_us);
    set_blocked(p, false);
    return r;
}

bool shader_program_then(ShaderProgram* p, void (*run)(void* context), void* context) {
    if (is_final(p->state))
        return false;
    ProgramThen* then = (ProgramThen*)malloc(sizeof(ProgramThen));
    if (!then)
        return false;
    then->run = run;
    then->context = context;
    // Under the lock that finishing takes after the final state is set: either this sees the final
    // state, or finishing sees this job.
    AcquireSRWLockExclusive(&g_job_lock);
    bool final = is_final(p->state);
    if (!final) {
        then->next = p->then;
        p->then = then;
    }
    ReleaseSRWLockExclusive(&g_job_lock);
    if (final)
        free(then);
    return !final;
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
    case SHADER_MESH:
        return L"ms_6_6";
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

// Translates the program to HLSL in `t`; false (logged, and the HLSL released) if it can't be.
static bool translate_program(const ShaderProgram* p, TranslatedShader* t) {
    bool translated = shader_translate(p->code, p->code_size, p->info, g_target, t);
    // The pipeline's wave size (unverified) against what the code proves, where both tell; the code wins.
    if (p->info.wave_size && t->code_wave_size && t->code_wave_size != p->info.wave_size)
        LOG_W("gpu", "shader %016llx: launched as wave%u, but its code is wave%u", (unsigned long long)p->key,
              p->info.wave_size, t->code_wave_size);
    if (!translated && p->stage == SHADER_MESH) {
        // Every mesh shader fails the same way until mesh translation exists: once is enough.
        LOG_W_ONCE("gpu", "mesh shaders aren't translated yet (%s); SLUDGE_SHADER_DUMP keeps them", t->error);
        LOG_D("gpu", "shader %016llx: translation failed: %s", (unsigned long long)p->key, t->error);
        dump(p, t->hlsl.data, t->error);
    } else if (!translated) {
        LOG_W("gpu", "shader %016llx: translation failed: %s", (unsigned long long)p->key, t->error);
        dump(p, t->hlsl.data, t->error);
    } else if (t->uses_int64_atomics && !gpu_int64_image_atomics()) {
        // The pipeline would fail to create (or worse); the adapter's lack was logged at startup.
        LOG_W("gpu", "shader %016llx: needs 64-bit atomics on typed views, which the adapter lacks",
              (unsigned long long)p->key);
        dump(p, t->hlsl.data, "needs 64-bit atomics on typed views");
        translated = false;
    }
    if (!translated)
        t->hlsl.release();
    return translated;
}

// Compiles the translation into `out`; false (logged) if it can't be.
static bool compile_program(Worker& w, const ShaderProgram* p, const TranslatedShader& t, ShaderBinary* out) {
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
    bool ok = false;
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
        const char* text = errors && errors->GetStringLength() ? errors->GetStringPointer() : "unknown error";
        LOG_W("gpu", "shader %016llx: compiling the translation failed: %.300s", (unsigned long long)p->key,
              text);
        dump(p, t.hlsl.data, text);
        if (errors)
            errors->Release();
    }
    if (object)
        object->Release();
    if (result)
        result->Release();
    return ok;
}

// The program is built (or failed): waiters are woken and the jobs waiting for it queued.
static void finish_program(ShaderProgram* p, bool ready, const ShaderBinary& b) {
    p->binary = b;
    free(p->code);
    p->code = nullptr;
    InterlockedExchange((volatile LONG*)&p->state, ready ? PROGRAM_READY : PROGRAM_FAILED);
    WakeByAddressAll((void*)&p->state);
    AcquireSRWLockExclusive(&g_job_lock);
    ProgramThen* then = p->then;
    p->then = nullptr;
    ReleaseSRWLockExclusive(&g_job_lock);
    while (then) {
        ProgramThen* next = then->next;
        // A job the full queue can't take runs here: it was promised to run.
        if (!queue({nullptr, then->run, then->context}, false, 0))
            then->run(then->context);
        free(then);
        then = next;
    }
}

// Compiles the translation, whose memory is reserved as `cost`, and finishes the program.
static void compile_and_finish(Worker& w, ShaderProgram* p, TranslatedShader& t, u64 cost) {
    ShaderBinary b;
    memset(&b, 0, sizeof(b));
    bool ready = compile_program(w, p, t, &b);
    t.hlsl.release();
    AcquireSRWLockExclusive(&g_job_lock);
    g_compile_in_flight -= cost;
    ReleaseSRWLockExclusive(&g_job_lock);
    InterlockedIncrement(ready ? &g_stats.built : &g_stats.failed);
    if (ready && g_cache_open && !shader_cache_store(p->key, p->code_size, b))
        LOG_W_ONCE("gpu", "shader cache: cannot write to %ls; programs are translated again next run",
                   shader_cache_folder());
    finish_program(p, ready, b);
}

// The worker has the program from here on: a thread waiting since before hurries it (it may run in
// the background, as the program was asked for first by its creation). A waiter that read the worker
// before it was named here can't, and would sleep out its timeout while the build crawls at background
// speed, so the worker raises itself for a program something waits for. The waiter marks `urgent`
// under g_job_lock before it reads the worker, and the worker reads `urgent` under it after naming
// itself: one of the two sees the other.
static void start_program(Worker& w, ShaderProgram* p) {
    u32 token = job_token(w);
    InterlockedExchange(&p->worker, (long)token);
    AcquireSRWLockShared(&g_job_lock);
    bool urgent = p->urgent;
    ReleaseSRWLockShared(&g_job_lock);
    if (urgent)
        gpu_job_hurry(token);
    WakeByAddressAll((void*)&p->state);
}

// A translated program that waited for memory, now compiled (taken by take_job, its memory reserved).
static void compile_translated(Worker& w, ShaderProgram* p) {
    start_program(w, p);
    TranslatedShader* t = p->translation;
    p->translation = nullptr;
    compile_and_finish(w, p, *t, p->compile_cost);
    free(t);
}

// A program from a lane: loaded from the disk cache, or translated and compiled. A background compile
// that doesn't fit the memory budget is put aside translated (the HLSL is a fraction of the compile's
// memory), and the worker goes on with other work.
static void build_program(Worker& w, ShaderProgram* p, bool urgent) {
    // Urgent jobs take over programs put aside: compiled now when a thread is blocked on them (whatever
    // the memory) or when they fit, else left aside until they do.
    if (urgent && p->state == PROGRAM_TRANSLATED) {
        u64 room = commit_room();
        AcquireSRWLockExclusive(&g_job_lock);
        bool taken = p->state == PROGRAM_TRANSLATED &&
                     shader_compile_admitted(g_compile_in_flight, p->compile_cost, g_schedule.compile_budget,
                                             room, p->blocked != 0);
        if (taken) {
            InterlockedExchange((volatile LONG*)&p->state, PROGRAM_BUILDING);
            reserve_compile(p->compile_cost);
        }
        ReleaseSRWLockExclusive(&g_job_lock);
        if (taken)
            compile_translated(w, p);
        return;
    }
    // Programs can be queued in both lanes; the first worker to get one builds it.
    if (InterlockedCompareExchange((volatile LONG*)&p->state, PROGRAM_BUILDING, PROGRAM_PENDING) !=
        PROGRAM_PENDING)
        return;
    start_program(w, p);
    ShaderBinary b;
    memset(&b, 0, sizeof(b));
    // Dumps are made by building, so they bypass the cache (and refresh it).
    if (!g_dump_dir[0] && shader_cache_load(p->key, p->code_size, &b)) {
        InterlockedIncrement(&g_stats.cached);
        finish_program(p, true, b);
        return;
    }
    TranslatedShader t = {};
    if (!translate_program(p, &t)) {
        InterlockedIncrement(&g_stats.failed);
        finish_program(p, false, b);
        return;
    }
    u64 cost = shader_compile_cost(t.hlsl.length);
    u64 room = commit_room();
    TranslatedShader* aside = nullptr;
    AcquireSRWLockExclusive(&g_job_lock);
    bool admitted =
        shader_compile_admitted(g_compile_in_flight, cost, g_schedule.compile_budget, room, p->blocked != 0);
    bool short_of_commit = !admitted && g_compile_in_flight + cost > room;
    if (!admitted) {
        aside = (TranslatedShader*)malloc(sizeof(TranslatedShader));
        // Out of memory or the heap full (more programs than it holds can't be): compiled now.
        if (!aside || !shader_job_heap_push(&g_compiles, {p, nullptr, nullptr}, t.hlsl.length)) {
            free(aside);
            aside = nullptr;
            admitted = true;
        }
    }
    if (admitted) {
        reserve_compile(cost);
    } else {
        *aside = t; // the HLSL goes with it
        p->translation = aside;
        p->compile_cost = cost;
        InterlockedExchange(&p->worker, 0);
        InterlockedExchange((volatile LONG*)&p->state, PROGRAM_TRANSLATED);
        g_stats.compile_waits++;
    }
    ReleaseSRWLockExclusive(&g_job_lock);
    if (admitted) {
        compile_and_finish(w, p, t, cost);
    } else {
        if (short_of_commit)
            LOG_W_ONCE("gpu",
                       "shader compiles no thread is blocked on wait: the system has little commit left "
                       "(%llu MiB to spare)",
                       (unsigned long long)(room >> 20));
        WakeByAddressAll((void*)&p->state);
    }
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
    long compile_waits = g_stats.compile_waits;
    u64 compile_peak = g_stats.compile_peak;
    ReleaseSRWLockExclusive(&g_job_lock);
    if (!report)
        return;
    LARGE_INTEGER now, frequency;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    LOG_I("gpu",
          "shader queue drained (busy %.1f s): %ld programs since the last report; in all %ld from the disk "
          "cache, %ld translated, %ld failed; %ld compiles waited for memory, at most %llu MiB of compiles "
          "estimated at once",
          (f64)(now.QuadPart - start.QuadPart) / (f64)frequency.QuadPart, since, cached, built, failed,
          compile_waits, (unsigned long long)(compile_peak >> 20));
}

static DWORD WINAPI worker_main(void* param) {
    Worker& w = *(Worker*)param;
    for (;;) {
        ShaderJob job;
        AcquireSRWLockExclusive(&g_job_lock);
        ShaderPick pick;
        // Compiles waiting for memory are looked at again now and then: commit other processes and
        // the title give back wakes no one.
        while ((pick = take_job(&job)) == PICK_NOTHING)
            SleepConditionVariableSRW(&g_job_ready, &g_job_lock, g_compiles.count ? 500 : INFINITE, 0);
        bool background = pick != PICK_URGENT;
        g_active++;
        if (background)
            g_background_running++;
        // Queuing wakes one worker, and the end of a job brings back only the worker that ran it: one
        // that finds more work than its own wakes the next.
        bool more = shader_pick(queue_state()) != PICK_NOTHING;
        ReleaseSRWLockExclusive(&g_job_lock);
        if (more)
            WakeConditionVariable(&g_job_ready);

        begin_job(w, !background);
        if (pick == PICK_COMPILE)
            compile_translated(w, job.program);
        else if (job.program)
            build_program(w, job.program, !background);
        else
            job.run(job.context);
        end_job(w);

        AcquireSRWLockExclusive(&g_job_lock);
        g_active--;
        if (background)
            g_background_running--;
        drop_taken_compiles();
        bool drained = !g_active && queues_empty();
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

// Cores rather than logical processors (gpu/shader_schedule.cpp); 0 if Windows doesn't tell.
static u32 count_physical_cores() {
    DWORD size = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &size);
    u8* info = size ? (u8*)malloc(size) : nullptr;
    u32 cores = 0;
    if (info && GetLogicalProcessorInformationEx(RelationProcessorCore,
                                                 (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*)info, &size)) {
        for (DWORD at = 0; at < size; at += ((SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*)(info + at))->Size)
            cores++;
    }
    free(info);
    return cores;
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
    g_urgent.jobs = (ShaderJob*)calloc(JOB_CAPACITY, sizeof(ShaderJob));
    g_programs = (ShaderProgram**)calloc(PROGRAM_TABLE_SIZE, sizeof(ShaderProgram*));
    if (!g_urgent.jobs || !g_programs || !shader_job_heap_init(&g_background, JOB_CAPACITY) ||
        !shader_job_heap_init(&g_compiles, JOB_CAPACITY))
        return false;
    QueryPerformanceFrequency(&g_clock_frequency);
    QueryPerformanceCounter(&g_clock_start);
    shader_wait_budget_init(&g_wait_budget, 0);
    MEMORYSTATUSEX memory = {sizeof(memory)};
    g_schedule =
        shader_schedule_for(count_physical_cores(), GlobalMemoryStatusEx(&memory) ? memory.ullTotalPhys : 0);
    // Compilers first: a pool without them would leave waits for programs hanging.
    for (u32 i = 0; i < g_schedule.workers; i++) {
        Worker& w = g_workers[i];
        w.index = i;
        w.lock = SRWLOCK_INIT;
        if (FAILED(g_dxc_create(CLSID_DxcCompiler, IID_PPV_ARGS(&w.compiler)))) {
            LOG_E("gpu", "cannot create a shader compiler: shaders cannot be translated");
            return false;
        }
    }
    g_ready = true; // jobs can be queued from here on; the workers take them once started
    open_cache(cache_root, g_workers[0].compiler);
    bool yielding = true;
    for (u32 i = 0; i < g_schedule.workers; i++) {
        Worker& w = g_workers[i];
        w.thread = CreateThread(nullptr, 1 << 20, worker_main, &w, CREATE_SUSPENDED, &w.thread_id);
        if (!w.thread)
            FATAL("cannot start shader workers");
        yielding &= set_speed(w, false);
        SetThreadDescription(w.thread, L"sludgemu shader worker");
        g_worker_count = i + 1; // gpu_job_worker and gpu_job_hurry look at started workers only
        ResumeThread(w.thread);
    }
    if (!yielding)
        LOG_W("gpu",
              "shader workers: cannot lower their priority, efficiency mode or memory priority (error %lu); "
              "background work competes with the title",
              GetLastError());
    LOG_I("gpu",
          "shader workers: %u, background work on at most %u of them (lowest priority, efficiency mode), "
          "compiles started while their estimated memory fits %llu MiB",
          g_schedule.workers, g_schedule.background_slots,
          (unsigned long long)(g_schedule.compile_budget >> 20));
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
    // Ordered by code size: the HLSL, and with it the compile, grows with it.
    if (p && !queue({p, nullptr, nullptr}, false, code_size)) {
        InterlockedExchange((volatile LONG*)&p->state, PROGRAM_FAILED);
        WakeByAddressAll((void*)&p->state);
    }
    return p;
}
