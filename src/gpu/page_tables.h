#pragma once

#include "gpu/gpu.h"

// GPU page tables over title memory (see docs/GPU_MEMORY.md).
//
// Title GPU addresses translate to segments: reserved buffers of 4 GiB starting every 2 GiB of title
// address space, whose pages are mapped to tiles of video memory. A tile belongs to the frame of
// physical memory behind the page (every address showing the frame shares it), or to the page itself in
// other graphics memory. Pages get tiles when a translation first reaches them, with the contents of
// title memory; CPU writes to them are uploaded before the next submission; pages GPU work may write are
// taken from the CPU after each submission and copied back when the CPU next touches them.

struct GpuAddress {
    ID3D12Resource* buffer; // the segment (or window, for coherent access)
    u64 offset;             // of the translated address within `buffer`
    u64 desktop_address;    // desktop GPU address of the same memory
    u64 available;          // bytes from there the same translation reaches (at least the size asked for)
};

enum GpuAccess {
    GPU_READ,  // the GPU only reads the memory
    GPU_WRITE, // GPU work may write it
};

void gpu_page_tables_init();

// Translates title GPU addresses [address, address + size). Fails (logged) if the range isn't graphics
// memory or reaches past a segment (accesses of up to 2 GiB always fit). Pages of the range not mapped
// in the title read as zero, as for the console's GPU.
bool gpu_translate(u64 address, u64 size, GpuAccess access, GpuAddress* out);

// Translates an address whose access has no stated size (root descriptors): windows reach the rest of
// their block anyway; through the page tables, the pages up to `reach` bytes on are mapped too, as far as
// the title's range goes.
bool gpu_translate_reach(u64 address, u64 reach, GpuAccess access, GpuAddress* out);

// Before a title submission on `queue`: maps pages translated since the last one and uploads what the
// CPU wrote, on the page tables' own queue, which `queue` then waits for, and takes from the CPU the
// pages GPU work may write. Call right before submitting the title's work.
void gpu_page_tables_submit(ID3D12CommandQueue* queue);

// The CPU is about to release GPU work waiting on it (a counter store, a fence signal). Titles fill data
// after submitting the work that reads it, so what the CPU wrote before the release reaches the GPU's
// copies first: returns once it has. Call right before signalling the desktop fence.
void gpu_page_tables_release();

// Where the GPU reads [address, address + size) as it is now: through the page tables if they hold any
// of it (GPU work may have written their copy), else through a window over title memory if it is one
// block of it and nothing in it is pending (mem_any_pending). False otherwise (the caller reads title
// memory itself). For texture uploads.
bool gpu_translate_current(u64 address, u64 size, GpuAddress* out);

// Memory reports: bytes of tiles the page tables hold, and the downloads so far.
void gpu_page_tables_usage(u64* tiles, u64* downloads);
