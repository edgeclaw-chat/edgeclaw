#ifndef EDC_CHAT_TRANSCRIPT_HPP
#define EDC_CHAT_TRANSCRIPT_HPP

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

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

/* Progressive replay (startup). A long log must not be decoded in one blocking
 * step, so the window paints only the newest bubbles first and backfills the rest.
 *
 * LoadTranscriptTail reads the last up-to-|max_entries| whole bubbles into |out|
 * (file order) and reports where the older history ends: |prefix_end| is the byte
 * offset of the first returned bubble, or 0 when the file holds no more than
 * |max_entries| (nothing to backfill). Only the tail of the file is read -- the
 * caller pays for the lines it shows, not for the whole log. */
size_t LoadTranscriptTail(const std::string &path, size_t max_entries, size_t *prefix_end,
                          std::vector<ChatTranscriptEntry> *out);

/* Decode the older history in [0, |prefix_end|) (|prefix_end| from
 * LoadTranscriptTail, always a line boundary) into |out|, in file order. */
size_t LoadTranscriptPrefix(const std::string &path, size_t prefix_end, std::vector<ChatTranscriptEntry> *out);

/* As LoadTranscriptPrefix, but |cancelled| is polled while decoding: when it returns
 * true the decode stops early, keeping whatever it decoded so far. The startup
 * backfill uses this so a session switch abandons a long decode promptly instead of
 * letting it run to completion. */
size_t LoadTranscriptPrefix(const std::string &path, size_t prefix_end, std::vector<ChatTranscriptEntry> *out,
                            const std::function<bool()> &cancelled);

/* True when |path| exists and does not end in a newline: the previous process died
 * mid-append, so the next append must first re-terminate that fragment. */
bool TranscriptEndsMidLine(const std::string &path);

} // namespace edc

#endif
