// Translated console shader programs: translation to HLSL and compilation to DXIL on worker
// threads, cached by program code and translation inputs, in memory and on disk (gpu/shader_cache).
//
// Programs are requested when pipelines are created and built in the background. A draw that needs a
// program not built yet waits for it (shader_program_wait): a console never skips a draw, and a frame
// missing one pass of a multi-pass effect looks broken, where a late frame only looks late. The workers
// also run other GPU jobs (desktop pipeline creation).
//
// Workers never wait for threads outside the pool, so waiting for them can't deadlock. One worker
// takes only urgent work (what draws wait for), so a wait never sits behind background work.

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
};

struct ShaderProgram {
    volatile u32 state;   // ShaderProgramState; READY/FAILED are final
    volatile long urgent; // queued again in the urgent lane
    volatile long worker; // the worker building it (index + 1), 0 while none
    ShaderStage stage;
    u64 key;

    ShaderBinary binary; // ready programs

    // The translation inputs, kept until the program is built.
    u8* code;
    u32 code_size;
    ShaderInfo info;
};

// Starts the workers, translating for a device that runs `target`, with the disk cache under
// `cache_root` (nullptr: none). Returns false if the shader compiler is missing.
bool shader_programs_init(const ShaderTarget& target, const wchar_t* cache_root);

// The program for `code` translated with `info`: an existing one, or a new one queued for building.
// Programs live until shutdown. Returns nullptr only when out of memory.
ShaderProgram* shader_program_get(const void* code, u32 code_size, const ShaderInfo& info);

// Waits until `program` is built, ahead of the programs merely created; true if it is ready, false if
// it failed. Returns at once when it is final. Not for worker threads (jobs use
// shader_program_wait_started).
bool shader_program_wait(ShaderProgram* program);

// For jobs on worker threads: waits for `program` if a worker is building it. False if no worker has
// started it yet (the job tries again later, rather than wait for work it might be holding up).
bool shader_program_wait_started(ShaderProgram* program);

// The geometry shader drawing console rectangle lists: each triangle of three rectangle corners
// becomes the whole rectangle, for vertex shaders exporting `param_mask`. Compiled on first use and
// kept; nullptr (logged) if it can't be.
const D3D12_SHADER_BYTECODE* shader_rect_list_gs(u32 param_mask);

// Runs `job(context)` on a worker thread: urgent jobs (something waits for them) before queued
// programs, background jobs after them. False (logged) if the queue is full; the job won't run.
bool gpu_job_queue(void (*job)(void* context), void* context);
bool gpu_job_queue_background(void (*job)(void* context), void* context);
