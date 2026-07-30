#include "edc_message.hpp"
#include "ftxui/component/event.hpp"
#include "ftxui/component/mouse.hpp"
#include "ftxui/dom/elements.hpp"

namespace edc
{

// ============================================================================
// Message
// ============================================================================

Message::Message(const std::string_view content, Role role, time_t ts) : content_(content), role_(role), ts_(ts)
{
}

Message::Role Message::role() const
{
    return role_;
}

bool Message::OnEvent(ftxui::Event)
{
    return false;
}

::ftxui::Element Message::OnRender()
{
    auto body = ::ftxui::paragraph(content_);
    auto header = ::ftxui::separatorHeavy() | ::ftxui::color(::ftxui::Color::Blue);

    if (role_ == Role::User)
    {
        body = ::ftxui::hbox({header, body | ::ftxui::borderStyled(::ftxui::BorderStyle::EMPTY)}) |
               ::ftxui::bgcolor(::ftxui::Color::RGB(30, 30, 30));
    }
    else
    {
        body = body | ::ftxui::borderStyled(::ftxui::BorderStyle::EMPTY) |
               ::ftxui::bgcolor(::ftxui::Color::RGB(30, 30, 30));
    }

    return body;
}

// ============================================================================
// ThoughtMessage
// ============================================================================

ThoughtMessage::ThoughtMessage(const std::string_view content, time_t ts) : Message(content, Role::Assistant, ts)
{
}

bool ThoughtMessage::OnEvent(ftxui::Event event)
{
    if (event.is_mouse())
    {
        if (event.mouse().button == ::ftxui::Mouse::Left && event.mouse().motion == ::ftxui::Mouse::Pressed &&
            box_.Contain(event.mouse().x, event.mouse().y))
        {
            thought_expanded_ = !thought_expanded_;
            return true;
        }
        return false;
    }

    if (event == ftxui::Event::Return)
    {
        thought_expanded_ = !thought_expanded_;
        return true;
    }
    return false;
}

::ftxui::Element ThoughtMessage::OnRender()
{
    auto arrow = thought_expanded_ ? "-" : "+";
    auto applied_color = thought_expanded_ ? ::ftxui::Color::RGB(151, 104, 44) : ::ftxui::Color::RGB(245, 167, 66);
    auto label = ::ftxui::text(" " + std::string(arrow) + " Thought") | ::ftxui::color(applied_color) | ::ftxui::bold |
                 ::ftxui::reflect(box_);

    if (!thought_expanded_)
    {
        return label;
    }

    auto body = ::ftxui::paragraph(content_) | ::ftxui::color(::ftxui::Color::RGB(147, 147, 147)) |
                ::ftxui::borderStyled(::ftxui::BorderStyle::EMPTY);

    return ::ftxui::vbox({label, body});
}

} // namespace edc
