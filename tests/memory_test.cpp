// Tests for the console memory model (core/memory), and the entry point of the unit tests. Run:
// sludgemu_tests.exe (exit code = failures).

#include <windows.h>
#include <intrin.h>
#include <stdio.h>
#include <string.h>

#include "common/log.h"
#include "core/memory.h"
#include "test.h"

int g_failures = 0;

#define PAGE MEM_PAGE_SIZE

// State the callbacks write is volatile, and so are accesses through aliases: callbacks run from the
// fault handler in the middle of a plain store or load, which optimized builds otherwise move or fold.
static volatile u32 g_notifications = 0;
static const u8* volatile g_last_notified = nullptr;

static void on_mapping_changed(const u8* base, u64) {
    g_notifications = g_notifications + 1;
    g_last_notified = base;
}

static bool range_is_free(const u8* base, u64 size) {
    const u8* p = base;
    while (p < base + size) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_FREE)
            return false;
        p = (const u8*)mbi.BaseAddress + mbi.RegionSize;
    }
    return true;
}

// A placeholder range with every page state mixed in: mapped pages, a split-but-unmapped page and
// several unsplit runs. Releasing it must free all of it, with every release succeeding once.
static void test_placeholder_release() {
    const u64 pages = 16;
    MemAddressLimits limits = {};
    u8* base = (u8*)mem_reserve_placeholder(pages * PAGE, limits, 0, PAGE_READWRITE, 0);
    CHECK(base != nullptr);
    if (!base)
        return;

    u64 pfns[3];
    CHECK(mem_physical_alloc(3, pfns));
    void* vas[3] = {base + 3 * PAGE, base + 4 * PAGE, base + 10 * PAGE};
    CHECK(mem_map_physical_scatter(vas, 3, pfns));
    ((volatile u8*)vas[0])[0] = 1; // mapped and writable

    u64 unmap = MEM_INVALID_PFN;
    CHECK(mem_map_physical(base + 4 * PAGE, 1, &unmap)); // page 4: split, nothing mapped

    u32 errors = log_error_count();
    CHECK(mem_release(base));
    CHECK(log_error_count() == errors);
    CHECK(range_is_free(base, pages * PAGE));
    CHECK(!mem_is_tracked(base));
    mem_physical_free(3, pfns);
}

// A fully backed range owns its pages: releasing it returns them to the physical allocator.
static void test_backed_release() {
    MemAddressLimits limits = {};
    u64 used = mem_physical_used_pages();
    u8* base = (u8*)mem_alloc_physical_backed(8 * PAGE, limits, 0, PAGE_READWRITE, 0);
    CHECK(base != nullptr);
    if (!base)
        return;
    CHECK(mem_physical_used_pages() == used + 8);
    base[0] = 1;
    base[8 * PAGE - 1] = 1;

    u32 errors = log_error_count();
    CHECK(mem_release(base));
    CHECK(log_error_count() == errors);
    CHECK(range_is_free(base, 8 * PAGE));
    CHECK(mem_physical_used_pages() == used);
}

// Recycled frames come back zeroed. With every other frame taken, the reallocation has to return
// the frame that was just freed.
static void test_frames_zeroed_on_reuse() {
    u64 count = mem_physical_total_pages() - mem_physical_used_pages();
    u64* all = (u64*)VirtualAlloc(nullptr, count * sizeof(u64), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    CHECK(mem_physical_alloc(count, all));
    u64 victim = all[count / 2];
    mem_physical_ptr(victim)[123] = 0xAB;
    mem_physical_free(1, &victim);

    u64 again;
    CHECK(mem_physical_alloc(1, &again));
    CHECK(again == victim);
    CHECK(mem_physical_ptr(again)[123] == 0);

    all[count / 2] = again;
    mem_physical_free(count, all);
    VirtualFree(all, 0, MEM_RELEASE);
}

// Writes a recognizable value into each frame through the linear view.
static void tag_frames(const u64* pfns, u32 count) {
    for (u32 i = 0; i < count; i++)
        *(volatile u64*)mem_physical_ptr(pfns[i]) = 0x5100000000ull + pfns[i];
}

static bool shows_frame(const u8* va, u64 pfn) {
    return *(const volatile u64*)va == 0x5100000000ull + pfn;
}

// The allocation (view or placeholder) that contains `va`, and its size.
static void query_allocation(const u8* va, void** allocation_base, u64* size, u32* state) {
    MEMORY_BASIC_INFORMATION mbi;
    VirtualQuery(va, &mbi, sizeof(mbi));
    *allocation_base = mbi.AllocationBase;
    *size = mbi.RegionSize;
    *state = mbi.State;
}

static void test_runs_and_remaps() {
    const u32 pages = 32;
    MemAddressLimits limits = {};
    u8* base = (u8*)mem_reserve_placeholder(pages * PAGE, limits, 0, PAGE_READWRITE, 0);
    CHECK(base != nullptr);
    if (!base)
        return;

    // Frames come from a fresh area of physical memory, so they are consecutive (page numbers count
    // 4 KiB units, so consecutive frames are 16 apart).
    u64 run[8];
    CHECK(mem_physical_alloc(8, run));
    for (u32 i = 1; i < 8; i++)
        CHECK(run[i] == run[0] + ((u64)i << MEM_PAGE_NUMBER_SHIFT));
    u64 other[2];
    CHECK(mem_physical_alloc(2, other));
    tag_frames(run, 8);
    tag_frames(other, 2);
    u32 errors = log_error_count();

    // A run of consecutive frames is one view, once backed.
    CHECK(mem_map_physical(base + 4 * PAGE, 8, run));
    mem_prepare(base + 4 * PAGE, 8 * PAGE);
    void* alloc;
    u64 size;
    u32 state;
    query_allocation(base + 4 * PAGE, &alloc, &size, &state);
    CHECK(alloc == base + 4 * PAGE);
    CHECK(size == 8 * PAGE);
    for (u32 i = 0; i < 8; i++)
        CHECK(shows_frame(base + (4 + i) * PAGE, run[i]));
    // The run is one block, seen through the linear view.
    MemBlock block;
    CHECK(mem_backing_block(base + 6 * PAGE + 100, &block));
    CHECK(block.base == base + 4 * PAGE && block.size == 8 * PAGE && block.host == mem_physical_ptr(run[0]));
    CHECK(!mem_backing_block(base + 3 * PAGE, &block)); // page 3 isn't mapped

    // Remapping one page in the middle splits the view; every page still shows the right frame.
    u32 notified = g_notifications;
    CHECK(mem_map_physical(base + 7 * PAGE, 1, &other[0]));
    CHECK(g_notifications == notified + 1);
    CHECK(g_last_notified == base + 7 * PAGE);
    for (u32 i = 0; i < 8; i++)
        CHECK(shows_frame(base + (4 + i) * PAGE, i == 3 ? other[0] : run[i]));
    query_allocation(base + 4 * PAGE, &alloc, &size, &state);
    CHECK(size == 3 * PAGE);
    // Mapping the frames that are there again changes nothing: no notification, the views stay.
    u64 again[8] = {run[0], run[1], run[2], other[0], run[4], run[5], run[6], run[7]};
    notified = g_notifications;
    CHECK(mem_map_physical(base + 4 * PAGE, 8, again));
    CHECK(g_notifications == notified);
    query_allocation(base + 4 * PAGE, &alloc, &size, &state);
    CHECK(alloc == base + 4 * PAGE && size == 3 * PAGE);
    // A span where one page changes notifies that page alone (here: back to the run, then away again).
    again[3] = run[3];
    CHECK(mem_map_physical(base + 4 * PAGE, 8, again));
    CHECK(g_notifications == notified + 1 && g_last_notified == base + 7 * PAGE);
    again[3] = other[0];
    void* again_vas[8];
    for (u32 i = 0; i < 8; i++)
        again_vas[i] = base + (4 + i) * PAGE;
    CHECK(mem_map_physical_scatter(again_vas, 8, again));
    CHECK(g_notifications == notified + 2 && g_last_notified == base + 7 * PAGE);
    for (u32 i = 0; i < 8; i++)
        CHECK(shows_frame(base + (4 + i) * PAGE, i == 3 ? other[0] : run[i]));
    // The frames are no longer consecutive at page 7: three blocks.
    CHECK(mem_backing_block(base + 5 * PAGE, &block));
    CHECK(block.base == base + 4 * PAGE && block.size == 3 * PAGE && block.host == mem_physical_ptr(run[0]));
    CHECK(mem_backing_block(base + 7 * PAGE, &block));
    CHECK(block.base == base + 7 * PAGE && block.size == PAGE && block.host == mem_physical_ptr(other[0]));
    CHECK(mem_backing_block(base + 11 * PAGE, &block));
    CHECK(block.base == base + 8 * PAGE && block.size == 4 * PAGE && block.host == mem_physical_ptr(run[4]));

    // Separately mapped views whose frames continue the sequence form one block; a view whose
    // frames don't starts a new one.
    CHECK(mem_map_physical(base + 12 * PAGE, 2, &run[0]));
    CHECK(mem_map_physical(base + 14 * PAGE, 1, &run[2]));
    mem_prepare(base + 12 * PAGE, 3 * PAGE);
    CHECK(mem_backing_block(base + 14 * PAGE, &block));
    CHECK(block.base == base + 12 * PAGE && block.size == 3 * PAGE && block.host == mem_physical_ptr(run[0]));
    CHECK(mem_backing_block(base + 11 * PAGE, &block) && block.base == base + 8 * PAGE); // run[7], run[0]: no
    u64 invalid3[3] = {MEM_INVALID_PFN, MEM_INVALID_PFN, MEM_INVALID_PFN};
    CHECK(mem_map_physical(base + 12 * PAGE, 3, invalid3));

    // Aliasing: the same frame at a second address, writes visible through both.
    CHECK(mem_map_physical(base + 20 * PAGE, 1, &other[0]));
    ((volatile u8*)base)[20 * PAGE + 100] = 0x77;
    CHECK(((volatile u8*)base)[7 * PAGE + 100] == 0x77);

    // Unmapping returns a page to a placeholder.
    u64 invalid = MEM_INVALID_PFN;
    CHECK(mem_map_physical(base + 5 * PAGE, 1, &invalid));
    query_allocation(base + 5 * PAGE, &alloc, &size, &state);
    CHECK(state == MEM_RESERVE);
    CHECK(shows_frame(base + 4 * PAGE, run[0]));
    CHECK(shows_frame(base + 6 * PAGE, run[2]));

    // Scatter: non-consecutive addresses.
    void* vas[3] = {base + 0 * PAGE, base + 30 * PAGE, base + 31 * PAGE};
    u64 scatter[3] = {other[1], run[6], run[7]};
    CHECK(mem_map_physical_scatter(vas, 3, scatter));
    CHECK(shows_frame(base, other[1]));
    CHECK(shows_frame(base + 30 * PAGE, run[6]));
    CHECK(shows_frame(base + 31 * PAGE, run[7]));

    // Mapping a frame that isn't allocated is refused and changes nothing.
    u64 bogus = mem_physical_total_pages() - 1;
    CHECK(!mem_map_physical(base + 4 * PAGE, 1, &bogus));
    CHECK(shows_frame(base + 4 * PAGE, run[0]));
    CHECK(!mem_map_physical(base + 31 * PAGE, 2, run)); // runs past the range
    errors += 2;                                        // both refusals are logged

    CHECK(log_error_count() == errors);
    CHECK(mem_release(base));
    CHECK(log_error_count() == errors);
    CHECK(range_is_free(base, pages * PAGE));
    mem_physical_free(8, run);
    mem_physical_free(2, other);
}

// Titles allocate page frames first and map slices of them later, not necessarily in address order.
// Frames are placed in memory when first mapped, following the range's layout, so the whole range
// still ends up as one block the GPU can see.
static void test_out_of_order_slices_form_one_block() {
    const u32 pages = 40;
    MemAddressLimits limits = {};
    u8* base = (u8*)mem_reserve_placeholder(pages * PAGE, limits, 0, PAGE_READWRITE, 0);
    CHECK(base != nullptr);
    if (!base)
        return;
    u64 ids[pages];
    CHECK(mem_physical_alloc(pages, ids));
    CHECK(mem_map_physical(base + 20 * PAGE, 10, &ids[0]));
    CHECK(mem_map_physical(base + 30 * PAGE, 10, &ids[10]));
    CHECK(mem_map_physical(base + 0 * PAGE, 10, &ids[20]));
    CHECK(mem_map_physical(base + 10 * PAGE, 10, &ids[30]));

    MemBlock block;
    mem_prepare(base, pages * PAGE);
    CHECK(mem_backing_block(base + 25 * PAGE, &block));
    CHECK(block.base == base && block.size == pages * PAGE);
    base[3 * PAGE + 5] = 0x42; // ids[23], seen through the linear view
    CHECK(mem_physical_ptr(ids[23])[5] == 0x42);
    CHECK(block.host + 3 * PAGE == mem_physical_ptr(ids[23]));

    CHECK(mem_release(base));
    mem_physical_free(pages, ids);
}

// Mapped frames stay placeholders until touched; a CPU access backs them transparently, and
// mem_prepare backs them for those that can't fault (the GPU, kernel I/O).
static void test_lazy_backing() {
    const u32 pages = 8;
    MemAddressLimits limits = {};
    u8* base = (u8*)mem_reserve_placeholder(pages * PAGE, limits, 0, PAGE_READWRITE, 0);
    CHECK(base != nullptr);
    if (!base)
        return;
    u64 frames[pages];
    CHECK(mem_physical_alloc(pages, frames));
    tag_frames(frames, pages);
    CHECK(mem_map_physical(base, pages, frames));

    void* alloc;
    u64 size;
    u32 state;
    query_allocation(base, &alloc, &size, &state);
    CHECK(state == MEM_RESERVE); // pending
    MemBlock block;
    CHECK(!mem_backing_block(base, &block));
    CHECK(shows_frame(base + 5 * PAGE, frames[5])); // the access backs the pages and retries
    query_allocation(base, &alloc, &size, &state);
    CHECK(state == MEM_COMMIT);

    // Remapped pages are pending again; mem_prepare backs them.
    CHECK(mem_map_physical(base + 2 * PAGE, 2, &frames[6]));
    query_allocation(base + 2 * PAGE, &alloc, &size, &state);
    CHECK(state == MEM_RESERVE);
    mem_prepare(base + 2 * PAGE, 2 * PAGE);
    CHECK(mem_backing_block(base + 2 * PAGE, &block));
    CHECK(shows_frame(base + 3 * PAGE, frames[7]));

    // Page number 0 is how titles release a slot: it unmaps.
    u64 none = 0;
    u32 errors = log_error_count();
    CHECK(mem_map_physical(base + 3 * PAGE, 1, &none));
    CHECK(log_error_count() == errors);
    mem_prepare(base + 3 * PAGE, PAGE);
    query_allocation(base + 3 * PAGE, &alloc, &size, &state);
    CHECK(state == MEM_RESERVE);

    CHECK(mem_release(base));
    CHECK(range_is_free(base, pages * PAGE));
    mem_physical_free(pages, frames);
}

// Large commits are reserved and committed as they're touched, a granule at a time.
static void test_lazy_commit() {
    MemAddressLimits limits = {};
    const u64 size = MiB(64);
    u8* base = (u8*)mem_alloc_virtual(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE, limits, 0, 0);
    CHECK(base != nullptr);
    if (!base)
        return;
    void* alloc;
    u64 region;
    u32 state;
    query_allocation(base + MiB(40), &alloc, &region, &state);
    CHECK(state == MEM_RESERVE);
    base[MiB(40) + 12] = 7; // faults, gets committed, retries
    CHECK(base[MiB(40) + 12] == 7);
    query_allocation(base + MiB(40), &alloc, &region, &state);
    CHECK(state == MEM_COMMIT);
    query_allocation(base + MiB(20), &alloc, &region, &state);
    CHECK(state == MEM_RESERVE); // untouched parts stay reserved
    mem_prepare(base + MiB(20), 16);
    query_allocation(base + MiB(20), &alloc, &region, &state);
    CHECK(state == MEM_COMMIT);
    CHECK(mem_release(base));
    CHECK(range_is_free(base, size));
}

// GPU-visible ranges are sections: the title's view commits as it is touched, the backing block is our
// own view of the same memory, and the title's pages can be protected (nothing locks them).
static void test_gpu_section_ranges() {
    MemAddressLimits limits = {};
    const u64 size = MiB(64);
    u8* base = (u8*)mem_alloc_virtual(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE, limits,
                                      MEM_RANGE_GPU, 0);
    CHECK(base != nullptr);
    if (!base)
        return;
    MemRange r;
    CHECK(mem_query_range(base, &r) && (r.flags & MEM_RANGE_SECTION) && (r.flags & MEM_RANGE_LAZY));
    void* alloc;
    u64 region;
    u32 state;
    query_allocation(base + MiB(20), &alloc, &region, &state);
    CHECK(state == MEM_RESERVE); // lazily committed
    base[MiB(20) + 5] = 42;      // faults, gets committed, retries
    query_allocation(base + MiB(20), &alloc, &region, &state);
    CHECK(state == MEM_COMMIT);
    MemBlock block;
    CHECK(mem_backing_block(base + MiB(20), &block));
    CHECK(block.base == base && block.size == size && block.host != base);
    CHECK(block.host && block.host[MiB(20) + 5] == 42); // the same memory
    DWORD old;
    CHECK(VirtualProtect(base + MiB(20), 4096, PAGE_READONLY, &old) && old == PAGE_READWRITE);
    CHECK(VirtualProtect(base + MiB(20), 4096, PAGE_READWRITE, &old));
    // Committing inside a reserved section range uses the range's protection.
    u8* reserved =
        (u8*)mem_alloc_virtual(nullptr, MiB(4), MEM_RESERVE, PAGE_READWRITE, limits, MEM_RANGE_GPU, 0);
    CHECK(reserved != nullptr);
    if (reserved) {
        CHECK(mem_alloc_virtual(reserved + MiB(1), MiB(1), MEM_COMMIT, PAGE_READWRITE | PAGE_WRITECOMBINE,
                                limits, MEM_RANGE_GPU, 0) == reserved + MiB(1));
        reserved[MiB(1) + 7] = 1;
        CHECK(mem_release(reserved));
    }
    CHECK(mem_release(base));
    CHECK(range_is_free(base, size));
}

static volatile u32 g_write_reports = 0;
static volatile u64 g_write_base = 0, g_write_size = 0;

static volatile u32 g_dropped_reports; // watches ended by other memory mapped where they were

static void on_write(u64 base, u64 size, MemWriteKind kind) {
    if (kind == MEM_WATCH_DROPPED) {
        g_dropped_reports = g_dropped_reports + 1;
        return;
    }
    g_write_reports = g_write_reports + 1;
    g_write_base = base;
    g_write_size = size;
}

static bool page_writable(const void* p) {
    MEMORY_BASIC_INFORMATION mbi;
    return VirtualQuery(p, &mbi, sizeof(mbi)) && mbi.State == MEM_COMMIT &&
           (mbi.Protect & 0xFF) == PAGE_READWRITE;
}

// Write watches: reads pass and the first write reports the watched range, also through another
// address showing the same frames, and releases the pages at every address; watches survive views
// being remapped and pages being backed or committed, and end unreported when other frames are mapped
// at their range.
static void test_write_watches() {
    mem_add_write_listener(on_write);
    MemAddressLimits limits = {};
    u8* a = (u8*)mem_reserve_placeholder(4 * PAGE, limits, 0, PAGE_READWRITE, 0);
    u8* b = (u8*)mem_reserve_placeholder(4 * PAGE, limits, 0, PAGE_READWRITE, 0);
    CHECK(a && b);
    if (!a || !b)
        return;
    u64 frames[3];
    CHECK(mem_physical_alloc(3, frames));
    CHECK(mem_map_physical(a, 2, frames));        // a: frames 0, 1 (pending until touched)
    CHECK(mem_map_physical(b + PAGE, 2, frames)); // b: the same frames, one page further
    volatile u8* pa = a;
    volatile u8* pb = b;

    mem_watch_writes(a, 2 * PAGE); // over pending pages: protected once they are backed
    CHECK(pa[100] == 0);           // a read backs the page, which comes up read-only
    CHECK(!page_writable(a));
    CHECK(g_write_reports == 0);
    pa[PAGE + 5] = 1; // a write backs and faults once more; reported
    CHECK(g_write_reports == 1 && g_write_base == (u64)a && g_write_size == 2 * PAGE);
    CHECK(pa[PAGE + 5] == 1 && page_writable(a));

    // Through the alias.
    mem_watch_writes(a, 2 * PAGE);
    CHECK(pb[PAGE + 7] == 0);
    CHECK(!page_writable(b + PAGE));
    pb[2 * PAGE + 9] = 2; // frame 1 through b
    CHECK(g_write_reports == 2 && g_write_base == (u64)a && g_write_size == 2 * PAGE);
    CHECK(pa[PAGE + 9] == 2 && page_writable(a + PAGE) && page_writable(b + 2 * PAGE));
    mem_end_write_watches(a, 2 * PAGE); // nothing left to end
    CHECK(g_write_reports == 2);

    // A view remapped over the same frames keeps the watch; other frames end it.
    g_write_reports = 0;
    mem_watch_writes(a, PAGE);
    CHECK(mem_map_physical(a + PAGE, 1, &frames[2])); // splits a's view next to the watch
    CHECK(g_write_reports == 0);
    CHECK(!page_writable(a));
    g_dropped_reports = 0;
    CHECK(mem_map_physical(a, 1, &frames[2]));
    CHECK(g_write_reports == 0 && g_dropped_reports == 1 && page_writable(b + PAGE)); // dropped; frame 0 free at b
    CHECK(mem_release(a));
    CHECK(mem_release(b));
    mem_physical_free(3, frames);
    g_write_reports = 0;

    // Pool pages shown elsewhere in another order: one piece per page, all watched.
    const u32 n = 20;
    u8* c = (u8*)mem_reserve_placeholder(n * PAGE, limits, 0, PAGE_READWRITE, 0);
    u8* d = (u8*)mem_reserve_placeholder(n * PAGE, limits, 0, PAGE_READWRITE, 0);
    u64 pool[n], reversed[n];
    CHECK(c && d && mem_physical_alloc(n, pool));
    if (c && d) {
        for (u32 i = 0; i < n; i++)
            reversed[i] = pool[n - 1 - i];
        CHECK(mem_map_physical(c, n, pool));
        CHECK(mem_map_physical(d, n, reversed));
        mem_prepare(c, n * PAGE);
        mem_prepare(d, n * PAGE);
        mem_watch_writes(c, n * PAGE);
        CHECK(!page_writable(d) && !page_writable(d + (n - 1) * PAGE));
        ((volatile u8*)d)[3] = 9; // pool page n - 1, the last of c
        CHECK(g_write_reports == 1 && g_write_base == (u64)c && g_write_size == n * PAGE);
        CHECK(c[(n - 1) * PAGE + 3] == 9);
        mem_end_write_watches(c, n * PAGE);
        CHECK(mem_release(c) && mem_release(d));
        mem_physical_free(n, pool);
    }
    g_write_reports = 0;

    // Section memory: committing a granule again keeps watched pages protected.
    u8* gpu = (u8*)mem_alloc_virtual(nullptr, MiB(32), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE, limits,
                                     MEM_RANGE_GPU, 0);
    CHECK(gpu != nullptr);
    if (gpu) {
        gpu[MiB(3)] = 1;
        mem_watch_writes(gpu + MiB(3), 4096);
        mem_prepare(gpu + MiB(3), MiB(1)); // commits the granule again
        CHECK(!page_writable(gpu + MiB(3)));
        ((volatile u8*)gpu)[MiB(3) + 10] = 3;
        CHECK(g_write_reports == 1 && g_write_base == (u64)gpu + MiB(3) && g_write_size == 4096);
        // A write elsewhere in a protected host page (small textures share pages) is caught too.
        gpu[MiB(5)] = 1;
        mem_watch_writes(gpu + MiB(5), 0x400);
        ((volatile u8*)gpu)[MiB(5) + 0xB00] = 4;
        CHECK(g_write_reports == 2 && g_write_base == (u64)gpu + MiB(5) && g_write_size == 0x400);
        CHECK(mem_release(gpu));
    }
    mem_remove_write_listener(on_write);
}

// Protection is counted per page: watches sharing pages end only when their own pages are written,
// a page stays protected while any watch needs it, unwatching releases only what nobody else needs,
// and GPU-only memory isn't protected.
static void test_watch_counts() {
    mem_add_write_listener(on_write);
    g_write_reports = 0;
    MemAddressLimits limits = {};
    u8* gpu = (u8*)mem_alloc_virtual(nullptr, MiB(16), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE, limits,
                                     MEM_RANGE_GPU, 0);
    CHECK(gpu != nullptr);
    if (!gpu)
        return;
    memset(gpu, 0, 5 * 4096);
    mem_watch_writes(gpu, 3 * 4096);        // watch A: pages 0-2
    mem_watch_writes(gpu + 2 * 4096, 8192); // watch B: pages 2-3 (shares page 2)
    mem_watch_writes(gpu, 3 * 4096);        // armed already: nothing changes
    mem_watch_writes(gpu, 3 * 4096, 1);     // another client's watch over the same range
    mem_unwatch_writes(gpu, 3 * 4096, 1);   // that client lets go: watch A stays
    CHECK(!page_writable(gpu) && !page_writable(gpu + 3 * 4096));
    ((volatile u8*)gpu)[3 * 4096] = 1; // B's own page: B ends, A's pages stay protected
    CHECK(g_write_reports == 1 && g_write_base == (u64)gpu + 2 * 4096);
    CHECK(!page_writable(gpu + 2 * 4096) && page_writable(gpu + 3 * 4096));
    ((volatile u8*)gpu)[2 * 4096] = 1; // now only A covers page 2
    CHECK(g_write_reports == 2 && g_write_base == (u64)gpu && page_writable(gpu));

    // Two watches over one page; unwatching one keeps the page protected for the other.
    mem_watch_writes(gpu + 4 * 4096, 0x100);
    mem_watch_writes(gpu + 4 * 4096 + 0x800, 0x100);
    mem_unwatch_writes(gpu + 4 * 4096, 0x100);
    CHECK(!page_writable(gpu + 4 * 4096));
    ((volatile u8*)gpu)[4 * 4096 + 0x10] = 1; // the page is written: the remaining watch ends
    CHECK(g_write_reports == 3 && g_write_base == (u64)gpu + 4 * 4096 + 0x800 &&
          page_writable(gpu + 4 * 4096));
    CHECK(mem_release(gpu));

    // GPU-only memory: the CPU can't write it, so it needs no protection.
    u8* gpu_only = (u8*)mem_alloc_virtual(nullptr, MiB(16), MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS, limits,
                                          MEM_RANGE_GPU, 0);
    if (gpu_only) {
        MEMORY_BASIC_INFORMATION before = {}, after = {};
        VirtualQuery(gpu_only, &before, sizeof(before));
        mem_watch_writes(gpu_only, 4096);
        VirtualQuery(gpu_only, &after, sizeof(after));
        CHECK(after.Protect == before.Protect); // left alone
        mem_unwatch_writes(gpu_only, 4096);
        CHECK(mem_release(gpu_only));
    }
    mem_remove_write_listener(on_write);
}

static volatile u32 g_store_reports = 0;
static volatile u64 g_store_base = 0, g_store_value = 0;

static void on_store(u64 base, u64) {
    g_store_reports = g_store_reports + 1;
    g_store_base = base;
    g_store_value = *(volatile u64*)base; // the store is done when it is reported
}

struct StoreThread {
    volatile u64* counter;
    u32 stores;
};

static DWORD WINAPI store_thread(void* p) {
    StoreThread* t = (StoreThread*)p;
    for (u32 i = 1; i <= t->stores; i++)
        *t->counter = 1000 + i;
    return 0;
}

// Store traps: every store into the trapped bytes is reported after it is done, also on pages a watch
// shares, by stores straddling two trapped pages and by repeated string stores; the pages stay
// protected throughout, also while another thread stores.
// The taken listener brings a page up to date through the host view of its memory and gives it back.
static volatile u32 g_taken_calls = 0;
static const void* volatile g_taken_addr = nullptr;
static volatile u8 g_taken_value = 0;

static void on_taken(const void* addr) {
    g_taken_calls = g_taken_calls + 1;
    g_taken_addr = addr;
    MemBlock block;
    if (mem_backing_block(addr, &block)) {
        u64 page = ALIGN_DOWN((u64)addr - (u64)block.base, PAGE);
        memset(block.host + page, g_taken_value, PAGE); // the newer copy
        mem_give_pages(block.base + page, PAGE);
    }
}

static bool page_inaccessible(const void* p) {
    MEMORY_BASIC_INFORMATION mbi;
    return VirtualQuery(p, &mbi, sizeof(mbi)) && (mbi.Protect & 0xFF) == PAGE_NOACCESS;
}

// Taken pages: no access at any address showing the frame until the listener gives them back, reads
// and writes alike; watches stay armed across taking; section memory works the same; freed frames
// aren't taken any more.
static void test_taken_pages() {
    mem_set_taken_listener(on_taken);
    mem_add_write_listener(on_write);
    MemAddressLimits limits = {};
    u8* a = (u8*)mem_reserve_placeholder(2 * PAGE, limits, 0, PAGE_READWRITE, 0);
    u8* b = (u8*)mem_reserve_placeholder(2 * PAGE, limits, 0, PAGE_READWRITE, 0);
    u64 frames[2];
    CHECK(a && b && mem_physical_alloc(2, frames));
    if (!a || !b)
        return;
    CHECK(mem_map_physical(a, 2, frames));
    CHECK(mem_map_physical(b, 1, &frames[1])); // b shows a's second frame
    mem_prepare(a, 2 * PAGE);
    mem_prepare(b, PAGE);
    volatile u8* pa = a;
    volatile u8* pb = b;

    g_taken_calls = 0;
    mem_take_pages(a + PAGE, PAGE);
    CHECK(page_inaccessible(a + PAGE) && page_inaccessible(b) && !page_inaccessible(a));
    g_taken_value = 0x5A;
    CHECK(pb[10] == 0x5A); // a read through the alias brings the page back
    CHECK(g_taken_calls == 1 && (u64)g_taken_addr == (u64)b + 10);
    CHECK(page_writable(a + PAGE) && page_writable(b));
    pa[PAGE + 1] = 7; // given back: no more calls
    CHECK(g_taken_calls == 1 && pb[1] == 7);

    // Watched and taken: given back, the page is read-only again and the write reports.
    g_write_reports = 0;
    mem_watch_writes(a, PAGE);
    mem_take_pages(a, PAGE);
    CHECK(page_inaccessible(a));
    g_taken_value = 3;
    pa[20] = 4; // a write: given back read-only, then the watch reports
    CHECK(g_taken_calls == 2 && g_write_reports == 1 && pa[20] == 4 && pa[0] == 3 && pa[PAGE - 1] == 3);
    CHECK(page_writable(a));

    // A freed frame isn't taken any more.
    mem_take_pages(a, PAGE);
    CHECK(mem_release(a) && mem_release(b));
    mem_physical_free(2, frames);
    u8* c = (u8*)mem_reserve_placeholder(PAGE, limits, 0, PAGE_READWRITE, 0);
    u64 again;
    CHECK(c && mem_physical_alloc(1, &again));
    if (c) {
        CHECK(mem_map_physical(c, 1, &again));
        ((volatile u8*)c)[5] = 1;
        CHECK(g_taken_calls == 2);
        CHECK(mem_release(c));
    }
    mem_physical_free(1, &again);

    // Section memory.
    u8* gpu = (u8*)mem_alloc_virtual(nullptr, MiB(32), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE, limits,
                                     MEM_RANGE_GPU | MEM_RANGE_CPU_ACCESS, 0);
    CHECK(gpu != nullptr);
    if (gpu) {
        gpu[MiB(2)] = 1;
        mem_take_pages(gpu + MiB(2), PAGE);
        CHECK(page_inaccessible(gpu + MiB(2)));
        g_taken_value = 9;
        CHECK(((volatile u8*)gpu)[MiB(2) + 3] == 9 && gpu[MiB(2)] == 9);
        CHECK(g_taken_calls == 3);
        mem_take_pages(gpu + MiB(2), PAGE);
        CHECK(mem_release(gpu)); // released while taken
    }
    mem_remove_write_listener(on_write);
    mem_set_taken_listener(nullptr);
}

static void test_store_traps() {
    mem_set_store_listener(on_store);
    mem_add_write_listener(on_write);
    MemAddressLimits limits = {};
    u8* gpu = (u8*)mem_alloc_virtual(nullptr, MiB(16), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE, limits,
                                     MEM_RANGE_GPU, 0);
    CHECK(gpu != nullptr);
    if (!gpu)
        return;
    volatile u64* counter = (volatile u64*)(gpu + 0x1000);
    *counter = 1;
    mem_trap_stores((const void*)counter, sizeof(u64));
    CHECK(!page_writable((const void*)counter));
    CHECK(*counter == 1 && g_store_reports == 0); // reads pass
    *counter = 2;
    CHECK(g_store_reports == 1 && g_store_base == (u64)counter && g_store_value == 2);
    CHECK(!page_writable((const void*)counter));
    *counter = 3;
    CHECK(g_store_reports == 2 && g_store_value == 3 && *counter == 3);
    ((volatile u8*)gpu)[0x1800] = 7; // elsewhere on the page: reported too, the listener checks the values
    CHECK(g_store_reports == 3 && ((volatile u8*)gpu)[0x1800] == 7);

    // A watch on the same page ends at the first write; the trap stays.
    g_write_reports = 0;
    mem_watch_writes(gpu + 0x1100, 0x100);
    *counter = 4;
    CHECK(g_write_reports == 1 && g_store_reports == 4 && g_store_value == 4);
    CHECK(!page_writable((const void*)counter));

    // A store straddling two trapped pages, and a repeated string store over a trapped page.
    volatile u64* next = (volatile u64*)(gpu + 0x2000);
    mem_trap_stores((const void*)next, sizeof(u64));
    g_store_reports = 0;
    *(volatile u64*)(gpu + 0x1FFC) = 0x1122334455667788ull;
    CHECK(g_store_reports >= 2 && *next == 0x11223344u);
    CHECK(!page_writable(gpu + 0x1000) && !page_writable(gpu + 0x2000));
    g_store_reports = 0;
    __stosb(gpu + 0x1F00, 0xAB, 0x200);
    CHECK(g_store_reports >= 1 && gpu[0x1F00] == 0xAB && gpu[0x20FF] == 0xAB && gpu[0x2100] == 0);
    CHECK(!page_writable(gpu + 0x1000) && !page_writable(gpu + 0x2000));

    // Another thread storing while this one does: every store lands, the pages end up protected.
    StoreThread t = {next, 2000};
    HANDLE thread = CreateThread(nullptr, 0, store_thread, &t, 0, nullptr);
    for (u32 i = 1; i <= 2000; i++)
        *counter = i;
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
    CHECK(*counter == 2000 && *next == 3000);
    CHECK(!page_writable(gpu + 0x1000) && !page_writable(gpu + 0x2000));

    // A repeated string store across many trapped pages (memcpy's rep movsb over memory full of
    // counters): it finishes, every trap reports, and the pages are protected again.
    for (u32 p = 3; p < 8; p++)
        mem_trap_stores(gpu + p * 0x1000, sizeof(u64));
    static u8 source[0x6000];
    memset(source, 0x5A, sizeof(source));
    g_store_reports = 0;
    __movsb(gpu + 0x1800, source, sizeof(source));
    CHECK(gpu[0x1800] == 0x5A && gpu[0x77FF] == 0x5A && gpu[0x7800] == 0);
    CHECK(g_store_reports >= 7);
    for (u32 p = 1; p < 8; p++)
        CHECK(!page_writable(gpu + p * 0x1000));

    // Untrapped, a page takes stores without reports (the other traps there stay).
    mem_untrap_stores(gpu + 0x5000, sizeof(u64));
    CHECK(page_writable(gpu + 0x5000) && !page_writable(gpu + 0x4000));
    g_store_reports = 0;
    gpu[0x5008] = 1;
    CHECK(g_store_reports == 0);
    for (u32 p = 3; p < 8; p++)
        mem_untrap_stores(gpu + p * 0x1000, sizeof(u64));

    // Page number 0 names no frame.
    CHECK(mem_physical_ptr(0) == nullptr);

    CHECK(mem_release(gpu));
    mem_set_store_listener(nullptr);
    mem_remove_write_listener(on_write);
}

// Device writes: pending until their event is set, then reported like CPU writes; and the addresses
// showing a frame, for writes through page numbers.
static void test_device_writes() {
    mem_add_write_listener(on_write);
    g_write_reports = 0;
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    MemSpan spans[2] = {{0x10000, 0x1000}, {0x50000, 0x2000}};
    mem_expect_device_writes(done, spans, 2);
    CHECK(mem_device_writes_outstanding());
    CHECK(mem_device_writes_pending(0x10800, 0x10) && mem_device_writes_pending(0x4F000, 0x1001));
    CHECK(!mem_device_writes_pending(0x20000, 0x1000) && !mem_device_writes_pending(0x4F000, 0x1000));
    mem_poll_device_writes();
    CHECK(g_write_reports == 0);
    SetEvent(done);
    mem_poll_device_writes();
    CHECK(g_write_reports == 2 && g_write_base == 0x50000 && g_write_size == 0x2000);
    CHECK(!mem_device_writes_outstanding() && !mem_device_writes_pending(0x10800, 0x10));

    MemAddressLimits limits = {};
    u8* a = (u8*)mem_reserve_placeholder(2 * PAGE, limits, 0, PAGE_READWRITE, 0);
    u8* b = (u8*)mem_reserve_placeholder(2 * PAGE, limits, 0, PAGE_READWRITE, 0);
    u64 frame = 0;
    CHECK(a && b && mem_physical_alloc(1, &frame));
    if (a && b) {
        u64 vas[4];
        CHECK(mem_frame_addresses(frame, vas, 4) == 0); // not mapped anywhere yet
        CHECK(mem_map_physical(a + PAGE, 1, &frame) && mem_map_physical(b, 1, &frame));
        u32 n = mem_frame_addresses(frame, vas, 4);
        CHECK(n == 2 && ((vas[0] == (u64)a + PAGE && vas[1] == (u64)b) ||
                         (vas[1] == (u64)a + PAGE && vas[0] == (u64)b)));
        CHECK(mem_frame_addresses(frame, vas, 1) == 2); // how many there are, even if fewer fit
        CHECK(mem_release(a) && mem_release(b));
        CHECK(mem_frame_addresses(frame, vas, 4) == 0);
        mem_physical_free(1, &frame);
    }
    mem_remove_write_listener(on_write);
}

void test_xapu_decode();
void test_xapu_convert();
void test_xdsp();
void test_xma();
void test_layout_addressing();
void test_bcpack();
void test_shader_translate();
void test_shader_lanes();
void test_shader_cache();
void test_shader_schedule();
void test_submission();

int main() {
    log_init(nullptr, LOG_LEVEL_WARN);
    if (!mem_init(GiB(1))) {
        printf("FAIL: mem_init\n");
        return 1;
    }
    mem_add_mapping_listener(on_mapping_changed);
    test_placeholder_release();
    test_runs_and_remaps();
    test_out_of_order_slices_form_one_block();
    test_backed_release();
    test_frames_zeroed_on_reuse();
    test_lazy_backing();
    test_lazy_commit();
    test_gpu_section_ranges();
    test_write_watches();
    test_watch_counts();
    test_store_traps();
    test_taken_pages();
    test_device_writes();
    test_xapu_decode();
    test_xapu_convert();
    test_xdsp();
    test_xma();
    test_layout_addressing();
    test_bcpack();
    test_shader_translate();
    test_shader_lanes();
    test_shader_cache();
    test_shader_schedule();
    test_submission();
    printf("%s (%d failures)\n", g_failures ? "FAILED" : "passed", g_failures);
    return g_failures;
}
