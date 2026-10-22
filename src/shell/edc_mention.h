#ifndef __EDC_MENTION_H__
#define __EDC_MENTION_H__
/* ── `@reference` scanning: ONE implementation, two consumers ─────────────
 *
 * The mention pass (src/shell/edc_pass_mentions.c) decides what an `@…` means, and the
 * TUI (src/tui/edc_token_style.cpp) paints it. Those two used to walk the text with two
 * separate scanners, which is how a fix in one could silently disagree with the other --
 * the input box would colour `@"my file.txt"` as one reference while the pass read a file
 * called `"my`. They now both call this.
 *
 * NOT a second quoting dialect: a quoted reference is decoded by edc_cmd_unquote(), the
 * same function the lexer uses. What lives here is only the `@`-specific part -- where a
 * reference starts, where it ends, and what happens to trailing punctuation.
 *
 * Note on why the LEXER cannot produce mention tokens (the alternative considered):
 * a message's text is never lexed. "A line that is not a shell line is one task's input,
 * verbatim, and nothing inside it is parsed" is a load-bearing rule (docs/input-
 * pipeline.zh.md §2), so `看下 "this" 和 @a.c` keeps its quotes as literal characters. A
 * mention token would therefore have to come from lexing message text, i.e. from breaking
 * that rule. Scanning here, with the lexer's own unquote, is the version that does not.
 */
#include <stdbool.h>
#include <stddef.h>

/* This header is also read by C++ (the input line and the chat bubbles paint references),
 * and the definitions live in a C translation unit: without this the linker looks for
 * mangled names and finds nothing. */
#ifdef __cplusplus
extern "C"
{
#endif

/* Where does the reference at |at| end, and what does it name?
 *
 * |text[at]| must be '@', and a reference only starts at the beginning of the text or
 * after whitespace (so `foo@bar.com` and `@Override` stay ordinary words) -- the callers
 * check that prefix condition, since both already iterate the text.
 *
 * Returns the byte length of the reference INCLUDING the '@'; 0 means "not a reference"
 * (a bare `@`, an unterminated quote, nothing but punctuation).
 *
 * A quoted body (`@"my file.txt"`, `@'my file.txt'`, `@$'…'`) names exactly the text
 * inside the quotes: quotes are delimiters, and trailing punctuation is KEPT because the
 * user delimited it. An unquoted body ends at whitespace and gives back trailing ASCII
 * punctuation (`,.):;!?]}`) to the sentence, so `看下 @a.c.` names `a.c`.
 *
 * |out_token| (optional) receives the text to resolve: quotes removed, a `#start[-end]`
 * line range still attached (it may follow the closing quote too). It is an sds -- the
 * caller frees it with sdsfree(). Declared as `char *` rather than `sds` so this header
 * stays usable from C++: sds.h is not C++-clean (its header accessors cast void*).
 * |out_self_closed| (optional) is true when a closing quote ended the reference, which
 * tells the display layer that no whitespace or punctuation is needed after it.
 */
size_t edc_mention_parse(const char *text, size_t at, char **out_token, bool *out_self_closed);

/* The NAME a reference resolves to, for the display layer: quotes decoded and the
 * `#range` suffix removed, so `@"my file.txt"#2-3` marks the file `my file.txt`.
 * Returns a malloc'd NUL-terminated string (free with free()), or NULL when |text[at]|
 * does not start a reference. Same implementation as edc_mention_parse(), so the mark and
 * the expansion cannot disagree about what a reference names. */
char *edc_mention_name(const char *text, size_t at);

/* Is |c| the ASCII punctuation an unquoted reference gives back to the sentence? Exported
 * because the display layer asks the same question when deciding whether a mention is
 * already finished (edc_token_style.cpp: ClosedAfter). */
bool edc_mention_is_trailing_punct(char c);

/* Is |c| whitespace for reference purposes? Same set both consumers use, exported so the
 * two cannot drift (` `, `\t`, `\n`, `\r`). */
bool edc_mention_is_space(char c);

#ifdef __cplusplus
}
#endif

#endif /* __EDC_MENTION_H__ */
