#ifndef EDC_CHAT_TRANSCRIPT_HPP
#define EDC_CHAT_TRANSCRIPT_HPP

#include <functional>
#include <string>

namespace edc
{

/* The chat transcript is the TUI's own record of what it put on screen: one line
 * per bubble, tab-separated kind / render_pref / task_id / header / content, with
 * `\\`, `\n`, `\r` and `\t` escaped inside every field.
 *
 * It is deliberately not derived from session.json: session.json is the model
 * state (every message, visible or not), while this is the view state. Replaying
 * session.json resurrects messages that were never on screen; replaying this file
 * reproduces the pre-exit log verbatim.
 *
 * The format is frozen: transcripts written by older builds stay readable. */
struct ChatTranscriptEntry
{
    std::string kind;    /* user | tool | thought | reply | sys */
    int render_pref = 0; /* EDC_APP_RENDER_PLAIN / _MARKDOWN (see edc_api.h) */
    std::string task_id;
    std::string header;
    std::string content;
};

/* Encode one bubble as a transcript line (newline included). */
std::string EncodeTranscriptLine(const ChatTranscriptEntry &entry);

/* Decode one line (trailing newline/CR optional). False for a line that cannot be
 * a bubble -- a blank line, or the partial tail a crash can leave behind. */
bool DecodeTranscriptLine(const std::string &line, ChatTranscriptEntry *out);

/* Feed every decodable line to |emit| in file order. Returns how many entries were
 * emitted: 0 means "no usable transcript" (the caller's cue to bootstrap). A
 * trailing line without its newline is a fragment of an append that never finished
 * and is dropped, never merged with the next append. */
size_t LoadTranscript(const std::string &path, const std::function<void(const ChatTranscriptEntry &)> &emit);

/* True when |path| exists and does not end in a newline: the previous process died
 * mid-append, so the next append must first re-terminate that fragment. */
bool TranscriptEndsMidLine(const std::string &path);

} // namespace edc

#endif
