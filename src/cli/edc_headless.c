#include "edc_headless.h"

#include "cJSON.h"
#include "edc_api.h"
#include "edc_app.h"
#include "log.h"
#include "sds.h"
#include "uv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

/* Exit codes are part of the CLI contract (see the header). */
#define EXIT_OK 0
#define EXIT_TASK_FAILED 1
#define EXIT_USAGE 2
#define EXIT_TIMEOUT 3

/* How often the driver pulls a task snapshot while waiting. The response arrives as a
 * message, so this is only the bound on "how late can we notice the task ended" -- 200 ms
 * is imperceptible for a CLI that runs for seconds. */
#define POLL_MS 200

struct headless_state
{
    uv_mutex_t mu;
    uv_cond_t cv;

    edc_headless_mode mode;
    sds prompt;
    sds session;      /* --session <id|name>; NULL = the active session */

    bool done;
    int exit_code;

    sds answer;   /* last RESP content (the final answer) */
    sds last_sys; /* last SYS text, used as the failure reason */
    sds final_state;

    /* A command line (/… or !…) is not a task: it answers through a system message and
     * creates nothing, so a snapshot can never describe it. The reply IS the output. */
    bool one_shot_reply;
    bool input_rejected;
    /* When a snapshot first reported the task terminal. The reply travels on the same
     * channel but on a different pull, so it can arrive a poll later: finishing on the
     * snapshot alone would drop the answer (and, in JSON mode, the `resp` event). */
    uint64_t terminal_seen_ns;

    uint64_t started_ns;
    uint64_t timeout_ns;
    bool json_header_emitted;
};

/* ── output helpers ───────────────────────────────────────────────────────── */

static void out_json(struct headless_state *st, const char *event, const char *key, const char *value)
{
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "event", event);
    if (key && value)
        cJSON_AddStringToObject(obj, key, value);
    char *line = cJSON_PrintUnformatted(obj);
    if (line)
    {
        fputs(line, stdout);
        fputc('\n', stdout);
        fflush(stdout);
        cJSON_free(line);
    }
    cJSON_Delete(obj);
    (void)st;
}

/* ── snapshot inspection ──────────────────────────────────────────────────── */

/* Is |state| one the task cannot leave on its own? (edc_task_state_str's terminal set.) */
static bool state_is_terminal(const char *state)
{
    return state && (strcmp(state, "COMPLETED") == 0 || strcmp(state, "ERROR") == 0 ||
                     strcmp(state, "CANCELLED") == 0 || strcmp(state, "INTERRUPTED") == 0);
}

/* Look for OUR task in a snapshot: the one whose `input` is the line we sent. Matching on
 * the input rather than on a task id is deliberate -- send_user_message does not return an
 * id, and the snapshot is the only place the frontend can learn one from. */
static bool snapshot_find_our_task(const char *json, const char *prompt, char *state_out, size_t state_cap)
{
    cJSON *root = cJSON_Parse(json);
    if (!root)
        return false;

    bool found = false;
    cJSON *groups = cJSON_GetObjectItem(root, "groups");
    cJSON *group = NULL;
    cJSON_ArrayForEach(group, groups)
    {
        cJSON *tasks = cJSON_GetObjectItem(group, "tasks");
        cJSON *task = NULL;
        cJSON_ArrayForEach(task, tasks)
        {
            const char *input = cJSON_GetStringValue(cJSON_GetObjectItem(task, "input"));
            if (!input || strcmp(input, prompt) != 0)
                continue;
            const char *state = cJSON_GetStringValue(cJSON_GetObjectItem(task, "state"));
            snprintf(state_out, state_cap, "%s", state ? state : "");
            found = true;
            break;
        }
        if (found)
            break;
    }
    cJSON_Delete(root);
    return found;
}

static void finish_locked(struct headless_state *st, int code, const char *state)
{
    if (st->done)
        return;
    st->done = true;
    st->exit_code = code;
    if (state)
        st->final_state = sdscatprintf(st->final_state ? st->final_state : sdsempty(), "%s", state);
    uv_cond_broadcast(&st->cv);
}

/* ── the channel adapter (runs on the actor thread) ───────────────────────── */

static void on_message(void *instance, struct edc_channel_message *msg)
{
    struct headless_state *st = (struct headless_state *)instance;
    if (!msg)
        return;

    uv_mutex_lock(&st->mu);
    if (st->done)
    {
        uv_mutex_unlock(&st->mu);
        return;
    }

    const char *content = edc_app_message_content(msg);

    if (edc_app_message_is_task_snapshot(msg))
    {
        char state[32] = {0};
        /* Closed loop: only OUR task matters. A snapshot that does not mention it yet is
         * simply an earlier one (the request was already in flight when the task landed). */
        if (snapshot_find_our_task(content, st->prompt, state, sizeof(state)) && state_is_terminal(state))
        {
            if (strcmp(state, "COMPLETED") == 0)
            {
                /* Success is concluded from the REPLY, not from here (see the fallback in
                 * the poll loop). Only note the moment. */
                if (st->terminal_seen_ns == 0)
                    st->terminal_seen_ns = uv_hrtime();
            }
            else
            {
                finish_locked(st, EXIT_TASK_FAILED, state);
            }
        }
    }
    else if (edc_app_message_is_resp(msg))
    {
        if (st->answer)
            sdsfree(st->answer);
        st->answer = sdsnew(content ? content : "");
        if (st->mode == EDC_HEADLESS_JSON)
            out_json(st, "resp", "content", content ? content : "");
        /* A reply means the turn settled. (A failed task sends no reply, only a system
         * message -- the snapshot path below is how a failure is noticed.) */
        finish_locked(st, EXIT_OK, "COMPLETED");
    }
    else if (edc_app_message_is_sys(msg))
    {
        if (st->last_sys)
            sdsfree(st->last_sys);
        st->last_sys = sdsnew(content ? content : "");
        if (st->mode == EDC_HEADLESS_JSON)
            out_json(st, "sys", "content", content ? content : "");
        if (st->one_shot_reply)
        {
            /* A command answers exactly once; nothing else will arrive, so finishing here
             * is what keeps `-p "/help"` from waiting for a task that never exists. A line
             * that failed to parse exits 2: it is a usage error, not a task failure. */
            finish_locked(st, st->input_rejected ? EXIT_USAGE : EXIT_OK,
                          st->input_rejected ? "REJECTED" : "REPLIED");
        }
    }
    else if (edc_app_message_is_tool(msg))
    {
        if (st->mode == EDC_HEADLESS_JSON)
            out_json(st, "tool", "content", content ? content : "");
    }
    else if (edc_app_message_is_thought(msg))
    {
        if (st->mode == EDC_HEADLESS_JSON)
            out_json(st, "thought", "content", content ? content : "");
    }
    else if (edc_app_message_is_user(msg) && st->mode == EDC_HEADLESS_JSON)
    {
        out_json(st, "user", "content", content ? content : "");
    }

    uv_mutex_unlock(&st->mu);
}

/* ── argument parsing ─────────────────────────────────────────────────────── */

edc_headless_mode edc_headless_parse(int argc, char **argv)
{
    for (int i = 1; i < argc; i++)
    {
        const char *a = argv[i];
        if (!a)
            continue;
        if (strcmp(a, "--mode") == 0 && i + 1 < argc && strcmp(argv[i + 1], "json") == 0)
            return EDC_HEADLESS_JSON;
        if (strncmp(a, "--mode=", 7) == 0 && strcmp(a + 7, "json") == 0)
            return EDC_HEADLESS_JSON;
        if (strcmp(a, "-p") == 0 || strcmp(a, "--print") == 0 || strcmp(a, "--list-sessions") == 0)
            return EDC_HEADLESS_PRINT;
    }
    return EDC_HEADLESS_NONE;
}

/* The prompt: every argument that is not one of our flags, joined with single spaces.
 * A flag that takes a value (`--mode json`, `--timeout 30`) consumes it here too, or the
 * value would be folded into the prompt. */
static sds collect_prompt(int argc, char **argv)
{
    sds prompt = sdsempty();
    for (int i = 1; i < argc; i++)
    {
        const char *a = argv[i];
        if (!a)
            continue;
        if (strcmp(a, "-p") == 0 || strcmp(a, "--print") == 0 || strcmp(a, "--no-sandbox") == 0 ||
            strcmp(a, "--sandbox") == 0)
            continue;
        if (strcmp(a, "--mode") == 0)
        {
            i++; /* its value is not part of the prompt */
            continue;
        }
        if (strncmp(a, "--mode=", 7) == 0 || strncmp(a, "--sandbox=", 10) == 0)
            continue;
        if (strcmp(a, "--timeout") == 0 || strcmp(a, "--session") == 0)
        {
            i++; /* its value is not part of the prompt */
            continue;
        }
        if (strcmp(a, "--list-sessions") == 0)
            continue;
        if (strncmp(a, "--timeout=", 10) == 0 || strncmp(a, "--session=", 10) == 0)
            continue;
        if (sdslen(prompt) > 0)
            prompt = sdscat(prompt, " ");
        prompt = sdscat(prompt, a);
    }
    return prompt;
}

/* --session <id|name>. The store resolves both (edc_session_manager_find), so the user can
 * pass whichever they know. Not validated up front: the root deliberately does not hold the
 * session store, so an unknown name surfaces as "nothing ever ran" -- which is why the
 * timeout path below says how to list them. */
static sds parse_session(int argc, char **argv)
{
    for (int i = 1; i < argc; i++)
    {
        const char *a = argv[i];
        if (!a)
            continue;
        if (strcmp(a, "--session") == 0 && i + 1 < argc)
            return sdsnew(argv[i + 1]);
        if (strncmp(a, "--session=", 10) == 0)
            return sdsnew(a + 10);
    }
    return NULL;
}

static long long parse_timeout(int argc, char **argv)
{
    for (int i = 1; i < argc; i++)
    {
        const char *a = argv[i];
        if (!a)
            continue;
        if (strcmp(a, "--timeout") == 0 && i + 1 < argc)
            return atoll(argv[i + 1]);
        if (strncmp(a, "--timeout=", 10) == 0)
            return atoll(a + 10);
    }
    return 300; /* seconds */
}

/* ── teardown ─────────────────────────────────────────────────────────────── */

static void close_handle_cb(uv_handle_t *handle, void *arg)
{
    (void)arg;
    if (!uv_is_closing(handle))
        uv_close(handle, NULL);
}

/* Same teardown the TUI does: walk every remaining handle, close it, drain the close
 * callbacks, then release the loop. Must run after edc_app_free(). */
static void close_default_loop(uv_loop_t *loop)
{
    uv_walk(loop, close_handle_cb, NULL);
    uv_run(loop, UV_RUN_DEFAULT);
    uv_loop_close(loop);
}

/* ── the driver ───────────────────────────────────────────────────────────── */

int edc_headless_run(int argc, char **argv, int sandbox_override, edc_headless_mode mode)
{
    if (mode == EDC_HEADLESS_NONE)
        return EXIT_USAGE;

#ifdef _WIN32
    /* JSONL consumers must not receive CRLF, and the answer should not be mangled by the
     * console code page. */
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
#endif

    sds prompt = collect_prompt(argc, argv);
    /* `--list-sessions` is the command that already exists; running it through the same path
     * keeps one implementation of "what sessions are there" (and needs no provider). */
    bool list_sessions = false;
    for (int i = 1; i < argc; i++)
    {
        if (argv[i] && strcmp(argv[i], "--list-sessions") == 0)
            list_sessions = true;
    }
    if (list_sessions)
    {
        sdsfree(prompt);
        prompt = sdsnew("/session list");
    }
    if (sdslen(prompt) == 0)
    {
        fprintf(stderr, "edgeclaw: no prompt given (usage: edgeclaw -p \"<prompt>\")\n");
        sdsfree(prompt);
        return EXIT_USAGE;
    }
    const long long timeout_sec = parse_timeout(argc, argv);

    struct edc_app *app = edc_app_new(false, sandbox_override);
    if (!app)
    {
        /* Two things fail here in practice and neither is guessable from a generic
         * message: a config that was never completed interactively, and a master key that
         * cannot be obtained without a terminal. Name both and the way out of each. */
        fprintf(stderr,
                "edgeclaw: failed to start. If this is a fresh install, run `edgeclaw` once "
                "interactively to complete the setup; in a non-interactive environment the "
                "master key must be supplied through EDGECLAW_MASTER_KEY (details in the log).\n");
        sdsfree(prompt);
        return EXIT_USAGE;
    }

    char api_url[512] = {0}, model[256] = {0}, key[512] = {0};
    (void)edc_app_config_api_url(app, api_url, sizeof(api_url));
    (void)edc_app_config_model(app, model, sizeof(model));
    (void)edc_app_config_api_key(app, key, sizeof(key));

    struct headless_state st;
    memset(&st, 0, sizeof(st));
    uv_mutex_init(&st.mu);
    uv_cond_init(&st.cv);
    st.mode = mode;
    st.prompt = prompt;
    st.session = parse_session(argc, argv);
    const char *session_arg = st.session ? st.session : NULL;;
    st.started_ns = uv_hrtime();
    st.timeout_ns = (uint64_t)(timeout_sec > 0 ? timeout_sec : 300) * 1000000000ull;

    edc_app_channel_adapter_init(app, &st, on_message);

    if (mode == EDC_HEADLESS_JSON)
    {
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "event", "start");
        cJSON_AddStringToObject(obj, "prompt", prompt);
        cJSON_AddStringToObject(obj, "model", model);
        char *line = cJSON_PrintUnformatted(obj);
        if (line)
        {
            fputs(line, stdout);
            fputc('\n', stdout);
            fflush(stdout);
            cJSON_free(line);
        }
        cJSON_Delete(obj);
    }

    /* Which kind of line is this? The same predicate the TUI uses to tag the input line, so
     * a command is never mistaken for a task (and vice versa). */
    switch (edc_app_input_mode(app, prompt))
    {
    case EDC_INPUT_COMMAND:
    case EDC_INPUT_LOCALSHELL:
        st.one_shot_reply = true;
        break;
    case EDC_INPUT_REJECTED:
        st.one_shot_reply = true;
        st.input_rejected = true;
        break;
    default:
        break;
    }

    /* A TASK needs a provider; a command does not -- which is why the check lives here and
     * not at startup (`edgeclaw -p /help` must work on a fresh install). Reported now,
     * before anything is sent, so the failure names its cause instead of surfacing later as
     * a network error. The teardown below is shared with every other exit path on purpose:
     * a second copy of it is exactly how a double free gets in. */
    if (!st.one_shot_reply && (api_url[0] == '\0' || model[0] == '\0' || key[0] == '\0'))
    {
        fprintf(stderr, "edgeclaw: no provider configured (run `edgeclaw` once and complete the "
                        "setup, or use /connect)\n");
        st.done = true;
        st.exit_code = EXIT_USAGE;
    }

    if (st.done)
    {
        /* nothing to run */
    }
    else if (st.one_shot_reply)
    {
        /* A command answers without creating anything, so there is no group to isolate. */
        edc_app_send_user_message(app, session_arg, prompt, NULL, NULL);
    }
    else
    {
        /* A TASK needs a provider; a command does not (that is why the check sits here and
         * not at startup: `edgeclaw -p /help` must work on a fresh install). Say what is
         * missing now rather than failing later with a network error that names no cause. */
        /* A task runs in its OWN group. Joining the session's foreground group would queue
         * this run behind whatever that conversation is doing -- and if that turn is itself
         * waiting on us (which is exactly what a CLI call from inside an agent session
         * looks like), neither side can ever finish. */
        edc_app_send_user_message_new_group(app, session_arg, prompt);
    }

    /* The wait: the adapter sets `done` from the actor thread; this side polls a snapshot,
     * which is also how a FAILURE is noticed (a failed task sends no reply, only a system
     * message, and a system message alone cannot be told apart from a notice). */
    for (;;)
    {
        uv_mutex_lock(&st.mu);
        const bool already = st.done;
        uv_mutex_unlock(&st.mu);
        if (already)
            break;

        edc_app_request_task_snapshot(app, session_arg);

        uv_mutex_lock(&st.mu);
        if (!st.done)
            uv_cond_timedwait(&st.cv, &st.mu, POLL_MS * 1000000ull);
        const bool done = st.done;
        const bool timed_out = !done && (uv_hrtime() - st.started_ns) > st.timeout_ns;
        uv_mutex_unlock(&st.mu);

        if (done)
            break;

        /* Fallback: the task completed and its reply has not shown up within a generous
         * couple of seconds (it normally arrives in the next poll). Finish anyway rather
         * than waiting for the overall timeout over a lost message. */
        uv_mutex_lock(&st.mu);
        const uint64_t seen = st.terminal_seen_ns;
        const bool still_open = !st.done;
        uv_mutex_unlock(&st.mu);
        if (still_open && seen != 0 && (uv_hrtime() - seen) > 2000000000ull)
        {
            uv_mutex_lock(&st.mu);
            finish_locked(&st, EXIT_OK, "COMPLETED");
            uv_mutex_unlock(&st.mu);
            break;
        }

        if (timed_out)
        {
            uv_mutex_lock(&st.mu);
            finish_locked(&st, EXIT_TIMEOUT, "TIMEOUT");
            uv_mutex_unlock(&st.mu);
            break;
        }
    }

    uv_mutex_lock(&st.mu);
    const int code = st.exit_code;
    const char *answer = st.answer ? st.answer : "";
    const char *last_sys = st.last_sys ? st.last_sys : "";
    const char *final_state = st.final_state ? st.final_state : "";
    const bool one_shot = st.one_shot_reply;
    uv_mutex_unlock(&st.mu);

    if (mode == EDC_HEADLESS_PRINT)
    {
        /* A command (or `!cmd`) answers through a system message and creates no task, so
         * THERE is where its output lives -- the reply slot stays empty. Without this,
         * `edgeclaw -p /help` printed nothing while exiting 0. */
        const char *text = (code == EXIT_OK && one_shot && !answer[0]) ? last_sys : answer;
        if (code == EXIT_OK && text[0])
        {
            /* The answer, and nothing else: `> answer.md` must not collect progress. */
            fputs(text, stdout);
            if (text[strlen(text) - 1] != '\n')
                fputc('\n', stdout);
            fflush(stdout);
        }
        else if (code != EXIT_OK)
        {
            if (last_sys[0])
                fprintf(stderr, "%s\n", last_sys);
            else if (strcmp(final_state, "TIMEOUT") == 0 && session_arg)
                /* The likeliest cause of "nothing ever happened" when a session was named:
                 * the name matched nothing, so the scheduler dropped the input. */
                fprintf(stderr,
                        "edgeclaw: timed out. If `--session %s` names nothing, the input was dropped; "
                        "run `edgeclaw --list-sessions` to see the ids and names.\n",
                        session_arg);
            else
                fprintf(stderr, "edgeclaw: the task ended in %s\n",
                        final_state[0] ? final_state : "an unknown state");
        }
    }
    else
    {
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "event", "done");
        cJSON_AddStringToObject(obj, "state", final_state);
        cJSON_AddNumberToObject(obj, "exit_code", code);
        if (code != EXIT_OK && last_sys[0])
            cJSON_AddStringToObject(obj, "error", last_sys);
        char *line = cJSON_PrintUnformatted(obj);
        if (line)
        {
            fputs(line, stdout);
            fputc('\n', stdout);
            fflush(stdout);
            cJSON_free(line);
        }
        cJSON_Delete(obj);
    }

    if (st.answer)
        sdsfree(st.answer);
    if (st.last_sys)
        sdsfree(st.last_sys);
    if (st.final_state)
        sdsfree(st.final_state);
    sdsfree(st.prompt);
    if (st.session)
        sdsfree(st.session);
    uv_mutex_destroy(&st.mu);
    uv_cond_destroy(&st.cv);

    edc_app_free(app); /* shuts down + persists; the runner thread stops here */
    close_default_loop(uv_default_loop());
    return code;
}
