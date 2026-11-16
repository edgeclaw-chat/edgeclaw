#pragma once

// Windowed chat log: the transcript file is the view-state authority, and only a
// bounded window of Message instances is ever materialized.
//
// See docs/tui-windowing-proposal.md. The point of this module is that a chat log
// must cost O(viewport) resident memory, not O(history): a transcript with 40k
// bubbles produces 40k small ChatBubbleRef descriptors but at most kMaxLive
// Message objects (each of which owns its text AND a cached ftxui render tree).
//
// This header is deliberately free of ftxui component/screen types: the index
// and the window are plain data, so they are testable without a terminal.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace edc
{

class Message;

/* One bubble's place in the transcript, without its text.
 *
 * `offset`/`length` address exactly one encoded line in the file, so Load() is
 * a pread of a single line followed by DecodeTranscriptLine(). Nothing here
 * duplicates the content: the file is the only copy. */
struct ChatBubbleRef
{
    uint64_t offset = 0;
    uint32_t length = 0;
    uint8_t kind = 0;        /* ChatBubbleKind */
    uint8_t render_pref = 0; /* EDC_APP_RENDER_PLAIN / _MARKDOWN */
    uint32_t task_id_off = 0;
    uint32_t task_id_len = 0;
    uint32_t header_len = 0;  /* encoded field lengths, for height estimates */
    uint32_t content_len = 0;
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

/* Scan an append-only transcript once and build the descriptor index. Only the
 * first four tab-separated fields are split; the content span is recorded, never
 * copied. A trailing line without '\n' (a crash mid-append) is dropped, matching
 * DecodeTranscriptLine/LoadTranscript's rule. Returns the number of descriptors.
 *
 * This is the ONLY full-file pass. It is O(file bytes) in time and O(n) in small
 * descriptors -- not O(n) in messages. */
size_t BuildChatBubbleRefs(const std::string &path, std::vector<ChatBubbleRef> *out);

/* Decode one indexed line into the five transcript fields. False when the line
 * is malformed or the file changed underneath us. */
bool DecodeChatBubbleAt(const std::string &path, const ChatBubbleRef &ref, std::string *kind,
                        int *render_pref, std::string *task_id, std::string *header, std::string *content);

/* The resident window.
 *
 * `live_` holds the only Message instances: indices inside [first, last] (plus
 * prefetch). Every SetWindow() call evicts everything outside the new range, so
 * their cached render trees go with them. The index is borrowed, not owned. */
class MessageWindow
{
  public:
    /* How many instances may be alive at once (review decision, §8). */
    static constexpr size_t kMaxLive = 200;

    explicit MessageWindow(std::string path, std::vector<ChatBubbleRef> *refs);

    /* Number of resident Message instances. Bounded by kMaxLive. */
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
    const std::string &path() const
    {
        return path_;
    }

    /* Borrowed descriptor (mutable so expanded/measured_h can be updated). */
    ChatBubbleRef *Ref(size_t index);
    const ChatBubbleRef *Ref(size_t index) const;

    /* Materialize window [first, last) (plus up to one screen of prefetch on each
     * side), evicting everything else. `prefetch` is in items, clamped so that the
     * total never exceeds kMaxLive. Returns the number of instances now alive. */
    size_t SetWindow(size_t first, size_t last, size_t prefetch);

    /* The instance for |index|, or nullptr when it cannot be decoded. Loads on
     * demand. Shared ownership: the window keeps its own reference, and a caller
     * that holds the returned shared_ptr keeps the bubble alive even after the
     * window evicts it (ChatModel's all_ slots do exactly that). */
    std::shared_ptr<Message> Get(size_t index);

    /* Evict one index (used when its expanded state or height changes). */
    void Evict(size_t index);
    void Clear();

    /* For tests/diagnostics: is |index| currently materialized? */
    bool Has(size_t index) const
    {
        return live_.find(index) != live_.end();
    }

  private:
    std::string path_;
    std::vector<ChatBubbleRef> *refs_; /* borrowed */
    std::unordered_map<size_t, std::shared_ptr<Message>> live_;
    size_t first_ = 0;
    size_t last_ = 0;
};

} // namespace edc
