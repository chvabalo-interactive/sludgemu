#include "gpu/list_pool.h"

#include <stdlib.h>
#include <string.h>

#include "common/log.h"

// More lists in flight than this means the GPU is far behind the queue (or blocked on later work).
#define POOL_LARGE 16

ID3D12GraphicsCommandList8* gpu_list_pool_begin(GpuListPool* pool, u64 completed, u32* entry) {
    bool created = false;
    u32 e = reuse_take(&pool->order, completed, &created);
    if (e != ~0u && reuse_capacity(&pool->order) > pool->entries_capacity) {
        u32 capacity = reuse_capacity(&pool->order);
        GpuPooledList* grown = (GpuPooledList*)realloc(pool->entries, capacity * sizeof(GpuPooledList));
        if (grown) {
            memset(grown + pool->entries_capacity, 0,
                   (capacity - pool->entries_capacity) * sizeof(GpuPooledList));
            pool->entries = grown;
            pool->entries_capacity = capacity;
        } else {
            e = ~0u;
        }
    }
    if (e == ~0u) {
        LOG_E("gpu", "%s: out of memory for another command list", pool->name);
        return nullptr;
    }
    if (created && pool->order.count >= POOL_LARGE && !(pool->order.count & (pool->order.count - 1)))
        LOG_W("gpu", "%s: %u lists in flight; the queue is far behind", pool->name, pool->order.count);

    GpuPooledList& l = pool->entries[e];
    HRESULT hr;
    if (!l.list) { // new, or its creation failed before
        ID3D12Device13* device = gpu_device();
        hr = l.allocator ? S_OK : device->CreateCommandAllocator(pool->type, IID_PPV_ARGS(&l.allocator));
        if (SUCCEEDED(hr))
            hr = device->CreateCommandList(0, pool->type, l.allocator, nullptr, IID_PPV_ARGS(&l.list));
        if (SUCCEEDED(hr))
            gpu_set_name(l.list, "%s %u", pool->name, e);
    } else {
        hr = l.allocator->Reset(); // the GPU finished its last submission (reuse_take)
        if (SUCCEEDED(hr))
            hr = l.list->Reset(l.allocator, nullptr);
    }
    if (FAILED(hr)) {
        if (!gpu_device_lost(hr))
            LOG_E("gpu", "%s: cannot record (0x%08lx)", pool->name, (unsigned long)hr);
        return nullptr;
    }
    *entry = e;
    return l.list;
}

void gpu_list_pool_submitted(GpuListPool* pool, u32 entry, u64 number) {
    reuse_submitted(&pool->order, entry, number);
}

void gpu_list_pool_release(GpuListPool* pool) {
    for (u32 i = 0; i < pool->order.count; i++) {
        gpu_retire(pool->entries[i].list);
        gpu_retire(pool->entries[i].allocator);
    }
    free(pool->entries);
    pool->entries = nullptr;
    pool->entries_capacity = 0;
    reuse_free(&pool->order);
}
