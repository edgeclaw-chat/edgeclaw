/* The fold runs as a system task { sink = GROUP_MEMORY } in the session's system
 * group -- an ordinary persisted task, not a hidden/transient one. Its input is the
 * fold prompt; the output lands in the group's memory.
 * This file pins: the fold behaviour, the barrier (group tasks held by a real dep
 * until the fold lands), and that it does not pollute the folded group. */

#include "edc_compat.h"
#include "edc_config.h"
#include "edc_context.h"
#include "edc_executor.h"
#include "edc_group_fold.h"
#include "edc_llm_actor.h"
#include "edc_scheduler.h"
#include "edc_session.h"
#include "edc_session_manager.h"
#include "edc_state_serializer.h"
#include "edc_task.h"
#include "edc_task_group.h"
#include "edc_token_estimate.h"
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

/* `edc_group_fold_due` 只有一条轴 (token, 2026-10-10 起: 消息条数轴整条退休):
 * 组的上下文 vs 窗口 - reserve。这三个助手让用例只说"过线 / 没过线", 不手算窗口;
 * cfg_win 供"特别宽 / 不是正数"这类边界用例直接用。 */
static struct edc_config *cfg_win(struct edc_config *cfg, int window)
{
    cfg->context_window = window;
    return cfg;
}

/* 窗口刚好让这个组"该折" (ctx > window - reserve 严格成立)。 */
static struct edc_config *cfg_due(struct edc_config *cfg, struct edc_task_group *group)
{
    long long ctx = edc_context_group_ctx_tokens(group, NULL);
    return cfg_win(cfg, (int)(ctx + EDC_FOLD_RESERVE_TOKENS - 1));
}

/* 窗口刚好让它"不该折" (ctx == window - reserve, 不严格大于)。 */
static struct edc_config *cfg_not_due(struct edc_config *cfg, struct edc_task_group *group)
{
    long long ctx = edc_context_group_ctx_tokens(group, NULL);
    return cfg_win(cfg, (int)(ctx + EDC_FOLD_RESERVE_TOKENS));
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
        if (edc_task_has_folded_turns(t))
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

    /* 先给这个组一份旧记忆: 它同时是 (a) token 估算的一个来源 —— 让 ctx 有非零值,
     * 好把"窗口 vs reserve"那个不等式两侧各钉一次, 和 (b) 后面迭代 prompt 的输入。 */
    group->memory = sdsnew("# Old memory\n- something remembered");

    /* 判据只有 token 轴。窗口宽 → 不折 (哪怕组里消息再多: 见本函数末尾的 500 条用例)。 */
    CHECK(!edc_group_fold_due(group, cfg_win(cfg, 1000000)), "窗口 1M: 这段小组远没到线 → 不折");
    CHECK(!edc_group_fold_due(NULL, cfg_win(cfg, 1000000)), "NULL 组不折");
    /* 窗口读不到 (脏 config / 有人手改) → 不自动折; "不知道窗口"不是"随便折"的理由。
     * 手动 /group fold 不过这道门。 */
    CHECK(!edc_group_fold_due(group, cfg_win(cfg, 0)), "窗口 0 → 不自动折");
    CHECK(!edc_group_fold_due(group, cfg_win(cfg, -1)), "负窗口 → 不自动折");
    /* 判据就是那一个不等式, 两侧各钉一次 (off-by-one 会在这里红)。 */
    {
        long long ctx = edc_context_group_ctx_tokens(group, NULL);
        CHECK(ctx > 0, "组上下文有估算值 (前置)");
        CHECK(!edc_group_fold_due(group, cfg_win(cfg, (int)ctx + EDC_FOLD_RESERVE_TOKENS)),
              "ctx == window - reserve → 还没过线");
        CHECK(edc_group_fold_due(group, cfg_win(cfg, (int)ctx + EDC_FOLD_RESERVE_TOKENS - 1)),
              "ctx 超过 window - reserve → 折");
    }

    group->memory = sdsnew("# Old memory\n- something remembered");

    /* 再加三个已完成任务: seq 2,3,4,各 4 条消息 → 未折叠 = 4*3 + leader的1 = 13 */
    add_foldable_task(session, group, "2", 2, 4, "task two");
    add_foldable_task(session, group, "3", 3, 4, "task three");
    add_foldable_task(session, group, "4", 4, 4, "task four");

    size_t unfolded = edc_task_group_unfolded_msg_count(group);
    CHECK(unfolded == 13, "未折叠消息数 = 13");

    struct edc_fold_cut cut;
    memset(&cut, 0, sizeof(cut));
    sds prompt = edc_group_fold_build_prompt(group, NULL, &cut);
    CHECK(prompt != NULL, "拿到 prompt");
    if (prompt)
    {
        CHECK(strstr(prompt, "<group-transcript>") != NULL && strstr(prompt, "</group-transcript>") != NULL,
              "语料包在 <group-transcript> 里 (pi §3)");
        CHECK(strstr(prompt, "<previous-summary>") != NULL && strstr(prompt, "</previous-summary>") != NULL,
              "有旧记忆 ⇒ 迭代更新: 旧摘要包在 <previous-summary> 里");
        CHECK(strstr(prompt, "# Old memory") != NULL, "旧记忆内容在内 (迭代的输入)");
        CHECK(strstr(prompt, "Output the complete updated summary, not a diff") != NULL,
              "迭代指令 (输出完整摘要, 不是 diff)");
        CHECK(strstr(prompt, "exactly these sections") == NULL, "迭代**不用**首次指令");
        CHECK(strstr(prompt, "[task 1]") != NULL && strstr(prompt, "[task 2]") != NULL,
              "最老的先压 (leader seq=1 与 task 2 都在)");
        CHECK(strstr(prompt, "output: task two") != NULL, "整条进语料的成员带 output (JOIN 语义)");
        /* 小组 (整组还不到 keep_recent 20k) 退化成"压最老一半": 13 条 → 留最新 6 条,
         * 语料 = 前 7 条 = leader(1) + task 2(4) + task 3 的前 2 条。切点落在 task 3
         * 的 turn **内部** —— 这就是 split turn (消息级水位, 不是整 turn)。 */
        CHECK(cut.task_id && strcmp(cut.task_id, "3") == 0, "切点落在 task 3");
        CHECK(cut.turn == 0, "task 3 的第 0 个 turn");
        CHECK(cut.prefix == 2, "折到该 turn 的第 2 条 (留 2 条当细节)");
        CHECK(strstr(prompt, "[task 3]") != NULL, "半折的成员也在语料里");
        CHECK(strstr(prompt, "output: task three") == NULL, "半折成员的 output 不进语料 (尾巴还在历史里)");
        CHECK(strstr(prompt, "[task 4]") == NULL, "切点之后的成员完全不动");
        CHECK(strstr(prompt, "### [task 2] task two") != NULL && strstr(prompt, "user: task two") != NULL,
              "语料按 ### [task id] 分节, 消息是 role: content");
        CHECK(strstr(prompt, "## Goal") == NULL, "迭代变体不复述六段格式 (只要求保持同样的段)");
        sdsfree(prompt);
    }

    /* 消息条数彻底没有发言权: 500 条未折叠消息 + 宽窗口 ⇒ 仍然不折。
     * (把任何"按条数触发"改回来都会在这里红 —— 阈值 <= 500 都拦得住。) */
    add_foldable_task(session, group, "9", 9, 500, "a very chatty task");
    CHECK(edc_task_group_unfolded_msg_count(group) >= 500, "前置: 未折叠消息 500 条");
    CHECK(!edc_group_fold_due(group, cfg_win(cfg, 1000000)), "500 条未折叠消息也压不动 1M 窗口");

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
    uint32_t epoch_before = group->fold_epoch;

    CHECK(edc_group_fold_start(sched, session, group, NULL) != NULL, "折叠任务起来了");
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
    CHECK(fold->fold_cut_task_id && strcmp(fold->fold_cut_task_id, "3") == 0, "知道切点落在哪个成员");
    CHECK(fold->fold_cut_turn == 0 && fold->fold_cut_prefix == 2, "以及切在它的第 0 个 turn 的第 2 条");
    CHECK(edc_task_awaits_exec(fold), "已派发 (在等 LLM 回话)");
    CHECK(fold->active_turn != NULL, "带着它的 input (折叠 prompt)");
    /* 系统组**不带**共享头部: 折叠说明随请求走 (edc_llm_request 用
     * edc_group_fold_system_prompt() 现拼, 见 tests/test_llm_request.c 的
     * "折叠请求是自足的")。从前它住这个组的 history[0], 而那个槽位不落盘 ——
     * 重启后加载期会把它填成主提示词, 摘要调用就背上了 EdgeClaw 的人设。 */
    CHECK(cJSON_GetArraySize(sg->history) == 0, "系统组没有共享 header (折叠说明随请求走)");

    /* 该 session 已有折叠在飞: 不再起第二次 (免得两次折叠看见半成品记忆)。 */
    CHECK(edc_scheduler_has_pending_system_task(session), "有折叠在飞");
    CHECK(edc_group_fold_start(sched, session, group, NULL) == NULL, "有折叠在飞时不起第二次");

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
    CHECK(!edc_group_fold_due(sg, cfg_win(cfg, 1000000)), "系统组永远不会触发折叠");
    CHECK(!edc_group_fold_due(sg, cfg_win(cfg, 1)), "窗口紧到 ctx 必然过线, 系统组仍不折 (排除靠 sink)");

    CHECK(group->memory != NULL && strstr(group->memory, "# Merged memory") != NULL, "记忆被写进目标组");
    CHECK(count_folded(group) == 3, "leader + task 2 整折, task 3 半折 (三个都带折叠前缀)");
    CHECK(edc_task_unfolded_msg_count(leader) == 0, "最老的 leader 整条进了语料 (没有未折叠消息)");
    CHECK(group->fold_epoch == epoch_before + 1, "折叠落地 bump 了 fold_epoch (旧锚失效)");

    /* ③ 不污染被折叠的组 (规则 ④): 折叠任务不在这个组里, 它的消息也不进这个组的
     * history —— 这里能动的只有"哪批消息算已折叠"。 */
    /* history 现在由 turns 派生 (折叠落地时重建): system prompt + 未折叠消息。
     * 旧断言 "== group_hist_before" 在这个夹具里是空转的 —— add_foldable_task
     * 从不往 history 里写, 两边都是 0。 */
    CHECK(cJSON_GetArraySize(group->history) == 1 + edc_task_group_unfolded_msg_count(group),
          "history = system prompt + 未折叠消息 (被折的 5 条已剪掉; 折叠任务的消息也不进来)");
    CHECK(group->member_count == 4, "折叠任务不在被折叠的组里 (它在系统组)");
    CHECK(edc_task_group_unfolded_msg_count(group) == group_msgs_before - 7,
          "该组的未折叠消息数只少了被折的那 7 条 (13 → 6)");
    CHECK(fold->completion_seq == 0, "折叠任务不占 session 的完成序号 (不是用户的完成次序里的一环)");
    {
        struct edc_task *t3 = edc_task_group_find_task(group, "3");
        struct edc_task *t4 = edc_task_group_find_task(group, "4");
        CHECK(t3 && t4 && edc_task_has_folded_turns(t3) && !edc_task_has_folded_turns(t4),
              "切点成员被半折, 切点之后的 task 4 完全没动");
        if (t3 && t3->turns.tail > t3->turns.head)
        {
            struct edc_turn *t3turn = t3->turns.data[t3->turns.head];
            CHECK(t3turn->folded_prefix == 2 && edc_queue_length(&t3turn->messages) == 4,
                  "task 3 那个 turn 折了 2 条、留 2 条 (消息级水位, 不是整 turn)");
            CHECK(cJSON_GetArraySize(group->history) == 1 + edc_task_group_unfolded_msg_count(group),
                  "历史里剩下的正好是未折叠段 (半折 turn 的尾巴在)");
        }
    }

    /* ④ 不回收: 折叠任务是普通任务, 完成后留在系统组。 */
    CHECK(count_tasks_in(session) == 5, "折叠任务完成后仍在 (不回收)");
    CHECK(edc_session_find_system_group(session) != NULL, "系统组还在");

    /* ⑤ 再折一次: 可折叠的还剩 3 个 → 还能起。 */
    CHECK(edc_group_fold_due(group, cfg_due(cfg, group)), "还有可折叠的 (seq 3/4)");
    struct edc_fold_cut cut2;
    memset(&cut2, 0, sizeof(cut2));
    sds prompt2 = edc_group_fold_build_prompt(group, NULL, &cut2);
    CHECK(prompt2 && cut2.task_id && strcmp(cut2.task_id, "4") == 0 && cut2.prefix == 1,
          "第二轮切点推进到 task 4 的第 1 条 (剩 6 条未折叠 → 留 3 条)");
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
    uint32_t epoch_before = group->fold_epoch;

    CHECK(edc_group_fold_start(sched, session, group, NULL) != NULL, "起了一次折叠");
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
    CHECK(group->fold_epoch == epoch_before, "折叠没落地: fold_epoch 没动");
    CHECK(edc_group_fold_due(group, cfg_due(cfg, group)), "下一轮还会再折 (可折叠的还在)");

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

    struct edc_task *fold = edc_group_fold_start(sched, session, group, NULL);
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
 * Pins: it scans all sessions, starts one fold at a time, and takes the window from
 * config (with none, it does not fold at all). */

static void test_try_scans_all_sessions(void)
{
    TEST("触发扫描: 扫所有 session / 一次一个 / 窗口来自 config");

    struct edc_config *cfg = make_config();

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
    cfg_due(cfg, bgroup); /* 这个组"该折" —— 只有它该折 */

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

    CHECK(!edc_group_fold_try(NULL, &sm, cfg, NULL), "NULL scheduler 不折");
    CHECK(!edc_group_fold_try(sched, NULL, cfg, NULL), "NULL 仓库不折");

    /* ③ conf=NULL → 读不到窗口 → 不折 (不拿"不知道"当"随便折"; 此时无折叠在飞) */
    g_send_llm_calls = 0;
    CHECK(!edc_group_fold_try(sched, &sm, NULL, NULL), "conf=NULL (窗口未知) → 不折");
    CHECK(g_send_llm_calls == 0, "窗口未知时没有 LLM 请求");

    /* ① 扫所有 session: 第二个 session 里的组被折 */
    g_send_llm_calls = 0;
    struct edc_session *folded_sess = NULL;
    CHECK(edc_group_fold_try(sched, &sm, cfg, &folded_sess), "第二个 session 里的组被折了 (扫全部)");
    CHECK(folded_sess == busy, "回传的 session = 真被折的那个 (调用方靠它落盘)");
    pump_schedule(sched);
    CHECK(g_send_llm_calls == 1, "折叠 prompt 真发出去了一次 LLM 请求");
    CHECK(edc_scheduler_has_pending_system_task(busy), "busy 里已有折叠在飞");

    /* ② 一次一个: 在飞的那个 session 跳过, 不再起第二次 */
    g_send_llm_calls = 0;
    CHECK(!edc_group_fold_try(sched, &sm, cfg, NULL), "有折叠在飞 → 不再起第二次");
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
    cfg_win(cfg, 1000000); /* 宽窗口 ⇒ 自动路径不动, 本用例只走显式起折 */
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

    struct edc_task *fold = edc_group_fold_start(sched, session, group, NULL);
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
    cfg_win(cfg, 1000000); /* 宽窗口 ⇒ 自动路径不动, 本用例只走显式起折 */
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

    struct edc_task *fold = edc_group_fold_start(sched, session, group, NULL);
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
    cfg_win(cfg, 1000000); /* 宽窗口 ⇒ 自动路径不动, 本用例只走显式起折 */
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

    struct edc_task *fold = edc_group_fold_start(sched, session, group, NULL);
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
    cfg_win(cfg, 1000000); /* 宽窗口 ⇒ 自动路径不动, 本用例只走显式起折 */
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

    struct edc_task *fold = edc_group_fold_start(sched, session, group, NULL);
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


/* ═══════════════════════════════════════════════════════
 *  Test 10: 触发时机 (b) —— 请求前: 该折的组里, 待发任务先被 park
 *  ═══════════════════════════════════════════════════════ */

/* 挂载点 (b) 与挂载点 (a) 的区别就是"谁先动": 这里没有显式调 fold_start, 只有一次
 * 普通的扫描 (消息边界的动作), 折叠必须**因为**这次扫描自己起来, 而不是被测试点起来。 */
static void test_fold_before_request(void)
{
    TEST("请求前触发: 组该折时, 待发成员先停在 AWAIT_DEPS, 出去的是折叠的请求");

    struct edc_config *cfg = make_config();
    struct edc_session *session = edc_session_new(cfg, "sess-pre-drive", "PreDrive");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, session);
    sched->send_llm = stub_send_llm;
    g_send_llm_calls = 0;

    /* 挂载点要经 try 回传的 session 落盘 (单测里 history_dir 为空 → save 是 no-op)。 */
    struct edc_session *arr[1] = {session};
    struct edc_session_manager sm = {0};
    sm.sessions = arr;
    sm.session_count = 1;
    sm.active_index = 0;
    sm.config = cfg;
    sched->sessions = &sm;

    struct edc_task_group *group = make_foldable_group(session, "sess-pre-drive", "leader work");
    /* 语料只有 leader 那一条: 这一折会把它整条吃掉, 落地后组里没有可折叠的 =
     * 不会再有第二次 (用例后面要数请求数, 不能有第二折)。 */
    cfg_due(cfg, group);

    /* 一个"即将发请求"的成员: READY + active_turn。 */
    /* id 不用数字: 折叠任务由生成器给号, 数字会撞上 (上一版就撞了 "9")。 */
    struct edc_task *ready = edc_task_new("drv-99", "sess-pre-drive", group->group_id, "next", edc_channel_adapter);
    edc_task_group_add_task(group, ready);
    edc_scheduler_attach_input(sched, ready, "about to run");
    CHECK(edc_task_is_ready(ready), "成员待发请求");

    CHECK(!edc_scheduler_has_pending_system_task(session), "起折前没有折叠在飞");

    /* 消息边界 = 扫描。没人点折叠 —— 它必须由这次扫描自己起来。 */
    pump_schedule(sched);

    CHECK(edc_scheduler_has_pending_system_task(session), "扫描自己起了折叠");
    CHECK(g_send_llm_calls == 1, "只发出一次请求");
    CHECK(g_last_llm_task_id && strcmp(g_last_llm_task_id, ready->task_id) != 0, "出去的不是待发成员");
    CHECK(edc_task_awaits_deps(ready), "待发成员被 park 到 AWAIT_DEPS (复用 B1, 零新机制)");
    CHECK(!edc_task_is_running(ready), "它自己没被驱动");

    /* 折叠落地 → 同一条边把它放出来, 下次扫描才发它自己的请求。 */
    struct edc_task *fold = edc_session_find_system_group(session)->members;
    CHECK(fold != NULL, "系统组里有折叠任务");
    struct edc_llm_actor_resp resp;
    memset(&resp, 0, sizeof(resp));
    resp.reason = BC_LLM_FINISH_REASON_STOP;
    resp.content = sdsnew("# merged");
    void *fold_req = fold->current_request_id;
    struct edc_advance_result fr = edc_executor_advance_llm_resp(&sched->executor, fold, fold_req, &resp);
    edc_scheduler_apply_advance(sched, fold, fold_req, &fr);
    CHECK(edc_task_is_ready(ready), "折叠落地 → 成员被释放");

    pump_schedule(sched);
    CHECK(g_send_llm_calls == 2, "释放后它自己的请求才出去");
    CHECK(g_last_llm_task_id && strcmp(g_last_llm_task_id, ready->task_id) == 0, "驱动的是被 park 的那个");

    sdsfree(resp.content);
    edc_session_free(session);
    edc_scheduler_free(sched);
    edc_config_free(cfg);
}

/* 挂载点 (a) 与"只在任务完成时查"的区别: 这条用例**不调扫描** (所以挂载点 (b) 不可能
 * 触发), 只灌一次不结算的推进 —— 工具应答那条路就是这个形状。 */
static void test_fold_on_non_settling_advance(void)
{
    TEST("触发时机 (a): 一次不结算的推进 (工具应答那种) 也会带起折叠");

    struct edc_config *cfg = make_config();
    struct edc_session *session = edc_session_new(cfg, "sess-advance", "Advance");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, session);
    sched->send_llm = stub_send_llm;
    g_send_llm_calls = 0;

    struct edc_session *arr[1] = {session};
    struct edc_session_manager sm = {0};
    sm.sessions = arr;
    sm.session_count = 1;
    sm.active_index = 0;
    sm.config = cfg;
    sched->sessions = &sm;

    struct edc_task_group *group = make_foldable_group(session, "sess-advance", "leader work");
    add_foldable_task(session, group, "2", 2, 1, "second work");
    cfg_due(cfg, group);

    /* 一个在飞的成员: 不在 READY, 所以挂载点 (b) 不会看它 (只有"即将发请求"才触发)。 */
    struct edc_task *flying = edc_task_new("adv-1", "sess-advance", group->group_id, "flying", edc_channel_adapter);
    edc_task_group_add_task(group, flying);
    edc_scheduler_attach_input(sched, flying, "mid turn");
    edc_task_await_exec(flying);
    CHECK(!edc_task_is_ready(flying), "成员在飞 (不是待发状态)");

    CHECK(!edc_scheduler_has_pending_system_task(session), "推进前没有折叠在飞");

    /* 一次不结算的推进: changed 但没进终态 —— 工具应答推进就是这个形状。 */
    struct edc_advance_result res;
    memset(&res, 0, sizeof(res));
    res.outcome = EDC_STEP_BLOCKED;
    res.changed = true;
    res.settled = false;
    edc_scheduler_apply_advance(sched, flying, NULL, &res);

    CHECK(edc_scheduler_has_pending_system_task(session), "折叠被这次推进带起来了 (挂载点 a)");
    CHECK(g_send_llm_calls == 0, "推进自己不发请求 (折叠由消息边界扫描派发)");
    bool has_dep = false;
    struct edc_task_dep *d;
    LL_FOREACH(flying->deps, d)
    {
        if (d->source && d->source->sink == EDC_SINK_GROUP_MEMORY)
            has_dep = true;
    }
    CHECK(has_dep, "在飞成员挂上了 SEQUENCE dep (到它的下一个请求边界 park, B1)");

    edc_session_free(session);
    edc_scheduler_free(sched);
    edc_config_free(cfg);
}

/* ═══════════════════════════════════════════════════════
 *  Test 11: 切点 (P2a-4) —— 尾部 token 预算, 且永不切在消息内部
 *  ═══════════════════════════════════════════════════════ */

/* 一条 chars 个 'a' 的大消息 (估算 ≈ chars/4 token), 前面带标记 TAG<tag>:。 */
static void add_big_task(struct edc_session *session, struct edc_task_group *group, uint32_t seq, size_t chars,
                         const char *tag)
{
    struct edc_task *t = edc_task_new(tag, session->session_id, group->group_id, tag, edc_channel_adapter);
    edc_task_group_add_task(group, t);
    struct edc_turn *turn = edc_turn_new();
    turn->task_id = strdup(t->task_id);
    char *buf = malloc(chars + 32);
    int n = snprintf(buf, chars + 32, "TAG%s:", tag);
    memset(buf + n, 'a', chars);
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "role", "user");
    cJSON_AddStringToObject(m, "content", buf);
    free(buf);
    edc_queue_push(&turn->messages, m);
    edc_task_add_turn(t, turn);
    edc_task_await_exec(t);
    edc_task_mark_completed(t);
    t->completion_seq = seq;
    t->output = sdsnew(tag);
}

static void test_cut_keeps_recent_tokens(void)
{
    struct edc_config *cfg = make_config();

    /* (a) 三条 ~25k token 的大消息 + 一条小 leader: 从队尾数, 最新那条自己就够了
     *     ⇒ 只留它。"压最老一半"会留 2 条 (cnt=4) ⇒ 这条用例能分辨两种策略。 */
    {
        struct edc_session *session = edc_session_new(cfg, "sess-budget", "Budget");
        struct edc_task_group *group = make_foldable_group(session, "sess-budget", "tiny leader");
        add_big_task(session, group, 2, 100000, "2");
        add_big_task(session, group, 3, 100000, "3");
        add_big_task(session, group, 4, 100000, "4");

        struct edc_fold_cut cut;
        memset(&cut, 0, sizeof(cut));
        sds prompt = edc_group_fold_build_prompt(group, NULL, &cut);
        CHECK(prompt != NULL, "拿到 prompt");
        CHECK(cut.task_id && strcmp(cut.task_id, "4") == 0 && cut.turn == 0 && cut.prefix == 0,
              "只留最新那条 (~25k ≥ 20k), 切点在它前面 (turn 边界)");
        if (prompt)
        {
            CHECK(strstr(prompt, "tiny leader") != NULL, "最老的小消息进语料");
            CHECK(strstr(prompt, "TAG2:") != NULL && strstr(prompt, "TAG3:") != NULL, "两条大的也进语料");
            CHECK(strstr(prompt, "TAG4:") == NULL, "最新那条不进语料");
            sdsfree(prompt);
        }
        edc_session_free(session);
    }

    /* (b) 老的都小、最新一条大: 预算只看 token (条数轴上"半量"会折掉两条)。 */
    {
        struct edc_session *session = edc_session_new(cfg, "sess-budget-b", "BudgetB");
        struct edc_task_group *group = make_foldable_group(session, "sess-budget-b", "tiny leader");
        add_big_task(session, group, 2, 8, "2");
        add_big_task(session, group, 3, 8, "3");
        add_big_task(session, group, 4, 100000, "4");

        struct edc_fold_cut cut;
        memset(&cut, 0, sizeof(cut));
        sds prompt = edc_group_fold_build_prompt(group, NULL, &cut);
        CHECK(cut.task_id && strcmp(cut.task_id, "4") == 0 && cut.prefix == 0,
              "大的那条自己顶满预算 ⇒ 只留它 (小的全进语料)");
        if (prompt)
        {
            CHECK(strstr(prompt, "TAG2:") && strstr(prompt, "TAG3:"), "两条小的进语料");
            sdsfree(prompt);
        }
        edc_session_free(session);
    }

    /* (c) 小-大-小: 从队尾数到"大"那条才够预算 ⇒ 它整条留下, **不切在它内部**。 */
    {
        struct edc_session *session = edc_session_new(cfg, "sess-budget-c", "BudgetC");
        struct edc_task_group *group = make_foldable_group(session, "sess-budget-c", "tiny leader");
        add_big_task(session, group, 2, 100000, "2");
        add_big_task(session, group, 3, 8, "3");

        struct edc_fold_cut cut;
        memset(&cut, 0, sizeof(cut));
        sds prompt = edc_group_fold_build_prompt(group, NULL, &cut);
        CHECK(cut.task_id && strcmp(cut.task_id, "2") == 0 && cut.prefix == 0,
              "切点落在那条大消息**之前** (整条留作细节)");
        if (prompt)
        {
            CHECK(strstr(prompt, "tiny leader") != NULL, "只有最老的那条进语料");
            CHECK(strstr(prompt, "TAG2:") == NULL, "大消息整条没进语料 (没被切一半)");
            sdsfree(prompt);
        }
        edc_session_free(session);
    }

    edc_config_free(cfg);
}

/* ═══════════════════════════════════════════════════════════
 *  Test 12: P2a-4b —— 折在**在飞 turn** 的内部 (split turn)
 *  ═══════════════════════════════════════════════════════════ */

static void push_big_msg(struct edc_turn *turn, size_t chars, const char *tag)
{
    char *buf = malloc(chars + 32);
    int n = snprintf(buf, chars + 32, "TAG%s:", tag);
    memset(buf + n, 'a', chars);
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "role", "user");
    cJSON_AddStringToObject(m, "content", buf);
    free(buf);
    edc_turn_add_message(turn, m);
}

/* 一个完成的 leader + 一个**在飞**成员 (3 条各 ~25k token 的 active turn)。 */
static struct edc_task_group *make_inflight_group(struct edc_session *session, struct edc_scheduler *sched,
                                                  struct edc_task **out_running, struct edc_task **out_older,
                                                  struct edc_task **out_cancelled, const char *sid)
{
    struct edc_task_group *group = make_foldable_group(session, sid, "old leader");
    /* 另一个在飞成员: 它整条进语料 (切点落在后面那个成员上), 落地时它的 active turn
     * 必须被**整折** —— 不能只折"切点之前的已提交 turn"。 */
    /* 一个**取消**掉的成员: 留着半个 turn (取消只表示停下, turn 不删), 它不是
     * ALIVE ⇒ 语料**不收**它的 active turn ⇒ 落地也不许折它 (折了就是静默丢内容)。
     * 它的已提交 turn 照样是组历史的一部分, 照样可折。 */
    struct edc_task *cancelled = edc_task_new("7", sid, group->group_id, "cancelled half", edc_channel_adapter);
    edc_task_group_add_task(group, cancelled);
    {
        struct edc_turn *ct = edc_turn_new();
        ct->task_id = strdup(cancelled->task_id);
        cJSON *cm = cJSON_CreateObject();
        cJSON_AddStringToObject(cm, "role", "user");
        cJSON_AddStringToObject(cm, "content", "cancelled committed message");
        edc_queue_push(&ct->messages, cm);
        edc_task_add_turn(cancelled, ct);
    }
    edc_scheduler_attach_input(sched, cancelled, "cancelled half turn");
    edc_task_mark_cancelled(cancelled);
    if (out_cancelled)
        *out_cancelled = cancelled;

    struct edc_task *older = edc_task_new("8", sid, group->group_id, "older running", edc_channel_adapter);
    edc_task_group_add_task(group, older);
    edc_scheduler_attach_input(sched, older, "older running input");
    out_older ? (*out_older = older) : (void)0;

    struct edc_task *running = edc_task_new("9", sid, group->group_id, "running", edc_channel_adapter);
    edc_task_group_add_task(group, running);
    edc_scheduler_attach_input(sched, running, "running input");
    push_big_msg(running->active_turn, 100000, "0");
    push_big_msg(running->active_turn, 100000, "1");
    push_big_msg(running->active_turn, 100000, "2");
    *out_running = running;
    return group;
}

static void test_cut_inside_active_turn(void)
{
    TEST("P2a-4b: 折在飞 turn 的前缀 ⇒ active_turn->folded_prefix (请求侧按它切片)");
    struct edc_config *cfg = make_config();
    struct edc_session *session = edc_session_new(cfg, "sess-inflight", "Inflight");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, session);
    sched->send_llm = stub_send_llm;

    struct edc_task *running = NULL, *older = NULL, *cancelled = NULL;
    struct edc_task_group *group = make_inflight_group(session, sched, &running, &older, &cancelled, "sess-inflight");
    uint32_t epoch_before = group->fold_epoch;

    struct edc_fold_cut cut;
    memset(&cut, 0, sizeof(cut));
    sds prompt = edc_group_fold_build_prompt(group, NULL, &cut);
    CHECK(prompt != NULL, "在飞 turn 也算语料");
    /* active turn = input + 3 条大消息 = 4 条; 最新那条自己 ≥ 20k ⇒ 只留它, prefix = 3。 */
    CHECK(cut.task_id && strcmp(cut.task_id, "9") == 0 && cut.turn == EDC_FOLD_CUT_ACTIVE_TURN && cut.prefix == 3,
          "切点落在在飞 turn 内部 (留最新那条, 前三条进语料)");
    CHECK(cut.prefix == edc_queue_length(&running->active_turn->messages) - 1, "留的正好是最新那条");
    if (prompt)
    {
        CHECK(strstr(prompt, "TAG0:") && strstr(prompt, "TAG1:") && strstr(prompt, "running input"),
              "active turn 的前三条 (含最初那条 input) 进语料");
        CHECK(strstr(prompt, "TAG2:") == NULL, "最新那条留作细节");
        CHECK(strstr(prompt, "old leader") != NULL, "老的完成成员照样在语料里");
        CHECK(strstr(prompt, "**Turn Context (split turn):**") != NULL, "语料尾部是没写完的 turn ⇒ 带 split-turn 段 (pi §5)");
        CHECK(strstr(prompt, "- Original Request:") != NULL && strstr(prompt, "- Early Progress:") != NULL &&
                  strstr(prompt, "- Context for Suffix:") != NULL,
              "段里三个聚焦点 (原请求 / 早期进展 / 续跑要的上下文)");
        sdsfree(prompt);
    }

    struct edc_task *fold = edc_group_fold_start(sched, session, group, NULL);
    CHECK(fold != NULL, "折叠任务起来了");
    CHECK(fold && fold->fold_cut_turn == EDC_FOLD_CUT_ACTIVE_TURN, "折叠任务记着'切在在飞 turn'");
    if (fold)
    {
        edc_group_fold_deliver_memory(fold, "# Merged\n- folded the running turn's head");
        CHECK(running->active_turn && running->active_turn->folded_prefix == 3,
              "active turn 的前缀 = 3 (这条前缀就是 req->turn_begin 的来源)");
        CHECK(group->fold_epoch == epoch_before + 1, "落地 bump fold_epoch");
        CHECK(edc_queue_length(&running->active_turn->messages) - running->active_turn->folded_prefix == 1,
              "在飞 turn 只剩那一条未折叠的细节");
    }
    /* 6 条里"压一半"会切在 prefix 1, 尾巴 token 预算切在 3 ⇒ 能分辨两种策略。 */
    CHECK(cut.prefix != 1, "不是'压最老一半'的结果 (半量会切在 prefix 1)");
    CHECK(older && older->active_turn &&
              older->active_turn->folded_prefix == (int)edc_queue_length(&older->active_turn->messages),
          "整条进语料的在飞成员: active turn 被整折 (不是只折已提交的 turn)");
    CHECK(cancelled && cancelled->active_turn && cancelled->active_turn->folded_prefix == 0,
          "取消成员的半个 turn 没进语料 ⇒ 落地也不折它 (折了就是丢内容)");
    CHECK(cancelled && cancelled->turns.tail > cancelled->turns.head &&
              ((struct edc_turn *)cancelled->turns.data[cancelled->turns.head])->folded_prefix ==
                  (int)edc_queue_length(&((struct edc_turn *)cancelled->turns.data[cancelled->turns.head])->messages),
          "但它的**已提交** turn 照折 (那是组历史的一部分)");

    edc_session_free(session);
    edc_scheduler_free(sched);
    edc_config_free(cfg);
}

/* 一条 role + 标记 + 可选大填充的消息 (估算 ≈ chars/4 token)。 */
static void push_msg(struct edc_turn *turn, const char *role, const char *tag, size_t chars, const char *tool_call_id)
{
    char *buf = malloc(chars + 64);
    int n = snprintf(buf, chars + 64, "%s:", tag);
    memset(buf + n, 'a', chars);
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "role", role);
    cJSON_AddStringToObject(m, "content", buf);
    free(buf);
    if (tool_call_id)
    {
        cJSON_AddStringToObject(m, "tool_call_id", tool_call_id);
        cJSON_AddStringToObject(m, "name", "some_tool");
    }
    edc_turn_add_message(turn, m);
}

static void test_cut_never_orphans_tool_results(void)
{
    TEST("切点合法性: 第一条留下的消息不能是工具结果 (往回退到它的 assistant, pi §4)");
    struct edc_config *cfg = make_config();
    struct edc_session *session = edc_session_new(cfg, "sess-orphan", "Orphan");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, session);
    sched->send_llm = stub_send_llm;

    struct edc_task_group *group = make_foldable_group(session, "sess-orphan", "old leader");
    struct edc_task *running = edc_task_new("9", "sess-orphan", group->group_id, "running", edc_channel_adapter);
    edc_task_group_add_task(group, running);
    edc_scheduler_attach_input(sched, running, "running input"); /* m0: user */
    struct edc_turn *turn = running->active_turn;

    /* m1: assistant 带 tool_calls (切在它上面是合法的: 工具结果自然跟随保留) */
    cJSON *a = cJSON_CreateObject();
    cJSON_AddStringToObject(a, "role", "assistant");
    cJSON_AddStringToObject(a, "content", "ASSISTANT_WITH_CALLS:");
    cJSON *tcs = cJSON_CreateArray();
    cJSON *tc = cJSON_CreateObject();
    cJSON_AddStringToObject(tc, "id", "call_1");
    cJSON_AddStringToObject(tc, "type", "function");
    cJSON *fn = cJSON_CreateObject();
    cJSON_AddStringToObject(fn, "name", "some_tool");
    cJSON_AddStringToObject(fn, "arguments", "{}");
    cJSON_AddItemToObject(tc, "function", fn);
    cJSON_AddItemToArray(tcs, tc);
    cJSON_AddItemToObject(a, "tool_calls", tcs);
    edc_turn_add_message(turn, a);

    push_msg(turn, "tool", "TOOL_BIG", 100000, "call_1");  /* m2: 大工具结果 (~25k token) */
    push_msg(turn, "tool", "TOOL_SMALL", 8, "call_1");     /* m3: 小工具结果 */
    push_msg(turn, "assistant", "FINAL", 8, NULL);         /* m4: 最终回复 */

    struct edc_fold_cut cut;
    memset(&cut, 0, sizeof(cut));
    sds prompt = edc_group_fold_build_prompt(group, NULL, &cut);
    CHECK(prompt != NULL, "拿到 prompt");
    /* 朴素的尾巴预算会切在 m2 (工具结果) 上 ⇒ 孤儿。回退一条到 m1 (assistant)。 */
    CHECK(cut.task_id && strcmp(cut.task_id, "9") == 0 && cut.prefix == 1,
          "切点回退到 assistant(tool_calls), 不是切在工具结果上");
    CHECK(cut.prefix != 2, "朴素切点 (m2 = 工具结果) 被拒了");
    if (prompt)
    {
        CHECK(strstr(prompt, "running input") != NULL, "turn 开头那条 user 进语料");
        CHECK(strstr(prompt, "TOOL_BIG") == NULL, "工具结果没进语料 (它跟着 assistant 一起留)");
        CHECK(strstr(prompt, "ASSISTANT_WITH_CALLS") == NULL, "assistant(tool_calls) 也留作细节");
        sdsfree(prompt);
    }

    /* 落地后: 留下的第一条**不是**工具结果 (否则请求起手就是孤儿 tool 消息)。 */
    struct edc_task *fold = edc_group_fold_start(sched, session, group, NULL);
    CHECK(fold != NULL, "折叠起来了");
    if (fold)
    {
        edc_group_fold_deliver_memory(fold, "# Merged\n- folded");
        CHECK(turn->folded_prefix == 1, "前缀 = 1 (请求从 m1 开始发)");
        cJSON *first_kept = turn->messages.data[turn->messages.head + turn->folded_prefix];
        cJSON *role = cJSON_GetObjectItem(first_kept, "role");
        CHECK(role && cJSON_IsString(role) && strcmp(role->valuestring, "tool") != 0,
              "留下的第一条不是工具结果 (不会孤儿)");
    }

    edc_session_free(session);
    edc_scheduler_free(sched);
    edc_config_free(cfg);
}

static void test_prompt_initial_variant_names_the_sections(void)
{
    TEST("P2b: 没有旧记忆 = 首次摘要, 指令逐段点名 pi §8 的六段格式");
    struct edc_config *cfg = make_config();
    struct edc_session *session = edc_session_new(cfg, "sess-initial", "Initial");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, session);
    sched->send_llm = stub_send_llm;

    struct edc_task_group *group = make_foldable_group(session, "sess-initial", "old leader");
    add_foldable_task(session, group, "2", 2, 4, "task two");
    CHECK(group->memory == NULL, "组还没有记忆 (首次)");

    struct edc_fold_cut cut;
    memset(&cut, 0, sizeof(cut));
    sds prompt = edc_group_fold_build_prompt(group, NULL, &cut);
    CHECK(prompt != NULL, "拿到 prompt");
    if (prompt)
    {
        CHECK(strstr(prompt, "<previous-summary>") == NULL, "首次: 没有 <previous-summary>");
        CHECK(strstr(prompt, "<group-transcript>") != NULL, "语料照旧包着");
        CHECK(strstr(prompt, "## Goal") != NULL && strstr(prompt, "## Constraints & Preferences") != NULL &&
                  strstr(prompt, "### Done") != NULL && strstr(prompt, "### In Progress") != NULL &&
                  strstr(prompt, "### Blocked") != NULL && strstr(prompt, "## Key Decisions") != NULL &&
                  strstr(prompt, "## Next Steps") != NULL && strstr(prompt, "## Critical Context") != NULL,
              "指令点名六段 (Goal / Constraints / Progress{Done,In Progress,Blocked} / Decisions / Next / Critical)");
        CHECK(strstr(prompt, "Preserve exact file paths") != NULL, "要求保留路径/函数名/错误原文");
        CHECK(strstr(prompt, "Output the complete updated summary") == NULL, "首次不用迭代指令");
        CHECK(strstr(prompt, "Additional focus:") == NULL, "没给 focus 就没有这一行");
        sdsfree(prompt);
    }

    /* focus: 手动 /group fold 的聚焦词, 原文进 prompt。 */
    struct edc_task *fold = edc_group_fold_start(sched, session, group, "keep the build flags");
    CHECK(fold != NULL, "带着 focus 也能起折");
    if (fold && fold->active_turn)
    {
        cJSON *m = fold->active_turn->messages.data[fold->active_turn->messages.head];
        cJSON *content = m ? cJSON_GetObjectItem(m, "content") : NULL;
        CHECK(content && cJSON_IsString(content) && strstr(content->valuestring, "\n\nAdditional focus: keep the build flags") != NULL,
              "focus 以 Additional focus: 追加在指令之后");
        CHECK(strstr(content->valuestring, "<previous-summary>") == NULL, "这个组还没有记忆 ⇒ 仍是首次变体");
    }

    edc_session_free(session);
    edc_scheduler_free(sched);
    edc_config_free(cfg);
}

/* P2b-2: 摘要被截断 (finish_reason=length) 不能当 checkpoint —— 半截记忆顶掉完整的
 * 记忆就是静默丢内容。pi §3 把 length 明确判为失败。 */
static void test_truncated_summary_discards_fold(void)
{
    TEST("P2b: 摘要被截断 ⇒ 折叠失败, 记忆与切点原样不动 (半截摘要不许当 checkpoint)");
    struct edc_config *cfg = make_config();
    struct edc_session *session = edc_session_new(cfg, "sess-trunc", "Trunc");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, session);
    sched->send_llm = stub_send_llm;

    struct edc_task_group *group = make_foldable_group(session, "sess-trunc", "old leader");
    add_foldable_task(session, group, "2", 2, 4, "task two");
    group->memory = sdsnew("# Full memory\n- everything remembered");

    struct edc_task *fold = edc_group_fold_start(sched, session, group, NULL);
    CHECK(fold != NULL, "折叠起来了");
    if (!fold)
        return;
    void *fold_req = (void *)0x7b01;
    fold->current_request_id = fold_req;

    struct edc_llm_actor_resp resp;
    memset(&resp, 0, sizeof(resp));
    resp.reason = BC_LLM_FINISH_REASON_LENGTH; /* 撞上 max_tokens */
    resp.content = sdsnew("# Half a summary\n- cut off in the middle");
    struct edc_advance_result res = edc_executor_advance_llm_resp(&sched->executor, fold, fold_req, &resp);
    edc_scheduler_apply_advance(sched, fold, fold_req, &res);
    sdsfree(resp.content);

    CHECK(edc_task_has_failed(fold), "折叠任务 ERROR (不是 COMPLETED)");
    CHECK(group->memory && strcmp(group->memory, "# Full memory\n- everything remembered") == 0,
          "记忆原样 (半截摘要没有顶掉它)");
    CHECK(count_folded(group) == 0, "一条也没折 (切点没动)");
    CHECK(res.notice && strstr(res.notice, "truncated") != NULL, "提示直说被截断 (可诊断, 不是 ‘unknown finish reason’)");
    CHECK(group->fold_failures == 1, "(c) 真实的 ERROR 路径也记一次失败 (调度器那条钩子)");

    edc_session_free(session);
    edc_scheduler_free(sched);
    edc_config_free(cfg);
}

/* pi §5 的另一半: 切点在 **turn 边界**、语料全是写完的 turn ⇒ 不出现 split-turn 段
 * (那条指令只在语料尾部是"没写完的 turn"时才该加)。 */
static void test_no_split_turn_at_boundary(void)
{
    TEST("P2b/pi §5: 切点在 turn 边界 ⇒ 不带 split-turn 段 (指令不乱加)");
    struct edc_config *cfg = make_config();
    struct edc_session *session = edc_session_new(cfg, "sess-boundary", "Boundary");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, session);

    /* 三个成员各一条消息: 整组远小于 keep_recent ⇒ 压最老一半 = 折 2 条, 切点落在
     * 第三个成员那条消息上 (它正好是 turn 的第一条) ⇒ 切在 turn 边界。 */
    struct edc_task_group *group = make_foldable_group(session, "sess-boundary", "old leader");
    add_foldable_task(session, group, "2", 2, 1, "task two");
    add_foldable_task(session, group, "3", 3, 1, "task three");

    struct edc_fold_cut cut;
    memset(&cut, 0, sizeof(cut));
    sds prompt = edc_group_fold_build_prompt(group, NULL, &cut);
    CHECK(prompt != NULL, "拿到 prompt");
    CHECK(cut.task_id && strcmp(cut.task_id, "3") == 0 && cut.prefix == 0, "切在 task 3 那条消息之前 (prefix 0)");
    if (prompt)
    {
        CHECK(strstr(prompt, "Turn Context (split turn)") == NULL, "没有 split-turn 段");
        CHECK(strstr(prompt, "## Critical Context") != NULL, "六段指令照旧 (不是别的原因没输出)");
        sdsfree(prompt);
    }

    edc_session_free(session);
    edc_scheduler_free(sched);
    edc_config_free(cfg);
}

/* P2 判据 (docs/pi-alignment.md §2.4): "折叠请求装得下" —— 在触发线上起折时,
 * 交给摘要器的语料必须小于 window - reserve, 否则**折叠请求自己**就超窗
 * (那正是 P2 从"按条数"改成"向窗口要空间"要消掉的隐患)。 */
static void add_big_member(struct edc_session *session, struct edc_task_group *group, const char *id,
                           uint32_t seq, const char *tag)
{
    struct edc_task *t = edc_task_new(id, session->session_id, group->group_id, tag, edc_channel_adapter);
    edc_task_group_add_task(group, t);
    struct edc_turn *turn = edc_turn_new();
    turn->task_id = strdup(t->task_id);
    push_big_msg(turn, 100000, tag); /* ascii/4 ≈ 25k token */
    edc_task_add_turn(t, turn);
    edc_task_await_exec(t);
    edc_task_mark_completed(t);
    t->completion_seq = seq;
    t->output = sdsnew(tag);
}

static void test_fold_request_fits_the_window(void)
{
    TEST("P2 判据: 触发线上起折 ⇒ 折叠 prompt 装在 window - reserve 里");

    struct edc_config *cfg = make_config();
    struct edc_session *session = edc_session_new(cfg, "sess-fit", "Fit");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, session);

    struct edc_task_group *group = make_foldable_group(session, "sess-fit", "leader work");
    add_big_member(session, group, "2", 2, "two");
    add_big_member(session, group, "3", 3, "three");
    add_big_member(session, group, "4", 4, "four");

    /* 真路径在成员完成时把它的 turn 并进组历史 (合成夹具这里补上这一步)。 */
    edc_task_group_rebuild_history(group);

    long long ctx = edc_context_group_ctx_tokens(group, NULL);
    CHECK(ctx > EDC_FOLD_KEEP_RECENT_TOKENS, "夹具够大 (走尾部 token 预算, 不是压最老一半)");

    /* 窗口取"刚刚过线": window - reserve = ctx - 1 ⇒ 这是触发的最坏位置 (ctx 最小)。 */
    cfg->context_window = (int)(ctx + EDC_FOLD_RESERVE_TOKENS - 1);
    long long budget = (long long)cfg->context_window - EDC_FOLD_RESERVE_TOKENS;
    CHECK(edc_group_fold_due(group, cfg), "刚好过线 ⇒ due");

    struct edc_fold_cut cut;
    memset(&cut, 0, sizeof(cut));
    sds prompt = edc_group_fold_build_prompt(group, NULL, &cut);
    CHECK(prompt != NULL, "拿到 prompt");
    if (prompt)
    {
        long long pt = edc_token_estimate_text(prompt);
        log_info("  fit: ctx=%lld budget=%lld prompt=%lld cut=%s/%u/%u", ctx, budget, pt,
                 cut.task_id ? cut.task_id : "(null)", cut.turn, cut.prefix);
        CHECK(pt < budget, "prompt < window - reserve (否则折叠请求自己超窗)");
        CHECK(strstr(prompt, "TAGtwo:") != NULL, "老成员进语料");
        CHECK(strstr(prompt, "TAGfour:") == NULL, "最新那条留在细节里 (尾部预算生效)");
        sdsfree(prompt);
    }

    edc_session_free(session);
    edc_scheduler_free(sched);
    edc_config_free(cfg);
}

static void test_unresolvable_cut_folds_nothing(void)
{
    TEST("切点解析不出来 ⇒ fail-closed: 只写记忆, 什么都不折 (绝不折进记忆没覆盖的消息)");
    struct edc_config *cfg = make_config();
    struct edc_session *session = edc_session_new(cfg, "sess-nocut", "NoCut");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, session);
    sched->send_llm = stub_send_llm;

    struct edc_task *running = NULL, *older = NULL, *cancelled = NULL;
    struct edc_task_group *group = make_inflight_group(session, sched, &running, &older, &cancelled, "sess-nocut");
    uint32_t epoch_before = group->fold_epoch;
    struct edc_task *fold = edc_group_fold_start(sched, session, group, NULL);
    CHECK(fold != NULL, "折叠起来了");
    if (!fold)
        return;

    /* 切点指着一条不存在的成员 —— 落地必须拒绝"猜"。 */
    free(fold->fold_cut_task_id);
    fold->fold_cut_task_id = strdup("no-such-task");
    edc_group_fold_deliver_memory(fold, "# Merged\n- nothing folded");

    CHECK(group->memory && strstr(group->memory, "nothing folded") != NULL, "记忆照写");
    CHECK(count_folded(group) == 0, "一条也没折 (fail-closed, 不是 fail-open)");
    CHECK(running->active_turn && running->active_turn->folded_prefix == 0, "在飞 turn 的前缀没动");
    CHECK(group->fold_epoch == epoch_before + 1, "epoch 照 bump (系统提示变了, 旧锚一样过期)");

    edc_session_free(session);
    edc_scheduler_free(sched);
    edc_config_free(cfg);
}

static void test_cut_active_turn_committed_before_landing(void)
{
    TEST("P2a-4b: 折叠期间在飞 turn 自己提交了 ⇒ 前缀落在它的新位置 (队列末尾)");
    struct edc_config *cfg = make_config();
    struct edc_session *session = edc_session_new(cfg, "sess-commit", "Commit");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, session);
    sched->send_llm = stub_send_llm;

    struct edc_task *running = NULL, *older = NULL, *cancelled = NULL;
    struct edc_task_group *group = make_inflight_group(session, sched, &running, &older, &cancelled, "sess-commit");
    struct edc_task *fold = edc_group_fold_start(sched, session, group, NULL);
    CHECK(fold != NULL, "折叠起来了");
    struct edc_turn *active = running->active_turn;

    /* 折叠在飞的时候这一轮结束了: turn 提交进队列 (新的在飞 turn 还没起)。 */
    edc_task_commit_active_turn(running);
    CHECK(running->active_turn == NULL && running->turns.tail - running->turns.head == 1, "turn 已提交, 队列里就它一个");

    /* 落地成功必须把失败账清零 (与"起折不清零"配对): 先人为记两笔。 */
    group->fold_failures = 2;
    edc_group_fold_deliver_memory(fold, "# Merged\n- folded");
    CHECK(active->folded_prefix == 3, "前缀打在**同一个 turn 对象**上 (提交后它在队列末尾)");
    CHECK(running->turns.data[running->turns.tail - 1] == active, "队列末尾就是它");
    CHECK(group->fold_failures == 0, "(c) 落地成功清零失败账 (起折不清零, 落地才清)");

    edc_session_free(session);
    edc_scheduler_free(sched);
    edc_config_free(cfg);
}

/* ═══════════════════════════════════════════════════════════
 *  Test 20 (b): 语料上限 —— 单次折叠不许把不可达成的压缩比压给摘要器
 * ═══════════════════════════════════════════════════════════ */

/* 一条 chars 个 'a' 的大消息 (估算 ≈ chars/4 token)。**显式写 NUL** —— 别赌 malloc
 * 给的是清零页 (老助手就少了这一句, 只是恰好一直没事)。 */
static void push_big_msg_n(struct edc_turn *turn, size_t chars, const char *tag)
{
    char *buf = malloc(chars + 64);
    int w = snprintf(buf, chars + 64, "TAG%s:", tag);
    memset(buf + w, 'a', chars);
    buf[w + (int)chars] = '\0';
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "role", "user");
    cJSON_AddStringToObject(m, "content", buf);
    free(buf);
    edc_turn_add_message(turn, m);
}

static void test_corpus_is_capped(void)
{
    TEST("(b) 语料超上限 ⇒ 切点往回收: 宁可多留细节, 也不发一发超压缩比的请求");
    struct edc_config *cfg = make_config();
    struct edc_session *session = edc_session_new(cfg, "sess-corpus-cap", "CorpusCap");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, session);

    /* 8 条各 ~25k token 的完成成员。尾部预算 (keep_recent) 单独作用只会留最新那一条
     * ⇒ 语料 ~175k, 是上限 131072 的 1.3 倍 —— 那正是"要求模型 13:1 压缩"的现场。 */
    struct edc_task_group *group = make_foldable_group(session, "sess-corpus-cap", "tiny leader");
    for (uint32_t seq = 2; seq <= 9; seq++)
    {
        char id[8];
        snprintf(id, sizeof(id), "%u", seq);
        struct edc_task *t = edc_task_new(id, "sess-corpus-cap", group->group_id, id, edc_channel_adapter);
        edc_task_group_add_task(group, t);
        struct edc_turn *turn = edc_turn_new();
        turn->task_id = strdup(t->task_id);
        push_big_msg_n(turn, 100000, id);
        edc_task_add_turn(t, turn);
        edc_task_await_exec(t);
        edc_task_mark_completed(t);
        t->completion_seq = seq;
        t->output = sdsnew(id);
    }

    struct edc_fold_cut cut;
    memset(&cut, 0, sizeof(cut));
    sds prompt = edc_group_fold_build_prompt(group, NULL, &cut);
    CHECK(prompt != NULL, "拿到 prompt");
    /* 上限把切点从 "9" (尾部预算的结果) 收到 "7": 语料 = leader + 2..6
     * = 11 + 5 x 25010 = 125061 <= 131072; 再加一条 (150071) 就超。 */
    CHECK(cut.task_id && strcmp(cut.task_id, "7") == 0,
          "切点被上限收到 task 7 (尾部预算单独作用会切在最新的 9)");
    if (prompt)
    {
        CHECK(strstr(prompt, "TAG2:") != NULL && strstr(prompt, "TAG6:") != NULL, "上限内的都进语料");
        CHECK(strstr(prompt, "TAG7:") == NULL && strstr(prompt, "TAG8:") == NULL && strstr(prompt, "TAG9:") == NULL,
              "超出上限的三条整条留作细节 (一条都没被切一半)");
        sdsfree(prompt);
    }

    edc_session_free(session);
    edc_scheduler_free(sched);
    edc_config_free(cfg);
}

/* ═══════════════════════════════════════════════════════════
 *  Test 21 (c): 连续失败上限 —— 失败不许变成"每 ~40s 原样重试"的永动机
 * ═══════════════════════════════════════════════════════════ */

static void test_fold_failure_cap(void)
{
    TEST("(c) 连续失败到上限 ⇒ 自动折叠停手; 手动折叠是唯一的重启开关");
    struct edc_config *cfg = make_config();
    struct edc_session *session = edc_session_new(cfg, "sess-failcap", "FailCap");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, session);
    sched->send_llm = stub_send_llm;

    struct edc_task_group *group = make_foldable_group(session, "sess-failcap", "old leader");
    add_foldable_task(session, group, "2", 2, 4, "task two");
    cfg_due(cfg, group); /* 这个组一定"该折" */

    CHECK(edc_group_fold_due(group, cfg), "基线: 该折");
    edc_group_fold_note_failure(group);
    CHECK(group->fold_failures == 1 && edc_group_fold_due(group, cfg), "1 次失败: 还该折 (一次失败就放弃太脆)");
    edc_group_fold_note_failure(group);
    CHECK(edc_group_fold_due(group, cfg), "2 次失败: 还没到顶");
    edc_group_fold_note_failure(group);
    CHECK(group->fold_failures == EDC_FOLD_MAX_FAILURES, "到上限");
    CHECK(!edc_group_fold_due(group, cfg), "到上限后自动路径不再该折 (永动机熄火)");

    /* 起折本身**不**复位 —— 否则"起折 → 失败 → 起折"永远到不了上限, 正是永动机的形状。
     * start 也不受限 (只有 due 那道门管自动路径, 手动/直接起折照走)。 */
    struct edc_task *direct = edc_group_fold_start(sched, session, group, NULL);
    CHECK(direct != NULL, "start 不受失败上限限制 (被限制的是 due 这道门)");
    CHECK(group->fold_failures == EDC_FOLD_MAX_FAILURES, "起折**不清零** (清零的是落地/手动/新内容)");
    if (direct)
        edc_task_mark_cancelled(direct); /* 收尾: 会话里别有在飞的系统任务, 免得下面的手动折叠被挡 */

    /* 手动折叠既不受上限限制, 也是复位开关 (自动路径熄火后唯一的重启方式)。 */
    struct edc_task *manual = edc_scheduler_fold_group_now(sched, session, group, NULL);
    CHECK(manual != NULL, "手动 /group fold 照起");
    CHECK(group->fold_failures == 0, "手动折叠清掉失败账");
    CHECK(edc_group_fold_due(group, cfg), "复位后自动路径又该折了");

    /* 组里又有了**已提交内容**也清零 (调度器那条钩子: 成员完成 = 新情况, 值得再试)。 */
    group->fold_failures = EDC_FOLD_MAX_FAILURES;
    struct edc_task *member = edc_task_new("99", "sess-failcap", group->group_id, "later work", edc_channel_adapter);
    edc_task_group_add_task(group, member);
    edc_task_await_exec(member);
    edc_task_mark_completed(member);
    struct edc_advance_result r;
    memset(&r, 0, sizeof(r));
    r.outcome = EDC_STEP_DONE;
    r.settled = true;
    r.changed = true;
    edc_scheduler_apply_advance(sched, member, NULL, &r);
    CHECK(group->fold_failures == 0, "成员完成 (新内容) ⇒ 失败账清零");

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
    test_fold_before_request();
    test_fold_on_non_settling_advance();
    test_cut_keeps_recent_tokens();
    test_cut_inside_active_turn();
    test_cut_never_orphans_tool_results();
    test_prompt_initial_variant_names_the_sections();
    test_truncated_summary_discards_fold();
    test_fold_request_fits_the_window();
    test_no_split_turn_at_boundary();
    test_unresolvable_cut_folds_nothing();
    test_cut_active_turn_committed_before_landing();
    test_corpus_is_capped();
    test_fold_failure_cap();

    log_info("=== %d passed, %d failed ===", test_passed, test_failed);
    printf("%d passed, %d failed\n", test_passed, test_failed);
    return test_failed == 0 ? 0 : 1;
}
