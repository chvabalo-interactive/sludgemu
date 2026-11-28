#pragma once

#include "hle/d3d12/d3d12_objects.h"

// Render target, depth and UAV textures share video memory where their title memory is the same.
//
// Console titles reuse render target memory across passes: textures of different passes sit on the same
// pages (or on pages showing the same frames), each used while the others aren't. Desktop textures of
// their own would take memory for every one of them. Instead, such a texture's desktop texture is
// reserved and its tiles are those of the title memory under it (gpu/shared_tiles.h): its tile i belongs
// to the page i * 64 KiB into its memory. Textures of the same description over the same memory share
// every tile, and so their texels, as on console; a pass reusing another's memory takes no more. Tiles
// past the title memory's last whole page (desktop layouts can be larger) and tiles of pages without
// memory are the texture's own.
//
// Tiles follow the title's mappings: a mapping change under a texture remaps its tiles before the GPU
// next runs title work. Mapping runs on a queue of ours, which title submissions wait for.
//
// Desktop render target and depth textures need their compression metadata initialized (a clear, copy
// or discard) before first use; console titles may draw into new targets without one. A new texture's
// subresources whose tiles hold no contents yet (fresh from the pool) are discarded on that queue right
// after their tiles are mapped. Subresources sharing tiles with a texture over the same memory keep them:
// on console that memory holds what the other texture left there.
//
// SLUDGE_TARGET_TILES=0 turns this off (whole desktop textures, as before), for comparisons.

void target_tiles_init();

// Whether a console resource's desktop texture shares tiles this way, by its description and memory
// (whether desktop D3D12 makes reserved textures of its kind is the caller's to check).
bool target_tiles_eligible(const ConsoleResource* r);

// Registers `texture`, a reserved texture just created for `r` from the desktop description `desc` in
// `layout`; its tiles are mapped before the next title submission. Takes a reference of its own. False
// (logged) if its tiles can't be followed: the caller makes a whole texture instead.
bool target_tiles_add(ConsoleResource* r, ID3D12Resource* texture, const D3D12_RESOURCE_DESC1& desc,
                      D3D12_BARRIER_LAYOUT layout);

// Before the resource is destroyed: its tiles go back (once the GPU is done with them).
void target_tiles_remove(ConsoleResource* r);

// Before a title submission on `queue`: maps the tiles of textures made or remapped since the last one,
// and makes `queue` wait for that.
void target_tiles_submit(ID3D12CommandQueue* queue);

struct TargetTileUsage {
    u32 textures;
    u64 shared;     // bytes of tiles named by title memory, each counted once
    u64 own;        // bytes of tiles of one texture's own
    u64 referenced; // bytes the same tiles would take as textures of their own
};
void target_tiles_usage(TargetTileUsage* out);
