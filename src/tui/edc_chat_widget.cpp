#include "edc_chat_widget.hpp"
#include "edc_style.hpp"
#include "ftxui/component/component.hpp"
#include "ftxui/component/event.hpp"
#include "ftxui/dom/elements.hpp"

#include <utility>

namespace edc
{

ChatWidget::ChatWidget() : ChatWidget(std::make_shared<model::ChatModel>())
{
}

ChatWidget::ChatWidget(std::shared_ptr<model::ChatModel> model)
    : model_(model ? std::move(model) : std::make_shared<model::ChatModel>())
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

    subscription_ = model_->Subscribe([this](const model::ChatModel::Change &change) { OnModelChanged(change); });
    // Initial sync: a shared model may already hold messages (e.g. a log replayed
    // before the view was built).
    OnModelChanged(model::ChatModel::Change{});
}

ChatWidget::~ChatWidget()
{
    if (subscription_ >= 0)
        model_->Unsubscribe(subscription_);
}

void ChatWidget::OnModelChanged(const model::ChatModel::Change &change)
{
    // An appended bubble: one list append, exactly as before (the reader's scroll
    // offset is untouched — if following the tail, layout pins it to the new
    // bottom, otherwise the reader stays where they were).
    if (change.appended)
    {
        list_->AddMessage(change.appended);
        return;
    }

    // A filter / task-input / clear change: rebuild the drawn list from the model.
    list_->Clear();
    for (const std::shared_ptr<Message> &message : model_->visible())
        list_->AddMessage(message);
}

void ChatWidget::AddMessage(std::shared_ptr<Message> message)
{
    model_->AddMessage(std::move(message));
}

void ChatWidget::Clear()
{
    model_->Clear();
}

void ChatWidget::SetFilter(std::shared_ptr<const std::set<std::string>> allowed, std::string label)
{
    model_->SetFilter(std::move(allowed), std::move(label));
}

void ChatWidget::SetTaskInputs(std::shared_ptr<const std::map<std::string, std::string>> inputs)
{
    model_->SetTaskInputs(std::move(inputs));
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
        if (model_->has_filter() && !model_->filter_label().empty())
            return ::ftxui::text("  no messages in " + model_->filter_label()) | ::ftxui::dim |
                   ::ftxui::color(style::palette().text_dim);
        return ::ftxui::filler();
    }

    return scroll_view_->Render();
}

} // namespace edc
