#include "edc_chat_window.hpp"

#include "edc_chat_transcript.hpp"
#include "edc_message.hpp"
#include "ftxui/component/event.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace edc
{
namespace
{

uint8_t ClassifyKind(const std::string &kind)
{
    if (kind == "user")
        return static_cast<uint8_t>(ChatBubbleKind::kUser);
    if (kind == "tool")
        return static_cast<uint8_t>(ChatBubbleKind::kTool);
    if (kind == "thought")
        return static_cast<uint8_t>(ChatBubbleKind::kThought);
    if (kind == "reply")
        return static_cast<uint8_t>(ChatBubbleKind::kReply);
    return static_cast<uint8_t>(ChatBubbleKind::kSys);
}

/* The transcript escapes newlines inside fields, so one line == one bubble and
 * the number of physical lines in a bubble is bounded by its preview chrome,
 * not by its content. A collapsed ToolMessage shows kPreviewLines + a "..." line
 * + a toggle; everything else is one (estimated) block of text. This is only a
 * scroll-geometry estimate -- the exact height is measured when the item enters
 * the window, and IScrollAnchor repairs the offset (see the proposal, §2.4). */
uint16_t EstimateLines(uint8_t kind)
{
    /* ToolMessage::kPreviewLines == 14; +3 for the "...", toggle and border. */
    return kind == static_cast<uint8_t>(ChatBubbleKind::kTool) ? 17 : 4;
}

/* Split the first four raw tabs of one encoded line, recording field spans
 * WITHOUT decoding or copying the content. Mirrors DecodeLineView's splitting
 * rule (only the first four tabs are separators). */
bool SplitUnescaped(std::string_view line, std::string_view parts[5])
{
    if (!line.empty() && line.back() == '\n')
        line.remove_suffix(1);
    if (!line.empty() && line.back() == '\r')
        line.remove_suffix(1);
    if (line.empty())
        return false;

    size_t start = 0;
    int field = 0;
    for (size_t i = 0; i < line.size(); i++)
    {
        if (line[i] == '\t' && field < 4)
        {
            parts[field++] = line.substr(start, i - start);
            start = i + 1;
        }
    }
    if (field < 4)
        return false;
    parts[4] = line.substr(start);
    return !parts[0].empty();
}

/* Undo the four escapes the transcript format uses; only the task id needs it
 * at index time (filtering matches on it). Content is never decoded here. */
std::string UnescapeSmall(std::string_view in)
{
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); i++)
    {
        if (in[i] == '\\' && i + 1 < in.size())
        {
            const char c = in[++i];
            switch (c)
            {
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            default: out.push_back(c); break;
            }
        }
        else
            out.push_back(in[i]);
    }
    return out;
}

ChatBubbleRef MakeMemoryRef(const ChatTranscriptEntry &entry, uint32_t mem_index)
{
    ChatBubbleRef ref;
    ref.source = ChatBubbleSource::kMemory;
    ref.mem_index = mem_index;
    ref.kind = ClassifyKind(entry.kind);
    ref.render_pref = static_cast<uint8_t>(entry.render_pref);
    ref.task_id = entry.task_id;
    ref.task_id_len = static_cast<uint32_t>(entry.task_id.size());
    ref.header_len = static_cast<uint32_t>(entry.header.size());
    ref.content_len = static_cast<uint32_t>(entry.content.size());
    ref.est_lines = EstimateLines(ref.kind);
    return ref;
}

} // namespace

size_t BuildChatBubbleRefsFromPage(const int64_t *seqs, const char *const *lines, size_t n,
                                   std::vector<ChatBubbleRef> *out)
{
    if (!out)
        return 0;
    out->clear();
    out->reserve(n);
    std::string_view parts[5];
    for (size_t i = 0; i < n; i++)
    {
        const std::string line = lines[i] ? lines[i] : "";
        if (line.empty() || !SplitUnescaped(line, parts))
            continue;
        ChatBubbleRef ref;
        ref.source = ChatBubbleSource::kDatabase;
        ref.seq = seqs[i];
        ref.line = line;
        ref.kind = ClassifyKind(std::string(parts[0]));
        ref.render_pref = static_cast<uint8_t>(std::atoi(std::string(parts[1]).c_str()));
        ref.task_id = UnescapeSmall(parts[2]);
        ref.est_lines = EstimateLines(ref.kind);
        out->push_back(std::move(ref));
    }
    return out->size();
}

MessageWindow::MessageWindow() : refs_owned_(new std::vector<ChatBubbleRef>()), refs_(refs_owned_)
{
}

MessageWindow::MessageWindow(std::vector<ChatBubbleRef> *refs) : refs_(refs)
{
}

ChatBubbleRef *MessageWindow::Ref(size_t index)
{
    return (refs_ && index < refs_->size()) ? &(*refs_)[index] : nullptr;
}

const ChatBubbleRef *MessageWindow::Ref(size_t index) const
{
    return (refs_ && index < refs_->size()) ? &(*refs_)[index] : nullptr;
}

size_t MessageWindow::AddMemoryBubble(const ChatTranscriptEntry &entry)
{
    if (!refs_)
        return 0;
    const uint32_t mem_index = static_cast<uint32_t>(memory_.size());
    memory_.push_back(entry);
    const size_t index = refs_->size();
    refs_->push_back(MakeMemoryRef(entry, mem_index));
    return index;
}

std::shared_ptr<Message> MessageWindow::Proxy(size_t index)
{
    const ChatBubbleRef *ref = Ref(index);
    if (!ref)
        return nullptr;
    return std::make_shared<LazyBubble>(this, index, ref->task_id);
}

size_t MessageWindow::SetWindow(size_t first, size_t last, size_t prefetch)
{
    if (!refs_ || refs_->empty())
    {
        live_.clear();
        first_ = last_ = 0;
        return 0;
    }

    const size_t n = refs_->size();
    first = std::min(first, n);
    last = std::min(std::max(last, first), n);

    size_t want_first = first > prefetch ? first - prefetch : 0;
    size_t want_last = std::min(n, last + prefetch);
    if (want_last - want_first > kMaxLive)
    {
        const size_t requested = last - first;
        if (requested >= kMaxLive)
        {
            want_first = first;
            want_last = std::min(n, first + kMaxLive);
        }
        else
        {
            const size_t extra = kMaxLive - requested;
            const size_t above = std::min(extra / 2, want_first);
            want_first -= above;
            want_last = std::min(n, want_last + (extra - above));
            if (want_last - want_first > kMaxLive)
                want_last = want_first + kMaxLive;
        }
    }

    first_ = want_first;
    last_ = want_last;
    for (auto it = live_.begin(); it != live_.end();)
    {
        if (it->first < want_first || it->first >= want_last)
            it = live_.erase(it);
        else
            ++it;
    }
    for (size_t i = want_first; i < want_last; i++)
        Get(i);
    return live_.size();
}

bool MessageWindow::Fields(size_t index, std::string *kind, int *render_pref, std::string *task_id,
                           std::string *header, std::string *content)
{
    if (!refs_ || index >= refs_->size())
        return false;
    const ChatBubbleRef &ref = (*refs_)[index];
    if (ref.source == ChatBubbleSource::kDatabase)
    {
        ChatTranscriptEntry entry;
        if (!DecodeTranscriptLine(ref.line, &entry))
            return false;
        *kind = std::move(entry.kind);
        *task_id = std::move(entry.task_id);
        *header = std::move(entry.header);
        *content = std::move(entry.content);
        *render_pref = entry.render_pref;
        return true;
    }
    if (ref.source == ChatBubbleSource::kMemory)
    {
        if (ref.mem_index >= memory_.size())
            return false;
        const ChatTranscriptEntry &e = memory_[ref.mem_index];
        *kind = e.kind;
        *task_id = e.task_id;
        *header = e.header;
        *content = e.content;
        *render_pref = e.render_pref;
        return true;
    }
    return false;
}

std::shared_ptr<Message> MessageWindow::Get(size_t index)
{
    if (!refs_ || index >= refs_->size())
        return nullptr;
    auto it = live_.find(index);
    if (it != live_.end())
        return it->second;

    std::string kind, task_id, header, content;
    int render_pref = 0;
    if (!Fields(index, &kind, &render_pref, &task_id, &header, &content))
        return nullptr;
    const ChatBubbleRef &ref = (*refs_)[index];

    std::shared_ptr<Message> bubble = MakeChatBubble(kind, header, content, task_id, render_pref, ::time(nullptr));
    if (!bubble)
        return nullptr;
    if (ref.expanded)
        bubble->SetExpanded(true); /* P4: state lives on the descriptor */
    if (live_.size() >= kMaxLive)
    {
        size_t victim = index;
        size_t best = 0;
        for (const auto &kv : live_)
        {
            const size_t d = kv.first > index ? kv.first - index : index - kv.first;
            if (d > best)
            {
                best = d;
                victim = kv.first;
            }
        }
        live_.erase(victim);
    }
    live_.emplace(index, bubble);
    return bubble;
}

void MessageWindow::Evict(size_t index)
{
    live_.erase(index);
}

void MessageWindow::Clear()
{
    live_.clear();
    first_ = last_ = 0;
}

/* ── LazyBubble: the O(n) small proxy, not the O(n) message ───────────── */

LazyBubble::LazyBubble(MessageWindow *window, size_t index, std::string task_id)
    : Message("", 0, task_id), window_(window), index_(index)
{
}

::ftxui::Element LazyBubble::OnRender()
{
    if (!window_)
        return ::ftxui::text("");
    std::shared_ptr<Message> real = window_->Get(index_);
    if (!real)
        return ::ftxui::text("");

    /* Let the correctly-typed bubble (ToolMessage / ThoughtMessage / ...) build
     * its tree, then CACHE it here. Caching on the proxy is what makes ftxui's
     * measure pass and draw pass see the same Node even if the window evicts and
     * rebuilds the real instance between frames. Delegating through
     * ComponentBase::Render() (or returning a transient tree) left the frame
     * blank after a scroll. */
    real->SetRenderWidth(render_width_);
    /* reflect(HitBox()) captures a Box& into the element tree. Rebind it to a
     * box owned by THIS proxy before building the tree: MessageWindow may evict
     * the real bubble while the proxy keeps rendering the cached tree, and a
     * reference into the evicted object is a heap-use-after-free. */
    real->UseHitBox(hit_box_);
    if (!body_)
        body_ = real->OnRender();
    return body_;
}

bool LazyBubble::OnEvent(::ftxui::Event event)
{
    if (!window_)
        return false;
    std::shared_ptr<Message> real = window_->Get(index_);
    if (!real)
        return false;
    /* The tree reflected the proxy's box, not the transient real bubble's. */
    real->UseHitBox(hit_box_);
    const bool handled = real->OnEvent(event);
    if (ChatBubbleRef *ref = window_->Ref(index_))
        ref->expanded = real->Expanded(); /* P4 */
    /* The toggle changed the real bubble's shape; drop the proxy's cached tree
     * so the next frame renders the new collapsed/expanded form. MessageList
     * also invalidates the row height through InvalidateHeight(). */
    body_ = nullptr;
    return handled;
}

void LazyBubble::SetRenderWidth(int cols)
{
    if (cols == render_width_)
        return;
    render_width_ = cols;
    body_ = nullptr; /* the cached tree was wrapped for the old width */
}

} // namespace edc
