#include "ftxui/component/screen_interactive.hpp"
#include "tui/edc_tui_window.hpp"
#include <memory>

using namespace ftxui;

int main()
{
    ftxui::Component window = std::make_shared<edc::TUIWindow>();

    auto screen = ScreenInteractive::Fullscreen();

    window |= CatchEvent([&](Event e) -> bool {
        if (e == Event::Escape || e == Event::Character('q') || e == Event::CtrlC)
        {
            screen.Exit();
            return true;
        }
        return false;
    });

    screen.Loop(window);
}
