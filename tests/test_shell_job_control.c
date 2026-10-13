/* Group / foreground semantics of one shell command line.
 *
 * Pins what `&`, `--bg` and the `@` prefix mean for the *group* dimension:
 *   - a plain command opens a group (when the session has none) and takes the
 *     foreground;
 *   - `cmd &` joins the **current** group and stays off the foreground -- it is not
 *     an "@" command, it does not open a group of its own;
 *   - `@cmd "..."` opens a **new** group (and takes the foreground unless background);
 *   - and a task that declared itself background must not touch the foreground concept
 *     at all -- not even when the session has no foreground group, which is the case
 *     that used to promote it (the AST used to set the foreground pointer there).
 *
 * Plus the syntax rule: an `@` command that carries options must quote its
 * description, otherwise nothing is built at all.
 *
 * And the serialization rule: joining the group that is in front means queueing behind
 * whatever is still running there (one in front at a time), while a background task in
 * the same group runs alongside; the active pointer -- what the frontend shows live --
 * follows whoever is running.
 *
 * harness copied from test_commands_task_new: drive the public edc_commands_handler
 * entry, then call edc_scheduler_schedule ourselves (the message end is the tick).
 * The spy hands us the task that was actually dispatched, so nothing is matched by
 * text. No channel actor ⇒ replies go nowhere.
 */
#include "edc_channel_actor.h"
#include "edc_commands.h"
#include "edc_compat.h"
#include "edc_config.h"
#include "edc_scheduler.h"
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

/* Returns the task this command line dispatched (each drive creates exactly one). */
static struct edc_task *drive_command(struct edc_config *cfg, struct edc_session_manager *sm,
                                      struct edc_scheduler *sched, const char *content)
{
    struct edc_channel_message *cm =
        edc_channel_message_new(edc_channel_adapter, EDC_CHANNEL_MESSAGE_USER_TYPE, sdsnew(content));
    edc_commands_handler(cfg, sm, sched, (struct edc_reply_to){.type = cm->type}, cm);
    g_llm_calls = 0;
    g_llm_task = NULL;
    /* The command layer only attaches the input; running it is the message-end scan. */
    edc_scheduler_schedule(sched);
    return g_llm_task;
}

/* Number of groups the session holds -- to assert that a rejected command line built
 * nothing at all (not even the group it would have opened). */
static int group_count(struct edc_session *sess)
{
    int n = 0;
    struct edc_task_group *g;
    DL_FOREACH(sess->groups, g)
    n++;
    return n;
}

/* A task that is waiting keeps out of send_llm, so lookup by description is the only
 * way to reach it. */
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

int main(void)
{
    const char *ws = "/tmp/test_shell_job_control_ws";
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "rm -rf %s && mkdir -p %s", ws, ws);
    if (system(cmd) != 0)
        return 1;

    struct edc_config *cfg = calloc(1, sizeof(*cfg));
    cfg->workspace = sdsnew(ws);
    struct edc_session_manager *sm = edc_session_manager_new(cfg);
    struct edc_session *sess = edc_session_manager_get_active(sm);
    CHECK(sess != NULL, "manager has an active session");
    if (!sess)
        return 1;

    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, sess);
    sched->send_llm = mock_send_llm;

    /* ── 普通命令: 开组 + 占前台 ─────────────────────────────────── */

    TEST("a plain command opens a group and takes the foreground");
    struct edc_task *seed = drive_command(cfg, sm, sched, "seed");
    CHECK(seed != NULL, "the task ran");
    struct edc_task_group *g0 = seed ? seed->group : NULL;
    CHECK(g0 != NULL && g0 == edc_scheduler_get_foreground(sess), "its group is the foreground");
    CHECK(edc_session_get_active_task(sess) == seed, "and it is the active task");

    /* ── cmd & : 进当前组, 不占前台 ─────────────────────────────── */

    TEST("`cmd &` joins the current group and leaves the foreground alone");
    struct edc_task *bg = drive_command(cfg, sm, sched, "helper &");
    CHECK(bg != NULL, "the background task ran anyway (bg is not 'do not run')");
    CHECK(bg && bg->group == g0, "it joined the group already in front");
    CHECK(g0 && g0->member_count == 2, "member count grew");
    CHECK(edc_scheduler_get_foreground(sess) == g0, "the foreground did not move");
    CHECK(edc_session_get_active_task(sess) == seed, "nor did the active task");

    /* ── 没有前台组时的 cmd & ───────────────────────────────────── */

    TEST("`cmd &` with no foreground group does not make one");
    /* 与 `/task bg`(无参数) 同一效果: 清掉前台指针。 */
    edc_scheduler_set_foreground(sess, NULL);
    edc_session_set_active_task(sess, NULL);
    struct edc_task *lonely = drive_command(cfg, sm, sched, "lonely &");
    CHECK(lonely != NULL, "the task ran");
    CHECK(lonely && lonely->group && lonely->group != g0, "nowhere to join -> a group of its own");
    CHECK(edc_scheduler_get_foreground(sess) == NULL, "still no foreground group");
    CHECK(edc_session_get_active_task(sess) == NULL, "still no active task");

    /* ── @ + 后台 + 没有前台组: 不得被推上前台 ───────────────────── */

    TEST("`@cmd \"...\" &` with no foreground group must not be promoted");
    struct edc_task *at = drive_command(cfg, sm, sched, "@grpwork \"new group work\" &");
    CHECK(at != NULL, "the task ran");
    CHECK(at && at->group && lonely && at->group != lonely->group, "the @ opened its own group");
    CHECK(edc_scheduler_get_foreground(sess) == NULL, "a background task did not touch the foreground");
    CHECK(edc_session_get_active_task(sess) == NULL, "nor the active task");

    /* 同一件事的另一种拼法: 整行以引号开头时, 后面的 `--bg` 才真的被解析
     * (parser 的规矩: 不以引号开头 => 整段就是输入, 不扫 flag)。 */
    TEST("`@\"...\" --bg` with no foreground group must not be promoted either");
    struct edc_task *at_bg = drive_command(cfg, sm, sched, "@\"quoted bg\" --bg");
    CHECK(at_bg != NULL, "the task ran");
    CHECK(at_bg && at_bg->group && at->group && at_bg->group != at->group, "its own group again");
    CHECK(edc_scheduler_get_foreground(sess) == NULL, "still no foreground group");
    CHECK(edc_session_get_active_task(sess) == NULL, "still no active task");

    /* ── desc 没带引号却带选项: 语法错误, 什么都不建 ────────────────── */

    TEST("`@cmd \"...\" --bg` with an unquoted desc is a syntax error");
    int groups_before = group_count(sess);
    struct edc_task *rejected = drive_command(cfg, sm, sched, "@grpwork \"new group work\" --bg");
    CHECK(rejected == NULL, "no task was created (nothing was dispatched)");
    CHECK(group_count(sess) == groups_before, "and no group was opened either");
    CHECK(edc_scheduler_get_foreground(sess) == NULL, "the foreground is still untouched");

    /* ── @ 不带 --bg: 这才是占前台的那条路 ──────────────────────── */

    TEST("`@cmd \"...\"` without --bg is the one that takes the foreground");
    struct edc_task *at_fg = drive_command(cfg, sm, sched, "@grp plain work");
    CHECK(at_fg != NULL, "the task ran");
    CHECK(at_fg && at_fg->group && at_fg->group == edc_scheduler_get_foreground(sess),
          "its new group became the foreground");
    CHECK(edc_session_get_active_task(sess) == at_fg, "and it became the active task");

    /* ── 前台串行: 同组的下一条排在前一条后面 ──────────────────────── */

    TEST("a second foreground task in the same group queues behind the first");
    /* 此刻前台组就是 at_fg 的组, 而 at_fg 正在跑。 */
    struct edc_task *queued = drive_command(cfg, sm, sched, "queued work");
    CHECK(queued == NULL, "nothing was dispatched -- it waits for the one in front");
    struct edc_task *waiting = find_task_by_desc(sess, "queued work");
    CHECK(waiting != NULL, "but the task was created");
    CHECK(waiting && edc_task_awaits_deps(waiting), "it is waiting on a dependency");
    CHECK(waiting && waiting->deps_total == 1, "one SEQUENCE edge");
    CHECK(at_fg && waiting && waiting->group == at_fg->group, "and it joined the same group");
    CHECK(at_fg && edc_scheduler_get_foreground(sess) == at_fg->group, "the group in front did not move");
    CHECK(edc_session_get_active_task(sess) == at_fg, "nor did the front land on a waiting task");

    TEST("a background task in that same group is not chained");
    struct edc_task *aside = drive_command(cfg, sm, sched, "aside &");
    CHECK(aside != NULL && aside->group == at_fg->group, "it joined the same group");
    CHECK(aside && aside->deps_total == 0, "but it waits for nobody");
    CHECK(g_llm_task == aside, "so it ran alongside the one in front");
    CHECK(edc_session_get_active_task(sess) == at_fg, "and it did not take the front");

    TEST("when the one in front finishes, the queued task runs and takes the front");
    edc_task_mark_completed(at_fg);
    edc_task_notify_dependents(at_fg); /* what the scheduler does when a task completes */
    g_llm_calls = 0;
    g_llm_task = NULL;
    edc_scheduler_schedule(sched);
    CHECK(g_llm_task == waiting, "the queued task was released and driven");
    CHECK(waiting && waiting->current_request_id != NULL, "it is running now");
    CHECK(edc_session_get_active_task(sess) == waiting, "and the front followed whoever is running");

    /* ── 无 id 的 /task stop: 打"场上的那个", 不是屏幕焦点 ───────── */

    TEST("`/task stop` with no id targets the one in front, not the screen focus");
    CHECK(edc_scheduler_current_task(sess) == aside, "the derivation picks the last unfinished member");
    CHECK(edc_session_get_active_task(sess) == waiting, "while the focus sits on another task");
    drive_command(cfg, sm, sched, "/task stop");
    CHECK(aside && edc_task_is_cancelled(aside), "the task in front was stopped");
    CHECK(waiting && !edc_task_is_cancelled(waiting), "the focused task was left alone");

    printf("\n  %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
