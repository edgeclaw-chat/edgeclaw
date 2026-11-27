#pragma once

/* Wire shape for the chat-bubble page protocol.
 *
 * The UI never touches the state store (see edc_state_store.h: only the
 * scheduler actor thread may). It sends a ChatBubblePageRequest through the
 * channel actor and receives a ChatBubblePageReply as a channel message.
 * Both halves are JSON sds so the C core can build the reply without knowing
 * any C++ types, and the TUI can parse it without knowing the DB.
 *
 * `line` is an ENCODED transcript line verbatim; the UI decodes it with
 * DecodeTranscriptLine. No escape rule is duplicated on the wire.
 */

struct edc_chat_bubble_row;

#include <cstdint>
#include <string>
#include <vector>

namespace edc
{

struct ChatBubblePageRequest
{
    std::string session_id;
    /* 0 (or any non-positive value) means "the newest page". A positive
     * seq is an exclusive cursor: return rows with seq < before_seq.
     * Never use INT64_MAX here: the wire format encodes numbers as JSON
     * doubles, and (double)INT64_MAX rounds to 2^63 -- casting that back to
     * int64_t is UB and saturates differently on amd64 vs aarch64. */
    int64_t before_seq = 0;
    int limit = 200;
};

struct ChatBubbleRow
{
    int64_t seq = 0;
    std::string kind;
    int render_pref = 0;
    std::string task_id;
    std::string header;
    std::string content;
    std::string highlight_lang;
};

struct ChatBubblePageReply
{
    std::string session_id;
    int64_t before_seq = 0; /* the cursor this page answers, echoed for ordering */
    std::vector<ChatBubbleRow> rows;
    bool has_more = false;
};

/* Request: C++ (UI) -> JSON string. The C boundary converts it to sds. */
std::string EncodeChatBubblePageRequest(const ChatBubblePageRequest &request);
/* Request: C (scheduler actor) -> fields. False on malformed input. */
bool DecodeChatBubblePageRequest(const char *json, ChatBubblePageRequest *out);

/* Reply: C (scheduler actor) -> JSON string. sds.h cannot be included from C++
 * (the SDS_HDR_VAR macro does an implicit void* conversion), so the C boundary
 * calls this with plain char buffers and wraps the result in sds itself. */
std::string EncodeChatBubblePageReply(const char *session_id, int64_t before_seq,
                                      const char *const *lines, const int64_t *seqs,
                                      int n, bool has_more);
/* Reply: C++ (UI) -> fields. False on malformed input. */
bool DecodeChatBubblePageReply(const char *header, const char *json, ChatBubblePageReply *out);

} // namespace edc
