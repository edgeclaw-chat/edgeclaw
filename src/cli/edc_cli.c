#include "edc_cli.h"

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

/* Exit codes are the contract (see edc_cli.h). */
#define EXIT_OK 0
#define EXIT_TASK_FAILED 1
#define EXIT_USAGE 2
#define EXIT_TIMEOUT 3

/* Snapshot poll. The run's tasks report through messages; this only bounds how late the
 * end of the line is noticed. */
#define POLL_MS 200

typedef enum
{
    OUT_ANSWER = 0, /* default: the final answer, alone on stdout */
    OUT_JSON,       /* every message, one object per line */
    OUT_SILENT,     /* nothing (the run is still recorded) */
} out_mode;

struct cli_state
{
    uv_mutex_t mu;
    uv_cond_t cv;

    out_mode out;
    sds line;    /* the input line as typed */
    sds session; /* --session, or NULL for the active one */

    /* Tasks that existed BEFORE the line was sent. Anything not in here was created by
     * this run -- that is how "the whole line is finished" is decided without the kernel
     * having to tag tasks for us. */
    sds *before_ids;
    size_t before_count;

    bool saw_task;   /* at least one task this run created has been seen */
    bool saw_reply;  /* a RESP arrived (the answer) */
    bool saw_output; /* any message arrived at all */

    sds snapshot; /* the newest task tree pulled by the driver, parsed on the driver side */
    sds answer;   /* last RESP content */
    sds last_sys;  /* last system text (a command's answer, or a failure notice) */
    sds last_tool; /* last tool output -- `!cmd`'s answer lives here and nowhere else */

    bool done;
    int exit_code;
    sds end_state;

    uint64_t started_ns;
    uint64_t timeout_ns;
};

/* ── output ───────────────────────────────────────────────────────────────── */

static void emit_json_event(const char *event, const char *value)
{
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "event", event);
    if (value)
        cJSON_AddStringToObject(obj, "content", value);
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

/* ── the snapshot: which tasks belong to this run ─────────────────────────── */

static bool already_known(const struct cli_state *st, const char *id)
{
    for (size_t i = 0; i < st->before_count; i++)
    {
        if (st->before_ids[i] && strcmp(st->before_ids[i], id) == 0)
            return true;
    }
    return false;
}

static bool state_is_terminal(const char *state)
{
    return state && (strcmp(state, "COMPLETED") == 0 || strcmp(state, "ERROR") == 0 ||
                     strcmp(state, "CANCELLED") == 0 || strcmp(state, "INTERRUPTED") == 0);
}

/* Walk the tree: how many of OUR tasks are still running, and what the worst ending was.
 * Called with the mutex held. |running| / |terminal_failed| / |count| are outputs. */
static void scan_snapshot_locked(struct cli_state *st, const char *json, int *running, int *failed, int *count)
{
    *running = 0;
    *failed = 0;
    *count = 0;

    cJSON *root = cJSON_Parse(json);
    if (!root)
        return;

    cJSON *groups = cJSON_GetObjectItem(root, "groups");
    cJSON *group = NULL;
    cJSON_ArrayForEach(group, groups)
    {
        cJSON *tasks = cJSON_GetObjectItem(group, "tasks");
        cJSON *task = NULL;
        cJSON_ArrayForEach(task, tasks)
        {
            const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(task, "id"));
            if (!id || already_known(st, id))
                continue;
            (*count)++;
            const char *state = cJSON_GetStringValue(cJSON_GetObjectItem(task, "state"));
            if (!state_is_terminal(state))
                (*running)++;
            else if (strcmp(state, "COMPLETED") != 0)
            {
                /* A task that ended badly. Counted, not just remembered: the exit code is
                 * built from this count, and a counter nobody increments turns every
                 * failure into a success (which is exactly what the first version did). */
                (*failed)++;
                if (st->end_state == NULL)
                    st->end_state = sdsnew(state);
            }
        }
    }
    cJSON_Delete(root);
}

/* Remember every task that exists now, so the next snapshot can subtract them. */
static void capture_before_locked(struct cli_state *st, const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (!root)
        return;
    cJSON *groups = cJSON_GetObjectItem(root, "groups");
    cJSON *group = NULL;
    size_t n = 0;
    cJSON_ArrayForEach(group, groups)
    {
        cJSON *tasks = cJSON_GetObjectItem(group, "tasks");
        if (tasks)
            n += (size_t)cJSON_GetArraySize(tasks);
    }
    st->before_ids = calloc(n ? n : 1, sizeof(sds));
    st->before_count = 0;
    cJSON_ArrayForEach(group, groups)
    {
        cJSON *tasks = cJSON_GetObjectItem(group, "tasks");
        cJSON *task = NULL;
        cJSON_ArrayForEach(task, tasks)
        {
            const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(task, "id"));
            if (id && st->before_count < n)
                st->before_ids[st->before_count++] = sdsnew(id);
        }
    }
    cJSON_Delete(root);
}

/* ── the channel adapter (actor thread) ───────────────────────────────────── */

static void finish_locked(struct cli_state *st, int code, const char *state)
{
    if (st->done)
        return;
    st->done = true;
    st->exit_code = code;
    if (state)
        st->end_state = sdscat(st->end_state ? st->end_state : sdsempty(), state);
    uv_cond_broadcast(&st->cv);
}

static void on_message(void *instance, struct edc_channel_message *msg)
{
    struct cli_state *st = (struct cli_state *)instance;
    if (!msg)
        return;

    uv_mutex_lock(&st->mu);
    st->saw_output = true;
    const char *content = edc_app_message_content(msg);

    if (edc_app_message_is_resp(msg))
    {
        if (st->answer)
            sdsfree(st->answer);
        st->answer = sdsnew(content ? content : "");
        st->saw_reply = true;
        if (st->out == OUT_JSON)
            emit_json_event("resp", content);
    }
    else if (edc_app_message_is_sys(msg))
    {
        if (st->last_sys)
            sdsfree(st->last_sys);
        st->last_sys = sdsnew(content ? content : "");
        if (st->out == OUT_JSON)
            emit_json_event("sys", content);
    }
    else if (edc_app_message_is_tool(msg))
    {
        if (st->last_tool)
            sdsfree(st->last_tool);
        st->last_tool = sdsnew(content ? content : "");
        if (st->out == OUT_JSON)
            emit_json_event("tool", content);
    }
    else if (edc_app_message_is_thought(msg))
    {
        if (st->out == OUT_JSON)
            emit_json_event("thought", content);
    }
    else if (edc_app_message_is_user(msg))
    {
        if (st->out == OUT_JSON)
            emit_json_event("user", content);
    }
    else if (edc_app_message_is_task_snapshot(msg))
    {
        /* Parking the JSON here (instead of parsing on the actor thread) keeps all the
         * "is my line finished" logic on one thread. */
        if (st->snapshot)
            sdsfree(st->snapshot);
        st->snapshot = sdsnew(content ? content : "");
        uv_cond_broadcast(&st->cv);
    }

    uv_mutex_unlock(&st->mu);
}

/* ── argument parsing ─────────────────────────────────────────────────────── */

struct cli_args
{
    sds line;    /* the input line (may be NULL when none was given) */
    sds session; /* --session */
    out_mode out;
    int64_t timeout_sec;
    int sandbox_override;
};

static void parse_args(int argc, char **argv, struct cli_args *out)
{
    memset(out, 0, sizeof(*out));
    out->out = OUT_ANSWER;
    out->timeout_sec = 300;
    out->sandbox_override = -1;

    sds line = sdsempty();
    bool no_more_flags = false;

    for (int i = 1; i < argc; i++)
    {
        const char *a = argv[i];
        if (!a)
            continue;

        if (!no_more_flags && strcmp(a, "--") == 0)
        {
            no_more_flags = true;
            continue;
        }

        /* Only OUTPUT-related flags exist. There is deliberately no flag for input
         * behaviour: the line itself says everything (`+new group`, `/command`, `!cmd`,
         * `"a" | "b"`), which is exactly what "same as the TUI" has to mean. */
        if (!no_more_flags)
        {
            if (strcmp(a, "--json") == 0)
            {
                out->out = OUT_JSON;
                continue;
            }
            if (strcmp(a, "--silent") == 0)
            {
                out->out = OUT_SILENT;
                continue;
            }
            if (strcmp(a, "--no-sandbox") == 0)
            {
                out->sandbox_override = 0;
                continue;
            }
            if (strcmp(a, "--sandbox") == 0)
            {
                out->sandbox_override = 1;
                continue;
            }
            if (strncmp(a, "--sandbox=", 10) == 0)
            {
                const char *v = a + 10;
                out->sandbox_override = (strcmp(v, "off") == 0 || strcmp(v, "0") == 0 || strcmp(v, "false") == 0 ||
                                         strcmp(v, "no") == 0)
                                            ? 0
                                            : 1;
                continue;
            }
            if (strcmp(a, "--session") == 0 && i + 1 < argc)
            {
                out->session = sdsnew(argv[++i]);
                continue;
            }
            if (strncmp(a, "--session=", 10) == 0)
            {
                out->session = sdsnew(a + 10);
                continue;
            }
            if (strcmp(a, "--timeout") == 0 && i + 1 < argc)
            {
                out->timeout_sec = atoll(argv[++i]);
                continue;
            }
            if (strncmp(a, "--timeout=", 10) == 0)
            {
                out->timeout_sec = atoll(a + 10);
                continue;
            }
        }

        /* Anything else is part of the input line. Joined with ONE space, so an unquoted
         * `edgeclaw_cli /task new "abc"` (three argv words) is the same line as the quoted
         * form -- quoting inside a line is the line's own business, not the shell's. */
        if (sdslen(line) > 0)
            line = sdscat(line, " ");
        line = sdscat(line, a);
    }

    out->line = sdslen(line) > 0 ? line : (sdsfree(line), NULL);
}

/* ── teardown ─────────────────────────────────────────────────────────────── */

static void close_handle_cb(uv_handle_t *handle, void *arg)
{
    (void)arg;
    if (!uv_is_closing(handle))
        uv_close(handle, NULL);
}

/* The same teardown the TUI does: close every remaining handle, drain, release the loop.
 * Must run after edc_app_free(). */
static void close_default_loop(uv_loop_t *loop)
{
    uv_walk(loop, close_handle_cb, NULL);
    uv_run(loop, UV_RUN_DEFAULT);
    uv_loop_close(loop);
}

static void state_free(struct cli_state *st)
{
    for (size_t i = 0; i < st->before_count; i++)
    {
        if (st->before_ids[i])
            sdsfree(st->before_ids[i]);
    }
    free(st->before_ids);
    if (st->line)
        sdsfree(st->line);
    if (st->session)
        sdsfree(st->session);
    if (st->snapshot)
        sdsfree(st->snapshot);
    if (st->answer)
        sdsfree(st->answer);
    if (st->last_sys)
        sdsfree(st->last_sys);
    if (st->last_tool)
        sdsfree(st->last_tool);
    if (st->end_state)
        sdsfree(st->end_state);
}

/* ── driver ───────────────────────────────────────────────────────────────── */

int edc_cli_main(int argc, char **argv)
{
#ifdef _WIN32
    /* JSONL consumers must not receive CRLF, and an answer must not be mangled by the
     * console code page. */
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
#endif

    struct cli_args args;
    parse_args(argc, argv, &args);

    if (!args.line)
    {
        fprintf(stderr,
                "edgeclaw_cli: give the input line, exactly as you would type it:\n"
                "  edgeclaw_cli 'explain @src/main.c'\n"
                "  edgeclaw_cli '/task new \"write the docs\"'\n"
                "  edgeclaw_cli '+\"explore the parser\"'\n"
                "  edgeclaw_cli '\"a\" | \"b\"'\n"
                "Output: (default) the final answer, --json every message, --silent nothing.\n");
        return EXIT_USAGE;
    }

    struct edc_app *app = edc_app_new(false, args.sandbox_override);
    if (!app)
    {
        fprintf(stderr,
                "edgeclaw_cli: failed to start. A fresh install has to be configured once "
                "interactively (run `edgeclaw`); without a terminal the master key must come "
                "from EDGECLAW_MASTER_KEY.\n");
        sdsfree(args.line);
        if (args.session)
            sdsfree(args.session);
        return EXIT_USAGE;
    }

    struct cli_state st;
    memset(&st, 0, sizeof(st));
    uv_mutex_init(&st.mu);
    uv_cond_init(&st.cv);
    st.out = args.out;
    st.line = args.line;
    st.session = args.session;
    st.started_ns = uv_hrtime();
    st.timeout_ns = (uint64_t)(args.timeout_sec > 0 ? args.timeout_sec : 300) * 1000000000ull;

    edc_app_channel_adapter_init(app, &st, on_message);
    const char *session_arg = st.session ? st.session : NULL;

    /* Baseline: learn what already exists, so those ids are never waited on. A TUI using
     * the same session keeps working, and its tasks must not hold this run open. */
    {
        uv_mutex_lock(&st.mu);
        edc_app_request_task_snapshot(app, session_arg);
        for (int i = 0; i < 10 && !st.snapshot; i++)
            uv_cond_timedwait(&st.cv, &st.mu, POLL_MS * 1000000ull);
        if (st.snapshot)
        {
            capture_before_locked(&st, st.snapshot);
            sdsfree(st.snapshot);
            st.snapshot = NULL;
        }
        uv_mutex_unlock(&st.mu);
    }

    if (st.out == OUT_JSON)
        emit_json_event("input", st.line);

    /* THE input path: the same call the TUI makes on Enter. Passes, classification
     * (command / local shell / shell line / plain message), groups, dependencies and
     * sessions all happen inside, unchanged. */
    edc_app_send_user_message(app, session_arg, st.line, NULL, NULL);

    int quiet_polls = 0;  /* consecutive polls with no message and no task of ours */
    bool answered = false;

    for (;;)
    {
        edc_app_request_task_snapshot(app, session_arg);

        uv_mutex_lock(&st.mu);
        if (!st.done)
            uv_cond_timedwait(&st.cv, &st.mu, POLL_MS * 1000000ull);
        const bool done = st.done;
        const bool timed_out = !done && (uv_hrtime() - st.started_ns) > st.timeout_ns;
        sds snap = st.snapshot; /* take ownership */
        st.snapshot = NULL;
        if (st.saw_output)
            answered = true;
        uv_mutex_unlock(&st.mu);

        if (done)
        {
            if (snap)
                sdsfree(snap);
            break;
        }

        if (timed_out)
        {
            if (snap)
                sdsfree(snap);
            uv_mutex_lock(&st.mu);
            finish_locked(&st, EXIT_TIMEOUT, "TIMEOUT");
            uv_mutex_unlock(&st.mu);
            break;
        }

        if (!snap)
            continue;

        uv_mutex_lock(&st.mu);
        int running = 0, failed = 0, ours = 0;
        scan_snapshot_locked(&st, snap, &running, &failed, &ours);
        uv_mutex_unlock(&st.mu);
        sdsfree(snap);

        /* Half one: the line created tasks -> it is finished when all of them are. */
        if (ours > 0 && running == 0)
        {
            uv_mutex_lock(&st.mu);
            finish_locked(&st, failed > 0 ? EXIT_TASK_FAILED : EXIT_OK,
                          failed > 0 ? (st.end_state ? st.end_state : "ERROR") : "COMPLETED");
            uv_mutex_unlock(&st.mu);
            break;
        }

        /* Half two: nothing was created. Either it was a command (it answers and that is
         * the end), or the input produced nothing at all. Two quiet polls separate
         * "answered" from "has not answered yet", and also give `/task new …` -- a command
         * that DOES create a task -- time to show up before concluding. */
        if (ours == 0)
        {
            quiet_polls++;
            if (answered && quiet_polls >= 2)
            {
                uv_mutex_lock(&st.mu);
                finish_locked(&st, EXIT_OK, "REPLIED");
                uv_mutex_unlock(&st.mu);
                break;
            }
        }
        else
        {
            quiet_polls = 0;
        }
    }

    /* ── output ────────────────────────────────────────────────────────────────
     * The messages already exist; this only decides what to do with them. */
    uv_mutex_lock(&st.mu);
    const int code = st.done ? st.exit_code : EXIT_TIMEOUT;
    const char *answer = st.answer ? st.answer : "";
    const char *last_sys = st.last_sys ? st.last_sys : "";
    const char *end_state = st.end_state ? st.end_state : "";
    sds answer_copy = sdsnew(answer);
    sds sys_copy = sdsnew(last_sys);
    sds tool_copy = sdsnew(st.last_tool ? st.last_tool : "");
    uv_mutex_unlock(&st.mu);

    if (st.out == OUT_JSON)
    {
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "event", "done");
        cJSON_AddStringToObject(obj, "state", end_state);
        cJSON_AddNumberToObject(obj, "exit_code", code);
        if (code != EXIT_OK && sys_copy[0])
            cJSON_AddStringToObject(obj, "error", sys_copy);
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
    else if (st.out == OUT_ANSWER)
    {
        /* Where the answer lies depends on what the line was, and that is the point of
         * taking the messages first: a message answers through a reply, a command through a
         * system message, and `!cmd` through its tool output -- it has no reply at all. In
         * that order, because a reply (when there is one) is the considered answer. */
        const char *text = answer_copy[0] ? answer_copy : (sys_copy[0] ? sys_copy : tool_copy);
        if (code == EXIT_OK && text[0])
        {
            /* The answer alone: `> answer.md` must not collect progress or notices. */
            fputs(text, stdout);
            if (text[strlen(text) - 1] != '\n')
                fputc('\n', stdout);
            fflush(stdout);
        }
        else if (code != EXIT_OK)
        {
            if (sys_copy[0])
                fprintf(stderr, "%s\n", sys_copy);
            else
                fprintf(stderr, "edgeclaw_cli: the input ended in %s\n",
                        end_state[0] ? end_state : "an unknown state");
        }
    }

    sdsfree(answer_copy);
    sdsfree(sys_copy);
    sdsfree(tool_copy);

    state_free(&st);
    uv_mutex_destroy(&st.mu);
    uv_cond_destroy(&st.cv);
    if (args.session)
        sdsfree(args.session);
    edc_app_free(app);
    close_default_loop(uv_default_loop());
    return code;
}
