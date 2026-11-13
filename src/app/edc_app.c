#include "edc_app.h"
#include "edc_scheduler.h"
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
#include "edc_session_manager.h"
#include "edc_state_store.h"
#include "edc_string_utils.h"
#include "edc_task.h"
#include "edc_task_group.h"
#include "edc_toolbox_actor.h"
#include "edc_toolbox.h"
#include "edc_tools_update.h"
#include "edc_toolset.h"
#include "edc_turn.h"
#include "edc_semver.h"
#include "edc_update.h"
#include "edc_update_service.h"
#include "edc_upgrade.h"
#include "edc_upgrade_run.h"
#include "edc_version.h"
#include "log.h"
#include "sds.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/* ── The tool registry, plugged into the two consumers that sit below it ────
 *
 * The kernel renders the system prompt and the actor assembles the request, but neither may
 * include the toolbox (//src/toolbox depends on //src/kernel, so the reverse is a cycle).
 * Both take a function pointer instead, installed here -- the one place that owns everything. */

/* The system prompt's tool section: what the model is told it has, in prose. */
static sds app_tool_section(void *ctx)
{
    return edc_toolset_render_prompt((struct edc_toolbox *)ctx, EDC_TOOL_SCOPE_NORMAL, NULL);
}

/* A group's declaration set, resolved once and then frozen: one conversation must send the
 * same `tools` bytes every turn. Returning NULL would make the request fall back to the
 * process-wide array, so this always answers when there is a toolbox. */
static cJSON *app_group_tools(void *ctx, struct edc_task *task)
{
    struct edc_toolbox *tb = (struct edc_toolbox *)ctx;
    if (!tb)
        return NULL;
    struct edc_task_group *group = task ? task->group : NULL;
    if (!group)
        return tb->schemas; /* no group to freeze against: the process-wide set */

    if (!group->tools_decl)
    {
        /* Once per group (then cached in tools_decl), so a full history load here is fine:
         * the per-request path never recomputes the tool set. */
        cJSON *hist = edc_task_group_history_msgs(group, 0, -1);
        group->tools_decl = edc_toolset_resolve(tb, hist, EDC_TOOL_SCOPE_NORMAL);
        if (hist)
            cJSON_Delete(hist);
        group->toolset_epoch++;
        log_info("toolset: group %s declares %d tools (epoch %u)", edc_id_str(group->group_id),
                 group->tools_decl ? cJSON_GetArraySize(group->tools_decl) : 0, group->toolset_epoch);
    }
    return group->tools_decl;
}

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

    /* The once-per-launch release check. NULL when it was not started (disabled in
     * config.json, or the thread/handles could not be created) -- and a NULL here is not an
     * error anywhere, it just means no check happened this run. */
    struct edc_update_service *update_service;

    /* An upgrade in flight, if any. Only one at a time: a second run would have two
     * installers racing over the same binary. */
    struct edc_upgrade_run *upgrade;

    /* Who to tell when a newer release is found. Set from the frontend after construction,
     * because the app is built before the UI exists. */
    edc_update_found_observer update_observer;
    void *update_observer_ud;

    /* The outcome callback for the running upgrade, so a cancel can still report. */
    edc_upgrade_done_observer upgrade_done;
    void *upgrade_done_ud;

    /* shutdown() idempotence gate (see edc_app_shutdown): persisting may happen
     * only once. */
    bool shutdown_done;
};

/* The three values the launch-time check needs, each with an environment override. The
 * reading lives in edc_update.c so the `--update-check` diagnostic honours the same variables
 * -- a diagnostic that cannot be pointed somewhere else cannot diagnose an unreachable
 * endpoint, because the failure looks exactly like "no update".
 *
 * The version falls back to the build's own, which is the only sensible default here. */
static sds app_update_url(void)
{
    return edc_update_releases_url();
}

static const char *app_update_current_version(void)
{
    return edc_update_current_version(edc_version());
}

static uint64_t app_update_delay_ms(void)
{
    return edc_update_check_delay_ms();
}

/* A newer release exists. Runs on the loop thread, once.
 *
 * For now this only logs: the banner and the /update confirmation are the next step, and
 * wiring a user-visible prompt is a UI change that deserves its own commit rather than being
 * smuggled in behind a network call. Logging it means the whole path -- thread, delay, fetch,
 * selection -- is exercised end to end from a normal launch. */
static void app_update_found(const char *tag, const char *asset, void *ud)
{
    (void)asset;
    struct edc_app *app = ud;
    log_info("update: %s is available", tag ? tag : "(unknown)");
    if (app && app->update_observer)
        app->update_observer(tag, app->update_observer_ud);
}

void edc_app_set_update_observer(struct edc_app *app, edc_update_found_observer fn, void *ud)
{
    if (!app)
        return;
    app->update_observer = fn;
    app->update_observer_ud = ud;
}

bool edc_app_update_can_self_upgrade(const struct edc_app *app)
{
    if (!app || !edc_upgrade_supported())
        return false;
    sds exe = edc_platform_exe_path();
    sds home = edc_homedir();
    const bool ok = exe && home && edc_upgrade_detect(exe, home) == EDC_UPGRADE_METHOD_INSTALLER;
    sdsfree(exe);
    sdsfree(home);
    return ok;
}

/* The upgrade finished. Runs on the loop thread, from edc_upgrade_run. */
static void app_upgrade_done(bool ok, const char *detail, void *ud)
{
    struct edc_app *app = ud;
    if (!app)
        return;
    /* Clear the slot BEFORE reporting: the observer may immediately start another upgrade,
     * and the "one at a time" guard must already be free by then. */
    edc_upgrade_done_observer cb = app->upgrade_done;
    void *cb_ud = app->upgrade_done_ud;
    app->upgrade_done = NULL;
    app->upgrade_done_ud = NULL;
    app->upgrade = NULL;
    if (cb)
        cb(ok, detail, cb_ud);
}

bool edc_app_update_start(struct edc_app *app, const char *tag, edc_upgrade_done_observer on_done, void *ud)
{
    if (!app || !app->runner || !tag || !tag[0] || !on_done)
        return false;
    if (app->upgrade)
        return false; /* one at a time */
    if (!edc_app_update_can_self_upgrade(app))
        return false;

    sds exe = edc_platform_exe_path();
    sds home = edc_homedir();
    sds dir = exe ? edc_path_dirname(exe) : NULL;
    sds cacert =
        app->config && app->config->workspace ? edc_path_join(app->config->workspace, "ssl", "cacert.pem", NULL) : NULL;
    /* Pinned to the tag, never a moving branch: the script and the archive it verifies then
     * come from the same release. */
    sds script_url = sdscatprintf(sdsempty(),
                                  "https://raw.githubusercontent.com/edgeclaw-chat/edgeclaw/%s/tools/%s", tag,
                                  edc_upgrade_installer_filename());
    struct edc_upgrade_run *run =
        exe ? edc_upgrade_run_start(&app->runner->loop, cacert, script_url, tag, dir, /* timeout */ 120000,
                                    app_upgrade_done, app)
            : NULL;

    sdsfree(script_url);
    sdsfree(exe);
    sdsfree(home);
    sdsfree(dir);
    sdsfree(cacert);
    if (!run)
        return false;

    app->upgrade = run;
    app->upgrade_done = on_done;
    app->upgrade_done_ud = ud;
    return true;
}

/* Adapter handed to the internal `update_install` tool: same contract as
 * edc_app_update_start except that the app and the completion context arrive separately.
 * |start_ud| is the app (registered once); |done_ud| is the tool's per-call state and is
 * forwarded untouched so the tool gets its own pointer back. */
static bool app_update_install_from_tool(void *start_ud, const char *tag, edc_upgrade_done_observer on_done,
                                         void *done_ud)
{
    return edc_app_update_start((struct edc_app *)start_ud, tag, on_done, done_ud);
}

bool edc_app_update_spawn_task(struct edc_app *app, const char *tag, struct edc_task **out_task)
{
    if (out_task)
        *out_task = NULL;
    if (!app || !app->scheduler || !tag || !tag[0])
        return false;
    /* Asked before the task exists: a task that can only fail is worse than a refusal, and
     * the caller has a dialog to keep open. */
    if (!edc_app_update_can_self_upgrade(app))
        return false;

    struct edc_session_manager *sessions = app->scheduler->sessions;
    struct edc_session *session = sessions ? edc_session_manager_get_active(sessions) : NULL;
    if (!session)
        return false;

    cJSON *args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "tag", tag);

    sds desc = sdscatprintf(sdsempty(), "update to %s", tag);
    sds input = sdscatprintf(sdsempty(), "update to %s", tag);

    struct edc_task_spec spec;
    memset(&spec, 0, sizeof(spec));
    spec.desc = desc;
    /* The turn's user message: what the transcript shows this task was for. */
    spec.input = input;
    spec.place = EDC_TASK_PLACE_FOREGROUND;

    /* BACKGROUND, and it must stay that way. The download-and-install takes up to two
     * minutes; taking the screen for that would push whatever the user was reading out of
     * view for something they already agreed to. `background` additionally keeps it from
     * queueing behind the group's current holder -- an update runs alongside. */
    spec.background = true;
    spec.take_foreground = false;

    /* The whole mechanism, exactly as `!cmd` uses it: one seeded call, no LLM round. The
     * tool is registered INTERNAL, so this is the only way it is ever invoked. */
    spec.initial_tool_name = "update_install";
    spec.initial_tool_args = args;

    struct edc_task *task = edc_scheduler_task_create(app->scheduler, session, &spec);
    cJSON_Delete(args);
    sdsfree(desc);
    sdsfree(input);
    if (out_task)
        *out_task = task;
    return task != NULL;
}

void edc_app_update_cancel(struct edc_app *app)
{
    if (!app || !app->upgrade)
        return;
    /* The run struct frees itself once its handles have closed; dropping the pointer is all
     * this side has to do. The done callback is cleared first so a late exit does not report
     * into a dialog that has already closed. */
    app->upgrade_done = NULL;
    app->upgrade_done_ud = NULL;
    edc_upgrade_run_stop(app->upgrade);
    app->upgrade = NULL;
}

void edc_app_update_manual_hint(const struct edc_app *app, char *buf, size_t cap)
{
    (void)app;
    if (!buf || cap == 0)
        return;
    /* The version the hint names: whatever the check offered is not stored here, so the
       placeholder form is used -- it still tells the user where to look. */
    sds hint = edc_upgrade_manual_hint(NULL);
    snprintf(buf, cap, "%s", hint ? hint : "see https://github.com/edgeclaw-chat/edgeclaw/releases");
    sdsfree(hint);
}

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

    /* The master key is deliberately NOT wiped here, and stays resident until shutdown:
     * /connect re-encrypts the API key when the user updates the provider at runtime, and
     * the write path stores whatever encryption returns -- with the key gone it would
     * overwrite the saved key with an empty string and still report success. This is the
     * documented contract in edc_secret.h; read that before adding a wipe. */

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

    /* `update_install`, registered INTERNAL: reachable by a seeded call, invisible to the
     * model. The upgrade entry point is injected because the toolbox sits below this file and
     * cannot name struct edc_app; |app| is what the callback carries.
     *
     * This is the only tool reached by edc_task_spec.initial_tool_name whose work outlives a
     * single actor turn (fetch the installer, spawn it, wait up to 120s), which is exactly why
     * it is a tool: the ledger already knows how to hold a pending call across that. */
    if (app->toolbox)
        edc_tools_update_register(app->toolbox, app_update_install_from_tool, app);

    app->toolbox_actor = edc_toolbox_actor_new(app->toolbox);
    edc_actor_runner_add(app->runner, app->toolbox_actor);

    /* These two ports' implementations need the toolbox (tool definitions / the
     * request chain), so this wiring comes after it. */
    edc_toolbox_actor_bind_scheduler(app->toolbox_actor, app->scheduler);
    edc_llm_request_bind_scheduler(app->scheduler, app->llm_actor, app->toolbox ? app->toolbox->schemas : NULL,
                                   app->toolbox ? app->toolbox->memory_schemas : NULL);

    /* The registry, in the two places that need it but sit below the toolbox in the
     * layering: the system prompt's tool section and the per-group declaration set. Both are
     * rendered from the same registry, so they cannot disagree with the request's `tools`
     * field. */
    edc_context_set_tool_section_provider(app_tool_section, app->toolbox);
    edc_llm_request_bind_group_tools(app_group_tools, app->toolbox);

    edc_actor_runner_start(app->runner);

    /* The release check, after everything above is running: one check per launch, off the UI
     * thread and off the actor loop, reported once if a newer release exists.
     *
     * A build whose version is not a release (a dev build reports the commit hash) has
     * nothing to compare against, so the feature is off for it -- and that is said at INFO
     * rather than debug. Silence here is not "nothing to report", it is "this build cannot
     * check", and a user wondering why they are never told about updates deserves the
     * difference. Every other reason for no result stays silent.
     *
     * The 10s delay is not politeness: at this instant the first model request is in flight,
     * and a check competing with it would delay the first thing the user sees. */
    if (app->config->update_check)
    {
        const char *current = app_update_current_version();
        if (!edc_semver_is_valid(current))
        {
            log_info("update check: off for this build (version '%s' is not a release)", current);
        }
        else
        {
            sds cacert =
                app->config->workspace ? edc_path_join(app->config->workspace, "ssl", "cacert.pem", NULL) : NULL;
            sds url = app_update_url();
            app->update_service = edc_update_service_start(&app->runner->loop, cacert, url, current,
                                                           app->config->update_allow_pre, app_update_delay_ms(),
                                                           app_update_found, app);
            sdsfree(url);
            sdsfree(cacert);
        }
    }
    else
    {
        log_info("update check: disabled in config.json (update_check=false)");
    }

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

    /* Stop the release check before anything it holds: its worker is blocked on a network
     * round trip and its handles live on the runner's loop, which the shutdown below tears
     * down. Joining here also guarantees on_found cannot fire after the app is gone. */
    edc_update_service_stop(app->update_service);
    app->update_service = NULL;

    /* Same for an upgrade in flight: its child process and pipes are on that loop too. */
    edc_app_update_cancel(app);

    /* Run the shutdown work first (stop actors + persist + drop turns), keeping
     * the old "destroy = persist" behaviour; callers wanting two steps call
     * `edc_app_shutdown()` explicitly. Before freeing parts: persisting needs the
     * scheduler. */
    edc_app_shutdown(app);

    /* Freeing the scheduler also frees the in-memory session manager it created. */
    edc_scheduler_free(app->scheduler);

    if (app->config)
        edc_config_free(app->config);
    if (app->toolbox)
        edc_toolbox_free(app->toolbox);

    /* The SQLite connection is a process-wide singleton owned by the state store, so nothing
     * above closes it. Leaving it open at exit loses more than the memory: the four fds
     * (db, -wal, -shm, lock) stay open, the WAL is never checkpointed back into the one file
     * that `edgeclaw db` inspects, and `PRAGMA optimize` never runs. Close it last, once every
     * consumer is gone; the call is idempotent and a no-op if the store was never opened. */
    edc_state_store_close();

    edc_secret_cleanup();
    free(app);
}
