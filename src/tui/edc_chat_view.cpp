#include "edc_chat_view.hpp"
#include "ftxui/component/event.hpp"
#include "ftxui/dom/elements.hpp"

namespace edc
{
Message::Message(const std::string_view content, Role role, time_t ts) : content_(content), role_(role), ts_(ts)
{
}

Message::Message(Message &&rhs) : content_(std::move(rhs.content_)), role_(rhs.role_), ts_(rhs.ts_)
{
}

bool Message::OnEvent(ftxui::Event)
{
    return false;
}

::ftxui::Element Message::OnRender()
{
    auto body = ::ftxui::paragraph(content_) | ::ftxui::borderStyled(::ftxui::BorderStyle::EMPTY) |
                ::ftxui::bgcolor(::ftxui::Color::RGB(30, 30, 30));

    auto header = ::ftxui::separatorHeavy() | ::ftxui::color(::ftxui::Color::Blue);
    auto line = role_ == Role::User ? ::ftxui::hbox({header, body}) : body;
    return ::ftxui::vbox({line, ftxui::separatorEmpty()});
}
} // namespace edc
