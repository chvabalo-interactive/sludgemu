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
                LOG_E("gpu", "    %s operation %u: %d", i == done ? ">" : " ", i, (int)n->pCommandHistory[i]);
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
    if (g_options.debug_layer)
        log_dred(g_device);
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
    if (g_options.debug_layer) {
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
    u64 submitted;
    u32 generation; // counts the queues that held the slot
};

struct RetiredObject {
    IUnknown* object;
    GpuRetirePoint point;
};

static SRWLOCK g_submit_lock = SRWLOCK_INIT;
static GpuQueueSlot g_queues[GPU_MAX_QUEUES];
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
            g_queues[q] = {queue, fence, 0, g_queues[q].generation + 1};
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
}

u64 gpu_queue_submitted(u32 slot) {
    if (slot >= GPU_MAX_QUEUES)
        return 0;
    AcquireSRWLockExclusive(&g_submit_lock);
    GpuQueueSlot& q = g_queues[slot];
    u64 number = ++q.submitted;
    q.queue->Signal(q.fence, number);
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
    collect_locked();
    ReleaseSRWLockExclusive(&g_submit_lock);
    return number;
}

u64 gpu_queue_completed(u32 slot) {
    if (slot >= GPU_MAX_QUEUES)
        return 0;
    AcquireSRWLockShared(&g_submit_lock);
    u64 completed = g_queues[slot].fence ? g_queues[slot].fence->GetCompletedValue() : 0;
    ReleaseSRWLockShared(&g_submit_lock);
    return completed;
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
    AcquireSRWLockExclusive(&g_submit_lock);
    for (u32 q = 0; q < GPU_MAX_QUEUES; q++) {
        GpuQueueSlot& s = g_queues[q];
        if (s.queue && s.fence->GetCompletedValue() < s.submitted)
            s.fence->SetEventOnCompletion(s.submitted, nullptr);
    }
    collect_locked();
    ReleaseSRWLockExclusive(&g_submit_lock);
}
