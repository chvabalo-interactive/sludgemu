// Translated console shader programs: translation to HLSL and compilation to DXIL on worker
// threads, cached by program code and translation inputs, in memory and on disk (gpu/shader_cache).
//
// Programs are requested when pipelines are created and built in the background. A draw that needs a
// program not built yet waits for it (shader_program_wait): a console never skips a draw, and a frame
// missing one pass of a multi-pass effect looks broken, where a late frame only looks late. The workers
// also run other GPU jobs (desktop pipeline creation).
//
// Workers never wait for threads outside the pool, so waiting for them can't deadlock. Background
// work yields to the title and takes only part of the workers, so work a draw waits for always finds
// one (gpu/shader_schedule.h has the rules).

#pragma once

#include "common/types.h"
#include "gpu/gpu.h"
#include "gpu/shader_cache.h"
#include "shader/translate.h"

enum ShaderProgramState : u32 {
    PROGRAM_PENDING,
    PROGRAM_READY,
    PROGRAM_FAILED,
    PROGRAM_BUILDING,
    PROGRAM_TRANSLATED, // translated in the background; the compile waits for memory to run in
};

struct ProgramThen;

struct ShaderProgram {
    volatile u32 state;   // ShaderProgramState; READY/FAILED are final
    volatile long worker; // the job building it (gpu_job_worker), 0 while none
    ShaderStage stage;
    u64 key;

    ShaderBinary binary; // ready programs

    // The translation inputs, kept until the program is built.
    u8* code;
    u32 code_size;
    ShaderInfo info;

    // Under the job lock (gpu/shader_programs.cpp):
    bool urgent;                   // something waits for it: its compile starts whatever the memory
    bool urgent_queued;            // queued in the urgent lane
    TranslatedShader* translation; // PROGRAM_TRANSLATED: the HLSL to compile
    u64 compile_cost;              // PROGRAM_TRANSLATED: the compile's estimated memory
    ProgramThen* then;             // jobs to queue once it's built (shader_program_then)
};

// Starts the workers, translating for a device that runs `target`, with the disk cache under
// `cache_root` (nullptr: none). Returns false if the shader compiler is missing.
bool shader_programs_init(const ShaderTarget& target, const wchar_t* cache_root);

// The program for `code` translated with `info`: an existing one, or a new one queued for building.
// Programs live until shutdown. Returns nullptr only when out of memory.
ShaderProgram* shader_program_get(const void* code, u32 code_size, const ShaderInfo& info);

// Waits until `program` is built, ahead of the programs merely created; true if it is ready, false if
// it failed. Returns at once when it is final. Not for worker threads (jobs use shader_program_then).
bool shader_program_wait(ShaderProgram* program);

// For jobs that need `program` built: queues `run(context)` as a background job once it is (ready or
// failed) and returns true; false if it already is (or out of memory), and the caller goes on. Jobs
// never wait for programs on a worker: the worker might be what the program waits for.
bool shader_program_then(ShaderProgram* program, void (*run)(void* context), void* context);

// The geometry shader drawing console rectangle lists: each triangle of three rectangle corners
// becomes the whole rectangle, for vertex shaders exporting `param_mask`. Compiled on first use and
// kept; nullptr (logged) if it can't be.
const D3D12_SHADER_BYTECODE* shader_rect_list_gs(u32 param_mask);

// Runs `job(context)` on a worker thread: urgent jobs (something waits for them) first, at full speed;
// background jobs (quick ones) ahead of the programs queued in the background. False (logged) if the
// queue is full; the job won't run.
bool gpu_job_queue(void (*job)(void* context), void* context);
bool gpu_job_queue_background(void (*job)(void* context), void* context);

// The job the calling worker runs (nonzero), or 0 for threads outside the pool.
u32 gpu_job_worker();
// Something waits for `job` (from gpu_job_worker): if it is a background job still running, its
// worker runs at full speed until the job ends.
void gpu_job_hurry(u32 job);
