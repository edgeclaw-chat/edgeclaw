#include "edc_chat_model.hpp"

#include <algorithm>
#include <ctime>
#include <utility>

namespace edc::model
{

int ChatModel::Subscribe(Listener listener)
{
    const int id = next_id_++;
    listeners_[id] = std::move(listener);
    return id;
}

void ChatModel::Unsubscribe(int id)
{
    listeners_.erase(id);
}

bool ChatModel::Visible(const Message &message) const
{
    if (!filter_)
        return true;
    return filter_->find(message.TaskId()) != filter_->end();
}

void ChatModel::AddMessage(std::shared_ptr<Message> message)
{
    if (!message)
        return;

    // The log keeps everything; the filter only decides what is drawn.
    all_.push_back(message);

    if (Visible(*message))
    {
        visible_.push_back(message);
        Notify(Change{message});
    }
    // Hidden: nothing on screen changes, so no notification.
}

void ChatModel::PrependMessages(std::vector<std::shared_ptr<Message>> messages)
{
    messages.erase(std::remove(messages.begin(), messages.end(), nullptr), messages.end());
    if (messages.empty())
        return;

    // The log keeps everything; the filter only decides what is drawn.
    all_.insert(all_.begin(), messages.begin(), messages.end());

    if (filter_)
    {
        // A filtered view is derived from all_ + task inputs, so rebuild it rather
        // than splice (rare: the backfill runs at session load, before any filter).
        Rebuild();
        Notify(Change{});
        return;
    }

    visible_.insert(visible_.begin(), messages.begin(), messages.end());
    Notify(Change{nullptr, std::move(messages)});
}

void ChatModel::Clear()
{
    all_.clear();
    visible_.clear();
    input_shown_.clear();
    Notify(Change{});
}
void ChatModel::SetFilter(std::shared_ptr<const std::set<std::string>> allowed, std::string label)
{
    filter_ = std::move(allowed);
    filter_label_ = std::move(label);
    Rebuild();
    Notify(Change{});
}

void ChatModel::SetTaskInputs(std::shared_ptr<const std::map<std::string, std::string>> inputs)
{
    task_inputs_ = std::move(inputs);
    // Only matters while a filter is set (it decides each task's opening line).
    if (filter_)
    {
        Rebuild();
        Notify(Change{});
    }
}

void ChatModel::Rebuild()
{
    visible_.clear();
    input_shown_.clear();
    for (const std::shared_ptr<Message> &message : all_)
    {
        if (!Visible(*message))
            continue;

        // Under a filter, each task's history opens with the input it was created
        // from. A message with no task (a command reply) gets none.
        const std::string &task_id = message->TaskId();
        if (filter_ && !task_id.empty() && task_inputs_ && input_shown_.insert(task_id).second)
        {
            const auto it = task_inputs_->find(task_id);
            if (it != task_inputs_->end() && !it->second.empty())
                visible_.push_back(std::make_shared<UserMessage>(it->second, ::time(nullptr), task_id));
        }
        visible_.push_back(message);
    }
}

void ChatModel::Notify(const Change &change)
{
    // Copy the listener map: a listener may unsubscribe/re-subscribe from inside.
    const std::map<int, Listener> listeners = listeners_;
    for (const auto &[id, listener] : listeners)
    {
        (void)id;
        if (listeners_.count(id))
            listener(change);
    }
}

} // namespace edc::model
