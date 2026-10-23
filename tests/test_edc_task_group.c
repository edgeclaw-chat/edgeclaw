#include "edc_task_group.h"
#include "test_ws.h"
#include "log.h"
#include <stdio.h>
#include <string.h>

static int test_failed = 0;
static int test_passed = 0;

#define TEST(name) do { log_info("  TEST: %s", name); } while(0)
#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        log_error("    FAIL: %s", msg); \
        test_failed++; \
    } else { \
        test_passed++; \
    } \
} while(0)

/* ── 1. Group 生命周期 ─────────────────────────────────────── */

static void test_group_lifecycle(void)
{
    TEST("group_new creates group with leader");
    struct edc_task *leader = edc_task_new("task-L", "sess-1", "task-L", "leader", edc_channel_adapter);
    struct edc_task_group *g = edc_task_group_new(leader);

    CHECK(g != NULL, "group_new returns non-NULL");
    CHECK(strcmp(g->group_id, "task-L") == 0, "group_id = leader's task_id");
    CHECK(g->leader == leader, "leader set correctly");
    CHECK(g->member_count == 1, "member_count = 1");
    CHECK(g->is_orphaned == false, "not orphaned");
    CHECK(leader->is_group_leader == 1, "leader marked as is_group_leader");

    edc_task_group_free(g);

    TEST("group_new returns NULL for NULL leader");
    g = edc_task_group_new(NULL);
    CHECK(g == NULL, "NULL leader returns NULL");
}

/* ── 2. 成员管理 ───────────────────────────────────────────── */

static void test_member_management(void)
{
    TEST("add_task adds member to group");
    struct edc_task *leader = edc_task_new("task-A", "sess-2", "task-A", "leader A", edc_channel_adapter);
    struct edc_task_group *g = edc_task_group_new(leader);

    struct edc_task *member = edc_task_new("task-B", "sess-2", "task-old", "member B", edc_channel_adapter);
    edc_task_group_add_task(g, member);

    CHECK(g->member_count == 2, "member_count = 2 after add");
    CHECK(strcmp(member->group_id, "task-A") == 0, "member group_id updated to leader's task_id");
    CHECK(member->is_group_leader == 0, "member is not leader");

    TEST("find_task finds member by task_id");
    CHECK(edc_task_group_find_task(g, "task-A") == leader, "find task-A returns leader");
    CHECK(edc_task_group_find_task(g, "task-B") == member, "find task-B returns member");
    CHECK(edc_task_group_find_task(g, "nonexistent") == NULL, "find nonexistent returns NULL");

    TEST("remove_task removes member");
    edc_task_group_remove_task(g, member);
    CHECK(g->member_count == 1, "member_count = 1 after remove");
    CHECK(edc_task_group_find_task(g, "task-B") == NULL, "removed member not found");
    edc_task_free(member);

    TEST("remove_task with leader → orphaned");
    struct edc_task *m2 = edc_task_new("task-C", "sess-2", "task-A", "member C", edc_channel_adapter);
    edc_task_group_add_task(g, m2);
    CHECK(g->member_count == 2, "member_count = 2");

    edc_task_group_remove_task(g, leader);
    CHECK(g->member_count == 1, "member_count = 1 after leader removed");
    CHECK(g->is_orphaned == true, "group is orphaned after leader removed");
    CHECK(g->leader == NULL, "leader is NULL after orphan");
    CHECK(leader->is_group_leader == 0, "old leader no longer marked as leader");
    edc_task_free(leader);

    edc_task_group_free(g);
}

/* ── 3. Leader 管理 ────────────────────────────────────────── */

static void test_leader_management(void)
{
    TEST("set_leader changes group_id and updates all members");
    struct edc_task *old_l = edc_task_new("task-L1", "sess-3", "task-L1", "old leader", edc_channel_adapter);
    struct edc_task_group *g = edc_task_group_new(old_l);

    struct edc_task *m = edc_task_new("task-M", "sess-3", "task-L1", "member", edc_channel_adapter);
    edc_task_group_add_task(g, m);

    struct edc_task *new_l = edc_task_new("task-L2", "sess-3", "task-L1", "new leader", edc_channel_adapter);
    /* Add to group first so it can be set as leader */
    edc_task_group_add_task(g, new_l);

    edc_task_group_set_leader(g, new_l);

    CHECK(g->leader == new_l, "leader changed");
    CHECK(new_l->is_group_leader == 1, "new leader marked");
    CHECK(old_l->is_group_leader == 0, "old leader unmarked");
    CHECK(strcmp(g->group_id, "task-L2") == 0, "group_id updated to new leader's task_id");
    CHECK(strcmp(m->group_id, "task-L2") == 0, "member group_id updated");
    CHECK(strcmp(new_l->group_id, "task-L2") == 0, "new leader group_id updated");
    CHECK(strcmp(old_l->group_id, "task-L2") == 0, "old leader group_id updated");

    edc_task_group_free(g);
}

/* 4. Signals: per-member signalling lives in the scheduler (it owes each member its
 * bookkeeping, and the group does not know the host), so this only tests the group's
 * own teardown. */

static void test_group_signals(void)
{
    TEST("finish_signal orphans the group only when the leader was cancelled");
    struct edc_task *leader = edc_task_new("task-GL", "sess-4", "task-GL", "grp leader", edc_channel_adapter);
    struct edc_task_group *g = edc_task_group_new(leader);

    struct edc_task *m1 = edc_task_new("task-GM1", "sess-4", "task-GL", "grp member 1", edc_channel_adapter);
    edc_task_await_exec(m1);
    edc_task_group_add_task(g, m1);

    /* 宿主还没发信号: 收尾什么都不做 */
    edc_task_group_finish_signal(g);
    CHECK(!g->is_orphaned, "group intact while the leader is alive");
    CHECK(edc_task_group_find_task(g, "task-GL") == leader, "leader still in the group");

    /* 宿主侧的扇出 = 逐成员发信号 (sched_signal_group_members 的循环)。 */
    edc_task_send_signal(leader, TASK_SIGTERM);
    edc_task_send_signal(m1, TASK_SIGTERM);
    CHECK(edc_task_is_cancelled(leader), "leader cancelled");
    CHECK(edc_task_is_cancelled(m1), "member cancelled");

    edc_task_group_finish_signal(g);
    CHECK(g->is_orphaned == true, "group orphaned after leader cancelled");

    TEST("finish_signal is safe on NULL and on an already orphaned group");
    edc_task_group_finish_signal(NULL);
    edc_task_group_finish_signal(g); /* leader 已经是 NULL 了 */
    CHECK(g->is_orphaned, "stays orphaned");
    CHECK(1, "NULL / repeated finish_signal survived");

    edc_task_group_free(g);
}

/* ── 5. 状态查询 ───────────────────────────────────────────── */

static void test_status_queries(void)
{
    TEST("is_foreground_capable");
    struct edc_task *leader = edc_task_new("task-QL", "sess-6", "task-QL", "query leader", edc_channel_adapter);
    struct edc_task_group *g = edc_task_group_new(leader);
    CHECK(edc_task_group_is_foreground_capable(g), "group with members is capable");

    edc_task_group_remove_task(g, leader);
    edc_task_free(leader);
    CHECK(!edc_task_group_is_foreground_capable(g), "empty group is not capable");
    /* Group is dead now (0 members), free it */
    edc_task_group_free(g);

    TEST("status_label");
    struct edc_task *l2 = edc_task_new("task-QL2", "sess-6", "task-QL2", "query leader 2", edc_channel_adapter);
    g = edc_task_group_new(l2);
    CHECK(strcmp(edc_task_group_status_label(g), "ACTIVE") == 0, "active group → ACTIVE");

    struct edc_task *m = edc_task_new("task-QM", "sess-6", "task-QL2", "query member", edc_channel_adapter);
    edc_task_group_add_task(g, m);
    edc_task_group_remove_task(g, l2);
    edc_task_free(l2);
    CHECK(strcmp(edc_task_group_status_label(g), "ORPHANED") == 0, "orphaned group → ORPHANED");

    edc_task_group_free(g);
}

/* ── 6. 综合场景 ───────────────────────────────────────────── */

static void test_scenario_orphan(void)
{
    TEST("scenario: leader exits → orphan → remove remaining → dead");
    struct edc_task *leader = edc_task_new("task-OL", "sess-7", "task-OL", "orphan L", edc_channel_adapter);
    struct edc_task_group *g = edc_task_group_new(leader);

    struct edc_task *m1 = edc_task_new("task-OM1", "sess-7", "task-OL", "orphan M1", edc_channel_adapter);
    struct edc_task *m2 = edc_task_new("task-OM2", "sess-7", "task-OL", "orphan M2", edc_channel_adapter);
    edc_task_group_add_task(g, m1);
    edc_task_group_add_task(g, m2);
    CHECK(g->member_count == 3, "3 members initially");

    /* Remove leader → orphaned */
    edc_task_group_remove_task(g, leader);
    CHECK(g->is_orphaned, "orphaned after leader removal");
    CHECK(g->member_count == 2, "2 members remaining");
    CHECK(strcmp(edc_task_group_status_label(g), "ORPHANED") == 0, "status = ORPHANED");
    edc_task_free(leader);

    /* Remove remaining members one by one */
    edc_task_group_remove_task(g, m1);
    CHECK(g->member_count == 1, "1 member");
    CHECK(g->is_orphaned, "still orphaned");
    edc_task_free(m1);

    edc_task_group_remove_task(g, m2);
    CHECK(g->member_count == 0, "0 members (dead)");
    edc_task_free(m2);

    edc_task_group_free(g);
}

/* ── 7. 组历史归属 ─────────────────────────────────────────── */

static cJSON *mk_msg(const char *role, const char *content)
{
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "role", role);
    cJSON_AddStringToObject(m, "content", content);
    return m;
}

static void test_history_append(void)
{
    TEST("history append (_owned / _ref)");
    struct edc_task *leader = edc_task_new("task-HL", "sess-8", "task-HL", "hist leader", edc_channel_adapter);
    struct edc_task_group *g = edc_task_group_new(leader);

    /* _owned: 接管所有权 (新构造的消息) */
    edc_task_group_history_add_owned(g, mk_msg("system", "group prompt"));
    edc_task_group_history_add_owned(g, mk_msg("user", "hi"));

    /* _ref: 只存引用 (消息归 turn 所有) */
    cJSON *borrowed = mk_msg("assistant", "hello");
    edc_task_group_history_add_ref(g, borrowed);

    cJSON *borrowed2 = mk_msg("user", "second");
    edc_task_group_history_add_ref(g, borrowed2);

    CHECK(cJSON_GetArraySize(g->history) == 4, "history has 4 entries");

    TEST("history survives growth past initial capacity");
    for (int i = 0; i < 40; i++)
        edc_task_group_history_add_owned(g, mk_msg("user", "x"));
    CHECK(cJSON_GetArraySize(g->history) == 44, "history grew to 44 entries");

    /* borrowed 消息不被 group 释放, 单独回收 */
    edc_task_group_free(g);
    cJSON_Delete(borrowed);
    cJSON_Delete(borrowed2);
}

/* ── main ──────────────────────────────────────────────────── */

int main(void)
{
    edc_test_workspace(); /* open the disposable store before anything writes */
    log_info("=== Phase 2: edc_task_group 模块测试 ===");
    log_info("");

    test_group_lifecycle();
    test_member_management();
    test_leader_management();
    test_group_signals();
    test_status_queries();
    test_scenario_orphan();
    test_history_append();

    log_info("");
    log_info("=== 测试结果: %d passed, %d failed ===", test_passed, test_failed);
    return test_failed > 0 ? 1 : 0;
}
