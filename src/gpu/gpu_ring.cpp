#include "gpu/gpu_ring.h"

#include "common/log.h"

#define RING_MIN_SIZE MiB(16)
// A ring this large means the GPU is far behind the submissions using it (or blocked on later work).
#define RING_LARGE_SIZE MiB(256)

// Bytes of all rings' buffers, current and retired (memory reports): upload heaps, default heaps.
static volatile s64 g_ring_bytes[2];

void gpu_ring_usage(u64* upload, u64* local) {
    *upload = (u64)g_ring_bytes[1];
    *local = (u64)g_ring_bytes[0];
}

// Gives up a ring's buffer, once the GPU is done with it.
static void retire_buffer(GpuRing* r) {
    InterlockedAdd64(&g_ring_bytes[r->upload ? 1 : 0], -(s64)r->space.size);
    gpu_retire(r->buffer);
}

static bool create(GpuRing* r, u64 size) {
    D3D12_HEAP_PROPERTIES heap = {r->upload ? D3D12_HEAP_TYPE_UPLOAD : D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC1 desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = r->upload ? D3D12_RESOURCE_FLAG_NONE : D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ID3D12Resource* buffer = nullptr;
    u8* mapped = nullptr;
    HRESULT hr = gpu_device()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                        D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0,
                                                        nullptr, IID_PPV_ARGS(&buffer));
    if (SUCCEEDED(hr) && r->upload) {
        hr = buffer->Map(0, nullptr, (void**)&mapped);
        if (FAILED(hr))
            buffer->Release();
    }
    if (FAILED(hr)) {
        if (!gpu_device_lost(hr))
            LOG_E("gpu", "cannot create a %llu MiB GPU ring (0x%08lx)", size >> 20, (unsigned long)hr);
        return false;
    }
    gpu_set_name(buffer, "GPU ring (%llu MiB)", size >> 20);
    if (r->buffer)
        retire_buffer(r); // what was handed out from it is kept alive by its users' references
    InterlockedAdd64(&g_ring_bytes[r->upload ? 1 : 0], (s64)size);
    r->buffer = buffer;
    r->mapped = mapped;
    r->address = buffer->GetGPUVirtualAddress();
    ring_space_reset(&r->space, size);
    return true;
}

bool gpu_ring_alloc(GpuRing* r, u64 bytes, u64 alignment, u64* offset) {
    u64 least = ALIGN_UP(bytes * 2, MiB(1));
    if (!r->buffer && !create(r, RING_MIN_SIZE > least ? RING_MIN_SIZE : least))
        return false;
    ring_space_reclaim(&r->space, gpu_queue_completed(r->slot));
    // Give back what a burst of work grew the ring to (nothing is in use when it shrinks). A failed
    // create keeps the ring as it is.
    u64 smaller = ring_space_shrink_size(&r->space, RING_MIN_SIZE > least ? RING_MIN_SIZE : least);
    if (smaller && create(r, smaller))
        LOG_I("gpu", "a GPU ring shrinks to %llu MiB", smaller >> 20);
    if (ring_space_take(&r->space, bytes, alignment, offset))
        return true;
    // Waiting for the GPU to finish older regions could wait for work that waits for the caller's
    // thread to submit more: grow instead. The ring settles at the size the work in flight needs.
    u64 size = r->space.size * 2 > least ? r->space.size * 2 : least;
    if (size >= RING_LARGE_SIZE)
        LOG_W("gpu", "a GPU ring grows to %llu MiB: its queue is far behind", size >> 20);
    if (!create(r, size))
        return false;
    return ring_space_take(&r->space, bytes, alignment, offset);
}

void gpu_ring_submitted(GpuRing* r, u64 number) {
    ring_space_submitted(&r->space, number);
}

void gpu_ring_release(GpuRing* r) {
    if (r->buffer)
        retire_buffer(r);
    r->buffer = nullptr;
    r->mapped = nullptr;
    ring_space_reset(&r->space, 0);
}
