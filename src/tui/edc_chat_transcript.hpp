#ifndef EDC_CHAT_TRANSCRIPT_HPP
#define EDC_CHAT_TRANSCRIPT_HPP

#include <string>

namespace edc
{

/* One bubble's decoded fields. The on-disk/wire form is a single tab-separated,
 * escaped line: kind \t render_pref \t task_id \t header \t content. The C
 * encoder (edc_chat_bubble_line.h) writes that line into chat_bubbles rows and
 * the codec here decodes it; there is no chat transcript file any more. */
struct ChatTranscriptEntry
{
    std::string kind;    /* user | tool | thought | reply | sys */
    int render_pref = 0; /* EDC_APP_RENDER_PLAIN / _MARKDOWN (see edc_api.h) */
    std::string task_id;
    std::string header;
    std::string content;
    std::string highlight_lang; /* language tag for tool/code output; '' when none */
};

/* Encode one bubble as an escaped, newline-terminated line. */
std::string EncodeTranscriptLine(const ChatTranscriptEntry &entry);

/* Decode one line (trailing newline/CR optional). False for a line that cannot be
 * a bubble -- a blank line, or a truncated field list. */
bool DecodeTranscriptLine(const std::string &line, ChatTranscriptEntry *out);

} // namespace edc

#endif
