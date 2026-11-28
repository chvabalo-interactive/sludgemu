#include "gpu/reuse_order.h"

#include <stdlib.h>
#include <string.h>

static bool grow(ReuseOrder* r) {
    u32 capacity = r->capacity ? r->capacity * 2 : 4;
    u32* order = (u32*)realloc(r->order, capacity * sizeof(u32));
    if (!order)
        return false;
    r->order = order;
    u64* submissions = (u64*)realloc(r->submissions, capacity * sizeof(u64));
    if (!submissions)
        return false;
    r->submissions = submissions;
    r->capacity = capacity;
    return true;
}

u32 reuse_take(ReuseOrder* r, u64 completed, bool* created) {
    if (r->count) {
        u32 entry = r->order[r->oldest];
        if (r->submissions[entry] <= completed) {
            *created = false;
            return entry;
        }
    }
    if (r->count == r->capacity && !grow(r))
        return ~0u;
    // The new entry goes where the oldest is, ahead of it: once submitted it is the newest use.
    u32 entry = r->count;
    memmove(r->order + r->oldest + 1, r->order + r->oldest, (r->count - r->oldest) * sizeof(u32));
    r->order[r->oldest] = entry;
    r->submissions[entry] = 0;
    r->count++;
    *created = true;
    return entry;
}

void reuse_submitted(ReuseOrder* r, u32 entry, u64 number) {
    r->submissions[entry] = number;
    r->oldest = (r->oldest + 1) % r->count;
}

void reuse_free(ReuseOrder* r) {
    free(r->order);
    free(r->submissions);
    memset(r, 0, sizeof(*r));
}
