#include "edc_chat_widget.hpp"
#include "ftxui/component/component.hpp"
#include "ftxui/component/event.hpp"
#include "ftxui/dom/elements.hpp"
#include <memory>

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

    return ::ftxui::vbox({body, ftxui::separatorEmpty()});
}

ChatWidget::ChatWidget()
{
    container_ = ::ftxui::Container::Vertical({});
}

void ChatWidget::AddMessage(std::shared_ptr<Message> message)
{
    container_->Add(message);
}

bool ChatWidget::OnEvent(::ftxui::Event event)
{
    return false;
}

::ftxui::Element ChatWidget::OnRender()
{
    return container_->Render() | ::ftxui::bgcolor(::ftxui::Color::RGB(10, 10, 10));
}

} // namespace edc
