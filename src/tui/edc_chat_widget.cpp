#include "edc_chat_widget.hpp"
#include "ftxui/component/component.hpp"
#include "ftxui/component/event.hpp"
#include "ftxui/dom/elements.hpp"

namespace edc
{

ChatWidget::ChatWidget()
{
    container_ = ::ftxui::Container::Vertical({});
    scroll_view_ = std::make_shared<ScrollView>(container_);
    Add(scroll_view_);
}

void ChatWidget::AddMessage(std::shared_ptr<Message> message)
{
    container_->Add(message);
    container_->Add(::ftxui::Renderer([] { return ::ftxui::separatorEmpty(); }));

    // Leave the scroll offset alone: if we are following the tail, layout pins
    // it to the new bottom, otherwise the reader stays where they were.
}

bool ChatWidget::Scroll(int delta)
{
    return scroll_view_->Scroll(delta);
}

bool ChatWidget::ScrollToTop()
{
    return scroll_view_->ScrollToTop();
}

bool ChatWidget::ScrollToBottom()
{
    return scroll_view_->ScrollToBottom();
}

bool ChatWidget::OnEvent(::ftxui::Event event)
{
    return scroll_view_->OnEvent(event);
}

::ftxui::Element ChatWidget::OnRender()
{
    return scroll_view_->Render() | ::ftxui::bgcolor(::ftxui::Color::RGB(10, 10, 10));
}

} // namespace edc
