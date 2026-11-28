// Internals of the memory system shared by its parts (memory.cpp: ranges, frames and mappings;
// memory_tracking.cpp: write tracking). Not for use outside core/memory*.

#pragma once

#include <windows.h>

#include "core/memory.h"

// One region of a placeholder range: a placeholder, or a view of consecutive real frames.
struct MemRegion {
    u32 first_page; // page index within the range; the region ends where the next one starts
    u32 pfn;        // real frame mapped at first_page, or REGION_PLACEHOLDER
};
#define REGION_PLACEHOLDER 0xFFFFFFFFu

struct RangeSlot {
    MemRange info;
    MemRegion* regions; // placeholder ranges only; sorted by first_page, regions[0].first_page == 0
    u32 region_count;
    u32 region_capacity;
    u32* anchors;   // placeholder ranges: per slice of REGION_PAGES pages, the first real frame of the
                    // region the slice prefers, or MEM_INVALID_PFN
    u32* pending;   // placeholder ranges, once frames are mapped: per page, the frame ID mapped there but
                    // not backed yet (see "Lazy backing"), or MEM_INVALID_PFN
    HANDLE section; // section ranges: the section and our own view of it, which the GPU sees
    u8* host;
    u16* watch_counts; // other than placeholder ranges, once tracked: per host page, what protects it
};

#define MAX_RANGES 8192
#define MAX_CHUNKS 256

// Placeholder ranges lay their frames out in regions of real memory, one per slice of this many
// pages (see preferred_real). Chunks hold several regions.
#define REGION_PAGES 512u
#define HOST_PAGE    4096u // the host page size, the grain of protection
#define MAX_REGIONS  (MAX_CHUNKS * (MEM_CHUNK_PAGES / REGION_PAGES))

// Title commits at least this large are committed lazily, this much at a time.
#define LAZY_COMMIT_MIN     MiB(16)
#define LAZY_COMMIT_GRANULE MiB(1)

// Win32 functions from newer API sets, resolved at runtime.
typedef PVOID(WINAPI* VirtualAlloc2Fn)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER*, ULONG);
typedef PVOID(WINAPI* MapViewOfFile3Fn)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG,
                                        MEM_EXTENDED_PARAMETER*, ULONG);
typedef BOOL(WINAPI* UnmapViewOfFile2Fn)(HANDLE, PVOID, ULONG);

#define MAX_MAPPING_LISTENERS 8

// A frame mapped at more than one address: the addresses after the first (see frame_va).
struct FrameAlias {
    u32 id;
    u64 va;
};

struct MemoryState {
    VirtualAlloc2Fn virtual_alloc2;
    MapViewOfFile3Fn map_view3;
    UnmapViewOfFile2Fn unmap_view2;

    // Physical memory is a set of committed sections ("chunks") created as pages get allocated, so
    // the commit charge follows actual use.
    HANDLE chunks[MAX_CHUNKS];
    bool region_claimed[MAX_REGIONS]; // preferred by a slice of a placeholder range (see preferred_real)
    u32 chunk_count;                  // chunks covering all of physical memory (created or not)
    u8* linear;                       // every chunk is mapped here, real frame r at r * MEM_PAGE_SIZE
    u64 total_pages;

    // The page frame numbers titles get are IDs. An ID is bound to a real frame (a page of the
    // linear view) only when it is first mapped or touched, which lets the binding follow the
    // title's address layout (see bind_frame). Allocating IDs reserves the budget, so binding
    // never runs out.
    u64 used_pages;    // allocated IDs
    u64* id_bitmap;    // 1 = ID allocated
    u32* id_to_real;   // MEM_INVALID_PFN while unbound
    u32* real_to_id;   // for bound real frames
    u64* real_bitmap;  // 1 = real frame bound
    u64* dirty_bitmap; // 1 = real frame was used before, zeroed when bound again
    u64 id_hint;
    u64 real_hint;
    u64 bound_real;     // real frames bound (bits set in real_bitmap)
    u32 chunks_created; // chunks whose section exists
    SRWLOCK phys_lock;

    RangeSlot ranges[MAX_RANGES];
    u32 range_count;
    SRWLOCK range_lock;

    u64 gpu_lowest;
    u64 gpu_highest;

    MemMappingListener mapping_listeners[MAX_MAPPING_LISTENERS];
    volatile long mapping_listener_count;

    // Where frames are mapped, for write watches: per frame ID the first address showing it (0: none),
    // further addresses in `aliases`, sorted by ID. Range lock held.
    u64* frame_va;
    FrameAlias* aliases;
    u32 alias_count;
    u32 alias_capacity;
};

extern MemoryState g_mem;

inline u32 range_pages(const RangeSlot* s) {
    return (u32)(s->info.size >> MEM_PAGE_SHIFT);
}

inline u8* page_va(const RangeSlot* s, u32 page) {
    return s->info.base + ((u64)page << MEM_PAGE_SHIFT);
}

// Index of the region containing `page`.
inline u32 region_find(const RangeSlot* s, u32 page) {
    u32 lo = 0, hi = s->region_count; // regions[0].first_page == 0
    while (hi - lo > 1) {
        u32 mid = lo + (hi - lo) / 2;
        if (s->regions[mid].first_page <= page)
            lo = mid;
        else
            hi = mid;
    }
    return lo;
}

inline u32 region_end(const RangeSlot* s, u32 index) {
    return index + 1 < s->region_count ? s->regions[index + 1].first_page : range_pages(s);
}

inline u32 mapping_protect(const RangeSlot* s) {
    // GPU-only ranges are still mapped CPU-accessible on our side; the title never touches them on
    // the CPU, but we need to read them for uploads.
    u32 prot = s->info.protect;
    if ((prot & 0xFF) == PAGE_NOACCESS || !(prot & 0xFF))
        prot = PAGE_READWRITE;
    return prot;
}

inline RangeSlot* find_range_locked(const void* addr) {
    for (u32 i = 0; i < g_mem.range_count; i++) {
        RangeSlot& s = g_mem.ranges[i];
        if ((const u8*)addr >= s.info.base && (const u8*)addr < s.info.base + s.info.size)
            return &s;
    }
    return nullptr;
}

// The frame ID mapped at `page` (pending or backed), or MEM_INVALID_PFN. Range lock held, phys_lock
// held shared.
inline u32 frame_at_locked(const RangeSlot* s, u32 page) {
    if (s->pending && s->pending[page] != MEM_INVALID_PFN)
        return s->pending[page];
    const MemRegion& r = s->regions[region_find(s, page)];
    if (r.pfn == REGION_PLACEHOLDER)
        return MEM_INVALID_PFN;
    return g_mem.real_to_id[r.pfn + (page - r.first_page)];
}

inline u32 alias_lower_bound(u32 id) {
    u32 lo = 0, hi = g_mem.alias_count;
    while (lo < hi) {
        u32 mid = lo + (hi - lo) / 2;
        if (g_mem.aliases[mid].id < id)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

// The rest of the memory system (memory.cpp).

// Whether `access` (0 read, 1 write, 8 execute) is allowed at `addr` now.
bool access_allowed(const void* addr, ULONG_PTR access);

// Write tracking (memory_tracking.cpp).

void tracking_init(); // after the frame tables exist
// Frames were mapped at pages [first_page, first_page + count) of `s`, real frames from `real` on: what
// tracking protects is protected there too. Range lock held exclusively, phys lock not held.
void tracking_view_mapped(const RangeSlot* s, u32 first_page, u32 count, u32 real);
// Pages were committed in [lo, hi) of a range that isn't a placeholder range. Range lock held.
void tracking_committed(const RangeSlot* s, u64 lo, u64 hi);
// [base, base + size) shows other memory now: watches reporting there end, unreported (mapping
// listeners mark what is there). No locks held.
void tracking_mapping_changed(const u8* base, u64 size);
// `s` is being released: its watches and traps go. Range lock held exclusively.
void tracking_range_released(RangeSlot* s);
// Frame ID `id` was freed: it is nobody's to take any more. Phys lock held.
void tracking_frame_freed(u32 id);
// A write fault on memory tracking protects (watches end; a trapped store gets single-stepped), and
// the single step after a trapped store. Whether the exception was handled.
bool tracking_write_fault(EXCEPTION_POINTERS* ep, u64 addr);
// A fault at `addr` on a taken page: the taken listener gives it back. True if the access can retry.
bool tracking_access_fault(u64 addr);
// Whether tracking protects the page at `addr` against this access (taken, or watched for a write): a
// retried access is then tracking's to claim. Range lock held exclusively (protection settled).
bool tracking_protects_access(const RangeSlot* s, u64 addr, bool write);
// Logs what the memory system knows about a write fault nothing claimed (diagnostics before a crash).
void tracking_describe_write_fault(u64 addr);
bool tracking_single_step(EXCEPTION_POINTERS* ep);
