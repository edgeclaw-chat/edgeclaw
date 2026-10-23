#include "edc_task.h"
#include "test_ws.h"
#include "edc_task_group.h"
#include "edc_turn.h"
#include "log.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int failed = 0;
static int passed = 0;

#define T(name) do { log_info("  TEST: %s", name); } while(0)
#define C(cond, ...) do { \
    if (!(cond)) { log_error("    FAIL: " __VA_ARGS__); failed++; } \
    else { passed++; } \
} while(0)

/* ── 1. group->history 初始化/清理 ────────────────────────── */

static void test_group_history_lifecycle(void)
{
    T("group_new creates empty history array");
    struct edc_task *l = edc_task_new("gh-1", "s-1", "gh-1", "leader", edc_channel_adapter);
    struct edc_task_group *g = edc_task_group_new(l);
    C(g->history != NULL, "history non-NULL");
    C(cJSON_GetArraySize(g->history) == 0, "history starts empty");
    edc_task_group_free(g);
}

/* ── 2. group->history 上下文累积 ─────────────────────────── */

static void test_group_history_accumulation(void)
{
    T("STOP sync adds messages to group->history");
    struct edc_task *l = edc_task_new("gh-acc-1", "s-2", "gh-acc-1", "L", edc_channel_adapter);
    struct edc_task_group *g = edc_task_group_new(l);

    /* Simulate turn completion sync (as done in STOP handler) */
    struct edc_turn *turn = edc_turn_new();
    edc_turn_add_user_message(turn, "hello");
    edc_turn_add_assistant_message(turn, cJSON_Parse("{\"role\":\"assistant\",\"content\":\"hi\"}"));

    /* Sync: copy messages to group history */
    for (int qi = 0; qi < turn->msg_count; qi++)
    {
        cJSON_AddItemReferenceToArray(g->history, edc_turn_msg_at(turn, qi));
    }
    C(cJSON_GetArraySize(g->history) == 2, "2 messages synced");

    /* Second turn */
    struct edc_turn *turn2 = edc_turn_new();
    edc_turn_add_user_message(turn2, "world");
    edc_turn_add_assistant_message(turn2, cJSON_Parse("{\"role\":\"assistant\",\"content\":\"there\"}"));

    for (int qi = 0; qi < turn2->msg_count; qi++)
    {
        cJSON_AddItemReferenceToArray(g->history, edc_turn_msg_at(turn2, qi));
    }
    C(cJSON_GetArraySize(g->history) == 4, "4 messages after second turn");

    /* Verify content */
    cJSON *m0 = cJSON_GetArrayItem(g->history, 0);
    C(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(m0, "role")), "user") == 0, "msg0 is user");
    cJSON *m3 = cJSON_GetArrayItem(g->history, 3);
    C(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(m3, "role")), "assistant") == 0, "msg3 is assistant");

    edc_turn_free(turn);
    edc_turn_free(turn2);
    edc_task_group_free(g);
}

/* ── 3. group 隔离 ─────────────────────────────────────────── */

static void test_group_isolation(void)
{
    T("different groups have independent history");
    struct edc_task *l1 = edc_task_new("iso-1", "s-3", "iso-1", "G1L", edc_channel_adapter);
    struct edc_task_group *g1 = edc_task_group_new(l1);

    struct edc_task *l2 = edc_task_new("iso-2", "s-3", "iso-2", "G2L", edc_channel_adapter);
    struct edc_task_group *g2 = edc_task_group_new(l2);

    /* Add message to g1 only */
    struct edc_turn *t = edc_turn_new();
    edc_turn_add_user_message(t, "g1 msg");
    for (int qi = 0; qi < t->msg_count; qi++)
        cJSON_AddItemReferenceToArray(g1->history, edc_turn_msg_at(t, qi));

    C(cJSON_GetArraySize(g1->history) == 1, "g1 has 1 msg");
    C(cJSON_GetArraySize(g2->history) == 0, "g2 remains empty (isolated)");

    edc_turn_free(t);
    edc_task_group_free(g1);
    edc_task_group_free(g2);
}

/* ── 4. task->group 指针 ───────────────────────────────────── */

static void test_task_group_pointer(void)
{
    T("task->group pointer set on add, cleared on remove");
    struct edc_task *l = edc_task_new("gp-1", "s-4", "gp-1", "L", edc_channel_adapter);
    struct edc_task_group *g = edc_task_group_new(l);
    C(l->group == g, "leader task->group set");

    struct edc_task *m = edc_task_new("gp-2", "s-4", "gp-1", "M", edc_channel_adapter);
    edc_task_group_add_task(g, m);
    C(m->group == g, "member task->group set");

    edc_task_group_remove_task(g, m);
    C(m->group == NULL, "group cleared after remove");
    edc_task_free(m);

    edc_task_group_free(g);
}

/* ── 5. JOIN 源完成 → 通知依赖者（回收是调用方的事）──────────── */

static void test_join_source_notify(void)
{
    T("JOIN source notifies its dependents");
    struct edc_task *src = edc_task_new("ar-src", "s-5", "ar-src", "source", edc_channel_adapter);
    struct edc_task *dst = edc_task_new("ar-dst", "s-5", "ar-dst", "dest", edc_channel_adapter);

    /* Set up groups */
    struct edc_task_group *g_src = edc_task_group_new(src);
    struct edc_task_group *g_dst = edc_task_group_new(dst);

    /* Chain: dst depends on src */
    edc_task_add_dependency(dst, src, TASK_DEP_JOIN);
    C(src->dependents != NULL, "src has dependents");
    C(dst->deps_total == 1, "dst has 1 dep");

    /* Simulate src completing */
    sds out = sdsnew("result from src");
    edc_task_set_output(src, out);
    edc_task_mark_ready(src);
    edc_task_await_exec(src);
    edc_task_mark_completed(src);

    /* 通知依赖者 (JOIN 源完成时 scheduler 的收尾路径)。 */
    edc_task_notify_dependents(src);
    C(dst->deps_completed == 1, "dst notified");

    /* 移除 + 释放是调用方的事 (运行时没有 auto-reap)。 */
    edc_task_group_remove_task(g_src, src);
    edc_task_free(src);
    src = NULL;

    C(g_src->member_count == 0, "移除唯一成员后组空掉");

    /* Cleanup */
    edc_task_group_free(g_src);  /* empty group */
    edc_task_group_free(g_dst);
}

/* ── 6. No-dependents task stays in the tree ────────────────── */

static void test_no_dep_stays(void)
{
    T("task without dependents stays COMPLETED in the tree");
    struct edc_task *t = edc_task_new("nd-1", "s-6", "nd-1", "standalone", edc_channel_adapter);
    struct edc_task_group *g = edc_task_group_new(t);

    sds out = sdsnew("standalone result");
    edc_task_set_output(t, out);
    edc_task_mark_ready(t);
    edc_task_await_exec(t);
    edc_task_mark_completed(t);

    /* 没有依赖者 → 没有要通知的; 任务留在树里。 */
    C(t->dependents == NULL, "no dependents");
    C(edc_task_is_completed(t), "state is COMPLETED");
    C(g->member_count == 1, "still in group");

    edc_task_group_free(g);
}

/* ── 7. edc_task_get_context_messages 迭代 ────────────────── */

static void test_context_messages_iteration(void)
{
    T("context_messages includes turn messages via EdcQueue iteration");
    struct edc_task *t = edc_task_new("ctx-1", "s-7", "ctx-1", "ctx test", edc_channel_adapter);

    /* Add completed turns to the queue */
    struct edc_turn *turn1 = edc_turn_new();
    edc_turn_add_user_message(turn1, "hello");
    edc_turn_add_assistant_message(turn1, cJSON_Parse("{\"role\":\"assistant\",\"content\":\"hi\"}"));
    edc_task_add_turn(t, turn1);

    struct edc_turn *turn2 = edc_turn_new();
    edc_turn_add_user_message(turn2, "world");
    edc_turn_add_assistant_message(turn2, cJSON_Parse("{\"role\":\"assistant\",\"content\":\"there\"}"));
    edc_task_add_turn(t, turn2);

    /* Build context (no config → no system message) */
    cJSON *msgs = edc_task_get_context_messages(t, NULL);
    /* Should have: just the 4 turn messages */
    int count = cJSON_GetArraySize(msgs);
    C(count == 4, "4 messages: the turn messages only, got %d", count);

    /* Verify roles */
    cJSON *m0 = cJSON_GetArrayItem(msgs, 0);
    cJSON *r0 = cJSON_GetObjectItem(m0, "role");
    C(r0 && strcmp(r0->valuestring, "user") == 0, "msg0 is user");

    cJSON *m1 = cJSON_GetArrayItem(msgs, 1);
    cJSON *r1 = cJSON_GetObjectItem(m1, "role");
    C(r1 && strcmp(r1->valuestring, "assistant") == 0, "msg1 is assistant");

    cJSON_Delete(msgs);
    edc_task_free(t);
}

/* ── 8. 压力: 大组历史 ────────────────────────────────────── */

static void test_large_group_history(void)
{
    T("group history with 100 turns (200 messages)");
    struct edc_task *l = edc_task_new("big-1", "s-8", "big-1", "L", edc_channel_adapter);
    struct edc_task_group *g = edc_task_group_new(l);

    for (int i = 0; i < 100; i++)
    {
        struct edc_turn *t = edc_turn_new();
        char buf[64];
        snprintf(buf, sizeof(buf), "user msg %d", i);
        edc_turn_add_user_message(t, buf);
        snprintf(buf, sizeof(buf), "assistant msg %d", i);
        cJSON *amsg = cJSON_CreateObject();
        cJSON_AddStringToObject(amsg, "role", "assistant");
        cJSON_AddStringToObject(amsg, "content", buf);
        edc_turn_add_assistant_message(t, amsg);

        for (int qi = 0; qi < t->msg_count; qi++)
            cJSON_AddItemReferenceToArray(g->history, edc_turn_msg_at(t, qi));

        edc_turn_free(t);
    }

    C(cJSON_GetArraySize(g->history) == 200, "200 messages in history");
    edc_task_group_free(g);
}

int main(void)
{
    edc_test_workspace(); /* open the disposable store before anything writes */
    log_info("=== Group-level 稳定性测试 ===");
    log_info("");

    test_group_history_lifecycle();
    test_group_history_accumulation();
    test_group_isolation();
    test_task_group_pointer();
    test_join_source_notify();
    test_no_dep_stays();
    test_context_messages_iteration();
    test_large_group_history();

    log_info("");
    log_info("=== %d passed, %d failed ===", passed, failed);
    return failed > 0 ? 1 : 0;
}
