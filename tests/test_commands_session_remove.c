/* test_commands_session_remove: `/session remove` and the two input errors a user hits.
 *
 * These were the last clusters of real, user-visible behaviour left unexecuted in
 * edc_commands.c after the reporting commands got their own file. What is left after this
 * is ~400 lines spread over ~200 one-to-five-line usage/error branches, which is a
 * different kind of work (see the handover doc).
 *
 * Why remove in particular: it is the one session command with side effects a user
 * cannot undo -- it cancels every non-terminal task in the target, drops the tab, and can
 * change which session is being displayed. Its three outcomes have to be right:
 *   - the target does not exist        -> "Session not found", nothing else happens;
 *   - it is the LAST session           -> refused, because a failed remove would leave
 *                                         the session's tasks CANCELLED and no tab to show;
 *   - the target exists and is not last-> cancelled, exported, the reply NAMES it, and the
 *                                         survivor is loaded.
 *
 * The reply is what is asserted, not just the bool: the success line is the only place
 * the user learns WHICH session went away (`[id] name`), and it must be the name that was
 * asked for, not "the active one".
 *
 * Harness: a real channel actor on a real runner with the adapter observer installed, the
 * same path the TUI receives replies on -- identical to test_commands_report.c.
 */

#include "edc_channel_actor.h"
#include "test_ws.h"
#include "edc_commands.h"
#include "edc_compat.h"
#include "edc_config.h"
#include "edc_scheduler.h"
#include "edc_session.h"
#include "edc_session_manager.h"
#include "log.h"
#include "sds.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "test_fs_util.h"

volatile int _run_loop = 1;

static int g_passed = 0, g_failed = 0;

#define CHECK(c, m) do { \
    if (!(c)) { printf("    FAIL: %s\n", m); g_failed++; } \
    else { printf("    ok: %s\n", m); g_passed++; } \
} while (0)

#define HAS(hay, needle) ((hay) != NULL && strstr((hay), (needle)) != NULL)

/* ── capturing the replies ────────────────────────────────────────────────────
 *
 * ALL replies for a driven line are kept, not just the last one. `/session remove`
 * deliberately sends two things: the "Session removed" line and, when the removal
 * changed which session is displayed, an active-session notice whose content is the new
 * id. Taking only the most recent reply picked up the notice and silently lost the line
 * the test was about.
 *
 * The observer runs on the actor runner thread, so the capture is guarded and the reader
 * waits on the same mutex+cond. */
static uv_mutex_t g_lock;
static uv_cond_t g_cond;
static sds g_all = NULL; /* every reply since the last reset, newline separated */
static int g_replies = 0;

static void on_channel_send(void *instance, struct edc_channel_message *msg)
{
    (void)instance;
    if (!msg || !msg->content)
        return;
    uv_mutex_lock(&g_lock);
    if (!g_all)
        g_all = sdsempty();
    g_all = sdscatsds(g_all, msg->content);
    g_all = sdscat(g_all, "\n");
    g_replies++;
    uv_cond_signal(&g_cond);
    uv_mutex_unlock(&g_lock);
}

static bool wait_replies(int want, int timeout_ms)
{
    uv_mutex_lock(&g_lock);
    uint64_t deadline = uv_now(uv_default_loop()) + (uint64_t)timeout_ms;
    while (g_replies < want)
    {
        if (uv_cond_timedwait(&g_cond, &g_lock, 50ULL * 1000000ULL) != 0)
        {
            if (uv_now(uv_default_loop()) >= deadline)
                break;
        }
    }
    const bool ok = g_replies >= want;
    uv_mutex_unlock(&g_lock);
    return ok;
}

static sds all_replies(void)
{
    uv_mutex_lock(&g_lock);
    sds out = g_all ? sdsdup(g_all) : NULL;
    uv_mutex_unlock(&g_lock);
    return out;
}

static int reply_count(void)
{
    uv_mutex_lock(&g_lock);
    const int n = g_replies;
    uv_mutex_unlock(&g_lock);
    return n;
}

static void reset_capture(void)
{
    uv_mutex_lock(&g_lock);
    if (g_all)
    {
        sdsfree(g_all);
        g_all = NULL;
    }
    g_replies = 0;
    uv_mutex_unlock(&g_lock);
}

/* ── the harness ──────────────────────────────────────────────────────────── */

static edc_actor_t *g_chan = NULL;

/* Drive one line through the public command entry and return every reply it produced
 * (newline separated, caller frees), or NULL when nothing arrived. The handler takes
 * ownership of the message. */
static sds say(struct edc_config *cfg, struct edc_session_manager *sm, struct edc_scheduler *sched,
               const char *content)
{
    reset_capture();
    struct edc_channel_message *cm =
        edc_channel_message_new(edc_channel_adapter, EDC_CHANNEL_MESSAGE_USER_TYPE, sdsnew(content));
    edc_commands_handler(cfg, sm, sched,
                         (struct edc_reply_to){.channel_actor = g_chan, .type = cm->type}, cm);
    edc_scheduler_schedule(sched); /* the actor handler schedules at the end of every message */
    if (!wait_replies(1, 5000))
        return NULL;
    /* Let a follow-up notice (the active-session change) land before reading. */
    int seen = reply_count();
    for (int i = 0; i < 20; i++)
    {
        uv_sleep(10);
        const int now = reply_count();
        if (now == seen)
            break;
        seen = now;
    }
    return all_replies();
}

int main(void)
{
    edc_test_workspace();

    uv_mutex_init(&g_lock);
    uv_cond_init(&g_cond);

    g_chan = edc_channel_actor_new();
    edc_actor_runner_t *runner = edc_actor_runner_new();
    edc_actor_runner_add(runner, g_chan);
    edc_actor_runner_start(runner);
    edc_channel_adapter_init(g_chan, NULL, on_channel_send);

    char ws[1024];


    test_tmp_template(ws, sizeof(ws), "edc_cmd_session_remove");
    if (!mkdtemp(ws))
    {
        perror("mkdtemp");
        return 2;
    }
    /* A real directory for `/session new` to bind to. */
    char dir[PATH_MAX];
    snprintf(dir, sizeof(dir), "%s/repo", ws);
    if (mkdir(dir, 0755) != 0)
    {
        perror("mkdir");
        return 2;
    }

    struct edc_config *cfg = g_new0(struct edc_config, 1);
    cfg->workspace = sdsnew(ws);
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    struct edc_session_manager *sm = edc_session_manager_new(cfg);
    if (!sm)
    {
        printf("FAIL: cannot build the session manager\n");
        return 2;
    }
    sched->sessions = sm;

    /* ── removing a session that is not there ─────────────────────────────── */
    printf("\n── /session remove: unknown name ──\n");
    {
        sds r = say(cfg, sm, sched, "/session remove nope");
        printf("    reply: %.120s\n", r ? r : "(none)");
        CHECK(r != NULL && HAS(r, "Session not found"), "an unknown name is refused by name");
        CHECK(r != NULL && HAS(r, "nope"), "and the reply repeats what was typed");
        sdsfree(r);
    }

    /* ── removing the only session is refused ─────────────────────────────── */
    printf("\n── /session remove: the last session ──\n");
    {
        /* Whatever the first session is, with nothing else around it is the last one. */
        struct edc_session *only = edc_session_manager_get_active(sm);
        const sds only_name = only ? sdsnew(edc_session_get_name(only)) : NULL;
        printf("    (only session: %s, count=%zu)\n", only_name ? only_name : "(none)",
               (size_t)sm->session_count);

        char cmd[1024];
        snprintf(cmd, sizeof(cmd), "/session remove %s", only_name ? only_name : "");
        sds r = say(cfg, sm, sched, cmd);
        printf("    reply: %.120s\n", r ? r : "(none)");
        CHECK(r != NULL && HAS(r, "cannot remove the last session"), "removing the last session is refused");
        CHECK(edc_session_manager_get_active(sm) != NULL, "and a session is still there");
        sdsfree(r);
        sdsfree(only_name);
    }

    /* ── removing a real second session ───────────────────────────────────── */
    printf("\n── /session remove: a real one ──\n");
    {
        char newcmd[PATH_MAX + 32];
        snprintf(newcmd, sizeof(newcmd), "/session new worker %s", dir);
        sds r = say(cfg, sm, sched, newcmd);
        printf("    (/session new -> %s)\n", r ? r : "(none)");
        sdsfree(r);

        const int before = (int)sm->session_count;
        CHECK(before >= 2, "a second session exists to remove");

        r = say(cfg, sm, sched, "/session remove worker");
        printf("    reply: %.160s\n", r ? r : "(none)");
        /* One check, not two: "the reply mentions worker" alone is satisfied by
         * "Session not found: `worker`", which is how a wrong-outcome reply would sneak
         * past. Success and the name have to hold in the same reply. */
        CHECK(r != NULL && HAS(r, "Session removed") && HAS(r, "worker"),
              "it reports success AND names the session that was removed");
        sdsfree(r);

        CHECK((int)sm->session_count == before - 1, "the session count went down by one");
        CHECK(edc_session_manager_find(sm, "worker") == NULL, "the removed session is gone from the manager");
        CHECK(edc_session_manager_get_active(sm) != NULL, "a session survives (the tab is usable)");
    }

    /* ── an unknown command is named, not answered generically ────────────── */
    printf("\n── an unknown command ──\n");
    {
        sds r = say(cfg, sm, sched, "/nosuchcommand arg");
        printf("    reply: %.120s\n", r ? r : "(none)");
        CHECK(r != NULL && HAS(r, "Unknown command"), "an unknown command is reported as unknown");
        CHECK(r != NULL && HAS(r, "nosuchcommand"), "and the reply quotes what was typed");
        CHECK(r != NULL && HAS(r, "/help"), "and points at /help");
        sdsfree(r);
    }

    /* ── a flag with no value is a usage error, not a silent no-op ────────── */
    printf("\n── /task new with a dangling flag ──\n");
    {
        sds r = say(cfg, sm, sched, "/task new do-something -d");
        printf("    reply: %.120s\n", r ? r : "(none)");
        CHECK(r != NULL && HAS(r, "needs a value"), "`-d` with no value is refused");
        CHECK(r != NULL && HAS(r, "-d"), "and the reply names the flag");
        sdsfree(r);

        /* And nothing was created: the refused line must not leave half a task behind. */
        r = say(cfg, sm, sched, "/task list");
        CHECK(r != NULL && !HAS(r, "do-something"), "no task was created by the refused line");
        sdsfree(r);
    }

    reset_capture();
    char cmd[PATH_MAX + 32];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", ws);
    if (test_sh(cmd) != 0)
        fprintf(stderr, "(warning: could not clean %s)\n", ws);

    printf("\n=== %d passed, %d failed ===\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
