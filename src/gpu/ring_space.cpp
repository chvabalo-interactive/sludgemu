#include "gpu/ring_space.h"

void ring_space_reset(RingSpace* s, u64 size) {
    s->size = size;
    s->head = 0;
    s->first = s->count = 0;
    s->peak = 0;
    s->takes = 0;
}

void ring_space_reclaim(RingSpace* s, u64 done) {
    while (s->count && s->uses[s->first].submission && s->uses[s->first].submission <= done) {
        s->first = (s->first + 1) % RING_SPACE_MAX_USES;
        s->count--;
    }
    if (!s->count)
        s->head = 0;
}

// Where `bytes` fit between the head and the oldest region in use, or ~0.
static u64 place(const RingSpace* s, u64 bytes, u64 alignment) {
    if (!s->count)
        return bytes <= s->size ? 0 : ~0ull;
    u64 tail = s->uses[s->first].start;
    u64 start = ALIGN_UP(s->head, alignment);
    if (tail < s->head) { // in use: [tail, head)
        if (start + bytes <= s->size)
            return start;
        return bytes <= tail ? 0 : ~0ull; // wrap around
    }
    return start + bytes <= tail ? start : ~0ull; // in use: [tail, end) and [0, head)
}

bool ring_space_take(RingSpace* s, u64 bytes, u64 alignment, u64* offset) {
    u64 start = place(s, bytes, alignment);
    if (start == ~0ull)
        return false;
    RingSpace::Use* last = s->count ? &s->uses[(s->first + s->count - 1) % RING_SPACE_MAX_USES] : nullptr;
    if (last && !last->submission && start >= last->end) {
        last->end = start + bytes; // the same submission's regions, side by side
    } else if (s->count == RING_SPACE_MAX_USES) {
        return false;
    } else {
        s->uses[(s->first + s->count) % RING_SPACE_MAX_USES] = {start, start + bytes, 0};
        s->count++;
    }
    s->head = start + bytes;
    *offset = start;
    // In use: from the oldest region to the head, around the end when it wrapped (the bytes skipped at
    // the end count too: the ring needed them).
    u64 tail = s->uses[s->first].start;
    u64 used = s->head > tail ? s->head - tail : s->size - tail + s->head;
    s->peak = used > s->peak ? used : s->peak;
    s->takes++;
    return true;
}

void ring_space_submitted(RingSpace* s, u64 number) {
    // The unsubmitted regions are the newest ones.
    for (u32 i = s->count; i > 0; i--) {
        RingSpace::Use& u = s->uses[(s->first + i - 1) % RING_SPACE_MAX_USES];
        if (u.submission)
            break;
        u.submission = number;
    }
}

u64 ring_space_shrink_size(RingSpace* s, u64 least) {
    if (s->count || s->takes < RING_SPACE_SHRINK_PERIOD)
        return 0;
    u64 want = ALIGN_UP(s->peak * 2, MiB(1));
    want = want > least ? want : least;
    bool shrink = s->peak * 4 <= s->size && want < s->size;
    s->peak = 0;
    s->takes = 0;
    return shrink ? want : 0;
}
