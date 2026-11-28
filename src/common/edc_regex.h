#ifndef EDC_REGEX_H
#define EDC_REGEX_H

/* A thin wrapper over PCRE2 for the two "search with a user pattern" call sites
 * (grep and session-memory search). Every regex in the tree goes through PCRE2;
 * keeping the compile flags here stops the two callers from drifting (case folding,
 * UTF handling and the error contract must be identical on every platform). */
#include <stdbool.h>
#include <stddef.h>

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

struct edc_regex
{
    pcre2_code *code;
    pcre2_match_data *match_data;
};

/* Compile `pattern` as UTF-8 with Unicode character classes. `ignore_case` selects
 * case folding (grep honours its flag; session-memory search always folds). Returns
 * true on success; on failure `out_error` and `out_offset`, when non-NULL, receive
 * PCRE2's error code and byte offset. */
bool edc_regex_compile(struct edc_regex *re, const char *pattern, size_t length,
                       bool ignore_case, int *out_error, PCRE2_SIZE *out_offset);

/* True when `text` matches. The subject is measured in bytes (UTF-8 strings are fine). */
bool edc_regex_match(const struct edc_regex *re, const char *text, size_t length);

void edc_regex_free(struct edc_regex *re);

#endif /* EDC_REGEX_H */
