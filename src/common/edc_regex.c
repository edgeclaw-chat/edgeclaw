#include "edc_regex.h"

/* These flags are the project-wide answer to "how do we compile a user regex?":
 *   - PCRE2_UTF: patterns and subjects are UTF-8; quantifiers count characters, not bytes.
 *   - PCRE2_UCP: \d / \w / \s and their negatives use Unicode properties.
 *   - PCRE2_CASELESS: added only when the caller asks for case folding (grep's
 *     `ignore_case` flag; session-memory search always asks for it).
 * Before this helper the two callers used POSIX regcomp(REG_EXTENDED|REG_ICASE|...) on
 * POSIX and PCRE2's POSIX wrapper on Windows, so the effective syntax differed per OS.
 * Compiling here keeps the semantics identical everywhere. */
#define EDC_REGEX_OPTIONS (PCRE2_UTF | PCRE2_UCP)

bool edc_regex_compile(struct edc_regex *re, const char *pattern, size_t length,
                       bool ignore_case, int *out_error, PCRE2_SIZE *out_offset)
{
    re->code = NULL;
    re->match_data = NULL;

    uint32_t options = EDC_REGEX_OPTIONS | (ignore_case ? PCRE2_CASELESS : 0);
    int err = 0;
    PCRE2_SIZE off = 0;
    pcre2_code *code = pcre2_compile((PCRE2_SPTR)pattern, (PCRE2_SIZE)length,
                                     options, &err, &off, NULL);
    if (!code)
    {
        if (out_error)
            *out_error = err;
        if (out_offset)
            *out_offset = off;
        return false;
    }

    pcre2_match_data *md = pcre2_match_data_create_from_pattern(code, NULL);
    if (!md)
    {
        pcre2_code_free(code);
        if (out_error)
            *out_error = PCRE2_ERROR_NOMEMORY;
        if (out_offset)
            *out_offset = 0;
        return false;
    }

    re->code = code;
    re->match_data = md;
    return true;
}

bool edc_regex_match(const struct edc_regex *re, const char *text, size_t length)
{
    /* pcre2_match returns >= 0 on a match, PCRE2_ERROR_NOMATCH on a clean miss, and
     * a different negative value on an actual error: only a positive result counts. */
    return pcre2_match(re->code, (PCRE2_SPTR)text, (PCRE2_SIZE)length, 0, 0,
                       re->match_data, NULL) >= 0;
}

void edc_regex_free(struct edc_regex *re)
{
    if (re->match_data)
        pcre2_match_data_free(re->match_data);
    if (re->code)
        pcre2_code_free(re->code);
    re->code = NULL;
    re->match_data = NULL;
}
