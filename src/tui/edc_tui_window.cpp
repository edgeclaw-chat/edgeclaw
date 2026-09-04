#include "edc_tui_window.hpp"
#include "edc_chat_widget.hpp"
#include "ftxui/component/event.hpp"
#include <memory>

namespace edc
{
TUIWindow::TUIWindow()
{
    chat_widget_ = std::make_shared<ChatWidget>();
    footer_ = ::ftxui::Renderer([]() {
        return ::ftxui::text(" 滚轮/↑↓/PgUp/PgDn/Home/End 滚动   点击 Thought 展开   q 退出 ") |
               ::ftxui::color(::ftxui::Color::RGB(147, 147, 147));
    });
}

::ftxui::Element TUIWindow::OnRender()
{
    return ::ftxui::vbox({chat_widget_->Render() | ftxui::flex, ftxui::separatorHeavy(), footer_->Render()});
}

bool TUIWindow::OnEvent(::ftxui::Event event)
{
    return chat_widget_->OnEvent(event);
}

TUIWindow::~TUIWindow()
{
}
} // namespace edc
