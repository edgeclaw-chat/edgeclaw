#include "src/tui/edc_chat_view.hpp"
#include "ftxui/component/component.hpp"
#include "ftxui/component/event.hpp"
#include "ftxui/component/screen_interactive.hpp"
#include "ftxui/dom/elements.hpp"
#include <ctime>
#include <memory>

using namespace ftxui;

int main()
{
    auto now = time(nullptr);

    auto msg_user = std::make_shared<edc::Message>("你好，这是一个用户消息。", edc::Message::Role::User, now);
    auto msg_assistant = std::make_shared<edc::Message>(
        "Hello! I am the assistant. Here is a longer response to demonstrate how paragraph wrapping works in the rendered output.",
        edc::Message::Role::Assistant,
        now);
    auto msg_tool = std::make_shared<edc::Message>(
        "ls -la\n-rw-r--r-- 1 user user 1024 Jan 01 12:00 file.txt\ndrwxr-xr-x 2 user user 4096 Jan 01 12:00 dir",
        edc::Message::Role::Tool,
        now);
    auto msg_system = std::make_shared<edc::Message>("System: connection established.", edc::Message::Role::System, now);

    auto container = Container::Vertical({msg_user, msg_assistant, msg_tool, msg_system});

    auto root = Renderer(container, [&] {
        return vbox({
            msg_user->Render(),
            msg_assistant->Render(),
            msg_tool->Render(),
            msg_system->Render(),
        });
    });

    root |= CatchEvent([&](Event e) -> bool {
        if (e == Event::Escape || e == Event::Character('q') || e == Event::CtrlC) {
            return true;
        }
        return false;
    });

    auto screen = ScreenInteractive::Fullscreen();
    screen.Loop(root);

    return 0;
}
