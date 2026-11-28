#include "gpu/gpu.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "common/log.h"
#include "core/title.h"
#include "gpu/shader_programs.h"

static GpuOptions g_options;
static ID3D12Device13* g_device = nullptr;
static IDXGIFactory6* g_factory = nullptr;
static IDXGIAdapter3* g_adapter = nullptr; // for memory budgets
static INIT_ONCE g_init_once = INIT_ONCE_STATIC_INIT;
static bool g_int64_image_atomics = false;

void gpu_set_options(const GpuOptions& options) {
    g_options = options;
}

const GpuOptions& gpu_options() {
    return g_options;
}

// What the translation relies on. Console titles are written against RDNA2-class features:
// shader model 6.6+ (wave intrinsics, 64-bit atomics, dynamic resources), enhanced barriers and
// mesh shaders.
static bool check_features(ID3D12Device* device) {
    D3D12_FEATURE_DATA_SHADER_MODEL sm = {D3D_SHADER_MODEL_6_6};
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm))) ||
        sm.HighestShaderModel < D3D_SHADER_MODEL_6_6) {
        LOG_E("gpu", "adapter lacks shader model 6.6");
        return false;
    }
    D3D12_FEATURE_DATA_D3D12_OPTIONS12 o12 = {};
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS12, &o12, sizeof(o12))) ||
        !o12.EnhancedBarriersSupported) {
        LOG_E("gpu", "adapter lacks enhanced barriers");
        return false;
    }
    D3D12_FEATURE_DATA_D3D12_OPTIONS7 o7 = {};
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &o7, sizeof(o7))) ||
        o7.MeshShaderTier == D3D12_MESH_SHADER_TIER_NOT_SUPPORTED) {
        LOG_E("gpu", "adapter lacks mesh shaders");
        return false;
    }
    // Optional: 64-bit atomics on typed views (console images of 64-bit texels), which translated
    // programs reach through the descriptor heap; programs using them fail to build without both.
    D3D12_FEATURE_DATA_D3D12_OPTIONS9 o9 = {};
    D3D12_FEATURE_DATA_D3D12_OPTIONS11 o11 = {};
    bool typed = SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS9, &o9, sizeof(o9))) &&
                 o9.AtomicInt64OnTypedResourceSupported;
    bool heap = SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS11, &o11, sizeof(o11))) &&
                o11.AtomicInt64OnDescriptorHeapResourceSupported;
    g_int64_image_atomics = typed && heap;
    LOG_I("gpu", "64-bit atomics on typed views: %s, on descriptor heap views: %s", typed ? "yes" : "no",
          heap ? "yes" : "no");
    return true;
}

bool gpu_int64_image_atomics() {
    return g_int64_image_atomics;
}

// The wave sizes the device runs, for shader translation: compute programs run console waves lane for
// lane on desktop waves of 32 lanes (see shader/translate.h), so the device must be able to run those.
static ShaderTarget shader_target(ID3D12Device* device) {
    ShaderTarget target = {};
    D3D12_FEATURE_DATA_D3D12_OPTIONS1 o1 = {};
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &o1, sizeof(o1))) || !o1.WaveOps) {
        LOG_E("gpu", "adapter lacks wave operations: compute shaders cannot be translated");
        return target;
    }
    target.wave_min = (u8)(o1.WaveLaneCountMin < 255 ? o1.WaveLaneCountMin : 255);
    target.wave_max = (u8)(o1.WaveLaneCountMax < 255 ? o1.WaveLaneCountMax : 255);
    if (target.wave_min > 32 || target.wave_max < 32)
        LOG_E("gpu", "adapter runs waves of %u-%u lanes, not 32: compute shaders cannot be translated",
              target.wave_min, target.wave_max);
    else
        LOG_I("gpu", "waves of %u-%u lanes", target.wave_min, target.wave_max);
    return target;
}

static void CALLBACK on_debug_message(D3D12_MESSAGE_CATEGORY, D3D12_MESSAGE_SEVERITY severity,
                                      D3D12_MESSAGE_ID id, LPCSTR description, void*) {
    if (severity <= D3D12_MESSAGE_SEVERITY_ERROR)
        LOG_E("d3d12dbg", "[%d] %s", id, description);
    else if (severity == D3D12_MESSAGE_SEVERITY_WARNING)
        LOG_W("d3d12dbg", "[%d] %s", id, description);
    else
        LOG_D("d3d12dbg", "[%d] %s", id, description);
}

// Debug layer messages go to our log, next to what the title was doing.
static void route_debug_messages(ID3D12Device* device) {
    ID3D12InfoQueue1* queue = nullptr;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&queue)))) {
        LOG_W("gpu", "debug layer messages unavailable (no ID3D12InfoQueue1)");
        return;
    }
    // A deprecation advisory the runtime raises from inside its own calls (DXGI checks features on
    // every present), which would bury everything else.
    //
    // Legacy resource state checks: every resource we create uses enhanced barriers, so they can only
    // concern the buffers the runtime makes behind heaps opened over existing memory (our windows'
    // heaps). Commands that take GPU addresses (vertex and index buffers, immediate writes, root
    // descriptors) are checked against those buffers, with legacy state promotion across all the
    // lists of a submission, while synchronization names our windows: one list writing a counter
    // and another reading vertices elsewhere in the same heap reads as a state conflict.
    D3D12_MESSAGE_ID deny[] = {D3D12_MESSAGE_ID_DEPRECATED_API, D3D12_MESSAGE_ID_INVALID_SUBRESOURCE_STATE};
    D3D12_INFO_QUEUE_FILTER filter = {};
    filter.DenyList.NumIDs = ARRAY_COUNT(deny);
    filter.DenyList.pIDList = deny;
    queue->PushStorageFilter(&filter);
    DWORD cookie = 0;
    if (FAILED(queue->RegisterMessageCallback(on_debug_message, D3D12_MESSAGE_CALLBACK_FLAG_NONE, nullptr,
                                              &cookie)))
        LOG_W("gpu", "cannot register for debug layer messages");
    queue->Release();
}

// --- device removal ---------------------------------------------------------------------------------
//
// A removed device completes every fence with UINT64_MAX. Waiting for that on a fence of our own
// reports the removal once, from wherever it happened, with DRED's account of the GPU's last work
// with the debug layer.

static ID3D12Fence* g_removal_fence = nullptr;
static HANDLE g_removal_event = nullptr;
static HANDLE g_removal_wait = nullptr;

// The marker set right before operation `index` of a list (pipeline names, say), or "".
static const wchar_t* breadcrumb_context(const D3D12_AUTO_BREADCRUMB_NODE1* n, u32 index) {
    for (u32 i = 0; i < n->BreadcrumbContextsCount; i++) {
        if (n->pBreadcrumbContexts[i].BreadcrumbIndex == index && n->pBreadcrumbContexts[i].pContextString)
            return n->pBreadcrumbContexts[i].pContextString;
    }
    return L"";
}

// Unfinished command lists (with the operations around where each stopped) and the GPU page fault.
static void log_dred(ID3D12Device* device) {
    ID3D12DeviceRemovedExtendedData1* dred = nullptr;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dred))))
        return;
    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 crumbs = {};
    if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput1(&crumbs))) {
        for (const D3D12_AUTO_BREADCRUMB_NODE1* n = crumbs.pHeadAutoBreadcrumbNode; n; n = n->pNext) {
            u32 done = n->pLastBreadcrumbValue ? *n->pLastBreadcrumbValue : 0;
            if (done >= n->BreadcrumbCount)
                continue;
            LOG_E("gpu",
                  "  list %p (%ls) on queue %p (%ls) stopped after %u of %u operations:", n->pCommandList,
                  n->pCommandListDebugNameW ? n->pCommandListDebugNameW : L"", n->pCommandQueue,
                  n->pCommandQueueDebugNameW ? n->pCommandQueueDebugNameW : L"", done, n->BreadcrumbCount);
            u32 from = done > 4 ? done - 4 : 0;
            for (u32 i = from; i < n->BreadcrumbCount && i <= done + 2; i++)
                LOG_E("gpu", "    %s operation %u: %d %ls", i == done ? ">" : " ", i,
                      (int)n->pCommandHistory[i], breadcrumb_context(n, i));
        }
    }
    D3D12_DRED_PAGE_FAULT_OUTPUT1 fault = {};
    if (SUCCEEDED(dred->GetPageFaultAllocationOutput1(&fault)) && fault.PageFaultVA) {
        LOG_E("gpu", "  page fault at GPU address 0x%llx", fault.PageFaultVA);
        for (const D3D12_DRED_ALLOCATION_NODE1* a = fault.pHeadExistingAllocationNode; a; a = a->pNext)
            LOG_E("gpu", "    allocation there: type %d %p (%ls)", (int)a->AllocationType, a->pObject,
                  a->ObjectNameW ? a->ObjectNameW : L"");
        for (const D3D12_DRED_ALLOCATION_NODE1* a = fault.pHeadRecentFreedAllocationNode; a; a = a->pNext)
            LOG_E("gpu", "    allocation freed there recently: type %d %p (%ls)", (int)a->AllocationType,
                  a->pObject, a->ObjectNameW ? a->ObjectNameW : L"");
    }
    dred->Release();
}

// Asking for the reason can block until the driver has finished recovering.
static void report_removal() {
    static volatile long reported = 0;
    if (InterlockedExchange(&reported, 1))
        return;
    LOG_E("gpu", "the D3D12 device was removed (reason 0x%08lx)",
          (unsigned long)g_device->GetDeviceRemovedReason());
    if (g_options.removal_reports)
        log_dred(g_device);
}

void gpu_remove_device(const char* why) {
    if (!g_options.removal_reports) {
        LOG_W("gpu", "%s; --gpu-dred would remove the device here to report where the GPU is", why);
        return;
    }
    ID3D12Device5* device = nullptr;
    if (!g_device || FAILED(g_device->QueryInterface(IID_PPV_ARGS(&device)))) {
        LOG_W("gpu", "%s; the device cannot be removed to report it", why);
        return;
    }
    LOG_E("gpu", "%s: removing the device to report where the GPU is", why);
    device->RemoveDevice();
    device->Release();
}

static void CALLBACK on_device_removed(void*, BOOLEAN) {
    report_removal();
}

bool gpu_device_lost(HRESULT hr) {
    if (hr != DXGI_ERROR_DEVICE_REMOVED && hr != DXGI_ERROR_DEVICE_HUNG && hr != DXGI_ERROR_DEVICE_RESET &&
        hr != DXGI_ERROR_DRIVER_INTERNAL_ERROR)
        return false;
    report_removal();
    return true;
}

static void watch_for_removal(ID3D12Device* device) {
    g_removal_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_removal_event ||
        FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_removal_fence))) ||
        FAILED(g_removal_fence->SetEventOnCompletion(UINT64_MAX, g_removal_event)) ||
        !RegisterWaitForSingleObject(&g_removal_wait, g_removal_event, on_device_removed, nullptr, INFINITE,
                                     WT_EXECUTEONLYONCE))
        LOG_W("gpu", "cannot watch for device removal");
}

// --- device ------------------------------------------------------------------------------------------

static BOOL CALLBACK create_device(PINIT_ONCE, void*, void**) {
    if (g_options.debug_layer) {
        ID3D12Debug* debug = nullptr;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
            debug->EnableDebugLayer();
            debug->Release();
            LOG_I("gpu", "D3D12 debug layer enabled");
        } else {
            LOG_W("gpu", "D3D12 debug layer requested but unavailable");
        }
    }
    if (g_options.removal_reports) {
        // Breadcrumbs and page fault reports, for explaining device removals.
        ID3D12DeviceRemovedExtendedDataSettings1* dred = nullptr;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dred)))) {
            dred->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            dred->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            dred->Release();
            LOG_I("gpu", "device removal reports (DRED) enabled");
        } else {
            LOG_W("gpu", "device removal reports (DRED) requested but unavailable");
        }
    }

    UINT factory_flags = g_options.debug_layer ? DXGI_CREATE_FACTORY_DEBUG : 0;
    if (FAILED(CreateDXGIFactory2(factory_flags, IID_PPV_ARGS(&g_factory)))) {
        LOG_E("gpu", "CreateDXGIFactory2 failed");
        return TRUE;
    }

    IDXGIAdapter1* adapter = nullptr;
    if (g_options.warp) {
        // The software rasterizer: slow, but it tells driver problems from ours.
        ID3D12Device13* device = nullptr;
        if (SUCCEEDED(g_factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter))) &&
            SUCCEEDED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_1, IID_PPV_ARGS(&device)))) {
            if (check_features(device)) {
                LOG_I("gpu", "adapter: WARP (software)");
                g_device = device;
                adapter->QueryInterface(IID_PPV_ARGS(&g_adapter));
            } else {
                device->Release();
            }
        }
        if (adapter)
            adapter->Release();
        if (!g_device)
            LOG_W("gpu", "WARP is unavailable or lacks required features; using the GPU");
    }
    for (UINT i = 0;
         !g_device && g_factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                            IID_PPV_ARGS(&adapter)) != DXGI_ERROR_NOT_FOUND;
         i++) {
        DXGI_ADAPTER_DESC1 desc;
        adapter->GetDesc1(&desc);
        ID3D12Device13* device = nullptr;
        if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
            SUCCEEDED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_1, IID_PPV_ARGS(&device)))) {
            if (check_features(device)) {
                LOG_I("gpu", "adapter: %ls (%llu MiB VRAM)", desc.Description,
                      (u64)desc.DedicatedVideoMemory >> 20);
                g_device = device;
                adapter->QueryInterface(IID_PPV_ARGS(&g_adapter));
                adapter->Release();
                break;
            }
            LOG_W("gpu", "skipping adapter %ls: missing required features", desc.Description);
            device->Release();
        }
        adapter->Release();
    }
    if (!g_device) {
        LOG_E("gpu", "no suitable D3D12 adapter found");
        return TRUE;
    }
    if (g_options.debug_layer)
        route_debug_messages(g_device);
    watch_for_removal(g_device);
    // Translated shaders are kept with the title's other data (%LOCALAPPDATA%\sludgemu\<title id>\).
    wchar_t cache_root[MAX_PATH];
    bool cache = title_data_dir(L"shader_cache", cache_root, MAX_PATH);
    if (!cache)
        LOG_W("gpu", "no folder for the shader cache; programs are translated on every run");
    shader_programs_init(shader_target(g_device), cache ? cache_root : nullptr);
    return TRUE;
}

ID3D12Device13* gpu_device() {
    InitOnceExecuteOnce(&g_init_once, create_device, nullptr, nullptr);
    return g_device;
}

IDXGIFactory6* gpu_factory() {
    InitOnceExecuteOnce(&g_init_once, create_device, nullptr, nullptr);
    return g_factory;
}

bool gpu_video_memory(GpuVideoMemory* out) {
    DXGI_QUERY_VIDEO_MEMORY_INFO local, shared;
    if (!g_adapter || FAILED(g_adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local)) ||
        FAILED(g_adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &shared)))
        return false;
    out->local_usage = local.CurrentUsage;
    out->local_budget = local.Budget;
    out->shared_usage = shared.CurrentUsage;
    out->shared_budget = shared.Budget;
    return true;
}

// --- submission and deferred release ----------------------------------------------------------------
//
// Each queue that runs translated work counts its submissions on a fence. A retired object waits
// until every queue has finished what it had submitted when the object was retired.

struct GpuQueueSlot {
    ID3D12CommandQueue* queue; // nullptr: free slot
    ID3D12Fence* fence;
    u64 submitted;  // numbers handed out
    u64 executed;   // the last number signaled on the queue
    u32 generation; // counts the queues that held the slot
    u32 sync_next;  // the next entry of the slot's sync history
};

// A queue's recent fence waits and signals, for stall reports. `before`: the first submission after
// the op. Entries hold a fence reference.
#define SYNC_HISTORY 1024 // power of two
struct SyncOp {
    ID3D12Fence* fence;
    u64 value;
    u64 before;
    bool wait;
};

struct RetiredObject {
    IUnknown* object;
    GpuRetirePoint point;
};

static SRWLOCK g_submit_lock = SRWLOCK_INIT;
static GpuQueueSlot g_queues[GPU_MAX_QUEUES];
static SyncOp g_sync_history[GPU_MAX_QUEUES][SYNC_HISTORY];

// The highest signal still pending on a desktop queue for each fence, for gpu_fence_wait_covered: open
// addressing by fence, entries holding a fence reference. A signal is pending until its queue finished
// the submission after it (`before`); one that ran says no more than the fence itself (which titles may
// also set lower again), so it covers nothing and its entry goes when the table fills. Under
// g_submit_lock.
#define QUEUED_SIGNALS 4096 // power of two
struct QueuedSignal {
    ID3D12Fence* fence;
    u64 value;
    u32 slot;
    u32 generation; // of the slot, when the signal was queued
    u64 before;
};
static QueuedSignal g_queued_signals[QUEUED_SIGNALS];
static u32 g_queued_signal_count;
static RetiredObject* g_retired = nullptr;
static u32 g_retired_count = 0;
static u32 g_retired_capacity = 0;

// Releases what no queue can still be using. Caller holds g_submit_lock.
static void collect_locked() {
    if (!g_retired_count)
        return;
    u64 completed[GPU_MAX_QUEUES];
    for (u32 q = 0; q < GPU_MAX_QUEUES; q++)
        completed[q] = g_queues[q].queue ? g_queues[q].fence->GetCompletedValue() : 0;
    u32 kept = 0;
    for (u32 i = 0; i < g_retired_count; i++) {
        RetiredObject& r = g_retired[i];
        bool done = true;
        for (u32 q = 0; q < GPU_MAX_QUEUES && done; q++)
            done = r.point.after[q] <= completed[q];
        if (done)
            r.object->Release();
        else
            g_retired[kept++] = r;
    }
    g_retired_count = kept;
}

u32 gpu_queue_register(ID3D12CommandQueue* queue) {
    ID3D12Fence* fence = nullptr;
    if (FAILED(gpu_device()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
        LOG_E("gpu", "cannot create a submission fence");
        return GPU_NO_QUEUE;
    }
    AcquireSRWLockExclusive(&g_submit_lock);
    u32 slot = GPU_NO_QUEUE;
    for (u32 q = 0; q < GPU_MAX_QUEUES; q++) {
        if (!g_queues[q].queue) {
            g_queues[q] = {queue, fence, 0, 0, g_queues[q].generation + 1, 0};
            slot = q;
            break;
        }
    }
    ReleaseSRWLockExclusive(&g_submit_lock);
    if (slot == GPU_NO_QUEUE) {
        LOG_E("gpu", "more than %u command queues", GPU_MAX_QUEUES);
        fence->Release();
    }
    return slot;
}

void gpu_queue_unregister(u32 slot) {
    if (slot >= GPU_MAX_QUEUES)
        return;
    AcquireSRWLockExclusive(&g_submit_lock);
    GpuQueueSlot q = g_queues[slot];
    g_queues[slot] = {};
    g_queues[slot].generation = q.generation;
    // Whatever waited on this queue now only waits for it to drain, which happens below.
    for (u32 i = 0; i < g_retired_count; i++)
        g_retired[i].point.after[slot] = 0;
    ReleaseSRWLockExclusive(&g_submit_lock);
    if (q.fence->GetCompletedValue() < q.submitted)
        q.fence->SetEventOnCompletion(q.submitted, nullptr); // blocks until the queue is idle
    q.fence->Release();
    AcquireSRWLockExclusive(&g_submit_lock);
    for (u32 i = 0; i < SYNC_HISTORY; i++) {
        SyncOp& op = g_sync_history[slot][i];
        if (op.fence)
            op.fence->Release();
        op = {};
    }
    ReleaseSRWLockExclusive(&g_submit_lock);
}

u64 gpu_queue_submitted(u32 slot) {
    if (slot >= GPU_MAX_QUEUES)
        return 0;
    AcquireSRWLockExclusive(&g_submit_lock);
    GpuQueueSlot& q = g_queues[slot];
    u64 number = ++q.submitted;
    q.queue->Signal(q.fence, number);
    q.executed = number;
    collect_locked();
    ReleaseSRWLockExclusive(&g_submit_lock);
    return number;
}

u64 gpu_queue_execute(ID3D12CommandQueue* queue, u32 slot, u32 count, ID3D12CommandList* const* lists) {
    if (slot >= GPU_MAX_QUEUES) {
        if (count)
            queue->ExecuteCommandLists(count, lists);
        return 0;
    }
    AcquireSRWLockExclusive(&g_submit_lock);
    GpuQueueSlot& q = g_queues[slot];
    if (count)
        q.queue->ExecuteCommandLists(count, lists);
    u64 number = ++q.submitted;
    q.queue->Signal(q.fence, number);
    q.executed = number;
    collect_locked();
    ReleaseSRWLockExclusive(&g_submit_lock);
    return number;
}

u64 gpu_queue_reserve(u32 slot) {
    if (slot >= GPU_MAX_QUEUES)
        return 0;
    AcquireSRWLockExclusive(&g_submit_lock);
    u64 number = ++g_queues[slot].submitted;
    ReleaseSRWLockExclusive(&g_submit_lock);
    return number;
}

void gpu_queue_execute_reserved(ID3D12CommandQueue* queue, u32 slot, u64 number, u32 count,
                                ID3D12CommandList* const* lists) {
    if (slot >= GPU_MAX_QUEUES) {
        if (count)
            queue->ExecuteCommandLists(count, lists);
        return;
    }
    AcquireSRWLockExclusive(&g_submit_lock);
    GpuQueueSlot& q = g_queues[slot];
    if (count)
        q.queue->ExecuteCommandLists(count, lists);
    q.queue->Signal(q.fence, number);
    q.executed = number;
    collect_locked();
    ReleaseSRWLockExclusive(&g_submit_lock);
}

u64 gpu_queue_completed(u32 slot) {
    if (slot >= GPU_MAX_QUEUES)
        return 0;
    AcquireSRWLockShared(&g_submit_lock);
    u64 completed = g_queues[slot].fence ? g_queues[slot].fence->GetCompletedValue() : 0;
    ReleaseSRWLockShared(&g_submit_lock);
    return completed;
}

u32 gpu_queue_generation(u32 slot) {
    if (slot >= GPU_MAX_QUEUES)
        return 0;
    AcquireSRWLockShared(&g_submit_lock);
    u32 generation = g_queues[slot].generation;
    ReleaseSRWLockShared(&g_submit_lock);
    return generation;
}

// A queue that left its slot drained its work first (gpu_queue_unregister).
bool gpu_queue_reached(u32 slot, u32 generation, u64 number) {
    if (slot >= GPU_MAX_QUEUES)
        return true;
    AcquireSRWLockShared(&g_submit_lock);
    const GpuQueueSlot& q = g_queues[slot];
    bool reached = !q.queue || q.generation != generation || q.fence->GetCompletedValue() >= number;
    ReleaseSRWLockShared(&g_submit_lock);
    return reached;
}

void gpu_queue_wait(u32 slot, u64 number) {
    if (slot >= GPU_MAX_QUEUES)
        return;
    AcquireSRWLockShared(&g_submit_lock);
    ID3D12Fence* fence = g_queues[slot].fence;
    if (fence)
        fence->AddRef();
    ReleaseSRWLockShared(&g_submit_lock);
    if (!fence)
        return;
    if (fence->GetCompletedValue() < number)
        fence->SetEventOnCompletion(number, nullptr);
    fence->Release();
}

ID3D12Fence* gpu_queue_fence(u32 slot) {
    if (slot >= GPU_MAX_QUEUES)
        return nullptr;
    AcquireSRWLockShared(&g_submit_lock);
    ID3D12Fence* fence = g_queues[slot].fence;
    ReleaseSRWLockShared(&g_submit_lock);
    return fence;
}

// Caller holds g_submit_lock (any mode).
static u32 slot_of(ID3D12CommandQueue* queue) {
    for (u32 q = 0; q < GPU_MAX_QUEUES; q++) {
        if (g_queues[q].queue == queue)
            return q;
    }
    return GPU_NO_QUEUE;
}

static u32 queued_signal_home(ID3D12Fence* fence) {
    return (u32)(((u64)(uintptr_t)fence >> 4) * 0x9E3779B97F4A7C15ull >> 52) & (QUEUED_SIGNALS - 1);
}

// The entry of `fence`, or the free one where it would go. g_submit_lock held.
static QueuedSignal* queued_signal_slot(ID3D12Fence* fence) {
    u32 i = queued_signal_home(fence);
    while (g_queued_signals[i].fence && g_queued_signals[i].fence != fence)
        i = (i + 1) & (QUEUED_SIGNALS - 1);
    return &g_queued_signals[i];
}

// Whether the signal may not have run yet. Conservative: it ran once its queue finished the submission
// after it, or once another queue took the slot. g_submit_lock held.
static bool signal_pending(const QueuedSignal& s) {
    const GpuQueueSlot& q = g_queues[s.slot];
    return q.queue && q.generation == s.generation && q.fence->GetCompletedValue() < s.before;
}

// Drops the entries of signals that ran. g_submit_lock held exclusively.
static void sweep_queued_signals() {
    static QueuedSignal kept[QUEUED_SIGNALS];
    u32 count = 0;
    for (u32 i = 0; i < QUEUED_SIGNALS; i++) {
        QueuedSignal& s = g_queued_signals[i];
        if (!s.fence)
            continue;
        if (!signal_pending(s))
            s.fence->Release();
        else
            kept[count++] = s;
        s = {};
    }
    for (u32 i = 0; i < count; i++)
        *queued_signal_slot(kept[i].fence) = kept[i];
    g_queued_signal_count = count;
}

// A signal queued on slot `slot` right before its submission `before`. g_submit_lock held exclusively.
static void note_queued_signal(ID3D12Fence* fence, u64 value, u32 slot, u64 before) {
    QueuedSignal entry = {fence, value, slot, g_queues[slot].generation, before};
    QueuedSignal* s = queued_signal_slot(fence);
    if (s->fence) {
        if (value >= s->value || !signal_pending(*s))
            *s = entry; // (keeps the reference it holds)
        return;
    }
    if (g_queued_signal_count >= QUEUED_SIGNALS * 3 / 4) {
        sweep_queued_signals();
        s = queued_signal_slot(fence);
        if (g_queued_signal_count >= QUEUED_SIGNALS * 3 / 4) {
            LOG_W_ONCE("gpu", "more than %u fences with queued signals; waits on others wait for the CPU",
                       QUEUED_SIGNALS * 3 / 4);
            return;
        }
    }
    fence->AddRef();
    *s = entry;
    g_queued_signal_count++;
}

bool gpu_fence_wait_covered(ID3D12Fence* fence, u64 value) {
    if (fence->GetCompletedValue() >= value)
        return true;
    AcquireSRWLockShared(&g_submit_lock);
    const QueuedSignal* s = queued_signal_slot(fence);
    bool queued = s->fence && s->value >= value && signal_pending(*s);
    ReleaseSRWLockShared(&g_submit_lock);
    return queued;
}

// The op goes to the queue under the lock, so its place among the queue's submissions is the one
// recorded.
static void sync_op(ID3D12CommandQueue* queue, ID3D12Fence* fence, u64 value, bool wait) {
    AcquireSRWLockExclusive(&g_submit_lock);
    u32 slot = slot_of(queue);
    if (wait) {
        queue->Wait(fence, value);
    } else {
        queue->Signal(fence, value);
        if (slot !=
            GPU_NO_QUEUE) // (signals of queues without a slot cover nothing: waits wait for the fence)
            note_queued_signal(fence, value, slot, g_queues[slot].executed + 1);
    }
    ID3D12Fence* old = nullptr;
    if (slot != GPU_NO_QUEUE) {
        GpuQueueSlot& q = g_queues[slot];
        SyncOp& op = g_sync_history[slot][q.sync_next++ & (SYNC_HISTORY - 1)];
        old = op.fence;
        fence->AddRef();
        op = {fence, value, q.executed + 1, wait};
    }
    ReleaseSRWLockExclusive(&g_submit_lock);
    if (old)
        old->Release();
}

void gpu_queue_wait_on(ID3D12CommandQueue* queue, ID3D12Fence* fence, u64 value) {
    sync_op(queue, fence, value, true);
}

void gpu_queue_signal_on(ID3D12CommandQueue* queue, ID3D12Fence* fence, u64 value) {
    sync_op(queue, fence, value, false);
}

bool gpu_queue_progress(u32 slot, GpuQueueProgress* out) {
    if (slot >= GPU_MAX_QUEUES)
        return false;
    AcquireSRWLockShared(&g_submit_lock);
    const GpuQueueSlot& q = g_queues[slot];
    bool used = q.queue != nullptr;
    if (used) {
        out->numbered = q.submitted;
        out->executed = q.executed;
        out->completed = q.fence->GetCompletedValue();
    }
    ReleaseSRWLockShared(&g_submit_lock);
    return used;
}

static const char* queue_type_name(ID3D12CommandQueue* queue) {
    switch (queue->GetDesc().Type) {
    case D3D12_COMMAND_LIST_TYPE_DIRECT:
        return "direct";
    case D3D12_COMMAND_LIST_TYPE_COMPUTE:
        return "compute";
    case D3D12_COMMAND_LIST_TYPE_COPY:
        return "copy";
    default:
        return "other";
    }
}

// What a stall report says about a queue and the unmet waits holding it, gathered under g_submit_lock
// and logged after it: naming a fence may take locks (a title counter's) that are held while
// submitting.
#define STALL_WAITS 8 // per queue
struct StallWait {
    ID3D12Fence* fence; // referenced
    u64 value;
    u64 reached;
    u32 fence_slot;  // the slot whose submissions the fence counts, or GPU_NO_QUEUE
    u32 signal_slot; // the queue holding a signal that reaches `value`, or GPU_NO_QUEUE
    u64 signal_value;
    u64 signal_before;
};
struct StallQueue {
    bool used;
    const char* type;
    u64 numbered, executed, completed;
    u32 wait_count;
    StallWait waits[STALL_WAITS];
};

// Which queued signal reaches `value` on `fence`. g_submit_lock held.
static void find_signal_for(StallWait* w) {
    w->signal_slot = GPU_NO_QUEUE;
    for (u32 s = 0; s < GPU_MAX_QUEUES; s++) {
        if (!g_queues[s].queue)
            continue;
        for (u32 i = 0; i < SYNC_HISTORY; i++) {
            const SyncOp& op = g_sync_history[s][i];
            if (op.fence == w->fence && !op.wait && op.value >= w->value) {
                w->signal_slot = s;
                w->signal_value = op.value;
                w->signal_before = op.before;
                return;
            }
        }
    }
}

// g_submit_lock held.
static void gather_stall(StallQueue* out) {
    for (u32 s = 0; s < GPU_MAX_QUEUES; s++) {
        const GpuQueueSlot& q = g_queues[s];
        StallQueue& r = out[s];
        r.used = q.queue != nullptr;
        if (!r.used)
            continue;
        r.type = queue_type_name(q.queue);
        r.numbered = q.submitted;
        r.executed = q.executed;
        r.completed = q.fence->GetCompletedValue();
        if (r.completed >= q.executed)
            continue;
        // Waits recorded right after submission `completed` hold back the one it is stuck at.
        for (u32 n = 0; n < SYNC_HISTORY && r.wait_count < STALL_WAITS; n++) {
            const SyncOp& op = g_sync_history[s][(q.sync_next + n) & (SYNC_HISTORY - 1)];
            if (!op.fence || !op.wait || op.before != r.completed + 1)
                continue;
            u64 reached = op.fence->GetCompletedValue();
            if (reached >= op.value)
                continue;
            StallWait& w = r.waits[r.wait_count++];
            op.fence->AddRef();
            w.fence = op.fence;
            w.value = op.value;
            w.reached = reached;
            w.fence_slot = GPU_NO_QUEUE;
            for (u32 f = 0; f < GPU_MAX_QUEUES; f++) {
                if (g_queues[f].queue && g_queues[f].fence == op.fence)
                    w.fence_slot = f;
            }
            find_signal_for(&w);
        }
    }
}

void gpu_queue_report_stall(GpuFenceNamer name) {
    static StallQueue queues[GPU_MAX_QUEUES]; // (one report at a time: the stall watch thread's)
    memset(queues, 0, sizeof(queues));
    AcquireSRWLockShared(&g_submit_lock);
    gather_stall(queues);
    ReleaseSRWLockShared(&g_submit_lock);
    for (u32 s = 0; s < GPU_MAX_QUEUES; s++) {
        StallQueue& r = queues[s];
        if (!r.used)
            continue;
        LOG_W("gpu", "  queue slot %u (%s): numbered %llu, executed %llu, completed %llu", s, r.type,
              r.numbered, r.executed, r.completed);
        if (r.completed >= r.executed)
            continue;
        if (!r.wait_count)
            LOG_W("gpu", "    no unmet fence wait before submission %llu in the history: it runs",
                  r.completed + 1);
        for (u32 i = 0; i < r.wait_count; i++) {
            StallWait& w = r.waits[i];
            char fence_name[160];
            if (w.fence_slot != GPU_NO_QUEUE)
                snprintf(fence_name, sizeof(fence_name), "the submissions of slot %u", w.fence_slot);
            else if (name)
                name(w.fence, fence_name, sizeof(fence_name));
            else
                snprintf(fence_name, sizeof(fence_name), "fence %p", (void*)w.fence);
            LOG_W("gpu", "    waits for %s to reach %llu (it is at %llu)", fence_name, w.value, w.reached);
            if (w.signal_slot != GPU_NO_QUEUE)
                LOG_W("gpu", "      a signal to %llu is queued on slot %u before its submission %llu",
                      w.signal_value, w.signal_slot, w.signal_before);
            else
                LOG_W("gpu", "      no queued GPU work signals it: CPU code or work not submitted yet must");
            w.fence->Release();
        }
    }
}

void gpu_set_name(ID3D12Object* object, const char* format, ...) {
    if (!g_options.debug_layer || !object)
        return;
    char name[128];
    va_list args;
    va_start(args, format);
    vsnprintf(name, sizeof(name), format, args);
    va_end(args);
    wchar_t wide[128];
    u32 i = 0;
    for (; name[i] && i + 1 < ARRAY_COUNT(wide); i++)
        wide[i] = (wchar_t)(u8)name[i];
    wide[i] = 0;
    object->SetName(wide);
}

void gpu_retire(IUnknown* object) {
    if (!object)
        return;
    AcquireSRWLockExclusive(&g_submit_lock);
    if (g_retired_count == g_retired_capacity) {
        u32 capacity = g_retired_capacity ? g_retired_capacity * 2 : 256;
        RetiredObject* grown = (RetiredObject*)realloc(g_retired, capacity * sizeof(RetiredObject));
        if (!grown) {
            // Out of memory: wait for the GPU rather than release something it may be using.
            ReleaseSRWLockExclusive(&g_submit_lock);
            LOG_E("gpu", "retire list full; waiting for the GPU to go idle");
            gpu_wait_idle();
            object->Release();
            return;
        }
        g_retired = grown;
        g_retired_capacity = capacity;
    }
    RetiredObject& r = g_retired[g_retired_count++];
    r.object = object;
    for (u32 q = 0; q < GPU_MAX_QUEUES; q++)
        r.point.after[q] = g_queues[q].queue ? g_queues[q].submitted : 0;
    collect_locked();
    ReleaseSRWLockExclusive(&g_submit_lock);
}

void gpu_retire_point(GpuRetirePoint* out) {
    AcquireSRWLockShared(&g_submit_lock);
    for (u32 q = 0; q < GPU_MAX_QUEUES; q++) {
        out->after[q] = g_queues[q].queue ? g_queues[q].submitted : 0;
        out->generation[q] = g_queues[q].generation;
    }
    ReleaseSRWLockShared(&g_submit_lock);
}

// A queue that left its slot since drained its work first (gpu_queue_unregister).
bool gpu_retire_point_reached(const GpuRetirePoint& point) {
    AcquireSRWLockShared(&g_submit_lock);
    bool reached = true;
    for (u32 q = 0; q < GPU_MAX_QUEUES && reached; q++) {
        const GpuQueueSlot& slot = g_queues[q];
        reached = !slot.queue || slot.generation != point.generation[q] ||
                  point.after[q] <= slot.fence->GetCompletedValue();
    }
    ReleaseSRWLockShared(&g_submit_lock);
    return reached;
}

void gpu_wait_idle() {
    // Waits without the lock: submissions held back in a queue backlog reach their queue (taking the
    // lock) while this waits for them.
    ID3D12Fence* fences[GPU_MAX_QUEUES];
    u64 values[GPU_MAX_QUEUES];
    AcquireSRWLockShared(&g_submit_lock);
    for (u32 q = 0; q < GPU_MAX_QUEUES; q++) {
        fences[q] = g_queues[q].queue ? g_queues[q].fence : nullptr;
        values[q] = g_queues[q].submitted;
        if (fences[q])
            fences[q]->AddRef();
    }
    ReleaseSRWLockShared(&g_submit_lock);
    for (u32 q = 0; q < GPU_MAX_QUEUES; q++) {
        if (!fences[q])
            continue;
        if (fences[q]->GetCompletedValue() < values[q])
            fences[q]->SetEventOnCompletion(values[q], nullptr);
        fences[q]->Release();
    }
    AcquireSRWLockExclusive(&g_submit_lock);
    collect_locked();
    ReleaseSRWLockExclusive(&g_submit_lock);
}
