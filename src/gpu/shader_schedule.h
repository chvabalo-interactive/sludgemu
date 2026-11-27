// How the shader workers share the machine with the title: how many there are, how much of them work
// nothing waits for may take, which job runs next, and how much memory compiles may hold at once. Kept
// free of threads and Windows calls so the decisions can be tested (tests/shader_schedule_test.cpp);
// gpu/shader_programs runs them.
//
// Background work (programs titles merely created, pipelines built ahead) yields to the title: it may
// take only part of the workers, so work a draw waits for always finds one free (a running compile
// can't be interrupted), it runs cheapest first, so most programs are cached early and the few huge
// ones (tens of seconds and about a gigabyte of compiler memory each) come last, and compiles start
// only while their estimated memory fits a budget. Work something waits for is never held back.

#pragma once

#include "common/types.h"

struct ShaderProgram;

#define SHADER_MAX_WORKERS 8

// What the workers may use, derived from the machine.
struct ShaderSchedule {
    u32 workers;          // worker threads
    u32 background_slots; // background jobs running at once at most (fewer than `workers`)
    u64 compile_budget;   // estimated memory of the compiles running at once, in bytes
};

ShaderSchedule shader_schedule_for(u32 physical_cores, u64 physical_memory);

// The memory a compile of `hlsl_bytes` of HLSL is estimated to take at most.
u64 shader_compile_cost(u64 hlsl_bytes);

// Whether a compile estimated at `cost` may start while compiles estimated at `in_flight` run. Urgent
// ones always may; others while the sum fits the budget, or when no compile runs (one larger than the
// whole budget would otherwise never start), and only while the sum fits `commit_room`, the commit the
// system can spare (a budget sized from the machine's memory doesn't know what other processes and the
// title hold; running the system out of commit stops the title).
bool shader_compile_admitted(u64 in_flight, u64 cost, u64 budget, u64 commit_room, bool urgent);

// A job: a program to build, a program whose compile waited for memory, or a function to run.
struct ShaderJob {
    ShaderProgram* program;
    void (*run)(void* context);
    void* context;
};

// Background jobs in order of cost, cheapest first; equal costs in the order queued.
struct ShaderJobHeap {
    struct Entry {
        u64 order; // cost in the high bits, then a sequence number
        ShaderJob job;
    };
    Entry* entries;
    u32 count;
    u32 capacity;
    u64 sequence;
};

// `capacity` entries; false if out of memory.
bool shader_job_heap_init(ShaderJobHeap* heap, u32 capacity);
void shader_job_heap_free(ShaderJobHeap* heap);
// False if the heap is full.
bool shader_job_heap_push(ShaderJobHeap* heap, const ShaderJob& job, u64 cost);
// The cheapest job, or false if there is none.
bool shader_job_heap_peek(const ShaderJobHeap* heap, ShaderJob* job);
bool shader_job_heap_pop(ShaderJobHeap* heap, ShaderJob* job);

// What the queues hold and run, for picking a worker's next job.
struct ShaderQueueState {
    u32 urgent_queued;      // jobs something waits for
    u32 background_queued;  // jobs nothing waits for yet
    u32 background_running; // background jobs (and compiles that waited for memory) running
    u32 background_slots;
    bool compile_waiting;  // a translated program waits for memory to compile in
    u64 compile_cost;      // the cheapest such program's estimate
    u64 compile_in_flight; // the estimates of the compiles running
    u64 compile_budget;
    u64 commit_room; // the commit the system can spare for compiles nothing waits for
};

enum ShaderPick : u32 {
    PICK_NOTHING,    // sleep until something changes
    PICK_URGENT,     // the next urgent job
    PICK_BACKGROUND, // the cheapest background job
    PICK_COMPILE,    // the cheapest program waiting for memory, which now fits
};

// Urgent work first. Background work only on a free background slot: a compile that waited for memory
// as soon as it fits (it was translated already, and what waits holds its HLSL), else the cheapest
// queued job.
ShaderPick shader_pick(const ShaderQueueState& s);
