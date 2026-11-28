#pragma once

#include "common/types.h"

// Tiles of video memory named by the title memory they stand for.
//
// Console titles alias memory: transient render targets of different passes share physical pages, and
// the same frame can show at several addresses. Desktop textures standing for such memory share their
// tiles where the title memory is the same, so they alias exactly where the console's do and take what
// the console's take. A tile is named by a key: the frame of physical memory behind a page (whatever
// address shows it), or the page itself in other title memory. Not thread safe: the owner locks.

#define SHARED_TILE_PRIVATE 0ull         // no title memory to share: the tile is one texture's own
#define SHARED_TILE_FRAME   (1ull << 63) // set in keys naming a frame (by ID); others are page addresses

// The keys of `count` pages of title memory from `va` (64 KiB aligned): frames where physical memory is
// mapped (pending or backed), page addresses in other title memory, SHARED_TILE_PRIVATE where nothing is
// mapped or the memory isn't the title's.
void shared_tile_keys(u64 va, u32 count, u64* keys);

struct SharedTileEntry {
    u64 key; // 0: empty, 1: removed
    u32 tile;
    u32 refs;
};

// Open addressing over keys.
struct SharedTileMap {
    SharedTileEntry* entries;
    u32 capacity; // a power of two, or 0
    u32 used;     // entries not empty (removed ones included)
    u32 live;     // keys with a tile
};

// Takes a reference to the tile of `key`: true with `*tile` if the key has one; false if not (the caller
// gets a tile and inserts it).
bool shared_tile_ref(SharedTileMap* m, u64 key, u32* tile);

// Gives `key` its tile, with one reference. False if out of memory (the key stays without).
bool shared_tile_insert(SharedTileMap* m, u64 key, u32 tile);

// Drops a reference to the tile of `key`. True with `*tile` if that was the last: the key is gone and
// the caller frees the tile.
bool shared_tile_unref(SharedTileMap* m, u64 key, u32* tile);

void shared_tile_map_free(SharedTileMap* m);
