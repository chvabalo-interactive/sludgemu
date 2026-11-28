// GPU stall reports.
//
// Titles give up on a GPU that stops making progress (watchdogs that fire after tens of seconds),
// and by then nothing in the log says what the GPU was waiting for. A low-priority thread watches
// the queues once a second: when one has work it doesn't finish for STALL_SECONDS, it logs where
// every queue is, which title counters GPU work waits on, and every thread's backtrace.

#include "common/log.h"
#include "gpu/gpu.h"
#include "hle/d3d12/d3d12_objects.h"
#include "loader/crash.h"

#define STALL_SECONDS 10

struct SlotWatch {
    u64 completed; // as of the last look
    u32 stuck;     // seconds with work pending and nothing finished
};

static SlotWatch g_watch[GPU_MAX_QUEUES];
static bool g_reported = false; // for the current stall

static const char* queue_type_name(D3D12_COMMAND_LIST_TYPE type) {
    switch (type) {
    case D3D12_COMMAND_LIST_TYPE_DIRECT: return "direct";
    case D3D12_COMMAND_LIST_TYPE_COMPUTE: return "compute";
    case D3D12_COMMAND_LIST_TYPE_COPY: return "copy";
    default: return "other";
    }
}

static void report(u32 stuck_slot) {
    LOG_W("d3d12", "GPU stall: queue slot %u finished nothing for %u s; where the queues are:", stuck_slot,
          g_watch[stuck_slot].stuck);
    for (u32 s = 0; s < GPU_MAX_QUEUES; s++) {
        GpuQueueProgress p;
        if (!gpu_queue_progress(s, &p))
            continue;
        if (p.wait_value)
            LOG_W("d3d12",
                  "  slot %u (%s): numbered %llu, executed %llu, completed %llu; last wait for %llu (fence "
                  "at %llu) holds back %llu on",
                  s, queue_type_name(p.type), p.numbered, p.executed, p.completed, p.wait_value, p.wait_reached,
                  p.wait_before);
        else
            LOG_W("d3d12", "  slot %u (%s): numbered %llu, executed %llu, completed %llu", s,
                  queue_type_name(p.type), p.numbered, p.executed, p.completed);
    }
    console_counter_report();
    crash_dump_all_threads("GPU stall");
}

static DWORD WINAPI stall_watch_thread(void*) {
    for (;;) {
        Sleep(1000);
        u32 worst = GPU_NO_QUEUE;
        bool any_stuck = false;
        for (u32 s = 0; s < GPU_MAX_QUEUES; s++) {
            SlotWatch& w = g_watch[s];
            GpuQueueProgress p;
            bool used = gpu_queue_progress(s, &p);
            if (!used || p.completed >= p.numbered || p.completed != w.completed) {
                if (w.stuck >= STALL_SECONDS && g_reported)
                    LOG_I("d3d12", "GPU stall: queue slot %u moves again after %u s", s, w.stuck);
                w.completed = used ? p.completed : 0;
                w.stuck = 0;
                continue;
            }
            w.stuck++;
            if (w.stuck >= STALL_SECONDS) {
                any_stuck = true;
                if (worst == GPU_NO_QUEUE || w.stuck > g_watch[worst].stuck)
                    worst = s;
            }
        }
        if (any_stuck && !g_reported) {
            report(worst);
            g_reported = true;
        } else if (!any_stuck) {
            g_reported = false;
        }
    }
}

void console_stall_watch_start() {
    HANDLE t = CreateThread(nullptr, 0, stall_watch_thread, nullptr, 0, nullptr);
    if (!t) {
        LOG_W("d3d12", "no GPU stall watch (%lu)", GetLastError());
        return;
    }
    SetThreadPriority(t, THREAD_PRIORITY_BELOW_NORMAL);
    SetThreadDescription(t, L"sludgemu GPU stall watch");
    CloseHandle(t);
}
