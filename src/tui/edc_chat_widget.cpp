#include "edc_chat_widget.hpp"
#include "edc_style.hpp"
#include "ftxui/component/component.hpp"
#include "ftxui/component/event.hpp"
#include "ftxui/dom/elements.hpp"

#include <utility>

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
    if (!message)
        return;

    // The log keeps everything; the filter only decides what is drawn. Recording
    // before the visibility test is what makes switching the filter back to
    // "all" non-destructive.
    all_.push_back(message);

    if (Visible(*message))
        list_->AddMessage(std::move(message));

    // Leave the scroll offset alone: if we are following the tail, layout pins
    // it to the new bottom, otherwise the reader stays where they were.
}

void ChatWidget::Clear()
{
    all_.clear();
    list_->Clear();
}

void ChatWidget::SetTaskInputs(std::shared_ptr<const std::map<std::string, std::string>> inputs)
{
    task_inputs_ = std::move(inputs);
    if (filter_)
        ApplyFilter();
}

void ChatWidget::SetFilter(std::shared_ptr<const std::set<std::string>> allowed, std::string label)
{
    filter_ = std::move(allowed);
    filter_label_ = std::move(label);
    ApplyFilter();
}

bool ChatWidget::Visible(const Message &message) const
{
    if (!filter_)
        return true;
    return filter_->find(message.TaskId()) != filter_->end();
}

void ChatWidget::ApplyFilter()
{
    list_->Clear();
    input_shown_.clear();
    for (const std::shared_ptr<Message> &message : all_)
    {
        if (!Visible(*message))
            continue;

        // Under a filter, each task's history opens with the input it was created
        // from. A message with no task (a command reply) is not a history, so it
        // gets none.
        const std::string &task_id = message->TaskId();
        if (filter_ && !task_id.empty() && task_inputs_ && input_shown_.insert(task_id).second)
        {
            const auto it = task_inputs_->find(task_id);
            if (it != task_inputs_->end() && !it->second.empty())
                list_->AddMessage(std::make_shared<UserMessage>(it->second, ::time(nullptr), task_id));
        }
        list_->AddMessage(message);
    }
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
    // instead so an empty chat is just empty. With a filter set the emptiness is
    // the filter's doing, so say which selection came up empty -- otherwise a
    // group that simply has no recorded output would read as a broken button.
    if (list_->Empty())
    {
        if (filter_ && !filter_label_.empty())
            return ::ftxui::text("  no messages in " + filter_label_) | ::ftxui::dim |
                   ::ftxui::color(style::palette().text_dim);
        return ::ftxui::filler();
    }

    return scroll_view_->Render();
}

} // namespace edc
