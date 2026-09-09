#include "edc_chat_widget.hpp"
#include "ftxui/component/component.hpp"
#include "ftxui/component/event.hpp"
#include "ftxui/dom/elements.hpp"

namespace edc
{

ChatWidget::ChatWidget()
{
    container_ = ::ftxui::Container::Vertical({});
    // Mouse-wheel-only: the log stays out of the keyboard focus cycle so the
    // arrow keys always belong to the input box (caret movement / history
    // recall) and are never stolen to scroll chat. The wheel still scrolls
    // wherever the pointer is, and clicking collapsible entries still works
    // (mouse events route by hit-testing, independent of focus).
    scroll_view_ = std::make_shared<ScrollView>(
        container_,
        ScrollOptions{ScrollMode::kFill, /*show_indicator=*/true, /*follow_tail=*/true,
                      /*focusable=*/false});
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
    // An empty Container::Vertical renders "Empty container" as a debug
    // fallback; show a blank area instead so an empty chat is just empty.
    if (container_->ChildCount() == 0)
    {
        return ::ftxui::filler();
    }

    return scroll_view_->Render();
}

} // namespace edc
