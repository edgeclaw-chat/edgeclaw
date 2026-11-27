#pragma once

// Windowed chat log: chat_bubbles in edgeclaw.db is the view-state authority
// for history, and only a bounded window of Message instances is materialized.
//
// A log with 40k bubbles costs 40k small descriptors + 40k tiny LazyBubble
// proxies, but at most kMaxLive real Message objects (each owning text AND a
// cached ftxui tree).

#include "edc_chat_transcript.hpp"
#include "edc_message.hpp"
#include "ftxui/component/event.hpp"
#include "ftxui/screen/box.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace edc
{

/* Where one bubble's data comes from.
 *
 * kMemory   -- a bubble produced in THIS process that is deliberately NOT
 *              persisted: command replies and the "task N finished" /
 *              interrupted-hint interface feedback. Its fields live in
 *              MessageWindow::memory_.
 * kDatabase -- a chat_bubbles row, line stored inline in the descriptor. */
enum class ChatBubbleSource : uint8_t
{
    kMemory = 0,   /* this process only, deliberately not persisted */
    kDatabase = 1, /* a chat_bubbles row, line stored inline */
};

struct ChatBubbleRef
{
    int64_t seq = 0;       /* kDatabase only: the row's per-session seq */
    std::string line;      /* kDatabase only: the ENCODED row, verbatim */
    uint32_t mem_index = 0;
    ChatBubbleSource source = ChatBubbleSource::kMemory;
    uint8_t kind = 0;        /* ChatBubbleKind */
    uint8_t render_pref = 0; /* EDC_APP_RENDER_PLAIN / _MARKDOWN */
    uint32_t task_id_len = 0;
    uint32_t header_len = 0;  /* encoded field lengths, for height estimates */
    uint32_t content_len = 0;
    std::string task_id;      /* decoded once at index time, for filtering */
    uint16_t est_lines = 1;   /* collapsed estimate; no element tree is built */
    bool expanded = false;    /* UI state lives here, not on the transient Message */
    int16_t measured_h = -1;  /* -1 = never measured at the current width */
    uint32_t measured_w = 0;  /* width the measured_h belongs to */
};

enum class ChatBubbleKind : uint8_t
{
    kUser = 0,
    kTool,
    kThought,
    kReply,
    kSys,
};

/* Build descriptors from one chat_bubbles page (seq + encoded line). The line is
 * decoded once here for the cheap index fields, and kept verbatim so Fields()
 * never needs a second DB read when the row scrolls into view. */
size_t BuildChatBubbleRefsFromPage(const int64_t *seqs, const char *const *lines, size_t n,
                                   std::vector<ChatBubbleRef> *out);

/* A lightweight stand-in occupying one slot in ChatModel::all_/visible_ (and
 * MessageList::items_). Holds NO content and NO cached element tree: TaskId()
 * answers the filter from the descriptor, and rendering/events are delegated to
 * the real bubble the window currently holds. Outside the window it renders an
 * empty block. It never owns a shared_ptr, so it cannot dangle on eviction. */
class LazyBubble : public Message
{
  public:
    LazyBubble(class MessageWindow *window, size_t index, std::string task_id);

    ::ftxui::Element OnRender() override;
    bool OnEvent(::ftxui::Event event) override;
    void SetRenderWidth(int cols) override;

  private:
    class MessageWindow *window_;
    size_t index_;
    /* The real bubble's collapsible header reflects its HitBox(). A cached tree
     * can outlive that bubble, so LazyBubble owns the box and points the real
     * bubble's HitBox() at it while the tree is cached. */
    ::ftxui::Box hit_box_;
};

/* The resident window: owns the descriptor index, the kMemory snapshots, and
 * `live_` -- the ONLY real Message instances. SetWindow() evicts everything
 * outside the new range, so their text and cached render trees go with them. */
class MessageWindow
{
  public:
    static constexpr size_t kMaxLive = 200; /* review decision, §8 */

    /* The window owns its descriptor index when no external one is supplied. */
    MessageWindow();
    explicit MessageWindow(std::vector<ChatBubbleRef> *refs);

    size_t LiveCount() const
    {
        return live_.size();
    }
    size_t RefCount() const
    {
        return refs_ ? refs_->size() : 0;
    }
    bool Empty() const
    {
        return !refs_ || refs_->empty();
    }

    /* Append a bubble that must NOT be persisted. Returns its
     * index; the real Message is built only when the index enters the window. */
    size_t AddMemoryBubble(const ChatTranscriptEntry &entry);
    /* The lightweight proxy for one slot. */
    std::shared_ptr<Message> Proxy(size_t index);

    ChatBubbleRef *Ref(size_t index);
    const ChatBubbleRef *Ref(size_t index) const;

    /* Materialize [first, last) plus prefetch, evicting the rest. */
    size_t SetWindow(size_t first, size_t last, size_t prefetch);

    /* The decoded fields for |index|. Used by LazyBubble to render itself;
     * returns false on a bad/missing line. */
    bool Fields(size_t index, std::string *kind, int *render_pref, std::string *task_id,
                std::string *header, std::string *content, std::string *highlight_lang);

    /* The real instance for |index|, or nullptr outside the window / on a bad
     * line. Shared ownership: the window keeps its own reference. */
    std::shared_ptr<Message> Get(size_t index);

    void Evict(size_t index);
    void Clear();

    bool Has(size_t index) const
    {
        return live_.find(index) != live_.end();
    }
    /* Visible index range of the last SetWindow call (for ChatWidget). */
    size_t first() const
    {
        return first_;
    }
    size_t last() const
    {
        return last_;
    }

  private:
    std::vector<ChatBubbleRef> *refs_owned_ = nullptr;
    std::vector<ChatBubbleRef> *refs_ = nullptr;
    std::vector<ChatTranscriptEntry> memory_;
    std::unordered_map<size_t, std::shared_ptr<Message>> live_;
    size_t first_ = 0;
    size_t last_ = 0;
};

} // namespace edc
