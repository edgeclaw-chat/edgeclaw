#include "edc_log.h"
#include "edc_theme.hpp"
#include "edc_tui_window.hpp"
#include "ftxui/component/loop.hpp"
#include "ftxui/component/screen_interactive.hpp"
#include "uv.h"
#include <memory>
#include <signal.h>
#include <stdio.h>
#include <string>
#include <thread>

using namespace ftxui;

namespace
{

// Parse the sandbox flag into a per-run override: -1 = keep the persisted config.json
// value, 0 = force off, 1 = force on. "--no-sandbox"/"--sandbox=off|0|false|no" turn it
// off (for gdb/strace/perf and other tools the seccomp policy blocks) and
// "--sandbox"/"--sandbox=on" turn it on; unknown arguments are ignored. Never persisted:
// only the in-TUI /sandbox command writes config.json.
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

// ftxui treats SIGINT/SIGTERM as a *deferred* exit: its handler only records the signal,
// the loop quits normally, and ~Loop re-raises it after restoring the previous
// disposition. With the default disposition that raise() kills the process from inside
// screen.Loop(), so Loop() never returns and ~TUIWindow() -- the one place calling
// edc_app_free() -- is skipped, leaking every agent resource. Installing a benign
// handler *before* Loop() makes ftxui record it as "previous", so the re-raise lands here
// and control falls through to a normal return from main().
void HandleTerminationSignal(int)
{
}

// 关闭 uv_default_loop: 把 loop 上残留的句柄全部 uv_close, 跑完关闭回调后
// uv_loop_close, 释放 libuv 内部 (epoll / eventfd / io_uring) 的句柄与堆内存。
// 必须在 TUIWindow 析构 (edc_app_free) 之后调用。
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

    // Pick the palette before anything renders: the probe asks the terminal for
    // its background colour, which needs the tty still in cooked mode and
    // unclaimed (see edc_theme.hpp).
    edc::theme::Init();

    uv_loop_t *default_loop = uv_default_loop();
    /* 文件工具的 loop 由 edc_app_new() 用 runner 的 loop 装上（文件 I/O 都发生在
     * 那里）；这里从前那次"先用 default loop 预初始化"反正会被它覆盖，删掉。 */

    int exit_code = 0;
    {
        // TUI 对象放在显式作用域里: 作用域退出时 screen/window 的 shared_ptr
        // 一同释放 (~TUIWindow -> edc_app_free, 停 actor 线程 + 关句柄),
        // 之后才能安全地关日志与 default loop。
        auto screen = ScreenInteractive::Fullscreen();
        auto window = std::make_shared<edc::TUIWindow>(sandbox_override);

        if (!window->Ready())
        {
            // 配置损坏 / 主密钥不对 / 无终端输入口令等, edc_app_new() 已写日志。
            // 没有 agent 就什么都做不了, 直接非零退出, 不进 TUI 循环。
            fprintf(stderr, "edgeclaw: failed to start, see the log for details\n");
            exit_code = 1;
        }
        else
        {
            window->SetScreen(&screen);
            // ftxui's Loop::Run() sleeps ~16.6ms after every iteration, even the
            // ones that just handled input (~59 batches/s). Counting events lets
            // the loop below sleep only on an iteration that saw none.
            uint64_t events_seen = 0;
            ftxui::Component component = window;
            component |= CatchEvent([&](Event e) -> bool {
                events_seen++;
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
            // Ctrl-C (and `kill`) exit through ~TUIWindow() -> edc_app_free()
            // instead of terminating the process before teardown can run. Installed
            // here, before Loop() calls PreMain()/Install() and takes over SIGINT.
            ::signal(SIGINT, HandleTerminationSignal);
            ::signal(SIGTERM, HandleTerminationSignal);

            // Loop::Run() with one change: sleep only on an iteration that saw
            // no input, so a burst (a scroll flick is hundreds of wheel events)
            // drains back to back instead of one batch per 16.6ms.
            ftxui::Loop loop(&screen, component);
            while (!loop.HasQuitted())
            {
                const uint64_t before = events_seen;
                loop.RunOnce();
                window->TickAnimations();
                if (events_seen == before)
                    std::this_thread::sleep_for(std::chrono::milliseconds(16));
            }
        }
    }

    // Normal loop exit only (Esc / Ctrl-C / Ctrl-D): the TUI objects above are destroyed
    // here, running edc_app_free() before the process returns. Hand the signals back
    // to their default disposition so a second Ctrl-C during a slow teardown still works.
    ::signal(SIGINT, SIG_DFL);
    ::signal(SIGTERM, SIG_DFL);

    // All actor threads are stopped and every handle they owned is closed: safe
    // now to release the one remaining fd (the log file) and to tear down the
    // default loop itself.
    edc_log_close();
    CloseDefaultLoop(default_loop);
    return exit_code;
}
