#include "edc_log.h"
#include "edc_theme.hpp"
#include "edc_tui_window.hpp"
#include "edc_state_store.h"
#include "edc_update_cli.h"
#include "edc_version.h"
#include "ftxui/component/loop.hpp"
#include "ftxui/component/screen_interactive.hpp"
#include "uv.h"
#include <memory>
#include <signal.h>
#include <stdio.h>
#include <string.h>
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
    /* Before anything else -- no terminal setup, no config, no passphrase. A version
     * question must be answerable by a binary that cannot start (wrong master key, no
     * tty, broken database): it is the first thing a bug report needs, and it must not
     * fail for the same reason the user is reporting.
     *
     * `--help` is answered here for the same reason, and one more: without it the first
     * thing a new user typed (`edgeclaw --help`) fell through to the TUI, which asked for
     * a passphrase. A program that answers "--help" with "Enter new passphrase" has not
     * told anyone anything. */
    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--version") == 0)
        {
            edc_version_print(EDC_DB_VERSION, stdout);
            return 0;
        }
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)
        {
            edc_usage_print(argc > 0 ? argv[0] : "edgeclaw", stdout);
            return 0;
        }
        if (strcmp(argv[i], "--update-check") == 0)
        {
            /* Answered before the terminal, the config and the passphrase, for the same reason
             * --version is: it is a diagnostic, and a diagnostic that needs a working setup to
             * run cannot diagnose a setup that does not work. The channel is the only option it
             * takes, because everything else it needs comes from the environment, not the
             * user's config. */
            bool allow_pre = false;
            const char *current_override = nullptr;
            for (int k = 1; k < argc; k++)
            {
                if (strcmp(argv[k], "--pre") == 0 || strcmp(argv[k], "-Pre") == 0)
                    allow_pre = true;
                /* --current <ver>: compare against this instead of the build's own version.
                 * The only way to exercise the network path on a development build, whose
                 * version is a commit hash and therefore skipped. */
                else if ((strcmp(argv[k], "--current") == 0 || strcmp(argv[k], "-Current") == 0) && k + 1 < argc)
                    current_override = argv[k + 1];
            }
            return edc_update_check_cli(allow_pre, current_override);
        }
    }


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
    ::SetConsoleCP(CP_UTF8); // keys arrive as UTF-8 *bytes* under ConPTY: see patches/ftxui-conpty-input.patch
#endif

    const int sandbox_override = ParseSandboxFlag(argc, argv);

    edc_log_init();

    // Settle the colour depth first: ftxui::Color::RGB() quantises to whatever
    // depth is in force when the colour is built, and the palettes are built
    // lazily on first use. Left to ftxui, a Windows session comes up 256-colour
    // and every surface step in the palette lands on the same grey rung (see
    // edc_theme.hpp).
    edc::theme::InitColorSupport();

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
            /* Bad config / wrong master key / no terminal for the passphrase:
             * edc_app_new() already logged it. With no app nothing can be done, so
             * exit non-zero without entering the TUI loop.
             *
             * The message names the cause it can know, gives both ways forward, and names
             * the log file rather than saying "the log" -- the previous wording
             * ("failed to start, see the log for details") left a user who could not start
             * the program to work out where its logs even go. */
            const char *log = edc_log_path();
            fprintf(stderr,
                    "edgeclaw: failed to start.\n"
                    "A fresh install has to be configured once interactively (run "
                    "`edgeclaw` in a terminal); without one, the master key must come "
                    "from EDGECLAW_MASTER_KEY.\n");
            if (log && log[0])
                fprintf(stderr, "The reason is in %s\n", log);
            else
                fprintf(stderr, "The reason was logged, but the log file could not be opened.\n");
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
