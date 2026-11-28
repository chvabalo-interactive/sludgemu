#include "gpu/shader_schedule.h"

#include <stdlib.h>

// --- the machine -------------------------------------------------------------------------------------

ShaderSchedule shader_schedule_for(u32 physical_cores, u64 physical_memory) {
    ShaderSchedule s;
    // A core each, leaving two to the title's busiest threads. Cores rather than logical processors: a
    // compile on the other half of a core the title runs on takes much of that core's speed and adds
    // little of its own.
    if (physical_cores == 0)
        physical_cores = 4;
    s.workers = physical_cores > 4 ? physical_cores - 2 : 2;
    if (s.workers > SHADER_MAX_WORKERS)
        s.workers = SHADER_MAX_WORKERS;
    // Half for background work: the rest are free for work draws wait for whatever runs in the
    // background, which can't be interrupted (a huge compile runs for a minute).
    s.background_slots = s.workers / 2;
    if (s.background_slots == 0)
        s.background_slots = 1;
    // A sixteenth of the memory: 1 GiB on a 16 GiB machine, enough for one of the largest programs
    // at a time, or for several ordinary ones next to it.
    s.compile_budget = physical_memory / 16;
    if (s.compile_budget < MiB(512))
        s.compile_budget = MiB(512);
    if (s.compile_budget > GiB(4))
        s.compile_budget = GiB(4);
    return s;
}

// --- compile admission -------------------------------------------------------------------------------

// Measured with DXC at -O3 (peak private bytes against HLSL size): from about 100 to 250 bytes per byte
// of HLSL, 250 for the largest program seen (4 MB of HLSL, 987 MiB). The estimate takes the worst, so
// the budget holds whatever the program is like; the base is the compiler's own.
#define COMPILE_BASE_BYTES     MiB(4)
#define COMPILE_BYTES_PER_BYTE 256

u64 shader_compile_cost(u64 hlsl_bytes) {
    return COMPILE_BASE_BYTES + hlsl_bytes * COMPILE_BYTES_PER_BYTE;
}

bool shader_compile_admitted(u64 in_flight, u64 cost, u64 budget, u64 commit_room, bool blocked) {
    if (blocked)
        return true;
    // The compiles running are counted against the room again, as most of what they will take is not
    // committed yet: a compile grows to its peak over its run.
    return in_flight + cost <= commit_room && (in_flight == 0 || in_flight + cost <= budget);
}

// --- the background queue ----------------------------------------------------------------------------

// The order: cost in the high 24 bits (capped: 16 MiB of code or HLSL, which only makes the costliest
// jobs equal), a sequence
// number below, so that equal costs run first queued first.
#define ORDER_SEQUENCE_BITS 40
#define ORDER_COST_MAX      ((1ull << (64 - ORDER_SEQUENCE_BITS)) - 1)

static u64 order_of(u64 cost, u64 sequence) {
    if (cost > ORDER_COST_MAX)
        cost = ORDER_COST_MAX;
    return cost << ORDER_SEQUENCE_BITS | (sequence & ((1ull << ORDER_SEQUENCE_BITS) - 1));
}

bool shader_job_heap_init(ShaderJobHeap* heap, u32 capacity) {
    heap->entries = (ShaderJobHeap::Entry*)malloc((usize)capacity * sizeof(ShaderJobHeap::Entry));
    heap->count = 0;
    heap->capacity = heap->entries ? capacity : 0;
    heap->sequence = 0;
    return heap->entries != nullptr;
}

void shader_job_heap_free(ShaderJobHeap* heap) {
    free(heap->entries);
    heap->entries = nullptr;
    heap->count = heap->capacity = 0;
}

bool shader_job_heap_push(ShaderJobHeap* heap, const ShaderJob& job, u64 cost) {
    if (heap->count == heap->capacity)
        return false;
    ShaderJobHeap::Entry entry = {order_of(cost, heap->sequence++), job};
    // Up from the new leaf while the parent comes later.
    u32 i = heap->count++;
    while (i > 0) {
        u32 parent = (i - 1) / 2;
        if (heap->entries[parent].order <= entry.order)
            break;
        heap->entries[i] = heap->entries[parent];
        i = parent;
    }
    heap->entries[i] = entry;
    return true;
}

bool shader_job_heap_peek(const ShaderJobHeap* heap, ShaderJob* job) {
    if (heap->count == 0)
        return false;
    *job = heap->entries[0].job;
    return true;
}

bool shader_job_heap_pop(ShaderJobHeap* heap, ShaderJob* job) {
    if (heap->count == 0)
        return false;
    *job = heap->entries[0].job;
    ShaderJobHeap::Entry last = heap->entries[--heap->count];
    // The last leaf down from the root while a child comes first.
    u32 i = 0;
    for (;;) {
        u32 child = 2 * i + 1;
        if (child >= heap->count)
            break;
        if (child + 1 < heap->count && heap->entries[child + 1].order < heap->entries[child].order)
            child++;
        if (last.order <= heap->entries[child].order)
            break;
        heap->entries[i] = heap->entries[child];
        i = child;
    }
    if (heap->count)
        heap->entries[i] = last;
    return true;
}

// --- picking ------------------------------------------------------------------------------------------

ShaderPick shader_pick(const ShaderQueueState& s) {
    if (s.urgent_queued)
        return PICK_URGENT;
    if (s.title_waiting || s.background_running >= s.background_slots)
        return PICK_NOTHING;
    if (s.compile_waiting &&
        shader_compile_admitted(s.compile_in_flight, s.compile_cost, s.compile_budget, s.commit_room, false))
        return PICK_COMPILE;
    if (s.background_queued)
        return PICK_BACKGROUND;
    return PICK_NOTHING;
}

// --- the wait budget ----------------------------------------------------------------------------------

void shader_wait_budget_init(ShaderWaitBudget* b, u64 now_us) {
    b->balance_us = SHADER_WAIT_BURST_US;
    b->stamp_us = now_us;
    b->waiters = 0;
}

// Brings the balance up to `now_us`: refilled all along (up to the burst), drained while waits run.
static void advance(ShaderWaitBudget* b, u64 now_us) {
    if (now_us <= b->stamp_us)
        return; // threads read the clock before taking the lock, so `now_us` may lag a little
    s64 elapsed = (s64)(now_us - b->stamp_us);
    b->balance_us += elapsed / SHADER_WAIT_REFILL_SHARE;
    if (b->waiters)
        b->balance_us -= elapsed;
    if (b->balance_us > SHADER_WAIT_BURST_US)
        b->balance_us = SHADER_WAIT_BURST_US;
    b->stamp_us = now_us;
}

u64 shader_wait_budget_begin(ShaderWaitBudget* b, u64 now_us, bool dispatch) {
    advance(b, now_us);
    s64 floor = dispatch ? -(s64)SHADER_WAIT_RESERVE_US : 0;
    if (b->balance_us <= floor)
        return 0;
    b->waiters++;
    // While waits run the balance drains at the wall time less the refill, so it reaches the floor
    // after (balance - floor) * SHARE / (SHARE - 1); at least a microsecond, so never 0.
    u64 left = (u64)(b->balance_us - floor);
    return b->stamp_us + left * SHADER_WAIT_REFILL_SHARE / (SHADER_WAIT_REFILL_SHARE - 1);
}

void shader_wait_budget_end(ShaderWaitBudget* b, u64 now_us) {
    advance(b, now_us);
    if (b->waiters)
        b->waiters--;
}
