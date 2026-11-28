// The shader workers' scheduling decisions: how many workers and how much memory a machine gives them,
// compile admission against the memory budget, the background queue's order (cheapest first, first
// queued first among equals), which job a worker takes next and the budget of the title's waits.

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
    // A title thread waiting: urgent work only, whatever slots are free.
    s.background_running = 0;
    s.title_waiting = true;
    CHECK(shader_pick(s) == PICK_NOTHING);
    s.urgent_queued = 1;
    CHECK(shader_pick(s) == PICK_URGENT);
    s.urgent_queued = 0;
    s.title_waiting = false;
    CHECK(shader_pick(s) == PICK_COMPILE);
}

#define BURST   SHADER_WAIT_BURST_US
#define RESERVE SHADER_WAIT_RESERVE_US
#define SHARE   SHADER_WAIT_REFILL_SHARE

// How long waits starting with `balance` left may run back to back until it reaches `floor`.
static u64 drain_time(s64 balance, s64 floor) {
    return (u64)(balance - floor) * SHARE / (SHARE - 1);
}

static void test_wait_budget() {
    ShaderWaitBudget b;
    u64 t = 5000000;
    shader_wait_budget_init(&b, t);
    // A full burst: a draw waits until it is spent, with the refill running meanwhile.
    u64 deadline = shader_wait_budget_begin(&b, t, false);
    CHECK(deadline == t + drain_time(BURST, 0));
    CHECK(b.waiters == 1);
    // A wait starting meanwhile on another thread gets the same deadline: the title is blocked once.
    u64 other = shader_wait_budget_begin(&b, t + 300000, false);
    CHECK(other + 1 >= deadline && other <= deadline + 1);
    shader_wait_budget_end(&b, t + 400000);
    CHECK(b.waiters == 1);
    shader_wait_budget_end(&b, deadline);
    CHECK(b.waiters == 0);
    CHECK(b.balance_us >= -1 && b.balance_us <= 1);
    // Spent: draws don't wait; dispatches still may, from the reserve.
    t = deadline;
    CHECK(shader_wait_budget_begin(&b, t, false) == 0);
    CHECK(b.waiters == 0);
    deadline = shader_wait_budget_begin(&b, t, true);
    CHECK(deadline + 2 >= t + drain_time(0, -RESERVE) && deadline <= t + drain_time(0, -RESERVE) + 2);
    // A draw starting while the dispatch overdraws doesn't wait (it would wait past zero).
    CHECK(shader_wait_budget_begin(&b, t + 100000, false) == 0);
    // The dispatch ends early: what it didn't use stays.
    shader_wait_budget_end(&b, t + 750000);
    s64 expected = 0 - 750000 + 750000 / SHARE;
    CHECK(b.balance_us >= expected - 1 && b.balance_us <= expected + 1);
    // Refilled at a share of the time passing: back to zero, then waits for draws again.
    t += 750000;
    u64 back = t + (u64)(-expected) * SHARE;
    CHECK(shader_wait_budget_begin(&b, back - 1000, false) == 0);
    t = back + 400000; // 100 ms of waiting time earned
    deadline = shader_wait_budget_begin(&b, t, false);
    u64 want = t + drain_time(400000 / SHARE, 0);
    CHECK(deadline + 2 >= want && deadline <= want + 2);
    // A wait overrunning its deadline (late wake-up) pays it back before the next one.
    shader_wait_budget_end(&b, deadline + 20000);
    CHECK(b.balance_us < 0 && b.balance_us > -20000);
    CHECK(shader_wait_budget_begin(&b, deadline + 20000, false) == 0);
    CHECK(shader_wait_budget_begin(&b, deadline + 20000 + 20000 * SHARE, false) != 0);
    shader_wait_budget_end(&b, deadline + 20000 + 20000 * SHARE);
    // The refill stops at the burst, however long nothing waits.
    t = deadline + 3600000000ull;
    deadline = shader_wait_budget_begin(&b, t, false);
    CHECK(deadline == t + drain_time(BURST, 0));
    shader_wait_budget_end(&b, t + 1000);
    // A clock read before another thread took the lock (lagging the stamp) changes nothing.
    s64 balance = b.balance_us;
    shader_wait_budget_end(&b, t);
    CHECK(b.balance_us == balance && b.waiters == 0);
}

// Threads waiting for builds of random lengths, draws and dispatches, against the bounds the budget
// promises: no stall longer than draining the burst and the reserve, and no more waiting over any 30 s
// than the burst, the reserve and the refill. Simulated in steps of a millisecond (a wait ends at the
// first step past its deadline: an overrun of up to a step, paid back).
#define SIM_STEP_US  1000
#define SIM_STEPS    600000 // ten minutes
#define SIM_THREADS  5
#define SIM_WINDOW   30000 // steps
#define SIM_SLACK_US 20000

static void test_wait_budget_bounds() {
    ShaderWaitBudget b;
    shader_wait_budget_init(&b, 0);
    struct Thread {
        u64 until; // waiting: the build's end or the deadline, whichever comes first; else the next wait
        bool waiting;
    } threads[SIM_THREADS] = {};
    static u8 blocked[SIM_STEPS];
    u32 seed = 777;
    u64 run = 0, longest_run = 0, skipped = 0, waits = 0;
    for (u32 step = 0; step < SIM_STEPS; step++) {
        u64 now = (u64)step * SIM_STEP_US;
        bool any = false;
        for (u32 i = 0; i < SIM_THREADS; i++) {
            Thread& th = threads[i];
            if (th.waiting && now >= th.until) {
                shader_wait_budget_end(&b, now);
                th.waiting = false;
                seed = seed * 1103515245 + 12345;
                th.until = now + (seed >> 16) % 40 * 1000; // records for up to 40 ms, then needs a build
            }
            if (!th.waiting && now >= th.until) {
                seed = seed * 1103515245 + 12345;
                // Builds from a millisecond to 20 s, mostly short.
                u32 r = (seed >> 16) % 100;
                u64 build = r < 70   ? (u64)(r + 1) * 2000
                            : r < 95 ? (u64)(r - 69) * 100000
                                     : (u64)(r - 94) * 4000000;
                bool dispatch = (seed >> 8) & 1;
                u64 deadline = shader_wait_budget_begin(&b, now, dispatch);
                if (deadline) {
                    th.waiting = true;
                    th.until = now + build < deadline ? now + build : deadline;
                    waits++;
                } else {
                    skipped++;
                    th.until = now + 33000; // tries again next frame
                }
            }
            any |= th.waiting;
        }
        blocked[step] = any;
        run = any ? run + 1 : 0;
        if (run > longest_run)
            longest_run = run;
    }
    CHECK(waits > 1000 && skipped > 1000); // the budget was spent often, and refilled often
    CHECK(longest_run * SIM_STEP_US <= drain_time(BURST, -RESERVE) + SIM_SLACK_US);
    u64 in_window = 0, most = 0;
    for (u32 step = 0; step < SIM_STEPS; step++) {
        in_window += blocked[step];
        if (step >= SIM_WINDOW)
            in_window -= blocked[step - SIM_WINDOW];
        if (in_window > most)
            most = in_window;
    }
    CHECK(most * SIM_STEP_US <= BURST + RESERVE + (u64)SIM_WINDOW * SIM_STEP_US / SHARE + SIM_SLACK_US);
}

void test_shader_schedule() {
    test_schedule_for_machine();
    test_compile_admission();
    test_job_heap_order();
    test_pick();
    test_wait_budget();
    test_wait_budget_bounds();
}
