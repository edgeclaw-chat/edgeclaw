#include "ftxui/component/screen_interactive.hpp"
#include "edc_tui_window.hpp"
#include <memory>

using namespace ftxui;

int main()
{
    ftxui::Component window = std::make_shared<edc::TUIWindow>();

    auto screen = ScreenInteractive::Fullscreen();

    window |= CatchEvent([&](Event e) -> bool {
        // No 'q' here: the window now has a text input, so a bare letter must
        // reach the box instead of quitting.
        if (e == Event::Escape || e == Event::CtrlC)
        {
            screen.Exit();
            return true;
        }
        return false;
    });

    screen.Loop(window);
}
