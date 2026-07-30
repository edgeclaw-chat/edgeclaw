#include "edc_chat_widget.hpp"
#include "ftxui/component/component.hpp"
#include "ftxui/component/event.hpp"
#include "ftxui/dom/elements.hpp"
#include <iostream>
#include <memory>

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

ChatWidget::ChatWidget()
{
    container_ = ::ftxui::Container::Vertical({});
}

void ChatWidget::AddMessage(std::shared_ptr<Message> message)
{
    ::ftxui::Component item = message;
    if (message->role() == Message::Role::Thought)
    {
        auto show = std::make_shared<bool>(false);
        auto collapsible = ::ftxui::Collapsible("Thought", message, show.get());

        auto thought_ui = ::ftxui::Renderer([show, collapsible]() {
            auto component = collapsible->ChildAt(0);
            auto label = component->ChildAt(0);
            auto content = component->ChildAt(1);

            auto applied_color = *show ? ::ftxui::color(::ftxui::Color::RGB(151, 104, 44))
                                       : ::ftxui::color(::ftxui::Color::RGB(245, 167, 66));

            return ::ftxui::vbox({label->Render() | applied_color | ::ftxui::bold, content->Render()});
        });

        item = CatchEvent(thought_ui, [collapsible](::ftxui::Event event) { return collapsible->OnEvent(event); });
    }

    container_->Add(item);
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
