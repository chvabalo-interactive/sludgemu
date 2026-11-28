// Console ID3D12CommandQueue.
//
// Console queues never block the thread submitting to them, and titles rely on it: GPU work may wait
// (on a fence or a counter in memory) for work the same thread submits afterwards. So nothing here
// waits for the GPU. Our own lists come from pools that grow while the GPU is behind, and work that
// desktop D3D12 can't take yet (a list submitted again before its last run finished) is held back in
// the queue's backlog, together with everything after it (queue_backlog.h).

#include <stddef.h>

#include <string.h>

#include "common/log.h"
#include "gpu/gpu.h"
#include "gpu/page_tables.h"
#include "gpu/presenter.h"
#include "hle/d3d12/frame_pacing.h"
#include "hle/d3d12/d3d12_objects.h"
#include "hle/d3d12/d3d12_tables.h"
#include "hle/d3d12/target_tiles.h"
#include "hle/d3d12/texture_sync.h"

static_assert(offsetof(ConsoleCommandQueue, type) == 0x10, "command queue layout");

static const GUID* const k_queue_iids[] = {&__uuidof(ID3D12Object), &__uuidof(ID3D12DeviceChild),
                                           &__uuidof(ID3D12Pageable), &__uuidof(ID3D12CommandQueue)};

static void queue_destroy(void* object);

static const ConsoleClass k_queue_class = {"ID3D12CommandQueue", k_queue_iids, ARRAY_COUNT(k_queue_iids),
                                           queue_destroy};

static void* const* g_queue_table = nullptr;

// Queues by slot, so a queued signal can wake the queues holding waits back.
static ConsoleCommandQueue* g_slot_queues[GPU_MAX_QUEUES];
static SRWLOCK g_slot_queues_lock = SRWLOCK_INIT;

// A signal reached the desktop queue of `self`: other queues may hold back a wait it covers. A
// backlog read without its lock is only a hint: a wait held back meanwhile follows its fence instead,
// which the signal reaches without the CPU.
static void wake_held_waits(const ConsoleCommandQueue* self) {
    AcquireSRWLockShared(&g_slot_queues_lock);
    for (u32 s = 0; s < GPU_MAX_QUEUES; s++) {
        ConsoleCommandQueue* q = g_slot_queues[s];
        if (q && q != self && q->backlog.event && *(volatile u32*)&q->backlog.count)
            SetEvent(q->backlog.event);
    }
    ReleaseSRWLockShared(&g_slot_queues_lock);
}

// --- the backlog -------------------------------------------------------------------------------------

// Hands one held-back op to the desktop queue. Queue lock held.
static void run_op(ConsoleCommandQueue* self, BacklogOp& op) {
    switch (op.kind) {
    case BACKLOG_EXECUTE:
        if (op.count) {
            target_tiles_submit(self->desktop); // textures recorded into the lists get their tiles first
            gpu_page_tables_submit(self->desktop);
        }
        gpu_queue_execute_reserved(self->desktop, self->slot, op.number, op.count, op.lists);
        for (u32 i = 0; i < op.count; i++)
            gpu_retire(op.lists[i]);
        break;
    case BACKLOG_SIGNAL:
        gpu_queue_signal_on(self->desktop, op.fence, op.value);
        gpu_retire(op.fence);
        wake_held_waits(self);
        break;
    case BACKLOG_WAIT:
        gpu_queue_wait_on(self->desktop, op.fence, op.value);
        gpu_retire(op.fence);
        break;
    case BACKLOG_PRESENT: {
        u64 done = presenter_present(self->desktop, op.planes, op.count);
        if (op.count)
            frame_on_present(op.token, done);
        for (u32 i = 0; i < op.count; i++)
            gpu_retire(op.planes[i].texture);
        break;
    }
    }
}

static bool op_ready(const BacklogOp& op) {
    if (op.kind == BACKLOG_WAIT)
        return gpu_fence_wait_covered(op.fence, op.value);
    return op.kind != BACKLOG_EXECUTE || op.after_slot == GPU_NO_QUEUE ||
           gpu_queue_reached(op.after_slot, op.after_generation, op.after);
}

// Sets the backlog's event to follow what the first op waits for: the fence value of a wait, or the
// run lists wait for. A signal queued meanwhile also sets it (wake_held_waits). Queue lock held.
static void arm(ConsoleCommandQueue* self, const BacklogOp& op) {
    QueueBacklog& b = self->backlog;
    ID3D12Fence* fence = op.kind == BACKLOG_WAIT ? op.fence : gpu_queue_fence(op.after_slot);
    u64 value = op.kind == BACKLOG_WAIT ? op.value : op.after;
    if (!b.event || (b.armed_fence == fence && b.armed_value == value))
        return; // (without an event SetEventOnCompletion would block)
    HRESULT hr = fence ? fence->SetEventOnCompletion(value, b.event) : E_FAIL;
    if (FAILED(hr)) {
        if (!gpu_device_lost(hr))
            LOG_E("d3d12", "cannot follow a fence for held-back queue work (0x%08lx)", (unsigned long)hr);
        return; // the next call into the queue tries again
    }
    b.armed_fence = fence;
    b.armed_value = value;
}

// Hands held-back work to the desktop queue, in order, as far as it can go without waiting. Queue lock
// held.
static void backlog_run(ConsoleCommandQueue* self) {
    QueueBacklog& b = self->backlog;
    while (BacklogOp* op = backlog_front(&b)) {
        if (!op_ready(*op)) {
            arm(self, *op);
            return;
        }
        run_op(self, *op);
        backlog_pop(&b);
    }
}

// The run the first held-back op waits for finished (or an older one the event followed).
static void CALLBACK backlog_wakeup(void* context, BOOLEAN) {
    ConsoleCommandQueue* self = (ConsoleCommandQueue*)context;
    AcquireSRWLockExclusive(&self->lock);
    self->backlog.armed_fence = nullptr;
    backlog_run(self);
    ReleaseSRWLockExclusive(&self->lock);
}

// Hands all held-back work to the desktop queue, waiting for what it waits for: only when the queue
// goes away, or without memory to hold more back. Queue lock held.
static void backlog_drain(ConsoleCommandQueue* self) {
    while (BacklogOp* op = backlog_front(&self->backlog)) {
        if (op->kind == BACKLOG_EXECUTE && !op_ready(*op))
            gpu_queue_wait(op->after_slot, op->after);
        run_op(self, *op);
        backlog_pop(&self->backlog);
    }
}

// A new held-back op at the end of the backlog, or nullptr if the work has to go straight to the queue
// (after draining the backlog: logged, out of memory). Queue lock held.
static BacklogOp* hold_back(ConsoleCommandQueue* self) {
    QueueBacklog& b = self->backlog;
    if (!b.event) {
        b.armed_fence = nullptr;
        b.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (b.event && !RegisterWaitForSingleObject(&b.wait, b.event, backlog_wakeup, self, INFINITE,
                                                    WT_EXECUTEDEFAULT)) {
            CloseHandle(b.event);
            b.event = nullptr;
        }
        if (!b.event)
            LOG_E("d3d12",
                  "cannot make a thread pool wait for held-back queue work (%lu); it moves on with "
                  "later submissions only",
                  GetLastError());
    }
    BacklogOp* op = backlog_push(&b);
    if (!op) {
        LOG_E("d3d12", "out of memory holding back queue work; waiting for the GPU instead");
        backlog_drain(self);
    } else if (b.count == 256) {
        LOG_W("d3d12",
              "a queue holds back %u submissions: what the first waits for doesn't come (a fence "
              "value, or the last run of a list submitted again)",
              b.count);
    }
    return op;
}

// --- submissions -------------------------------------------------------------------------------------

// Hands `lists` (none: just a count) to the desktop queue, after the page table work they need, once
// submission `after` of queue slot `after_slot` finished (GPU_NO_QUEUE: no such wait). Held back while
// anything else is, or while that run isn't finished. Returns the queue submission it is (reserved when
// held back). Queue lock held.
static u64 submit(ConsoleCommandQueue* self, u32 count, ID3D12CommandList* const* lists, u32 after_slot,
                  u32 after_generation, u64 after) {
    bool ready = after_slot == GPU_NO_QUEUE || gpu_queue_reached(after_slot, after_generation, after);
    if (!self->backlog.count && ready) {
        if (count) {
            target_tiles_submit(self->desktop); // textures recorded into the lists get their tiles first
            gpu_page_tables_submit(self->desktop);
        }
        return gpu_queue_execute(self->desktop, self->slot, count, lists);
    }
    BacklogOp* op = hold_back(self);
    if (!op) {
        if (!ready)
            gpu_queue_wait(after_slot, after);
        if (count) {
            target_tiles_submit(self->desktop); // textures recorded into the lists get their tiles first
            gpu_page_tables_submit(self->desktop);
        }
        return gpu_queue_execute(self->desktop, self->slot, count, lists);
    }
    op->kind = BACKLOG_EXECUTE;
    op->count = count;
    op->after_slot = ready ? GPU_NO_QUEUE : after_slot;
    op->after_generation = after_generation;
    op->after = after;
    for (u32 i = 0; i < count; i++) {
        lists[i]->AddRef();
        op->lists[i] = lists[i];
    }
    u64 number = gpu_queue_reserve(self->slot);
    op->number = number;
    backlog_run(self); // runs what is ready by now (the op may be gone after) and arms the wait
    return number;
}

// A desktop fence signal or wait on the queue, in order with its submissions. A wait nothing queued
// ends yet is held back until something does. Queue lock held.
static void fence_op(ConsoleCommandQueue* self, BacklogKind kind, ID3D12Fence* fence, u64 value) {
    bool hold = self->backlog.count || (kind == BACKLOG_WAIT && !gpu_fence_wait_covered(fence, value));
    BacklogOp* op = hold ? hold_back(self) : nullptr;
    if (!op) {
        if (kind == BACKLOG_SIGNAL) {
            gpu_queue_signal_on(self->desktop, fence, value);
            wake_held_waits(self);
        } else {
            gpu_queue_wait_on(self->desktop, fence, value);
        }
        return;
    }
    op->kind = kind;
    fence->AddRef();
    op->fence = fence;
    op->value = value;
    if (self->backlog.count == 1)
        backlog_run(self); // covered meanwhile, or the wait arms
}

void console_queues_report_backlogs(GpuFenceNamer name) {
    AcquireSRWLockShared(&g_slot_queues_lock);
    for (u32 s = 0; s < GPU_MAX_QUEUES; s++) {
        ConsoleCommandQueue* q = g_slot_queues[s];
        if (!q || !TryAcquireSRWLockExclusive(&q->lock))
            continue; // (one busy now isn't stuck; waiting could deadlock with wake_held_waits)
        if (const BacklogOp* op = backlog_front(&q->backlog)) {
            if (op->kind == BACKLOG_WAIT) {
                char fence_name[160];
                name(op->fence, fence_name, sizeof(fence_name));
                LOG_W(
                    "d3d12",
                    "  queue slot %u holds back %u ops; the first waits for %s to reach %llu (it is at %llu)",
                    s, q->backlog.count, fence_name, op->value, op->fence->GetCompletedValue());
            } else if (op->kind == BACKLOG_EXECUTE && op->after_slot != GPU_NO_QUEUE) {
                LOG_W("d3d12",
                      "  queue slot %u holds back %u ops; the first waits for submission %llu of slot %u", s,
                      q->backlog.count, op->after, op->after_slot);
            } else {
                LOG_W("d3d12", "  queue slot %u holds back %u ops", s, q->backlog.count);
            }
        }
        ReleaseSRWLockExclusive(&q->lock);
    }
    ReleaseSRWLockShared(&g_slot_queues_lock);
}

static void queue_destroy(void* object) {
    ConsoleCommandQueue* queue = (ConsoleCommandQueue*)object;
    if (queue->slot != GPU_NO_QUEUE) {
        AcquireSRWLockExclusive(&g_slot_queues_lock);
        g_slot_queues[queue->slot] = nullptr;
        ReleaseSRWLockExclusive(&g_slot_queues_lock);
    }
    AcquireSRWLockExclusive(&queue->lock);
    backlog_drain(queue);
    ReleaseSRWLockExclusive(&queue->lock);
    if (queue->backlog.wait)
        UnregisterWaitEx(queue->backlog.wait, INVALID_HANDLE_VALUE); // waits for a running wakeup
    if (queue->backlog.event)
        CloseHandle(queue->backlog.event);
    backlog_free(&queue->backlog);
    gpu_queue_unregister(queue->slot);
    gpu_list_pool_release(&queue->writers);
    gpu_list_pool_release(&queue->uploaders);
    gpu_ring_release(&queue->scratch);
    gpu_ring_release(&queue->staging);
}

// Texture contents waiting for upload from title memory go first, from a list of the queue's own.
// Queue lock held.
static void upload_textures(ConsoleCommandQueue* self) {
    if (self->type == D3D12_COMMAND_LIST_TYPE_COPY || !texture_sync_pending())
        return;
    u32 entry;
    ID3D12GraphicsCommandList8* list =
        gpu_list_pool_begin(&self->uploaders, gpu_queue_completed(self->slot), &entry);
    if (!list)
        return;
    TextureUploadSources sources;
    self->scratch.slot = self->staging.slot = self->slot;
    self->staging.upload = true;
    bool recorded = texture_sync_record(self->desktop, list, &sources, &self->scratch, &self->staging);
    if (SUCCEEDED(list->Close()) && recorded) {
        ID3D12CommandList* lists[] = {list};
        u64 number = submit(self, 1, lists, GPU_NO_QUEUE, 0, 0); // uploads may read title memory through
                                                                 // the page tables, submitted first
        gpu_list_pool_submitted(&self->uploaders, entry, number);
        gpu_ring_submitted(&self->scratch, number);
        gpu_ring_submitted(&self->staging, number);
    } else if (sources.count) {
        submit(self, 0, nullptr, GPU_NO_QUEUE, 0, 0); // tile mapping changes alone: counted so their
                                                      // textures stay
    }
    for (u32 i = 0; i < sources.count; i++)
        gpu_retire(sources.resources[i]);
}

static HRESULT WINAPI queue_copy_page_mappings_x(ConsoleCommandQueue*, D3D12_GPU_VIRTUAL_ADDRESS destination,
                                                 UINT range_count, const ConsolePageRange* ranges,
                                                 D3D12_GPU_VIRTUAL_ADDRESS pool, UINT pool_pages,
                                                 UINT flags) {
    return console_copy_page_mappings(destination, range_count, ranges, pool, pool_pages, flags);
}

static HRESULT WINAPI queue_clear_page_mappings_x(ConsoleCommandQueue*, D3D12_GPU_VIRTUAL_ADDRESS destination,
                                                  UINT page_count, UINT flags) {
    ConsolePageRange r = {PAGE_RANGE_NULL, 0, page_count};
    return console_copy_page_mappings(destination, 1, &r, 0, 0, flags);
}

static HRESULT WINAPI queue_copy_page_mappings_batch_x(ConsoleCommandQueue*, UINT batch_count,
                                                       const ConsolePageBatch* batches,
                                                       D3D12_GPU_VIRTUAL_ADDRESS pool, UINT pool_pages,
                                                       UINT flags) {
    if (!batches && batch_count)
        return E_INVALIDARG;
    HRESULT result = S_OK;
    for (UINT i = 0; i < batch_count; i++) {
        HRESULT hr = console_copy_page_mappings(batches[i].destination, batches[i].range_count,
                                                batches[i].ranges, pool, pool_pages, flags);
        if (FAILED(hr))
            result = hr;
    }
    return result;
}

// Lists submitted per desktop call; longer submissions are split, which keeps their order.
#define SUBMIT_BATCH BACKLOG_LISTS

// The lists of the next desktop submission, and the run they wait for (after_slot GPU_NO_QUEUE: none).
struct SubmitBatch {
    ID3D12CommandList* lists[SUBMIT_BATCH];
    u32 count;
    u32 after_slot;
    u32 after_generation;
    u64 after;
};

// Submits the batch. Returns the submission's number, or `last` if there was nothing. Queue lock held.
static u64 flush(ConsoleCommandQueue* self, SubmitBatch* b, u64 last) {
    if (!b->count)
        return last;
    u64 number = submit(self, b->count, b->lists, b->after_slot, b->after_generation, b->after);
    b->count = 0;
    b->after_slot = GPU_NO_QUEUE;
    return number;
}

// Each console list runs its parts in order, with the fence signals and waits between them. A console
// list can be submitted again while it still runs; desktop lists can't, so that run is held back until
// the one before finished (the same list twice in one call is the same case).
static void WINAPI queue_execute_command_lists(ConsoleCommandQueue* self, UINT count,
                                               ConsoleCommandList* const* lists) {
    AcquireSRWLockExclusive(&self->lock);
    backlog_run(self); // what can go now goes first
    upload_textures(self);
    SubmitBatch b;
    b.count = 0;
    b.after_slot = GPU_NO_QUEUE;
    u64 last = 0; // the call's last submission
    for (UINT i = 0; i < count; i++) {
        ConsoleCommandList* list = lists[i];
        if (!list)
            continue;
        for (UINT k = 0; k < i; k++) {
            if (lists[k] != list)
                continue;
            last = flush(self, &b, last);
            list->submitted_slot = self->slot;
            list->submitted_generation = self->generation;
            list->submitted_number = last;
            break;
        }
        if (list->submitted_slot != GPU_NO_QUEUE &&
            !gpu_queue_reached(list->submitted_slot, list->submitted_generation, list->submitted_number)) {
            last = flush(self, &b, last); // the lists before don't wait for it
            b.after_slot = list->submitted_slot;
            b.after_generation = list->submitted_generation;
            b.after = list->submitted_number;
        }
        for (u32 t = 0; t < list->table_count; t++) {
            const ConsoleCommandList::TableUse& use = list->tables[t];
            console_descriptor_mirror(use.heap, use.first, use.count, use.desktop_first);
        }
        for (u32 s = 0; s < list->segment_count; s++) {
            const ListSegment& seg = list->segments[s];
            if (b.count == SUBMIT_BATCH)
                last = flush(self, &b, last);
            if (seg.closed) // a part that didn't close would remove the device (logged at Close)
                b.lists[b.count++] = seg.list;
            if (s + 1 == list->segment_count)
                break;
            last = flush(self, &b, last);
            if (seg.sync == LIST_SYNC_SIGNAL) {
                fence_op(self, BACKLOG_SIGNAL, seg.fence, seg.value);
            } else {
                if (seg.counter)
                    console_counter_wait(seg.counter, seg.value);
                fence_op(self, BACKLOG_WAIT, seg.fence, seg.value);
            }
        }
    }
    last = flush(self, &b, last);
    u64 submission = last ? last : submit(self, 0, nullptr, GPU_NO_QUEUE, 0, 0);
    bool held_back = self->backlog.count != 0;
    for (UINT i = 0; i < count; i++) {
        if (lists[i]) {
            lists[i]->submitted_slot = self->slot;
            lists[i]->submitted_generation = self->generation;
            lists[i]->submitted_number = submission;
            lists[i]->held_back |= held_back;
        }
    }
    ReleaseSRWLockExclusive(&self->lock);
    if (gpu_options().serialize)
        gpu_queue_wait(self->slot, submission); // diagnostics: the GPU runs one submission at a time
}

// The frame token and flags tie the work to console frame pacing.
static void WINAPI queue_execute_command_lists_2x(ConsoleCommandQueue* self, UINT count,
                                                  ConsoleCommandList* const* lists, UINT64, UINT) {
    queue_execute_command_lists(self, count, lists);
}

// Title code polls fence values in memory, so a queue signal also writes the value there once the
// queue's earlier work is done, from a small list run just before the desktop signal. Low half
// first: a reader never sees a value ahead of its time. Queue lock held.
static void write_fence_value(ConsoleCommandQueue* self, ConsoleFence* fence, u64 value) {
    if (!fence->value_desktop &&
        !console_translate_coherent((u64)fence->value, sizeof(u64), &fence->value_desktop)) {
        LOG_W_ONCE("d3d12", "fence value memory is not GPU-visible; queue signals won't update it");
        return;
    }
    u32 entry;
    ID3D12GraphicsCommandList8* list =
        gpu_list_pool_begin(&self->writers, gpu_queue_completed(self->slot), &entry);
    if (!list)
        return;
    D3D12_WRITEBUFFERIMMEDIATE_PARAMETER params[2] = {{fence->value_desktop, (u32)value},
                                                      {fence->value_desktop + 4, (u32)(value >> 32)}};
    D3D12_WRITEBUFFERIMMEDIATE_MODE modes[2] = {D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_OUT,
                                                D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_OUT};
    list->WriteBufferImmediate(2, params, modes);
    HRESULT hr = list->Close();
    if (FAILED(hr)) {
        if (!gpu_device_lost(hr))
            LOG_E("d3d12", "cannot write a fence value from the queue (0x%08lx)", (unsigned long)hr);
        return;
    }
    ID3D12CommandList* lists[] = {list};
    gpu_list_pool_submitted(&self->writers, entry, submit(self, 1, lists, GPU_NO_QUEUE, 0, 0));
}

static HRESULT WINAPI queue_signal(ConsoleCommandQueue* self, ConsoleFence* fence, UINT64 value) {
    if (!fence)
        return E_INVALIDARG;
    AcquireSRWLockExclusive(&self->lock);
    backlog_run(self);
    write_fence_value(self, fence, value);
    fence_op(self, BACKLOG_SIGNAL, fence->desktop, value);
    ReleaseSRWLockExclusive(&self->lock);
    return S_OK;
}

// The flush flags pick which console caches to write back first; desktop signals see all
// earlier writes.
static HRESULT WINAPI queue_signal_x(ConsoleCommandQueue* self, ConsoleFence* fence, UINT64 value, UINT) {
    return queue_signal(self, fence, value);
}

static HRESULT WINAPI queue_wait(ConsoleCommandQueue* self, ConsoleFence* fence, UINT64 value) {
    if (!fence)
        return E_INVALIDARG;
    AcquireSRWLockExclusive(&self->lock);
    backlog_run(self);
    fence_op(self, BACKLOG_WAIT, fence->desktop, value);
    ReleaseSRWLockExclusive(&self->lock);
    return S_OK;
}

// The sync parameters pick console caches to flush and pipeline points to wait at around the signal;
// a desktop signal comes after all earlier work and sees all its writes.
static HRESULT WINAPI queue_signal_2x(ConsoleCommandQueue* self, ConsoleFence* fence, UINT64 value,
                                      const void*, const void*) {
    return queue_signal(self, fence, value);
}

// Likewise for the work after a wait: desktop waits hold back all of it.
static HRESULT WINAPI queue_wait_x(ConsoleCommandQueue* self, ConsoleFence* fence, UINT64 value,
                                   const void*) {
    return queue_wait(self, fence, value);
}

// Markers only label work for graphics debuggers: while work is held back they are left out rather
// than held back too. An event's end goes where its begin went, so the desktop queue's events stay
// balanced when the backlog empties or fills between the two.
static void WINAPI queue_set_marker(ConsoleCommandQueue* self, UINT metadata, const void* data, UINT size) {
    AcquireSRWLockExclusive(&self->lock);
    if (!self->backlog.count)
        self->desktop->SetMarker(metadata, data, size);
    ReleaseSRWLockExclusive(&self->lock);
}

static void WINAPI queue_begin_event(ConsoleCommandQueue* self, UINT metadata, const void* data, UINT size) {
    AcquireSRWLockExclusive(&self->lock);
    bool send = !self->backlog.count;
    if (send)
        self->desktop->BeginEvent(metadata, data, size);
    if (self->event_depth < 64) {
        u64 bit = 1ull << self->event_depth;
        self->events_sent = send ? self->events_sent | bit : self->events_sent & ~bit;
    }
    self->event_depth++;
    ReleaseSRWLockExclusive(&self->lock);
}

static void WINAPI queue_end_event(ConsoleCommandQueue* self) {
    AcquireSRWLockExclusive(&self->lock);
    bool send;
    if (!self->event_depth) {
        send = !self->backlog.count; // an end without a begin: the title's own imbalance, passed on
    } else {
        self->event_depth--;
        send = self->event_depth < 64 ? (self->events_sent >> self->event_depth) & 1 : !self->backlog.count;
    }
    if (send)
        self->desktop->EndEvent();
    ReleaseSRWLockExclusive(&self->lock);
}

// --- presentation ------------------------------------------------------------------------------------

// One plane of a console present: the views' resources (one per view; stereo has two), the part of
// each to show and where it goes on the display (normalized).
struct ConsolePresentPlane {
    FrameToken token;
    u32 resource_count;
    ConsoleResource** resources;
    const D3D12_RECT* source_rects; // optional
    const f32 (*dest_rects)[4];     // optional: left, top, right, bottom
    DXGI_COLOR_SPACE_TYPE color_space;
    u32 scale_filter;
    u32 extended_count;
    const void* extended;
    u32 flags;
};
static_assert(sizeof(ConsolePresentPlane) == 72, "console present plane layout");

// Presents per second and the longest interval between two, logged once a second (--frame-stats).
// Called from the presenting thread only.
static void count_frame() {
    static LARGE_INTEGER frequency, last, second;
    static u32 frames;
    static u64 longest;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!frequency.QuadPart) {
        QueryPerformanceFrequency(&frequency);
        second = now;
    } else if ((u64)(now.QuadPart - last.QuadPart) > longest) {
        longest = (u64)(now.QuadPart - last.QuadPart);
    }
    last = now;
    frames++;
    if (now.QuadPart - second.QuadPart >= frequency.QuadPart) {
        LOG_I("present", "%u presents/s, longest interval %.1f ms", frames,
              (double)longest * 1000.0 / (double)frequency.QuadPart);
        frames = 0;
        longest = 0;
        second = now;
    }
}

static HRESULT WINAPI queue_present_x(ConsoleCommandQueue* self, UINT plane_count,
                                      const ConsolePresentPlane* planes, const void* parameters) {
    (void)parameters; // immediate flips, tearing and VRR preferences
    if (gpu_options().frame_stats)
        count_frame();
    if (plane_count > PRESENT_MAX_PLANES || (plane_count && !planes))
        return E_INVALIDARG;
    PresentPlane out[PRESENT_MAX_PLANES] = {};
    for (UINT i = 0; i < plane_count; i++) {
        const ConsolePresentPlane& in = planes[i];
        if (!in.resource_count || !in.resources || !in.resources[0])
            continue;
        if (in.color_space != DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709)
            LOG_W_ONCE("present", "plane color space %d is shown as SDR", in.color_space);
        u64 offset;
        out[i].texture = console_resource_desktop(in.resources[0], &offset, GPU_READ);
        if (in.source_rects)
            out[i].source = in.source_rects[0];
        static const f32 k_full[4] = {0.0f, 0.0f, 1.0f, 1.0f};
        const f32* dest = in.dest_rects ? in.dest_rects[0] : k_full;
        for (u32 c = 0; c < 4; c++)
            out[i].dest[c] = dest[c];
    }
    AcquireSRWLockExclusive(&self->lock);
    backlog_run(self);
    BacklogOp* op = self->backlog.count ? hold_back(self) : nullptr;
    if (op) { // after the held-back work
        op->kind = BACKLOG_PRESENT;
        op->count = plane_count;
        op->token = plane_count ? planes[0].token : 0;
        for (UINT i = 0; i < plane_count; i++) {
            op->planes[i] = out[i];
            if (out[i].texture)
                out[i].texture->AddRef();
        }
    } else {
        u64 done = presenter_present(self->desktop, out, plane_count);
        if (plane_count)
            frame_on_present(planes[0].token, done);
    }
    ReleaseSRWLockExclusive(&self->lock);
    return S_OK;
}

// Work is submitted as it arrives, so there is nothing to kick off.
static void WINAPI queue_kickoff_x(ConsoleCommandQueue*) {}

// Titles suspend their queues while the console suspends them; desktop processes keep running, and the
// queue simply has no work meanwhile.
static HRESULT WINAPI queue_suspend_x(ConsoleCommandQueue*, UINT) {
    return S_OK;
}

static HRESULT WINAPI queue_resume_x(ConsoleCommandQueue*) {
    return S_OK;
}

// Titles managing GPU page mappings themselves flush the GPU's address translation caches; desktop
// tile mappings take effect in queue order without that.
static void WINAPI queue_invalidate_graphics_tlb_x(ConsoleCommandQueue*) {}

// The display hardware's HDR tone mapping table. Presentation shows SDR (see queue_present_x), so the
// table has nothing to apply to.
static HRESULT WINAPI queue_set_hdr_tone_mapper_x(ConsoleCommandQueue*, void* tone_mapper) {
    if (tone_mapper)
        LOG_W_ONCE("present", "SetHDRToneMapperX: HDR output isn't supported; the tone mapper is ignored");
    return S_OK;
}

// Markers for the console's own capture tools; desktop debuggers see the PIX events.
static void WINAPI queue_insert_gpu_marker(ConsoleCommandQueue*, UINT8, const void*, UINT) {}
static void WINAPI queue_insert_timing_marker(ConsoleCommandQueue*, UINT8) {}

// Captures the console's GPU capture tool takes on the title's request: desktop captures are taken with
// desktop tools (RenderDoc, PIX on Windows) instead.
static HRESULT WINAPI queue_pix_capture(ConsoleCommandQueue*, UINT, LPCWSTR) {
    LOG_W_ONCE("d3d12", "GPU captures requested by the title aren't supported; use a desktop capture tool");
    return E_NOTIMPL;
}

static HRESULT WINAPI queue_pix_end_capture(ConsoleCommandQueue*) {
    return E_NOTIMPL;
}

static HRESULT WINAPI queue_enable_live_debugging_x(ConsoleCommandQueue*, void*) {
    return S_OK;
}

// Tile mappings of reserved resources: titles can't create reserved resources here (CreateReservedResource
// fails), so there is nothing to map. Console titles map memory with page mappings (CopyPageMappingsX).
static void WINAPI queue_update_tile_mappings(ConsoleCommandQueue*, void*, UINT, const void*, const void*,
                                              void*, UINT, const void*, const void*, const void*, UINT) {
    LOG_W_ONCE("d3d12", "UpdateTileMappings: reserved resources aren't supported; ignored");
}

static void WINAPI queue_copy_tile_mappings(ConsoleCommandQueue*, void*, const void*, void*, const void*,
                                            const void*, UINT) {
    LOG_W_ONCE("d3d12", "CopyTileMappings: reserved resources aren't supported; ignored");
}

static HRESULT WINAPI queue_get_timestamp_frequency(ConsoleCommandQueue* self, UINT64* frequency) {
    return self->desktop->GetTimestampFrequency(frequency);
}

static HRESULT WINAPI queue_get_clock_calibration(ConsoleCommandQueue* self, UINT64* gpu, UINT64* cpu) {
    return self->desktop->GetClockCalibration(gpu, cpu);
}

static void WINAPI queue_get_desc(ConsoleCommandQueue* self, D3D12_COMMAND_QUEUE_DESC* desc) {
    *desc = self->desktop->GetDesc();
}

static const ConsoleEntry k_queue_impl[] = {
    CONSOLE_ENTRY(8, queue_update_tile_mappings),
    CONSOLE_ENTRY(9, queue_copy_tile_mappings),
    CONSOLE_ENTRY(10, queue_execute_command_lists),
    CONSOLE_ENTRY(14, queue_insert_gpu_marker),
    CONSOLE_ENTRY(15, queue_insert_timing_marker),
    CONSOLE_ENTRY(20, queue_pix_capture),
    CONSOLE_ENTRY(21, queue_pix_capture),
    CONSOLE_ENTRY(22, queue_pix_end_capture),
    CONSOLE_ENTRY(23, queue_suspend_x),
    CONSOLE_ENTRY(24, queue_resume_x),
    CONSOLE_ENTRY(26, queue_invalidate_graphics_tlb_x),
    CONSOLE_ENTRY(33, queue_set_hdr_tone_mapper_x),
    CONSOLE_ENTRY(34, queue_signal_2x),
    CONSOLE_ENTRY(36, queue_enable_live_debugging_x),
    CONSOLE_ENTRY(37, queue_wait_x),
    CONSOLE_ENTRY(11, queue_set_marker),
    CONSOLE_ENTRY(12, queue_begin_event),
    CONSOLE_ENTRY(13, queue_end_event),
    CONSOLE_ENTRY(16, queue_signal),
    CONSOLE_ENTRY(17, queue_wait),
    CONSOLE_ENTRY(25, queue_signal_x),
    CONSOLE_ENTRY(27, queue_kickoff_x),
    CONSOLE_ENTRY(18, queue_get_timestamp_frequency),
    CONSOLE_ENTRY(19, queue_get_clock_calibration),
    CONSOLE_ENTRY(28, queue_get_desc),
    CONSOLE_ENTRY(29, queue_copy_page_mappings_x),
    CONSOLE_ENTRY(30, queue_clear_page_mappings_x),
    CONSOLE_ENTRY(31, queue_copy_page_mappings_batch_x),
    CONSOLE_ENTRY(32, queue_present_x),
    CONSOLE_ENTRY(35, queue_execute_command_lists_2x),
};

void console_command_queue_init() {
    g_queue_table =
        console_build_table("ID3D12CommandQueue", k_command_queue_entries,
                            ARRAY_COUNT(k_command_queue_entries), k_queue_impl, ARRAY_COUNT(k_queue_impl));
}

HRESULT console_create_command_queue(ConsoleDevice* device, const D3D12_COMMAND_QUEUE_DESC* desc, REFIID iid,
                                     void** out) {
    if (!desc)
        return E_INVALIDARG;

    // Console-only queue flags have no desktop meaning.
    D3D12_COMMAND_QUEUE_DESC d = *desc;
    d.Flags &= D3D12_COMMAND_QUEUE_FLAG_DISABLE_GPU_TIMEOUT;
    ID3D12CommandQueue* desktop = nullptr;
    HRESULT hr = device->desktop->CreateCommandQueue(&d, IID_PPV_ARGS(&desktop));
    LOG_I("d3d12", "CreateCommandQueue(type %d, priority %d, flags 0x%x) -> 0x%08lx", desc->Type,
          desc->Priority, desc->Flags, (unsigned long)hr);
    if (FAILED(hr))
        return hr;

    ConsoleCommandQueue* queue = (ConsoleCommandQueue*)console_alloc(sizeof(ConsoleCommandQueue),
                                                                     &k_queue_class, g_queue_table, desktop);
    queue->type = desc->Type;
    queue->desktop = desktop;
    queue->slot = gpu_queue_register(desktop);
    queue->generation = gpu_queue_generation(queue->slot);
    if (queue->slot != GPU_NO_QUEUE) {
        AcquireSRWLockExclusive(&g_slot_queues_lock);
        g_slot_queues[queue->slot] = queue;
        ReleaseSRWLockExclusive(&g_slot_queues_lock);
    }
    queue->writers.type = queue->uploaders.type = desc->Type;
    queue->writers.name = "fence value writes";
    queue->uploaders.name = "texture uploads";
    return console_return(queue, iid, out);
}
