#ifndef __EDC_UPDATE_H__
#define __EDC_UPDATE_H__

#include <stdbool.h>

#include "sds.h"

/* Is there a newer release than this build?
 *
 * The check runs ONCE per process, shortly after startup, and only reports. Downloading and
 * replacing the binary is a separate path (edc_selfreplace) that the user has to confirm;
 * nothing here writes to disk.
 *
 * Two states decide whether anything happens at all:
 *   - `update_check` false          -> no network, ever
 *   - the running version is not a parseable tag (the `dev` builds) -> no offer, because
 *     there is nothing to compare against and a developer build must not try to replace
 *     itself.
 */

/* The release asset target for THIS build, e.g. "edgeclaw_amd64_linux". Compile-time, and
 * kept in step with tools/install.sh's table: a wrong answer here downloads another
 * platform's binary, which the pre-replace `--version` probe would then catch -- but only
 * after spending the bandwidth. */
const char *edc_update_target(void);

/* Pick the release to offer, from the JSON returned by
 * `GET /repos/<owner>/<repo>/releases`.
 *
 * Pure: no I/O, no globals, so the selection rules are testable without a network.
 *
 * Returns true and fills |out_tag| (e.g. "v1.0.1") and |out_asset| (the archive name for
 * this build) when a release is both newer than |current_version| and ships the asset this
 * platform needs. |allow_pre| admits pre-releases; false keeps them out, which is what a
 * stable build should do.
 *
 * Returns false when nothing qualifies: no newer release, no asset for this platform, or
 * unparseable input. Callers must treat false as "say nothing", not as an error.
 */
bool edc_update_pick(const char *releases_json, const char *current_version, bool allow_pre, sds *out_tag,
                     sds *out_asset);

#endif /* __EDC_UPDATE_H__ */
