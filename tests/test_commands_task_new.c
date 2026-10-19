/* /task new: `desc` is the input, so the task runs on creation.
 *
 * handle_task_new is static, so this drives the public edc_commands_handler entry
 * with a spy send_llm to observe whether it actually ran. */
#include "edc_channel_actor.h"
#include "edc_commands.h"
#include "edc_config.h"
#include "edc_scheduler.h"
#include "edc_session.h"
#include "edc_session_manager.h"
#include "edc_compat.h"
#include "edc_task.h"
#include "edc_task_group.h"
#include "edc_turn.h"
#include "utlist_fix.h"
#include <cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* The in-memory session store handed to the scheduler; an empty label means the
 * active session (active_index points at the first). */
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
static int passed = 0, failed = 0;
#define TEST(n)                                                                                                        \
    do                                                                                                                 \
    {                                                                                                                  \
        printf("  TEST: %-55s ", n);                                                                                   \
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

static int g_llm_calls = 0;
static struct edc_task *g_llm_task = NULL;

static void mock_send_llm(struct edc_scheduler *sched, struct edc_task *task, void *request_id)
{
    (void)sched;
    (void)request_id;
    g_llm_calls++;
    g_llm_task = task;
}

/* 走公开命令入口。注意 handler 可能接管/释放 message, 所以这里不 free —— 测试
 * 里宁可漏一点也不要 double free。 */
static void drive_command(struct edc_config *cfg, struct edc_session_manager *sm, struct edc_scheduler *sched,
                          const char *content)
{
    struct edc_channel_message *cm =
        edc_channel_message_new(edc_channel_adapter, EDC_CHANNEL_MESSAGE_USER_TYPE, sdsnew(content));
    /* 这个用例没有通道 actor (收不到回话, 也不关心) —— 回话地址只给类型。 */
    edc_commands_handler(cfg, sm, sched, (struct edc_reply_to){.type = cm->type}, cm);
    /* 命令层只**装输入**; "把它跑起来"发生在消息末尾的调度 (`edc_scheduler_schedule`
     * —— actor handler 在每条消息末尾调它, 见 edc_scheduler.h)。这里直接调那个入口,
     * 等价于"这条消息处理完了"。 */
    edc_scheduler_schedule(sched);
}

/* 挂起的任务不会进 send_llm, 所以只能自己去 session 的组里找。 */
static int count_tasks(struct edc_session *sess)
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

/* 这条 turn 发给 LLM 的第一条用户消息。 */
static const char *first_user_content(struct edc_turn *turn)
{
    if (!turn || turn->messages.tail == turn->messages.head)
        return NULL;
    cJSON *msg = turn->messages.data[turn->messages.head];
    return cJSON_GetStringValue(cJSON_GetObjectItem(msg, "content"));
}

/* 组里除 |skip| 之外最后一个成员。 */
static struct edc_task *last_member_except(struct edc_task_group *g, struct edc_task *skip)
{
    struct edc_task *last = NULL, *t;
    if (!g)
        return NULL;
    LL_FOREACH2(g->members, t, group_next)
    {
        if (t != skip)
            last = t;
    }
    return last;
}

int main(void)
{
    const char *ws = "/tmp/test_commands_task_new_ws";
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "rm -rf %s && mkdir -p %s", ws, ws);
    if (system(cmd) != 0)
        return 1;

    struct edc_config *cfg = calloc(1, sizeof(*cfg));
    cfg->workspace = sdsnew(ws);
    /* The `+/task new` case below is shell syntax; the switch is off by default. */
    cfg->shell_syntax = true;
    struct edc_session_manager *sm = edc_session_manager_new(cfg);
    struct edc_session *sess = edc_session_manager_get_active(sm);
    CHECK(sess != NULL, "manager has an active session");
    if (!sess)
        return 1;

    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, sess);
    sched->send_llm = mock_send_llm;

    /* ── /task new <desc> 必须创建**并且**派发 ──────────────────── */

    TEST("/task new <desc> creates and dispatches in one go");
    g_llm_calls = 0;
    g_llm_task = NULL;
    drive_command(cfg, sm, sched, "/task new probe");

    CHECK(g_llm_calls == 1, "send_llm called once (created, then dispatched at the message end)");
    CHECK(g_llm_task != NULL, "a task was handed to send_llm");
    if (g_llm_task)
    {
        CHECK(edc_task_awaits_exec(g_llm_task), "state advanced to LLM_PROCESSING");
        CHECK(g_llm_task->active_turn != NULL, "has an active turn (not a dead end)");
        CHECK(g_llm_task->current_request_id != NULL, "request registered (late events can be dropped)");
        CHECK(g_llm_task->description && strcmp(g_llm_task->description, "probe") == 0,
              "description is the desc argument");
    }

    /* `/task new --suspended` was removed (never used): a task is dispatched on
     * creation, always. A task restored from disk comes back as INTERRUPTED, and
     * `/task resume` is covered by
     * test_cross_dep_restart (it drives the real command). */

    /* ── 默认进当前组（因此会排队）; 想另起一段得显式 --new-group ──── */

    TEST("/task new (no flags) joins the group in front and queues there");
    struct edc_task *holder = g_llm_task; /* the task the case above dispatched */
    CHECK(holder && holder->group, "a task holds the front");
    if (holder && holder->group)
    {
        g_llm_calls = 0;
        g_llm_task = NULL;
        drive_command(cfg, sm, sched, "/task new \"queued\"");
        CHECK(g_llm_task == NULL, "nothing was dispatched -- it waits for the one in front");
        struct edc_task *queued = last_member_except(holder->group, holder);
        CHECK(queued && queued->group == holder->group, "it joined the group in front (no new group)");
        CHECK(queued && edc_task_awaits_deps(queued), "and it is waiting on a dependency");
        CHECK(edc_session_get_active_task(sess) == holder, "the front is still on the running task");

        TEST("/task new --new-group opens one, explicitly");
        g_llm_calls = 0;
        g_llm_task = NULL;
        drive_command(cfg, sm, sched, "/task new \"fresh\" --new-group");
        CHECK(g_llm_task != NULL, "it ran immediately -- there is nothing to queue behind");
        CHECK(g_llm_task && g_llm_task->group && strcmp(g_llm_task->group->group_id, g_llm_task->task_id) == 0,
              "in a group of its own");
        CHECK(g_llm_task && g_llm_task->group != holder->group, "which is not the group in front");
        CHECK(g_llm_task && edc_scheduler_get_foreground(sess) == g_llm_task->group,
              "and it took the foreground");
    }

    /* ── 带选项但 desc 没引号: 语法错误 ──────────────────────────── */

    TEST("/task new with options needs the description quoted");
    g_llm_calls = 0;
    g_llm_task = NULL;
    drive_command(cfg, sm, sched, "/task new probe --bg");
    CHECK(g_llm_task == NULL, "nothing was created (no silently-swallowed option)");
    CHECK(g_llm_calls == 0, "and nothing ran");

    /* ── 没有输入: 和 shell 那条路一样, 不该建任务 ───────────────── */

    TEST("/task new with an empty description creates nothing");
    int before = count_tasks(sess);
    g_llm_calls = 0;
    g_llm_task = NULL;
    drive_command(cfg, sm, sched, "/task new \"\"");
    CHECK(count_tasks(sess) == before, "no task was created (a task needs an input)");
    CHECK(g_llm_task == NULL, "nothing was dispatched");

    /* ── 命令建的任务：入参是文本，不过 shell ───────────────────── */

    TEST("/task new takes its input as TEXT: `|` is not a pipe");
    /* A command's input is not a shell line, so nothing in it is an operator. The
     * `--new-group` keeps the group fresh, so deps_total can only come from the input. */
    g_llm_calls = 0;
    g_llm_task = NULL;
    drive_command(cfg, sm, sched, "/task new \"alpha | beta\" --new-group");
    CHECK(g_llm_task != NULL, "the task ran (a fresh group has nothing to queue behind)");
    if (g_llm_task)
    {
        const char *sent = first_user_content(g_llm_task->active_turn);
        CHECK(sent && strcmp(sent, "alpha | beta") == 0, "the whole text reached the model, `|` and all");
        CHECK(g_llm_task->deps_total == 0, "and no second task was made out of it");
    }

    TEST("/task new takes its input as TEXT: `{{LAST}}` is not a reference");
    g_llm_calls = 0;
    g_llm_task = NULL;
    drive_command(cfg, sm, sched, "/task new \"see {{LAST}}\" --new-group");
    CHECK(g_llm_task != NULL, "the task ran");
    if (g_llm_task)
    {
        const char *sent = first_user_content(g_llm_task->active_turn);
        CHECK(sent && strcmp(sent, "see {{LAST}}") == 0, "the text is literal");
        CHECK(!g_llm_task->has_deferred_vars, "no deferred expansion was registered");
        CHECK(g_llm_task->deps_total == 0, "and no dependency was made for it");
    }

    TEST("/task new refuses a stray token after the quoted description");
    int before_stray = count_tasks(sess);
    g_llm_calls = 0;
    g_llm_task = NULL;
    drive_command(cfg, sm, sched, "/task new \"scoped\" junk");
    CHECK(count_tasks(sess) == before_stray, "no task was created (the stray token is refused)");

    /* ── desc 只用于显示名, 送给 LLM 的是完整输入 ──────────────────── */

    TEST("/task new truncates the display name but sends the whole prompt");
    g_llm_calls = 0;
    g_llm_task = NULL;
    /* --new-group: it must run right away, and the group in front already has a runner. */
    drive_command(cfg, sm, sched, "/task new \"a prompt clearly longer than thirty characters\" --new-group");
    CHECK(g_llm_task != NULL, "the task ran");
    if (g_llm_task)
    {
        const char *full = "a prompt clearly longer than thirty characters";
        CHECK(g_llm_task->description && strcmp(g_llm_task->description, full) != 0 &&
                  sdslen(g_llm_task->description) < strlen(full),
              "the description is the truncated display name");
        CHECK(g_llm_task->description && strstr(g_llm_task->description, "...") != NULL,
              "and the truncation is marked");
        const char *sent = first_user_content(g_llm_task->active_turn);
        CHECK(sent && strcmp(sent, full) == 0, "while the prompt reaches the LLM in full");
    }

    /* ── /task resume: 一次一个，没有批量 ──────────────────────────── */

    TEST("/task resume with an option is refused — nothing is revived in bulk");
    /* 先造两个: A 在新组里跑, B 排在它后面(同组)。 */
    g_llm_calls = 0;
    g_llm_task = NULL;
    drive_command(cfg, sm, sched, "/task new \"alpha\" --new-group");
    struct edc_task *alpha = g_llm_task;
    CHECK(alpha != NULL, "A 在跑");
    struct edc_task *beta = NULL;
    if (alpha)
    {
        drive_command(cfg, sm, sched, "/task new \"beta\"");
        beta = last_member_except(alpha->group, alpha);
        CHECK(beta && edc_task_awaits_deps(beta), "B 排在 A 后面");

        /* 模拟重启后的样子: A 被中断。组队列是**顺序**约束, 不是数据依赖 —— 排队等它的
         * B 不该跟着倒: 边作废, B 回到可跑。 */
        CHECK(edc_task_mark_interrupted(alpha), "A 被中断");
        CHECK(beta && !edc_task_has_failed(beta), "排队等它的 B 不倒 (队列边是 BARRIER)");
        CHECK(beta && beta->deps_total == 0, "那条边被作废, 不再是约束");

        /* 批量恢复已删除: `--all` 是用法错误, 不是任务 id, 也不许悄悄复活任何任务。 */
        g_llm_task = NULL;
        drive_command(cfg, sm, sched, "/task resume --all");
        CHECK(alpha && edc_task_is_interrupted(alpha), "`--all` 被拒: A 仍是 INTERRUPTED");
        CHECK(g_llm_task != alpha, "也没有任何请求发给 A");

        /* id 是必须的: 不带 id 的 `/task resume` 同样是用法错误, 不会去"猜"该恢复谁。 */
        drive_command(cfg, sm, sched, "/task resume");
        CHECK(alpha && edc_task_is_interrupted(alpha), "裸 `/task resume` 被拒: A 仍是 INTERRUPTED");
        CHECK(g_llm_task != alpha, "也没有请求发给 A");
    }

    TEST("/task resume <id> revives exactly that task; it joins the back of the queue");
    if (alpha)
    {
        char cmd[128];
        snprintf(cmd, sizeof cmd, "/task resume %s", alpha->task_id);
        g_llm_task = NULL;
        g_llm_calls = 0;
        drive_command(cfg, sm, sched, cmd);
        /* 前台已经被 B 占着, 所以复活的 A 只能排到队尾 —— resume 是"让已有任务再跑起来"
         * 的另一道门, 同样不许插队。 */
        CHECK(g_llm_task != alpha, "A 没有立刻跑: 前台还是 B");
        CHECK(alpha && edc_task_awaits_deps(alpha), "复活的 A 排在队尾 (没有插队)");
        CHECK(g_llm_calls == 0, "这一轮没有新的 LLM 请求 (A 只是重新排队)");
    }

    TEST("/task resume refuses a task that `||` skipped");
    if (alpha && alpha->group)
    {
        /* `src` succeeds → `skipped` is the `||` branch, so it never runs (end=SKIPPED). */
        struct edc_task *src = edc_scheduler_task_new(sched, sess, "|| source", edc_channel_adapter);
        edc_task_group_add_task(alpha->group, src);
        struct edc_task *skipped = edc_scheduler_task_new(sched, sess, "|| branch", edc_channel_adapter);
        edc_task_group_add_task(alpha->group, skipped);
        edc_task_add_dependency(skipped, src, TASK_DEP_UNLESS);
        edc_task_await_exec(src);
        edc_task_mark_completed(src);
        edc_task_notify_dependents(src);
        CHECK(skipped->end == EDC_END_SKIPPED, "夹具: 该分支被 `||` 跳过 (CANCELLED + SKIPPED)");

        char cmd[128];
        snprintf(cmd, sizeof cmd, "/task resume %s", skipped->task_id);
        g_llm_task = NULL;
        g_llm_calls = 0;
        drive_command(cfg, sm, sched, cmd);
        CHECK(edc_task_is_cancelled(skipped) && skipped->end == EDC_END_SKIPPED,
              "resume 被拒: 仍是跳过态 (理由保留，不再被悄悄改成可跑)");
        CHECK(g_llm_calls == 0, "没有请求被发出");
        CHECK(g_llm_task != skipped, "也没有驱动它");

        /* 但它的下游(task BLOCKED on a skipped source)在源成功时并不复活 —— 只有源真的
         * 产出才算数。这里 src 早就完成，所以那个下游的边本来就是完成的。 */
    }

    TEST("/task dep <id> --drop <source> voids the edge so the task can run");
    {
        struct edc_task *dsrc = edc_scheduler_task_new(sched, sess, "dep source", edc_channel_adapter);
        struct edc_task_group *dg = edc_scheduler_create_group(sched, sess, dsrc);
        edc_scheduler_set_foreground(sess, dg);
        struct edc_task *dtgt = edc_scheduler_task_new(sched, sess, "dep target", edc_channel_adapter);
        edc_task_group_add_task(dg, dtgt);
        edc_scheduler_attach_input(sched, dtgt, "go");
        edc_task_add_dependency(dtgt, dsrc, TASK_DEP_SEQUENCE);
        CHECK(edc_task_awaits_deps(dtgt), "夹具: 目标在等源");

        char cmd[160];
        snprintf(cmd, sizeof cmd, "/task dep %s", dtgt->task_id);
        drive_command(cfg, sm, sched, cmd); /* 列表: 不改状态 */
        CHECK(edc_task_awaits_deps(dtgt), "只是列出来，不动状态");

        g_llm_task = NULL;
        g_llm_calls = 0;
        snprintf(cmd, sizeof cmd, "/task dep %s --drop %s", dtgt->task_id, dsrc->task_id);
        drive_command(cfg, sm, sched, cmd);
        CHECK(!edc_task_depends_on(dtgt, dsrc), "边被作废");
        CHECK(g_llm_task == dtgt, "目标随即被驱动 (不再卡在等依赖)");
    }

    TEST("/task dep --drop releases a task that was BLOCKED by that dependency");
    {
        struct edc_task *bsrc = edc_scheduler_task_new(sched, sess, "blocked src", edc_channel_adapter);
        struct edc_task_group *bg = edc_scheduler_create_group(sched, sess, bsrc);
        edc_scheduler_set_foreground(sess, bg);
        struct edc_task *btgt = edc_scheduler_task_new(sched, sess, "blocked tgt", edc_channel_adapter);
        edc_task_group_add_task(bg, btgt);
        edc_scheduler_attach_input(sched, btgt, "go");
        edc_task_add_dependency(btgt, bsrc, TASK_DEP_SEQUENCE);
        edc_task_await_exec(bsrc);
        CHECK(edc_task_mark_interrupted(bsrc), "源被中断");
        CHECK(btgt->end == EDC_END_BLOCKED, "夹具: 目标被拖下水 (BLOCKED)");

        /* 源永远不会再产出了：断掉这条边必须把它放回来，而不是留在 BLOCKED 里。 */
        char cmd[160];
        snprintf(cmd, sizeof cmd, "/task dep %s --drop %s", btgt->task_id, bsrc->task_id);
        g_llm_task = NULL;
        g_llm_calls = 0;
        drive_command(cfg, sm, sched, cmd);
        CHECK(!edc_task_depends_on(btgt, bsrc), "边被作废");
        CHECK(btgt->end == EDC_END_NONE, "BLOCKED 的原因被清掉");
        CHECK(g_llm_task == btgt, "目标随即被驱动 (不再是 BLOCKED 的僵尸)");
    }

    TEST("resume reply names what the task was, and repeats the last error when it had failed");
    {
        struct edc_task *r = edc_task_new("rr-1", "sess-reply", "rr-1", "r", edc_channel_adapter);
        edc_task_await_exec(r);

        sds txt = edc_commands_resume_text(r, EDC_END_INTERRUPTED, EDC_FAIL_NONE);
        CHECK(strstr(txt, "was interrupted") != NULL, "INTERRUPTED 说得出来");
        CHECK(strstr(txt, "Last error") == NULL, "没失败过就不附错误");
        sdsfree(txt);

        edc_task_set_output(r, sdsnew("LLM request failed: 401 Unauthorized\nmore detail follows"));
        txt = edc_commands_resume_text(r, EDC_END_FAILED, EDC_FAIL_OTHER);
        CHECK(strstr(txt, "had failed") != NULL, "FAILED 说得出来");
        CHECK(strstr(txt, "Last error: LLM request failed: 401 Unauthorized") != NULL, "并附上上次错误的第一行");
        CHECK(strstr(txt, "more detail") == NULL, "只取第一行");
        sdsfree(txt);

        sds long_err = sdsnew("boom");
        for (int i = 0; i < 40; i++)
            long_err = sdscat(long_err, " xxxxxxxxx");
        edc_task_set_output(r, long_err);
        txt = edc_commands_resume_text(r, EDC_END_FAILED, EDC_FAIL_OTHER);
        CHECK(strstr(txt, "…") != NULL, "过长的错误被截断并加省略号");
        CHECK(sdslen(txt) < 260, "而且回执不会失控");
        sdsfree(txt);

        txt = edc_commands_resume_text(r, EDC_END_STOPPED, EDC_FAIL_NONE);
        CHECK(strstr(txt, "was stopped") != NULL, "STOPPED 说得出来");
        sdsfree(txt);

        /* 注定失败的失败要说出来：坏 key / 上下文放不下，重试不会自己好。 */
        txt = edc_commands_resume_text(r, EDC_END_FAILED, EDC_FAIL_AUTH);
        CHECK(strstr(txt, "auth failure") != NULL, "AUTH: 预警重试无用");
        CHECK(strstr(txt, "blind retry") != NULL, "并指出该先修什么");
        sdsfree(txt);

        txt = edc_commands_resume_text(r, EDC_END_FAILED, EDC_FAIL_CONTEXT_OVERFLOW);
        CHECK(strstr(txt, "did not fit") != NULL, "CONTEXT_OVERFLOW: 预警同一请求仍放不下");
        sdsfree(txt);

        txt = edc_commands_resume_text(r, EDC_END_FAILED, EDC_FAIL_OTHER);
        CHECK(strstr(txt, "auth failure") == NULL && strstr(txt, "did not fit") == NULL,
              "OTHER/TRANSPORT/RATE_LIMIT 不预警 (重试可能成功)");
        sdsfree(txt);

        edc_task_free(r);
    }

    printf("\n  %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
