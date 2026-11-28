#pragma once

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include "common/types.h"

// The desktop D3D12 device everything is translated to (Agility SDK runtime).

struct GpuOptions {
    bool debug_layer;     // D3D12 debug layer + GPU-based validation off (too slow for a title)
    bool removal_reports; // DRED: where the GPU stopped when the device goes (implied by debug_layer)
    bool warp;            // use the software rasterizer instead of the GPU
    bool serialize;       // every title submission waits for the GPU to finish it (diagnostics)
    bool frame_stats;     // log presents per second and the longest frame interval (diagnostics)
};

void gpu_set_options(const GpuOptions& options);
const GpuOptions& gpu_options();

// Creates the desktop device on first use. Returns nullptr (and logs why) if no suitable
// adapter exists.
ID3D12Device13* gpu_device();
IDXGIFactory6* gpu_factory();
// Whether the device does 64-bit atomics on typed views in the descriptor heap (after gpu_device()).
bool gpu_int64_image_atomics();

// The adapter's memory as the OS accounts it to this process: `local` is the GPU's own (VRAM),
// `shared` system memory it uses. Budgets are what the OS lets the process keep resident.
struct GpuVideoMemory {
    u64 local_usage;
    u64 local_budget;
    u64 shared_usage;
    u64 shared_budget;
};
bool gpu_video_memory(GpuVideoMemory* out);

// True if `hr` says the device is lost. The first time, logs why (DRED details with the debug
// layer). The removal is also reported on its own once the runtime notices it.
bool gpu_device_lost(HRESULT hr);
// With removal reports: removes the device on purpose, so DRED reports where every queue stopped (the
// markers of the operations around it included). Without them, logs that they would. For GPU work
// that never finishes, which the driver keeps running when it can preempt it.
void gpu_remove_device(const char* why);

// Command queues running translated work. Registering gives a queue a slot whose fence counts its
// submissions; call gpu_queue_submitted after every ExecuteCommandLists. Unregistering waits for
// the queue to finish its work.
#define GPU_NO_QUEUE 0xffffffffu
u32 gpu_queue_register(ID3D12CommandQueue* queue);
void gpu_queue_unregister(u32 slot);
u64 gpu_queue_submitted(u32 slot); // returns the submission's number
// Executes `lists` (none: a submission of nothing) on `queue`, registered in `slot`, and counts the
// submission in one step, so a retire point taken meanwhile includes work already handed to the queue.
// Returns the submission's number (0 for a queue without a slot, whose work runs uncounted).
u64 gpu_queue_execute(ID3D12CommandQueue* queue, u32 slot, u32 count, ID3D12CommandList* const* lists);

// Work held back from its queue (see the console queue backlog) is numbered when it is held back:
// gpu_queue_reserve hands out the slot's next number without submitting, and gpu_queue_execute_reserved
// later executes the lists (none: nothing but the count) and signals that number. Reserved numbers must
// reach the queue in order, and nothing else may be counted on the slot meanwhile: a later number
// signaled first would complete the reserved ones.
u64 gpu_queue_reserve(u32 slot);
void gpu_queue_execute_reserved(ID3D12CommandQueue* queue, u32 slot, u64 number, u32 count,
                                ID3D12CommandList* const* lists);

// The number of the last submission the queue finished.
u64 gpu_queue_completed(u32 slot);

// Which queue holds the slot: counts the queues that held it.
u32 gpu_queue_generation(u32 slot);

// Whether the queue that held the slot as `generation` finished submission `number` (or left the slot,
// which drains its work). Doesn't wait.
bool gpu_queue_reached(u32 slot, u32 generation, u64 number);

// Blocks until the queue finished submission `number`. Never from a title's submission paths: GPU work
// may wait for work that thread submits later.
void gpu_queue_wait(u32 slot, u64 number);

// The fence counting the queue's submissions, for other queues to wait on (alive as long as the slot).
ID3D12Fence* gpu_queue_fence(u32 slot);

// Fence waits and signals on a queue, in order with its submissions. A registered queue remembers
// its recent ones for stall reports (gpu_queue_report_stall).
void gpu_queue_wait_on(ID3D12CommandQueue* queue, ID3D12Fence* fence, u64 value);
void gpu_queue_signal_on(ID3D12CommandQueue* queue, ID3D12Fence* fence, u64 value);

// Whether a wait for `fence` to reach `value` ends without work not submitted yet: the fence is there,
// or a signal handed to a desktop queue takes it there. Waits that aren't covered must not reach a
// desktop queue: the video memory manager may wait for queued work (making memory resident, for one)
// on the very thread that would submit what ends the wait.
bool gpu_fence_wait_covered(ID3D12Fence* fence, u64 value);

// How far a queue got: submissions numbered (held-back ones included), handed to the desktop queue,
// and finished by the GPU. False for a free slot.
struct GpuQueueProgress {
    u64 numbered;
    u64 executed;
    u64 completed;
};
bool gpu_queue_progress(u32 slot, GpuQueueProgress* out);

// Logs where every queue is and what holds it: the fence wait in front of the submission it is
// stuck at, and which queued signal (if any) would release it. `name` describes a fence (a title
// counter, say) into `out`; nullptr names fences by address.
typedef void (*GpuFenceNamer)(ID3D12Fence* fence, char* out, u32 size);
void gpu_queue_report_stall(GpuFenceNamer name);

// Names a desktop object for the debug layer's messages (printf-style); nothing without the layer.
void gpu_set_name(ID3D12Object* object, const char* format, ...);

// Releases a desktop object once no queue can still be using it: after every queue finished what
// it had been given when the object was retired.
void gpu_retire(IUnknown* object);

// The same point for memory of our own (tiles, see gpu/tile_pool.h): taken when something stops being
// used, reached once every queue finished what it had been given by then.
#define GPU_MAX_QUEUES 16
struct GpuRetirePoint {
    u64 after[GPU_MAX_QUEUES];      // per queue slot: the submission that must complete first
    u32 generation[GPU_MAX_QUEUES]; // which queue held the slot (a queue leaving drains its work)
};
void gpu_retire_point(GpuRetirePoint* out);
bool gpu_retire_point_reached(const GpuRetirePoint& point);

// Waits until every queue finished all submitted work.
void gpu_wait_idle();
