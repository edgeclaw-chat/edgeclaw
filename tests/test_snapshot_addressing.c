/* ══════════════════════════════════════════════════════════════
 *  P3 ①b 防线: 把"变了"与"是什么"分开
 *
 *  老做法: 任何变化都把 **scheduler 当前绑定 session** 的整棵树序列化, 推进
 *  channel 给前端。那次推送同时回答了两个问题("变了吗" / "是什么"), 于是它
 *  顺带决定了前端**显示哪棵树** —— 前端收到的快照里带着 session_id, 不匹配就
 *  把用户的对话记录换掉 (`SyncChatSession` → `LoadChatTranscript`)。
 *
 *  新做法(本文件钉住的):
 *    - **变了**: `snapshot_changed(session_id, reason)` —— 很小, 只说"session X
 *      的树变了", **事件的属主**是那个变了的 session (不是绑定的那个);
 *    - **是什么**: 前端 `BC_ACTOR_MSG_TASK_SNAPSHOT` **点名**要哪棵树 (data =
 *      session_id), kernel 用 store 读口解析后序列化应答。
 *
 *  四条断言(反面优先):
 *    ① 事件带的是**属主** session —— 哪怕 scheduler 绑的是别人;
 *    ② 没有请求就**没有树** (状态变化只发事件, 一个字节的树都不上路);
 *    ③ 应答的 session_id == 请求点名的那个 (寻址, 不是"顺便给你"); 
 *    ④ 标签解析不出来 ⇒ 不回应答 (不给别人的树)。
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
/* ── P3 ③ 测试读口 (两个 session) ────────────────────────────────
 * scheduler 不再持有"当前 session": 拉取/入站请求带 session 标签, 由读口解析。
 * 本文件每条用例都有自己的两棵树, 所以槽位**每次覆盖** (不留上一轮的指针)。 */
static struct edc_session *g_store[2];

static struct edc_session *test_resolve_session(void *ctx, const char *session_id)
{
    (void)ctx;
    if (!session_id || !*session_id)
        return g_store[0] ? g_store[0] : NULL;
    for (int i = 0; i < 2; i++)
    {
        if (g_store[i] && g_store[i]->session_id && strcmp(g_store[i]->session_id, session_id) == 0)
            return g_store[i];
    }
    return NULL;
}

static void install_store(struct edc_scheduler *sched, struct edc_session *first, struct edc_session *second)
{
    g_store[0] = first;
    g_store[1] = second;
    sched->resolve_session = test_resolve_session;
}

/* Stub: _run_loop is referenced by edc_commands (pulled in transitively). */
volatile sig_atomic_t _run_loop = 1;

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
        {                                                                                                                  \
            log_error("    FAIL: %s", msg);                                                                            \
            test_failed++;                                                                                             \
        }                                                                                                              \
        else                                                                                                           \
        {                                                                                                              \
            test_passed++;                                                                                             \
        }                                                                                                              \
    } while (0)

/* ── 捕获两条线 ────────────────────────────────────────────── */

static int g_event_count = 0;
static int g_event_wrong_session = 0; /* 带错 session 的事件数 (期望恒为 0) */
static const char *g_event_expect_session = NULL;
static sds g_event_session = NULL;
static sds g_event_reason = NULL;

static void capture_event(struct edc_scheduler *sched, const char *session_id, const char *reason)
{
    (void)sched;
    g_event_count++;
    if (g_event_expect_session && (!session_id || strcmp(session_id, g_event_expect_session) != 0))
        g_event_wrong_session++;
    if (g_event_session)
        sdsfree(g_event_session);
    if (g_event_reason)
        sdsfree(g_event_reason);
    g_event_session = sdsnew(session_id ? session_id : "");
    g_event_reason = sdsnew(reason ? reason : "");
}

static int g_tree_count = 0;
static sds g_tree = NULL;

static void capture_tree(struct edc_scheduler *sched, sds json)
{
    (void)sched;
    g_tree_count++;
    if (g_tree)
        sdsfree(g_tree);
    g_tree = json; /* 接管所有权 */
}

static void reset_capture(void)
{
    g_event_count = 0;
    g_event_wrong_session = 0;
    g_tree_count = 0;
    if (g_event_session)
        sdsfree(g_event_session);
    if (g_event_reason)
        sdsfree(g_event_reason);
    g_event_session = g_event_reason = NULL;
    if (g_tree)
        sdsfree(g_tree);
    g_tree = NULL;
}

/* ── 夹具 ──────────────────────────────────────────────────── */

static struct edc_session *g_sess_a = NULL;
static struct edc_session *g_sess_b = NULL;


/* 处理一条无害消息: 让 handler 尾巴上的"结算"跑起来 (脏表 → 事件)。
 * CHANNEL_RECV 且 data=NULL 时 sched_handle_input 立刻返回, 零副作用。 */
static void pump_settle(struct edc_scheduler *sched)
{
    edc_actor_t *actor = edc_scheduler_get_actor(sched);
    edc_actor_msg_t *msg = edc_actor_msg_new(BC_ACTOR_MSG_CHANNEL_RECV, NULL, NULL, NULL, NULL);
    actor->handler(actor, msg);
}

/* 前端点名要一棵树。 */
static void pull(struct edc_scheduler *sched, const char *session_id)
{
    edc_actor_t *actor = edc_scheduler_get_actor(sched);
    edc_actor_msg_t *msg = edc_actor_msg_new(BC_ACTOR_MSG_TASK_SNAPSHOT, NULL, NULL, sdsnew(session_id ? session_id : ""),
                                             (edc_actor_data_free_fn)sdsfree);
    actor->handler(actor, msg);
}

static struct edc_task *add_group_with_task(struct edc_session *session, const char *task_id, const char *desc)
{
    struct edc_task *task = edc_task_new(task_id, session->session_id, task_id, desc, edc_channel_adapter);
    struct edc_task_group *group = edc_task_group_new(task);
    edc_session_add_group(session, group);
    return task;
}

static bool tree_has_group(const char *json, const char *group_id)
{
    if (!json)
        return false;
    sds pat = sdscatprintf(sdsempty(), "\"id\":\"%s\"", group_id);
    bool found = strstr(json, pat) != NULL;
    sdsfree(pat);
    return found;
}

static bool tree_is_for(const char *json, const char *session_id)
{
    if (!json)
        return false;
    sds pat = sdscatprintf(sdsempty(), "\"session_id\":\"%s\"", session_id);
    bool found = strstr(json, pat) != NULL;
    sdsfree(pat);
    return found;
}

/* ══════════════════════════════════════════════════════════════
 *  Test 1: 事件带**属主** session; 状态变化**不**发出树
 * ══════════════════════════════════════════════════════════════ */

static void test_event_names_the_owner_and_carries_no_tree(void)
{
    TEST("事件: 带属主 session (不是绑定的那个); 且状态变化一个字节的树都不上路");

    struct edc_config *cfg = g_new0(struct edc_config, 1);
    cfg->workspace = sdsnew("/tmp/test_snapshot_addressing_ws");
    g_sess_a = edc_session_new(cfg, "sess-a", "A");
    g_sess_b = edc_session_new(cfg, "sess-b", "B");

    /* scheduler 绑 A —— 但下面动的是 B 里的任务。 */
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_store(sched, g_sess_a, g_sess_b);
    sched->snapshot_changed = capture_event;
    sched->snapshot_reply = capture_tree;

    add_group_with_task(g_sess_a, "100", "work in A");
    struct edc_task *tb = add_group_with_task(g_sess_b, "200", "work in B");

    reset_capture();
    /* 下面所有变化都发生在 B 里 ⇒ **每一个**事件的标签都必须是 sess-b。 */
    g_event_expect_session = "sess-b";

    /* 两条置脏路径都要覆盖 (它们各自有一处置脏点, 都要看属主而不是绑定):
     *   ① 信号 → 强制迁移 (force path);
     *   ② 一条入站输入 → 建任务 + 正常迁移 (sched_move_task)。 */
    edc_scheduler_send_signal_to_task_ptr(sched, tb, TASK_SIGTERM);
    pump_settle(sched);
    {
        edc_actor_t *actor = edc_scheduler_get_actor(sched);
        struct edc_channel_message *cm =
            edc_channel_message_new(edc_channel_adapter, EDC_CHANNEL_MESSAGE_USER_TYPE, sdsnew("hello in B"));
        cm->session_id = sdsnew("sess-b");
        edc_actor_msg_t *im = edc_actor_msg_new(BC_ACTOR_MSG_CHANNEL_RECV, NULL, NULL, cm,
                                                (edc_actor_data_free_fn)edc_channel_message_free);
        actor->handler(actor, im);
    }

    CHECK(g_event_count >= 1, "状态变化发了事件");
    CHECK(g_event_wrong_session == 0, "每个事件带的都是**属主** session (sess-b), 没有一个来自绑定的 sess-a");
    CHECK(g_tree_count == 0, "状态变化**没有**发出树 (没有请求就没有树)");

    /* 明确反向对照: 有人看 A 时要 A 的树, 而事件说的是 B —— 两件事互不冒充。 */
    pull(sched, "sess-a");
    CHECK(g_tree_count == 1, "点名要 A ⇒ 有一棵 (正好一棵)");
    CHECK(tree_is_for(g_tree, "sess-a"), "那棵是 A 的");
    CHECK(tree_has_group(g_tree, "100"), "装着 A 的组 100");
    CHECK(!tree_has_group(g_tree, "200"), "不含 B 的组 200");

    edc_scheduler_free(sched);
    edc_session_free(g_sess_a);
    edc_session_free(g_sess_b);
    edc_config_free(cfg);
    g_sess_a = g_sess_b = NULL;
    g_event_expect_session = NULL;
    reset_capture();
}

/* ══════════════════════════════════════════════════════════════
 *  Test 2: 寻址 —— 要哪棵给哪棵 (换回来还是那棵, 不串、不缓存)
 * ══════════════════════════════════════════════════════════════ */

static void test_pull_is_addressed(void)
{
    TEST("拉取: 要 B 给 B, 要 A 给 A (不串、不缓存)");

    struct edc_config *cfg = g_new0(struct edc_config, 1);
    cfg->workspace = sdsnew("/tmp/test_snapshot_addressing_ws");
    g_sess_a = edc_session_new(cfg, "sess-a", "A");
    g_sess_b = edc_session_new(cfg, "sess-b", "B");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_store(sched, g_sess_a, g_sess_b);
    sched->snapshot_changed = capture_event;
    sched->snapshot_reply = capture_tree;

    add_group_with_task(g_sess_a, "100", "work in A");
    add_group_with_task(g_sess_b, "200", "work in B");
    reset_capture();

    pull(sched, "sess-b");
    CHECK(g_tree_count == 1, "第一次: 一棵");
    CHECK(tree_is_for(g_tree, "sess-b") && tree_has_group(g_tree, "200"), "要 B 给 B");
    CHECK(!tree_has_group(g_tree, "100"), "B 的树里没有 A 的组");

    pull(sched, "sess-a");
    CHECK(g_tree_count == 2, "第二次: 两棵");
    CHECK(tree_is_for(g_tree, "sess-a") && tree_has_group(g_tree, "100"), "要 A 给 A");
    CHECK(!tree_has_group(g_tree, "200"), "A 的树里没有 B 的组");

    pull(sched, "sess-b");
    CHECK(tree_is_for(g_tree, "sess-b"), "再要 B 又是 B (没缓存成上一棵)");

    edc_scheduler_free(sched);
    edc_session_free(g_sess_a);
    edc_session_free(g_sess_b);
    edc_config_free(cfg);
    g_sess_a = g_sess_b = NULL;
    reset_capture();
}

/* ══════════════════════════════════════════════════════════════
 *  Test 3: 未知标签 ⇒ 不给别人的树
 * ══════════════════════════════════════════════════════════════ */

static void test_unknown_tag_yields_no_tree(void)
{
    TEST("拉取: 标签指向不存在的 session ⇒ 一个字节都不回 (不拿别人的树顶包)");

    struct edc_config *cfg = g_new0(struct edc_config, 1);
    cfg->workspace = sdsnew("/tmp/test_snapshot_addressing_ws");
    g_sess_a = edc_session_new(cfg, "sess-a", "A");
    g_sess_b = edc_session_new(cfg, "sess-b", "B");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_store(sched, g_sess_a, g_sess_b);
    sched->snapshot_reply = capture_tree;
    add_group_with_task(g_sess_a, "100", "work in A");
    reset_capture();

    pull(sched, "sess-does-not-exist");
    CHECK(g_tree_count == 0, "没有应答 (既不是 A 的树也不是 B 的)");

    pull(sched, "sess-a");
    CHECK(g_tree_count == 1 && tree_has_group(g_tree, "100"), "正常的请求照旧拿到自己的树");

    edc_scheduler_free(sched);
    edc_session_free(g_sess_a);
    edc_session_free(g_sess_b);
    edc_config_free(cfg);
    g_sess_a = g_sess_b = NULL;
    reset_capture();
}

/* ══════════════════════════════════════════════════════════════ */

int main(void)
{
    log_info("=== test_snapshot_addressing (P3 ①b 防线) ===");

    test_event_names_the_owner_and_carries_no_tree();
    test_pull_is_addressed();
    test_unknown_tag_yields_no_tree();

    log_info("=== %d passed, %d failed ===", test_passed, test_failed);
    printf("%d passed, %d failed\n", test_passed, test_failed);
    return test_failed == 0 ? 0 : 1;
}
