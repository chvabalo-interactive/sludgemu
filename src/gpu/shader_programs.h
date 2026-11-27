// Translated console shader programs: translation to HLSL and compilation to DXIL on worker
// threads, cached by program code and translation inputs.
//
// Programs are requested when pipelines are created and built in the background; draws using a
// pipeline whose programs are not ready yet are skipped. The workers also run other GPU jobs that
// must wait for programs (desktop pipeline creation).

#pragma once

#include "common/types.h"
#include "gpu/gpu.h"
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
    ShaderStage stage;
    u64 key;

    // Ready programs: DXIL and what the pipeline needs about the program.
    void* dxil;
    usize dxil_size;
    u32 vertex_element_mask;
    u32 element_format[SHADER_MAX_VERTEX_ELEMENTS];
    u32 ps_target_mask;
    bool ps_writes_depth;
    u32 sampler_count; // inline samplers: static samplers in the pipeline's root signature
    InlineSampler samplers[SHADER_MAX_INLINE_SAMPLERS];
    u16 table_reach[SHADER_MAX_ROOT_PARAMETERS]; // see TranslatedShader
    u32 vs_param_mask;                           // vertex shaders: the parameters they export

    // The translation inputs, kept until the program is built.
    u8* code;
    u32 code_size;
    ShaderInfo info;
};

// Starts the workers, translating for a device that runs `target`. Returns false if the shader compiler
// is missing.
bool shader_programs_init(const ShaderTarget& target);

// The program for `code` translated with `info`: an existing one, or a new one queued for building.
// Programs live until shutdown. Returns nullptr only when out of memory.
ShaderProgram* shader_program_get(const void* code, u32 code_size, const ShaderInfo& info);

// The geometry shader drawing console rectangle lists: each triangle of three rectangle corners
// becomes the whole rectangle, for vertex shaders exporting `param_mask`. Compiled on first use and
// kept; nullptr (logged) if it can't be.
const D3D12_SHADER_BYTECODE* shader_rect_list_gs(u32 param_mask);

// A draw is waiting for `program`: build it before the programs merely created.
void shader_program_hurry(ShaderProgram* program);

// Runs `job(context)` on a worker thread, before queued programs (jobs are for draws waiting).
void gpu_job_queue(void (*job)(void* context), void* context);
