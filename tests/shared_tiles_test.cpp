// Tests for tiles shared through title memory (gpu/shared_tiles).

#include <windows.h>
#include <stdlib.h>

#include "core/memory.h"
#include "gpu/shared_tiles.h"
#include "test.h"

#define PAGE MEM_PAGE_SIZE

// References count per key; the last one gives the tile back, and the key is gone.
static void test_map_references() {
    SharedTileMap m = {};
    u32 tile = 0;
    CHECK(!shared_tile_ref(&m, 0x10000, &tile));
    CHECK(shared_tile_insert(&m, 0x10000, 7));
    CHECK(shared_tile_ref(&m, 0x10000, &tile) && tile == 7);
    CHECK(shared_tile_ref(&m, 0x10000, &tile) && tile == 7);
    CHECK(m.live == 1);
    CHECK(!shared_tile_unref(&m, 0x10000, &tile));
    CHECK(!shared_tile_unref(&m, 0x10000, &tile));
    tile = 0;
    CHECK(shared_tile_unref(&m, 0x10000, &tile) && tile == 7);
    CHECK(m.live == 0);
    CHECK(!shared_tile_ref(&m, 0x10000, &tile));
    CHECK(!shared_tile_unref(&m, 0x10000, &tile)); // unknown keys release nothing
    shared_tile_map_free(&m);
}

// Many keys, inserted and removed in turns: lookups stay right as the map grows and reuses removed
// entries.
static void test_map_churn() {
    SharedTileMap m = {};
    const u32 keys = 20000;
    for (u32 i = 0; i < keys; i++)
        CHECK(shared_tile_insert(&m, SHARED_TILE_FRAME | i, i));
    for (u32 i = 0; i < keys; i += 2) {
        u32 tile = ~0u;
        CHECK(shared_tile_unref(&m, SHARED_TILE_FRAME | i, &tile) && tile == i);
    }
    CHECK(m.live == keys / 2);
    for (u32 round = 0; round < 3; round++) {
        for (u32 i = 0; i < keys; i += 2)
            CHECK(shared_tile_insert(&m, (u64)(i + 1) << MEM_PAGE_SHIFT, i + round));
        for (u32 i = 0; i < keys; i += 2) {
            u32 tile = ~0u;
            CHECK(shared_tile_unref(&m, (u64)(i + 1) << MEM_PAGE_SHIFT, &tile) && tile == i + round);
        }
    }
    u32 wrong = 0;
    for (u32 i = 0; i < keys; i++) {
        u32 tile = ~0u;
        bool found = shared_tile_ref(&m, SHARED_TILE_FRAME | i, &tile);
        wrong += found != (i % 2 == 1) || (found && tile != i);
    }
    CHECK(wrong == 0);
    CHECK(m.capacity < 8 * keys); // removed entries are reclaimed when the map grows
    shared_tile_map_free(&m);
}

// Keys name what backs title memory: the same frame at two addresses gets one key, other frames other
// keys, unmapped pages none.
static void test_keys_follow_frames() {
    const u32 pages = 4;
    MemAddressLimits limits = {};
    u8* a = (u8*)mem_reserve_placeholder(pages * PAGE, limits, 0, PAGE_READWRITE, 0);
    u8* b = (u8*)mem_reserve_placeholder(pages * PAGE, limits, 0, PAGE_READWRITE, 0);
    CHECK(a && b);
    if (!a || !b)
        return;
    u64 frames[pages];
    CHECK(mem_physical_alloc(pages, frames));
    CHECK(mem_map_physical(a, pages, frames));
    // b shows a's frames 2 and 3 first, then frame 0; its last page stays unmapped.
    u64 shown[3] = {frames[2], frames[3], frames[0]};
    CHECK(mem_map_physical(b, 3, shown));

    u64 ka[pages], kb[pages];
    shared_tile_keys((u64)a, pages, ka);
    shared_tile_keys((u64)b, pages, kb);
    for (u32 i = 0; i < pages; i++)
        CHECK(ka[i] == (SHARED_TILE_FRAME | (frames[i] >> MEM_PAGE_NUMBER_SHIFT)));
    CHECK(kb[0] == ka[2] && kb[1] == ka[3] && kb[2] == ka[0]);
    CHECK(kb[3] == SHARED_TILE_PRIVATE);
    CHECK(ka[0] != ka[1]);

    // Remapping changes the keys; nothing was backed to find them.
    CHECK(mem_map_physical(b, 1, &frames[1]));
    shared_tile_keys((u64)b, 1, kb);
    CHECK(kb[0] == ka[1]);
    CHECK(mem_physical_bound_pages_at(a, pages * PAGE) == 0);

    // Keys from inside a range.
    u64 keys[pages * 2];
    shared_tile_keys((u64)a + 2 * PAGE, 2, keys);
    CHECK(keys[0] == ka[2] && keys[1] == ka[3]);

    CHECK(mem_release(a));
    CHECK(mem_release(b));
    mem_physical_free(pages, frames);
}

// Other title memory is named by its address; memory that isn't the title's names nothing.
static void test_keys_of_other_memory() {
    MemAddressLimits limits = {};
    u8* p = (u8*)mem_alloc_virtual(nullptr, 2 * PAGE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE, limits,
                                   MEM_RANGE_GPU | MEM_RANGE_CPU_ACCESS, 0);
    CHECK(p != nullptr);
    if (!p)
        return;
    u64 keys[2];
    shared_tile_keys((u64)p, 2, keys);
    CHECK(keys[0] == (u64)p && keys[1] == (u64)p + PAGE);
    void* untracked = VirtualAlloc(nullptr, PAGE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    shared_tile_keys((u64)untracked, 1, keys);
    CHECK(keys[0] == SHARED_TILE_PRIVATE);
    VirtualFree(untracked, 0, MEM_RELEASE);
    CHECK(mem_release(p));
}

void test_shared_tiles() {
    test_map_references();
    test_map_churn();
    test_keys_follow_frames();
    test_keys_of_other_memory();
}
