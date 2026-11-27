#ifndef EDC_CHAT_BUBBLE_LINE_H
#define EDC_CHAT_BUBBLE_LINE_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* The chat_bubbles row format, shared by the C producer (channel actor, which
 * writes chat_bubbles rows) and the C++ consumer (edc_chat_transcript.cpp).
 *
 * One line = one bubble, five tab-separated fields:
 *   kind \t render_pref \t task_id \t header \t content
 * with '\\' -> "\\\\", '\n' -> "\\n", '\r' -> "\\r", '\t' -> "\\t" inside every
 * field. Keeping ONE encoder in C avoids the classic drift where the writer
 * escapes slightly differently from the reader and tool output silently loses
 * its newlines after a restart.
 *
 * Returns a malloc'd NUL-terminated line WITHOUT the trailing newline. NULL on
 * OOM. */
char *edc_chat_bubble_encode(const char *kind, int render_pref, const char *task_id,
                             const char *header, const char *content, const char *highlight_lang);

/* Decode an encoded line into its raw fields. Accepts both the legacy V18
 * five-field form (kind\tpref\ttask_id\theader\tcontent) and the new six-field
 * form with a trailing \thighlight_lang. Used by the v18->v19 migration and by
 * the TUI's in-memory row representation. Every out* is malloc'd and must be
 * freed by the caller; returns false for a truncated/malformed line. A trailing
 * newline/CR is tolerated. */
bool edc_chat_bubble_decode_line(const char *line, char **kind, int *render_pref,
                                 char **task_id, char **header, char **content,
                                 char **highlight_lang);

#ifdef __cplusplus
}
#endif

#endif /* EDC_CHAT_BUBBLE_LINE_H */
