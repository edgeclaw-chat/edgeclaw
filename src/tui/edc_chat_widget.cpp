#include "edc_chat_widget.hpp"
#include "ftxui/component/component.hpp"
#include "ftxui/component/event.hpp"
#include "ftxui/component/mouse.hpp"
#include "ftxui/dom/elements.hpp"

namespace edc
{
Message::Message(const std::string_view content, Role role, time_t ts) : content_(content), role_(role), ts_(ts)
{
}

Message::Message(Message &&rhs) : content_(std::move(rhs.content_)), role_(rhs.role_), ts_(rhs.ts_)
{
}

Message::Role Message::role() const
{
    return role_;
}

bool Message::OnEvent(ftxui::Event event)
{
    if (role_ != Role::Thought)
    {
        return false;
    }

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

::ftxui::Element Message::RenderThought()
{
    auto arrow = thought_expanded_ ? "-" : "+";
    auto applied_color = thought_expanded_ ? ::ftxui::Color::RGB(151, 104, 44) : ::ftxui::Color::RGB(245, 167, 66);
    auto label = ::ftxui::text(" " + std::string(arrow) + " Thought") | ::ftxui::color(applied_color) | ::ftxui::bold |
                 ::ftxui::reflect(box_);

    if (!thought_expanded_)
    {
        return label;
    }

    auto body = ::ftxui::paragraph(content_) | ::ftxui::borderStyled(::ftxui::BorderStyle::EMPTY) |
                ::ftxui::bgcolor(::ftxui::Color::RGB(30, 30, 30));

    return ::ftxui::vbox({label, body});
}

::ftxui::Element Message::OnRender()
{
    if (role_ == Role::Thought)
    {
        return RenderThought();
    }

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

ChatWidget::ChatWidget()
{
    container_ = ::ftxui::Container::Vertical({});
}

void ChatWidget::AddMessage(std::shared_ptr<Message> message)
{
    container_->Add(message);
    container_->Add(::ftxui::Renderer([] { return ::ftxui::separatorEmpty(); }));
}

bool ChatWidget::OnEvent(::ftxui::Event event)
{
    return container_->OnEvent(event);
}

::ftxui::Element ChatWidget::OnRender()
{
    return container_->Render() | ::ftxui::bgcolor(::ftxui::Color::RGB(10, 10, 10));
}

} // namespace edc
