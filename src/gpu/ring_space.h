#pragma once

#include "common/types.h"

// Bookkeeping of a ring of memory whose regions one queue's submissions use once (see gpu/gpu_ring.h):
// regions are handed out in order and come back once the submission that used them finished. Pure
// bookkeeping, no GPU objects. Not thread safe.

#define RING_SPACE_MAX_USES 64

// Takes between two looks at whether a ring outgrew what its work needs (see ring_space_shrink_size).
#define RING_SPACE_SHRINK_PERIOD 1024

struct RingSpace {
    u64 size;
    u64 head; // where the next region starts
    // Regions in use, oldest first, and the submission using each (0: not submitted yet, the regions
    // handed out since the last ring_space_submitted). Regions handed out between two submissions are
    // merged while they are contiguous, so a submission takes one or two entries.
    struct Use {
        u64 start, end;
        u64 submission;
    } uses[RING_SPACE_MAX_USES];
    u32 first, count;
    // The most bytes in use at once over the last takes, and how many takes that was (since the reset or
    // the last ring_space_shrink_size that looked).
    u64 peak;
    u32 takes;
};

// Empties the ring and gives it `size` bytes.
void ring_space_reset(RingSpace* s, u64 size);

// Forgets the regions whose submission is `done` or older.
void ring_space_reclaim(RingSpace* s, u64 done);

// Hands out `bytes` aligned to `alignment` (a power of two) beside the regions in use. False when they
// don't fit; the ring then has to grow (nothing waits for regions to come back).
bool ring_space_take(RingSpace* s, u64 bytes, u64 alignment, u64* offset);

// The regions handed out since the last call are used by submission `number`.
void ring_space_submitted(RingSpace* s, u64 number);

// A ring grows instead of waiting for the GPU, so a burst of work leaves it far larger than the work
// after needs, and its memory counts against the system's commit for as long as it lives. Once every
// RING_SPACE_SHRINK_PERIOD takes, while nothing is in use, this returns the smaller size (at least
// `least`) to recreate the ring at when the peak use over those takes left three quarters of it unused,
// else 0. Each look starts a new period.
u64 ring_space_shrink_size(RingSpace* s, u64 least);
