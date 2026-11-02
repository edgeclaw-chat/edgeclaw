#ifndef __BC_TOKEN_ESTIMATE_H__
#define __BC_TOKEN_ESTIMATE_H__

#include <stdint.h>
#include <cJSON.h>
#include <stddef.h>

/* Heuristic token estimate (see the .c for the calibration and its limits). Used
 * only for "the tail after a provider's real count" and "the whole span when there
 * is no real count"; any real count (response usage) takes precedence. */

/* Estimate for a UTF-8 text span (excluding message structure overhead). */
extern int64_t edc_token_estimate_text(const char *s);

/* Estimate for one message (cJSON object): all its string values plus the
 * per-message structure overhead. */
extern int64_t edc_token_estimate_msg(const cJSON *msg);

/* Estimate for the [from, to) slice of a JSON message array (out-of-range clamped). */
extern int64_t edc_token_estimate_msgs(const cJSON *arr, size_t from, size_t to);

#endif
