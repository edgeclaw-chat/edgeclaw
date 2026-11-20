#include "edc_chat_model.hpp"
#include "edc_chat_bubble_page.hpp"

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
    /* A live in-process bubble means the reader has moved past the startup
     * fallback; a stale startup page must no longer replace the log. */
    legacy_fallback_ = false;
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
    legacy_fallback_ = false;
    Notify(Change{});
}

void ChatModel::LoadTranscript(const std::string &path)
{
    legacy_fallback_ = true;
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


bool ChatModel::LoadFromPage(const std::string &session_id, const std::string &payload)
{
    ChatBubblePageReply reply;
    if (!DecodeChatBubblePageReply(session_id.c_str(), payload.c_str(), &reply))
        return false;
    if (!reply.session_id.empty() && reply.session_id != session_id)
        return false; /* a late reply for a session the reader left */

    refs_holder_ = std::make_unique<std::vector<ChatBubbleRef>>();
    std::vector<const char *> lines;
    std::vector<int64_t> seqs;
    lines.reserve(reply.rows.size());
    seqs.reserve(reply.rows.size());
    for (const auto &row : reply.rows)
    {
        lines.push_back(row.line.c_str());
        seqs.push_back(row.seq);
    }
    BuildChatBubbleRefsFromPage(seqs.data(), lines.data(), lines.size(), refs_holder_.get());
    window_ = std::make_unique<MessageWindow>("", refs_holder_.get());

    all_.clear();
    all_.reserve(window_->RefCount());
    for (size_t i = 0; i < window_->RefCount(); i++)
        all_.push_back(window_->Proxy(i));

    session_id_ = session_id;
    has_more_ = reply.has_more;
    oldest_seq_ = all_.empty() ? 0 : (*refs_holder_)[0].seq;
    legacy_fallback_ = false;
    const size_t n = all_.size();
    window_->SetWindow(n > kPrimeWindow ? n - kPrimeWindow : 0, n, /*prefetch=*/40);
    Rebuild();
    Notify(Change{});
    return true;
}

bool ChatModel::RecoverStartupPage(const std::string &session_id, const std::string &payload)
{
    ChatBubblePageReply reply;
    if (!DecodeChatBubblePageReply(session_id.c_str(), payload.c_str(), &reply))
        return false;
    if (reply.before_seq != 0 || reply.session_id != session_id)
        return false; /* only the cursor-0 startup page can replace a fallback */
    if (!legacy_fallback_ || !window_ || !window_->IsLegacyFallback())
        return false; /* live bubbles are already on screen: never clobber them */
    return LoadFromPage(session_id, payload);
}

bool ChatModel::ApplyOlderPage(const std::string &session_id, const std::string &payload)
{
    ChatBubblePageReply reply;
    if (!DecodeChatBubblePageReply(session_id.c_str(), payload.c_str(), &reply))
        return false;
    if (reply.session_id != session_id || reply.session_id != session_id_ || reply.rows.empty() ||
        !refs_holder_ || !window_)
        return false;
    /* The scheduler echoes the requested cursor in before_seq. Only the page
     * we actually asked for may be prepended. Two stale replies must be refused:
     *   - cursor 0 is the STARTUP ("newest") page, echoed by a synchronous
         fetch that may have timed out under a slow sanitizer build. It must
     *     never be prepended to a log that has since taken live bubbles.
     *   - a cursor that is no longer oldest_seq_ answers a request the reader
     *     has already moved past (session switch, a faster second scroll).
     * An older-page request always carries a POSITIVE seq, so rejecting 0
     * cannot hide a legitimate page. */
    if (reply.before_seq <= 0 || reply.before_seq != oldest_seq_)
        return false;

    std::vector<ChatBubbleRef> older;
    std::vector<const char *> lines;
    std::vector<int64_t> seqs;
    for (const auto &row : reply.rows)
    {
        lines.push_back(row.line.c_str());
        seqs.push_back(row.seq);
    }
    BuildChatBubbleRefsFromPage(seqs.data(), lines.data(), lines.size(), &older);
    if (older.empty())
        return false;

    /* Prepend in file order. A LazyBubble stores its position, so ALL proxies
     * must be rebuilt after the insert (they are cheap; the descriptors and the
     * expanded flags live in refs_holder_ and survive). Rebuilding also drops
     * the resident live_ instances, which are keyed by the old positions. */
    refs_holder_->insert(refs_holder_->begin(), older.begin(), older.end());
    window_->Clear(); /* drops live_ keyed by the old indices */
    all_.clear();
    all_.reserve(window_->RefCount());
    for (size_t i = 0; i < window_->RefCount(); i++)
        all_.push_back(window_->Proxy(i));

    oldest_seq_ = (*refs_holder_)[0].seq;
    has_more_ = reply.has_more;
    Rebuild();
    Notify(Change{});
    return true;
}

int64_t ChatModel::oldest_seq() const
{
    return oldest_seq_;
}


ChatModel::Snapshot ChatModel::TakeSnapshot()
{
    Snapshot snap;
    snap.refs = std::move(refs_holder_);
    snap.window = std::move(window_);
    snap.all = std::move(all_);
    snap.visible = std::move(visible_);
    snap.session_id = std::move(session_id_);
    snap.oldest_seq = oldest_seq_;
    snap.has_more = has_more_;
    snap.legacy_fallback = legacy_fallback_;
    /* Leave this model empty but usable: RenderTaskHistory will AddMessage()
     * into it, and AddMessage() creates a fresh MessageWindow when window_ is
     * null. The saved window keeps owning the saved refs (raw pointer stays
     * valid because the vector object itself does not move). */
    input_shown_.clear();
    filter_label_.clear();
    oldest_seq_ = 0;
    has_more_ = false;
    legacy_fallback_ = false;
    return snap;
}

void ChatModel::RestoreSnapshot(Snapshot snap)
{
    refs_holder_ = std::move(snap.refs);
    window_ = std::move(snap.window);
    all_ = std::move(snap.all);
    visible_ = std::move(snap.visible);
    session_id_ = std::move(snap.session_id);
    oldest_seq_ = snap.oldest_seq;
    has_more_ = snap.has_more;
    legacy_fallback_ = snap.legacy_fallback;
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
