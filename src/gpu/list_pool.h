#pragma once

#include "gpu/gpu.h"
#include "gpu/reuse_order.h"

// Desktop command lists of our own that one queue runs once per recording (texture uploads, fence
// value writes, page table work), each with its allocator. A recording takes the oldest list whose last
// submission finished, or a new one when none has: recording never waits for the GPU (see
// gpu/reuse_order.h). Not thread safe: one owner, under its own lock.

struct GpuPooledList {
    ID3D12CommandAllocator* allocator;
    ID3D12GraphicsCommandList8* list;
};

struct GpuListPool {
    D3D12_COMMAND_LIST_TYPE type; // set before the first use
    const char* name;             // for the debug layer and logs
    ReuseOrder order;
    GpuPooledList* entries; // indexed like `order`
    u32 entries_capacity;
};

// A list recording on a reset allocator, or nullptr (logged). `completed` is the last submission of the
// pool's queue the GPU finished. Returns the entry for gpu_list_pool_submitted in `*entry`.
ID3D12GraphicsCommandList8* gpu_list_pool_begin(GpuListPool* pool, u64 completed, u32* entry);

// The list of `entry` (closed by the caller) went to the queue as submission `number`. A list that
// isn't submitted is taken again by the next begin.
void gpu_list_pool_submitted(GpuListPool* pool, u32 entry, u64 number);

// Retires every list (gpu_retire) and frees the pool.
void gpu_list_pool_release(GpuListPool* pool);
