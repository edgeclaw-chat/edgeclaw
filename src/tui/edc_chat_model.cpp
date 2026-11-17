#include "edc_chat_model.hpp"

#include <algorithm>
#include <ctime>
#include <utility>

#include "edc_api.h"

namespace
{
constexpr size_t kPrimeWindow = 40;
} // namespace

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
    if (!window_)
        window_ = std::make_unique<MessageWindow>();

    /* An in-process bubble becomes a kMemory entry. NOTHING is written to the
     * transcript: command replies and interface feedback stay in memory, are
     * evicted when they scroll away, and are rebuilt from this snapshot when
     * they come back. */
    ChatTranscriptEntry entry;
    entry.kind = message->KindName();
    entry.render_pref = message->RenderPref();
    entry.task_id = message->TaskId();
    entry.header = message->Header();
    entry.content = message->Content();
    const size_t index = window_->AddMemoryBubble(entry);

    std::shared_ptr<Message> proxy = window_->Proxy(index);
    all_.push_back(proxy);
    if (Visible(*proxy))
    {
        visible_.push_back(proxy);
        Notify(Change{proxy});
    }
}

void ChatModel::Clear()
{
    all_.clear();
    visible_.clear();
    input_shown_.clear();
    window_.reset();
    refs_holder_.reset();
    Notify(Change{});
}

void ChatModel::LoadTranscript(const std::string &path)
{
    refs_holder_ = std::make_unique<std::vector<ChatBubbleRef>>();
    BuildChatBubbleRefs(path, refs_holder_.get());
    window_ = std::make_unique<MessageWindow>(path, refs_holder_.get());

    all_.clear();
    all_.reserve(window_->RefCount());
    for (size_t i = 0; i < window_->RefCount(); i++)
        all_.push_back(window_->Proxy(i));
    /* Prime the tail window now, before the first frame: ftxui measures items
     * during layout, which is earlier than ChatWidget::OnRender can drive us. */
    const size_t n = all_.size();
    window_->SetWindow(n > kPrimeWindow ? n - kPrimeWindow : 0, n, /*prefetch=*/40);
    Rebuild();
    Notify(Change{});
}

void ChatModel::SetWindow(size_t first, size_t last, size_t prefetch)
{
    if (window_)
        window_->SetWindow(first, last, prefetch);
}

size_t ChatModel::LiveCount() const
{
    return window_ ? window_->LiveCount() : 0;
}

size_t ChatModel::CachedTreeCount() const
{
    size_t n = 0;
    for (const std::shared_ptr<Message> &message : all_)
        if (message && message->HasCachedTree())
            n++;
    return n;
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
