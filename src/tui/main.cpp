#include "edc_log.h"
#include "edc_tui_window.hpp"
#include "ftxui/component/screen_interactive.hpp"
#include "uv.h"
#include <memory>
#include <signal.h>

// Referenced by edc_commands.c (/exit etc.); the shell handler sets it to ask
// the host binary to stop its run loop.
volatile sig_atomic_t _run_loop = 1;

using namespace ftxui;

int main()
{

    edc_log_init();

    uv_loop_t *default_loop = uv_default_loop();
    edc_c_file_utils_init(default_loop);

    auto screen = ScreenInteractive::Fullscreen();
    auto window = std::make_shared<edc::TUIWindow>();
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

    screen.Loop(component);
}
