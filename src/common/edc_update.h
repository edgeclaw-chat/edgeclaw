#ifndef __EDC_UPDATE_H__
#define __EDC_UPDATE_H__

#include <stdbool.h>
#include <stdint.h>

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

/* Ask the release list whether a newer version exists.
 *
 * Runs off the UI thread (it blocks on a condition variable, the same shape as
 * edc_endpoint_probe). Fills |out_tag| / |out_asset| and returns true only when a newer
 * release for THIS platform is available and |allow_pre| admits it.
 *
 * |api_url| is parameterised rather than hardcoded so the tests can point it at a local
 * server; production passes the GitHub releases endpoint.
 *
 * Every failure -- DNS, TLS, a timeout, a non-200, a body that is not the JSON we expect, a
 * body larger than |max_bytes| -- returns false and says nothing. A background check must
 * never produce a user-visible error: there is nothing the user could do about it, and the
 * next launch tries again.
 */
bool edc_update_check(const char *cacert_path, const char *api_url, const char *current_version, bool allow_pre,
                      size_t max_bytes, int timeout_ms, sds *out_tag, sds *out_asset);

/* The release endpoint, honouring `EDGECLAW_UPDATE_RELEASES_URL`.
 *
 * Both callers read it here: the launch-time check and the `--update-check` diagnostic. The
 * diagnostic honouring it is what makes the command useful when GitHub is unreachable or
 * rate-limited -- it can be pointed at a local server, which is otherwise impossible because
 * the failure it exists to diagnose is indistinguishable from "no update".
 *
 * From the environment rather than config.json on purpose: config.json travels with a
 * workspace, and reading the endpoint from it would let a shared workspace redirect its own
 * updates. The environment is the one the process was started in. Caller frees. */
sds edc_update_releases_url(void);

/* The version to compare against, honouring `EDGECLAW_UPDATE_CURRENT_VERSION`.
 *
 * A development build reports its commit hash, which is not a version, so the check skips
 * itself and there is no way to exercise the path before a tag exists. This is the environment
 * spelling of what `--update-check --current` does explicitly. Returns |fallback| when unset. */
const char *edc_update_current_version(const char *fallback);

/* Delay before the launch-time request, honouring `EDGECLAW_UPDATE_CHECK_DELAY_MS`.
 *
 * Not politeness: at that instant the first model request is in flight, and a check competing
 * with it delays the first thing the user sees. A test asserting what happens AFTER the delay
 * should not have to sleep through it. */
uint64_t edc_update_check_delay_ms(void);

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
