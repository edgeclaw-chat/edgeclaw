#ifndef EDC_CHAT_TRANSCRIPT_HPP
#define EDC_CHAT_TRANSCRIPT_HPP

#include <functional>
#include <string>

namespace edc
{

/* The chat transcript is the TUI's own record of what it put on screen: one line
 * per bubble, tab-separated kind / render_pref / task_id / header / content,
 * with `\\`, `\n`, `\r` and `\t` escaped inside every field.
 *
 * It is deliberately *not* derived from session.json: the two record different
 * things. session.json is the model state (every message, whether or not it was
 * ever visible), while this is the view state — a background task's output never
 * appears in the displayed tab, and a task superseded mid-run never shows its
 * reply, yet both are in session.json. Replaying session.json therefore
 * resurrects messages that were never on screen; replaying this file reproduces
 * the pre-exit log verbatim.
 *
 * The format is frozen: transcripts written by older builds (and by a still
 * running older process) stay readable. */
struct ChatTranscriptEntry
{
    std::string kind;    /* user | tool | thought | reply | sys */
    int render_pref = 0; /* EDC_C_RENDER_PLAIN / _MARKDOWN (see edc_c_bridge.h) */
    std::string task_id;
    std::string header;
    std::string content;
};

/* Encode one bubble as a transcript line (newline included). */
std::string EncodeTranscriptLine(const ChatTranscriptEntry &entry);

/* Decode one line (the trailing newline/CR is optional). Returns false for a
 * line that cannot be a bubble — a blank line, or the partial tail a crash can
 * leave behind. */
bool DecodeTranscriptLine(const std::string &line, ChatTranscriptEntry *out);

/* Feed every decodable line to |emit| in file order, skipping the undecodable
 * ones. Returns how many entries were emitted: 0 means "no usable transcript"
 * (missing file, or nothing in it), which is the caller's cue to bootstrap.
 *
 * A trailing line without its terminating newline is a fragment of an append
 * that never finished (the process died mid-write); it is dropped, never
 * merged with whatever the next append writes after it. */
size_t LoadTranscript(const std::string &path, const std::function<void(const ChatTranscriptEntry &)> &emit);

/* True when |path| exists and does not end in a newline: the previous process
 * died mid-append. The next append must first re-terminate that fragment, or the
 * two bubbles would merge into one undecodable line. */
bool TranscriptEndsMidLine(const std::string &path);

} // namespace edc

#endif
