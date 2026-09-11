#include "edc_chat_widget.hpp"
#include "ftxui/component/component.hpp"
#include "ftxui/component/event.hpp"
#include "ftxui/dom/elements.hpp"

namespace edc
{

ChatWidget::ChatWidget()
{
    // Mouse-wheel-only: the log stays out of the keyboard focus cycle so the
    // arrow keys always belong to the input box (caret movement / history
    // recall) and are never stolen to scroll chat. The wheel still scrolls
    // wherever the pointer is, and clicking collapsible entries still works
    // (mouse events route by hit-testing, independent of focus).
    list_ = std::make_shared<MessageList>();
    scroll_view_ = std::make_shared<ScrollView>(list_, ScrollOptions{ScrollMode::kFill, /*show_indicator=*/true,
                                                                     /*follow_tail=*/true,
                                                                     /*focusable=*/false});
    Add(scroll_view_);
}

void ChatWidget::AddMessage(std::shared_ptr<Message> message)
{
    list_->AddMessage(std::move(message));

    // Leave the scroll offset alone: if we are following the tail, layout pins
    // it to the new bottom, otherwise the reader stays where they were.
}

void ChatWidget::Clear()
{
    list_->Clear();
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
    // An empty list would render as an empty box of nothing; show a blank area
    // instead so an empty chat is just empty.
    if (list_->Empty())
    {
        return ::ftxui::filler();
    }

    return scroll_view_->Render();
}

} // namespace edc
