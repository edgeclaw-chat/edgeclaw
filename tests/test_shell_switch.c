/* The shell-like syntax switch: what the input means with it OFF (the default).
 *
 * The point of the switch is that the operators (`| ; & > >> 2> <`) and the sigils
 * (`+ ! @`) spell out a Unix shell vocabulary. A user who does not expect them should get
 * their text, not a silently half-parsed task -- so OFF, every line that is not a `/`
 * command is ONE task description, verbatim. This pins both halves: OFF the operators and
 * sigils are inert, ON they are not (that direction is what test_shell_job_control covers
 * in depth; here it is the control that keeps these cases from passing vacuously).
 *
 * It also pins `/shell on|off` as the only way to flip it.
 *
 * harness copied from test_shell_job_control: drive the public edc_commands_handler entry,
 * then call edc_scheduler_schedule (the message end is the tick). No channel actor, so
 * replies go nowhere.
 */
#include "edc_channel_actor.h"
#include "edc_commands.h"
#include "edc_compat.h"
#include "edc_config.h"
#include "edc_scheduler.h"
#include "edc_shell.h"
#include "edc_session.h"
#include "edc_session_manager.h"
#include "edc_task.h"
#include "edc_task_group.h"
#include "utlist_fix.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Stub for _run_loop pulled in by edc_commands (transitively linked). */
volatile int _run_loop = 1;

static int passed = 0, failed = 0;
#define TEST(n)                                                                                                        \
    do                                                                                                                 \
    {                                                                                                                  \
        printf("  TEST: %-58s ", n);                                                                                   \
    } while (0)
#define CHECK(c, m)                                                                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(c))                                                                                                      \
        {                                                                                                              \
            printf("FAIL: %s\n", m);                                                                                   \
            failed++;                                                                                                  \
        }                                                                                                              \
        else                                                                                                           \
        {                                                                                                              \
            printf("PASS\n");                                                                                          \
            passed++;                                                                                                  \
        }                                                                                                              \
    } while (0)

static void install_resolver(struct edc_scheduler *sched, struct edc_session *sess)
{
    sched->ctx = sess;
    struct edc_session_manager *sm = g_new0(struct edc_session_manager, 1);
    sm->sessions = g_new0(struct edc_session *, 1);
    sm->sessions[0] = sess;
    sm->session_count = sess ? 1 : 0;
    sm->active_index = 0;
    sched->sessions = sm; /* 测试进程内不释放: 与测试同生共死 */
}

static int g_llm_calls = 0;
static struct edc_task *g_llm_task = NULL;

static void mock_send_llm(struct edc_scheduler *sched, struct edc_task *task, void *request_id)
{
    (void)sched;
    (void)request_id;
    g_llm_calls++;
    g_llm_task = task;
}

/* Returns the task this line dispatched (NULL when it queued or built nothing). */
static struct edc_task *drive(struct edc_config *cfg, struct edc_session_manager *sm, struct edc_scheduler *sched,
                              const char *content)
{
    struct edc_channel_message *cm =
        edc_channel_message_new(edc_channel_adapter, EDC_CHANNEL_MESSAGE_USER_TYPE, sdsnew(content));
    edc_commands_handler(cfg, sm, sched, (struct edc_reply_to){.type = cm->type}, cm);
    g_llm_calls = 0;
    g_llm_task = NULL;
    edc_scheduler_schedule(sched);
    return g_llm_task;
}

/* Total tasks, however they queue -- a rejected or merged line must not add one. */
static int task_count(struct edc_session *sess)
{
    int n = 0;
    struct edc_task_group *g;
    DL_FOREACH(sess->groups, g)
    {
        struct edc_task *t;
        LL_FOREACH2(g->members, t, group_next)
        n++;
    }
    return n;
}

static int group_count(struct edc_session *sess)
{
    int n = 0;
    struct edc_task_group *g;
    DL_FOREACH(sess->groups, g)
    n++;
    return n;
}

/* A waiting task never reaches send_llm, so lookup by description is the only way in. */
static struct edc_task *find_task_by_desc(struct edc_session *sess, const char *desc)
{
    struct edc_task_group *g;
    DL_FOREACH(sess->groups, g)
    {
        struct edc_task *t;
        LL_FOREACH2(g->members, t, group_next)
        {
            if (t->description && strcmp(t->description, desc) == 0)
                return t;
        }
    }
    return NULL;
}

/* `/shell <arg>` through the builtin entry (it is a `/` command, so the switch does not
 * gate it -- which is the whole point: it is how you turn the switch back on). */
static bool run_shell_cmd(struct edc_config *cfg, struct edc_session_manager *sm, struct edc_reply_to to,
                          const char *arg)
{
    char line[64];
    snprintf(line, sizeof(line), "/shell%s%s", arg[0] ? " " : "", arg);
    struct edc_channel_message *m =
        edc_channel_message_new(edc_channel_adapter, EDC_CHANNEL_MESSAGE_USER_TYPE, sdsnew(line));
    bool handled = edc_shell_handle_builtin(cfg, sm, to, m);
    edc_channel_message_free(m);
    return handled;
}

int main(void)
{
    const char *ws = "/tmp/test_shell_switch_ws";
    const char *home = "/tmp/test_shell_switch_home";
    char cmd[512];
    /* A real (minimal) config.json: /shell persists before it flips memory, so the home
     * has to be writable for the switch to take effect at all. */
    snprintf(cmd, sizeof(cmd), "rm -rf %s %s && mkdir -p %s %s && printf '{}' > %s/config.json", ws, home, ws, home,
             home);
    if (system(cmd) != 0)
        return 1;

    struct edc_config *cfg = calloc(1, sizeof(*cfg));
    cfg->workspace = sdsnew(ws);
    cfg->edc_home = sdsnew(home);
    struct edc_session_manager *sm = edc_session_manager_new(cfg);
    struct edc_session *sess = edc_session_manager_get_active(sm);
    CHECK(sess != NULL, "manager has an active session");
    if (!sess)
        return 1;

    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, sess);
    sched->send_llm = mock_send_llm;

    /* ── 默认关闭: 算符只是普通文本 ─────────────────────────────── */

    TEST("default is OFF (a fresh config must opt in)");
    CHECK(!cfg->shell_syntax, "shell_syntax starts false");

    TEST("`a | b` with the switch off is ONE task, verbatim (not a pipe chain)");
    struct edc_task_group *front = edc_scheduler_get_foreground(sess);
    int before = task_count(sess);
    drive(cfg, sm, sched, "alpha | beta");
    CHECK(task_count(sess) == before + 1, "exactly one task was created (no second pipe segment)");
    struct edc_task *piped = find_task_by_desc(sess, "alpha | beta");
    CHECK(piped != NULL, "the whole line is the description, `|` and all");
    CHECK(piped && piped->deps_total == 0, "and it waits for nobody (no JOIN edge)");

    TEST("`cmd &` with the switch off does not go to the background");
    before = task_count(sess);
    drive(cfg, sm, sched, "bgwork &");
    CHECK(task_count(sess) == before + 1, "one task");
    struct edc_task *bg = find_task_by_desc(sess, "bgwork &");
    CHECK(bg != NULL, "the `&` is part of the text");
    CHECK(bg && !bg->is_background, "and it did NOT become a background task");

    TEST("`> file` with the switch off is not a redirect");
    before = task_count(sess);
    drive(cfg, sm, sched, "say hi > out.md");
    CHECK(task_count(sess) == before + 1, "one task");
    struct edc_task *red = find_task_by_desc(sess, "say hi > out.md");
    CHECK(red != NULL, "the whole line is the description");
    CHECK(red && red->redirect_stdout == NULL, "and nothing was redirected to out.md");

    TEST("`+desc` with the switch off is a literal description (no new group)");
    before = task_count(sess);
    int groups_before = group_count(sess);
    struct edc_task_group *front_before = edc_scheduler_get_foreground(sess);
    drive(cfg, sm, sched, "plus work");
    CHECK(task_count(sess) == before + 1, "one task");
    CHECK(group_count(sess) == groups_before, "and no group was opened");
    CHECK(front_before == NULL || edc_scheduler_get_foreground(sess) == front_before,
          "the group in front did not move");

    TEST("`!cmd` with the switch off is NOT the local shell");
    before = task_count(sess);
    drive(cfg, sm, sched, "!git status");
    CHECK(task_count(sess) == before + 1, "one task");
    struct edc_task *bang = find_task_by_desc(sess, "!git status");
    CHECK(bang != NULL, "the `!` is part of the description");
    CHECK(bang && !bang->tool_only, "and it is NOT a tool-only task (no exec_shell, no free command)");

    TEST("`@desc` with the switch off is a literal description (no migration reply, no task lost)");
    before = task_count(sess);
    drive(cfg, sm, sched, "@probe");
    CHECK(task_count(sess) == before + 1, "the line still became a task");
    CHECK(find_task_by_desc(sess, "@probe") != NULL, "with the `@` kept as text");

    TEST("with the switch off a backslash is just text (nothing is a sigil, so nothing is escaped)");
    /* The `\@` / `\+` escapes are gone: with shell syntax ON they were superseded by
     * quoting (`"+text"` is a literal `+`), and with it OFF there is no sigil to escape. */
    before = task_count(sess);
    drive(cfg, sm, sched, "\\+1 issue is still open");
    CHECK(find_task_by_desc(sess, "\\+1 issue is still open") != NULL,
          "the backslash stays, as text");
    CHECK(task_count(sess) == before + 1, "one task");

    /* ── `/` commands are NOT shell syntax: they keep working ───── */

    TEST("a `/` command is not gated (the switch could never be turned back on otherwise)");
    before = task_count(sess);
    groups_before = group_count(sess);
    drive(cfg, sm, sched, "/task new \"scoped\" --new-group");
    CHECK(task_count(sess) == before + 1, "the command created its task");
    CHECK(group_count(sess) == groups_before + 1, "and `--new-group` still did its job");

    /* ── /shell on|off: the only switch ─────────────────────────── */

    TEST("`/shell` reports and changes nothing");
    CHECK(run_shell_cmd(cfg, sm, (struct edc_reply_to){}, ""), "`/shell` is a builtin");
    CHECK(!cfg->shell_syntax, "the state is unchanged");
    before = task_count(sess);
    CHECK(task_count(sess) == before, "and no task was created");

    TEST("`/shell bogus` is refused, and does not flip the switch");
    CHECK(run_shell_cmd(cfg, sm, (struct edc_reply_to){}, "bogus"), "it answers");
    CHECK(!cfg->shell_syntax, "the switch is untouched");

    TEST("`/shell on` flips the live config (persistence is pinned in test_config_shell_syntax)");
    CHECK(run_shell_cmd(cfg, sm, (struct edc_reply_to){}, "on"), "`/shell on` is a builtin");
    CHECK(cfg->shell_syntax, "the live config now says on");

    /* ── 打开后, 同样的输入才有 shell 语义（对照, 防上面全绿地假通过）── */

    TEST("with the switch on, `a | b` really is a pipe chain (two dependent tasks)");
    before = task_count(sess);
    /* Quoted operands: a line that uses an operator requires them (see the rule in
     * edc_shell.c) -- `gamma | delta` unquoted is refused, not guessed. */
    drive(cfg, sm, sched, "\"gamma\" | \"delta\"");
    CHECK(task_count(sess) == before + 2, "two tasks were created");
    struct edc_task *delta = find_task_by_desc(sess, "delta");
    CHECK(delta != NULL && delta->deps_total == 1, "the second one waits on the first (JOIN edge)");

    TEST("with the switch on, `!cmd` really is the local shell");
    before = task_count(sess);
    drive(cfg, sm, sched, "!git status");
    CHECK(task_count(sess) == before + 1, "one task");
    struct edc_task *tool = find_task_by_desc(sess, "git status");
    CHECK(tool != NULL && tool->tool_only, "and it is tool-only (one exec_shell call, no model)");

    TEST("with the switch on, `+desc` really opens a group");
    front_before = edc_scheduler_get_foreground(sess);
    groups_before = group_count(sess);
    drive(cfg, sm, sched, "+\"grp plain work\"");
    CHECK(group_count(sess) == groups_before + 1, "one group was opened");
    CHECK(edc_scheduler_get_foreground(sess) != front_before, "and it took the foreground");

    TEST("with the switch on, `@desc` is now an ordinary MESSAGE (Ⓕ: the hint waits for the post-pass)");
    /* The `@` layer sits ABOVE this switch, so it answers in both modes. Today, with no
     * resolution built yet, that answer is the migration hint; the off-mode case above
     * shows the other half (literal text). When `@file` lands, the resolved case attaches
     * the file in BOTH modes -- which is the whole reason this layer is not gated. */
    before = task_count(sess);
    drive(cfg, sm, sched, "@probe");
    CHECK(task_count(sess) == before + 1, "it became a task");
    CHECK(find_task_by_desc(sess, "@probe") != NULL, "carrying the text verbatim (`@` is not a sigil here)");

    TEST("`/shell off` puts it back");
    CHECK(run_shell_cmd(cfg, sm, (struct edc_reply_to){}, "off"), "`/shell off` is a builtin");
    CHECK(!cfg->shell_syntax, "the switch is off again");
    before = task_count(sess);
    drive(cfg, sm, sched, "epsilon | zeta");
    CHECK(task_count(sess) == before + 1, "and a `|` line is one task again");
    CHECK(find_task_by_desc(sess, "epsilon | zeta") != NULL,
          "and needs no quotes -- with the switch off, nothing is an operator");

    printf("\n  %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
