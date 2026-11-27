// The shader workers' scheduling decisions: how many workers and how much memory a machine gives them,
// compile admission against the memory budget, the background queue's order (cheapest first, first
// queued first among equals) and which job a worker takes next.

#include <stdlib.h>
#include <string.h>

#include "gpu/shader_schedule.h"
#include "test.h"

static void test_schedule_for_machine() {
    // Two cores left to the title, at most eight workers, half of them for background work.
    ShaderSchedule s = shader_schedule_for(24, GiB(32));
    CHECK(s.workers == SHADER_MAX_WORKERS);
    CHECK(s.background_slots == 4);
    CHECK(s.compile_budget == GiB(2));
    s = shader_schedule_for(8, GiB(16));
    CHECK(s.workers == 6 && s.background_slots == 3);
    CHECK(s.compile_budget == GiB(1));
    // Few cores: still two workers, one of them always free of background work.
    for (u32 cores = 0; cores <= 4; cores++) {
        s = shader_schedule_for(cores, GiB(8));
        CHECK(s.workers == 2 && s.background_slots == 1);
    }
    // Every machine: some background work, and some workers kept free of it.
    for (u32 cores = 0; cores <= 64; cores++) {
        s = shader_schedule_for(cores, GiB(16));
        CHECK(s.workers >= 2 && s.workers <= SHADER_MAX_WORKERS);
        CHECK(s.background_slots >= 1 && s.background_slots < s.workers);
    }
    // The budget has a floor (memory unknown or a small machine) and a ceiling.
    CHECK(shader_schedule_for(8, 0).compile_budget == MiB(512));
    CHECK(shader_schedule_for(8, GiB(4)).compile_budget == MiB(512));
    CHECK(shader_schedule_for(8, GiB(256)).compile_budget == GiB(4));
}

// Commit enough for anything.
#define ROOM (~0ull)

static void test_compile_admission() {
    // The estimate grows with the HLSL; the largest program seen (4 MB of HLSL) took 987 MiB.
    CHECK(shader_compile_cost(0) > 0);
    CHECK(shader_compile_cost(4082777) >= MiB(987));
    CHECK(shader_compile_cost(20000) < MiB(16));
    u64 huge = shader_compile_cost(4082777);
    u64 large = shader_compile_cost(2700000);
    u64 ordinary = shader_compile_cost(30000);
    u64 budget = GiB(1);
    // Nothing running: anything starts, even a compile larger than the whole budget.
    CHECK(shader_compile_admitted(0, huge, MiB(512), ROOM, false));
    // One of the largest running: another waits, ordinary ones fit next to it.
    CHECK(!shader_compile_admitted(huge, large, budget, ROOM, false));
    CHECK(shader_compile_admitted(large, ordinary, budget, ROOM, false));
    CHECK(!shader_compile_admitted(large, large, budget, ROOM, false));
    // On a 32 GiB machine the two largest run at once, not a third large one with them.
    u64 budget32 = shader_schedule_for(24, GiB(32)).compile_budget;
    u64 second = shader_compile_cost(3375245);
    CHECK(shader_compile_admitted(huge, second, budget32, ROOM, false));
    CHECK(!shader_compile_admitted(huge + second, large, budget32, ROOM, false));
    CHECK(shader_compile_admitted(2 * large, large, budget32, ROOM, false));
    CHECK(!shader_compile_admitted(3 * large, large, budget32, ROOM, false));
    // Up to the budget exactly.
    CHECK(shader_compile_admitted(budget - ordinary, ordinary, budget, ROOM, false));
    CHECK(!shader_compile_admitted(budget - ordinary + 1, ordinary, budget, ROOM, false));
    // Something waits for it: always.
    CHECK(shader_compile_admitted(budget, huge, budget, ROOM, true));
    CHECK(shader_compile_admitted(4 * huge, huge, budget, ROOM, true));
    // The system short of commit: compiles nothing waits for wait even when nothing compiles (a
    // compile running the system out of commit stops the title), counting the running ones again.
    CHECK(!shader_compile_admitted(0, huge, budget, huge - 1, false));
    CHECK(shader_compile_admitted(0, huge, budget, huge, false));
    CHECK(!shader_compile_admitted(ordinary, ordinary, budget, 2 * ordinary - 1, false));
    CHECK(shader_compile_admitted(ordinary, ordinary, budget, 2 * ordinary, false));
    CHECK(!shader_compile_admitted(0, ordinary, budget, 0, false));
    // Something waits for it: whatever the commit.
    CHECK(shader_compile_admitted(0, huge, budget, 0, true));
}

static ShaderJob job_with(uintptr_t id) {
    return {nullptr, nullptr, (void*)id};
}

static void test_job_heap_order() {
    ShaderJobHeap heap;
    CHECK(shader_job_heap_init(&heap, 8));
    ShaderJob job;
    CHECK(!shader_job_heap_peek(&heap, &job));
    CHECK(!shader_job_heap_pop(&heap, &job));
    // Cheapest first; equal costs (the quick function jobs at 0) in the order queued.
    const u64 costs[8] = {500, 0, 77696, 0, 1200, 500, 0, 110016};
    for (u32 i = 0; i < 8; i++)
        CHECK(shader_job_heap_push(&heap, job_with(i), costs[i]));
    CHECK(!shader_job_heap_push(&heap, job_with(99), 1)); // full
    const uintptr_t expected[8] = {1, 3, 6, 0, 5, 4, 2, 7};
    CHECK(shader_job_heap_peek(&heap, &job) && (uintptr_t)job.context == 1);
    for (u32 i = 0; i < 8; i++)
        CHECK(shader_job_heap_pop(&heap, &job) && (uintptr_t)job.context == expected[i]);
    CHECK(heap.count == 0);
    // Costs past the order's range only tie with each other.
    CHECK(shader_job_heap_push(&heap, job_with(1), u64(1) << 40));
    CHECK(shader_job_heap_push(&heap, job_with(2), u64(1) << 41));
    CHECK(shader_job_heap_push(&heap, job_with(3), 5));
    CHECK(shader_job_heap_pop(&heap, &job) && (uintptr_t)job.context == 3);
    CHECK(shader_job_heap_pop(&heap, &job) && (uintptr_t)job.context == 1);
    CHECK(shader_job_heap_pop(&heap, &job) && (uintptr_t)job.context == 2);
    shader_job_heap_free(&heap);

    // Against a sort, with jobs going in and out in between.
    CHECK(shader_job_heap_init(&heap, 4096));
    u64 model_order[4096];
    uintptr_t model_id[4096];
    u32 model_count = 0;
    u32 seed = 12345;
    uintptr_t next_id = 0;
    bool same = true;
    for (u32 step = 0; step < 20000 && same; step++) {
        seed = seed * 1103515245 + 12345;
        bool push = model_count == 0 || (model_count < 4096 && (seed >> 16) % 3 != 0);
        if (push) {
            seed = seed * 1103515245 + 12345;
            u64 cost = (seed >> 16) % 64;
            // The model's order: cost, then the sequence (the id counts pushes).
            model_order[model_count] = cost << 32 | next_id;
            model_id[model_count++] = next_id;
            same &= shader_job_heap_push(&heap, job_with(next_id++), cost);
        } else {
            u32 first = 0;
            for (u32 i = 1; i < model_count; i++)
                if (model_order[i] < model_order[first])
                    first = i;
            same &= shader_job_heap_pop(&heap, &job) && (uintptr_t)job.context == model_id[first];
            model_order[first] = model_order[--model_count];
            model_id[first] = model_id[model_count];
        }
    }
    CHECK(same);
    CHECK(heap.count == model_count);
    shader_job_heap_free(&heap);
}

static ShaderQueueState idle_queues() {
    ShaderQueueState s;
    memset(&s, 0, sizeof(s));
    s.background_slots = 2;
    s.compile_budget = GiB(1);
    s.commit_room = ROOM;
    return s;
}

static void test_pick() {
    ShaderQueueState s = idle_queues();
    CHECK(shader_pick(s) == PICK_NOTHING);
    // Urgent work first, whatever runs in the background or waits for memory.
    s.urgent_queued = 1;
    s.background_queued = 5;
    s.background_running = 2;
    s.compile_waiting = true;
    s.compile_cost = MiB(100);
    CHECK(shader_pick(s) == PICK_URGENT);
    // Background work only on a free slot.
    s.urgent_queued = 0;
    CHECK(shader_pick(s) == PICK_NOTHING);
    s.background_running = 1;
    // A compile that waited for memory, once it fits, ahead of queued jobs (it holds its HLSL).
    CHECK(shader_pick(s) == PICK_COMPILE);
    s.compile_in_flight = GiB(1) - MiB(50);
    CHECK(shader_pick(s) == PICK_BACKGROUND);
    s.background_queued = 0;
    CHECK(shader_pick(s) == PICK_NOTHING);
    s.compile_in_flight = GiB(1) - MiB(100);
    CHECK(shader_pick(s) == PICK_COMPILE);
    // One larger than the whole budget once nothing compiles.
    s.compile_cost = GiB(3);
    CHECK(shader_pick(s) == PICK_NOTHING);
    s.compile_in_flight = 0;
    CHECK(shader_pick(s) == PICK_COMPILE);
    // Not while the system is short of commit, even with nothing compiling; queued jobs still run.
    s.commit_room = GiB(3) - 1;
    CHECK(shader_pick(s) == PICK_NOTHING);
    s.background_queued = 1;
    CHECK(shader_pick(s) == PICK_BACKGROUND);
    s.commit_room = GiB(3);
    CHECK(shader_pick(s) == PICK_COMPILE);
    // No slot: still nothing, even for a compile that fits.
    s.background_running = 2;
    CHECK(shader_pick(s) == PICK_NOTHING);
}

void test_shader_schedule() {
    test_schedule_for_machine();
    test_compile_admission();
    test_job_heap_order();
    test_pick();
}
