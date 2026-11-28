// GPU stall reports.
//
// Titles give up on a GPU that stops making progress (watchdogs that fire after tens of seconds),
// and by then nothing in the log says what the GPU was waiting for. A low-priority thread watches
// the queues once a second: when one has work it doesn't finish for STALL_SECONDS, it logs where
// every queue is, the fence wait holding each one and what would signal it, and every thread's
// backtrace.

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

static void report(u32 stuck_slot) {
    LOG_W("d3d12", "GPU stall: queue slot %u finished nothing for %u s; where the queues are:", stuck_slot,
          g_watch[stuck_slot].stuck);
    gpu_queue_report_stall(console_fence_name);
    console_queues_report_backlogs(console_fence_name);
    crash_dump_all_threads("GPU stall");
    gpu_remove_device("GPU stall");
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
