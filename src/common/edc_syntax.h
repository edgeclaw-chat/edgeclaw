#ifndef EDC_SYNTAX_H
#define EDC_SYNTAX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Syntax highlighting engine (tree-sitter based).
 *
 * Turns a source snippet + a language name into a list of byte ranges tagged
 * with a semantic group. Rendering (colour mapping) is deliberately NOT here:
 * callers map edc_syntax_group to their own colours. This keeps the engine
 * independent of the TUI and trivially testable.
 *
 * A returned span's [start_byte, end_byte) are byte offsets into the input
 * string, sorted ascending and non-overlapping. Unknown languages / query
 * errors / parse failures return false and leave |out| untouched, so callers
 * can fall back to unhighlighted text.
 */

typedef enum {
    EDC_SYNTAX_COMMENT = 0,
    EDC_SYNTAX_KEYWORD,
    EDC_SYNTAX_STRING,
    EDC_SYNTAX_NUMBER,
    EDC_SYNTAX_FUNCTION,
    EDC_SYNTAX_VARIABLE,
    EDC_SYNTAX_TYPE,
    EDC_SYNTAX_OPERATOR,
    EDC_SYNTAX_CONSTANT,
    EDC_SYNTAX_PUNCTUATION,
    EDC_SYNTAX_COUNT,
} edc_syntax_group;

typedef struct {
    uint32_t start_byte;
    uint32_t end_byte;
    edc_syntax_group group;
} edc_syntax_span;

typedef struct {
    edc_syntax_span *spans; /* owned; NULL when count == 0 */
    size_t count;
    size_t cap;
} edc_syntax_result;

/* True when |lang| has a registered grammar and highlight query. */
bool edc_syntax_available(const char *lang);

/* Highlight |code| (|len| bytes, need not be NUL-terminated).
 * Returns 0 on success (fills |out|, call edc_syntax_result_free) or -1 when
 * the language is unknown or highlighting failed. */
int edc_syntax_highlight(const char *code, size_t len, const char *lang,
                          edc_syntax_result *out);

void edc_syntax_result_free(edc_syntax_result *out);

/* Stable, human-readable name for a group (for tests / debugging). */
const char *edc_syntax_group_name(edc_syntax_group group);

#ifdef __cplusplus
}
#endif

#endif /* EDC_SYNTAX_H */
