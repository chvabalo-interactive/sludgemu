// amd_fidelityfx_loader_xbox.dll: AMD FidelityFX upscaler HLE.
//
// Translates console FidelityFX calls to the desktop FidelityFX SDK DLLs (copied next to
// sludgemu.exe at build time), substituting the desktop ID3D12Device, ID3D12GraphicsCommandList,
// desktop ID3D12Resource textures, and desktop resource states.

#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ffx_api.h>
#include <ffx_api_dx12.h>
#ifndef _WINDOWS
#define _WINDOWS
#endif
#include <ffx_api_loader.h>
#include <ffx_api_types.h>
#include <ffx_upscale.h>

#include "common/log.h"
#include "common/types.h"
#include "gpu/gpu.h"
#include "hle/d3d12/command_list.h"
#include "hle/d3d12/d3d12_objects.h"
#include "hle/d3d12/texture_sync.h"
#include "hle/hle.h"

// Legacy Xbox console backend descriptor ID seen in title chains.
#define FFX_DESC_CONSOLE_BACKEND 0x1000002ull

// Console FidelityFX titles share the same descriptor ABI layout as the desktop FidelityFX SDK.
static_assert(sizeof(ffxCreateContextDescUpscale) == 0x30, "ffxCreateContextDescUpscale layout");
static_assert(sizeof(FfxApiResource) == 0x30, "FfxApiResource layout");
static_assert(sizeof(ffxDispatchDescUpscale) == 0x1B0, "ffxDispatchDescUpscale layout");

struct FfxHost {
    HMODULE dll;
    ffxFunctions fn;
    ID3D12Device13* device;
    u64 provider_id;
    char provider_name[64];
    bool ready;
};

// FidelityFX keeps pointers in the create descriptor chain until destroy.
struct FfxContextEntry {
    ffxContext handle;
    ffxCreateContextDescUpscale create;
    ffxCreateBackendDX12Desc backend;
    ffxOverrideVersion version;
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

static bool select_provider(PfnFfxQuery query_fn, ID3D12Device13* device, u64* out_id, char* out_name,
                            usize out_cap) {
    u64 count = 0;
    ffxQueryDescGetVersions q = {};
    q.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    q.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    q.device = device;
    q.outputCount = &count;
    ffxReturnCode_t code = query_fn(nullptr, &q.header);
    if (code != FFX_API_RETURN_OK || count == 0) {
        LOG_W("ffx", "host upscaler reports no providers for this GPU (code %u, count %llu)", code, count);
        return false;
    }
    if (count > 64)
        count = 64;
    u64 ids[64] = {};
    const char* names[64] = {};
    q.versionIds = ids;
    q.versionNames = names;
    code = query_fn(nullptr, &q.header);
    if (code != FFX_API_RETURN_OK) {
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

static HMODULE load_loader_dll() {
    wchar_t path[MAX_PATH] = {};
    DWORD exe_len = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (exe_len > 0 && exe_len < MAX_PATH) {
        wchar_t* slash = wcsrchr(path, L'\\');
        if (slash && (usize)(slash - path + 36) < MAX_PATH) {
            wcscpy_s(slash + 1, (usize)MAX_PATH - (usize)(slash - path + 1), L"amd_fidelityfx_loader_dx12.dll");
            HMODULE m =
                LoadLibraryExW(path, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
            if (m)
                return m;
        }
    }
    return LoadLibraryW(L"amd_fidelityfx_loader_dx12.dll");
}

static BOOL CALLBACK init_host_once(PINIT_ONCE, PVOID, PVOID*) {
    ID3D12Device13* device = gpu_device();
    if (!device)
        return TRUE;
    HMODULE dll = load_loader_dll();
    if (!dll) {
        LOG_W("ffx", "amd_fidelityfx_loader_dx12.dll not found; FidelityFX disabled");
        return TRUE;
    }
    g_host.dll = dll;
    ffxLoadFunctions(&g_host.fn, dll);
    g_host.device = device;
    if (!g_host.fn.CreateContext || !g_host.fn.DestroyContext || !g_host.fn.Configure || !g_host.fn.Query ||
        !g_host.fn.Dispatch) {
        LOG_W("ffx", "amd_fidelityfx_loader_dx12.dll is missing expected exports");
        FreeLibrary(dll);
        g_host.dll = nullptr;
        return TRUE;
    }
    if (!select_provider(g_host.fn.Query, device, &g_host.provider_id, g_host.provider_name,
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
    case FFX_API_RESOURCE_STATE_COMMON:
    case FFX_API_RESOURCE_STATE_PRESENT:
        *d3d_state = 0; // D3D12_RESOURCE_STATE_COMMON
        return true;
    case FFX_API_RESOURCE_STATE_UNORDERED_ACCESS:
        *d3d_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        return true;
    case FFX_API_RESOURCE_STATE_COMPUTE_READ:
        *d3d_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        return true;
    case FFX_API_RESOURCE_STATE_PIXEL_READ:
        *d3d_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        return true;
    case FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ:
        *d3d_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        return true;
    case FFX_API_RESOURCE_STATE_COPY_SRC:
        *d3d_state = D3D12_RESOURCE_STATE_COPY_SOURCE;
        return true;
    case FFX_API_RESOURCE_STATE_COPY_DEST:
        *d3d_state = D3D12_RESOURCE_STATE_COPY_DEST;
        return true;
    case FFX_API_RESOURCE_STATE_RENDER_TARGET:
        *d3d_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
        return true;
    case FFX_API_RESOURCE_STATE_DEPTH_ATTACHMENT:
        *d3d_state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        return true;
    default:
        return false;
    }
}

static bool is_console_backend_desc(const ffxApiHeader* h) {
    return h->type == FFX_DESC_CONSOLE_BACKEND || (h->type & FFX_API_BACKEND_MASK) == FFX_API_BACKEND_ID_XBOX;
}

static ffxReturnCode_t ffx_create_context(ffxContext* context, ffxCreateContextDescHeader* desc,
                                          const ffxAllocationCallbacks* mem_cb) {
    const FfxHost* host = ffx_host();
    if (!host)
        return FFX_API_RETURN_NO_PROVIDER;
    if (!context || !desc)
        return FFX_API_RETURN_ERROR_PARAMETER;
    if (desc->type != FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE) {
        LOG_W_ONCE("ffx", "unsupported FidelityFX context type 0x%llx refused", desc->type);
        return FFX_API_RETURN_NO_PROVIDER;
    }

    FfxContextEntry* entry = (FfxContextEntry*)calloc(1, sizeof(FfxContextEntry));
    if (!entry)
        return FFX_API_RETURN_NO_PROVIDER;
    entry->create = *(const ffxCreateContextDescUpscale*)desc;

    ffxApiHeader* tail = entry->create.header.pNext;
    while (tail && is_console_backend_desc(tail))
        tail = tail->pNext;
    entry->version.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
    entry->version.header.pNext = tail;
    entry->version.versionId = host->provider_id;
    entry->backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    entry->backend.header.pNext = &entry->version.header;
    entry->backend.device = host->device;
    entry->create.header.pNext = &entry->backend.header;

    ffxReturnCode_t code = host->fn.CreateContext(context, &entry->create.header, mem_cb);
    if (code != FFX_API_RETURN_OK) {
        LOG_E("ffx", "host ffxCreateContext failed (code %u, render %ux%u, output %ux%u, flags 0x%x)", code,
              entry->create.maxRenderSize.width, entry->create.maxRenderSize.height,
              entry->create.maxUpscaleSize.width, entry->create.maxUpscaleSize.height, entry->create.flags);
        free(entry);
        return code;
    }

    entry->handle = *context;
    AcquireSRWLockExclusive(&g_ctx_lock);
    entry->next = g_contexts;
    g_contexts = entry;
    ReleaseSRWLockExclusive(&g_ctx_lock);

    LOG_I("ffx", "created upscaler context %p (render %ux%u -> output %ux%u, flags 0x%x)", *context,
          entry->create.maxRenderSize.width, entry->create.maxRenderSize.height,
          entry->create.maxUpscaleSize.width, entry->create.maxUpscaleSize.height, entry->create.flags);
    return FFX_API_RETURN_OK;
}

static ffxReturnCode_t ffx_destroy_context(ffxContext* context, const ffxAllocationCallbacks* mem_cb) {
    const FfxHost* host = ffx_host();
    if (!host)
        return FFX_API_RETURN_NO_PROVIDER;
    if (!context || !*context)
        return FFX_API_RETURN_ERROR_PARAMETER;

    ffxContext handle = *context;
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
        return FFX_API_RETURN_ERROR_PARAMETER;

    gpu_wait_idle();
    ffxReturnCode_t code = host->fn.DestroyContext(context, mem_cb);
    free(found);
    return code;
}

static ffxReturnCode_t ffx_configure(ffxContext* context, const ffxConfigureDescHeader* desc) {
    const FfxHost* host = ffx_host();
    return host ? host->fn.Configure(context, desc) : FFX_API_RETURN_NO_PROVIDER;
}

static ffxReturnCode_t ffx_query(ffxContext* context, ffxQueryDescHeader* desc) {
    if (!desc)
        return FFX_API_RETURN_ERROR_PARAMETER;
    const FfxHost* host = ffx_host();
    if (!host)
        return FFX_API_RETURN_NO_PROVIDER;

    if (desc->type == FFX_API_QUERY_DESC_TYPE_GET_VERSIONS) {
        ffxQueryDescGetVersions q = *(const ffxQueryDescGetVersions*)desc;
        q.device = host->device;
        return host->fn.Query(context, &q.header);
    }
    if (desc->type == FFX_API_QUERY_DESC_TYPE_UPSCALE_GPU_MEMORY_USAGE_V2) {
        ffxQueryDescUpscaleGetGPUMemoryUsageV2 q = *(const ffxQueryDescUpscaleGetGPUMemoryUsageV2*)desc;
        q.device = host->device;
        return host->fn.Query(context, &q.header);
    }
    return host->fn.Query(context, desc);
}

static ffxReturnCode_t ffx_dispatch(ffxContext* context, const ffxDispatchDescHeader* desc) {
    const FfxHost* host = ffx_host();
    if (!host)
        return FFX_API_RETURN_NO_PROVIDER;
    if (!context || !*context || !desc)
        return FFX_API_RETURN_ERROR_PARAMETER;
    if (desc->type != FFX_API_DISPATCH_DESC_TYPE_UPSCALE) {
        LOG_W_ONCE("ffx", "unsupported FidelityFX dispatch type 0x%llx refused", desc->type);
        return FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE;
    }

    ffxDispatchDescUpscale d = *(const ffxDispatchDescUpscale*)desc;
    ConsoleCommandList* list = (ConsoleCommandList*)d.commandList;
    if (!list || !list->recording || !list->desktop) {
        LOG_W_ONCE("ffx", "ffxDispatch called without a recording command list");
        return FFX_API_RETURN_ERROR_PARAMETER;
    }

    FfxApiResource* resources[] = {&d.color,    &d.depth, &d.motionVectors,            &d.exposure,
                                   &d.reactive, &d.transparencyAndComposition, &d.output};
    for (u32 i = 0; i < ARRAY_COUNT(resources); i++) {
        FfxApiResource* r = resources[i];
        if (!r->resource) {
            if (i == 0 || i == 1 || i == 2 || i == 6) {
                LOG_W_ONCE("ffx", "ffxDispatch missing required texture %u", i);
                return FFX_API_RETURN_ERROR_PARAMETER;
            }
            continue;
        }
        ConsoleResource* cr = (ConsoleResource*)r->resource;
        if (cr->desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
            LOG_W_ONCE("ffx", "ffxDispatch resource %u is a buffer", i);
            return FFX_API_RETURN_ERROR_PARAMETER;
        }
        u64 offset = 0;
        ID3D12Resource* tex = console_resource_desktop(cr, &offset, r == &d.output ? GPU_WRITE : GPU_READ);
        if (!tex) {
            LOG_W_ONCE("ffx", "ffxDispatch texture %u has no desktop backing", i);
            return FFX_API_RETURN_ERROR_PARAMETER;
        }
        u32 host_state = 0;
        if (!map_host_state(r->state, &host_state)) {
            LOG_W_ONCE("ffx", "ffxDispatch resource %u has unsupported state 0x%x", i, r->state);
            return FFX_API_RETURN_ERROR_PARAMETER;
        }
        // Keep read-only texture-sync or no-UAV-on-desktop textures aligned with barriers.cpp's layout.
        if (texture_sync_tracks(cr->desc) ||
            (console_texture_loses_uav(cr->desc) && host_state == D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
            host_state = 0;
        r->resource = tex;
        r->state = host_state;
    }

    d.commandList = list->desktop;
    ffxReturnCode_t code = host->fn.Dispatch(context, &d.header);
    list_restore_state(list);
    if (code == FFX_API_RETURN_OK) {
        static volatile long s_first_dispatch = 0;
        if (log_once_flag(&s_first_dispatch)) {
            LOG_I("ffx", "first FSR dispatch succeeded (%ux%u -> %ux%u)", d.renderSize.width, d.renderSize.height,
                  d.upscaleSize.width, d.upscaleSize.height);
        }
    } else {
        LOG_W_ONCE("ffx", "host ffxDispatch returned error %u (render %ux%u -> output %ux%u)", code,
                   d.renderSize.width, d.renderSize.height, d.upscaleSize.width, d.upscaleSize.height);
    }
    return code;
}

static const HleExport k_ffx_exports[] = {
    HLE_EXPORT_AS(ffx_configure, "ffxConfigure"),
    HLE_EXPORT_AS(ffx_create_context, "ffxCreateContext"),
    HLE_EXPORT_AS(ffx_destroy_context, "ffxDestroyContext"),
    HLE_EXPORT_AS(ffx_dispatch, "ffxDispatch"),
    HLE_EXPORT_AS(ffx_query, "ffxQuery"),
};

extern const HleModule g_hle_module_ffx_loader = {
    "amd_fidelityfx_loader_xbox.dll",
    k_ffx_exports,
    ARRAY_COUNT(k_ffx_exports),
};
