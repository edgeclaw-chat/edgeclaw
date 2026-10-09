/* edc_llm_error_kind.c -- see the header for why the message is the primary key. */
#include "edc_llm_error_kind.h"
#include <stdbool.h>

/* ASCII case-insensitive substring search. strcasestr is a GNU extension and the
 * Windows build has no <strings.h> to fall back on, so it is spelled out. */
static bool contains_ci(const char *hay, const char *needle)
{
    if (!hay || !needle || !*needle)
        return false;
    for (; *hay; hay++)
    {
        const char *h = hay;
        const char *n = needle;
        for (; *h && *n; h++, n++)
        {
            char a = *h;
            char b = *n;
            if (a >= 'A' && a <= 'Z')
                a += 'a' - 'A';
            if (b >= 'A' && b <= 'Z')
                b += 'a' - 'A';
            if (a != b)
                break;
        }
        if (!*n)
            return true;
    }
    return false;
}

edc_llm_error_kind edc_llm_error_classify(int http_status, const char *message)
{
    const char *m = message ? message : "";

    /* No status line means no response ever arrived: the transport failed before the
     * provider could say anything, whatever the error text happens to contain. */
    if (http_status == 0)
        return EDC_LLM_ERR_TRANSPORT;

    /* Throttling first, and explicitly: 429 bodies do talk about tokens and lengths,
     * and pi treats "rate limit" as the counterexample that must not classify as an
     * overflow. Only after this branch can a length word mean "too much context". */
    if (http_status == 429 || contains_ci(m, "rate limit") || contains_ci(m, "too many requests"))
        return EDC_LLM_ERR_RATE_LIMIT;

    /* The measured feature strings. Kept as a plain string table on purpose: a new
     * provider way of saying "too long" is one line here, and the tests feed these
     * exact phrases through. */
    if (contains_ci(m, "maximum context length") || contains_ci(m, "reduce the length of the messages") ||
        contains_ci(m, "context length exceeded") || contains_ci(m, "context_length_exceeded") ||
        contains_ci(m, "prompt is too long") || contains_ci(m, "too many tokens"))
        return EDC_LLM_ERR_CONTEXT_OVERFLOW;

    if (http_status == 401 || http_status == 403 || contains_ci(m, "authentication") ||
        contains_ci(m, "invalid api key") || contains_ci(m, "unauthorized"))
        return EDC_LLM_ERR_AUTH;

    return EDC_LLM_ERR_OTHER;
}

const char *edc_llm_error_kind_name(edc_llm_error_kind kind)
{
    switch (kind)
    {
    case EDC_LLM_ERR_CONTEXT_OVERFLOW:
        return "CONTEXT_OVERFLOW";
    case EDC_LLM_ERR_RATE_LIMIT:
        return "RATE_LIMIT";
    case EDC_LLM_ERR_AUTH:
        return "AUTH";
    case EDC_LLM_ERR_TRANSPORT:
        return "TRANSPORT";
    case EDC_LLM_ERR_OTHER:
    default:
        return "OTHER";
    }
}
