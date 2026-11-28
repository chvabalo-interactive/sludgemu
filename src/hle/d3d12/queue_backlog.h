#pragma once

#include <windows.h>
#include <d3d12.h>

#include "common/types.h"
#include "gpu/presenter.h"

// Work a console queue holds back from its desktop queue.
//
// Console queues never block the thread that submits to them, and titles rely on it: GPU work may wait
// for work the same thread submits afterwards. Desktop D3D12 can't run a command list again before its
// last run finished, so a console list submitted again while it still runs is held back here, and so is
// everything submitted to the queue after it, to keep the queue's order. So is a fence wait nothing
// queued ends yet (gpu_fence_wait_covered): only the CPU or work submitted later can, and the desktop
// queue must not hold such waits. The held-back work reaches the desktop queue, in order, as soon as
// what it waits for is there: on the next call into the queue or from a thread pool wait, whichever
// comes first.

enum BacklogKind : u8 {
    BACKLOG_EXECUTE, // lists (or none: just counted), then the queue's submission count
    BACKLOG_SIGNAL,  // desktop fence signal
    BACKLOG_WAIT,    // desktop fence wait (held while not covered)
    BACKLOG_PRESENT, // a console present
};

#define BACKLOG_LISTS 64 // lists per execute (longer submissions are split, which keeps their order)

struct BacklogOp {
    BacklogKind kind;
    u32 count; // EXECUTE: lists; PRESENT: planes
    // EXECUTE: the lists run once submission `after` of queue slot `after_slot` finished (GPU_NO_QUEUE:
    // no such run); `after_generation` tells whether the slot still holds that queue.
    u32 after_slot;
    u32 after_generation;
    u64 after;
    u64 number;         // EXECUTE: the queue submission reserved for it (gpu_queue_reserve)
    ID3D12Fence* fence; // SIGNAL / WAIT: referenced
    u64 value;
    u64 token;                               // PRESENT: the console frame token
    ID3D12CommandList* lists[BACKLOG_LISTS]; // EXECUTE: referenced
    PresentPlane planes[PRESENT_MAX_PLANES]; // PRESENT: textures referenced
};

struct QueueBacklog {
    BacklogOp* ops; // circular from `first`
    u32 first;
    u32 count;
    u32 capacity;
    HANDLE event;             // set once the run the first op waits for finished (made on first use)
    HANDLE wait;              // thread pool wait on `event`
    ID3D12Fence* armed_fence; // what `event` was last set to follow (nullptr: nothing)
    u64 armed_value;
};

// A new op at the end, zeroed; nullptr without memory.
BacklogOp* backlog_push(QueueBacklog* b);

inline BacklogOp* backlog_front(QueueBacklog* b) {
    return b->count ? &b->ops[b->first] : nullptr;
}

void backlog_pop(QueueBacklog* b);

// Frees the op storage (the ops must have been run or released).
void backlog_free(QueueBacklog* b);
