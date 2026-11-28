#include "gpu/gpu_ring.h"

#include "common/log.h"

#define RING_MIN_SIZE MiB(16)

// Bytes of all rings' buffers, current and retired (memory reports): upload heaps, default heaps.
static volatile s64 g_ring_bytes[2];

void gpu_ring_usage(u64* upload, u64* local) {
    *upload = (u64)g_ring_bytes[1];
    *local = (u64)g_ring_bytes[0];
}

// Gives up a ring's buffer, once the GPU is done with it.
static void retire_buffer(GpuRing* r) {
    InterlockedAdd64(&g_ring_bytes[r->upload ? 1 : 0], -(s64)r->size);
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
    r->size = size;
    r->address = buffer->GetGPUVirtualAddress();
    r->head = 0;
    r->first = r->count = 0;
    return true;
}

// Forgets the regions whose submission finished.
static void reclaim(GpuRing* r) {
    if (!r->count)
        return;
    u64 done = gpu_queue_completed(r->slot);
    while (r->count && r->uses[r->first].submission && r->uses[r->first].submission <= done) {
        r->first = (r->first + 1) % GPU_RING_MAX_USES;
        r->count--;
    }
}

// Where `bytes` fit between the head and the oldest region in use, or ~0.
static u64 place(const GpuRing* r, u64 bytes, u64 alignment) {
    if (!r->count)
        return bytes <= r->size ? 0 : ~0ull;
    if (r->count == GPU_RING_MAX_USES)
        return ~0ull;
    u64 tail = r->uses[r->first].start;
    u64 start = ALIGN_UP(r->head, alignment);
    if (tail < r->head) { // in use: [tail, head)
        if (start + bytes <= r->size)
            return start;
        return bytes <= tail ? 0 : ~0ull; // wrap around
    }
    return start + bytes <= tail ? start : ~0ull; // in use: [tail, end) and [0, head)
}

bool gpu_ring_alloc(GpuRing* r, u64 bytes, u64 alignment, u64* offset) {
    if (!r->buffer && !create(r, RING_MIN_SIZE > bytes * 2 ? RING_MIN_SIZE : ALIGN_UP(bytes * 2, MiB(1))))
        return false;
    reclaim(r);
    if (!r->count)
        r->head = 0;
    u64 start = place(r, bytes, alignment);
    // Wait for the oldest submitted region while that makes room; else grow.
    while (start == ~0ull && r->count && r->uses[r->first].submission) {
        gpu_queue_wait(r->slot, r->uses[r->first].submission);
        reclaim(r);
        if (!r->count)
            r->head = 0;
        start = place(r, bytes, alignment);
    }
    if (start == ~0ull) {
        u64 size = r->size * 2 > ALIGN_UP(bytes * 2, MiB(1)) ? r->size * 2 : ALIGN_UP(bytes * 2, MiB(1));
        if (!create(r, size))
            return false;
        start = 0;
    }
    u32 at = (r->first + r->count) % GPU_RING_MAX_USES;
    r->uses[at] = {start, start + bytes, 0};
    r->count++;
    r->head = start + bytes;
    *offset = start;
    return true;
}

void gpu_ring_submitted(GpuRing* r, u64 number) {
    for (u32 i = 0; i < r->count; i++) {
        GpuRing::Use& u = r->uses[(r->first + i) % GPU_RING_MAX_USES];
        if (!u.submission)
            u.submission = number;
    }
}

void gpu_ring_release(GpuRing* r) {
    if (r->buffer)
        retire_buffer(r);
    r->buffer = nullptr;
    r->mapped = nullptr;
    r->size = 0;
}
