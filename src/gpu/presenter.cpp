#include "gpu/presenter.h"

#include <dxgi1_6.h>
#include <stdio.h>
#include <stdlib.h>

#include "common/log.h"
#include "gpu/gpu.h"
#include "platform/window.h"
#include "shaders/present_ps.h"
#include "shaders/present_vs.h"

#define FRAME_COUNT    3
#define BACK_FORMAT    DXGI_FORMAT_R10G10B10A2_UNORM
#define DISPLAY_ASPECT (16.0f / 9.0f)

struct Presenter {
    SRWLOCK lock;
    bool failed; // setup failed once; presents are dropped (logged then)

    ID3D12CommandQueue* queue;
    IDXGISwapChain4* swap_chain;
    UINT swap_flags;
    bool tearing;
    u32 width;
    u32 height;
    ID3D12Resource* buffers[FRAME_COUNT];

    ID3D12DescriptorHeap* rtv_heap;
    ID3D12DescriptorHeap* srv_heap; // shader-visible: PRESENT_MAX_PLANES per frame
    u32 rtv_stride;
    u32 srv_stride;

    ID3D12RootSignature* root_signature;
    ID3D12PipelineState* opaque; // the scene
    ID3D12PipelineState* blend;  // planes above it: premultiplied alpha

    ID3D12CommandAllocator* allocators[FRAME_COUNT];
    ID3D12GraphicsCommandList7* list;
    ID3D12Fence* fence;
    u64 fence_value;
    u64 frame_fence[FRAME_COUNT]; // when each back buffer's last frame is done
};

static Presenter g_presenter = {SRWLOCK_INIT};

static ID3D12PipelineState* create_pipeline(ID3D12Device* device, ID3D12RootSignature* root, bool blend) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
    d.pRootSignature = root;
    d.VS = {k_present_vs, sizeof(k_present_vs)};
    d.PS = {k_present_ps, sizeof(k_present_ps)};
    D3D12_RENDER_TARGET_BLEND_DESC& rt = d.BlendState.RenderTarget[0];
    rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    if (blend) {
        rt.BlendEnable = TRUE;
        rt.SrcBlend = D3D12_BLEND_ONE;
        rt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        rt.BlendOp = D3D12_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D12_BLEND_ONE;
        rt.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    }
    d.SampleMask = UINT_MAX;
    d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    d.RasterizerState.DepthClipEnable = TRUE;
    d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    d.NumRenderTargets = 1;
    d.RTVFormats[0] = BACK_FORMAT;
    d.SampleDesc.Count = 1;
    ID3D12PipelineState* pso = nullptr;
    if (FAILED(device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso))))
        LOG_E("present", "cannot create the presentation pipeline");
    return pso;
}

static void acquire_buffers(Presenter* p) {
    ID3D12Device* device = gpu_device();
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = p->rtv_heap->GetCPUDescriptorHandleForHeapStart();
    for (u32 i = 0; i < FRAME_COUNT; i++) {
        p->swap_chain->GetBuffer(i, IID_PPV_ARGS(&p->buffers[i]));
        device->CreateRenderTargetView(p->buffers[i], nullptr, {rtv.ptr + i * p->rtv_stride});
    }
}

static void capture_init();

static bool setup(Presenter* p, ID3D12CommandQueue* queue) {
    capture_init();
    ID3D12Device13* device = gpu_device();
    IDXGIFactory6* factory = gpu_factory();
    HWND hwnd = window_main();
    if (!device || !factory || !hwnd)
        return false;

    BOOL tearing = FALSE;
    factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing));
    p->tearing = tearing != FALSE;
    p->swap_flags = p->tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;

    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Format = BACK_FORMAT;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = FRAME_COUNT;
    sd.Scaling = DXGI_SCALING_NONE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.Flags = p->swap_flags;
    IDXGISwapChain1* sc1 = nullptr;
    HRESULT hr = factory->CreateSwapChainForHwnd(queue, hwnd, &sd, nullptr, nullptr, &sc1);
    if (FAILED(hr)) {
        LOG_E("present", "cannot create the swap chain (0x%08lx)", (unsigned long)hr);
        return false;
    }
    hr = sc1->QueryInterface(IID_PPV_ARGS(&p->swap_chain));
    sc1->Release();
    if (FAILED(hr))
        return false;
    // The window handles Alt+Enter itself (borderless fullscreen).
    factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);

    DXGI_SWAP_CHAIN_DESC1 actual;
    p->swap_chain->GetDesc1(&actual);
    p->width = actual.Width;
    p->height = actual.Height;

    D3D12_DESCRIPTOR_HEAP_DESC rh = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, FRAME_COUNT};
    D3D12_DESCRIPTOR_HEAP_DESC sh = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, FRAME_COUNT * PRESENT_MAX_PLANES,
                                     D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
    if (FAILED(device->CreateDescriptorHeap(&rh, IID_PPV_ARGS(&p->rtv_heap))) ||
        FAILED(device->CreateDescriptorHeap(&sh, IID_PPV_ARGS(&p->srv_heap))))
        return false;
    p->rtv_stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    p->srv_stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    acquire_buffers(p);

    // The root signature is part of the shaders.
    if (FAILED(device->CreateRootSignature(0, k_present_vs, sizeof(k_present_vs),
                                           IID_PPV_ARGS(&p->root_signature))))
        return false;
    p->opaque = create_pipeline(device, p->root_signature, false);
    p->blend = create_pipeline(device, p->root_signature, true);
    if (!p->opaque || !p->blend)
        return false;

    for (u32 i = 0; i < FRAME_COUNT; i++)
        if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                  IID_PPV_ARGS(&p->allocators[i]))))
            return false;
    if (FAILED(device->CreateCommandList1(0, D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_LIST_FLAG_NONE,
                                          IID_PPV_ARGS(&p->list))) ||
        FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&p->fence))))
        return false;

    p->queue = queue;
    queue->AddRef();
    LOG_I("present", "swap chain %ux%u, %s", p->width, p->height,
          p->tearing ? "tearing allowed" : "no tearing");
    return true;
}

// RenderDoc: under renderdoccmd, SLUDGE_RENDERDOC_FRAME=<n>[,<n>...] captures the frames after those
// presents (up to 16, ascending). Title rendering runs on its own queues, so a capture spans the
// whole frame.
typedef int(__cdecl* RenderDocGetApi)(int version, void** api);
#define RENDERDOC_API_VERSION_1_1_2 10102
#define RENDERDOC_TRIGGER_CAPTURE   15 // entry of the API function table
#define RENDERDOC_MAX_FRAMES        16

static void renderdoc_frame() {
    static void** s_api = nullptr;
    static u64 s_frames[RENDERDOC_MAX_FRAMES];
    static u32 s_frame_count = 0, s_next = 0;
    static u64 s_frame = 0;
    static bool s_checked = false;
    if (!s_checked) {
        s_checked = true;
        char value[256];
        HMODULE renderdoc = GetModuleHandleW(L"renderdoc.dll");
        RenderDocGetApi get_api =
            renderdoc ? (RenderDocGetApi)GetProcAddress(renderdoc, "RENDERDOC_GetAPI") : nullptr;
        if (get_api && GetEnvironmentVariableA("SLUDGE_RENDERDOC_FRAME", value, sizeof(value)) &&
            get_api(RENDERDOC_API_VERSION_1_1_2, (void**)&s_api) == 1) {
            for (const char* p = value; *p && s_frame_count < RENDERDOC_MAX_FRAMES;) {
                char* end = nullptr;
                u64 frame = strtoull(p, &end, 10);
                if (end == p)
                    break;
                s_frames[s_frame_count++] = frame;
                p = *end == ',' ? end + 1 : end;
            }
            LOG_I("present", "RenderDoc: capturing %u frames from %llu", s_frame_count,
                  s_frame_count ? (unsigned long long)s_frames[0] : 0ull);
        }
    }
    if (!s_api)
        return;
    ++s_frame;
    while (s_next < s_frame_count && s_frames[s_next] < s_frame)
        s_next++; // passed (not ascending)
    if (s_next < s_frame_count && s_frames[s_next] == s_frame) {
        ((void(__cdecl*)())s_api[RENDERDOC_TRIGGER_CAPTURE])();
        s_next++;
    }
}

static void wait_frame(Presenter* p, u64 value) {
    if (p->fence->GetCompletedValue() < value)
        p->fence->SetEventOnCompletion(value, nullptr);
}

// Follows the window's size; the swap chain's buffers must be idle and unreferenced to resize. False
// while the GPU still runs presents: waiting for them could wait for title work submitted after this
// present (see presenter_present).
static bool fit_window(Presenter* p) {
    u32 w, h;
    window_client_size(&w, &h);
    if (!w || !h || (w == p->width && h == p->height))
        return true;
    if (p->fence->GetCompletedValue() < p->fence_value)
        return false;
    for (u32 i = 0; i < FRAME_COUNT; i++) {
        p->buffers[i]->Release();
        p->buffers[i] = nullptr;
    }
    HRESULT hr = p->swap_chain->ResizeBuffers(FRAME_COUNT, w, h, BACK_FORMAT, p->swap_flags);
    if (FAILED(hr))
        LOG_E("present", "cannot resize the swap chain to %ux%u (0x%08lx)", w, h, (unsigned long)hr);
    DXGI_SWAP_CHAIN_DESC1 actual;
    p->swap_chain->GetDesc1(&actual);
    p->width = actual.Width;
    p->height = actual.Height;
    acquire_buffers(p);
    return true;
}

// A view that reads the stored values as they are: the swap chain shows them unconverted.
static DXGI_FORMAT plane_view_format(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8X8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default:
        return f;
    }
}

static void draw_plane(Presenter* p, u32 frame, u32 index, const PresentPlane& plane, const f32 display[4]) {
    ID3D12Device* device = gpu_device();
    D3D12_RESOURCE_DESC desc = plane.texture->GetDesc();
    D3D12_SHADER_RESOURCE_VIEW_DESC v = {};
    v.Format = plane_view_format(desc.Format);
    v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    v.Texture2D.MipLevels = 1;
    u32 slot = frame * PRESENT_MAX_PLANES + index;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = p->srv_heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = p->srv_heap->GetGPUDescriptorHandleForHeapStart();
    cpu.ptr += slot * p->srv_stride;
    gpu.ptr += slot * p->srv_stride;
    device->CreateShaderResourceView(plane.texture, &v, cpu);

    // Destination: the plane's rectangle of the display area, in clip space (y up).
    f32 dw = display[2] - display[0], dh = display[3] - display[1];
    f32 x0 = display[0] + plane.dest[0] * dw, x1 = display[0] + plane.dest[2] * dw;
    f32 y0 = display[1] + plane.dest[1] * dh, y1 = display[1] + plane.dest[3] * dh;
    f32 tw = (f32)desc.Width, th = (f32)desc.Height;
    const D3D12_RECT& s = plane.source;
    bool whole = s.right <= s.left || s.bottom <= s.top;
    f32 constants[8] = {
        x0 / p->width * 2.0f - 1.0f,  1.0f - y0 / p->height * 2.0f, x1 / p->width * 2.0f - 1.0f,
        1.0f - y1 / p->height * 2.0f, whole ? 0.0f : s.left / tw,   whole ? 0.0f : s.top / th,
        whole ? 1.0f : s.right / tw,  whole ? 1.0f : s.bottom / th,
    };
    p->list->SetPipelineState(index == 0 ? p->opaque : p->blend);
    p->list->SetGraphicsRoot32BitConstants(0, 8, constants, 0);
    p->list->SetGraphicsRootDescriptorTable(1, gpu);
    p->list->DrawInstanced(4, 1, 0, 0);
}

static void buffer_barrier(ID3D12GraphicsCommandList7* list, ID3D12Resource* buffer, bool to_target) {
    D3D12_TEXTURE_BARRIER b = {};
    b.SyncBefore = to_target ? D3D12_BARRIER_SYNC_NONE : D3D12_BARRIER_SYNC_RENDER_TARGET;
    b.SyncAfter = to_target ? D3D12_BARRIER_SYNC_RENDER_TARGET : D3D12_BARRIER_SYNC_NONE;
    b.AccessBefore = to_target ? D3D12_BARRIER_ACCESS_NO_ACCESS : D3D12_BARRIER_ACCESS_RENDER_TARGET;
    b.AccessAfter = to_target ? D3D12_BARRIER_ACCESS_RENDER_TARGET : D3D12_BARRIER_ACCESS_NO_ACCESS;
    b.LayoutBefore = to_target ? D3D12_BARRIER_LAYOUT_PRESENT : D3D12_BARRIER_LAYOUT_RENDER_TARGET;
    b.LayoutAfter = to_target ? D3D12_BARRIER_LAYOUT_RENDER_TARGET : D3D12_BARRIER_LAYOUT_PRESENT;
    b.pResource = buffer;
    b.Subresources.IndexOrFirstMipLevel = 0xffffffff; // all
    D3D12_BARRIER_GROUP g = {D3D12_BARRIER_TYPE_TEXTURE, 1};
    g.pTextureBarriers = &b;
    list->Barrier(1, &g);
}

// Console titles hand over planes in the present layout (desktop's common layout) with no access
// after their last write. The composite reads them as shader resources in between and gives them
// back as they came, which is what the title's next barrier on them names as its starting point.
static void plane_barriers(ID3D12GraphicsCommandList7* list, const PresentPlane* planes, u32 count,
                           bool to_read) {
    D3D12_TEXTURE_BARRIER b[PRESENT_MAX_PLANES] = {};
    u32 n = 0;
    for (u32 i = 0; i < count && i < PRESENT_MAX_PLANES; i++) {
        ID3D12Resource* t = planes[i].texture;
        if (!t || (n == 1 && b[0].pResource == t)) // both planes may show one texture
            continue;
        D3D12_TEXTURE_BARRIER& o = b[n++];
        o.SyncBefore = to_read ? D3D12_BARRIER_SYNC_NONE : D3D12_BARRIER_SYNC_PIXEL_SHADING;
        o.SyncAfter = to_read ? D3D12_BARRIER_SYNC_PIXEL_SHADING : D3D12_BARRIER_SYNC_NONE;
        o.AccessBefore = to_read ? D3D12_BARRIER_ACCESS_NO_ACCESS : D3D12_BARRIER_ACCESS_SHADER_RESOURCE;
        o.AccessAfter = to_read ? D3D12_BARRIER_ACCESS_SHADER_RESOURCE : D3D12_BARRIER_ACCESS_NO_ACCESS;
        o.LayoutBefore = to_read ? D3D12_BARRIER_LAYOUT_PRESENT : D3D12_BARRIER_LAYOUT_SHADER_RESOURCE;
        o.LayoutAfter = to_read ? D3D12_BARRIER_LAYOUT_SHADER_RESOURCE : D3D12_BARRIER_LAYOUT_PRESENT;
        o.pResource = t;
        o.Subresources.IndexOrFirstMipLevel = 0xffffffff; // all
    }
    if (!n)
        return;
    D3D12_BARRIER_GROUP g = {D3D12_BARRIER_TYPE_TEXTURE, n};
    g.pTextureBarriers = b;
    list->Barrier(1, &g);
}

// --- frame capture (diagnostics) ---------------------------------------------------------------------
//
// SLUDGE_CAPTURE_FRAMES=<first>,<count> writes presented frames <first> .. <first + count - 1> as BMP
// files (to SLUDGE_CAPTURE_DIR, or next to the log): what reaches the window frame by frame, for
// flicker and other things window screenshots don't show. Captured frames wait for the GPU.

static struct {
    u64 first, count;
    char dir[260];
    u64 presented;
    ID3D12Resource* readback;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
    u64 width, height;
} g_capture;

static void capture_init() {
    // The CRT's copy of the environment: the title may change the process's own by now.
    const char* spec = getenv("SLUDGE_CAPTURE_FRAMES");
    if (!spec)
        return;
    char* comma = nullptr;
    g_capture.first = strtoull(spec, &comma, 10);
    g_capture.count = comma && *comma == ',' ? strtoull(comma + 1, nullptr, 10) : 1;
    const char* dir = getenv("SLUDGE_CAPTURE_DIR");
    strcpy_s(g_capture.dir, dir && strlen(dir) < sizeof(g_capture.dir) ? dir : ".");
    LOG_I("present", "capturing frames %llu..%llu to %s", g_capture.first, g_capture.first + g_capture.count - 1,
          g_capture.dir);
}

// Whether to capture the frame being presented; makes the readback buffer for its size.
static bool capture_this_frame(ID3D12Resource* buffer) {
    u64 index = g_capture.presented++;
    if (!g_capture.count || index < g_capture.first || index >= g_capture.first + g_capture.count)
        return false;
    D3D12_RESOURCE_DESC desc = buffer->GetDesc();
    if (!g_capture.readback || desc.Width != g_capture.width || desc.Height != g_capture.height) {
        if (g_capture.readback)
            gpu_retire(g_capture.readback);
        g_capture.readback = nullptr;
        u64 bytes = 0;
        gpu_device()->GetCopyableFootprints(&desc, 0, 1, 0, &g_capture.footprint, nullptr, nullptr, &bytes);
        D3D12_HEAP_PROPERTIES heap = {D3D12_HEAP_TYPE_READBACK};
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = bytes;
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        HRESULT hr = gpu_device()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &rd,
                                                           D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                           IID_PPV_ARGS(&g_capture.readback));
        if (FAILED(hr)) {
            LOG_E("present", "cannot create the frame capture buffer (0x%08lx)", (unsigned long)hr);
            g_capture.count = 0;
            return false;
        }
        g_capture.width = desc.Width;
        g_capture.height = desc.Height;
    }
    return true;
}

static void capture_barrier(ID3D12GraphicsCommandList7* list, ID3D12Resource* buffer, bool before_copy) {
    D3D12_TEXTURE_BARRIER b = {};
    b.SyncBefore = before_copy ? D3D12_BARRIER_SYNC_RENDER_TARGET : D3D12_BARRIER_SYNC_COPY;
    b.SyncAfter = before_copy ? D3D12_BARRIER_SYNC_COPY : D3D12_BARRIER_SYNC_RENDER_TARGET;
    b.AccessBefore = before_copy ? D3D12_BARRIER_ACCESS_RENDER_TARGET : D3D12_BARRIER_ACCESS_COPY_SOURCE;
    b.AccessAfter = before_copy ? D3D12_BARRIER_ACCESS_COPY_SOURCE : D3D12_BARRIER_ACCESS_RENDER_TARGET;
    b.LayoutBefore = before_copy ? D3D12_BARRIER_LAYOUT_RENDER_TARGET : D3D12_BARRIER_LAYOUT_COPY_SOURCE;
    b.LayoutAfter = before_copy ? D3D12_BARRIER_LAYOUT_COPY_SOURCE : D3D12_BARRIER_LAYOUT_RENDER_TARGET;
    b.pResource = buffer;
    b.Subresources.IndexOrFirstMipLevel = 0xffffffff;
    D3D12_BARRIER_GROUP g = {D3D12_BARRIER_TYPE_TEXTURE, 1};
    g.pTextureBarriers = &b;
    list->Barrier(1, &g);
}

static void capture_record(ID3D12GraphicsCommandList7* list, ID3D12Resource* buffer) {
    capture_barrier(list, buffer, true);
    D3D12_TEXTURE_COPY_LOCATION dst = {g_capture.readback, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    dst.PlacedFootprint = g_capture.footprint;
    D3D12_TEXTURE_COPY_LOCATION src = {buffer, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    capture_barrier(list, buffer, false);
}

// Writes the captured frame (10-bit channels, cut to 8) once the GPU is done with it.
static void capture_write(u64 index) {
    u8* data = nullptr;
    D3D12_RANGE all = {0, (SIZE_T)(g_capture.footprint.Footprint.RowPitch * g_capture.height)};
    HRESULT hr = g_capture.readback->Map(0, &all, (void**)&data);
    if (FAILED(hr)) {
        LOG_E("present", "cannot map the frame capture buffer (0x%08lx)", (unsigned long)hr);
        return;
    }
    char path[320];
    snprintf(path, sizeof(path), "%s/frame_%06llu.bmp", g_capture.dir, index);
    FILE* f = nullptr;
    if (fopen_s(&f, path, "wb") != 0 || !f)
        LOG_E("present", "cannot write %s", path);
    if (f) {
        u32 w = (u32)g_capture.width, h = (u32)g_capture.height, row = w * 3, pad = (4 - row % 4) % 4;
        u32 image = (row + pad) * h;
        u8 header[54] = {'B', 'M'};
        *(u32*)(header + 2) = 54 + image;
        *(u32*)(header + 10) = 54;
        *(u32*)(header + 14) = 40;
        *(s32*)(header + 18) = (s32)w;
        *(s32*)(header + 22) = -(s32)h; // top down
        *(u16*)(header + 26) = 1;
        *(u16*)(header + 28) = 24;
        *(u32*)(header + 34) = image;
        fwrite(header, 1, sizeof(header), f);
        u8* line = (u8*)malloc(row + pad);
        for (u32 y = 0; line && y < h; y++) {
            const u32* px = (const u32*)(data + (u64)y * g_capture.footprint.Footprint.RowPitch);
            for (u32 x = 0; x < w; x++) {
                u32 v = px[x];
                line[x * 3 + 0] = (u8)(((v >> 20) & 0x3ff) >> 2); // B
                line[x * 3 + 1] = (u8)(((v >> 10) & 0x3ff) >> 2); // G
                line[x * 3 + 2] = (u8)((v & 0x3ff) >> 2);         // R
            }
            memset(line + row, 0, pad);
            fwrite(line, 1, row + pad, f);
        }
        free(line);
        fclose(f);
    }
    D3D12_RANGE none = {0, 0};
    g_capture.readback->Unmap(0, &none);
}

u64 presenter_completed() {
    // Created once with the swap chain and kept, so it can be read without the lock.
    ID3D12Fence* fence = g_presenter.fence;
    return fence ? fence->GetCompletedValue() : UINT64_MAX;
}

u64 presenter_present(ID3D12CommandQueue* queue, const PresentPlane* planes, u32 count) {
    Presenter* p = &g_presenter;
    AcquireSRWLockExclusive(&p->lock);
    if (!p->swap_chain && !p->failed && !setup(p, queue)) {
        LOG_E("present", "presentation unavailable; frames are dropped");
        p->failed = true;
    }
    if (p->failed) {
        ReleaseSRWLockExclusive(&p->lock);
        return 0;
    }
    if (queue != p->queue)
        LOG_W_ONCE("present", "presents from a second queue go through the first one's swap chain");

    // The presenter never waits for the GPU: the present comes from a title's submission path, and the
    // GPU may be waiting for title work submitted after it. While the GPU is a whole swap chain of
    // presents behind (the back buffer's allocator and descriptors still in use), or a resize waits for
    // it, presents are dropped; the GPU catches up meanwhile.
    u32 frame = p->swap_chain->GetCurrentBackBufferIndex();
    if (!fit_window(p) || p->fence->GetCompletedValue() < p->frame_fence[frame]) {
        LOG_W_ONCE("present", "the GPU is behind the presents; presents are dropped until it catches up");
        ReleaseSRWLockExclusive(&p->lock);
        return 0;
    }
    frame = p->swap_chain->GetCurrentBackBufferIndex(); // resizing starts the buffers over
    p->allocators[frame]->Reset();
    p->list->Reset(p->allocators[frame], nullptr);

    ID3D12Resource* buffer = p->buffers[frame];
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = p->rtv_heap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += frame * p->rtv_stride;
    buffer_barrier(p->list, buffer, true);
    plane_barriers(p->list, planes, count, true);
    static const f32 k_black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    p->list->ClearRenderTargetView(rtv, k_black, 0, nullptr);
    p->list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    D3D12_VIEWPORT viewport = {0.0f, 0.0f, (f32)p->width, (f32)p->height, 0.0f, 1.0f};
    D3D12_RECT scissor = {0, 0, (LONG)p->width, (LONG)p->height};
    p->list->RSSetViewports(1, &viewport);
    p->list->RSSetScissorRects(1, &scissor);
    p->list->SetGraphicsRootSignature(p->root_signature);
    p->list->SetDescriptorHeaps(1, &p->srv_heap);
    p->list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

    // The display area: the largest 16:9 rectangle centered in the window.
    f32 w = (f32)p->width, h = (f32)p->height;
    f32 dw = w, dh = w / DISPLAY_ASPECT;
    if (dh > h) {
        dh = h;
        dw = h * DISPLAY_ASPECT;
    }
    f32 display[4] = {(w - dw) * 0.5f, (h - dh) * 0.5f, (w + dw) * 0.5f, (h + dh) * 0.5f};
    for (u32 i = 0; i < count && i < PRESENT_MAX_PLANES; i++)
        if (planes[i].texture)
            draw_plane(p, frame, i, planes[i], display);
    u64 capture_index = g_capture.presented;
    bool capture = capture_this_frame(buffer);
    if (capture)
        capture_record(p->list, buffer);

    plane_barriers(p->list, planes, count, false);
    buffer_barrier(p->list, buffer, false);
    HRESULT hr = p->list->Close();
    if (SUCCEEDED(hr)) {
        ID3D12CommandList* list = p->list;
        queue->ExecuteCommandLists(1, &list);
        renderdoc_frame(); // called under the presenter lock
        hr = p->swap_chain->Present(1, 0);
        if (FAILED(hr) && !gpu_device_lost(hr))
            LOG_E("present", "Present failed (0x%08lx)", (unsigned long)hr);
    } else {
        LOG_E("present", "presentation list failed to close (0x%08lx)", (unsigned long)hr);
    }
    queue->Signal(p->fence, ++p->fence_value);
    p->frame_fence[frame] = p->fence_value;
    u64 value = p->fence_value;
    if (capture) {
        wait_frame(p, value);
        capture_write(capture_index);
    }
    ReleaseSRWLockExclusive(&p->lock);
    return value;
}
