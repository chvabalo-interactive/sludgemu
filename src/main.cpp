// sludgemu — runs Xbox Series X|S titles on Windows desktop.
//
//   sludgemu.exe [options] <package dir | title exe> [-- title arguments]
//
//   --console          open a console window with the live log
//   --log-level=LEVEL  trace | debug | info | warn | error   (default: info)
//   --break-on-stub    break into the debugger when an unimplemented import is called
//   --gpu-debug        enable the D3D12 debug layer (and device removal reports)
//   --gpu-dred         device removal reports only (DRED): where the GPU stopped, without the debug
//                      layer's cost; a GPU stall removes the device to report it
//   --gpu-serialize    wait for the GPU after every title submission (timing diagnostics)
//   --gpu-warp         render with the software rasterizer (WARP) instead of the GPU
//   --frame-stats      log presents per second and the longest frame interval, every second
//   --model=MODEL      console to present as: series-x or series-s (default: what the host fits)
//   --dump-threads-after=N   log every thread's backtrace every N seconds (hang diagnosis)
//   --watch-write=SPEC       log every write to an 8-byte location ("module.dll+0xRVA" or "0xADDR")
//   --trace-exec=SPEC[,...]  log registers when up to three code locations run
//                            (SLUDGE_TRACE_SKIP=N skips the first N hits, SLUDGE_TRACE_DUMP=BYTES
//                            also dumps memory at pointer registers)

#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>

#include "common/log.h"
#include "common/str.h"
#include "core/console_profile.h"
#include "core/cpu_compat.h"
#include "core/title.h"
#include "gpu/console_layout.h"
#include "gpu/gpu.h"
#include "hle/hle.h"
#include "hle/xmem/xmem.h"
#include "loader/crash.h"
#include "loader/loader.h"
#include "loader/stubs.h"
#include "platform/window.h"

struct Options {
    bool console;
    bool break_on_stub;
    bool gpu_debug;
    bool gpu_dred;
    bool gpu_serialize;
    bool gpu_warp;
    bool frame_stats;
    ConsoleModel model;
    bool model_given;
    u32 dump_threads_after;
    char watch_write[128];
    char trace_exec[256];
    LogLevel log_level;
    const wchar_t* title_path;
    wchar_t title_args[4096];
};

static void error_box(const wchar_t* fmt, const wchar_t* arg) {
    wchar_t msg[2048];
    _snwprintf(msg, ARRAY_COUNT(msg), fmt, arg);
    msg[ARRAY_COUNT(msg) - 1] = 0;
    char narrow[2048];
    str_to_narrow(narrow, sizeof(narrow), msg);
    LOG_E("main", "%s", narrow);
    MessageBoxW(nullptr, msg, L"sludgemu", MB_OK | MB_ICONERROR);
}

static bool parse_options(int argc, wchar_t** argv, Options* opt) {
    memset(opt, 0, sizeof(*opt));
    opt->log_level = LOG_LEVEL_INFO;
    int i = 1;
    for (; i < argc; i++) {
        const wchar_t* a = argv[i];
        if (wcscmp(a, L"--") == 0) {
            i++;
            break;
        }
        if (wcscmp(a, L"--console") == 0) {
            opt->console = true;
        } else if (wcsncmp(a, L"--model=", 8) == 0) {
            opt->model_given = true;
            if (!console_model_parse(a + 8, &opt->model)) {
                error_box(L"Unknown console model: %s\nUse series-x or series-s.", a + 8);
                return false;
            }
        } else if (wcscmp(a, L"--gpu-debug") == 0) {
            opt->gpu_debug = true;
        } else if (wcscmp(a, L"--gpu-dred") == 0) {
            opt->gpu_dred = true;

        } else if (wcscmp(a, L"--gpu-serialize") == 0) {
            opt->gpu_serialize = true;
        } else if (wcscmp(a, L"--gpu-warp") == 0) {
            opt->gpu_warp = true;
        } else if (wcscmp(a, L"--frame-stats") == 0) {
            opt->frame_stats = true;
        } else if (wcscmp(a, L"--break-on-stub") == 0) {
            opt->break_on_stub = true;
        } else if (wcsncmp(a, L"--dump-threads-after=", 21) == 0) {
            opt->dump_threads_after = (u32)_wtoi(a + 21);
        } else if (wcsncmp(a, L"--watch-write=", 14) == 0) {
            str_to_narrow(opt->watch_write, sizeof(opt->watch_write), a + 14);
        } else if (wcsncmp(a, L"--trace-exec=", 13) == 0) {
            str_to_narrow(opt->trace_exec, sizeof(opt->trace_exec), a + 13);
        } else if (wcsncmp(a, L"--log-level=", 12) == 0) {
            const wchar_t* l = a + 12;
            if (str_ieq_w(l, L"trace"))
                opt->log_level = LOG_LEVEL_TRACE;
            else if (str_ieq_w(l, L"debug"))
                opt->log_level = LOG_LEVEL_DEBUG;
            else if (str_ieq_w(l, L"info"))
                opt->log_level = LOG_LEVEL_INFO;
            else if (str_ieq_w(l, L"warn"))
                opt->log_level = LOG_LEVEL_WARN;
            else if (str_ieq_w(l, L"error"))
                opt->log_level = LOG_LEVEL_ERROR;
        } else if (a[0] == L'-' && a[1] == L'-') {
            error_box(L"Unknown option: %s", a);
            return false;
        } else if (!opt->title_path) {
            opt->title_path = a;
        } else {
            break; // first unknown positional starts the title's own arguments
        }
    }
    // Everything left goes to the title, re-quoted where needed.
    usize used = 0;
    for (; i < argc; i++) {
        bool quote = wcschr(argv[i], L' ') != nullptr;
        int n = _snwprintf(opt->title_args + used, ARRAY_COUNT(opt->title_args) - used,
                           quote ? L"%s\"%s\"" : L"%s%s", used ? L" " : L"", argv[i]);
        if (n < 0)
            break;
        used += (usize)n;
    }
    return true;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    Options opt;
    if (!parse_options(argc, argv, &opt))
        return 1;
    if (!opt.title_path) {
        MessageBoxW(
            nullptr,
            L"Usage: sludgemu.exe [--console] [--log-level=LEVEL] <package directory | title exe> [-- args]",
            L"sludgemu", MB_OK | MB_ICONINFORMATION);
        return 1;
    }

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    if (opt.console) {
        AllocConsole();
        SetConsoleTitleW(L"sludgemu log");
    }

    wchar_t self_path[MAX_PATH], self_dir[MAX_PATH], log_path[MAX_PATH], dump_path[MAX_PATH];
    GetModuleFileNameW(nullptr, self_path, MAX_PATH);
    path_dir_w(self_dir, MAX_PATH, self_path);
    _snwprintf(log_path, MAX_PATH, L"%s\\sludgemu.log", self_dir);
    _snwprintf(dump_path, MAX_PATH, L"%s\\sludgemu_crash.dmp", self_dir);
    log_init(log_path, opt.log_level);
    LOG_I("main", "sludgemu starting");

    TitleInfo& title = g_title;
    wchar_t error[1024];
    if (!title_resolve(opt.title_path, &title, error, ARRAY_COUNT(error))) {
        error_box(L"%s", error);
        return 1;
    }
    char narrow[MAX_PATH * 2];
    str_to_narrow(narrow, sizeof(narrow), title.exe_path);
    LOG_I("main", "title executable: %s", narrow);
    str_to_narrow(narrow, sizeof(narrow), title.display_name);
    LOG_I("main", "title: %s (title id %s)", narrow, title.title_id[0] ? title.title_id : "unknown");

    SetCurrentDirectoryW(title.dir);
    crash_install(dump_path);
    cpu_compat_init();
    stubs_init(opt.break_on_stub);
    console_profile_select(opt.model_given ? opt.model : console_model_for_host());
    LOG_I("main", "presenting as %s, title memory %llu MiB", console_profile().name,
          console_title_memory() >> 20);
    xmem_init();
    if (!console_layout_init(console_profile().model))
        return 1;
    hle_init();
    GpuOptions gpu_options = {};
    gpu_options.debug_layer = opt.gpu_debug;
    gpu_options.removal_reports = opt.gpu_debug || opt.gpu_dred;
    gpu_options.warp = opt.gpu_warp;
    gpu_options.serialize = opt.gpu_serialize;
    gpu_options.frame_stats = opt.frame_stats;
    gpu_set_options(gpu_options);
    window_init(title.display_name);

    // Before the title is mapped, so this helper thread never runs title thread-attach code.
    crash_start_thread_dump_timer(opt.dump_threads_after);
    // Loader-entry watches are armed before loading, to also catch writes during import binding.
    bool watch_early = strncmp(opt.watch_write, "ldrentry:", 9) == 0;
    if (watch_early)
        crash_arm_write_watch(opt.watch_write);

    g_loader.init(title.dir);
    if (!g_loader.load_main(title.exe_path)) {
        error_box(L"Failed to load the title executable. See the log for details:\n%s", log_path);
        return 1;
    }
    g_loader.set_command_line(opt.title_args);
    LocalFree(argv);
    if (opt.watch_write[0] && !watch_early)
        crash_arm_write_watch(opt.watch_write);
    if (opt.trace_exec[0])
        crash_arm_exec_trace(opt.trace_exec);

    g_loader.run_main();
}
