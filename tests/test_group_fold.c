/* The fold runs as a system task { sink = GROUP_MEMORY } in the session's system
 * group -- an ordinary persisted task, not a hidden/transient one. Its input is the
 * fold prompt; the output lands in the group's memory.
 * This file pins: the fold behaviour, the barrier (group tasks held by a real dep
 * until the fold lands), and that it does not pollute the folded group. */

#include "edc_compat.h"
#include "edc_config.h"
#include "edc_executor.h"
#include "edc_group_fold.h"
#include "edc_llm_actor.h"
#include "edc_scheduler.h"
#include "edc_session.h"
#include "edc_session_manager.h"
#include "edc_state_serializer.h"
#include "edc_task.h"
#include "edc_task_group.h"
#include "edc_turn.h"
#include "log.h"
#include "utlist_fix.h"
#include <signal.h>
#include <stdio.h>
#include <string.h>
/* The session store: folding itself needs none, but scheduling does, since the
 * message-end scan enumerates tasks through it. A simple in-memory manager. */
static void install_resolver(struct edc_scheduler *sched, struct edc_session *sess)
{
    struct edc_session_manager *sm = g_new0(struct edc_session_manager, 1);
    sm->sessions = g_new0(struct edc_session *, 1);
    sm->sessions[0] = sess;
    sm->session_count = sess ? 1 : 0;
    sm->active_index = 0;
    sched->sessions = sm; /* 测试进程内不释放: 与测试同生共死 */
}
volatile sig_atomic_t _run_loop = 1;

/* ══════════════════════════════════════════════════════════════
 *  Test framework
 *  ══════════════════════════════════════════════════════════════ */

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
 *  ══════════════════════════════════════════════════════════════ */

static struct edc_config *make_config(void)
{
    struct edc_config *cfg = g_new0(struct edc_config, 1);
    cfg->workspace = sdsnew("/tmp/test_group_fold_workspace");
    return cfg;
}

/* Folding takes a real scheduler (a plain struct, no constructor).
 * Build a group of finished, turn-bearing (foldable) tasks. */
static void add_foldable_task(struct edc_session *session, struct edc_task_group *group, const char *id,
                              uint32_t completion_seq, int msgs_per_task, const char *text)
{
    struct edc_task *t = edc_task_new(id, session->session_id, group->group_id, text, edc_channel_adapter);
    edc_task_group_add_task(group, t);
    struct edc_turn *turn = edc_turn_new();
    turn->task_id = strdup(t->task_id);
    for (int i = 0; i < msgs_per_task; i++)
    {
        cJSON *m = cJSON_CreateObject();
        cJSON_AddStringToObject(m, "role", "user");
        cJSON_AddStringToObject(m, "content", text);
        edc_queue_push(&turn->messages, m);
    }
    edc_task_add_turn(t, turn);
    edc_task_await_exec(t);
    edc_task_mark_completed(t);
    t->completion_seq = completion_seq;
    t->output = sdsnew(text);
}

/* send_llm 的桩: 单测里没有 LLM actor。派发路径只证明"prompt 被当成一次正常的
 * LLM 请求发出去了"; 响应由测试用 edc_executor_advance_llm_resp 灌回来 —— 与真实
 * 路径 (llm actor → scheduler → executor) 的差别只在传输。 */
static int g_send_llm_calls = 0;
static sds g_last_llm_task_id = NULL; /* 最近一次 send_llm 的任务 id (闸门断言用) */
static void stub_send_llm(struct edc_scheduler *sched, struct edc_task *task, void *request_id)
{
    (void)sched;
    (void)request_id;
    g_send_llm_calls++;
    if (g_last_llm_task_id)
        sdsfree(g_last_llm_task_id);
    g_last_llm_task_id = task ? sdsnew(task->task_id) : NULL;
}

/* 折叠任务也是**普通任务**: 起折叠只把 prompt 装上, "跑"发生在消息末尾的调度
 * (actor handler 每条消息末尾调 `edc_scheduler_schedule`)。测试里直接调那个入口,
 * 等价于"这条消息处理完了"。 */
static void pump_schedule(struct edc_scheduler *sched)
{
    edc_scheduler_schedule(sched);
}

static int count_folded(struct edc_task_group *group)
{
    int n = 0;
    struct edc_task *t;
    LL_FOREACH2(group->members, t, group_next)
    {
        if (t->turns_folded)
            n++;
    }
    return n;
}

static int count_tasks_in(struct edc_session *session)
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

/* ══════════════════════════════════════════════════════════════
 *  Test 1: 触发判据 + prompt 构造 (行为与旧 memory_actor 一致)
 *  ══════════════════════════════════════════════════════════════ */

static void test_prompt_and_due(void)
{
    TEST("触发判据 + prompt (半量策略 / 现有记忆 / output / max_seq)");

    struct edc_config *cfg = make_config();
    struct edc_session *session = edc_session_new(cfg, "sess-prompt", "Prompt");

    struct edc_task *leader = edc_task_new("1", "sess-prompt", "1", "leader", edc_channel_adapter);
    struct edc_task_group *group = edc_task_group_new(leader);
    edc_session_add_group(session, group);
    edc_task_await_exec(leader);
    edc_task_mark_completed(leader);
    leader->completion_seq = 1;
    struct edc_turn *lturn = edc_turn_new();
    cJSON *lm = cJSON_CreateObject();
    cJSON_AddStringToObject(lm, "role", "user");
    cJSON_AddStringToObject(lm, "content", "leader work");
    edc_queue_push(&lturn->messages, lm);
    edc_task_add_turn(leader, lturn);

    /* 还没到阈值: 4 条未折叠消息 < 10 */
    CHECK(!edc_group_fold_due(group, 10), "未达阈值不折");
    CHECK(!edc_group_fold_due(group, 2), "未折叠消息数 (leader 的 1 条) 仍低于阈值");
    CHECK(!edc_group_fold_due(NULL, 2), "NULL 组不折");
    CHECK(!edc_group_fold_due(group, 0), "阈值 <= 0 不折");

    group->memory = sdsnew("# Old memory\n- something remembered");

    /* 再加三个已完成任务: seq 2,3,4,各 4 条消息 → 未折叠 = 4*3 + leader的1 = 13 */
    add_foldable_task(session, group, "2", 2, 4, "task two");
    add_foldable_task(session, group, "3", 3, 4, "task three");
    add_foldable_task(session, group, "4", 4, 4, "task four");

    size_t unfolded = edc_task_group_unfolded_msg_count(group);
    CHECK(unfolded == 13, "未折叠消息数 = 13");

    uint32_t max_seq = 0;
    sds prompt = edc_group_fold_build_prompt(group, &max_seq);
    CHECK(prompt != NULL, "拿到 prompt");
    if (prompt)
    {
        CHECK(strstr(prompt, "## Existing Group Memory") != NULL, "带现有记忆段");
        CHECK(strstr(prompt, "# Old memory") != NULL, "现有记忆内容在内 (合并语义)");
        CHECK(strstr(prompt, "## New Completed Tasks") != NULL, "带新任务段");
        CHECK(strstr(prompt, "[task 1]") != NULL && strstr(prompt, "[task 2]") != NULL,
              "最老的先压 (leader seq=1 与 task 2 都在)");
        CHECK(strstr(prompt, "output: task two") != NULL, "任务的 output 也进去 (JOIN 语义)");
        /* 半量: 预算 = 13/2 = 6 → 只取 seq 2 (4 条) + seq 3 (4 条)? 4+4=8 > 6 → 只取
         * seq 2; 即"最老、且累计不超过一半"的前缀。 */
        CHECK(max_seq == 2, "max_seq 落在最老那批的边界 (leader 1 条 + task 2 的 4 条 = 5 ≤ 预算 6)");
        CHECK(strstr(prompt, "[task 3]") == NULL, "没被选中的 task 不进 prompt (留给下一轮当细节)");
        CHECK(strstr(prompt, "Merge the tasks above") != NULL, "带合并指令");
        sdsfree(prompt);
    }

    edc_session_free(session);
    edc_config_free(cfg);
}

/* ══════════════════════════════════════════════════════════════
 *  Test 2: 起一次折叠 = 建一个 system task (并跑完它)
 *  ══════════════════════════════════════════════════════════════ */

static void test_fold_runs_as_a_system_task(void)
{
    TEST("起折叠: 建 system task → 产出经 sink 写进记忆 → 回收");

    struct edc_config *cfg = make_config();
    struct edc_session *session = edc_session_new(cfg, "sess-fold", "Fold");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, session);
    sched->send_llm = stub_send_llm;
    g_send_llm_calls = 0;

    struct edc_task *leader = edc_task_new("1", "sess-fold", "1", "leader", edc_channel_adapter);
    struct edc_task_group *group = edc_task_group_new(leader);
    edc_session_add_group(session, group);
    edc_task_await_exec(leader);
    edc_task_mark_completed(leader);
    leader->completion_seq = 1;
    struct edc_turn *lturn = edc_turn_new();
    cJSON *lm = cJSON_CreateObject();
    cJSON_AddStringToObject(lm, "role", "user");
    cJSON_AddStringToObject(lm, "content", "leader work");
    edc_queue_push(&lturn->messages, lm);
    edc_task_add_turn(leader, lturn);

    for (int i = 2; i <= 4; i++)
    {
        char id[8];
        snprintf(id, sizeof(id), "%d", i);
        add_foldable_task(session, group, id, (uint32_t)i, 4, "foldable work");
    }
    size_t group_hist_before = cJSON_GetArraySize(group->history);
    size_t group_msgs_before = edc_task_group_unfolded_msg_count(group);
    uint32_t seq_before = group->folded_through_completion_seq;

    CHECK(edc_group_fold_start(sched, session, group) != NULL, "折叠任务起来了");
    pump_schedule(sched);
    CHECK(g_send_llm_calls == 1, "折叠 prompt 作为一次普通 LLM 请求发出去了");

    /* ① 它是一条**普通任务**, 只是属性不同。 */
    struct edc_task_group *sg = edc_session_find_system_group(session);
    CHECK(sg != NULL, "出现了系统组");
    CHECK(edc_task_group_is_system(sg), "它就是系统组");
    CHECK(sg->member_count == 1, "里面正好一个任务");
    struct edc_task *fold = sg->members;
    CHECK(fold != NULL, "折叠任务出现了");
    CHECK(fold->sink == EDC_SINK_GROUP_MEMORY, "sink = GROUP_MEMORY (产出写记忆, 不回话)");
    CHECK(fold->sink_group == group, "知道产出该写进哪个组");
    CHECK(fold->fold_max_seq == 2, "知道本轮折到哪个水位");
    CHECK(edc_task_awaits_exec(fold), "已派发 (在等 LLM 回话)");
    CHECK(fold->active_turn != NULL, "带着它的 input (折叠 prompt)");
    /* 系统组的共用头部 = 折叠说明 (等价于旧实现现拼的那条 system 消息)。 */
    CHECK(cJSON_GetArraySize(sg->history) == 1, "系统组头部只有一条 system prompt");
    if (cJSON_GetArraySize(sg->history) == 1)
    {
        cJSON *sys = cJSON_GetArrayItem(sg->history, 0);
        cJSON *content = cJSON_GetObjectItem(sys, "content");
        CHECK(content && cJSON_IsString(content) && strstr(content->valuestring, "memory summarizer") != NULL,
              "它就是折叠说明");
    }

    /* 该 session 已有折叠在飞: 不再起第二次 (免得两次折叠看见半成品记忆)。 */
    CHECK(edc_scheduler_has_pending_system_task(session), "有折叠在飞");
    CHECK(edc_group_fold_start(sched, session, group) == NULL, "有折叠在飞时不起第二次");

    /* ② 跑完它: LLM 返回摘要 → exec_done → sink 分流 → 落地。 */
    struct edc_llm_actor_resp resp;
    memset(&resp, 0, sizeof(resp));
    resp.reason = BC_LLM_FINISH_REASON_STOP;
    resp.content = sdsnew("# Merged memory\n- folded");
    void *fold_req = fold->current_request_id;
    struct edc_advance_result fold_res = edc_executor_advance_llm_resp(&sched->executor, fold, fold_req, &resp);
    /* executor 只报事实; 告诉谁 在策略点 (契约见 edc_scheduler.h)。 */
    edc_scheduler_apply_advance(sched, fold, fold_req, &fold_res);

    /* The fold task is not itself foldable (a regression fixture once chained
     * "fold completed -> system group triggers -> another fold"). Asserted after it
     * completes, the exact state the fixture hit; only sink holds it out. */
    CHECK(!edc_task_is_foldable(fold), "折叠任务本身不可折叠 (sink=GROUP_MEMORY 不进折叠账)");
    CHECK(edc_task_group_count_foldable(sg) == 0, "系统组里没有可折叠任务");
    CHECK(edc_task_group_unfolded_msg_count(sg) == 0, "系统组的消息也不算进折叠阈值");
    CHECK(!edc_group_fold_due(sg, 2), "系统组永远不会触发折叠");
    CHECK(!edc_group_fold_due(sg, 1), "阈值再低也不折系统组");

    CHECK(group->memory != NULL && strstr(group->memory, "# Merged memory") != NULL, "记忆被写进目标组");
    CHECK(count_folded(group) == 2, "只有被选中的那两个 (leader + task 2) 标了已折叠");
    CHECK(leader->turns_folded == true, "最老的 leader 被折了");
    CHECK(group->folded_through_completion_seq == 2 && seq_before == 0, "水位推到 2");

    /* ③ 不污染被折叠的组 (规则 ④): 折叠任务不在这个组里, 它的消息也不进这个组的
     * history —— 这里能动的只有"哪批消息算已折叠"。 */
    CHECK(cJSON_GetArraySize(group->history) == group_hist_before, "折叠任务的消息不进被折叠组的 history");
    CHECK(group->member_count == 4, "折叠任务不在被折叠的组里 (它在系统组)");
    CHECK(edc_task_group_unfolded_msg_count(group) == group_msgs_before - 5,
          "该组的未折叠消息数只少了被折的那 5 条 (其余照旧)");
    CHECK(fold->completion_seq == 0, "折叠任务不占 session 的完成序号 (不是用户的完成次序里的一环)");
    CHECK(group->folded_through_completion_seq == 2, "水位照旧只由被折的任务决定");

    /* ④ 不回收: 折叠任务是普通任务, 完成后留在系统组。 */
    CHECK(count_tasks_in(session) == 5, "折叠任务完成后仍在 (不回收)");
    CHECK(edc_session_find_system_group(session) != NULL, "系统组还在");

    /* ⑤ 再折一次: 可折叠的还剩 3 个 → 还能起。 */
    CHECK(edc_group_fold_due(group, 4), "还有可折叠的 (seq 3/4)");
    uint32_t seq2 = 0;
    sds prompt2 = edc_group_fold_build_prompt(group, &seq2);
    CHECK(prompt2 && seq2 == 3, "第二轮折到 seq 3 (max_seq 推进: 只剩 seq 3/4 可选)");
    if (prompt2)
    {
        CHECK(strstr(prompt2, "- folded") != NULL, "第二轮把上一轮的摘要带上 (合并语义)");
        sdsfree(prompt2);
    }

    sdsfree(resp.content);
    edc_session_free(session);
    edc_scheduler_free(sched);
    edc_config_free(cfg);
}

/* ══════════════════════════════════════════════════════════════
 *  Test 3: 失败不落地 (记忆与水位原样), 任务照样回收
 *  ══════════════════════════════════════════════════════════════ */

static void test_fold_failure_keeps_memory(void)
{
    TEST("折叠失败: 记忆与水位原样, 折叠任务照常回收 (下一轮再折)");

    struct edc_config *cfg = make_config();
    struct edc_session *session = edc_session_new(cfg, "sess-fail", "Fail");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, session);
    sched->send_llm = stub_send_llm;
    g_send_llm_calls = 0;

    struct edc_task *leader = edc_task_new("1", "sess-fail", "1", "leader", edc_channel_adapter);
    struct edc_task_group *group = edc_task_group_new(leader);
    edc_session_add_group(session, group);
    edc_task_await_exec(leader);
    edc_task_mark_completed(leader);
    leader->completion_seq = 1;
    struct edc_turn *lturn = edc_turn_new();
    cJSON *lm = cJSON_CreateObject();
    cJSON_AddStringToObject(lm, "role", "user");
    cJSON_AddStringToObject(lm, "content", "leader work");
    edc_queue_push(&lturn->messages, lm);
    edc_task_add_turn(leader, lturn);
    add_foldable_task(session, group, "2", 2, 6, "work");
    group->memory = sdsnew("# memory before");

    CHECK(edc_group_fold_start(sched, session, group) != NULL, "起了一次折叠");
    pump_schedule(sched);
    struct edc_task *fold = edc_session_find_system_group(session)->members;

    /* 空的 LLM 回复 (旧实现里这也算失败) */
    struct edc_llm_actor_resp resp;
    memset(&resp, 0, sizeof(resp));
    resp.reason = BC_LLM_FINISH_REASON_STOP;
    resp.content = NULL;
    void *fold_req = fold->current_request_id;
    struct edc_advance_result fold_res = edc_executor_advance_llm_resp(&sched->executor, fold, fold_req, &resp);
    /* executor 只报事实; 告诉谁 在策略点 (契约见 edc_scheduler.h)。 */
    edc_scheduler_apply_advance(sched, fold, fold_req, &fold_res);

    CHECK(strcmp(group->memory, "# memory before") == 0, "记忆没被改");
    CHECK(count_folded(group) == 0, "没有任何任务被标已折叠");
    CHECK(group->folded_through_completion_seq == 0, "水位没动");
    CHECK(edc_group_fold_due(group, 4), "下一轮还会再折 (可折叠的还在)");

    CHECK(count_tasks_in(session) == 3, "失败的折叠任务仍在 (普通任务, 不回收)");
    CHECK(edc_session_find_system_group(session) != NULL, "系统组还在");

    edc_session_free(session);
    edc_scheduler_free(sched);
    edc_config_free(cfg);
}

/* ══════════════════════════════════════════════════════════════
 *  Test 4: 折叠任务**落盘** (重启可续跑)
 *  ══════════════════════════════════════════════════════════════ */

static void test_fold_task_is_persisted(void)
{
    TEST("折叠任务在跑的时候落盘: 盘上有它、有系统组; 读回后置回就绪");

    struct edc_config *cfg = make_config();
    struct edc_session *session = edc_session_new(cfg, "sess-disk", "Disk");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, session);
    sched->send_llm = stub_send_llm;
    g_send_llm_calls = 0;

    struct edc_task *leader = edc_task_new("1", "sess-disk", "1", "leader", edc_channel_adapter);
    struct edc_task_group *group = edc_task_group_new(leader);
    edc_session_add_group(session, group);
    edc_task_await_exec(leader);
    edc_task_mark_completed(leader);
    leader->completion_seq = 1;
    struct edc_turn *lturn = edc_turn_new();
    cJSON *lm = cJSON_CreateObject();
    cJSON_AddStringToObject(lm, "role", "user");
    cJSON_AddStringToObject(lm, "content", "leader work");
    edc_queue_push(&lturn->messages, lm);
    edc_task_add_turn(leader, lturn);
    add_foldable_task(session, group, "2", 2, 6, "work");

    struct edc_task *fold = edc_group_fold_start(sched, session, group);
    CHECK(fold != NULL, "折叠在飞");
    pump_schedule(sched);

    cJSON *j = edc_state_serialize(session, sched);
    CHECK(j != NULL, "可序列化");
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItem(j, "tasks")) == 3, "tasks 块含折叠任务 (落盘)");
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItem(j, "groups")) == 2, "groups 块含系统组 (落盘)");

    struct edc_session *s2 = edc_session_new(cfg, "sess-disk", "Disk");
    struct edc_scheduler *s2s = edc_scheduler_new(cfg);
    install_resolver(s2s, s2);
    CHECK(edc_state_deserialize(j, s2, s2s), "读回成功");
    struct edc_task_group *sg2 = edc_session_find_system_group(s2);
    CHECK(sg2 != NULL && edc_task_group_is_system(sg2), "系统组读回来");
    struct edc_task *fold2 = sg2 ? sg2->members : NULL;
    CHECK(fold2 != NULL, "折叠任务读回来");
    CHECK(fold2 && edc_task_is_ready(fold2) && edc_task_is_alive(fold2),
          "未完成的折叠任务读回后置回就绪 (重启续跑), 不是 SUSPENDED");
    CHECK(fold2 && fold2->sink_group && strcmp(fold2->sink_group->group_id, group->group_id) == 0,
          "sink_group 解析回目标组");

    cJSON_Delete(j);
    edc_session_free(session);
    edc_scheduler_free(sched);
    edc_session_free(s2);
    edc_scheduler_free(s2s);
    edc_config_free(cfg);
}

/* Test 5: the trigger scan (edc_group_fold_try), the only entry the scheduler calls.
 * Pins: it scans all sessions, starts one fold at a time, and takes the threshold
 * from config (default otherwise). */

static void test_try_scans_all_sessions(void)
{
    TEST("触发扫描: 扫所有 session / 一次一个 / 阈值来自 config");

    struct edc_config *cfg = make_config();
    cfg->group_fold_msg_threshold = 2; /* 低阈值: 一个 4 条消息的可折叠 task 就够 */

    /* 第一个 session: 没有可折叠内容 (扫到它 → 不折) */
    struct edc_session *idle = edc_session_new(cfg, "sess-try-idle", "TryIdle");
    struct edc_task *ileader = edc_task_new("11", "sess-try-idle", "11", "idle leader", edc_channel_adapter);
    struct edc_task_group *igroup = edc_task_group_new(ileader);
    edc_session_add_group(idle, igroup);
    edc_task_await_exec(ileader);
    edc_task_mark_completed(ileader);

    /* 第二个 session: 可折叠的内容只在这里 —— 只有"扫全部"才会命中 */
    struct edc_session *busy = edc_session_new(cfg, "sess-try-busy", "TryBusy");
    struct edc_task *bleader = edc_task_new("21", "sess-try-busy", "21", "busy leader", edc_channel_adapter);
    struct edc_task_group *bgroup = edc_task_group_new(bleader);
    edc_session_add_group(busy, bgroup);
    edc_task_await_exec(bleader);
    edc_task_mark_completed(bleader);
    bleader->completion_seq = 1;
    add_foldable_task(busy, bgroup, "22", 2, 4, "foldable in the second session");

    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, idle);
    sched->send_llm = stub_send_llm;

    /* 仓库只被读两个字段 (session_count / sessions[]) → 栈上搭一个, 不碰磁盘 */
    struct edc_session *arr[2] = {idle, busy};
    struct edc_session_manager sm = {0};
    sm.sessions = arr;
    sm.session_count = 2;
    sm.active_index = 0;
    sm.config = cfg;

    /* 调度器看到的仓库 = 同一个 (它按这个枚举任务: 折叠任务落在 busy, 不在 idle)。
     * 这两行从前靠 "fold 直接 send_llm" 掩盖 —— 现在"跑"要经调度, 仓库必须是真那份。 */
    sched->sessions = &sm;

    CHECK(!edc_group_fold_try(NULL, &sm, cfg), "NULL scheduler 不折");
    CHECK(!edc_group_fold_try(sched, NULL, cfg), "NULL 仓库不折");

    /* ③ 阈值: conf=NULL → 默认 (500) → 这个规模的组折不动 (此时无折叠在飞) */
    g_send_llm_calls = 0;
    CHECK(!edc_group_fold_try(sched, &sm, NULL), "conf=NULL 用默认阈值 → 不折");
    CHECK(g_send_llm_calls == 0, "默认阈值下没有 LLM 请求");

    /* ① 扫所有 session: 第二个 session 里的组被折 */
    g_send_llm_calls = 0;
    CHECK(edc_group_fold_try(sched, &sm, cfg), "第二个 session 里的组被折了 (扫全部)");
    pump_schedule(sched);
    CHECK(g_send_llm_calls == 1, "折叠 prompt 真发出去了一次 LLM 请求");
    CHECK(edc_scheduler_has_pending_system_task(busy), "busy 里已有折叠在飞");

    /* ② 一次一个: 在飞的那个 session 跳过, 不再起第二次 */
    g_send_llm_calls = 0;
    CHECK(!edc_group_fold_try(sched, &sm, cfg), "有折叠在飞 → 不再起第二次");
    CHECK(g_send_llm_calls == 0, "第二次没有 LLM 请求");

    edc_session_free(idle);
    edc_session_free(busy);
    edc_scheduler_free(sched);
    edc_config_free(cfg);
}

/* ═══════════════════════════════════════════════════════
 *  Test 6: 折叠是一次屏障 (barrier) —— 折叠不让路, 组内成员挂在它上面等
 *  ═══════════════════════════════════════════════════════ */

/* 建一个"该折"的组骨架: leader (seq1, 一条 user 消息)。 */
static struct edc_task_group *make_foldable_group(struct edc_session *session, const char *sid, const char *leader_text)
{
    struct edc_task *leader = edc_task_new("1", sid, "1", leader_text, edc_channel_adapter);
    struct edc_task_group *group = edc_task_group_new(leader);
    edc_session_add_group(session, group);
    edc_task_await_exec(leader);
    edc_task_mark_completed(leader);
    leader->completion_seq = 1;
    struct edc_turn *lturn = edc_turn_new();
    cJSON *lm = cJSON_CreateObject();
    cJSON_AddStringToObject(lm, "role", "user");
    cJSON_AddStringToObject(lm, "content", leader_text);
    edc_queue_push(&lturn->messages, lm);
    edc_task_add_turn(leader, lturn);
    return group;
}

static void test_fold_preempts_running_member(void)
{
    TEST("折叠前置: 在飞的成员不被 stop; dep 挂上, 到它的下一个请求边界才生效");

    struct edc_config *cfg = make_config();
    cfg->group_fold_msg_threshold = 1000;
    struct edc_session *session = edc_session_new(cfg, "sess-preempt", "Preempt");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, session);
    sched->send_llm = stub_send_llm;
    g_send_llm_calls = 0;

    struct edc_task_group *group = make_foldable_group(session, "sess-preempt", "leader work");

    /* 一个在飞的成员: LLM 已发, 正在等回包 (AWAIT_EXEC)。 */
    struct edc_task *run = edc_task_new("9", "sess-preempt", group->group_id, "running", edc_channel_adapter);
    edc_task_group_add_task(group, run);
    edc_scheduler_attach_input(sched, run, "in flight");
    edc_task_await_exec(run);
    CHECK(edc_task_is_running(run), "成员在跑");

    struct edc_task *fold = edc_group_fold_start(sched, session, group);
    CHECK(fold != NULL, "有成员在跑也起折叠 (不再推迟)");
    CHECK(edc_task_is_running(run), "在飞的成员**不被 stop** (这一轮已发出去, 拦不住)");
    CHECK(!edc_task_awaits_deps(run), "它也没被立刻 park");
    bool has_dep = false;
    struct edc_task_dep *d;
    LL_FOREACH(run->deps, d)
    if (d->source == fold)
        has_dep = true;
    CHECK(has_dep, "但 dep 边已经挂上 (到它的下一个请求边界生效)");

    pump_schedule(sched);
    CHECK(g_send_llm_calls == 1, "只发出折叠的请求");
    CHECK(g_last_llm_task_id && strcmp(g_last_llm_task_id, fold->task_id) == 0, "发的是折叠");

    edc_session_free(session);
    edc_scheduler_free(sched);
    edc_config_free(cfg);
}

static void test_fold_holds_next_llm_send(void)
{
    TEST("B1: 工具跑完后, 有未决 dep 的成员拒绝发下一个 LLM 请求, park 成 AWAIT_DEPS");

    struct edc_config *cfg = make_config();
    cfg->group_fold_msg_threshold = 1000;
    struct edc_session *session = edc_session_new(cfg, "sess-sendhold", "SendHold");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, session);
    sched->send_llm = stub_send_llm;
    g_send_llm_calls = 0;

    struct edc_task_group *group = make_foldable_group(session, "sess-sendhold", "leader work");

    /* 一个成员: 工具链正在跑 (AWAIT_TOOLS + 账本一条)。 */
    struct edc_task *m = edc_task_new("8", "sess-sendhold", group->group_id, "worker", edc_channel_adapter);
    edc_task_group_add_task(group, m);
    edc_scheduler_attach_input(sched, m, "do work");
    edc_task_await_exec(m);
    edc_task_await_tools(m);
    edc_task_tool_call_add(m, "c1", "echo", NULL);
    void *rid = (void *)0x1234;
    m->current_request_id = rid;
    CHECK(edc_task_is_running(m), "成员的工具链在跑");

    struct edc_task *fold = edc_group_fold_start(sched, session, group);
    CHECK(fold != NULL, "折叠起来了");
    CHECK(edc_task_awaits_tools(m), "成员仍在 AWAIT_TOOLS (工具已跑, 不打断)");

    /* 最后一个工具回包: executor 本该发下一个 LLM; 但 dep 未决 → park。 */
    struct edc_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.kind = EDC_EVENT_TOOL_DONE;
    ev.request_id = (const char *)rid;
    ev.as.tool_done.call_id = "c1";
    ev.as.tool_done.name = "echo";
    ev.as.tool_done.result = sdsnew("out");
    ev.as.tool_done.has_next = false;
    struct edc_advance_result res = edc_executor_advance(&sched->executor, m, &ev);
    edc_scheduler_apply_advance(sched, m, rid, &res);

    CHECK(g_send_llm_calls == 0, "没有发出 LLM 请求 (被 dep 拦下)");
    CHECK(edc_task_awaits_deps(m), "被 park 到 AWAIT_DEPS");

    /* 表里没动, 扫描先驱动折叠 (m 在 AWAIT_DEPS, 不会被驱动)。 */
    pump_schedule(sched);
    CHECK(g_send_llm_calls == 1, "被挂住的成员没有被驱动");
    CHECK(g_last_llm_task_id && strcmp(g_last_llm_task_id, fold->task_id) == 0, "只驱动了折叠");

    /* 折叠落地 → 释放成员 → 扫描驱动它发请求。 */
    struct edc_llm_actor_resp resp;
    memset(&resp, 0, sizeof(resp));
    resp.reason = BC_LLM_FINISH_REASON_STOP;
    resp.content = sdsnew("# merged");
    void *fold_req = fold->current_request_id;
    struct edc_advance_result fr = edc_executor_advance_llm_resp(&sched->executor, fold, fold_req, &resp);
    edc_scheduler_apply_advance(sched, fold, fold_req, &fr);
    CHECK(edc_task_is_ready(m), "折叠落地后成员被释放 (READY)");

    pump_schedule(sched);
    CHECK(g_send_llm_calls == 2, "现在为成员发出请求");
    CHECK(g_last_llm_task_id && strcmp(g_last_llm_task_id, m->task_id) == 0, "发的是那个成员");

    sdsfree(resp.content);
    edc_session_free(session);
    edc_scheduler_free(sched);
    edc_config_free(cfg);
}

static void test_group_held_until_fold_lands(void)
{
    TEST("屏障: 折叠在飞时组内成员停在 AWAIT_DEPS, 折叠结算后被释放并驱动");

    struct edc_config *cfg = make_config();
    cfg->group_fold_msg_threshold = 1000;
    struct edc_session *session = edc_session_new(cfg, "sess-hold", "Hold");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, session);
    sched->send_llm = stub_send_llm;
    g_send_llm_calls = 0;

    struct edc_task_group *group = make_foldable_group(session, "sess-hold", "leader work");

    struct edc_task *held = edc_task_new("7", "sess-hold", group->group_id, "held", edc_channel_adapter);
    edc_task_group_add_task(group, held);
    edc_scheduler_attach_input(sched, held, "queued work");
    CHECK(edc_task_is_ready(held), "held 就绪、有 turn");

    struct edc_task *fold = edc_group_fold_start(sched, session, group);
    CHECK(fold != NULL, "折叠起来了");
    CHECK(edc_task_awaits_deps(held), "成员被挂成 AWAIT_DEPS (wait dep)");

    pump_schedule(sched);
    CHECK(g_send_llm_calls == 1, "只发出折叠自己的请求");
    CHECK(edc_task_awaits_deps(held), "成员仍等依赖 (没被驱动)");

    struct edc_llm_actor_resp resp;
    memset(&resp, 0, sizeof(resp));
    resp.reason = BC_LLM_FINISH_REASON_STOP;
    resp.content = sdsnew("# merged");
    void *fold_req = fold->current_request_id;
    struct edc_advance_result fr = edc_executor_advance_llm_resp(&sched->executor, fold, fold_req, &resp);
    edc_scheduler_apply_advance(sched, fold, fold_req, &fr);
    CHECK(group->memory && strstr(group->memory, "# merged") != NULL, "记忆已写入");
    CHECK(edc_task_is_ready(held), "成员被释放 (deps 完成 → READY)");

    pump_schedule(sched);
    CHECK(g_send_llm_calls == 2, "折叠落地后成员被驱动");
    CHECK(g_last_llm_task_id && strcmp(g_last_llm_task_id, held->task_id) == 0, "驱动的是被挂住的成员");
    CHECK(edc_task_awaits_exec(held), "成员现在在跑");

    sdsfree(resp.content);
    edc_session_free(session);
    edc_scheduler_free(sched);
    edc_config_free(cfg);
}

static void test_resume_held_while_fold_in_flight(void)
{
    TEST("屏障: 折叠在飞时 resume 一个不在 dep 里的成员, 被闸门扣在 READY");

    struct edc_config *cfg = make_config();
    cfg->group_fold_msg_threshold = 1000;
    struct edc_session *session = edc_session_new(cfg, "sess-resume", "Resume");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, session);
    sched->send_llm = stub_send_llm;
    g_send_llm_calls = 0;

    struct edc_task_group *group = make_foldable_group(session, "sess-resume", "leader work");

    /* 起折时它是终态 (CANCELLED), 所以没被挂 dep; 之后 resume。 */
    struct edc_task *t = edc_task_new("7", "sess-resume", group->group_id, "resume me", edc_channel_adapter);
    edc_task_group_add_task(group, t);
    edc_scheduler_attach_input(sched, t, "resume me");
    edc_task_mark_cancelled(t);
    CHECK(edc_task_is_cancelled(t), "t 是 CANCELLED");

    struct edc_task *fold = edc_group_fold_start(sched, session, group);
    CHECK(fold != NULL, "折叠起来了");
    pump_schedule(sched);
    CHECK(g_send_llm_calls == 1, "只发了折叠");

    edc_scheduler_resume_task(sched, t);
    CHECK(edc_task_is_ready(t), "resume 被闸门扣在 READY (没被驱动)");
    CHECK(g_send_llm_calls == 1, "没有为 t 发请求");

    struct edc_llm_actor_resp resp;
    memset(&resp, 0, sizeof(resp));
    resp.reason = BC_LLM_FINISH_REASON_STOP;
    resp.content = sdsnew("# merged");
    void *fold_req = fold->current_request_id;
    struct edc_advance_result fr = edc_executor_advance_llm_resp(&sched->executor, fold, fold_req, &resp);
    edc_scheduler_apply_advance(sched, fold, fold_req, &fr);

    pump_schedule(sched);
    CHECK(edc_task_awaits_exec(t), "折叠落地后 resume 生效 (t 在跑)");
    CHECK(g_send_llm_calls == 2, "为 t 发出了请求");

    sdsfree(resp.content);
    edc_session_free(session);
    edc_scheduler_free(sched);
    edc_config_free(cfg);
}


/* ══════════════════════════════════════════════════════════════ */

int main(void)
{
    test_prompt_and_due();
    test_fold_runs_as_a_system_task();
    test_fold_failure_keeps_memory();
    test_fold_task_is_persisted();
    test_try_scans_all_sessions();
    test_fold_preempts_running_member();
    test_fold_holds_next_llm_send();
    test_group_held_until_fold_lands();
    test_resume_held_while_fold_in_flight();

    log_info("=== %d passed, %d failed ===", test_passed, test_failed);
    printf("%d passed, %d failed\n", test_passed, test_failed);
    return test_failed == 0 ? 0 : 1;
}
