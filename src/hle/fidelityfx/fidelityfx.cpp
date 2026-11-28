// amd_fidelityfx_loader_xbox.dll: AMD FidelityFX upscaler HLE.
//
// Translates console FidelityFX calls to the host PC amd_fidelityfx_upscaler_dx12.dll when present
// (next to sludgemu.exe or in SLUDGE_FIDELITYFX_DIR), substituting the desktop ID3D12Device,
// desktop ID3D12GraphicsCommandList, desktop ID3D12Resource textures, and desktop resource states.
// If no host upscaler DLL is installed, queries return valid FSR parameters so the title does not
// divide by zero on uninitialized query outputs.

#include <windows.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/log.h"
#include "common/types.h"
#include "gpu/gpu.h"
#include "hle/d3d12/command_list.h"
#include "hle/d3d12/d3d12_objects.h"
#include "hle/d3d12/texture_sync.h"
#include "hle/hle.h"

#define FFX_OK                   0u
#define FFX_ERROR_UNKNOWN_TYPE   2u
#define FFX_NO_PROVIDER          4u
#define FFX_ERROR_PARAMETER      6u

#define FFX_DESC_GET_VERSIONS              4ull
#define FFX_DESC_OVERRIDE_VERSION          5ull
#define FFX_DESC_GET_PROVIDER_VERSION      6ull
#define FFX_DESC_HOST_BACKEND              2ull
#define FFX_DESC_CONSOLE_BACKEND           0x1000002ull
#define FFX_DESC_CREATE_UPSCALE            0x10000ull
#define FFX_DESC_DISPATCH_UPSCALE          0x10001ull
#define FFX_DESC_GET_UPSCALE_RATIO         0x10002ull
#define FFX_DESC_GET_RENDER_RESOLUTION     0x10003ull
#define FFX_DESC_GET_JITTER_PHASE_COUNT    0x10004ull
#define FFX_DESC_GET_JITTER_OFFSET         0x10005ull
#define FFX_DESC_MEMORY_USAGE_WITH_DEVICE  0x10009ull

#define FFX_STATE_COMMON       0x1u
#define FFX_STATE_UAV          0x2u
#define FFX_STATE_COMPUTE_READ 0x4u
#define FFX_STATE_PIXEL_READ   0x8u
#define FFX_STATE_COPY_SRC     0x10u
#define FFX_STATE_COPY_DEST    0x20u
#define FFX_STATE_PRESENT      0x80u
#define FFX_STATE_RENDER_TARGET 0x100u
#define FFX_STATE_DEPTH_WRITE  0x200u

struct FfxHeader {
    u64 type;
    FfxHeader* next;
};

struct FfxCreateUpscale {
    FfxHeader header;
    u32 flags;
    u32 max_render[2];
    u32 max_output[2];
    u64 message_cb;
};

struct FfxBackend {
    FfxHeader header;
    void* device;
};

struct FfxOverrideVersion {
    FfxHeader header;
    u64 id;
};

struct FfxProviderVersion {
    FfxHeader header;
    u64 id;
    const char* name;
};

struct FfxGetVersions {
    FfxHeader header;
    u64 create_type;
    void* device;
    u64* count;
    u64* ids;
    const char** names;
};

struct FfxMemoryUsageWithDevice {
    FfxHeader header;
    void* device;
    u32 max_render[2];
    u32 max_output[2];
    u32 flags;
    void* usage;
};

struct FfxResource {
    void* resource;
    u32 description[8];
    u32 state;
};

struct FfxDispatchUpscale {
    FfxHeader header;
    void* command_list;
    FfxResource resources[7]; // color, depth, motion, exposure, reactive, transparency, output
    f32 jitter[2];
    f32 motion_scale[2];
    u32 render_size[2];
    u32 upscale_size[2];
    u32 settings[10];
};

struct FfxQueryUpscaleRatio {
    FfxHeader header;
    u32 quality_mode;
    f32* out_ratio;
};

struct FfxQueryRenderResolution {
    FfxHeader header;
    u32 display_width;
    u32 display_height;
    u32 quality_mode;
    u32* out_render_width;
    u32* out_render_height;
};

struct FfxQueryJitterPhaseCount {
    FfxHeader header;
    s32 render_width;
    s32 display_width;
    s32* out_phase_count;
};

struct FfxQueryJitterOffset {
    FfxHeader header;
    s32 index;
    s32 phase_count;
    f32* out_x;
    f32* out_y;
};

static_assert(sizeof(FfxCreateUpscale) == 0x30, "FfxCreateUpscale layout");
static_assert(sizeof(FfxResource) == 0x30, "FfxResource layout");
static_assert(sizeof(FfxDispatchUpscale) == 0x1B0, "FfxDispatchUpscale layout");

typedef u32 (*PfnFfxCreateContext)(void** context, FfxHeader* desc, const void* mem_cb);
typedef u32 (*PfnFfxDestroyContext)(void** context, const void* mem_cb);
typedef u32 (*PfnFfxCall)(void** context, FfxHeader* desc);

struct FfxHost {
    HMODULE dll;
    PfnFfxCreateContext create;
    PfnFfxDestroyContext destroy;
    PfnFfxCall configure;
    PfnFfxCall query;
    PfnFfxCall dispatch;
    ID3D12Device13* device;
    u64 provider_id;
    char provider_name[64];
    bool ready;
};

// FidelityFX keeps pointers in the create descriptor chain until destroy.
struct FfxContextEntry {
    void* handle;
    FfxCreateUpscale create;
    FfxBackend backend;
    FfxOverrideVersion version;
    FfxContextEntry* next;
};

static INIT_ONCE g_init_once = INIT_ONCE_STATIC_INIT;
static FfxHost g_host = {};
static SRWLOCK g_ctx_lock = SRWLOCK_INIT;
static FfxContextEntry* g_contexts = nullptr;

static bool parse_fsr31_patch(const char* name, u32* patch) {
    if (!name || strncmp(name, "3.1.", 4) != 0)
        return false;
    const char* p = name + 4;
    if (*p < '0' || *p > '9')
        return false;
    *patch = (u32)strtoul(p, nullptr, 10);
    return true;
}

static bool is_fsr412(const char* name) {
    return name && strncmp(name, "4.1.2", 5) == 0 && (name[5] < '0' || name[5] > '9');
}

static bool select_provider(PfnFfxCall query_fn, ID3D12Device13* device, u64* out_id, char* out_name,
                            usize out_cap) {
    u64 count = 0;
    FfxGetVersions q = {};
    q.header.type = FFX_DESC_GET_VERSIONS;
    q.create_type = FFX_DESC_CREATE_UPSCALE;
    q.device = device;
    q.count = &count;
    u32 code = query_fn(nullptr, &q.header);
    if (code != FFX_OK || count == 0) {
        LOG_W("ffx", "host upscaler reports no providers for this GPU (code %u, count %llu)", code, count);
        return false;
    }
    if (count > 64)
        count = 64;
    u64 ids[64] = {};
    const char* names[64] = {};
    q.ids = ids;
    q.names = names;
    code = query_fn(nullptr, &q.header);
    if (code != FFX_OK) {
        LOG_W("ffx", "host upscaler failed version query (code %u)", code);
        return false;
    }

    u32 chosen = 0;
    bool found = false;
    for (u32 i = 0; i < (u32)count; i++) {
        if (is_fsr412(names[i])) {
            chosen = i;
            found = true;
            break;
        }
    }
    if (!found) {
        u32 best_patch = 0;
        for (u32 i = 0; i < (u32)count; i++) {
            u32 patch = 0;
            if (parse_fsr31_patch(names[i], &patch) && (!found || patch >= best_patch)) {
                best_patch = patch;
                chosen = i;
                found = true;
            }
        }
    }
    if (!found)
        chosen = 0;

    *out_id = ids[chosen];
    snprintf(out_name, out_cap, "%s", names[chosen] ? names[chosen] : "unknown");
    return true;
}

static HMODULE load_upscaler_dll() {
    wchar_t path[MAX_PATH] = {};
    DWORD env_len = GetEnvironmentVariableW(L"SLUDGE_FIDELITYFX_DIR", path, MAX_PATH);
    if (env_len > 0 && env_len < MAX_PATH - 40) {
        wcscat_s(path, L"\\amd_fidelityfx_upscaler_dx12.dll");
        HMODULE m = LoadLibraryExW(path, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (m)
            return m;
    }
    DWORD exe_len = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (exe_len > 0 && exe_len < MAX_PATH) {
        wchar_t* slash = wcsrchr(path, L'\\');
        if (slash && (usize)(slash - path + 38) < MAX_PATH) {
            wcscpy_s(slash + 1, (usize)MAX_PATH - (usize)(slash - path + 1), L"amd_fidelityfx_upscaler_dx12.dll");
            HMODULE m = LoadLibraryExW(path, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
            if (m)
                return m;
        }
    }
    return LoadLibraryW(L"amd_fidelityfx_upscaler_dx12.dll");
}

static BOOL CALLBACK init_host_once(PINIT_ONCE, PVOID, PVOID*) {
    ID3D12Device13* device = gpu_device();
    if (!device)
        return TRUE;
    HMODULE dll = load_upscaler_dll();
    if (!dll) {
        LOG_W("ffx", "amd_fidelityfx_upscaler_dx12.dll not found; FSR dispatch disabled (queries emulated)");
        return TRUE;
    }
    g_host.dll = dll;
    g_host.create = (PfnFfxCreateContext)GetProcAddress(dll, "ffxCreateContext");
    g_host.destroy = (PfnFfxDestroyContext)GetProcAddress(dll, "ffxDestroyContext");
    g_host.configure = (PfnFfxCall)GetProcAddress(dll, "ffxConfigure");
    g_host.query = (PfnFfxCall)GetProcAddress(dll, "ffxQuery");
    g_host.dispatch = (PfnFfxCall)GetProcAddress(dll, "ffxDispatch");
    g_host.device = device;
    if (!g_host.create || !g_host.destroy || !g_host.configure || !g_host.query || !g_host.dispatch) {
        LOG_W("ffx", "amd_fidelityfx_upscaler_dx12.dll is missing expected exports");
        FreeLibrary(dll);
        g_host.dll = nullptr;
        return TRUE;
    }
    if (!select_provider(g_host.query, device, &g_host.provider_id, g_host.provider_name,
                         sizeof(g_host.provider_name))) {
        FreeLibrary(dll);
        g_host.dll = nullptr;
        return TRUE;
    }
    g_host.ready = true;
    LOG_I("ffx", "using host FidelityFX upscaler provider %s (0x%llx)", g_host.provider_name, g_host.provider_id);
    return TRUE;
}

static const FfxHost* ffx_host() {
    InitOnceExecuteOnce(&g_init_once, init_host_once, nullptr, nullptr);
    return g_host.ready ? &g_host : nullptr;
}

static bool map_host_state(u32 ffx_state, u32* d3d_state) {
    switch (ffx_state) {
    case FFX_STATE_COMMON:
    case FFX_STATE_PRESENT:
        *d3d_state = 0; // D3D12_RESOURCE_STATE_COMMON
        return true;
    case FFX_STATE_UAV:
        *d3d_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        return true;
    case FFX_STATE_COMPUTE_READ:
        *d3d_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        return true;
    case FFX_STATE_PIXEL_READ:
        *d3d_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        return true;
    case FFX_STATE_COMPUTE_READ | FFX_STATE_PIXEL_READ:
        *d3d_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        return true;
    case FFX_STATE_COPY_SRC:
        *d3d_state = D3D12_RESOURCE_STATE_COPY_SOURCE;
        return true;
    case FFX_STATE_COPY_DEST:
        *d3d_state = D3D12_RESOURCE_STATE_COPY_DEST;
        return true;
    case FFX_STATE_RENDER_TARGET:
        *d3d_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
        return true;
    case FFX_STATE_DEPTH_WRITE:
        *d3d_state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        return true;
    default:
        return false;
    }
}

static f32 quality_ratio(u32 mode) {
    switch (mode) {
    case 0: return 1.0f;  // NativeAA
    case 1: return 1.5f;  // Quality
    case 2: return 1.7f;  // Balanced
    case 3: return 2.0f;  // Performance
    case 4: return 3.0f;  // Ultra Performance
    default: return 1.5f;
    }
}

static f32 halton(s32 index, s32 base) {
    f32 f = 1.0f;
    f32 r = 0.0f;
    s32 i = index;
    while (i > 0) {
        f /= (f32)base;
        r += f * (f32)(i % base);
        i /= base;
    }
    return r;
}

static u32 emulate_query(FfxHeader* desc) {
    switch (desc->type) {
    case FFX_DESC_GET_UPSCALE_RATIO: {
        FfxQueryUpscaleRatio* q = (FfxQueryUpscaleRatio*)desc;
        if (!q->out_ratio)
            return FFX_ERROR_PARAMETER;
        *q->out_ratio = quality_ratio(q->quality_mode);
        return FFX_OK;
    }
    case FFX_DESC_GET_RENDER_RESOLUTION: {
        FfxQueryRenderResolution* q = (FfxQueryRenderResolution*)desc;
        if (!q->out_render_width || !q->out_render_height || !q->display_width || !q->display_height)
            return FFX_ERROR_PARAMETER;
        f32 ratio = quality_ratio(q->quality_mode);
        u32 rw = (u32)roundf((f32)q->display_width / ratio);
        u32 rh = (u32)roundf((f32)q->display_height / ratio);
        *q->out_render_width = rw ? rw : 1;
        *q->out_render_height = rh ? rh : 1;
        return FFX_OK;
    }
    case FFX_DESC_GET_JITTER_PHASE_COUNT: {
        FfxQueryJitterPhaseCount* q = (FfxQueryJitterPhaseCount*)desc;
        if (!q->out_phase_count || q->render_width <= 0 || q->display_width <= 0)
            return FFX_ERROR_PARAMETER;
        f32 ratio = (f32)q->display_width / (f32)q->render_width;
        s32 phases = (s32)ceilf(8.0f * ratio * ratio);
        *q->out_phase_count = phases > 0 ? phases : 8;
        return FFX_OK;
    }
    case FFX_DESC_GET_JITTER_OFFSET: {
        FfxQueryJitterOffset* q = (FfxQueryJitterOffset*)desc;
        if (!q->out_x || !q->out_y || q->phase_count <= 0)
            return FFX_ERROR_PARAMETER;
        s32 idx = (q->index % q->phase_count);
        if (idx < 0)
            idx += q->phase_count;
        idx += 1;
        *q->out_x = halton(idx, 2) - 0.5f;
        *q->out_y = halton(idx, 3) - 0.5f;
        return FFX_OK;
    }
    default:
        return FFX_NO_PROVIDER;
    }
}

static u32 ffxCreateContext(void** context, FfxHeader* desc, const void* mem_cb) {
    const FfxHost* host = ffx_host();
    if (!host)
        return FFX_NO_PROVIDER;
    if (!context || !desc)
        return FFX_ERROR_PARAMETER;
    if (desc->type != FFX_DESC_CREATE_UPSCALE) {
        LOG_W_ONCE("ffx", "unsupported FidelityFX context type 0x%llx refused", desc->type);
        return FFX_NO_PROVIDER;
    }

    FfxContextEntry* entry = (FfxContextEntry*)calloc(1, sizeof(FfxContextEntry));
    if (!entry)
        return FFX_NO_PROVIDER;
    entry->create = *(const FfxCreateUpscale*)desc;

    FfxHeader* after = entry->create.header.next;
    FfxHeader* tail = (after && after->type == FFX_DESC_CONSOLE_BACKEND) ? after->next : after;
    entry->version.header.type = FFX_DESC_OVERRIDE_VERSION;
    entry->version.header.next = tail;
    entry->version.id = host->provider_id;
    entry->backend.header.type = FFX_DESC_HOST_BACKEND;
    entry->backend.header.next = &entry->version.header;
    entry->backend.device = host->device;
    entry->create.header.next = &entry->backend.header;

    u32 code = host->create(context, &entry->create.header, mem_cb);
    if (code != FFX_OK) {
        LOG_E("ffx", "host ffxCreateContext failed (code %u, render %ux%u, output %ux%u, flags 0x%x)", code,
              entry->create.max_render[0], entry->create.max_render[1], entry->create.max_output[0],
              entry->create.max_output[1], entry->create.flags);
        free(entry);
        return code;
    }

    entry->handle = *context;
    AcquireSRWLockExclusive(&g_ctx_lock);
    entry->next = g_contexts;
    g_contexts = entry;
    ReleaseSRWLockExclusive(&g_ctx_lock);

    LOG_I("ffx", "created upscaler context %p (render %ux%u -> output %ux%u, flags 0x%x)", *context,
          entry->create.max_render[0], entry->create.max_render[1], entry->create.max_output[0],
          entry->create.max_output[1], entry->create.flags);
    return FFX_OK;
}

static u32 ffxDestroyContext(void** context, const void* mem_cb) {
    const FfxHost* host = ffx_host();
    if (!host)
        return FFX_NO_PROVIDER;
    if (!context || !*context)
        return FFX_ERROR_PARAMETER;

    void* handle = *context;
    AcquireSRWLockExclusive(&g_ctx_lock);
    FfxContextEntry** prev = &g_contexts;
    FfxContextEntry* found = nullptr;
    while (*prev) {
        if ((*prev)->handle == handle) {
            found = *prev;
            *prev = found->next;
            break;
        }
        prev = &(*prev)->next;
    }
    ReleaseSRWLockExclusive(&g_ctx_lock);
    if (!found)
        return FFX_ERROR_PARAMETER;

    gpu_wait_idle();
    u32 code = host->destroy(context, mem_cb);
    free(found);
    return code;
}

static u32 ffxConfigure(void** context, FfxHeader* desc) {
    const FfxHost* host = ffx_host();
    return host ? host->configure(context, desc) : FFX_NO_PROVIDER;
}

static u32 ffxQuery(void** context, FfxHeader* desc) {
    if (!desc)
        return FFX_ERROR_PARAMETER;
    const FfxHost* host = ffx_host();
    if (!host)
        return emulate_query(desc);

    if (desc->type == FFX_DESC_GET_VERSIONS) {
        FfxGetVersions q = *(const FfxGetVersions*)desc;
        q.device = host->device;
        return host->query(context, &q.header);
    }
    if (desc->type == FFX_DESC_MEMORY_USAGE_WITH_DEVICE) {
        FfxMemoryUsageWithDevice q = *(const FfxMemoryUsageWithDevice*)desc;
        q.device = host->device;
        return host->query(context, &q.header);
    }
    return host->query(context, desc);
}

static u32 ffxDispatch(void** context, FfxHeader* desc) {
    const FfxHost* host = ffx_host();
    if (!host)
        return FFX_NO_PROVIDER;
    if (!context || !*context || !desc)
        return FFX_ERROR_PARAMETER;
    if (desc->type != FFX_DESC_DISPATCH_UPSCALE) {
        LOG_W_ONCE("ffx", "unsupported FidelityFX dispatch type 0x%llx refused", desc->type);
        return FFX_ERROR_UNKNOWN_TYPE;
    }

    FfxDispatchUpscale d = *(const FfxDispatchUpscale*)desc;
    ConsoleCommandList* list = (ConsoleCommandList*)d.command_list;
    if (!list || !list->recording || !list->desktop) {
        LOG_W_ONCE("ffx", "ffxDispatch called without a recording command list");
        return FFX_ERROR_PARAMETER;
    }

    for (u32 i = 0; i < ARRAY_COUNT(d.resources); i++) {
        if (!d.resources[i].resource) {
            if (i == 0 || i == 1 || i == 2 || i == 6) {
                LOG_W_ONCE("ffx", "ffxDispatch missing required texture %u", i);
                return FFX_ERROR_PARAMETER;
            }
            continue;
        }
        ConsoleResource* cr = (ConsoleResource*)d.resources[i].resource;
        if (cr->desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
            LOG_W_ONCE("ffx", "ffxDispatch resource %u is a buffer", i);
            return FFX_ERROR_PARAMETER;
        }
        u64 offset = 0;
        ID3D12Resource* tex = console_resource_desktop(cr, &offset, i == 6 ? GPU_WRITE : GPU_READ);
        if (!tex) {
            LOG_W_ONCE("ffx", "ffxDispatch texture %u has no desktop backing", i);
            return FFX_ERROR_PARAMETER;
        }
        u32 host_state = 0;
        if (!map_host_state(d.resources[i].state, &host_state)) {
            LOG_W_ONCE("ffx", "ffxDispatch resource %u has unsupported state 0x%x", i, d.resources[i].state);
            return FFX_ERROR_PARAMETER;
        }
        // Keep read-only texture-sync or no-UAV-on-desktop textures aligned with barriers.cpp's layout.
        if (texture_sync_tracks(cr->desc) ||
            (console_texture_loses_uav(cr->desc) && host_state == D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
            host_state = 0;
        d.resources[i].resource = tex;
        d.resources[i].state = host_state;
    }

    d.command_list = list->desktop;
    u32 code = host->dispatch(context, &d.header);
    list_restore_state(list);
    if (code == FFX_OK) {
        static volatile long s_first_dispatch = 0;
        if (log_once_flag(&s_first_dispatch)) {
            LOG_I("ffx", "first FSR dispatch succeeded (%ux%u -> %ux%u)", d.render_size[0], d.render_size[1],
                  d.upscale_size[0], d.upscale_size[1]);
        }
    } else {
        LOG_W_ONCE("ffx", "host ffxDispatch returned error %u (render %ux%u -> output %ux%u)", code,
                   d.render_size[0], d.render_size[1], d.upscale_size[0], d.upscale_size[1]);
    }
    return code;
}

static const HleExport k_ffx_exports[] = {
    HLE_EXPORT(ffxConfigure),
    HLE_EXPORT(ffxCreateContext),
    HLE_EXPORT(ffxDestroyContext),
    HLE_EXPORT(ffxDispatch),
    HLE_EXPORT(ffxQuery),
};

extern const HleModule g_hle_module_ffx_loader = {
    "amd_fidelityfx_loader_xbox.dll",
    k_ffx_exports,
    ARRAY_COUNT(k_ffx_exports),
};
