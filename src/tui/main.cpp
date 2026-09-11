#include "edc_log.h"
#include "edc_tui_window.hpp"
#include "ftxui/component/screen_interactive.hpp"
#include "uv.h"
#include <memory>
#include <signal.h>
#include <stdio.h>
#include <string>

using namespace ftxui;

namespace
{

// Parse the sandbox flag from argv into a per-run override: -1 = no flag (keep
// the persisted config.json value), 0 = force off, 1 = force on. The sandbox is
// on by default in config.json; "--no-sandbox" / "--sandbox=off|0|false|no"
// force it off (for gdb/strace/perf/valgrind and other tools the seccomp policy
// blocks), while "--sandbox" / "--sandbox=on" force it on. Unknown arguments
// are ignored. The override is never persisted -- only the in-TUI /sandbox
// command writes config.json.
int ParseSandboxFlag(int argc, char **argv)
{
    int sandbox_override = -1;
    for (int i = 1; i < argc; i++)
    {
        const std::string arg = argv[i];
        if (arg == "--no-sandbox")
            sandbox_override = 0;
        else if (arg == "--sandbox")
            sandbox_override = 1;
        else if (arg.rfind("--sandbox=", 0) == 0)
        {
            const std::string v = arg.substr(10);
            sandbox_override = (v == "off" || v == "0" || v == "false" || v == "no") ? 0 : 1;
        }
    }
    return sandbox_override;
}

// ftxui treats SIGINT/SIGTERM as a *deferred* exit: its signal handler only
// records the signal, the main loop then quits normally, and ~Loop re-raises
// the signal after restoring whichever disposition was installed before ftxui
// took over (see ftxui App::Internal::PostMain -> RestoreSignalHandlerAndRaise).
// With the default disposition that final raise() kills the process from
// inside screen.Loop(), so screen.Loop() never returns and the local `window`
// shared_ptr is never destroyed -- ~TUIWindow(), the one place that calls
// edc_c_agent_free(), is skipped. Every agent resource (uv loop, worker
// threads, sockets, in-flight HTTPS buffers) leaks and the process dies by
// signal instead of exiting cleanly (valgrind then reports the whole live heap
// as still-reachable/possibly-lost).
//
// Install a benign handler *before* screen.Loop() so ftxui records it as the
// "previous" disposition and restores it right before its post-loop raise():
// the re-raise now lands here, returns immediately, and control falls through
// to a normal return from main() with full teardown.
void HandleTerminationSignal(int) {}

// 关闭 uv_default_loop: 把 loop 上残留的句柄全部 uv_close, 跑完关闭回调后
// uv_loop_close, 释放 libuv 内部 (epoll / eventfd / io_uring) 的句柄与堆内存。
// 必须在 TUIWindow 析构 (edc_c_agent_free) 之后调用。
void CloseDefaultLoop(uv_loop_t *loop)
{
    uv_walk(
        loop,
        [](uv_handle_t *handle, void *) {
            if (!uv_is_closing(handle))
                uv_close(handle, nullptr);
        },
        nullptr);
    uv_run(loop, UV_RUN_DEFAULT);
    uv_loop_close(loop);
}

} // namespace

int main(int argc, char **argv)
{

    const int sandbox_override = ParseSandboxFlag(argc, argv);

    edc_log_init();

    uv_loop_t *default_loop = uv_default_loop();
    edc_c_file_utils_init(default_loop);

    int exit_code = 0;
    {
        // TUI 对象放在显式作用域里: 作用域退出时 screen/window 的 shared_ptr
        // 一同释放 (~TUIWindow -> edc_c_agent_free, 停 actor 线程 + 关句柄),
        // 之后才能安全地关日志与 default loop。
        auto screen = ScreenInteractive::Fullscreen();
        auto window = std::make_shared<edc::TUIWindow>(sandbox_override);

        if (!window->Ready())
        {
            // 配置损坏 / 主密钥不对 / 无终端输入口令等, edc_agent_new() 已写日志。
            // 没有 agent 就什么都做不了, 直接非零退出, 不进 TUI 循环。
            fprintf(stderr, "edgeclaw: failed to start, see the log for details\n");
            exit_code = 1;
        }
        else
        {
            edc_c_agent_set_command_handler();
            window->SetScreen(&screen);
            ftxui::Component component = window;
            component |= CatchEvent([&](Event e) -> bool {
                // No 'q' here: the window now has a text input, so a bare letter must
                // reach the box instead of quitting.
                if (e == Event::Escape || e == Event::CtrlC)
                {
                    screen.Exit();
                    return true;
                }
                return false;
            });

            // See HandleTerminationSignal(): swallow ftxui's post-loop re-raise so
            // Ctrl-C (and `kill`) exit through ~TUIWindow() -> edc_c_agent_free()
            // instead of terminating the process before teardown can run. Installed
            // here, before Loop() calls PreMain()/Install() and takes over SIGINT.
            ::signal(SIGINT, HandleTerminationSignal);
            ::signal(SIGTERM, HandleTerminationSignal);

            screen.Loop(component);
        }
    }

    // Reached only after a normal loop exit (Esc / Ctrl-C / Ctrl-D), now that
    // the re-raise above is neutralised: the TUI objects above are destroyed
    // here, running edc_c_agent_free() (uv walk + thread joins) before the
    // process returns. ftxui's deferred-exit re-raise has already happened
    // inside Loop(), so from here on hand the signals back to their default
    // disposition: a second Ctrl-C / kill during a slow teardown must still be
    // able to terminate us.
    ::signal(SIGINT, SIG_DFL);
    ::signal(SIGTERM, SIG_DFL);

    // All actor threads are stopped and every handle they owned is closed: safe
    // now to release the one remaining fd (the log file) and to tear down the
    // default loop itself.
    edc_log_close();
    CloseDefaultLoop(default_loop);
    return exit_code;
}
