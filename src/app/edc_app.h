#ifndef __EDC_APP_H__
#define __EDC_APP_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>

/* ── The application instance (composition root) ──────────────
 *
 * `struct edc_app` assembles config / session_manager / toolbox / scheduler /
 * actors / runner, injects the scheduler's ops and installs the userland command
 * handlers (all inside `edc_app_new()`). It is visible only here:
 *   - defined in `edc_app.c` (opaque): outside code cannot reach its fields and
 *     borrow parts of the assembly;
 *   - userland (commands / shell) does not know it: it receives exactly the
 *     pieces it needs (the signature of `edc_commands_handler` is the roster).
 *
 * This header holds the root's own job: create / shutdown / free, plus
 * `set_llm_config` (which IS business: write config.json + notify llm_actor) and
 * two handle accessors (config / channel_actor). The frontend-facing surface
 * (send input / request tree / register adapter / read config strings / message
 * read-only views) is NOT here but in `edc_api.h` (`//src/app:api`): those are
 * just the C++ <-> C boundary projection, not part of the root.
 *
 * ── C++-safe ─────────────────────────────────────────────────
 * C++ can include this header and call these directly. One hard rule: include no
 * header that pulls in sds -- `sds.h`'s SDS_HDR_VAR macro does not compile as C++
 * (`invalid conversion from 'void*' to 'sdshdr8*'`; cJSON/uthash are fine). So:
 * signatures use only bool / int / char* / opaque pointers, and every "sds ->
 * caller buffer" read lives in `edc_api.h`.
 */
struct edc_app;

/* Labels used in prototypes must be declared once here: otherwise GCC treats
 * `struct edc_scheduler` in a parameter list as a different prototype-scope type
 * and the definition reports "conflicting types". */
struct edc_actor;
struct edc_config;

/* |sandbox_override|: -1 = keep the persisted config.json value (the startup
 * default), 0 = force off, 1 = force on. Per-run, NOT persisted; only the in-TUI
 * /sandbox command writes config.json. */
extern struct edc_app *edc_app_new(bool setup, int sandbox_override);

/* ── the release check ───────────────────────────────────────────────────────
 *
 * A newer release was found. |tag| is only valid for the duration of the call. Runs on the
 * actor-loop thread, so an observer that touches UI state must marshal it onto its own
 * thread (the TUI posts through its dispatcher) -- and must not block: it is called from the
 * loop that runs every actor.
 *
 * A function-pointer hook rather than a channel message, because this is not conversation
 * traffic: it has no session, no group and no task, and inventing one of those to carry it
 * would put a "system" bubble in the transcript for something that belongs outside it. */
typedef void (*edc_update_found_observer)(const char *tag, void *ud);
extern void edc_app_set_update_observer(struct edc_app *app, edc_update_found_observer fn, void *ud);

/* Whether this copy can replace itself: the installer put it there, and the platform has one
 * (see edc_upgrade_detect / edc_upgrade_supported). False means "tell the user to do it by
 * hand", never "try anyway". */
extern bool edc_app_update_can_self_upgrade(const struct edc_app *app);

/* Start the upgrade to |tag|. Returns false when nothing was started (already running, not
 * upgradable, no thread); the dialog then shows the manual command instead.
 *
 * |on_done| runs on the loop thread exactly once with the outcome. `edc_app_update_cancel`
 * abandons an upgrade in flight -- the process is killed if still running, and |on_done| may
 * or may not have fired by then, which is why the confirmation tells the user what abandoning
 * means before they start. */
typedef void (*edc_upgrade_done_observer)(bool ok, const char *detail, void *ud);
extern bool edc_app_update_start(struct edc_app *app, const char *tag, edc_upgrade_done_observer on_done, void *ud);
extern void edc_app_update_cancel(struct edc_app *app);

/* The command a user would type to upgrade by hand, written into |buf|. Always succeeds with
 * a non-empty string (a placeholder version is better than an empty hint). */
extern void edc_app_update_manual_hint(const struct edc_app *app, char *buf, size_t cap);

/* Shut the process down (idempotent): stop actor/runner -> persist every session
 * as-is -> drop in-flight turns. It DOES write to disk, which is why it is not
 * called "free": destroying an app is more than free(). Exposed separately so
 * "persist first, free later" are two distinct steps. */
extern void edc_app_shutdown(struct edc_app *app);

/* Free memory. Runs `edc_app_shutdown()` first (keeping "destroy = persist"),
 * then frees scheduler / session_manager / config / toolbox. */
extern void edc_app_free(struct edc_app *app);

/* ── Handle accessors: the root <-> `//src/app:api` contract ──
 * app is opaque, so outside code only gets these parts; the frontend projection
 * functions read everything through them (in `edc_api.c`, `app->` appears 0
 * times). */
extern struct edc_config *edc_app_config(const struct edc_app *app);
extern struct edc_actor *edc_app_channel_actor(const struct edc_app *app);

/* Update the LLM provider settings at runtime (from the TUI /connect menu):
 * persist api_url/model/api_key to config.json, update the in-memory config, and
 * refresh llm_actor's copy. A NULL/empty argument leaves that field unchanged.
 * Returns true when the config was persisted. */
extern bool edc_app_set_llm_config(struct edc_app *app, const char *api_url, const char *model,
                                   const char *api_key);

/** Finish the first-run wizard: persist the LLM settings, the chosen model's context
 *  window (when > 0) and the `onboarded` flag, then hot-apply. Returns true on success;
 *  false leaves nothing half-written that the running app disagrees with (the LLM write
 *  is the gate, the rest are best-effort). */
extern bool edc_app_complete_setup(struct edc_app *app, const char *api_url, const char *model, const char *api_key,
                                   int context_window);

/** Mark the first-run wizard as done WITHOUT changing the LLM settings (the "Skip"
 *  path): persist `onboarded` so the setup page does not reopen. */
extern bool edc_app_mark_onboarded(struct edc_app *app);

/** Test an OpenAI-compatible endpoint (the setup page's Test button): send one tiny chat
 *  request and report. Writes a short human message into |out| and returns true when the
 *  endpoint ANSWERED (any HTTP status). BLOCKING for up to |timeout_ms| -- call it off
 *  the UI/runner thread. */
extern bool edc_app_test_endpoint(struct edc_app *app, const char *api_url, const char *api_key, int timeout_ms,
                                  char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif
