/* ══════════════════════════════════════════════════════════════
 *  P0b: fold 迁移为 system task
 *
 *  旧实现: scheduler 回调 → memory_actor (专用 actor) → 自己发 LLM 请求 →
 *  BC_ACTOR_MSG_GROUP_FOLD_DONE → scheduler 落盘。专用协议 + 专用 actor。
 *
 *  新实现: 触发方建一个 **system task**
 *    { sink = GROUP_MEMORY, lifetime = TRANSIENT }   —— 放进 session 的 system group
 *  它的 input 就是折叠 prompt; 产出经 executor 按 sink 分流到
 *  ops->deliver_group_memory, 落地在 scheduler: 写 memory、标已折叠、推水位。
 *  跑完按 lifetime 被回收, 组空了连组一起删。
 *
 *  本文件钉住三件事:
 *    ① 折叠行为本身**没有变** (prompt 构造逐字来自旧 memory_actor);
 *    ② 它确实是一条普通任务 (属性、调度、回收、可见);
 *    ③ 它不污染被折叠的组 (消息不进那个组的 history / 不占它的 completion_seq)。
 * ══════════════════════════════════════════════════════════════ */

#include "edc_agent.h"
#include "edc_compat.h"
#include "edc_config.h"
#include "edc_executor.h"
#include "edc_group_fold.h"
#include "edc_llm_actor.h"
#include "edc_scheduler.h"
#include "edc_session.h"
#include "edc_state_serializer.h"
#include "edc_task.h"
#include "edc_task_group.h"
#include "edc_turn.h"
#include "log.h"
#include "utlist_fix.h"
#include <signal.h>
#include <stdio.h>
#include <string.h>

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

/* 最小 agent: edc_group_fold_start 只用到 agent->scheduler (触发方还会用
 * session_manager/config, 但那条路是 shell/agent 侧的, 这里测的是折叠本体)。
 * 用真结构体而不是 mock —— 它就是个普通结构, 没有构造函数。 */
static struct edc_agent *make_agent(struct edc_scheduler *sched)
{
    struct edc_agent *agent = g_new0(struct edc_agent, 1);
    agent->scheduler = sched;
    sched->ctx = agent;
    return agent;
}

/* 建一组"已完成、带 turn"的任务 (可折叠), 每个 turn 装 `msgs_per_task` 条消息。 */
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
static void stub_send_llm(struct edc_scheduler *sched, struct edc_task *task, void *request_id)
{
    (void)sched;
    (void)task;
    (void)request_id;
    g_send_llm_calls++;
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
    struct edc_scheduler *sched = edc_scheduler_new(session, cfg);
    struct edc_agent *agent = make_agent(sched);
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

    CHECK(edc_group_fold_start(agent, session, group) != NULL, "折叠任务起来了");
    CHECK(g_send_llm_calls == 1, "折叠 prompt 作为一次普通 LLM 请求发出去了");

    /* ① 它是一条**普通任务**, 只是三个属性不同。 */
    struct edc_task_group *sg = edc_session_find_system_group(session);
    CHECK(sg != NULL, "出现了系统组");
    CHECK(edc_task_group_is_system(sg), "它就是系统组");
    CHECK(sg->member_count == 1, "里面正好一个任务");
    struct edc_task *fold = sg->members;
    CHECK(fold != NULL && edc_task_is_transient(fold), "折叠任务是 TRANSIENT (不落盘)");
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
    CHECK(edc_group_fold_start(agent, session, group) == NULL, "有折叠在飞时不起第二次");


    /* ② 跑完它: LLM 返回摘要 → exec_done → sink 分流 → 落地。 */
    struct edc_llm_actor_resp resp;
    memset(&resp, 0, sizeof(resp));
    resp.reason = BC_LLM_FINISH_REASON_STOP;
    resp.content = sdsnew("# Merged memory\n- folded");
    edc_executor_advance_llm_resp(&sched->executor, fold, fold->current_request_id, &resp);

    /* 折叠任务**自己**不是可折叠对象 (回归: 夹具实测过一次"折叠任务完成 → 触发
     * 折叠系统组 → 又起一个折叠任务"的连锁)。折叠账只为留下来的任务做 (见
     * edc_task_is_foldable), 所以系统组永远不够触发。
     *
     * 断言必须放在**它完成之后**: 那一刻它已经是 COMPLETED、turn 也已提交, 正是
     * 夹具里出事的状态 —— 只有 lifetime 这一条挡得住 (它还没被回收, 因为回收要等
     * 本条消息末尾)。 */
    CHECK(!edc_task_is_foldable(fold), "折叠任务本身不可折叠 (TRANSIENT 不进折叠账)");
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

    /* ④ 完成即回收: 结算点之后它没了, 系统组也空了。 */
    edc_scheduler_drain_reap_pending(sched);
    CHECK(count_tasks_in(session) == 4, "只剩被折叠组的 4 个任务 (折叠任务已回收)");
    CHECK(edc_session_find_system_group(session) == NULL, "系统组空了就被删掉");

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
    free(agent);
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
    struct edc_scheduler *sched = edc_scheduler_new(session, cfg);
    struct edc_agent *agent = make_agent(sched);
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

    CHECK(edc_group_fold_start(agent, session, group) != NULL, "起了一次折叠");
    struct edc_task *fold = edc_session_find_system_group(session)->members;

    /* 空的 LLM 回复 (旧实现里这也算失败) */
    struct edc_llm_actor_resp resp;
    memset(&resp, 0, sizeof(resp));
    resp.reason = BC_LLM_FINISH_REASON_STOP;
    resp.content = NULL;
    edc_executor_advance_llm_resp(&sched->executor, fold, fold->current_request_id, &resp);

    CHECK(strcmp(group->memory, "# memory before") == 0, "记忆没被改");
    CHECK(count_folded(group) == 0, "没有任何任务被标已折叠");
    CHECK(group->folded_through_completion_seq == 0, "水位没动");
    CHECK(edc_group_fold_due(group, 4), "下一轮还会再折 (可折叠的还在)");

    edc_scheduler_drain_reap_pending(sched);
    CHECK(count_tasks_in(session) == 2, "失败的折叠任务照样被回收 (不留下半成品)");
    CHECK(edc_session_find_system_group(session) == NULL, "系统组也没了");

    edc_session_free(session);
    edc_scheduler_free(sched);
    free(agent);
    edc_config_free(cfg);
}

/* ══════════════════════════════════════════════════════════════
 *  Test 4: 折叠不落盘 (重启无残留)
 *  ══════════════════════════════════════════════════════════════ */

static void test_fold_task_is_not_persisted(void)
{
    TEST("折叠任务在跑的时候落盘: 盘上没有它, 也没有系统组");

    struct edc_config *cfg = make_config();
    struct edc_session *session = edc_session_new(cfg, "sess-disk", "Disk");
    struct edc_scheduler *sched = edc_scheduler_new(session, cfg);
    struct edc_agent *agent = make_agent(sched);
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

    /* 对照组: 没有折叠时的产物 */
    struct edc_session *ctrl = edc_session_new(cfg, "sess-disk", "Disk");
    struct edc_scheduler *ctrl_sched = edc_scheduler_new(ctrl, cfg);
    ctrl_sched->send_llm = stub_send_llm;
    struct edc_task *cleader = edc_task_new("1", "sess-disk", "1", "leader", edc_channel_adapter);
    struct edc_task_group *cgroup = edc_task_group_new(cleader);
    edc_session_add_group(ctrl, cgroup);
    edc_task_await_exec(cleader);
    edc_task_mark_completed(cleader);
    cleader->completion_seq = 1;
    struct edc_turn *cturn = edc_turn_new();
    cJSON *cm = cJSON_CreateObject();
    cJSON_AddStringToObject(cm, "role", "user");
    cJSON_AddStringToObject(cm, "content", "leader work");
    edc_queue_push(&cturn->messages, cm);
    edc_task_add_turn(cleader, cturn);
    add_foldable_task(ctrl, cgroup, "2", 2, 6, "work");

    CHECK(edc_group_fold_start(agent, session, group) != NULL, "折叠在飞");

    cJSON *j_with = edc_state_serialize(session, sched);
    cJSON *j_ctrl = edc_state_serialize(ctrl, ctrl_sched);
    char *tw = cJSON_PrintUnformatted(j_with);
    char *tc = cJSON_PrintUnformatted(j_ctrl);
    CHECK(tw && tc && strcmp(tw, tc) == 0, "产物与没有折叠时逐字节相同 (系统组/折叠任务都不落盘)");
    if (tw)
        cJSON_free(tw);
    if (tc)
        cJSON_free(tc);
    cJSON_Delete(j_with);
    cJSON_Delete(j_ctrl);

    edc_session_free(session);
    edc_scheduler_free(sched);
    edc_session_free(ctrl);
    edc_scheduler_free(ctrl_sched);
    free(agent);
    edc_config_free(cfg);
}

/* ══════════════════════════════════════════════════════════════ */

int main(void)
{
    test_prompt_and_due();
    test_fold_runs_as_a_system_task();
    test_fold_failure_keeps_memory();
    test_fold_task_is_not_persisted();

    log_info("=== %d passed, %d failed ===", test_passed, test_failed);
    printf("%d passed, %d failed\n", test_passed, test_failed);
    return test_failed == 0 ? 0 : 1;
}
