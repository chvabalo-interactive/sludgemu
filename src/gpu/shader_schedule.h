// How the shader workers share the machine with the title: how many there are, how much of them work
// nothing waits for may take, which job runs next, and how much memory compiles may hold at once. Kept
// free of threads and Windows calls so the decisions can be tested (tests/shader_schedule_test.cpp);
// gpu/shader_programs runs them.
//
// Background work (programs titles merely created, pipelines built ahead) yields to the title: it may
// take only part of the workers, so work a draw waits for always finds one free (a running compile
// can't be interrupted), it runs cheapest first, so most programs are cached early and the few huge
// ones (tens of seconds and about a gigabyte of compiler memory each) come last, and compiles start
// only while their estimated memory fits a budget. Work a draw needs is never held back, and no
// background job starts while a title thread waits; its compiles, though, pass the memory budget only
// while a title thread is blocked on them (draws that skip what isn't built make it urgent without
// blocking, so urgent work alone may be many of the largest compiles at once).
//
// Title threads wait for what their draws need for a bounded time only (the wait budget below).

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

// Whether a compile estimated at `cost` may start while compiles estimated at `in_flight` run. Ones a
// title thread is `blocked` on always may; others while the sum fits the budget, or when no compile runs
// (one larger than the whole budget would otherwise never start), and only while the sum fits
// `commit_room`, the commit the system can spare (a budget sized from the machine's memory doesn't know
// what other processes and the title hold; running the system out of commit stops the title).
bool shader_compile_admitted(u64 in_flight, u64 cost, u64 budget, u64 commit_room, bool blocked);

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
    bool title_waiting;    // a title thread is blocked on a program or pipeline
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

// Urgent work first. Background work only on a free background slot and while no title thread waits:
// a background job can't be stopped once started, so one started while the title waits would hold a
// worker the next urgent job may need, and takes power and memory bandwidth from the build the title
// waits for (on laptops the cores share one power limit). Waits are bounded, so this holds background
// work back for a fraction of the time at most. Then a compile that waited for memory as soon as it
// fits (it was translated already, and what waits holds its HLSL), else the cheapest queued job.
ShaderPick shader_pick(const ShaderQueueState& s);

// --- the wait budget ---
//
// A draw or dispatch whose program or desktop pipeline isn't built yet waits for it: one pass of an
// effect left out looks broken, a late frame only late. But titles take a thread blocked for long for
// a hang (watchdogs stop the title after some seconds without frame progress), and the largest
// programs take seconds to compile, tens of seconds on a slow machine. So the wall time during which
// any title thread waits is budgeted, the way a token bucket meters a rate: a burst for the first
// sight of a scene, refilled at a share of the time passing. Waits that overlap draw on it once (they
// block the title once, not once each), so all threads waiting at once wait until the same deadline.
// Past it, the draw is skipped and what it needs stays urgent, ready a frame or a few later.
//
// Dispatches may overdraw the budget, into a reserve below zero, which draws then refill before they
// wait again: compute work tends to feed state that later passes and frames build on (simulations,
// histories, tables computed once at load), where a skipped draw usually costs one frame's pixels.
//
// Bounds: a stall (all waits back to back) lasts at most (BURST + RESERVE) * SHARE / (SHARE - 1),
// 4 s with the values below, and over any stretch of time T the title waits at most
// BURST + RESERVE + T / SHARE (plus the wake-up latency of waits ending at their deadline, which is
// paid back). Rather than a cap per wait (a frame has many draws, recorded on several threads, each of
// which may wait) or a budget per frame (titles also record while they present nothing, as when they
// load), this bounds what watchdogs measure: wall time.

// A second: one hitch when a scene is first seen, with all its new pipelines drawn. Most desktop
// pipelines are made in tens of milliseconds and most programs in tens to hundreds.
#define SHADER_WAIT_BURST_US 1000000
// Two seconds more for dispatches.
#define SHADER_WAIT_RESERVE_US 2000000
// A quarter of the time passing: while the workers can't keep up, the title still runs three quarters
// of the time, and its frames miss only what isn't built.
#define SHADER_WAIT_REFILL_SHARE 4

struct ShaderWaitBudget {
    s64 balance_us; // waiting time left; below zero after dispatches overdrew it or waits overran
    u64 stamp_us;   // when `balance_us` was brought up to date
    u32 waiters;    // waits running
};

// A full budget at `now_us` (microseconds on a monotonic clock).
void shader_wait_budget_init(ShaderWaitBudget* b, u64 now_us);
// A wait starts at `now_us`, for a dispatch or a draw: the time it may wait until, the same for every
// wait running (nonzero), and it counts as running until shader_wait_budget_end; or 0, the budget
// being spent, and it doesn't wait.
u64 shader_wait_budget_begin(ShaderWaitBudget* b, u64 now_us, bool dispatch);
// A wait shader_wait_budget_begin gave a deadline ends at `now_us`, at the deadline or before (or a
// little after: the overrun is paid back).
void shader_wait_budget_end(ShaderWaitBudget* b, u64 now_us);
