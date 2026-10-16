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

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using namespace ftxui;

namespace
{

// Parse the sandbox flag into a per-run override: -1 = keep the persisted
// config.json value, 0 = force off, 1 = force on. "--no-sandbox"/"--sandbox=off|0|false|no"
// turn it off (for gdb/strace/perf, which the seccomp policy blocks) and
// "--sandbox"/"--sandbox=on" turn it on; unknown arguments are ignored. Never
// persisted: only the in-TUI /sandbox command writes config.json.
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

// ftxui treats SIGINT/SIGTERM as a *deferred* exit: its handler only records the
// signal, the loop quits normally, and ~Loop re-raises it. With the default
// disposition that raise() kills the process from inside screen.Loop(), so Loop()
// never returns and ~TUIWindow() -- the one place calling edc_app_free() -- is
// skipped, leaking every app resource. Installing this benign handler *before*
// Loop() makes ftxui record it as "previous", so the re-raise lands here and
// control falls through to a normal return from main().
void HandleTerminationSignal(int)
{
}

// Close uv_default_loop: uv_close every remaining handle, run the close callbacks,
// then uv_loop_close, releasing libuv's internal handles and heap. Must be called
// after ~TUIWindow() (edc_app_free).
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

#ifdef _WIN32
    // FTXUI renders each frame to a UTF-8 std::string and writes it as raw
    // bytes (App::Internal::TerminalSend does `std::cout << output_buffer`).
    // The Windows console, however, decodes those bytes using its *output code
    // page*, which defaults to the OEM/ANSI page (e.g. 936/GBK on a Chinese
    // system, 437/850 elsewhere). Every non-ASCII glyph -- Chinese above all --
    // is therefore misinterpreted and shows up as mojibake.
    //
    // Switch the console to UTF-8 (65001) before anything renders so the bytes
    // on stdout are interpreted the way FTXUI produced them. Must run before
    // theme::Init()/ScreenInteractive, both of which already write to stdout.
    ::SetConsoleOutputCP(CP_UTF8);
    ::SetConsoleCP(CP_UTF8); // also decode typed/pasted UTF-8 input correctly
#endif

    const int sandbox_override = ParseSandboxFlag(argc, argv);

    edc_log_init();

    // Pick the palette before anything renders: the probe asks the terminal for its
    // background colour, which needs the tty still in cooked mode and unclaimed.
    edc::theme::Init();

    uv_loop_t *default_loop = uv_default_loop();

    int exit_code = 0;
    {
        // TUI objects in an explicit scope: on exit their shared_ptrs are released
        // (~TUIWindow -> edc_app_free stops the actor threads and closes handles),
        // only after which are the log and default loop safe to close.
        auto screen = ScreenInteractive::Fullscreen();
        auto window = std::make_shared<edc::TUIWindow>(sandbox_override);

        if (!window->Ready())
        {
            // Bad config / wrong master key / no terminal for the passphrase:
            // edc_app_new() already logged it. With no app nothing can be done, so
            // exit non-zero without entering the TUI loop.
            fprintf(stderr, "edgeclaw: failed to start, see the log for details\n");
            exit_code = 1;
        }
        else
        {
            window->SetScreen(&screen);
            // ftxui's Ctrl-C default is "quit, even if the component handled the event"
            // (force_handle_ctrl_c_ defaults to true). We handle it ourselves in
            // window->OnInterrupt() -- first press clears the input box, a second one in
            // a row quits -- so tell ftxui to stay out of the way: it now only quits for
            // a Ctrl-C nobody handled.
            screen.ForceHandleCtrlC(false);
            // ftxui's Loop::Run() sleeps ~16.6ms after every iteration, even the
            // ones that just handled input (~59 batches/s). Counting events lets
            // the loop below sleep only on an iteration that saw none.
            uint64_t events_seen = 0;
            ftxui::Component component = window;
            component |= CatchEvent([&](Event e) -> bool {
                events_seen++;
                // No 'q' (a bare letter must reach the text input) and no Escape
                // (it belongs to the innermost component: the /connect dialog and
                // the history view). Ctrl-C is routed to the window: a lone press
                // clears the input box, a second one in a row quits.
                if (e == Event::CtrlC)
                {
                    if (window->OnInterrupt())
                        screen.Exit();
                    return true;
                }
                return false;
            });

            // See HandleTerminationSignal(): swallow ftxui's post-loop re-raise so
            // Ctrl-C (and `kill`) exit through ~TUIWindow() -> edc_app_free()
            // instead of terminating before teardown. Installed before Loop()
            // takes over SIGINT.
            ::signal(SIGINT, HandleTerminationSignal);
            ::signal(SIGTERM, HandleTerminationSignal);

            // Loop::Run() with one change: sleep only on an iteration that saw no
            // input, so a burst (a scroll flick is hundreds of wheel events) drains
            // back to back instead of one batch per 16.6ms.
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

    // Normal loop exit only: the TUI objects above are destroyed here, running
    // edc_app_free() before the process returns. Hand the signals back to their
    // default disposition so a second Ctrl-C during a slow teardown still works.
    ::signal(SIGINT, SIG_DFL);
    ::signal(SIGTERM, SIG_DFL);

    // All actor threads are stopped and their handles closed: safe now to release
    // the log file and tear down the default loop.
    edc_log_close();
    CloseDefaultLoop(default_loop);
    return exit_code;
}
