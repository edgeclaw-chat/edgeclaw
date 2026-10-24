/* ══════════════════════════════════════════════════════════════
 *  P3 防线: 快照按 session 取, 换 session 后取到的是**新的那棵**
 *
 *  P3 (session 去绑定) 有四条防线, 这是最后一条:
 *  "快照按 `session_id` 取, 换 session 后取到的是新的那棵"。
 *
 *  P3 拆掉了 `sched->session` 这份"当前 session"的副本 (§4.1), 而快照正是
 *  它**最重的读者** —— 以前 `sched_handle_task_snapshot` 直接拿
 *  `sched->session->groups` 建整棵树。拆的过程里最容易被改坏的是三种情形:
 *
 *    ① 快照不跟着走 (换了 session 还建旧的那棵);
 *    ② 快照被缓存住 (第一棵建完就复用);
 *    ③ 换了 session 后两棵树混在一起 (过滤写成了"或")。
 *
 *  所以这里**只钉可观测结果** —— 快照 JSON 的 `session_id` 与它包含的组 ——
 *  不钉实现。P3 把"绑定字段"换成"请求里显式带 session"时, 断言必须原样通过;
 *  唯一允许改的是 `request_snapshot()` 这个 helper (见它的注释)。
 * ══════════════════════════════════════════════════════════════ */

#include "edc_actor.h"
#include "test_ws.h"
#include "edc_compat.h"
#include "edc_config.h"
#include "edc_scheduler.h"
#include "edc_session.h"
#include "edc_session_manager.h"
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

/* ── 单测的 session 仓库 (两个) ────────────────────────────────
 * 同 install_resolver, 只是装两个: 标签为空 = 活跃的那个 (第一个), 标签给定则按
 * id/name 找, 找不到返回 NULL (调用方拒绝这条输入)。NULL 的槽位不放进数组 ——
 * 真 store 的 find 会解引用每个槽。 */
static void install_store(struct edc_scheduler *sched, struct edc_session *first, struct edc_session *second)
{
    struct edc_session_manager *sm = g_new0(struct edc_session_manager, 1);
    sm->sessions = g_new0(struct edc_session *, 2);
    int n = 0;
    if (first)
        sm->sessions[n++] = first;
    if (second)
        sm->sessions[n++] = second;
    sm->session_count = n;
    sm->active_index = first ? 0 : -1; /* 第一个没给 → 空标签也解析不出来 */
    sched->sessions = sm;              /* 测试进程内不释放 */
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
    cfg->workspace = sdsnew(edc_test_workspace());
    return cfg;
}

/* 单测里没有跑 loop (edc_actor_send 需要 actor->loop), 但 handler 本身就是要测的
 * 那条路径 —— 快照就挂在它的尾巴上。 */
static void pump(struct edc_scheduler *sched, edc_actor_msg_type_t type)
{
    edc_actor_msg_t *msg = edc_actor_msg_new(type, NULL, NULL, NULL, NULL);
    sched->self->handler(sched->self, msg);
}

static sds g_snapshot = NULL;

static void capture_snapshot(struct edc_scheduler *sched, sds json)
{
    (void)sched;
    if (g_snapshot)
        sdsfree(g_snapshot);
    g_snapshot = json; /* 接管所有权; 下一次 capture 或收尾时释放 */
}

/* ── 这是 P3 改过的**唯一**部分（当初就是按这个预期写的）────────
 * 请求里直接带 session（P3 ①b: 拉取点名要哪棵树, 不再有"绑定"这一步）。 */
static void request_snapshot(struct edc_scheduler *sched, struct edc_session *session)
{
    edc_actor_msg_t *msg = edc_actor_msg_new(BC_ACTOR_MSG_TASK_SNAPSHOT, NULL, NULL,
                                             sdsnew(session ? edc_id_str(session->session_id) : ""),
                                             (edc_actor_data_free_fn)sdsfree);
    sched->self->handler(sched->self, msg);
}

/* ── 从快照 JSON 里读可观测结果 (断言的唯一入口) ── */
static cJSON *parse_snapshot(void)
{
    return g_snapshot ? cJSON_Parse(g_snapshot) : NULL;
}

static const char *snapshot_session_id(cJSON *root)
{
    cJSON *s = root ? cJSON_GetObjectItem(root, "session_id") : NULL;
    return (s && cJSON_IsString(s)) ? cJSON_GetStringValue(s) : NULL;
}

static int snapshot_group_count(cJSON *root)
{
    cJSON *groups = root ? cJSON_GetObjectItem(root, "groups") : NULL;
    return cJSON_IsArray(groups) ? cJSON_GetArraySize(groups) : -1;
}

static bool snapshot_has_group(cJSON *root, const char *group_id)
{
    cJSON *groups = root ? cJSON_GetObjectItem(root, "groups") : NULL;
    if (!cJSON_IsArray(groups))
        return false;
    cJSON *g;
    cJSON_ArrayForEach(g, groups)
    {
        cJSON *id = cJSON_GetObjectItem(g, "id");
        if (cJSON_IsString(id) && strcmp(cJSON_GetStringValue(id), group_id) == 0)
            return true;
    }
    return false;
}

/* 给 session 装一个组 + 一个成员任务 (id 与组 id 都显式给, 断言里要认它们)。 */
static struct edc_task *add_group_with_task(struct edc_session *session, const char *task_id, const char *desc)
{
    struct edc_task *task = edc_task_new(edc_test_id(task_id), session->session_id, edc_test_id(task_id), desc, edc_channel_adapter);
    struct edc_task_group *group = edc_task_group_new(task);
    edc_session_add_group(session, group);
    return task;
}

/* ══════════════════════════════════════════════════════════════
 *  Test 1: 快照跟着 session 走, 且只装那一个 session 的树
 * ══════════════════════════════════════════════════════════════ */

static void test_snapshot_follows_the_session(void)
{
    TEST("快照: 换 session 后取到的是新的那棵 (不是旧的、不是两棵混在一起)");

    struct edc_config *cfg = make_config();
    struct edc_session *sa = edc_session_new(cfg, edc_test_session_id("sess-a"), "A");
    struct edc_session *sb = edc_session_new(cfg, edc_test_session_id("sess-b"), "B");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    sched->snapshot_reply = capture_snapshot;
    install_store(sched, sa, sb);

    /* A 里一棵树 (组 100), B 里另一棵 (组 200) —— 两边 id 不重叠, 混在一起能立刻看出来。 */
    add_group_with_task(sa, "100", "work in A");
    add_group_with_task(sb, "200", "work in B");

    /* ① 要 A 的树 → 拿到的必须是 A 的那棵。 */
    request_snapshot(sched, sa);
    cJSON *root = parse_snapshot();
    CHECK(root != NULL, "A: 快照可解析");
    CHECK(snapshot_session_id(root) &&
              strcmp(snapshot_session_id(root), edc_test_session_id_str("sess-a")) == 0,
          "A: session_id 是 A 的 id");
    CHECK(snapshot_group_count(root) == 1, "A: 只有 1 个组");
    CHECK(snapshot_has_group(root, "100"), "A: 装的是组 100");
    CHECK(!snapshot_has_group(root, "200"), "A: 不含 B 的组 200");
    cJSON_Delete(root);

    /* ② 要 B 的树 → 必须是 B 的那棵 (情形①: 不还旧的; 情形③: 不混)。 */
    request_snapshot(sched, sb);
    root = parse_snapshot();
    CHECK(root != NULL, "B: 快照可解析");
    CHECK(snapshot_session_id(root) &&
              strcmp(snapshot_session_id(root), edc_test_session_id_str("sess-b")) == 0,
          "B: session_id 是 B 的 id");
    CHECK(snapshot_group_count(root) == 1, "B: 只有 1 个组");
    CHECK(snapshot_has_group(root, "200"), "B: 装的是组 200");
    CHECK(!snapshot_has_group(root, "100"), "B: 不含 A 的组 100 (没混在一起)");
    cJSON_Delete(root);

    /* ③ 再要 A 的树 → 又是 A 的那棵 (情形②: 没被缓存成第一棵或最后一棵)。 */
    request_snapshot(sched, sa);
    root = parse_snapshot();
    CHECK(root != NULL, "回到 A: 快照可解析");
    CHECK(snapshot_session_id(root) && strcmp(snapshot_session_id(root), edc_test_session_id_str("sess-a")) == 0,
          "回到 A: session_id 是 A 的 id");
    CHECK(snapshot_has_group(root, "100") && !snapshot_has_group(root, "200"), "回到 A: 又只有 A 的树");
    cJSON_Delete(root);

    edc_session_free(sa);
    edc_session_free(sb);
    edc_scheduler_free(sched);
    edc_config_free(cfg);
}

/* ══════════════════════════════════════════════════════════════
 *  Test 2: 空 session 也要给出**它自己**那一棵 (空树, 不是别人的树)
 * ══════════════════════════════════════════════════════════════ */

static void test_snapshot_of_an_empty_session(void)
{
    TEST("快照: 空 session 给空树 (不崩、不退回别人的树)");

    struct edc_config *cfg = make_config();
    struct edc_session *full = edc_session_new(cfg, edc_test_session_id("sess-full"), "Full");
    struct edc_session *empty = edc_session_new(cfg, edc_test_session_id("sess-empty"), "Empty");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    sched->snapshot_reply = capture_snapshot;
    install_store(sched, full, empty);
    add_group_with_task(full, "300", "work in full");

    request_snapshot(sched, empty);
    cJSON *root = parse_snapshot();
    CHECK(root != NULL, "空 session: 快照可解析");
    CHECK(snapshot_session_id(root) && strcmp(snapshot_session_id(root), edc_test_session_id_str("sess-empty")) == 0,
          "空 session: session_id 是 sess-empty");
    CHECK(snapshot_group_count(root) == 0, "空 session: groups 是空数组");
    CHECK(!snapshot_has_group(root, "300"), "空 session: 没退回 full 的树");
    cJSON_Delete(root);

    edc_session_free(full);
    edc_session_free(empty);
    edc_scheduler_free(sched);
    edc_config_free(cfg);
}

/* ══════════════════════════════════════════════════════════════
 *  Test 3: 组的用量汇总 (侧栏那行) —— 成员相加, ctx 取规模最大的那个成员的对话
 * ══════════════════════════════════════════════════════════════ */

/* 读某个组的某个数字字段 (缺失 -> -1, 好让断言在"没这个键"时也失败)。 */
static long long group_number(cJSON *root, const char *group_id, const char *key)
{
    cJSON *groups = root ? cJSON_GetObjectItem(root, "groups") : NULL;
    if (!cJSON_IsArray(groups))
        return -1;
    cJSON *g;
    cJSON_ArrayForEach(g, groups)
    {
        cJSON *id = cJSON_GetObjectItem(g, "id");
        if (cJSON_IsString(id) && strcmp(cJSON_GetStringValue(id), group_id) == 0)
        {
            cJSON *v = cJSON_GetObjectItem(g, key);
            return cJSON_IsNumber(v) ? (long long)v->valuedouble : -1;
        }
    }
    return -1;
}

/* 读某个组的某个布尔字段 (缺失 -> -1)。 */
static int group_flag(cJSON *root, const char *group_id, const char *key)
{
    cJSON *groups = root ? cJSON_GetObjectItem(root, "groups") : NULL;
    if (!cJSON_IsArray(groups))
        return -1;
    cJSON *g;
    cJSON_ArrayForEach(g, groups)
    {
        cJSON *id = cJSON_GetObjectItem(g, "id");
        if (cJSON_IsString(id) && strcmp(cJSON_GetStringValue(id), group_id) == 0)
        {
            cJSON *v = cJSON_GetObjectItem(g, key);
            return cJSON_IsBool(v) ? (cJSON_IsTrue(v) ? 1 : 0) : -1;
        }
    }
    return -1;
}

static void test_group_usage_rollup(void)
{
    TEST("快照: 组的用量 = 成员相加; ctx = 规模最大的那个成员的对话 (不是相加)");

    struct edc_config *cfg = make_config();
    struct edc_session *s = edc_session_new(cfg, edc_test_session_id("sess-usage"), "Usage");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    sched->snapshot_reply = capture_snapshot;
    install_store(sched, s, NULL);

    struct edc_task *t1 = edc_task_new(edc_test_id("4100"), s->session_id, edc_test_id("4100"), "leader", edc_channel_adapter);
    struct edc_task_group *group = edc_task_group_new(t1);
    edc_session_add_group(s, group);
    struct edc_task *t2 = edc_task_new(edc_test_id("4101"), s->session_id, edc_test_id("4100"), "member", edc_channel_adapter);
    edc_task_group_add_task(group, t2);

    /* 真实顺序: 组里已经有 system prompt (history[0]) 才发得出请求 —— 锚的位置
     * 信息 (last_hist_len) 就是在那一刻记下的。没有它锚不可用, 组 ctx 会退回
     * 从零估一遍 (那是另一条路径, 由 test_group_ctx 覆盖)。 */
    {
        cJSON *sys = cJSON_CreateObject();
        cJSON_AddStringToObject(sys, "role", "system");
        cJSON_AddStringToObject(sys, "content", "sys");
        edc_task_group_history_add_owned(group, sys);
    }

    /* leader 跑两轮 (工具循环): 计数器相加, 对话规模是**最近一轮**的 total。 */
    edc_task_add_usage(t1, 1000, 200, 1200, 750);
    edc_task_add_usage(t1, 3000, 100, 3100, 2000);
    /* member 那一轮的对话更小: 组 ctx 仍应是 leader 那次更大的。 */
    edc_task_add_usage(t2, 500, 50, 550, 0);

    request_snapshot(sched, s);
    cJSON *root = parse_snapshot();
    CHECK(root != NULL, "快照可解析");
    CHECK(group_number(root, "4100", "tokens_prompt") == 4500, "输入 = 两个成员相加");
    CHECK(group_number(root, "4100", "tokens_completion") == 350, "输出 = 两个成员相加");
    CHECK(group_number(root, "4100", "tokens_total") == 4850, "总数 = 两个成员相加");
    CHECK(group_number(root, "4100", "tokens_cached") == 2750, "缓存命中的那部分 = 两个成员相加");
    CHECK(group_number(root, "4100", "ctx_tokens") == 3100, "当前上下文 = 规模最大的成员 (最近一轮的输入+输出)");
    CHECK(group_flag(root, "4100", "ctx_estimated") == 0, "锚之后没追加东西 => 是 provider 真值, 不打估算标记");
    cJSON_Delete(root);

    /* 锚之后又追加了一条消息 (别的成员提交的): 估算只落在这一小段上, 且整行转为
     * "含估算" —— 估算不得冒充 provider 的测量。 */
    {
        cJSON *m = cJSON_CreateObject();
        char body[41];
        memset(body, 'a', 40);
        body[40] = '\0';
        cJSON_AddStringToObject(m, "role", "user");
        cJSON_AddStringToObject(m, "content", body);
        edc_task_group_history_add_owned(group, m);
    }
    request_snapshot(sched, s);
    cJSON *root2 = parse_snapshot();
    CHECK(root2 != NULL, "追加之后的快照可解析");
    CHECK(group_number(root2, "4100", "ctx_tokens") == 3100 + 19, "只把锚之后的增量按估算加上去");
    CHECK(group_flag(root2, "4100", "ctx_estimated") == 1, "含估算 => 打标记");
    cJSON_Delete(root2);

    edc_session_free(s);
    edc_scheduler_free(sched);
    edc_config_free(cfg);
}

/* ══════════════════════════════════════════════════════════════ */

int main(void)
{
    edc_test_workspace(); /* open the disposable store before anything writes */
    log_info("=== test_session_snapshot (P3 防线) ===");

    test_snapshot_follows_the_session();
    test_snapshot_of_an_empty_session();
    test_group_usage_rollup();

    if (g_snapshot)
        sdsfree(g_snapshot);

    log_info("=== %d passed, %d failed ===", test_passed, test_failed);
    printf("%d passed, %d failed\n", test_passed, test_failed);
    return test_failed == 0 ? 0 : 1;
}
