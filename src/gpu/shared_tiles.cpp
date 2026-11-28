#include "gpu/shared_tiles.h"

#include <stdlib.h>
#include <string.h>

#include "core/memory.h"

void shared_tile_keys(u64 va, u32 count, u64* keys) {
    // Mostly one placeholder range: all frames in one call (the page numbers land in `keys`).
    if (mem_frames_at((const void*)va, count, keys)) {
        for (u32 i = 0; i < count; i++)
            keys[i] = keys[i] == MEM_INVALID_PFN ? SHARED_TILE_PRIVATE
                                                 : SHARED_TILE_FRAME | (keys[i] >> MEM_PAGE_NUMBER_SHIFT);
        return;
    }
    for (u32 i = 0; i < count; i++) {
        u64 page = va + ((u64)i << MEM_PAGE_SHIFT);
        u64 pfn = MEM_INVALID_PFN;
        MemRange range;
        if (mem_frames_at((const void*)page, 1, &pfn))
            keys[i] = pfn == MEM_INVALID_PFN ? SHARED_TILE_PRIVATE
                                             : SHARED_TILE_FRAME | (pfn >> MEM_PAGE_NUMBER_SHIFT);
        else if (mem_query_range((const void*)page, &range) && !(range.flags & MEM_RANGE_PLACEHOLDER))
            keys[i] = page; // one view of its memory: the address names it
        else
            keys[i] = SHARED_TILE_PRIVATE;
    }
}

#define KEY_EMPTY   0ull
#define KEY_REMOVED 1ull

static u32 slot_of(u64 key, u32 capacity) {
    return (u32)((key * 0x9E3779B97F4A7C15ull) >> 32) & (capacity - 1);
}

// The entry of `key`, or nullptr.
static SharedTileEntry* find(SharedTileMap* m, u64 key) {
    if (!m->capacity)
        return nullptr;
    for (u32 i = slot_of(key, m->capacity), n = 0; n < m->capacity; i = (i + 1) & (m->capacity - 1), n++) {
        SharedTileEntry& e = m->entries[i];
        if (e.key == key)
            return &e;
        if (e.key == KEY_EMPTY)
            return nullptr;
    }
    return nullptr;
}

// Room for one more key: at most half the entries in use, removed ones included.
static bool make_room(SharedTileMap* m) {
    if ((m->used + 1) * 2 <= m->capacity)
        return true;
    u32 capacity = 1024;
    while (capacity < (m->live + 1) * 4)
        capacity *= 2;
    SharedTileEntry* entries = (SharedTileEntry*)calloc(capacity, sizeof(SharedTileEntry));
    if (!entries)
        return false;
    for (u32 i = 0; i < m->capacity; i++) {
        const SharedTileEntry& e = m->entries[i];
        if (e.key == KEY_EMPTY || e.key == KEY_REMOVED)
            continue;
        u32 at = slot_of(e.key, capacity);
        while (entries[at].key != KEY_EMPTY)
            at = (at + 1) & (capacity - 1);
        entries[at] = e;
    }
    free(m->entries);
    m->entries = entries;
    m->capacity = capacity;
    m->used = m->live;
    return true;
}

bool shared_tile_ref(SharedTileMap* m, u64 key, u32* tile) {
    SharedTileEntry* e = find(m, key);
    if (!e)
        return false;
    e->refs++;
    *tile = e->tile;
    return true;
}

bool shared_tile_insert(SharedTileMap* m, u64 key, u32 tile) {
    if (!make_room(m))
        return false;
    u32 at = slot_of(key, m->capacity);
    while (m->entries[at].key != KEY_EMPTY && m->entries[at].key != KEY_REMOVED)
        at = (at + 1) & (m->capacity - 1);
    if (m->entries[at].key == KEY_EMPTY)
        m->used++;
    m->entries[at] = {key, tile, 1};
    m->live++;
    return true;
}

bool shared_tile_unref(SharedTileMap* m, u64 key) {
    SharedTileEntry* e = find(m, key);
    if (!e || !e->refs)
        return false;
    return --e->refs == 0;
}

bool shared_tile_drop(SharedTileMap* m, u64 key, u32* tile) {
    SharedTileEntry* e = find(m, key);
    if (!e || e->refs)
        return false;
    *tile = e->tile;
    e->key = KEY_REMOVED;
    m->live--;
    return true;
}

void shared_tile_map_free(SharedTileMap* m) {
    free(m->entries);
    memset(m, 0, sizeof(*m));
}
