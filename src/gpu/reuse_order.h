#pragma once

#include "common/types.h"

// The order in which a pool of things the GPU uses once per submission (command allocators, lists)
// gets reused, without ever waiting for the GPU.
//
// Entries are used round robin, oldest use first. The oldest entry is reused once the queue finished
// the submission that last used it; otherwise a new entry joins the pool right there, so the order
// stays oldest first. Waiting instead would block the submitting thread on GPU work, and GPU work may
// wait for work only that thread submits later (console queues never block their callers), which is a
// deadlock. The pool grows to the number of submissions in flight and stays there.
//
// Not thread safe: one owner, under its own lock. Entry indices stay valid while the pool lives; the
// caller keeps its per-entry data in arrays it grows to reuse_capacity().

struct ReuseOrder {
    u32* order;       // entry indices, circular from `oldest`: the oldest use first
    u64* submissions; // per entry index: the submission that last used it (0: never submitted)
    u32 count;
    u32 capacity;
    u32 oldest;
};

// The entry to record into next. `completed` is the last submission the queue finished. Sets
// `*created` when the entry is new (its index is the entry count before the call), so the caller makes
// its data. Returns ~0u without memory to grow. Until reuse_submitted, calls return the same entry.
u32 reuse_take(ReuseOrder* r, u64 completed, bool* created);

// The entry reuse_take returned is used by submission `number`; the next take looks at the entry after.
void reuse_submitted(ReuseOrder* r, u32 entry, u64 number);

inline u32 reuse_capacity(const ReuseOrder* r) {
    return r->capacity;
}

void reuse_free(ReuseOrder* r);
