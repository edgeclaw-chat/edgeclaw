#ifndef EDC_DATETIME_UTILS_H
#define EDC_DATETIME_UTILS_H

#include "sds.h"
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

/* The one wall-clock convention in the codebase: local time, "%Y-%m-%d %H:%M:%S".
 * `0` means "unknown" (an old archive that predates the field) and renders as
 * "unknown", never as 1970. Callers that only show a human-glanceable moment use
 * the minute form. NOTE: this clock can be wrong (the host may freeze the guest
 * clock on sleep, see the repo's clock-drift note) -- treat every timestamp as a
 * hint, and keep monotonic event seq as the authoritative order. */

/** The local wall-clock string (kept for existing callers). */
sds edc_datetime_timestamp(void);

/** Seconds since the epoch, or 0 if the clock is unavailable. */
time_t edc_datetime_now(void);

/** Milliseconds since the epoch, for identities that must be unique within a
 *  process. Unlike clock_gettime() this has no extra runtime dependency on
 *  Windows (see the implementation note). */
int64_t edc_datetime_now_ms(void);

/** Format |t| as "%Y-%m-%d %H:%M:%S"; |t| == 0 -> "unknown". Caller sdsfree. */
sds edc_datetime_format(time_t t);

/** Format |t| as "%Y-%m-%d %H:%M" (a compact form for indexes/notices);
 *  |t| == 0 -> "unknown". Caller sdsfree. */
sds edc_datetime_format_min(time_t t);

/** Parse a "%Y-%m-%d %H:%M:%S" string (what messages carry). Returns false and leaves
 *  *out untouched when it cannot be parsed. */
bool edc_datetime_parse(const char *s, time_t *out);

#endif
