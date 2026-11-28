// The bookkeeping that keeps submissions from waiting for the GPU: list pools that grow instead of
// waiting (gpu/reuse_order.h), rings that grow instead of waiting (gpu/ring_space.h) and the console
// queue backlog's order (hle/d3d12/queue_backlog.h).

#include "test.h"

#include "gpu/reuse_order.h"
#include "gpu/ring_space.h"
#include "hle/d3d12/queue_backlog.h"

// A pool with a GPU that finishes `completed` submissions: entries come back only once theirs did.
static void test_reuse_order() {
    ReuseOrder r = {};
    bool created = false;

    // The first take makes an entry; taking again before submitting returns it again.
    u32 a = reuse_take(&r, 0, &created);
    CHECK(a == 0 && created);
    CHECK(reuse_take(&r, 0, &created) == a && !created);
    reuse_submitted(&r, a, 1);

    // GPU at 0: submission 1 runs, so the next take makes a second entry, and a third.
    u32 b = reuse_take(&r, 0, &created);
    CHECK(b == 1 && created);
    reuse_submitted(&r, b, 2);
    u32 c = reuse_take(&r, 0, &created);
    CHECK(c == 2 && created);
    reuse_submitted(&r, c, 3);
    CHECK(r.count == 3);

    // GPU at 1: the oldest (a) comes back, then b isn't done yet, so a new entry goes ahead of it.
    CHECK(reuse_take(&r, 1, &created) == a && !created);
    reuse_submitted(&r, a, 4);
    u32 d = reuse_take(&r, 1, &created);
    CHECK(d == 3 && created);
    reuse_submitted(&r, d, 5);

    // Oldest first: b (2), c (3), a (4), d (5). With the GPU at 5 they come back in that order and the
    // pool stops growing.
    u32 expected[] = {b, c, a, d, b, c};
    u64 number = 6;
    for (u32 i = 0; i < ARRAY_COUNT(expected); i++) {
        u32 e = reuse_take(&r, number - 1, &created);
        CHECK(e == expected[i] && !created);
        reuse_submitted(&r, e, number++);
    }
    CHECK(r.count == 4);

    // A GPU that stops (waiting for work submitted later) never makes a take wait: the pool grows,
    // across several capacity doublings, and every entry stays distinct.
    u64 stalled = number - 1;
    bool seen[64] = {};
    for (u32 i = 0; i < 40; i++) {
        u32 e = reuse_take(&r, stalled - 4, &created); // stuck: every entry still runs
        CHECK(e < 64 && !seen[e]);
        if (e < 64)
            seen[e] = true;
        reuse_submitted(&r, e, number++);
    }
    CHECK(r.count == 44 && reuse_capacity(&r) >= 44);

    // Once the GPU catches up the oldest entries come back first, and no more are made.
    u32 before = r.count;
    for (u32 i = 0; i < 100; i++) {
        u32 e = reuse_take(&r, number - 1, &created);
        CHECK(!created && e < r.count);
        reuse_submitted(&r, e, number++);
    }
    CHECK(r.count == before);
    reuse_free(&r);
    CHECK(!r.order && !r.submissions && !r.count);
}

static void test_ring_space() {
    RingSpace s;
    ring_space_reset(&s, 1024);
    u64 at = ~0ull;

    // Regions of one submission merge into one use; aligned starts.
    CHECK(ring_space_take(&s, 100, 16, &at) && at == 0);
    CHECK(ring_space_take(&s, 100, 256, &at) && at == 256);
    CHECK(s.count == 1 && s.uses[0].end == 356);
    ring_space_submitted(&s, 1);

    CHECK(ring_space_take(&s, 300, 16, &at) && at == 368);
    ring_space_submitted(&s, 2);
    CHECK(s.count == 2);

    // GPU at 0: nothing comes back and 400 more bytes don't fit at the end nor before the oldest
    // region: the caller grows the ring instead of waiting.
    ring_space_reclaim(&s, 0);
    CHECK(!ring_space_take(&s, 400, 16, &at));
    CHECK(s.count == 2);                                   // a failed take changes nothing
    CHECK(ring_space_take(&s, 300, 16, &at) && at == 672); // what fits still goes
    ring_space_submitted(&s, 3);

    // GPU at 1: [0, 356) comes back; the next region wraps around to 0.
    ring_space_reclaim(&s, 1);
    CHECK(s.count == 2);
    CHECK(ring_space_take(&s, 200, 16, &at) && at == 0);
    CHECK(!ring_space_take(&s, 200, 16, &at)); // would run into the region of submission 2 at 368
    ring_space_submitted(&s, 4);

    // The wrapped region (submission 4) didn't merge with the one before it.
    CHECK(s.count == 3);

    // Everything done: the ring starts over at 0.
    ring_space_reclaim(&s, 4);
    CHECK(s.count == 0 && s.head == 0);
    CHECK(ring_space_take(&s, 1024, 16, &at) && at == 0);
    CHECK(!ring_space_take(&s, 1, 1, &at)); // full
    ring_space_submitted(&s, 5);

    // Unsubmitted regions never come back, whatever the GPU finished.
    ring_space_reset(&s, 1 << 20);
    for (u32 i = 0; i < RING_SPACE_MAX_USES; i++) {
        CHECK(ring_space_take(&s, 64, 64, &at));
        ring_space_submitted(&s, 10 + i);
    }
    CHECK(s.count == RING_SPACE_MAX_USES);
    CHECK(!ring_space_take(&s, 64, 64, &at)); // no use entry left: grow
    ring_space_reclaim(&s, 10);
    CHECK(s.count == RING_SPACE_MAX_USES - 1);
    CHECK(ring_space_take(&s, 64, 64, &at));
    CHECK(ring_space_take(&s, 64, 64, &at)); // merges with the unsubmitted one before
    CHECK(s.count == RING_SPACE_MAX_USES);
    ring_space_reclaim(&s, ~0ull);
    CHECK(s.count == 1 && s.uses[s.first].submission == 0);
}

// A ring a burst grew gives the memory back once the work after needs far less, and only while nothing
// is in use.
static void test_ring_shrink() {
    RingSpace s;
    ring_space_reset(&s, MiB(256));
    u64 at = 0;
    u64 number = 1;

    // Too few takes yet: no look.
    CHECK(ring_space_take(&s, MiB(1), 256, &at));
    ring_space_submitted(&s, number++);
    ring_space_reclaim(&s, number);
    CHECK(ring_space_shrink_size(&s, MiB(16)) == 0);

    // A period of 4 MiB submissions, two in flight at a time: peak 8 MiB, so it shrinks to 16 MiB...
    for (u32 i = 1; i < RING_SPACE_SHRINK_PERIOD; i++) {
        CHECK(ring_space_take(&s, MiB(4), 256, &at));
        ring_space_submitted(&s, number);
        ring_space_reclaim(&s, number - 1);
        number++;
    }
    CHECK(s.peak == MiB(8));
    // ...but not while a region is still in use.
    CHECK(ring_space_shrink_size(&s, MiB(1)) == 0);
    ring_space_reclaim(&s, number);
    CHECK(s.count == 0);
    CHECK(ring_space_shrink_size(&s, MiB(1)) == MiB(16));
    // The look started a new period.
    CHECK(s.takes == 0 && s.peak == 0 && ring_space_shrink_size(&s, MiB(1)) == 0);

    // Never below the least size asked for.
    for (u32 i = 0; i < RING_SPACE_SHRINK_PERIOD; i++) {
        CHECK(ring_space_take(&s, 4096, 256, &at));
        ring_space_submitted(&s, number);
        ring_space_reclaim(&s, number++);
    }
    CHECK(ring_space_shrink_size(&s, MiB(16)) == MiB(16));

    // A ring whose work uses more than a quarter of it stays.
    ring_space_reset(&s, MiB(16));
    for (u32 i = 0; i < RING_SPACE_SHRINK_PERIOD; i++) {
        CHECK(ring_space_take(&s, MiB(5), 256, &at));
        ring_space_submitted(&s, number);
        ring_space_reclaim(&s, number++);
    }
    CHECK(s.peak == MiB(5) && ring_space_shrink_size(&s, MiB(1)) == 0);

    // Use that wraps around the end counts the bytes skipped there.
    ring_space_reset(&s, 1000);
    CHECK(ring_space_take(&s, 600, 1, &at) && at == 0);
    ring_space_submitted(&s, number);
    ring_space_reclaim(&s, number++);
    CHECK(ring_space_take(&s, 300, 1, &at) && at == 0); // empty: starts over
    ring_space_submitted(&s, number++);
    CHECK(ring_space_take(&s, 500, 1, &at) && at == 300);
    ring_space_submitted(&s, number++);
    ring_space_reclaim(&s, number - 2);                 // [0, 300) done; [300, 800) in use
    CHECK(ring_space_take(&s, 250, 1, &at) && at == 0); // wraps: 200 bytes skipped at the end
    CHECK(s.peak == 1000 - 300 + 250);
}

static void test_backlog_order() {
    QueueBacklog b = {};
    CHECK(!backlog_front(&b));

    // Ops come out in the order they went in, across wrap-arounds and growth.
    u64 pushed = 0, popped = 0;
    for (u32 round = 0; round < 6; round++) {
        u32 push = 5 + round * 9; // grows past 16 and 32 ops while some are held
        for (u32 i = 0; i < push; i++) {
            BacklogOp* op = backlog_push(&b);
            CHECK(op && op->kind == BACKLOG_EXECUTE && op->count == 0 && !op->fence); // zeroed
            if (op)
                op->number = ++pushed;
        }
        u32 pop = push - 2;
        for (u32 i = 0; i < pop; i++) {
            BacklogOp* op = backlog_front(&b);
            CHECK(op && op->number == ++popped);
            backlog_pop(&b);
        }
    }
    CHECK(b.count == pushed - popped);
    while (BacklogOp* op = backlog_front(&b)) {
        CHECK(op->number == ++popped);
        backlog_pop(&b);
    }
    CHECK(popped == pushed && b.count == 0);
    backlog_pop(&b); // popping an empty backlog does nothing
    CHECK(b.count == 0);
    backlog_free(&b);
    CHECK(!b.ops && !b.capacity);
}

void test_submission() {
    test_reuse_order();
    test_ring_space();
    test_ring_shrink();
    test_backlog_order();
}
