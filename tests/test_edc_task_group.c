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
    struct edc_task *leader = edc_task_new(edc_test_id("task-L"), edc_test_session_id("sess-1"), edc_test_id("task-L"), "leader", edc_channel_adapter);
    struct edc_task_group *g = edc_task_group_new(leader);

    CHECK(g != NULL, "group_new returns non-NULL");
    CHECK(g->group_id == edc_test_id("task-L"), "group_id = leader's task_id");
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
    struct edc_task *leader = edc_task_new(edc_test_id("task-A"), edc_test_session_id("sess-2"), edc_test_id("task-A"), "leader A", edc_channel_adapter);
    struct edc_task_group *g = edc_task_group_new(leader);

    struct edc_task *member = edc_task_new(edc_test_id("task-B"), edc_test_session_id("sess-2"), edc_test_id("task-old"), "member B", edc_channel_adapter);
    edc_task_group_add_task(g, member);

    CHECK(g->member_count == 2, "member_count = 2 after add");
    CHECK(member->group_id == edc_test_id("task-A"), "member group_id updated to leader's task_id");
    CHECK(member->is_group_leader == 0, "member is not leader");

    TEST("find_task finds member by task_id");
    CHECK(edc_task_group_find_task(g, edc_test_id("task-A")) == leader, "find task-A returns leader");
    CHECK(edc_task_group_find_task(g, edc_test_id("task-B")) == member, "find task-B returns member");
    CHECK(edc_task_group_find_task(g, edc_test_id("nonexistent")) == NULL, "find nonexistent returns NULL");

    TEST("remove_task removes member");
    edc_task_group_remove_task(g, member);
    CHECK(g->member_count == 1, "member_count = 1 after remove");
    CHECK(edc_task_group_find_task(g, edc_test_id("task-B")) == NULL, "removed member not found");
    edc_task_free(member);

    TEST("remove_task with leader → orphaned");
    struct edc_task *m2 = edc_task_new(edc_test_id("task-C"), edc_test_session_id("sess-2"), edc_test_id("task-A"), "member C", edc_channel_adapter);
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
    struct edc_task *old_l = edc_task_new(edc_test_id("task-L1"), edc_test_session_id("sess-3"), edc_test_id("task-L1"), "old leader", edc_channel_adapter);
    struct edc_task_group *g = edc_task_group_new(old_l);

    struct edc_task *m = edc_task_new(edc_test_id("task-M"), edc_test_session_id("sess-3"), edc_test_id("task-L1"), "member", edc_channel_adapter);
    edc_task_group_add_task(g, m);

    struct edc_task *new_l = edc_task_new(edc_test_id("task-L2"), edc_test_session_id("sess-3"), edc_test_id("task-L1"), "new leader", edc_channel_adapter);
    /* Add to group first so it can be set as leader */
    edc_task_group_add_task(g, new_l);

    edc_task_group_set_leader(g, new_l);

    CHECK(g->leader == new_l, "leader changed");
    CHECK(new_l->is_group_leader == 1, "new leader marked");
    CHECK(old_l->is_group_leader == 0, "old leader unmarked");
    CHECK(g->group_id == edc_test_id("task-L2"), "group_id updated to new leader's task_id");
    CHECK(m->group_id == edc_test_id("task-L2"), "member group_id updated");
    CHECK(new_l->group_id == edc_test_id("task-L2"), "new leader group_id updated");
    CHECK(old_l->group_id == edc_test_id("task-L2"), "old leader group_id updated");

    edc_task_group_free(g);
}

/* 4. Signals: per-member signalling lives in the scheduler (it owes each member its
 * bookkeeping, and the group does not know the host), so this only tests the group's
 * own teardown. */

static void test_group_signals(void)
{
    TEST("finish_signal orphans the group only when the leader was cancelled");
    struct edc_task *leader = edc_task_new(edc_test_id("task-GL"), edc_test_session_id("sess-4"), edc_test_id("task-GL"), "grp leader", edc_channel_adapter);
    struct edc_task_group *g = edc_task_group_new(leader);

    struct edc_task *m1 = edc_task_new(edc_test_id("task-GM1"), edc_test_session_id("sess-4"), edc_test_id("task-GL"), "grp member 1", edc_channel_adapter);
    edc_task_await_exec(m1);
    edc_task_group_add_task(g, m1);

    /* 宿主还没发信号: 收尾什么都不做 */
    edc_task_group_finish_signal(g);
    CHECK(!g->is_orphaned, "group intact while the leader is alive");
    CHECK(edc_task_group_find_task(g, edc_test_id("task-GL")) == leader, "leader still in the group");

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
    struct edc_task *leader = edc_task_new(edc_test_id("task-QL"), edc_test_session_id("sess-6"), edc_test_id("task-QL"), "query leader", edc_channel_adapter);
    struct edc_task_group *g = edc_task_group_new(leader);
    CHECK(edc_task_group_is_foreground_capable(g), "group with members is capable");

    edc_task_group_remove_task(g, leader);
    edc_task_free(leader);
    CHECK(!edc_task_group_is_foreground_capable(g), "empty group is not capable");
    /* Group is dead now (0 members), free it */
    edc_task_group_free(g);

    TEST("status_label");
    struct edc_task *l2 = edc_task_new(edc_test_id("task-QL2"), edc_test_session_id("sess-6"), edc_test_id("task-QL2"), "query leader 2", edc_channel_adapter);
    g = edc_task_group_new(l2);
    CHECK(strcmp(edc_task_group_status_label(g), "ACTIVE") == 0, "active group → ACTIVE");

    struct edc_task *m = edc_task_new(edc_test_id("task-QM"), edc_test_session_id("sess-6"), edc_test_id("task-QL2"), "query member", edc_channel_adapter);
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
    struct edc_task *leader = edc_task_new(edc_test_id("task-OL"), edc_test_session_id("sess-7"), edc_test_id("task-OL"), "orphan L", edc_channel_adapter);
    struct edc_task_group *g = edc_task_group_new(leader);

    struct edc_task *m1 = edc_task_new(edc_test_id("task-OM1"), edc_test_session_id("sess-7"), edc_test_id("task-OL"), "orphan M1", edc_channel_adapter);
    struct edc_task *m2 = edc_task_new(edc_test_id("task-OM2"), edc_test_session_id("sess-7"), edc_test_id("task-OL"), "orphan M2", edc_channel_adapter);
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
    TEST("history append (owned messages, no borrowed references)");
    struct edc_task *leader = edc_task_new(edc_test_id("task-HL"), edc_test_session_id("sess-8"), edc_test_id("task-HL"), "hist leader", edc_channel_adapter);
    struct edc_task_group *g = edc_task_group_new(leader);

    /* The history holds either a message ID (a row, read on demand) or an owned object
     * (the system prompt, a tool notice). There is no "borrowed reference" any more: the
     * old _ref existed only because the turn owned the message, and a turn has no message
     * array to own. */
    edc_task_group_history_add_owned(g, mk_msg("system", "group prompt"));
    edc_task_group_history_add_owned(g, mk_msg("user", "hi"));
    edc_task_group_history_add_owned(g, mk_msg("assistant", "hello"));
    edc_task_group_history_add_owned(g, mk_msg("user", "second"));

    CHECK(edc_task_group_history_count(g) == 4, "history has 4 entries");

    /* Read back through the slot API: an owned slot hands out a copy the caller owns. */
    cJSON *first = edc_task_group_history_at(g, 0);
    CHECK(first != NULL, "slot 0 reads back");
    CHECK(first && strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(first, "content")), "group prompt") == 0,
          "slot 0 content");
    cJSON_Delete(first);

    TEST("history survives growth past initial capacity");
    for (int i = 0; i < 40; i++)
        edc_task_group_history_add_owned(g, mk_msg("user", "x"));
    CHECK(edc_task_group_history_count(g) == 44, "history grew to 44 entries");

    edc_task_group_free(g);
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
