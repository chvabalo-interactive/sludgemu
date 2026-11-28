// Console ID3D12CommandQueue.

#include <stddef.h>

#include <string.h>

#include "common/log.h"
#include "gpu/gpu.h"
#include "gpu/presenter.h"
#include "hle/d3d12/frame_pacing.h"
#include "hle/d3d12/d3d12_objects.h"
#include "hle/d3d12/d3d12_tables.h"
#include "hle/d3d12/texture_sync.h"

static_assert(offsetof(ConsoleCommandQueue, type) == 0x10, "command queue layout");

static const GUID* const k_queue_iids[] = {&__uuidof(ID3D12Object), &__uuidof(ID3D12DeviceChild),
                                           &__uuidof(ID3D12Pageable), &__uuidof(ID3D12CommandQueue)};

static void queue_destroy(void* object);

static const ConsoleClass k_queue_class = {"ID3D12CommandQueue", k_queue_iids, ARRAY_COUNT(k_queue_iids),
                                           queue_destroy};

static void* const* g_queue_table = nullptr;

static void queue_destroy(void* object) {
    ConsoleCommandQueue* queue = (ConsoleCommandQueue*)object;
    gpu_queue_unregister(queue->slot);
    for (u32 i = 0; i < QUEUE_SIGNAL_WRITERS; i++) {
        gpu_retire(queue->writers[i].list);
        gpu_retire(queue->writers[i].allocator);
    }
    for (u32 i = 0; i < QUEUE_UPLOADERS; i++) {
        gpu_retire(queue->uploaders[i].list);
        gpu_retire(queue->uploaders[i].allocator);
    }
    gpu_ring_release(&queue->scratch);
    gpu_ring_release(&queue->staging);
}

// Texture contents waiting for upload from title memory go first, from a list of the queue's own.
static void upload_textures(ConsoleCommandQueue* self) {
    if (self->type == D3D12_COMMAND_LIST_TYPE_COPY || !texture_sync_pending())
        return;
    AcquireSRWLockExclusive(&self->writer_lock); // titles may submit to a queue from several threads
    QueueSignalWriter& w = self->uploaders[self->next_uploader];
    HRESULT hr;
    if (!w.list) {
        ID3D12Device13* device = gpu_device();
        hr = device->CreateCommandAllocator(self->type, IID_PPV_ARGS(&w.allocator));
        if (SUCCEEDED(hr))
            hr = device->CreateCommandList(0, self->type, w.allocator, nullptr, IID_PPV_ARGS(&w.list));
        if (SUCCEEDED(hr))
            gpu_set_name(w.list, "texture uploads");
    } else {
        gpu_queue_wait(self->slot, w.submission); // its allocator may still be in use
        hr = w.allocator->Reset();
        if (SUCCEEDED(hr))
            hr = w.list->Reset(w.allocator, nullptr);
    }
    if (FAILED(hr)) {
        ReleaseSRWLockExclusive(&self->writer_lock);
        if (!gpu_device_lost(hr))
            LOG_E("d3d12", "cannot record texture uploads (0x%08lx)", (unsigned long)hr);
        return;
    }
    TextureUploadSources sources;
    self->scratch.slot = self->staging.slot = self->slot;
    self->staging.upload = true;
    bool recorded = texture_sync_record(self->desktop, w.list, &sources, &self->scratch, &self->staging);
    if (SUCCEEDED(w.list->Close()) && recorded) {
        ID3D12CommandList* list = w.list;
        gpu_page_tables_submit(self->desktop); // uploads may read title memory through the page tables
        w.submission = gpu_queue_execute(self->desktop, self->slot, 1, &list);
        gpu_ring_submitted(&self->scratch, w.submission);
        gpu_ring_submitted(&self->staging, w.submission);
        self->next_uploader = (self->next_uploader + 1) % QUEUE_UPLOADERS; // this one is in flight
    } else if (sources.count) {
        gpu_queue_submitted(self->slot); // tile mapping changes alone: counted so their textures stay
    }
    ReleaseSRWLockExclusive(&self->writer_lock);
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
#define SUBMIT_BATCH 64

struct SubmitBatch {
    ID3D12CommandList* lists[SUBMIT_BATCH];
    u32 count;
};

// Submits the batch after the page table work it needs: descriptors mirrored for it may have reached
// pages the page tables map only now. Returns the submission's number, or `last` if there was nothing.
static u64 flush(ConsoleCommandQueue* self, SubmitBatch* b, u64 last) {
    if (!b->count)
        return last;
    gpu_page_tables_submit(self->desktop);
    u64 number = gpu_queue_execute(self->desktop, self->slot, b->count, b->lists);
    b->count = 0;
    return number;
}

// Each console list runs its parts in order, with the fence signals and waits between them. A console
// list can be submitted again while it still runs; desktop lists can't, so that waits for the run
// before (the same list twice in one call ends the submission there).
static void WINAPI queue_execute_command_lists(ConsoleCommandQueue* self, UINT count,
                                               ConsoleCommandList* const* lists) {
    upload_textures(self);
    SubmitBatch b;
    b.count = 0;
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
            list->submitted_number = last;
            break;
        }
        if (list->submitted_slot != GPU_NO_QUEUE &&
            gpu_queue_completed(list->submitted_slot) < list->submitted_number)
            gpu_queue_wait(list->submitted_slot, list->submitted_number);
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
                self->desktop->Signal(seg.fence, seg.value);
            } else {
                if (seg.counter)
                    console_counter_wait(seg.counter, seg.value);
                self->desktop->Wait(seg.fence, seg.value);
            }
        }
    }
    last = flush(self, &b, last);
    u64 submission = last ? last : gpu_queue_submitted(self->slot);
    for (UINT i = 0; i < count; i++) {
        if (lists[i]) {
            lists[i]->submitted_slot = self->slot;
            lists[i]->submitted_number = submission;
        }
    }
    if (gpu_options().serialize)
        gpu_queue_wait(self->slot, submission);
}

// The frame token and flags tie the work to console frame pacing.
static void WINAPI queue_execute_command_lists_2x(ConsoleCommandQueue* self, UINT count,
                                                  ConsoleCommandList* const* lists, UINT64, UINT) {
    queue_execute_command_lists(self, count, lists);
}

// Title code polls fence values in memory, so a queue signal also writes the value there once the
// queue's earlier work is done, from a small list run just before the desktop signal. Low half
// first: a reader never sees a value ahead of its time.
static void write_fence_value(ConsoleCommandQueue* self, ConsoleFence* fence, u64 value) {
    if (!fence->value_desktop &&
        !console_translate_coherent((u64)fence->value, sizeof(u64), &fence->value_desktop)) {
        LOG_W_ONCE("d3d12", "fence value memory is not GPU-visible; queue signals won't update it");
        return;
    }
    AcquireSRWLockExclusive(&self->writer_lock);
    QueueSignalWriter& w = self->writers[self->next_writer];
    self->next_writer = (self->next_writer + 1) % QUEUE_SIGNAL_WRITERS;
    HRESULT hr;
    if (!w.list) {
        ID3D12Device13* device = gpu_device();
        hr = device->CreateCommandAllocator(self->type, IID_PPV_ARGS(&w.allocator));
        if (SUCCEEDED(hr))
            hr = device->CreateCommandList(0, self->type, w.allocator, nullptr, IID_PPV_ARGS(&w.list));
        if (SUCCEEDED(hr))
            gpu_set_name(w.list, "fence value writes");
    } else {
        gpu_queue_wait(self->slot, w.submission); // its allocator may still be in use
        hr = w.allocator->Reset();
        if (SUCCEEDED(hr))
            hr = w.list->Reset(w.allocator, nullptr);
    }
    if (SUCCEEDED(hr)) {
        D3D12_WRITEBUFFERIMMEDIATE_PARAMETER params[2] = {{fence->value_desktop, (u32)value},
                                                          {fence->value_desktop + 4, (u32)(value >> 32)}};
        D3D12_WRITEBUFFERIMMEDIATE_MODE modes[2] = {D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_OUT,
                                                    D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_OUT};
        w.list->WriteBufferImmediate(2, params, modes);
        hr = w.list->Close();
    }
    if (SUCCEEDED(hr)) {
        ID3D12CommandList* list = w.list;
        w.submission = gpu_queue_execute(self->desktop, self->slot, 1, &list);
    } else {
        if (!gpu_device_lost(hr))
            LOG_E("d3d12", "cannot write a fence value from the queue (0x%08lx)", (unsigned long)hr);
    }
    ReleaseSRWLockExclusive(&self->writer_lock);
}

static HRESULT WINAPI queue_signal(ConsoleCommandQueue* self, ConsoleFence* fence, UINT64 value) {
    if (!fence)
        return E_INVALIDARG;
    write_fence_value(self, fence, value);
    return self->desktop->Signal(fence->desktop, value);
}

// The flush flags pick which console caches to write back first; desktop signals see all
// earlier writes.
static HRESULT WINAPI queue_signal_x(ConsoleCommandQueue* self, ConsoleFence* fence, UINT64 value, UINT) {
    return queue_signal(self, fence, value);
}

static HRESULT WINAPI queue_wait(ConsoleCommandQueue* self, ConsoleFence* fence, UINT64 value) {
    return fence ? self->desktop->Wait(fence->desktop, value) : E_INVALIDARG;
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

static void WINAPI queue_set_marker(ConsoleCommandQueue* self, UINT metadata, const void* data, UINT size) {
    self->desktop->SetMarker(metadata, data, size);
}

static void WINAPI queue_begin_event(ConsoleCommandQueue* self, UINT metadata, const void* data, UINT size) {
    self->desktop->BeginEvent(metadata, data, size);
}

static void WINAPI queue_end_event(ConsoleCommandQueue* self) {
    self->desktop->EndEvent();
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
    u64 done = presenter_present(self->desktop, out, plane_count);
    if (plane_count)
        frame_on_present(planes[0].token, done);
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
    return console_return(queue, iid, out);
}
