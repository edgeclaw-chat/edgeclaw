/* /task new: `desc` 就是 input, 任务**创建即运行** (2026-09-14 决定)。
 *
 * 为什么需要这条测试: 此前 /task new 只建不派发 —— 任务停在 WAITING_INPUT 且
 * **永远等不到输入** (没有任何代码会给它), 是个死胡同。补上派发之后必须有东西
 * 钉住它, 否则很容易再退回去。而且它没有别的见证: 现有测试**本来就不覆盖命令
 * 层**, 全量测试在改动前后都是 43/5。
 *
 * handle_task_new 是 static, 所以走公开入口 edc_commands_handler, 用一个 spy
 * send_llm 观察"到底跑没跑"。
 *
 * 缺口 (如实记录): 这里断言的是"任务有了 active_turn 并且被派发", **没有**
 * 断言那个 turn 里装的就是 desc 的正文 —— 那需要 turn 的消息读取 API, 超出
 * 这条测试想守的范围 (description 已经钉住了"用的是 desc"这一半)。 */
#include "edc_channel_actor.h"
#include "edc_commands.h"
#include "edc_config.h"
#include "edc_scheduler.h"
#include "edc_session.h"
#include "edc_session_manager.h"
#include "edc_compat.h"
#include "edc_task.h"
#include "edc_task_group.h"
#include "utlist_fix.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* ── 单测的 session 仓库 ─────────────────────────────────────────
 * scheduler 现在**同包直调 store** (P3 的 `resolve_session` 端口已退休),
 * 所以测试给它的不是"解析函数"而是**仓库**: 一个只装着
 * 这些 session 的内存 manager (普通结构体, 没有构造函数 —— 与 poke 别的结构体同法)。
 * 标签为空 = 用活跃的那个 (active_index 指向第一个), 与真 store 的策略一致。
 * `ctx` 仍留给测试自己用 (它不再是读口的入参)。 */
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
    const char *ws = "/tmp/test_commands_task_new_ws";
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

    /* ── /task new --suspended: 建, 但不立即执行 ─────────────────── */

    TEST("/task new <desc> --suspended creates it without running it");
    g_llm_calls = 0;
    g_llm_task = NULL;
    /* 注意 /task new 的 flag **只有在引号输入之后才被解析** —— 不加引号时整个
     * 剩余文本都成为 desc, flag 被忽略 (这个陷阱会让人以为新 flag 没生效)。 */
    drive_command(cfg, sm, sched, "/task new \"\"\"shelved\"\"\" --suspended");

    struct edc_task *susp = find_task_by_desc(sess, "shelved");
    CHECK(g_llm_calls == 0, "send_llm NOT called (nothing executed)");
    CHECK(susp != NULL, "the task exists");
    if (susp)
    {
        CHECK(edc_task_is_suspended(susp), "state is SUSPENDED");
        /* 初始相位与普通新建任务相同 —— 输入已经装上了, 只是没跑。 */
        CHECK(susp->active_turn != NULL, "input is attached (same initial phase as a normal task)");
        CHECK(susp->current_request_id == NULL, "no request registered (nothing is in flight)");
        /* 决定 1: 挂起的任务不算 active task。 */
        CHECK(edc_session_get_active_task(sess) != susp, "does not become the active task");
    }

    /* ── resume 之后就走普通路径 ─────────────────────────────────── */

    TEST("/task resume runs a task that was created suspended");
    if (susp)
    {
        g_llm_calls = 0;
        g_llm_task = NULL;
        edc_scheduler_resume_task(sched, susp);
        CHECK(g_llm_calls == 1, "send_llm called once (it runs now)");
        CHECK(edc_task_awaits_exec(susp), "resume advances to LLM_PROCESSING");
    }

    printf("\n  %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
