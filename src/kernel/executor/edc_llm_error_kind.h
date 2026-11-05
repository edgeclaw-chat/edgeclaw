/* edc_llm_error_kind.h -- tell a context overflow apart from every other failure.
 *
 * The provider is the only one who knows why a request failed, and its structured
 * fields cannot be trusted: a real 400 came back with `type` and `code` both reading
 * the generic `invalid_request_error`, with the window spelled out only in the
 * message ("This model's maximum context length is 1048576 tokens. However, you
 * requested 2011367 tokens ... Please reduce the length of the messages"). So the
 * message is the primary key and `http_status` only breaks ties -- the table below comes
 * from measurements on real responses.
 *
 * A pure function: no I/O, no state, no allocation, so the whole table is testable
 * by feeding it strings.
 */
#ifndef EDC_LLM_ERROR_KIND_H
#define EDC_LLM_ERROR_KIND_H

typedef enum
{
    EDC_LLM_ERR_OTHER = 0,        /* anything we cannot name */
    EDC_LLM_ERR_CONTEXT_OVERFLOW, /* the request did not fit the model's window */
    EDC_LLM_ERR_RATE_LIMIT,       /* throttled -- explicitly *not* an overflow */
    EDC_LLM_ERR_AUTH,             /* missing/bad key, or not entitled to the model */
    EDC_LLM_ERR_TRANSPORT,        /* no HTTP response at all: DNS/TCP/TLS/timeout */
} edc_llm_error_kind;

/* `http_status` is 0 when no response line ever arrived. `message` may be NULL.
 * Matching is ASCII case-insensitive. */
edc_llm_error_kind edc_llm_error_classify(int http_status, const char *message);

const char *edc_llm_error_kind_name(edc_llm_error_kind kind);

#endif /* EDC_LLM_ERROR_KIND_H */
