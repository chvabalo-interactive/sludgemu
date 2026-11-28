#pragma once

#include "gpu/gpu.h"

// A ring of GPU memory for data one queue's submissions use once (detiled texture contents on their
// way into textures): regions are handed out in order and reused once the submission that used them
// finished. Not thread safe: one owner (the queue, under its own lock).

#define GPU_RING_MAX_USES 64

struct GpuRing {
    bool upload; // an upload heap buffer the CPU writes through `mapped`; else a default heap buffer for
                 // unordered access (set before the first gpu_ring_alloc)
    ID3D12Resource* buffer;
    u8* mapped;
    u64 size;
    u64 address; // the buffer's GPU address
    u64 head;    // where the next region starts
    u32 slot;    // the queue's submission tracking (gpu_queue_register)
    // Regions in use, oldest first: where each ends and the submission using it (0: not submitted
    // yet, the regions handed out since the last gpu_ring_submitted).
    struct Use {
        u64 start, end;
        u64 submission;
    } uses[GPU_RING_MAX_USES];
    u32 first, count;
};

// Hands out `bytes` (aligned to `alignment`), waiting for the GPU to finish older regions if the ring
// is full and growing it if they don't make room. Regions handed out before the call stay valid: a
// grown ring has a new buffer, the old one retired with gpu_retire (users hold a reference until their
// work is submitted). Fails, logged, without memory.
bool gpu_ring_alloc(GpuRing* ring, u64 bytes, u64 alignment, u64* offset);

// The regions handed out since the last call are used by submission `number` of the ring's queue.
void gpu_ring_submitted(GpuRing* ring, u64 number);

void gpu_ring_release(GpuRing* ring);

// Bytes of every ring's buffer (memory reports): upload heap (system memory) and default heap (VRAM).
void gpu_ring_usage(u64* upload, u64* local);
