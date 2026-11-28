#pragma once

#include "common/types.h"

// Console-style memory model on top of Win32.
//
// "Physical" memory is divided into 64 KiB page frames, backed by committed pagefile sections
// ("chunks") created as frames are allocated. Titles reserve placeholder ranges and map page
// frames into them themselves, so the same physical page can be visible at several virtual
// addresses, exactly as on console. Every chunk is also mapped once into a linear view, which
// gives us (and the GPU layer) a stable view of every page frame.
//
// Mappings are 64 KiB granular because console pages are (and Windows view granularity matches),
// but runs of consecutive frames are mapped as one view.
//
// Every range the title marks as GPU-visible is tracked, so the graphics layer can find the host
// memory behind title GPU addresses (mem_backing_block) and hear about mapping changes.

#define MEM_PAGE_SIZE  KiB(64)
#define MEM_PAGE_SHIFT 16

// Physical memory is committed in chunks of this size (each one a section; see memory.cpp).
#define MEM_CHUNK_SIZE  MiB(256)
#define MEM_CHUNK_PAGES (MEM_CHUNK_SIZE / MEM_PAGE_SIZE)
#define MEM_INVALID_PFN 0xFFFFFFFFu

// Page numbers exchanged with titles count 4 KiB units, like console page numbers: a page frame's
// number is its index shifted left by this, so it is a multiple of 16 and the next frame's number
// is 16 higher. Titles depend on that (some store page numbers without their low four bits).
#define MEM_PAGE_NUMBER_SHIFT 4

enum MemRangeFlags : u32 {
    MEM_RANGE_GPU = 1u << 0,         // GPU visible
    MEM_RANGE_PLACEHOLDER = 1u << 1, // reserved for physical page mapping
    MEM_RANGE_CPU_ACCESS = 1u << 2,  // CPU may access (not GPU-only)
    MEM_RANGE_OWNS_PAGES = 1u << 3,  // physical pages were allocated with the range; freed on release
    MEM_RANGE_LAZY = 1u << 4,        // committed by the title but reserved here, committed when touched
    MEM_RANGE_SECTION = 1u << 5, // a section the GPU sees through a view of its own (see mem_alloc_virtual)
};

// Description of a tracked range. Ranges can be released by another thread at any time, so they
// are only ever handed out by copy.
struct MemRange {
    u8* base;
    u64 size;
    u32 flags;   // MemRangeFlags
    u32 protect; // host page protection used for mappings
    u64 xflags;  // console allocation flags as passed by the title (for logging / later use)
};

bool mem_init(u64 physical_bytes);

// Whether the system can commit `bytes` more right now. Commit (RAM and pagefile) is shared with every
// process, and Windows charges video memory allocations to it too.
bool mem_commit_available(u64 bytes);

// Physical page frames ------------------------------------------------------------------------

u64 mem_physical_total_pages();
u64 mem_physical_used_pages();

// Allocates `count` page frames (not necessarily contiguous) and stores their page numbers in
// `pfns`. Pages are committed and zeroed. Returns false (allocating nothing) if not enough are free.
bool mem_physical_alloc(u64 count, u64* pfns);
void mem_physical_free(u64 count, const u64* pfns);

// Host pointer to the page frame with page number `pfn`, through the linear view; nullptr if it
// isn't an allocated frame.
u8* mem_physical_ptr(u64 pfn);

// Whether page frame `pfn` has system memory yet: frames are bound when first touched, and read as
// zero until then (see mem_physical_alloc).
bool mem_physical_bound(u64 pfn);

// Called after frames are freed (no locks held), with their page numbers.
typedef void (*MemFramesFreedListener)(u64 count, const u64* pfns);
void mem_set_frames_freed_listener(MemFramesFreedListener listener);

// Physical memory chunks (committed sections, nullptr until first used). Chunk i holds frames
// [i * MEM_CHUNK_PAGES, (i + 1) * MEM_CHUNK_PAGES). Allocations that fit in a chunk get consecutive
// frames of one chunk where possible, so the GPU can see them as one block.
u32 mem_physical_chunk_count();
u32 mem_physical_chunks_created(); // each commits MEM_CHUNK_SIZE of system memory
// Pages of physical memory bound to system memory (frames read or written so far, see bind_frame); with
// `va`, only those mapped in [va, va + size) of one placeholder range. Memory reports.
u64 mem_physical_bound_pages();
u64 mem_physical_bound_pages_at(const void* va, u64 size);
void* mem_physical_chunk(u32 index); // section HANDLE

// Where a host pointer into the linear view lies: its chunk and the offset in it. False for any
// other memory.
bool mem_linear_location(const void* host, u32* chunk, u64* offset);

// Virtual ranges ------------------------------------------------------------------------------

// Address constraints for new reservations (0 = no constraint).
struct MemAddressLimits {
    u64 lowest;
    u64 highest; // inclusive upper bound of the whole range
    u64 alignment;
};

// Reserves a placeholder range for later physical page mapping.
void* mem_reserve_placeholder(u64 size, const MemAddressLimits& limits, u32 range_flags, u32 protect,
                              u64 xflags);

// Reserves a placeholder range, allocates physical pages for all of it and maps them: committed
// memory that is explicitly backed by physical page frames.
void* mem_alloc_physical_backed(u64 size, const MemAddressLimits& limits, u32 range_flags, u32 protect,
                                u64 xflags);

// Reserves (+ optionally commits) ordinary memory with address constraints. Returns nullptr on
// failure. Ranges with MEM_RANGE_GPU are registered. Large commits are only reserved and get
// committed as they are touched, like mapped physical frames (see mem_map_physical).
//
// New GPU-visible ranges are views of a section, which the GPU sees through a second view of ours
// (MemBlock::host): heaps opened over memory lock its pages, and pages locked that way can't have
// their protection changed, so the title's own addresses must never be the ones locked. Sections
// can't decommit: decommitting part of such a range keeps it committed.
void* mem_alloc_virtual(void* base, u64 size, u32 alloc_type, u32 protect, const MemAddressLimits& limits,
                        u32 range_flags, u64 xflags);

// Maps page frames (by page number) into a placeholder range. pfns == nullptr unmaps. Each page
// may be at a different address (scatter) when `vas` is given; otherwise pages are consecutive from
// `va`.
//
// Mapped frames are backed by memory only when their pages are first touched: CPU accesses are
// caught and handled transparently, but anything that reaches title memory without faulting in user
// mode (kernel I/O, the GPU) must call mem_prepare first.
bool mem_map_physical(void* va, u64 count, const u64* pfns);
bool mem_map_physical_scatter(void* const* vas, u64 count, const u64* pfns);

// Backs anything committed or mapped by the title but not backed yet under [addr, addr + size) (see
// mem_alloc_virtual and mem_map_physical).
void mem_prepare(const void* addr, u64 size);

// Releases a range created by the functions above. Returns false if `base` isn't one of ours.
bool mem_release(void* base);

// Copies the description of the range containing `addr`; false if it isn't tracked.
bool mem_query_range(const void* addr, MemRange* out);

// A block of title memory backed by one contiguous block of a single host allocation, which is what
// a GPU heap can be opened over: a whole ordinary range, or in a placeholder range the longest run
// of pages around an address that map consecutive frames of one chunk (seen through the linear
// view).
struct MemBlock {
    u8* base; // title address
    u64 size;
    u8* host; // host address of the same memory
};

// The block holding `addr`. False if `addr` isn't tracked or isn't mapped.
bool mem_backing_block(const void* addr, MemBlock* out);

// The frames mapped at `count` pages from `va` in a placeholder range, by page number
// (MEM_INVALID_PFN where nothing is mapped). False if the pages aren't all in one placeholder range.
bool mem_frames_at(const void* va, u64 count, u64* pfns);

// Whether every page of [addr, addr + size) is tracked and has memory: committed, or mapped frames.
bool mem_is_mapped(const void* addr, u64 size);

// Whether any page of [addr, addr + size) shows a frame that isn't bound to memory yet (see
// mem_map_physical): nothing has written it through any address, so it reads as zero. Readers that only need its contents (texture uploads)
// take zeros instead of touching it, which would back the frames of its whole slice.
bool mem_any_pending(const void* addr, u64 size);

// Called after page mappings change in [base, base + size) of a tracked range, or when the range is
// released. Called without memory locks held, possibly from any thread; must be cheap. Listeners are
// added once each, from one thread at a time.
typedef void (*MemMappingListener)(const u8* base, u64 size);
void mem_add_mapping_listener(MemMappingListener listener);

// Write watches: the memory under [base, base + size) of tracked title memory becomes read-only until
// the first write into one of its pages, which ends the watch and reports its range (as armed) to the
// write listener. Watches are on memory, not addresses: mapped frames can show at several addresses (the
// console's physical pages), and a write through any address showing a watched page ends the watch,
// whose pages then become writable at every address (those no other watch or trap needs). Protection
// is per host page, so a write next to a watched range on one of its pages ends it too. GPU-only and
// read-only memory needs no protection: the CPU can't write it. Watches survive views being remapped
// over the same frames; they end unreported when other frames are mapped at their range (mapping
// listeners get that change). Writes through our own views of the memory (for the GPU) and by the
// kernel aren't seen: kernel I/O into title memory ends the watches first (mem_end_write_watches),
// since its writes would fail rather than fault, and gives taken pages there back first. Watches belong
// to a client (any number naming the caller): arming a range the same client watches already does
// nothing, and unwatching ends that client's watch only.
enum MemWriteKind {
    MEM_WATCH_ENDED,  // a write ended the watch armed over [base, base + size)
    MEM_DEVICE_WROTE, // a device finished writing [base, base + size) (see mem_expect_device_writes)
    MEM_WATCH_DROPPED, // other memory was mapped where the watch over [base, base + size) was: it ended
                       // without a write (the memory it watched may be written unseen from now on)
};
typedef void (*MemWriteListener)(u64 base, u64 size, MemWriteKind kind); // called without memory locks held
void mem_add_write_listener(MemWriteListener listener);
void mem_remove_write_listener(MemWriteListener listener);
void mem_watch_writes(const void* base, u64 size, u32 client = 0);

// Ends the watch armed for exactly [base, base + size), unreported (its user is gone).
void mem_unwatch_writes(const void* base, u64 size, u32 client = 0);

// The memory under [base, base + size) is about to be written without faults (kernel I/O): the
// watches on its pages end and are reported.
void mem_end_write_watches(const void* base, u64 size);

// Device writes: title memory that something other than the CPU writes (DirectStorage requests),
// finishing when `done` is signaled. Until then mem_device_writes_pending reports the memory; once
// mem_poll_device_writes finds `done` signaled (and closes it), the spans are reported to the write
// listener (MEM_DEVICE_WROTE). Spans are title addresses.
struct MemSpan {
    u64 base;
    u64 size;
};
void mem_expect_device_writes(void* done, const MemSpan* spans, u32 count);
// Whether device writes into [base, base + size) haven't finished. Doesn't report finished ones.
bool mem_device_writes_pending(u64 base, u64 size);
// Whether device writes are outstanding at all; cheap.
bool mem_device_writes_outstanding();
// Reports the device writes that finished.
void mem_poll_device_writes();

// The title addresses showing the page frame with page number `pfn`: up to `max` of them in `vas`,
// returning how many there are.
u32 mem_frame_addresses(u64 pfn, u64* vas, u32 max);

// Store traps: every CPU store into [base, base + size) of tracked title memory is reported to the
// store listener right after it completes, for values something else waits on (GPU work waiting for a
// counter the CPU writes). The pages stay read-only: a store faults, runs alone with its page writable
// (single-stepped), and the page is protected again before the report. Like watches, traps are on
// memory: stores through any address showing it are reported. Stores to the same pages outside the
// range are reported too (the listener checks the values); stores through our views and by the
// kernel aren't seen. Traps last until mem_untrap_stores or until the range is released: every store
// to their pages costs two exceptions, and kernel writes there fail, so they should be short-lived.
typedef void (*MemStoreListener)(u64 base, u64 size); // called on the storing thread, no locks held
void mem_set_store_listener(MemStoreListener listener);
void mem_trap_stores(const void* base, u64 size);
// Ends the trap armed for exactly [base, base + size).
void mem_untrap_stores(const void* base, u64 size);

// Pages taken from the CPU: no access at any address showing the same memory until they are given
// back. For memory another copy of which is newer (written by the GPU in video memory): the first
// access calls the taken listener with the address (no locks held, on the accessing thread), which
// brings the page up to date through another view of it (mem_physical_ptr, a section's host view) and
// gives it back; then the access retries. Pages are host pages; memory the CPU can't access isn't
// taken. Write watches covering taken pages stay armed: given back, such pages are read-only again.
typedef void (*MemTakenListener)(const void* addr);
void mem_set_taken_listener(MemTakenListener listener);
void mem_take_pages(const void* base, u64 size);
void mem_give_pages(const void* base, u64 size);

// Changes the host protection used for future mappings in the range containing `addr`. A range given
// CPU access this way is CPU-accessible from then on (MEM_RANGE_CPU_ACCESS).
void mem_set_range_protect(const void* addr, u32 protect);
bool mem_is_tracked(const void* addr);

// Graphics address window reported to the title (D3D12 virtual memory configuration).
void mem_set_gpu_window(u64 lowest, u64 highest);
void mem_gpu_window(u64* lowest, u64* highest);
