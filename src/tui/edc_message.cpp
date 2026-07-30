#include "edc_message.hpp"
#include "ftxui/component/event.hpp"
#include "ftxui/component/mouse.hpp"
#include "ftxui/dom/elements.hpp"
#include "markdown.hpp"

namespace edc
{

Message::Message(const std::string_view content, time_t ts) : content_(content), ts_(ts)
{
}

bool Message::OnEvent(ftxui::Event)
{
    return false;
}

::ftxui::Element Message::OnRender()
{
    auto body = RenderMarkdown(content_);
    body =
        body | ::ftxui::borderStyled(::ftxui::BorderStyle::EMPTY) | ::ftxui::bgcolor(::ftxui::Color::RGB(30, 30, 30));

    return body;
}

ThoughtMessage::ThoughtMessage(const std::string_view content, time_t ts) : Message(content, ts)
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

UserMessage::UserMessage(const std::string_view content, time_t ts) : Message(content, ts)
{
}

::ftxui::Element UserMessage::OnRender()
{
    auto body = ::ftxui::paragraph(content_);
    auto header = ::ftxui::separatorHeavy() | ::ftxui::color(::ftxui::Color::Blue);

    body = ::ftxui::hbox({header, body | ::ftxui::borderStyled(::ftxui::BorderStyle::EMPTY)}) |
           ::ftxui::bgcolor(::ftxui::Color::RGB(30, 30, 30));
    return body;
}

bool UserMessage::OnEvent(ftxui::Event)
{
    return false;
}

ToolMessage::ToolMessage(const std::string_view header, const std::string_view content, time_t ts)
    : Message(content, ts), header_(header)
{
}

bool ToolMessage::OnEvent(::ftxui::Event)
{
    return false;
}

::ftxui::Element ToolMessage::OnRender()
{
    auto header = ::ftxui::paragraph(header_) | ftxui::color(::ftxui::Color::RGB(147, 147, 147));
    auto body = ::ftxui::paragraph(content_);
    return ::ftxui::vbox({header, ::ftxui::separatorEmpty(), body}) |
           ::ftxui::borderStyled(::ftxui::BorderStyle::EMPTY) | ::ftxui::bgcolor(::ftxui::Color::RGB(30, 30, 30));
}

} // namespace edc
