#include "edc_tui_window.hpp"
#include "edc_chat_widget.hpp"
#include "ftxui/component/component.hpp"
#include "ftxui/component/component_options.hpp"
#include "ftxui/component/event.hpp"
#include "ftxui/dom/elements.hpp"
#include <ctime>
#include <memory>

namespace edc
{
TUIWindow::TUIWindow() : input_text_("")
{
    chat_widget_ = std::make_shared<ChatWidget>();

    footer_ = ::ftxui::Renderer([]() {
        return ::ftxui::text(" Enter 发送   Alt+Enter 换行   Tab 切换输入/滚动   滚轮/↑↓ 滚动   Esc 退出 ") |
               ::ftxui::color(::ftxui::Color::RGB(147, 147, 147));
    });

    ::ftxui::InputOption option;
    option.multiline = true;

    option.transform = [](::ftxui::InputState state) {
        if (state.is_placeholder)
        {
            state.element |= ::ftxui::dim;
        }
        return state.element;
    };

    input_ = ::ftxui::Input(&input_text_, "输入消息，Enter 发送，Alt+Enter 换行...", option);

    container_ = ::ftxui::Container::Vertical({chat_widget_, input_}, &focused_);
    Add(container_);
}

void TUIWindow::Submit()
{
    // Trim first: even without the Input's own Return handling, a message may be
    // all whitespace (e.g. Alt+Enter then Enter), which should not be sent.
    const auto first = input_text_.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
    {
        input_text_.clear();
        return;
    }
    const auto last = input_text_.find_last_not_of(" \t\r\n");
    std::string message = input_text_.substr(first, last - first + 1);

    chat_widget_->AddMessage(std::make_shared<UserMessage>(message, ::time(nullptr)));
    input_text_.clear();

    chat_widget_->ScrollToBottom();
}

::ftxui::Element TUIWindow::OnRender()
{
    return ::ftxui::vbox({chat_widget_->Render() | ::ftxui::flex, ::ftxui::separator(), input_->Render(),
                          ::ftxui::separator(), footer_->Render()});
}

bool TUIWindow::OnEvent(::ftxui::Event event)
{
    // Alt+Enter arrives as ESC-prefixed Return: the terminal (raw mode, ICRNL
    // off) sends CR for Enter, and Alt adds the ESC prefix, so the parser emits
    // a single Special({27, 13}). ESC+LF is accepted too for terminals that
    // send LF. Insert a newline via a synthetic character event, which reuses
    // the Input's cursor and UTF-8 handling without firing on_enter.
    static const ::ftxui::Event kAltEnterCR = ::ftxui::Event::Special(std::string({27, 13}));
    static const ::ftxui::Event kAltEnterLF = ::ftxui::Event::Special(std::string({27, 10}));
    if (event == kAltEnterCR || event == kAltEnterLF)
    {
        input_->OnEvent(::ftxui::Event::Character("\n"));
        return true;
    }

    // Plain Enter submits. Intercepting it here means it never reaches the
    // Input, so multiline stays purely for Alt+Enter and typed text.
    if (event == ::ftxui::Event::Return)
    {
        Submit();
        return true;
    }

    return container_->OnEvent(event);
}

TUIWindow::~TUIWindow()
{
}
} // namespace edc
