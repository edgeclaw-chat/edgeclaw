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

/* Why a check ended the way it did.
 *
 * This exists because `false` was answering two different questions. "There is no newer
 * release" and "the request was refused with a 403" both came back as `false`, and the
 * diagnostic printed the same line for both -- so a rate-limited install looked exactly like
 * an up-to-date one, which is the one distinction `--update-check` exists to make (see
 * edc_update_releases_url below, whose whole rationale is that the failure it diagnoses is
 * otherwise indistinguishable from "no update").
 *
 * The launch-time path still says nothing to the user: nothing here is actionable by whoever
 * is looking at the screen, and the next launch tries again. The outcome is for the
 * diagnostic, and for the log.
 */
typedef enum edc_update_outcome
{
    EDC_UPDATE_OK = 0,         /* a newer release for this platform is available */
    EDC_UPDATE_UP_TO_DATE,     /* the API answered; nothing newer than |current_version| */
    EDC_UPDATE_BAD_ARG,        /* a required argument was NULL or empty */
    EDC_UPDATE_NO_CLIENT,      /* the TLS client could not be created (unreadable CA bundle) */
    EDC_UPDATE_CONNECT_FAILED, /* DNS, TCP, or the TLS handshake */
    EDC_UPDATE_TIMEOUT,        /* no close event before the deadline */
    EDC_UPDATE_HTTP_STATUS,    /* answered with a status other than 200 */
    EDC_UPDATE_BODY_TOO_LARGE, /* the body passed |max_bytes| and was discarded */
    EDC_UPDATE_MALFORMED,      /* 200, but the body is not the release list */
} edc_update_outcome;

/* What the check ended with, and the status line behind it when there was one.
 *
 * |http_status| is 0 when no status line arrived (connect failure, timeout). A 403 is worth
 * naming on its own: it is what GitHub's unauthenticated rate limit (60 requests/hour per IP)
 * returns, and on a shared address it is the most likely reason for a check that "found
 * nothing". */
typedef struct edc_update_diag
{
    edc_update_outcome outcome;
    int http_status;
} edc_update_diag;

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
 * body larger than |max_bytes| -- returns false. That is the whole answer for the launch
 * path, which must never produce a user-visible error: there is nothing the user could do
 * about it, and the next launch tries again.
 *
 * For the diagnostic, |diag| (may be NULL) carries WHY. |diag->outcome| distinguishes "up to
 * date" from "could not determine", and |diag->http_status| names the 403. This variant
 * exists rather than an extra parameter on the one above so that the silent callers -- the
 * worker thread and the tests that pin "false and nothing else" -- keep exactly their old
 * contract.
 */
bool edc_update_check_ex(const char *cacert_path, const char *api_url, const char *current_version, bool allow_pre,
                         size_t max_bytes, int timeout_ms, sds *out_tag, sds *out_asset,
                         edc_update_diag *diag);

/* edc_update_check_ex with no diagnosis. The launch-time check uses this. */
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
