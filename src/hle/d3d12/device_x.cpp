// Console ID3D12Device entries beyond creating the core objects: the other versions of the creation
// entries, residency, features, footprints, and the console's debugging and hardware tuning entries.
//
// Console memory is always resident and the console GPU belongs to the title, so residency and power
// entries succeed without doing anything. Debugging entries feed console tools (hang dumps, the live
// debugger, the debug layer); they keep what the title sets where it reads it back, and do nothing
// else. Entries the desktop can't do (shared handles, protected sessions, counter sets, sampler
// feedback, meta commands) fail as a driver without the feature does, and say so once.
//
// Ray tracing state objects and acceleration structure entries belong to the ray tracing work and
// stay unimplemented stubs here.

#include "common/log.h"
#include "gpu/formats.h"
#include "gpu/gpu.h"
#include "hle/d3d12/barriers.h"
#include "hle/d3d12/command_args.h"
#include "hle/d3d12/d3d12_objects.h"
#include "hle/d3d12/device.h"
#include "hle/d3d12/views.h"
#include "core/console_profile.h"

// --- creation entries of other device versions --------------------------------------------------

static HRESULT WINAPI device_create_graphics_pipeline_state(ConsoleDevice* self,
                                                            const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc,
                                                            REFIID iid, void** out) {
    return console_create_graphics_pipeline_state(self, desc, iid, out);
}

// The extended descriptions tune console wave launch and shader limits, which don't apply to
// translated programs.
static HRESULT WINAPI device_create_graphics_pipeline_state_x(ConsoleDevice* self,
                                                              const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc,
                                                              UINT, const void*, REFIID iid, void** out) {
    return console_create_graphics_pipeline_state(self, desc, iid, out);
}

static HRESULT WINAPI device_create_compute_pipeline_state(ConsoleDevice* self,
                                                           const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc,
                                                           REFIID iid, void** out) {
    return console_create_compute_pipeline_state(self, desc, iid, out);
}

static HRESULT WINAPI device_create_command_list(ConsoleDevice* self, UINT, D3D12_COMMAND_LIST_TYPE type,
                                                 ConsoleCommandAllocator* allocator, void* initial_state,
                                                 REFIID iid, void** out) {
    ConsoleCommandListDesc desc = {type, 0, 0, 0};
    return console_create_command_list(self, &desc, allocator, initial_state, iid, out);
}

static HRESULT WINAPI device_create_command_list1(ConsoleDevice* self, UINT, D3D12_COMMAND_LIST_TYPE type,
                                                  UINT, REFIID iid, void** out) {
    return console_create_closed_command_list(self, type, iid, out);
}

// The creator ID groups queues for desktop drivers' scheduling.
static HRESULT WINAPI device_create_command_queue1(ConsoleDevice* self, const D3D12_COMMAND_QUEUE_DESC* desc,
                                                   REFIID, REFIID iid, void** out) {
    return console_create_command_queue(self, desc, iid, out);
}

// Console resource descriptions add color compression options after the D3D12 description; those
// are console hardware settings.
static HRESULT WINAPI device_create_committed_resource_x(
    ConsoleDevice* self, const D3D12_HEAP_PROPERTIES* heap, D3D12_HEAP_FLAGS heap_flags,
    const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* clear, REFIID iid,
    void** out) {
    if (!desc)
        return E_INVALIDARG;
    D3D12_RESOURCE_DESC1 d1 = console_desc1_from_desc(*desc);
    return console_create_committed_resource(self, heap, heap_flags, &d1, console_layout_from_state(state),
                                             clear, iid, out);
}

// Protected sessions don't exist for console titles: always null.
static HRESULT WINAPI device_create_committed_resource1(
    ConsoleDevice* self, const D3D12_HEAP_PROPERTIES* heap, D3D12_HEAP_FLAGS heap_flags,
    const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* clear, void*,
    REFIID iid, void** out) {
    return device_create_committed_resource_x(self, heap, heap_flags, desc, state, clear, iid, out);
}

static HRESULT WINAPI device_create_committed_resource2(
    ConsoleDevice* self, const D3D12_HEAP_PROPERTIES* heap, D3D12_HEAP_FLAGS heap_flags,
    const D3D12_RESOURCE_DESC1* desc, D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* clear, void*,
    REFIID iid, void** out) {
    return console_create_committed_resource(self, heap, heap_flags, desc, console_layout_from_state(state),
                                             clear, iid, out);
}

// Component placed resources put a texture's samples and its compression metadata at separate
// addresses. The samples are what desktop textures and texture sync use; the metadata planes (color
// and depth compression, fragment masks) have no desktop meaning. The first address is the samples'.
static HRESULT WINAPI device_create_component_placed_resource_x(ConsoleDevice* self, const u64* addresses,
                                                                const D3D12_RESOURCE_DESC* desc,
                                                                D3D12_RESOURCE_STATES state,
                                                                const D3D12_CLEAR_VALUE* clear, REFIID iid,
                                                                void** out) {
    if (!addresses || !desc)
        return E_INVALIDARG;
    D3D12_RESOURCE_DESC1 d1 = console_desc1_from_desc(*desc);
    return console_create_placed_resource(self, addresses[0], &d1, console_layout_from_state(state), clear,
                                          iid, out);
}

static HRESULT WINAPI device_create_component_placed_resource_x2(ConsoleDevice* self, const u64* addresses,
                                                                 const D3D12_RESOURCE_DESC1* desc,
                                                                 D3D12_BARRIER_LAYOUT layout,
                                                                 const D3D12_CLEAR_VALUE* clear, REFIID iid,
                                                                 void** out) {
    if (!addresses || !desc)
        return E_INVALIDARG;
    return console_create_placed_resource(self, addresses[0], desc, console_desktop_layout(layout), clear,
                                          iid, out);
}

static HRESULT WINAPI device_create_heap(ConsoleDevice* self, const D3D12_HEAP_DESC* desc, REFIID iid,
                                         void** out) {
    return console_create_heap(self, desc, iid, out);
}

static HRESULT WINAPI device_create_heap1(ConsoleDevice* self, const D3D12_HEAP_DESC* desc, void*, REFIID iid,
                                          void** out) {
    return console_create_heap(self, desc, iid, out);
}

static HRESULT WINAPI device_create_placed_resource(ConsoleDevice* self, ConsoleHeap* heap, UINT64 offset,
                                                    const D3D12_RESOURCE_DESC* desc,
                                                    D3D12_RESOURCE_STATES state,
                                                    const D3D12_CLEAR_VALUE* clear, REFIID iid, void** out) {
    if (!desc)
        return E_INVALIDARG;
    D3D12_RESOURCE_DESC1 d1 = console_desc1_from_desc(*desc);
    return console_create_heap_resource(self, heap, offset, &d1, console_layout_from_state(state), clear, iid,
                                        out);
}

static HRESULT WINAPI device_create_placed_resource1(ConsoleDevice* self, ConsoleHeap* heap, UINT64 offset,
                                                     const D3D12_RESOURCE_DESC1* desc,
                                                     D3D12_RESOURCE_STATES state,
                                                     const D3D12_CLEAR_VALUE* clear, REFIID iid, void** out) {
    return console_create_heap_resource(self, heap, offset, desc, console_layout_from_state(state), clear,
                                        iid, out);
}

// Desktop textures needing views in other formats are created typeless, so castable formats don't
// carry over (see device_create_committed_resource3).
static HRESULT WINAPI device_create_placed_resource2(ConsoleDevice* self, ConsoleHeap* heap, UINT64 offset,
                                                     const D3D12_RESOURCE_DESC1* desc,
                                                     D3D12_BARRIER_LAYOUT layout,
                                                     const D3D12_CLEAR_VALUE* clear, UINT32,
                                                     const DXGI_FORMAT*, REFIID iid, void** out) {
    return console_create_heap_resource(self, heap, offset, desc, console_desktop_layout(layout), clear, iid,
                                        out);
}

// Reserved resources get their memory from tile mappings on console as on desktop. Console titles map
// GPU memory with page mappings at their own addresses instead (CopyPageMappingsX), which is what
// sludgemu follows; reserved resources aren't supported.
static HRESULT WINAPI device_create_reserved_resource(ConsoleDevice*, const void*, UINT, const void*, REFIID,
                                                      void** out) {
    if (out)
        *out = nullptr;
    LOG_W_ONCE("d3d12", "CreateReservedResource: reserved resources aren't supported; refused");
    return E_NOTIMPL;
}

static HRESULT WINAPI device_create_reserved_resource1(ConsoleDevice*, const void*, UINT, const void*, void*,
                                                       REFIID, void** out) {
    return device_create_reserved_resource(nullptr, nullptr, 0, nullptr, IID_NULL, out);
}

static HRESULT WINAPI device_create_reserved_resource2(ConsoleDevice*, const void*, UINT, const void*, void*,
                                                       UINT32, const void*, REFIID, void** out) {
    return device_create_reserved_resource(nullptr, nullptr, 0, nullptr, IID_NULL, out);
}

static void WINAPI device_get_resource_tiling(ConsoleDevice*, void*, UINT* tiles,
                                              D3D12_PACKED_MIP_INFO* packed, D3D12_TILE_SHAPE* shape,
                                              UINT* subresource_count, UINT, void*) {
    // No resource here is reserved: no tiles, as for any resource that isn't.
    if (tiles)
        *tiles = 0;
    if (packed)
        *packed = {};
    if (shape)
        *shape = {};
    if (subresource_count)
        *subresource_count = 0;
}

// D3D12_SAMPLER_DESC2: a version-0 description with an integer border color option; translated
// samplers take the float border color.
struct ConsoleSamplerDesc2 {
    D3D12_FILTER filter;
    D3D12_TEXTURE_ADDRESS_MODE address[3];
    FLOAT mip_bias;
    UINT anisotropy;
    D3D12_COMPARISON_FUNC comparison;
    union {
        FLOAT float_border[4];
        UINT uint_border[4];
    };
    FLOAT min_lod, max_lod;
    UINT flags;
};

#define CONSOLE_SAMPLER_FLAG_UINT_BORDER_COLOR 1u

static void WINAPI device_create_sampler2(ConsoleDevice* self, const ConsoleSamplerDesc2* desc,
                                          D3D12_CPU_DESCRIPTOR_HANDLE dest) {
    if (!desc)
        return;
    D3D12_SAMPLER_DESC d = {desc->filter,   desc->address[0], desc->address[1], desc->address[2],
                            desc->mip_bias, desc->anisotropy, desc->comparison, {},
                            desc->min_lod,  desc->max_lod};
    for (u32 i = 0; i < 4; i++)
        d.BorderColor[i] = (desc->flags & CONSOLE_SAMPLER_FLAG_UINT_BORDER_COLOR)
                               ? (FLOAT)desc->uint_border[i]
                               : desc->float_border[i];
    if (desc->flags & CONSOLE_SAMPLER_FLAG_UINT_BORDER_COLOR)
        LOG_W_ONCE("d3d12", "integer sampler border colors are taken as their float values");
    device_create_sampler(self, &d, dest);
}

// Views of a resource's description at another place in memory (the view's location); with the
// resource's own place, an ordinary view of it.
static void WINAPI device_create_placed_srv_x(ConsoleDevice* self, ConsoleResource* resource,
                                              const void* desc, D3D12_CPU_DESCRIPTOR_HANDLE dest) {
    const D3D12_SHADER_RESOURCE_VIEW_DESC* view = (const D3D12_SHADER_RESOURCE_VIEW_DESC*)desc;
    u64 location = desc ? console_view_location_x(desc, false) : 0;
    if (!resource || !desc || location == resource->gpu_address) {
        device_create_shader_resource_view(self, resource, view, dest);
        return;
    }
    D3D12_RESOURCE_DESC d = console_desc0_from_desc1(resource->desc);
    device_create_placed_raw_srv_x(self, &d, (const ConsoleSrvDescX*)desc, dest);
}

static void WINAPI device_create_placed_uav_x(ConsoleDevice* self, ConsoleResource* resource,
                                              const void* desc, D3D12_CPU_DESCRIPTOR_HANDLE dest) {
    u64 location = desc ? console_view_location_x(desc, true) : 0;
    if (!resource || !desc || location == resource->gpu_address) {
        device_create_unordered_access_view(self, resource, nullptr, desc, dest);
        return;
    }
    D3D12_RESOURCE_DESC d = console_desc0_from_desc1(resource->desc);
    device_create_placed_raw_uav_x(self, &d, (const ConsoleUavDescX*)desc, dest);
}

// --- residency, power, priorities ----------------------------------------------------------------

static HRESULT WINAPI device_make_resident(ConsoleDevice*, UINT, void* const*) {
    return S_OK;
}

static HRESULT WINAPI device_evict(ConsoleDevice*, UINT, void* const*) {
    return S_OK;
}

// Resident at once, so the fence is signalled right away.
static HRESULT WINAPI device_enqueue_make_resident(ConsoleDevice*, UINT, UINT, void* const*,
                                                   ConsoleFence* fence, UINT64 value) {
    return fence ? console_fence_signal_cpu(fence, value) : E_INVALIDARG;
}

static HRESULT WINAPI device_set_residency_priority(ConsoleDevice*, UINT, void* const*, const void*) {
    return S_OK;
}

static HRESULT WINAPI device_set_gpu_memory_priority_x(ConsoleDevice*, UINT) {
    return S_OK;
}

// Desktop stable power states are for profiling and remove the device outside developer mode, so the
// request isn't passed on: clocks are the desktop driver's.
static HRESULT WINAPI device_set_stable_power_state(ConsoleDevice*, BOOL) {
    return S_OK;
}

static HRESULT WINAPI device_set_background_processing_mode(ConsoleDevice*, UINT, UINT, HANDLE,
                                                            BOOL* further) {
    if (further)
        *further = FALSE;
    return S_OK;
}

// --- device information --------------------------------------------------------------------------

static HRESULT WINAPI device_get_device_removed_reason(ConsoleDevice* self) {
    return self->desktop->GetDeviceRemovedReason();
}

// An 8-byte struct comes back in a register, like an integer.
static LUID WINAPI device_get_adapter_luid(ConsoleDevice* self) {
    return self->desktop->GetAdapterLuid();
}

// Console hardware versions the console runtime reports.
#define CONSOLE_HARDWARE_SERIES_S 4
#define CONSOLE_HARDWARE_SERIES_X 5

struct ConsoleGpuHardware {
    u64 frequency; // GPU clock, Hz
    u32 version;
};

static void WINAPI device_get_gpu_hardware_configuration_x(ConsoleDevice*, ConsoleGpuHardware* out) {
    if (!out)
        return;
    bool series_x = console_profile().model == CONSOLE_SERIES_X;
    out->frequency = series_x ? 1825000000ull : 1565000000ull;
    out->version = series_x ? CONSOLE_HARDWARE_SERIES_X : CONSOLE_HARDWARE_SERIES_S;
}

// The console runtime's answers where titles size memory by them (one memory pool shared with the
// CPU, cache coherent); format queries in desktop formats; the rest from the desktop device, which
// is the GPU the title's work runs on.
static HRESULT WINAPI device_check_feature_support(ConsoleDevice* self, D3D12_FEATURE feature, void* data,
                                                   UINT size) {
    if (!data)
        return E_INVALIDARG;
    switch (feature) {
    case D3D12_FEATURE_ARCHITECTURE:
    case D3D12_FEATURE_ARCHITECTURE1: {
        u32 expected = feature == D3D12_FEATURE_ARCHITECTURE ? sizeof(D3D12_FEATURE_DATA_ARCHITECTURE)
                                                             : sizeof(D3D12_FEATURE_DATA_ARCHITECTURE1);
        if (size != expected)
            return E_INVALIDARG;
        D3D12_FEATURE_DATA_ARCHITECTURE1* a = (D3D12_FEATURE_DATA_ARCHITECTURE1*)data;
        a->TileBasedRenderer = FALSE;
        a->UMA = TRUE;
        a->CacheCoherentUMA = TRUE;
        if (feature == D3D12_FEATURE_ARCHITECTURE1)
            a->IsolatedMMU = TRUE;
        return S_OK;
    }
    case D3D12_FEATURE_FORMAT_SUPPORT:
    case D3D12_FEATURE_FORMAT_INFO:
    case D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS: {
        // Each starts with the format.
        DXGI_FORMAT* format = (DXGI_FORMAT*)data;
        DXGI_FORMAT console = *format;
        DXGI_FORMAT desktop = format_desktop(console);
        if (desktop == DXGI_FORMAT_UNKNOWN && console != DXGI_FORMAT_UNKNOWN)
            return E_FAIL; // what desktop says of formats it lacks
        *format = desktop;
        HRESULT hr = self->desktop->CheckFeatureSupport(feature, data, size);
        *format = console;
        return hr;
    }
    default:
        return self->desktop->CheckFeatureSupport(feature, data, size);
    }
}

static void WINAPI device_get_copyable_footprints1(ConsoleDevice*, const D3D12_RESOURCE_DESC1* desc,
                                                   UINT first, UINT count, UINT64 base,
                                                   D3D12_PLACED_SUBRESOURCE_FOOTPRINT* layouts, UINT* rows,
                                                   UINT64* row_sizes, UINT64* total) {
    if (!desc || !console_copyable_footprints(*desc, first, count, base, layouts, rows, row_sizes, total))
        LOG_W_ONCE("d3d12", "GetCopyableFootprints: description without footprints (format %d)",
                   desc ? desc->Format : 0);
}

static void WINAPI device_get_copyable_footprints(ConsoleDevice* self, const D3D12_RESOURCE_DESC* desc,
                                                  UINT first, UINT count, UINT64 base,
                                                  D3D12_PLACED_SUBRESOURCE_FOOTPRINT* layouts, UINT* rows,
                                                  UINT64* row_sizes, UINT64* total) {
    if (!desc) {
        if (total)
            *total = UINT64_MAX;
        return;
    }
    D3D12_RESOURCE_DESC1 d1 = console_desc1_from_desc(*desc);
    device_get_copyable_footprints1(self, &d1, first, count, base, layouts, rows, row_sizes, total);
}

// The D3D standard sample patterns (1/16 pixel offsets from the center), which desktop GPUs use too.
static const s8 k_standard_2[2][2] = {{4, 4}, {-4, -4}};
static const s8 k_standard_4[4][2] = {{-2, -6}, {6, -2}, {-6, 2}, {2, 6}};
static const s8 k_standard_8[8][2] = {{1, -3}, {-1, 3}, {5, 1}, {-3, -5}, {-5, 5}, {-7, -1}, {3, 7}, {7, -7}};
static const s8 k_standard_16[16][2] = {{1, 1},  {-1, -3}, {-3, 2}, {4, -1}, {-5, -2}, {2, 5},
                                        {5, 3},  {3, -5},  {-2, 6}, {0, -7}, {-4, -6}, {-6, 4},
                                        {-8, 0}, {7, -4},  {6, 7},  {-7, -8}};

// What the console's default MSAA settings come to: the standard positions for every pixel of the quad,
// samples in order of priority, and the sample count where the parameters keep it.
static HRESULT WINAPI device_get_default_msaa_parameters_x(ConsoleDevice*,
                                                           const DXGI_SAMPLE_DESC* sample_desc,
                                                           ConsoleSampleLocations* locations, u8* priorities,
                                                           u32* parameters) {
    if (!sample_desc)
        return E_INVALIDARG;
    u32 count = sample_desc->Count;
    const s8(*pattern)[2] = count == 2    ? k_standard_2
                            : count == 4  ? k_standard_4
                            : count == 8  ? k_standard_8
                            : count == 16 ? k_standard_16
                                          : nullptr;
    if (count != 1 && !pattern)
        return E_INVALIDARG;
    if (locations) {
        memset(locations, 0, sizeof(*locations));
        for (u32 p = 0; p < 4 && pattern; p++)
            for (u32 s = 0; s < count; s++) {
                locations->pixels[p][s][0] = pattern[s][0];
                locations->pixels[p][s][1] = pattern[s][1];
            }
    }
    if (priorities)
        for (u32 i = 0; i < 16; i++)
            priorities[i] = (u8)(i < count ? i : 0);
    if (parameters) {
        u32 log2 = count >= 16 ? 4 : count >= 8 ? 3 : count >= 4 ? 2 : count >= 2 ? 1 : 0;
        // The first word: sample count, then the samples exposed to pixel shaders (bits 7-9), both log2.
        parameters[0] = log2 | log2 << 7;
        parameters[1] = 0;
    }
    return S_OK;
}

// --- debugging -----------------------------------------------------------------------------------

static u32 g_debug_flags;
static volatile long g_custom_fence_handles;

static UINT64 WINAPI device_set_driver_hint_x(ConsoleDevice*, UINT64, UINT64) {
    return 0; // nothing on desktop reads driver hints; the previous value is reported as off
}

static void WINAPI device_set_debug_flags_x(ConsoleDevice*, UINT flags) {
    g_debug_flags = flags;
}

static UINT WINAPI device_get_debug_flags_x(ConsoleDevice*) {
    return g_debug_flags;
}

// The console debug layer's messages: desktop validation (--gpu-debug) reports to the log instead.
static void WINAPI device_set_debug_callback_x(ConsoleDevice*, void*, void*) {}
static HRESULT WINAPI device_set_debug_error_filter_x(ConsoleDevice*, UINT, UINT) {
    return S_OK;
}

// GPU hang reporting for console dumps: device removals are reported by sludgemu itself (--gpu-debug).
static void WINAPI device_set_hang_callbacks_x(ConsoleDevice*, void*, void*, void*) {}
static void WINAPI device_report_gpu_hang_x(ConsoleDevice*, UINT flags) {
    LOG_W("d3d12", "the title reports a GPU hang (flags 0x%x)", flags);
}
static void WINAPI device_report_gpu_halt_x(ConsoleDevice*, UINT flags) {
    LOG_W("d3d12", "the title reports a GPU halt (flags 0x%x)", flags);
}
static HRESULT WINAPI device_add_blob_to_gpu_hang_dump_x(ConsoleDevice*, UINT64, UINT, UINT64) {
    return S_OK;
}

// Names for fences in console GPU captures.
static UINT WINAPI device_register_custom_fence_location_x(ConsoleDevice*, UINT64, LPCWSTR) {
    return (UINT)InterlockedIncrement(&g_custom_fence_handles);
}
static void WINAPI device_unregister_custom_fence_location_x(ConsoleDevice*, UINT) {}

// Desktop address translation has no TLB for titles to manage.
static void WINAPI device_enable_manual_graphics_tlb_invalidation_x(ConsoleDevice*, BOOL) {}

static HRESULT WINAPI device_enable_live_debugging_x(ConsoleDevice*, void*) {
    return S_OK;
}
static BOOL WINAPI device_is_live_debug_server_active_x(ConsoleDevice*) {
    return FALSE;
}

// Shader recompilation and symbol paths for console shader debugging: programs are translated here.
static void WINAPI device_set_shader_recompile_callback_x(ConsoleDevice*, void*, void*) {}
static HRESULT WINAPI device_set_compile_time_shader_pdb_path_x(ConsoleDevice*, LPCWSTR) {
    return S_OK;
}

// A title asking for its own device removal (a test of its recovery): not passed on, since desktop
// removal can't be undone for the process.
static void WINAPI device_remove_device(ConsoleDevice*) {
    LOG_W("d3d12", "RemoveDevice: the title asks to remove the device; ignored");
}

// --- no desktop equivalent -----------------------------------------------------------------------

static HRESULT refuse(void** out, const char* what) {
    if (out)
        *out = nullptr;
    LOG_W("d3d12", "%s isn't supported; refused", what);
    return DXGI_ERROR_UNSUPPORTED;
}

static HRESULT WINAPI device_create_shared_handle(ConsoleDevice*, void*, const void*, DWORD, LPCWSTR,
                                                  HANDLE* out) {
    if (out)
        *out = nullptr;
    LOG_W_ONCE("d3d12", "CreateSharedHandle: sharing with other processes isn't supported; refused");
    return DXGI_ERROR_UNSUPPORTED;
}
static HRESULT WINAPI device_open_shared_handle(ConsoleDevice*, HANDLE, REFIID, void** out) {
    return refuse(out, "OpenSharedHandle");
}
static HRESULT WINAPI device_open_shared_handle_by_name(ConsoleDevice*, LPCWSTR, DWORD, HANDLE* out) {
    if (out)
        *out = nullptr;
    LOG_W_ONCE("d3d12", "OpenSharedHandleByName: sharing with other processes isn't supported; refused");
    return DXGI_ERROR_UNSUPPORTED;
}
static HRESULT WINAPI device_open_existing_heap_from_address(ConsoleDevice*, const void*, REFIID,
                                                             void** out) {
    return refuse(out, "OpenExistingHeapFromAddress");
}
static HRESULT WINAPI device_open_existing_heap_from_file_mapping(ConsoleDevice*, HANDLE, REFIID,
                                                                  void** out) {
    return refuse(out, "OpenExistingHeapFromFileMapping");
}
static HRESULT WINAPI device_create_protected_resource_session(ConsoleDevice*, const void*, REFIID,
                                                               void** out) {
    return refuse(out, "CreateProtectedResourceSession");
}
static HRESULT WINAPI device_create_counter_set_x(ConsoleDevice*, const void*, REFIID, void** out) {
    return refuse(out, "CreateCounterSetX (hardware performance counters)");
}
// Pipeline libraries are an optional cache; DXGI_ERROR_UNSUPPORTED is how drivers decline them.
static HRESULT WINAPI device_create_pipeline_library(ConsoleDevice*, const void*, SIZE_T, REFIID,
                                                     void** out) {
    if (out)
        *out = nullptr;
    LOG_W_ONCE("d3d12", "CreatePipelineLibrary: pipelines are cached by sludgemu's shader cache; refused");
    return DXGI_ERROR_UNSUPPORTED;
}
static HRESULT WINAPI device_create_lifetime_tracker(ConsoleDevice*, void*, REFIID, void** out) {
    return refuse(out, "CreateLifetimeTracker");
}
static HRESULT WINAPI device_create_shader_cache_session(ConsoleDevice*, const void*, REFIID, void** out) {
    return refuse(out, "CreateShaderCacheSession");
}
static HRESULT WINAPI device_shader_cache_control(ConsoleDevice*, UINT, UINT) {
    LOG_W_ONCE("d3d12", "ShaderCacheControl isn't supported; refused");
    return DXGI_ERROR_UNSUPPORTED;
}
static HRESULT WINAPI device_serialize_pipeline_state_x(ConsoleDevice*, void*, UINT, void*) {
    LOG_W_ONCE("d3d12",
               "Serialize*PipelineStateX: translated pipelines have no console packets to serialize");
    return E_NOTIMPL;
}

// No meta commands: an empty list.
static HRESULT WINAPI device_enumerate_meta_commands(ConsoleDevice*, UINT* count, void*) {
    if (!count)
        return E_INVALIDARG;
    *count = 0;
    return S_OK;
}
static HRESULT WINAPI device_enumerate_meta_command_parameters(ConsoleDevice*, REFGUID, UINT, UINT*,
                                                               UINT* count, void*) {
    if (count)
        *count = 0;
    return E_INVALIDARG; // no command has that ID
}
static HRESULT WINAPI device_create_meta_command(ConsoleDevice*, REFGUID, UINT, const void*, SIZE_T, REFIID,
                                                 void** out) {
    return refuse(out, "CreateMetaCommand");
}

// Views over sampler feedback and minimum mip maps (partially resident texture tooling): the views
// stay as they were.
static void WINAPI device_create_sampler_feedback_uav_x(ConsoleDevice*, void*, void*, const void*,
                                                        D3D12_CPU_DESCRIPTOR_HANDLE) {
    LOG_W_ONCE("d3d12",
               "CreateSamplerFeedbackUnorderedAccessViewX: sampler feedback isn't supported; no view");
}
static void WINAPI device_create_min_mip_srv_x(ConsoleDevice*, void*, const void*, void*, const void*,
                                               D3D12_CPU_DESCRIPTOR_HANDLE) {
    LOG_W_ONCE("d3d12", "CreateMinMipShaderResourceViewX: minimum mip maps aren't supported; no view");
}
static UINT WINAPI device_get_srv_prt_lod_index_x(ConsoleDevice*, D3D12_CPU_DESCRIPTOR_HANDLE) {
    return 0;
}
static void WINAPI device_set_srv_prt_lod_index_x(ConsoleDevice*, D3D12_CPU_DESCRIPTOR_HANDLE, UINT) {
    LOG_W_ONCE("d3d12",
               "SetShaderResourceViewPRTLODIndexX: mip statistics indices aren't supported; ignored");
}

// An entry the console runtime reserves; nothing documents what it takes.
static HRESULT WINAPI device_xbox_reserved1(ConsoleDevice*, UINT64 argument) {
    LOG_W_ONCE("d3d12", "XboxReserved1(0x%llx): reserved entry; refused", (unsigned long long)argument);
    return E_NOTIMPL;
}

// Several fences at once: the desktop fences behind them.
static HRESULT WINAPI device_set_event_on_multiple_fence_completion(ConsoleDevice* self,
                                                                    ConsoleFence* const* fences,
                                                                    const UINT64* values, UINT count,
                                                                    D3D12_MULTIPLE_FENCE_WAIT_FLAGS flags,
                                                                    HANDLE event) {
    ID3D12Fence* desktop[64];
    if (!fences || !values || count > ARRAY_COUNT(desktop))
        return E_INVALIDARG;
    for (UINT i = 0; i < count; i++) {
        if (!fences[i])
            return E_INVALIDARG;
        desktop[i] = fences[i]->desktop;
    }
    return self->desktop->SetEventOnMultipleFenceCompletion(desktop, values, count, flags, event);
}

const ConsoleEntry k_device_x_entries[] = {
    CONSOLE_ENTRY(10, device_create_graphics_pipeline_state),
    CONSOLE_ENTRY(11, device_create_compute_pipeline_state),
    CONSOLE_ENTRY(12, device_create_command_list),
    CONSOLE_ENTRY(13, device_check_feature_support),
    CONSOLE_ENTRY(28, device_create_heap),
    CONSOLE_ENTRY(29, device_create_placed_resource),
    CONSOLE_ENTRY(30, device_create_reserved_resource),
    CONSOLE_ENTRY(31, device_create_shared_handle),
    CONSOLE_ENTRY(32, device_open_shared_handle),
    CONSOLE_ENTRY(33, device_open_shared_handle_by_name),
    CONSOLE_ENTRY(34, device_make_resident),
    CONSOLE_ENTRY(35, device_evict),
    CONSOLE_ENTRY(37, device_get_device_removed_reason),
    CONSOLE_ENTRY(38, device_get_copyable_footprints),
    CONSOLE_ENTRY(40, device_set_stable_power_state),
    CONSOLE_ENTRY(42, device_get_resource_tiling),
    CONSOLE_ENTRY(43, device_get_adapter_luid),
    CONSOLE_ENTRY(45, device_create_component_placed_resource_x),
    CONSOLE_ENTRY(46, device_create_placed_srv_x),
    CONSOLE_ENTRY(47, device_create_placed_uav_x),
    CONSOLE_ENTRY(49, device_set_driver_hint_x),
    CONSOLE_ENTRY(50, device_set_hang_callbacks_x),
    CONSOLE_ENTRY(51, device_report_gpu_hang_x),
    CONSOLE_ENTRY(52, device_set_debug_flags_x),
    CONSOLE_ENTRY(53, device_get_debug_flags_x),
    CONSOLE_ENTRY(54, device_set_debug_callback_x),
    CONSOLE_ENTRY(56, device_serialize_pipeline_state_x),
    CONSOLE_ENTRY(58, device_enable_manual_graphics_tlb_invalidation_x),
    CONSOLE_ENTRY(59, device_get_default_msaa_parameters_x),
    CONSOLE_ENTRY(60, device_create_graphics_pipeline_state_x),
    CONSOLE_ENTRY(64, device_register_custom_fence_location_x),
    CONSOLE_ENTRY(65, device_unregister_custom_fence_location_x),
    CONSOLE_ENTRY(66, device_serialize_pipeline_state_x),
    CONSOLE_ENTRY(68, device_get_gpu_hardware_configuration_x),
    CONSOLE_ENTRY(69, device_set_debug_error_filter_x),
    CONSOLE_ENTRY(72, device_set_gpu_memory_priority_x),
    CONSOLE_ENTRY(76, device_create_committed_resource_x),
    CONSOLE_ENTRY(77, device_create_component_placed_resource_x),
    CONSOLE_ENTRY(82, device_create_counter_set_x),
    CONSOLE_ENTRY(83, device_create_pipeline_library),
    CONSOLE_ENTRY(84, device_set_event_on_multiple_fence_completion),
    CONSOLE_ENTRY(85, device_set_residency_priority),
    CONSOLE_ENTRY(87, device_open_existing_heap_from_address),
    CONSOLE_ENTRY(88, device_open_existing_heap_from_file_mapping),
    CONSOLE_ENTRY(89, device_enqueue_make_resident),
    CONSOLE_ENTRY(90, device_create_command_list1),
    CONSOLE_ENTRY(91, device_create_protected_resource_session),
    CONSOLE_ENTRY(92, device_create_committed_resource1),
    CONSOLE_ENTRY(93, device_create_heap1),
    CONSOLE_ENTRY(94, device_create_reserved_resource1),
    CONSOLE_ENTRY(96, device_create_lifetime_tracker),
    CONSOLE_ENTRY(97, device_remove_device),
    CONSOLE_ENTRY(98, device_enumerate_meta_commands),
    CONSOLE_ENTRY(99, device_enumerate_meta_command_parameters),
    CONSOLE_ENTRY(100, device_create_meta_command),
    CONSOLE_ENTRY(105, device_set_background_processing_mode),
    CONSOLE_ENTRY(107, device_create_protected_resource_session),
    CONSOLE_ENTRY(109, device_create_committed_resource2),
    CONSOLE_ENTRY(110, device_create_placed_resource1),
    CONSOLE_ENTRY(111, device_create_sampler_feedback_uav_x),
    CONSOLE_ENTRY(112, device_get_copyable_footprints1),
    CONSOLE_ENTRY(113, device_create_min_mip_srv_x),
    CONSOLE_ENTRY(117, device_get_srv_prt_lod_index_x),
    CONSOLE_ENTRY(118, device_set_srv_prt_lod_index_x),
    CONSOLE_ENTRY(119, device_add_blob_to_gpu_hang_dump_x),
    CONSOLE_ENTRY(121, device_set_shader_recompile_callback_x),
    CONSOLE_ENTRY(122, device_set_compile_time_shader_pdb_path_x),
    CONSOLE_ENTRY(123, device_xbox_reserved1),
    CONSOLE_ENTRY(124, device_create_command_queue1),
    CONSOLE_ENTRY(125, device_create_shader_cache_session),
    CONSOLE_ENTRY(126, device_shader_cache_control),
    CONSOLE_ENTRY(128, device_create_placed_resource2),
    CONSOLE_ENTRY(129, device_create_reserved_resource2),
    CONSOLE_ENTRY(130, device_create_sampler2),
    CONSOLE_ENTRY(131, device_enable_live_debugging_x),
    CONSOLE_ENTRY(132, device_report_gpu_halt_x),
    CONSOLE_ENTRY(134, device_create_component_placed_resource_x2),
    CONSOLE_ENTRY(135, device_is_live_debug_server_active_x),
};

const u32 k_device_x_entry_count = ARRAY_COUNT(k_device_x_entries);
