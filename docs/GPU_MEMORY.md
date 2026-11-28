# GPU memory: windows and page tables

Console titles share one memory between CPU and GPU, and the GPU reaches it through page tables:
an address is a page of 64 KiB anywhere in physical memory, the same frame can sit at several
addresses (titles alias render target memory, move texture pages around, map pools twice), and
memory can be GPU-only. Desktop GPUs have video memory of their own, reached through resources.

This layer gives the GPU two ways to title memory, chosen page by page:

- **Windows** (`gpu/gpu_window.h`): desktop buffers over heaps opened on the title's own pages. The
  GPU reads and writes the CPU's memory in place, coherent with the CPU's caches line by line, as on
  console. Nothing is copied, but a window covers one block of memory: pages that follow each other
  for the title must follow each other in RAM.
- **Page tables** (`gpu/page_tables.h`): reserved buffers whose pages map to tiles of video memory.
  Any layout of frames works (aliases, pools mapped out of order), and the memory needs no RAM, but
  where the CPU uses it too, copies keep the two in step.

## Which pages go where

Memory both CPU and GPU use is windowed: the title's own graphics allocations (one block each),
write-combined memory (what the CPU writes for the GPU to read once) and physical memory something
has written. Memory the CPU can't reach, and physical memory nothing has written yet (render target
memory, buffers the GPU fills), goes to the page tables, as do accesses a window can't serve (frames
mapped out of order). A page keeps its way once chosen, so every view of it sees the same memory;
when the title maps other frames there, it is chosen again.

## Page tables

Title GPU address space is covered by **segments**: reserved buffers of 4 GiB starting every 2 GiB
(segment *k* covers `[k * 2 GiB, k * 2 GiB + 4 GiB)`). Desktop buffer views can't reach more than
4 GiB into their resource, so a single buffer can't cover a large range; with overlapping segments,
any access of up to 2 GiB starting in granule *k* lies inside segment *k*. Segments are created when
the GPU first reaches their granule.

A page the page tables serve is backed by a **tile** of video memory (`gpu/tile_pool.h`) mapped into
the segments that reach it. The tile belongs to what backs the page, not to the address: in physical
memory the **frame** mapped there (every address showing it shares the tile, as on console), in other
graphics memory the page itself. Mapping happens on the page tables' own copy queue, which every title
submission waits for; tiles nothing maps any more go back to the pool once the GPU is done with them.

## Coherence

A tiled page the CPU can reach (through any address showing its frame) has two copies:

- **CPU writes** go to title memory. Tiled pages are write-watched (`mem_watch_writes`); the first
  write after an upload marks the page, and the next submission, or the next CPU release of GPU work
  (a counter store or fence signal), uploads it first. Device writes (file reads, DirectStorage) are
  reported and uploaded the same way.
- **GPU writes** go to the tile. Pages GPU work may write (unordered access views, copy destinations,
  query resolves) are taken from the CPU at each submission (no access, `mem_take_pages`); the first
  CPU access waits for the GPU and copies the tile back. Shared memory is windowed, so this is rare.
- **Small values the CPU polls** (fence values, counters written at the end of work) are written to
  title memory through a window as well as to the page tables.

Texture uploads (`hle/d3d12/texture_sync`) read whichever copy is current.

## Textures

Desktop textures lay texels out their own way, so they are resources of their own. Textures the GPU
only reads are reserved textures whose tiles hold the desktop layout and are mapped where the title
mapped memory; their contents come from title memory.

Render target, depth and UAV textures (`hle/d3d12/target_tiles.h`) are reserved textures too, and
their tiles belong to the title memory under them (`gpu/shared_tiles.h`): tile *i* of such a texture
is the tile of the frame (or, outside physical memory, the page) *i* × 64 KiB into its memory. Titles
place the transient targets of different passes on the same memory; textures of the same description
there share every tile and so their texels, and a pass reusing another's memory takes no video memory
of its own, as on console. Tiles past the memory's last whole page, and tiles of pages the title
hasn't mapped, are the texture's own. A mapping change under such a texture remaps its tiles before
the next title submission, on a queue of their own that submissions wait for. These tiles are separate
from the page tables' tiles of the same frames: GPU writes to a render target don't reach buffers over
its memory (they hold different layouts anyway).

Texture uploads read frames nothing has written yet as zeros without backing them (`mem_any_pending`),
so memory only the GPU writes, seen through views of placed memory, stays without system memory.

## Open points

- Page mapping changes a title orders on a queue (`CopyPageMappings` on a command queue) are applied
  when called and reach the page tables at the next submission, not after the work queued before them.
- Root descriptors carry no size; through the page tables they reach 64 KiB (a constant buffer's
  maximum). Raw buffers read further through a root descriptor would need the reach of the program.
- A small value written by the GPU through both ways (fence values, counters) can be overwritten in the
  tile by an upload of the same page made before the write ran.

## System memory

Title memory grows commit on demand. Before committing more, the system's remaining commit is checked
(`mem_commit_available`): commit is shared with every process and with video memory allocations, and
title memory that takes the last of it makes the whole system fail, so the title stops first.
