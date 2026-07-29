#include "edc_chat_view.hpp"
#include "ftxui/component/event.hpp"
#include "ftxui/dom/elements.hpp"

namespace edc
{
Message::Message(const std::string_view content, Role role, time_t ts) : content_(content), role_(role), ts_(ts)
{
    switch (role_)
    {
    case Role::User:
        label_ = "▎you";
        color_ = ::ftxui::Color::Yellow;
        break;
    case Role::Assistant:
        label_ = "▎assistant";
        color_ = ::ftxui::Color::Green;
        break;
    case Role::System:
        label_ = "▎system";
        color_ = ::ftxui::Color::GrayDark;
        break;
    case Role::Tool:
        label_ = "▎tool";
        color_ = ::ftxui::Color::Blue;
        break;
    }
}

Message::Message(Message &&rhs)
    : content_(std::move(rhs.content_)), role_(rhs.role_), ts_(rhs.ts_), label_(rhs.label_), color_(rhs.color_)
{
}

bool Message::OnEvent(ftxui::Event)
{
    return false;
}

::ftxui::Element Message::OnRender()
{
    ftxui::Element body = ftxui::paragraph(content_);
    ftxui::Elements children = {ftxui::text(label_) | ftxui::bold | ftxui::color(color_), body};
    children.push_back(ftxui::separatorEmpty());

    return ftxui::vbox(children);
}
} // namespace edc
