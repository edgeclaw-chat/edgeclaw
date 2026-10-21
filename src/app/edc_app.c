#include "edc_app.h"
#include "edc_actor.h"
#include "edc_channel_actor.h"
#include "edc_commands.h"
#include "edc_compat.h"
#include "edc_platform.h"
#include "edc_config.h"
#include "edc_context.h"
#include "edc_endpoint_probe.h"
#include "edc_file_utils.h"
#include "edc_init_env.h"
#include "edc_llm_actor.h"
#include "edc_llm_request.h"
#include "edc_sandbox.h"
#include "edc_secret.h"
#include "edc_state_serializer.h"
#include "edc_string_utils.h"
#include "edc_task.h"
#include "edc_task_group.h"
#include "edc_toolbox_actor.h"
#include "edc_turn.h"
#include "log.h"
#include "sds.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/* ── Composition root: defined ONLY here (opaque) ────────────
 * Outside reads through the accessors; userland receives its own few pieces
 * (see edc_app.h). */
struct edc_app
{
    struct edc_config *config;
    /* The session store is not a field here: edc_scheduler_open creates it and the
     * scheduler owns it. The root neither holds it nor asks on its behalf -- the
     * frontend gets session_id / workdir from the snapshot. */
    struct edc_toolbox *toolbox;
    struct edc_scheduler *scheduler;

    edc_actor_t *channel_actor;
    edc_actor_t *llm_actor;
    edc_actor_t *toolbox_actor;

    edc_actor_runner_t *runner;

    /* shutdown() idempotence gate (see edc_app_shutdown): persisting may happen
     * only once. */
    bool shutdown_done;
};

/* Session store / cross-session resolution / persistence -> edc_scheduler_open/close:
 * the store is the scheduler's private property; the root only builds the object
 * and tells it to open/close. */

/* ── Boundary accessors ────────────────────────────────────── */

struct edc_config *edc_app_config(const struct edc_app *app)
{
    return app ? app->config : NULL;
}

edc_actor_t *edc_app_channel_actor(const struct edc_app *app)
{
    return app ? app->channel_actor : NULL;
}

/* ── Lifetime ──────────────────────────────────────────────── */

struct edc_app *edc_app_new(bool setup, int sandbox_override)
{
    sds edc_home = edc_config_resolve_bc_home();

    if (!edc_init_env(edc_home, setup))
    {
        log_error("Failed to initialize environment");
        sdsfree(edc_home);
        return NULL;
    }

    if (!edc_secret_init(false, edc_home))
    {
        log_error("Failed to initialize secret module");
        sdsfree(edc_home);
        return NULL;
    }

    struct edc_app *app = g_new0(struct edc_app, 1);
    app->config = edc_config_load(edc_home);

    /* edc_config_load() returns NULL on a missing/corrupt config.json or when
     * the stored API key cannot be decrypted (e.g. wrong passphrase / master
     * key). Everything below dereferences app->config, so bail out cleanly
     * instead of crashing. The concrete reason is already logged by the loader. */
    if (!app->config)
    {
        log_error("Failed to load configuration (%s); aborting startup", edc_home);
        free(app);
        sdsfree(edc_home);
        return NULL;
    }

    /* The master key is intentionally NOT wiped here: the /connect command
     * re-encrypts the API key when the user updates the provider at runtime,
     * so the key must stay available (it remains mlock()ed). */

    /* The master switch was loaded from config.json (default on). The CLI flag,
     * when given, overrides it for this run only: sandbox_override < 0 keeps the
     * persisted value, 0 forces off, 1 forces on. It is deliberately not
     * written back -- only the in-TUI /sandbox command persists the state. */
    if (sandbox_override >= 0)
        app->config->enable_sandbox = (sandbox_override != 0);

    /* "Forced on" cannot be honoured where there is no backend. Say it out loud instead of
     * starting with a switch that reads on while nothing is enforced -- the persisted value
     * was already clamped by edc_config_load, this covers the CLI flag only. */
    if (app->config->enable_sandbox && !edc_platform_has_sandbox())
    {
        log_warn("sandbox: forced on, but this platform has no sandbox backend; running with it OFF");
        app->config->enable_sandbox = false;
    }
    if (app->config->enable_sandbox)
    {
        edc_sandbox_init(app->config);
    }

    app->channel_actor = edc_channel_actor_new();
    app->llm_actor = edc_llm_actor_new(app->config);

    /* Create the scheduler -- no active session is handed to it (the scheduler does
     * not hold "the current session"; each entry point carries its session). */
    app->scheduler = edc_scheduler_new(app->config);

    /* Build the session store and restore the active session's saved state: the
     * store is the scheduler's private property, built by its own entry point. */
    if (!edc_scheduler_open(app->scheduler))
    {
        log_error("failed to open the scheduler session store");
        edc_app_free(app);
        return NULL;
    }

    /* Wiring: each provider installs its OWN ports onto the scheduler -- the root
     * only "builds objects + tells them to wire", and never appears on a runtime
     * path. The four outbound ports belong to the channel (message shape is its
     * knowledge), the command entry to the command layer. Port signatures are
     * unchanged, so kernel call sites and test fakes are unaffected. */
    edc_channel_actor_bind_scheduler(app->channel_actor, app->scheduler);
    edc_commands_bind_scheduler(app->scheduler);

    edc_channel_actor_set_observer(app->channel_actor, edc_scheduler_get_actor(app->scheduler));

    app->runner = edc_actor_runner_new();
    edc_file_utils_init(&app->runner->loop);
    edc_actor_runner_add(app->runner, edc_scheduler_get_actor(app->scheduler));
    /* The scheduler actor now has a loop: register the built-in background services and
     * arm their shared timer (before this, uv_now has no loop to read). */
    edc_scheduler_services_start(app->scheduler);
    edc_actor_runner_add(app->runner, app->channel_actor);
    edc_actor_runner_add(app->runner, app->llm_actor);

    app->toolbox = edc_toolbox_new(app->config, edc_scheduler_get_actor(app->scheduler), app->channel_actor);
    app->toolbox_actor = edc_toolbox_actor_new(app->toolbox);
    edc_actor_runner_add(app->runner, app->toolbox_actor);

    /* These two ports' implementations need the toolbox (tool definitions / the
     * request chain), so this wiring comes after it. */
    edc_toolbox_actor_bind_scheduler(app->toolbox_actor, app->scheduler);
    edc_llm_request_bind_scheduler(app->scheduler, app->llm_actor, app->toolbox ? app->toolbox->schemas : NULL,
                                   app->toolbox ? app->toolbox->memory_schemas : NULL);

    edc_actor_runner_start(app->runner);

    sdsfree(edc_home);
    return app;
}

bool edc_app_set_llm_config(struct edc_app *app, const char *api_url, const char *model, const char *api_key)
{
    if (!app || !app->config)
        return false;

    /* Persist first: if the file cannot be written, leave the running app
     * untouched so memory and disk stay in agreement. */
    if (!edc_config_update_llm(app->config->edc_home, api_url, model, api_key))
        return false;

    struct llm_api_conf *c = &app->config->llm_api_conf;
    if (api_url && api_url[0])
    {
        sdsfree(c->api_url);
        c->api_url = sdsnew(api_url);
    }
    if (model && model[0])
    {
        sdsfree(c->model);
        c->model = sdsnew(model);
    }
    if (api_key && api_key[0])
    {
        sdsfree(c->api_key);
        c->api_key = sdsnew(api_key);
    }

    /* The llm_actor keeps its own copy of the provider settings and runs on the
     * actor-runner thread, so hand it the new values through its mailbox. */
    struct edc_llm_api_conf_update *u = calloc(1, sizeof(*u));
    if (!u)
        return true; /* config is already updated; actor refresh is best-effort */
    u->api_url = api_url ? sdsnew(api_url) : NULL;
    u->model = model ? sdsnew(model) : NULL;
    u->api_key = api_key ? sdsnew(api_key) : NULL;
    edc_actor_send(app->llm_actor, edc_actor_msg_new(BC_ACTOR_MSG_LLM_SET_CONFIG, NULL, NULL, u,
                                                     (edc_actor_data_free_fn)edc_llm_api_conf_update_free));
    return true;
}

bool edc_app_complete_setup(struct edc_app *app, const char *api_url, const char *model, const char *api_key,
                            int context_window)
{
    if (!app || !app->config)
        return false;

    /* The LLM settings are the gate: on failure nothing else is touched, so the running
     * app and disk stay in agreement (same rule as edc_app_set_llm_config). */
    if (!edc_app_set_llm_config(app, api_url, model, api_key))
        return false;

    /* The chosen model's window: best-effort (the LLM write already succeeded). */
    if (context_window > 0 && edc_config_update_context_window(app->config->edc_home, context_window))
        app->config->context_window = context_window;

    /* Mark the wizard done so it does not reopen (persist + live field). */
    edc_config_set_onboarded(app->config, true);
    return true;
}

bool edc_app_mark_onboarded(struct edc_app *app)
{
    if (!app || !app->config)
        return false;
    return edc_config_set_onboarded(app->config, true);
}

bool edc_app_test_endpoint(struct edc_app *app, const char *api_url, const char *api_key, int timeout_ms, char *out,
                           size_t cap)
{
    if (!app || !app->config)
        return false;
    sds cacert = app->config->workspace ? edc_path_join(app->config->workspace, "ssl", "cacert.pem", NULL) : NULL;
    /* The config holds a base URL; probe its /chat/completions endpoint (see the actor). */
    sds url = edc_url_chat_completions(api_url);
    bool ok = edc_endpoint_probe(cacert, url ? url : api_url, api_key, timeout_ms, out, cap);
    sdsfree(url);
    sdsfree(cacert);
    return ok;
}

/* ── sandbox path whitelist (ro/rw) management ─────────────────────────── */

/* The sandbox config writers (config only) live in edc_config.{c,h} -- the same
 * concept as config read/write, so they do not belong on app. */
void edc_app_shutdown(struct edc_app *app)
{
    if (!app || app->shutdown_done)
        return;

    /* 1. Stop the actors (and runner) first -- persistence must follow, or it would
     * write concurrently with the actor threads. */
    edc_actor_runner_free(app->runner);
    app->runner = NULL;

    /* These three frees do not accept NULL (they dereference `actor->state`), so
     * each is null-checked: on edc_app_new's failure path toolbox_actor is not
     * built yet. */
    if (app->channel_actor)
    {
        edc_channel_actor_free(app->channel_actor);
        app->channel_actor = NULL;
    }
    if (app->toolbox_actor)
    {
        edc_toolbox_actor_free(app->toolbox_actor);
        app->toolbox_actor = NULL;
    }
    if (app->llm_actor)
    {
        edc_llm_actor_free(app->llm_actor);
        app->llm_actor = NULL;
    }

    /* 2. Shut down: persist every session as-is + drop in-flight turns, all in the
     * scheduler's single entry point. Must run after step 1. */
    if (app->scheduler)
        edc_scheduler_close(app->scheduler);

    app->shutdown_done = true; /* idempotent: persistence runs only once */
}

void edc_app_free(struct edc_app *app)
{
    if (!app)
        return;

    /* Run the shutdown work first (stop actors + persist + drop turns), keeping
     * the old "destroy = persist" behaviour; callers wanting two steps call
     * `edc_app_shutdown()` explicitly. Before freeing parts: persisting needs the
     * scheduler. */
    edc_app_shutdown(app);

    /* Freeing the scheduler also frees the session store it created. */
    edc_scheduler_free(app->scheduler);

    if (app->config)
        edc_config_free(app->config);
    if (app->toolbox)
        edc_toolbox_free(app->toolbox);

    edc_secret_cleanup();
    free(app);
}
