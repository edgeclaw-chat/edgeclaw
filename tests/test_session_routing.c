/* ══════════════════════════════════════════════════════════════
 *  P3 ①a 防线: 入站消息的 **session 标签** 决定任务落在哪个 session
 *
 *  "当前 session" 只有一份权威 (session_manager.active_index),
 *  scheduler **不知道它**——它把 `session_id` 当**不透明作用域标签**, 需要对象时经
 *  store 读口解析 (规则 8 允许的"向外调用")。
 *
 *  这一拍拆掉的正是那条老路: 入站任务"落在 scheduler 当前绑定的 session 上" ——
 *  绑定是谁取决于最后一次 set_session, 与这条输入是谁发的**毫无关系**。老实现里
 *  "切错台"的家族事故 (working dir 不跟着切、消息写进别人的 session) 全从这里来。
 *
 *  三条断言 (都是**反面**优先):
 *    ① 标签指向 B、而 scheduler 绑着 A ⇒ 任务与组落在 **B**, A 一棵都没多;
 *    ② 标签指向不存在的 session ⇒ **拒绝**这条输入 (不猜、不落到绑定 session);
 *    ③ 标签为空 (crontab 这类没有 session 概念的入站) ⇒ 由读口给默认值。
 * ══════════════════════════════════════════════════════════════ */

#include "edc_actor.h"
#include "edc_channel_actor.h"
#include "edc_compat.h"
#include "edc_config.h"
#include "edc_scheduler.h"
#include "edc_session.h"
#include "edc_task.h"
#include "edc_task_group.h"
#include "log.h"
#include "utlist_fix.h"
#include <signal.h>
#include <stdio.h>
#include <string.h>
/* ── P3 ③ 测试读口 ──────────────────────────────────────────────
 * scheduler 不再持有"当前 session", 所以单测要给它装一个 store 读口: 把请求里
 * 的 session 标签解析成 session 对象 (入站路由 / 快照拉取都走它)。这一层测试
 * 没有 agent, ctx 就留给它 (agent 那层用它自己的读口)。 */
static struct edc_session *test_resolve_session(void *ctx, const char *session_id)
{
    struct edc_session *sess = ctx;
    if (!sess)
        return NULL;
    if (!session_id || !*session_id)
        return sess;
    return (sess->session_id && strcmp(session_id, sess->session_id) == 0) ? sess : NULL;
}

static void install_resolver(struct edc_scheduler *sched, struct edc_session *sess)
{
    sched->ctx = sess;
    sched->resolve_session = test_resolve_session;
}

/* Stub: _run_loop is referenced by edc_commands (pulled in transitively). */
volatile sig_atomic_t _run_loop = 1;

/* ══════════════════════════════════════════════════════════════
 *  Test framework (same minimal pattern as the other tests)
 * ══════════════════════════════════════════════════════════════ */

static int test_failed = 0;
static int test_passed = 0;

#define TEST(name)                                                                                                     \
    do                                                                                                                 \
    {                                                                                                                  \
        log_info("  TEST: %s", name);                                                                                  \
    } while (0)
#define CHECK(cond, msg)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            log_error("    FAIL: %s", msg);                                                                            \
            test_failed++;                                                                                             \
        }                                                                                                              \
        else                                                                                                           \
        {                                                                                                              \
            test_passed++;                                                                                             \
        }                                                                                                              \
    } while (0)

/* ══════════════════════════════════════════════════════════════
 *  Helpers
 * ══════════════════════════════════════════════════════════════ */

static struct edc_config *make_config(void)
{
    struct edc_config *cfg = g_new0(struct edc_config, 1);
    cfg->workspace = sdsnew("/tmp/test_session_routing_workspace");
    return cfg;
}

static int count_groups(struct edc_session *session)
{
    int n = 0;
    struct edc_task_group *g;
    DL_FOREACH(session->groups, g)
    n++;
    return n;
}

static int count_tasks(struct edc_session *session)
{
    int n = 0;
    struct edc_task_group *g;
    DL_FOREACH(session->groups, g)
    {
        struct edc_task *t;
        LL_FOREACH2(g->members, t, group_next)
        n++;
    }
    return n;
}

static char *first_task_id(struct edc_session *session)
{
    struct edc_task_group *g;
    DL_FOREACH(session->groups, g)
    {
        struct edc_task *t;
        LL_FOREACH2(g->members, t, group_next)
        return t->task_id;
    }
    return NULL;
}

/* ── 测试用的 session store: 两张表 + 一个读口 ──────────────
 * 与生产里 agent 注入的那个同形 (标签空 = 默认/活跃, 未知标签 = NULL)。 */
static struct edc_session *g_sess_a = NULL;
static struct edc_session *g_sess_b = NULL;

static struct edc_session *stub_resolve_session(void *ctx, const char *session_id)
{
    (void)ctx;
    if (!session_id || !*session_id)
        return g_sess_a; /* "调用方没指定" → 默认给 A (对应生产里的活跃 session) */
    if (g_sess_a && g_sess_a->session_id && strcmp(session_id, g_sess_a->session_id) == 0)
        return g_sess_a;
    if (g_sess_b && g_sess_b->session_id && strcmp(session_id, g_sess_b->session_id) == 0)
        return g_sess_b;
    return NULL; /* 未知标签 → 调用方必须拒绝 */
}

/* 投递一条入站消息 (模拟前端): |tag| 就是消息上的 session 标签。 */
static void deliver_input(struct edc_scheduler *sched, const char *tag, const char *content, long req_id)
{
    edc_actor_t *actor = edc_scheduler_get_actor(sched);
    struct edc_channel_message *cm =
        edc_channel_message_new(edc_channel_adapter, EDC_CHANNEL_MESSAGE_USER_TYPE, sdsnew(content));
    if (tag)
        cm->session_id = sdsnew(tag);
    edc_actor_msg_t *msg = edc_actor_msg_new(BC_ACTOR_MSG_CHANNEL_RECV, NULL, (void *)(intptr_t)req_id, cm,
                                             (edc_actor_data_free_fn)edc_channel_message_free);
    actor->handler(actor, msg); /* dispatches + frees msg */
}

/* ══════════════════════════════════════════════════════════════
 *  Test 1: 标签说 B, 任务就落 B —— 哪怕 scheduler 绑的是 A
 * ══════════════════════════════════════════════════════════════ */

static void test_input_lands_in_the_tagged_session(void)
{
    TEST("入站: 标签指向 B ⇒ 任务/组落在 B (scheduler 绑的是 A)");

    struct edc_config *cfg = make_config();
    g_sess_a = edc_session_new(cfg, "sess-a", "A");
    g_sess_b = edc_session_new(cfg, "sess-b", "B");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, g_sess_a);
    sched->resolve_session = stub_resolve_session;
    /* 没有 handle_command → 走 scheduler 自己的兜底建任务路径 (这正是要测的那条)。 */

    deliver_input(sched, "sess-b", "hello from B", 1);

    CHECK(count_groups(g_sess_a) == 0, "A 没有被建组");
    CHECK(count_tasks(g_sess_a) == 0, "A 里没有新任务");
    CHECK(count_groups(g_sess_b) == 1, "B 里建了 1 个组");
    CHECK(count_tasks(g_sess_b) == 1, "B 里有 1 个任务");

    char *tid = first_task_id(g_sess_b);
    CHECK(tid != NULL, "B 的任务存在");
    CHECK(g_sess_b->active_task != NULL, "B 的 active task 被设置");
    CHECK(g_sess_b->active_task && tid && strcmp(g_sess_b->active_task->task_id, tid) == 0,
          "active task 就是刚建的那个");
    CHECK(g_sess_b->foreground_group != NULL, "B 有了前台组");
    CHECK(g_sess_b->foreground_group && tid && strcmp(g_sess_b->foreground_group->group_id, tid) == 0,
          "前台组以它为首 (group_id == leader id)");
    CHECK(g_sess_a->active_task == NULL, "A 的 active task 没被动过");
    CHECK(g_sess_a->foreground_group == NULL, "A 的前台组没被动过");

    /* 任务自己的 session 字段也必须是 B (落盘时按它归位)。 */
    struct edc_task *t = g_sess_b->active_task;
    CHECK(t && t->session_id && strcmp(t->session_id, "sess-b") == 0, "任务的 session_id 是 sess-b");

    edc_scheduler_free(sched);
    edc_session_free(g_sess_a);
    edc_session_free(g_sess_b);
    edc_config_free(cfg);
    g_sess_a = g_sess_b = NULL;
}

/* ══════════════════════════════════════════════════════════════
 *  Test 2: 未知标签 ⇒ 拒绝 (不猜、不落到绑定 session)
 * ══════════════════════════════════════════════════════════════ */

static void test_unknown_tag_is_rejected(void)
{
    TEST("入站: 标签指向不存在的 session ⇒ 拒绝这条输入 (两边都不落)");

    struct edc_config *cfg = make_config();
    g_sess_a = edc_session_new(cfg, "sess-a", "A");
    g_sess_b = edc_session_new(cfg, "sess-b", "B");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, g_sess_a);
    sched->resolve_session = stub_resolve_session;

    deliver_input(sched, "sess-does-not-exist", "who am I", 2);

    CHECK(count_groups(g_sess_a) == 0, "A 没有被建组 (没有落到绑定 session)");
    CHECK(count_tasks(g_sess_a) == 0, "A 里没有新任务");
    CHECK(count_groups(g_sess_b) == 0, "B 也没有");
    CHECK(g_sess_a->active_task == NULL, "A 的 active task 没被设");

    edc_scheduler_free(sched);
    edc_session_free(g_sess_a);
    edc_session_free(g_sess_b);
    edc_config_free(cfg);
    g_sess_a = g_sess_b = NULL;
}

/* ══════════════════════════════════════════════════════════════
 *  Test 3: 没有标签 (crontab / 老渠道) ⇒ 读口给默认值
 * ══════════════════════════════════════════════════════════════ */

static void test_tagless_input_uses_the_default(void)
{
    TEST("入站: 没有标签 ⇒ 由读口给默认 session (crontab 行为不变)");

    struct edc_config *cfg = make_config();
    g_sess_a = edc_session_new(cfg, "sess-a", "A");
    g_sess_b = edc_session_new(cfg, "sess-b", "B");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, g_sess_a);
    sched->resolve_session = stub_resolve_session;

    deliver_input(sched, NULL, "reminder: drink water", 3);

    CHECK(count_tasks(g_sess_a) == 1, "无标签的输入落在默认 (A) session");
    CHECK(count_tasks(g_sess_b) == 0, "B 不受影响");

    edc_scheduler_free(sched);
    edc_session_free(g_sess_a);
    edc_session_free(g_sess_b);
    edc_config_free(cfg);
    g_sess_a = g_sess_b = NULL;
}

/* ══════════════════════════════════════════════════════════════ */

int main(void)
{
    log_info("=== test_session_routing (P3 ①a 防线) ===");

    test_input_lands_in_the_tagged_session();
    test_unknown_tag_is_rejected();
    test_tagless_input_uses_the_default();

    log_info("=== %d passed, %d failed ===", test_passed, test_failed);
    printf("%d passed, %d failed\n", test_passed, test_failed);
    return test_failed == 0 ? 0 : 1;
}
