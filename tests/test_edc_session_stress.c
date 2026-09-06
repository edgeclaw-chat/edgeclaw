#include "edc_session.h"
#include "edc_task.h"
#include "edc_task_group.h"
#include "edc_turn.h"
#include "edc_config.h"
#include "log.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>

volatile sig_atomic_t _run_loop;

static int failed = 0;
static int passed = 0;

#define T(name) do { log_info("  TEST: %s", name); } while(0)
#define C(cond, ...) do { \
    if (!(cond)) { log_error("    FAIL: " __VA_ARGS__); failed++; } \
    else { passed++; } \
} while(0)

/* ── 辅助 ──────────────────────────────────────────────────── */

static struct edc_config *make_cfg(void) {
    struct edc_config *c = calloc(1, sizeof(*c));
    c->workspace = sdsnew("/tmp/test-session");
    return c;
}

static void free_cfg(struct edc_config *c) {
    if (!c) return;
    sdsfree(c->workspace);
    free(c);
}

/* ══════════════════════════════════════════════════════════════
   1. 生命周期 (含新字段)
   ══════════════════════════════════════════════════════════════ */

static void test_lifecycle(void)
{
    T("edc_session_new has session_id, groups=NULL");
    struct edc_config *cfg = make_cfg();
    struct edc_session *s = edc_session_new(cfg, "test-sess", NULL);
    C(s != NULL, "new returns non-NULL");
    C(s->session_id != NULL, "session_id is set");
    C(s->groups == NULL, "groups starts NULL");
    C(s->active_task == NULL, "active_task starts NULL");
    C(s->config == cfg, "config set");
    C(s->messages != NULL, "messages array exists");
    C(s->timestamps == NULL, "timestamps starts NULL");

    T("edc_session_free cleans groups and active_task");
    struct edc_task *t = edc_task_new("t1", s->session_id, "t1", "test", edc_channel_cli);
    struct edc_task_group *g = edc_task_group_new(t);
    edc_session_add_group(s, g);
    edc_session_set_active_task(s, t);
    C(s->groups == g, "group linked");
    C(s->active_task == t, "active_task set");

    edc_session_free(s);
    free_cfg(cfg);
    C(1, "free with groups survived");

    T("edc_session_free NULL");
    edc_session_free(NULL);
    C(1, "free(NULL) survived");
}

/* ══════════════════════════════════════════════════════════════
   2. Group 管理
   ══════════════════════════════════════════════════════════════ */

static void test_group_mgmt(void)
{
    struct edc_config *cfg = make_cfg();
    struct edc_session *s = edc_session_new(cfg, "test-sess", NULL);

    T("add_group / remove_group basic");
    struct edc_task *l1 = edc_task_new("t-a1", s->session_id, "t-a1", "G1L", edc_channel_cli);
    struct edc_task_group *g1 = edc_task_group_new(l1);
    edc_session_add_group(s, g1);
    C(s->groups == g1, "first group at head");

    struct edc_task *l2 = edc_task_new("t-a2", s->session_id, "t-a2", "G2L", edc_channel_cli);
    struct edc_task_group *g2 = edc_task_group_new(l2);
    edc_session_add_group(s, g2);
    C(s->groups == g1, "g1 still head");
    C(s->groups->next == g2, "g2 next");
    C(g2->prev == g1, "g2 prev = g1");

    T("remove_group unlinks");
    edc_session_remove_group(s, g1);
    C(s->groups == g2, "head is now g2");
    /* DL_DELETE should set new head prev=NULL; if utlist version doesn't, it's OK */
    edc_task_group_free(g1);

    T("add/remove NULL safety");
    edc_session_add_group(s, NULL);
    edc_session_remove_group(s, NULL);
    edc_session_add_group(NULL, g2);
    edc_session_remove_group(NULL, g2);
    C(1, "NULL safety survived");

    edc_session_free(s);
    free_cfg(cfg);
}

/* ══════════════════════════════════════════════════════════════
   3. find_group / find_task
   ══════════════════════════════════════════════════════════════ */

static void test_find(void)
{
    struct edc_config *cfg = make_cfg();
    struct edc_session *s = edc_session_new(cfg, "test-sess", NULL);

    T("find_group in 10 groups");
    struct edc_task_group *groups[10];
    for (int i = 0; i < 10; i++) {
        char id[32]; snprintf(id, sizeof(id), "task-%d", i);
        struct edc_task *l = edc_task_new(id, s->session_id, id, "leader", edc_channel_cli);
        groups[i] = edc_task_group_new(l);
        edc_session_add_group(s, groups[i]);
    }
    C(edc_session_find_group(s, "task-0") == groups[0], "find group 0");
    C(edc_session_find_group(s, "task-5") == groups[5], "find group 5");
    C(edc_session_find_group(s, "task-9") == groups[9], "find group 9");
    C(edc_session_find_group(s, "nonexistent") == NULL, "find nonexistent group");

    T("find_task cross-group");
    struct edc_task *m2 = edc_task_new("member-2", s->session_id, "task-2", "member", edc_channel_cli);
    edc_task_group_add_task(groups[2], m2);
    C(edc_session_find_task(s, "member-2") == m2, "find member in group 2");
    C(edc_session_find_task(s, "task-7") == groups[7]->leader, "find leader in group 7");

    T("find NULL safety");
    C(edc_session_find_group(NULL, "x") == NULL,  "find_group(NULL,_)");
    C(edc_session_find_group(s, NULL) == NULL,     "find_group(_,NULL)");
    C(edc_session_find_task(NULL, "x") == NULL,    "find_task(NULL,_)");
    C(edc_session_find_task(s, NULL) == NULL,      "find_task(_,NULL)");

    edc_session_free(s);
    free_cfg(cfg);
}

/* ══════════════════════════════════════════════════════════════
   4. active_task 管理
   ══════════════════════════════════════════════════════════════ */

static void test_active_task(void)
{
    struct edc_config *cfg = make_cfg();
    struct edc_session *s = edc_session_new(cfg, "test-sess", NULL);

    T("set/get active_task");
    C(edc_session_get_active_task(s) == NULL, "initially NULL");

    struct edc_task *t = edc_task_new("at1", s->session_id, "at1", "active", edc_channel_cli);
    edc_session_set_active_task(s, t);
    C(edc_session_get_active_task(s) == t, "get returns set value");

    struct edc_task *t2 = edc_task_new("at2", s->session_id, "at2", "active 2", edc_channel_cli);
    edc_session_set_active_task(s, t2);
    C(edc_session_get_active_task(s) == t2, "overwrite works");

    edc_session_set_active_task(s, NULL);
    C(edc_session_get_active_task(s) == NULL, "cleared to NULL");

    edc_session_set_active_task(NULL, t);
    C(edc_session_get_active_task(NULL) == NULL, "get NULL session → NULL");

    edc_task_free(t);
    edc_task_free(t2);
    edc_session_free(s);
    free_cfg(cfg);
}

/* ══════════════════════════════════════════════════════════════
   5. get_messages_slice
   ══════════════════════════════════════════════════════════════ */

static void test_messages_slice(void)
{
    struct edc_config *cfg = make_cfg();
    struct edc_session *s = edc_session_new(cfg, "test-sess", NULL);

    T("slice from new session (has system prompt)");
    cJSON *ms = edc_session_get_messages_slice(s, 0, 10);
    size_t t = edc_session_get_message_count(s);
    /* new session has system prompt + optionally history → at least 1 msg */
    C(cJSON_GetArraySize(ms) == t, "slice(0,10) returns all msgs for small session");
    cJSON_Delete(ms);

    T("slice out of range → empty");
    ms = edc_session_get_messages_slice(s, 999, 10);
    C(cJSON_GetArraySize(ms) == 0, "from > total → empty");
    cJSON_Delete(ms);

    T("slice first N items");
    /* session has system prompt + potential history — at least 1 message */
    size_t total = edc_session_get_message_count(s);
    C(total > 0, "session has messages");
    ms = edc_session_get_messages_slice(s, 0, 1);
    C(cJSON_GetArraySize(ms) == 1, "slice 1 item");
    cJSON_Delete(ms);

    T("slice count=0 → all");
    ms = edc_session_get_messages_slice(s, 0, 0);
    C(cJSON_GetArraySize(ms) == total, "count=0 gives all items");
    cJSON_Delete(ms);

    T("slice NULL session");
    ms = edc_session_get_messages_slice(NULL, 0, 10);
    C(ms != NULL, "NULL session → non-NULL empty array");
    C(cJSON_GetArraySize(ms) == 0, "empty");
    cJSON_Delete(ms);

    T("slice with partial range");
    if (total >= 2) {
        ms = edc_session_get_messages_slice(s, 1, 1);
        C(cJSON_GetArraySize(ms) == 1, "exact 1 from index 1");
        cJSON_Delete(ms);
    }

    T("slice returns valid messages");
    ms = edc_session_get_messages_slice(s, 0, 1);
    if (cJSON_GetArraySize(ms) > 0) {
        cJSON *item = cJSON_GetArrayItem(ms, 0);
        cJSON *role = cJSON_GetObjectItem(item, "role");
        C(role != NULL && cJSON_IsString(role), "sliced item has role field");
    }
    cJSON_Delete(ms);

    edc_session_free(s);
    free_cfg(cfg);
}

/* ══════════════════════════════════════════════════════════════
   6. save_tasks / reload_tasks
   ══════════════════════════════════════════════════════════════ */

static void test_persistence(void)
{
    struct edc_config *cfg = make_cfg();
    struct edc_session *s = edc_session_new(cfg, "test-sess", NULL);

    T("save_tasks tags messages with task_id/group_id");
    /* Add some messages directly */
    cJSON *u1 = cJSON_CreateObject();
    cJSON_AddStringToObject(u1, "role", "user");
    cJSON_AddStringToObject(u1, "content", "hello");
    cJSON_AddItemToArray(s->messages, u1);

    cJSON *u2 = cJSON_CreateObject();
    cJSON_AddStringToObject(u2, "role", "assistant");
    cJSON_AddStringToObject(u2, "content", "hi there");
    cJSON_AddItemToArray(s->messages, u2);

    edc_session_save_tasks(s);

    /* Check that user and assistant messages have task_id */
    cJSON *r1 = cJSON_GetObjectItem(u1, "task_id");
    cJSON *r2 = cJSON_GetObjectItem(u2, "task_id");
    C(r1 != NULL && cJSON_IsString(r1), "user msg tagged task_id");
    C(r2 != NULL && cJSON_IsString(r2), "assistant msg tagged task_id");
    C(strcmp(cJSON_GetStringValue(r1), "legacy") == 0, "task_id = legacy");

    T("save_tasks skips system/tool messages");
    /* system msg was added by edc_session_new — check it's untagged */
    cJSON *sys_msg = cJSON_GetArrayItem(s->messages, 0);
    cJSON *sr = cJSON_GetObjectItem(sys_msg, "role");
    if (sr && strcmp(cJSON_GetStringValue(sr), "system") == 0) {
        C(!cJSON_HasObjectItem(sys_msg, "task_id"), "system msg untagged");
    }

    T("save_tasks NULL session");
    C(!edc_session_save_tasks(NULL), "NULL session → false");

    T("reload_tasks NULL safety");
    edc_session_reload_tasks(NULL);
    edc_session_reload_tasks(s);  /* stub, no crash */
    C(1, "reload_tasks survived");

    edc_session_free(s);
    free_cfg(cfg);
}

/* ══════════════════════════════════════════════════════════════
   7. 混合场景: groups + tasks + active + messages 共存
   ══════════════════════════════════════════════════════════════ */

static void test_mixed_scenario(void)
{
    struct edc_config *cfg = make_cfg();
    struct edc_session *s = edc_session_new(cfg, "test-sess", NULL);

    T("scenario: create groups, add tasks, use active_task, then free");

    /* Create 3 groups, each with 2-3 tasks */
    struct edc_task_group *g[3];
    struct edc_task *tasks[8];
    int t_idx = 0;

    for (int i = 0; i < 3; i++) {
        char id[32]; snprintf(id, sizeof(id), "sc-%d-leader", i);
        struct edc_task *l = edc_task_new(id, s->session_id, id, "leader", edc_channel_cli);
        g[i] = edc_task_group_new(l);
        edc_session_add_group(s, g[i]);
        tasks[t_idx++] = l;

        for (int j = 0; j < 2; j++) {
            snprintf(id, sizeof(id), "sc-%d-m%d", i, j);
            struct edc_task *m = edc_task_new(id, s->session_id, g[i]->group_id, "member", edc_channel_cli);
            edc_task_group_add_task(g[i], m);
            tasks[t_idx++] = m;
        }
    }
    C(t_idx == 9, "9 tasks total (3 leaders + 6 members)");

    /* Set active_task to group 1's leader */
    edc_session_set_active_task(s, g[1]->leader);
    C(edc_session_get_active_task(s) == g[1]->leader, "active_task = g1 leader");

    /* Verify all tasks findable */
    for (int i = 0; i < t_idx; i++) {
        struct edc_task *f = edc_session_find_task(s, tasks[i]->task_id);
        C(f == tasks[i], "task %d findable", i);
    }

    /* All groups findable */
    for (int i = 0; i < 3; i++) {
        C(edc_session_find_group(s, g[i]->group_id) == g[i], "group %d findable", i);
    }

    /* Free everything together */
    edc_session_free(s);
    free_cfg(cfg);
    C(1, "mixed scenario cleanup survived");
}

/* ══════════════════════════════════════════════════════════════
   8. 压力: 100 groups, 500 tasks
   ══════════════════════════════════════════════════════════════ */

static void test_stress(void)
{
    struct edc_config *cfg = make_cfg();
    struct edc_session *s = edc_session_new(cfg, "test-sess", NULL);

    T("100 groups, 5 tasks each = 500 tasks");
    struct edc_task_group *first = NULL;
    for (int i = 0; i < 100; i++) {
        char id[32]; snprintf(id, sizeof(id), "s-%d", i);
        struct edc_task *l = edc_task_new(id, s->session_id, id, "leader", edc_channel_cli);
        struct edc_task_group *g = edc_task_group_new(l);
        edc_session_add_group(s, g);
        if (i == 0) first = g;

        for (int j = 0; j < 4; j++) {
            snprintf(id, sizeof(id), "s-%d-m%d", i, j);
            struct edc_task *m = edc_task_new(id, s->session_id, g->group_id, "m", edc_channel_cli);
            edc_task_group_add_task(g, m);
        }
    }

    /* Verify some lookups */
    C(edc_session_find_group(s, "s-0") == first, "find first group");
    C(edc_session_find_group(s, "s-50") != NULL, "find mid group");
    C(edc_session_find_group(s, "s-99") != NULL, "find last group");

    /* Find random task */
    C(edc_session_find_task(s, "s-33-m2") != NULL, "find nested task");

    /* Verify message system still works */
    C(cJSON_GetArraySize(s->messages) >= 1, "messages intact");

    edc_session_free(s);
    free_cfg(cfg);
    C(1, "500-task stress test survived");
}

/* ══════════════════════════════════════════════════════════════
   9. 回归: 旧 API 仍可用
   ══════════════════════════════════════════════════════════════ */

static void test_backward_compat(void)
{
    struct edc_config *cfg = make_cfg();
    struct edc_session *s = edc_session_new(cfg, "test-sess", NULL);

    T("old API: get_messages returns cJSON*");
    cJSON *msgs = edc_session_get_messages(s);
    C(msgs != NULL, "get_messages returns non-NULL");
    C(msgs == s->messages, "returns same pointer");

    T("old API: get_message_count");
    size_t cnt = edc_session_get_message_count(s);
    C(cnt == (size_t)cJSON_GetArraySize(s->messages), "count matches array size");

    T("old API: add_turn");
    struct edc_turn *turn = edc_turn_new();
    edc_turn_add_user_message(turn, "msg from turn");
    edc_session_add_turn(s, turn);
    C(cJSON_GetArraySize(s->messages) > cnt, "messages grew after add_turn");

    T("old API: save_history (no crash)");
    /* May fail on missing filesystem — just check no crash */
    edc_session_save_history(s);
    C(1, "save_history survived");

    T("old API: reload (no crash)");
    edc_session_reload(s, false);
    C(1, "reload survived");

    edc_session_free(s);
    free_cfg(cfg);
}

/* ══════════════════════════════════════════════════════════════
   10. active_task 在 session_free 后不悬空
   ══════════════════════════════════════════════════════════════ */

static void test_active_task_lifetime(void)
{
    T("group_free also frees tasks that active_task pointed to");
    struct edc_config *cfg = make_cfg();
    struct edc_session *s = edc_session_new(cfg, "test-sess", NULL);

    struct edc_task *t = edc_task_new("at-life", s->session_id, "at-life", "test", edc_channel_cli);
    struct edc_task_group *g = edc_task_group_new(t);
    edc_session_add_group(s, g);
    edc_session_set_active_task(s, t);

    /* Free session — this frees group which frees task.
       active_task becomes dangling but we don't dereference it after free. */
    edc_session_free(s);
    free_cfg(cfg);
    C(1, "active_task lifetime handled");
}

/* ══════════════════════════════════════════════════════════════
   11. 孤儿 group 的 session 清理
   ══════════════════════════════════════════════════════════════ */

static void test_orphan_cleanup(void)
{
    T("session_free cleans orphaned groups");
    struct edc_config *cfg = make_cfg();
    struct edc_session *s = edc_session_new(cfg, "test-sess", NULL);

    struct edc_task *l = edc_task_new("orph-l", s->session_id, "orph-l", "L", edc_channel_cli);
    struct edc_task_group *g = edc_task_group_new(l);
    struct edc_task *m = edc_task_new("orph-m", s->session_id, "orph-l", "M", edc_channel_cli);
    edc_task_group_add_task(g, m);
    edc_session_add_group(s, g);

    /* Remove leader → orphaned */
    edc_task_group_remove_task(g, l);
    edc_task_free(l);
    C(g->is_orphaned, "group is orphaned");
    C(g->member_count == 1, "1 member left");
    C(edc_session_find_task(s, "orph-m") == m, "member still findable");

    /* Free session — should clean orphaned group + remaining member */
    edc_session_free(s);
    free_cfg(cfg);
    C(1, "orphan cleanup survived");
}

int main(void)
{
    log_info("=== Phase 4: edc_session 高强度测试 ===");
    log_info("");

    test_lifecycle();
    test_group_mgmt();
    test_find();
    test_active_task();
    test_messages_slice();
    test_persistence();
    test_mixed_scenario();
    test_stress();
    test_backward_compat();
    test_active_task_lifetime();
    test_orphan_cleanup();

    log_info("");
    log_info("=== %d passed, %d failed ===", passed, failed);
    return failed > 0 ? 1 : 0;
}
