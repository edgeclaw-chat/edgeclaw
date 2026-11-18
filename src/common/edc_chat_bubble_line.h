#ifndef EDC_CHAT_BUBBLE_LINE_H
#define EDC_CHAT_BUBBLE_LINE_H

#ifdef __cplusplus
extern "C"
{
#endif

/* The transcript's line format, shared by the C producer (channel actor, which
 * writes chat_bubbles rows) and the C++ consumer (edc_chat_transcript.cpp).
 *
 * One line = one bubble, five tab-separated fields:
 *   kind \t render_pref \t task_id \t header \t content \n
 * with '\\' -> "\\\\", '\n' -> "\\n", '\r' -> "\\r", '\t' -> "\\t" inside every
 * field. Keeping ONE encoder in C avoids the classic drift where the writer
 * escapes slightly differently from the reader and tool output silently loses
 * its newlines after a restart.
 *
 * Returns a malloc'd NUL-terminated line WITHOUT the trailing newline (the
 * caller stores it as a DB row; the file importer historically had a '\n' and
 * strips it, so both authorities hold the same bytes). NULL on OOM. */
char *edc_chat_bubble_encode(const char *kind, int render_pref, const char *task_id,
                             const char *header, const char *content);

#ifdef __cplusplus
}
#endif

#endif /* EDC_CHAT_BUBBLE_LINE_H */
