#include "edc_token_estimate.h"

#include <string.h>

/* Token estimation, pi-shaped: a provider's real usage is always the anchor; this
 * heuristic only fills in what was appended after it, because "the last reply's
 * usage" answers "how big was that request", never "how big is the context now".
 *
 * Calibration: ASCII at ~4 chars/token, other characters (mostly CJK) at ~1
 * char/token; chars/4 alone underestimates Chinese by 3-4x, and much of our prompt
 * is Chinese.
 *
 * It is a heuristic, not a measurement: never present it as provider truth (the
 * caller is responsible for the estimate marker). */

/* Per-message structure overhead: role / name / separators / JSON framing. */
#define EDC_TOKEN_MSG_OVERHEAD 8

static void est_count(const char *s, long long *ascii, long long *wide)
{
    if (!s)
        return;
    for (const unsigned char *p = (const unsigned char *)s; *p;)
    {
        if (*p < 0x80)
        {
            (*ascii)++;
            p++;
        }
        else
        {
            /* One non-ASCII character (all its UTF-8 bytes count as one). */
            (*wide)++;
            p++;
            while ((*p & 0xc0) == 0x80)
                p++;
        }
    }
}

/* Walk all VALUES of a cJSON subtree, accumulating strings. Two things are not
 * counted:
 *   - key names: framing already covered by the per-message overhead;
 *   - `ts` / `render` / `id`: metadata we attach (by edc_turn_add_message); the provider's
 *     template does not know them, so counting them would add ~6 tokens per message.
 *     `id` belongs here for the same reason `ts` does -- it is attached by the same
 *     function, is stripped before the request is sent (kWireMessageKeys in
 *     edc_llm_actor.c), and a 16-char id would otherwise add 4 phantom tokens each. */
static void est_walk_values(const cJSON *node, long long *ascii, long long *wide)
{
    for (const cJSON *c = node; c; c = c->next)
    {
        if (c->string && (strcmp(c->string, "ts") == 0 || strcmp(c->string, "render") == 0 ||
                          strcmp(c->string, "id") == 0))
            continue;
        if (cJSON_IsString(c))
            est_count(c->valuestring, ascii, wide);
        else if (cJSON_IsArray(c) || cJSON_IsObject(c))
            est_walk_values(c->child, ascii, wide);
    }
}

long long edc_token_estimate_text(const char *s)
{
    if (!s)
        return 0;
    long long ascii = 0, wide = 0;
    est_count(s, &ascii, &wide);
    return ascii / 4 + wide;
}

long long edc_token_estimate_msg(const cJSON *msg)
{
    if (!msg)
        return 0;
    long long ascii = 0, wide = 0;
    est_walk_values(msg->child, &ascii, &wide);
    return EDC_TOKEN_MSG_OVERHEAD + ascii / 4 + wide;
}

long long edc_token_estimate_msgs(const cJSON *arr, size_t from, size_t to)
{
    if (!arr || to <= from)
        return 0;

    long long total = 0;
    size_t i = 0;
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, arr)
    {
        if (i >= to)
            break;
        if (i >= from)
            total += edc_token_estimate_msg(item);
        i++;
    }
    return total;
}
