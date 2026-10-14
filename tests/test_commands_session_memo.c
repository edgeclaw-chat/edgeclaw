/* `/session memo show|search|compact`。

 * 钉住四件:
 *   ① show: 空记忆 / 列全部 (索引行 + 总数) / 单条 (明细 + 来源) / 未知 id;
 *   ② search: 命中与未命中;
 *   ③ compact: 立刻起一个 SINK=SESSION_MEMORY 的任务, 材料里带所有组的记忆与 focus;
 *      已有 system task 在飞时**拒绝** (与自动路径同一条闸门);
 *   ④ 未知子命令 = usage (不起任何任务)。

 * harness 抄 test_commands_group_fold.c: 走公开命令入口 edc_commands_handler,
 * 末尾自己调 edc_scheduler_schedule (消息边界就是 tick)。没有通道 actor ⇒ 回话
 * 只给类型 (收不到), 所以命令层看**行为**, 文本看 kernel 的纯函数格式化器。 */
#include "edc_channel_actor.h"
#include "edc_commands.h"
#include "edc_executor.h"
#include "edc_llm_actor.h"
#include "edc_compat.h"
#include "edc_config.h"
#include "edc_scheduler.h"
#include "edc_session.h"
#include "edc_session_manager.h"
#include "edc_session_memory.h"
#include "edc_task.h"
#include "edc_task_group.h"
#include "edc_turn.h"
#include "utlist_fix.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

volatile int _run_loop = 1;

static int passed = 0, failed = 0;
#define TEST(n)                                                                                                        \
    do                                                                                                                 \
    {                                                                                                                  \
        printf("  TEST: %-58s ", n);                                                                                   \
    } while (0)
#define CHECK(c, m)                                                                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(c))                                                                                                      \
        {                                                                                                              \
            printf("FAIL: %s\n", m);                                                                                   \
            failed++;                                                                                                  \
        }                                                                                                              \
        else                                                                                                           \
        {                                                                                                              \
            printf("PASS\n");                                                                                          \
            passed++;                                                                                                  \
        }                                                                                                              \
    } while (0)

static void install_resolver(struct edc_scheduler *sched, struct edc_session *sess)
{
    sched->ctx = sess;
    struct edc_session_manager *sm = g_new0(struct edc_session_manager, 1);
    sm->sessions = g_new0(struct edc_session *, 1);
    sm->sessions[0] = sess;
    sm->session_count = sess ? 1 : 0;
    sm->active_index = 0;
    sched->sessions = sm;
}

static int g_llm_calls = 0;
static struct edc_task *g_llm_task = NULL;

static void mock_send_llm(struct edc_scheduler *sched, struct edc_task *task, void *request_id)
{
    (void)sched;
    (void)request_id;
    g_llm_calls++;
    g_llm_task = task;
}

static void drive_command(struct edc_config *cfg, struct edc_session_manager *sm, struct edc_scheduler *sched,
                          const char *content)
{
    struct edc_channel_message *cm =
        edc_channel_message_new(edc_channel_adapter, EDC_CHANNEL_MESSAGE_USER_TYPE, sdsnew(content));
    edc_commands_handler(cfg, sm, sched, (struct edc_reply_to){.type = cm->type}, cm);
    edc_scheduler_schedule(sched);
}

static struct edc_task *find_l2(struct edc_session *sess)
{
    struct edc_task_group *sys = edc_session_find_system_group(sess);
    if (!sys)
        return NULL;
    struct edc_task *t;
    LL_FOREACH2(sys->members, t, group_next)
    {
        if (t->sink == EDC_SINK_SESSION_MEMORY && !edc_task_is_completed(t))
            return t;
    }
    return NULL;
}

/* 给会话放一条条目 (与 memory_write 同形: create + CREATED 事件)。 */
static sds seed_entry(struct edc_session *sess, const char *title, const char *summary, const char *detail)
{
    sds id = edc_session_memory_create_entry(sess->memo, title, summary, detail, "task-1");
    uint64_t seq = edc_session_memory_add_event(sess->memo, id, EDC_MEMO_CREATED, title, summary);
    struct edc_memo_entry *e = edc_session_memory_entry(sess->memo, id);
    if (e)
    {
        e->created_seq = e->updated_seq = seq;
        e->sources = strdup("group 675");
    }
    return id;
}

int main(void)
{
    struct edc_config *cfg = g_new0(struct edc_config, 1);
    cfg->workspace = sdsnew("/tmp/test_cmd_session_memo");
    struct edc_session *sess = edc_session_new(cfg, "sess-memo", "Memo");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, sess);
    sched->send_llm = mock_send_llm;

    struct edc_session_manager *sm = sched->sessions;

    /* 一个带记忆的组 (手动策展的材料来源) */
    struct edc_task *leader = edc_task_new("675", "sess-memo", "675", "leader", edc_channel_adapter);
    struct edc_task_group *g = edc_task_group_new(leader);
    edc_session_add_group(sess, g);
    g->memory = sdsnew("group 675 memory: 构建入口是 bazelisk");

    TEST("show (空记忆): 给出说明而不是空回话");
    sds text = edc_session_memory_show_text(sess->memo, NULL);
    CHECK(text && strstr(text, "nothing yet") != NULL, "说明去哪儿找记忆");
    sdsfree(text);

    TEST("/session memo show: 列全部 (索引行 + 总数)");
    sds id1 = seed_entry(sess, "构建入口", "唯一入口是 bazelisk", "明细一");
    sds id2 = seed_entry(sess, "沙箱策略", "uid 1000 + sudo", NULL);
    CHECK(strcmp(id1, "m1") == 0 && strcmp(id2, "m2") == 0, "发号 m1/m2");
    drive_command(cfg, sm, sched, "/session memo show");
    text = edc_session_memory_show_text(sess->memo, NULL);
    CHECK(text && strstr(text, "2 entries") != NULL, "报了总数");
    CHECK(text && strstr(text, "[m1] 构建入口 — 唯一入口是 bazelisk") != NULL, "第一条索引行");
    CHECK(text && strstr(text, "[m2] 沙箱策略") != NULL, "第二条索引行");
    sdsfree(text);

    TEST("/session memo show <id>: 明细 + 来源; 未知 id 给提示");
    text = edc_session_memory_show_text(sess->memo, "m1");
    CHECK(text && strstr(text, "明细一") != NULL, "明细在");
    CHECK(text && strstr(text, "sources: group 675") != NULL, "来源在");
    CHECK(text && strstr(text, "proposed by task task-1") != NULL, "谁写的也在");
    sdsfree(text);
    text = edc_session_memory_show_text(sess->memo, "m99");
    CHECK(text && strstr(text, "no entry") != NULL && strstr(text, "m99") != NULL, "未知 id 说清楚");
    sdsfree(text);

    TEST("/session memo search: 命中 (标题/摘要/明细) 与未命中");
    text = edc_session_memory_search_text(sess->memo, "sandbox-no-such-word");
    CHECK(text && strstr(text, "nothing matches") != NULL, "未命中");
    sdsfree(text);
    text = edc_session_memory_search_text(sess->memo, "bazelisk");
    CHECK(text && strstr(text, "1 hit") != NULL && strstr(text, "[m1]") != NULL, "命中摘要");
    sdsfree(text);
    text = edc_session_memory_search_text(sess->memo, "明细一");
    CHECK(text && strstr(text, "1 hit") != NULL, "命中明细");
    sdsfree(text);
    drive_command(cfg, sm, sched, "/session memo search bazelisk");

    TEST("未知子命令: usage, 不起任何任务");
    g_llm_calls = 0;
    drive_command(cfg, sm, sched, "/session memo frobnicate");
    CHECK(g_llm_calls == 0, "没有请求出去");
    CHECK(find_l2(sess) == NULL, "也没有 L2 被建出来");

    TEST("/session memo curate: 立刻起 L2, 材料含所有组与 focus（收料=它的职责）");
    g_llm_calls = 0;
    g_llm_task = NULL;
    drive_command(cfg, sm, sched, "/session memo curate keep the build flags");
    CHECK(g_llm_calls == 1, "策展请求发出去了");
    struct edc_task *l2 = find_l2(sess);
    CHECK(l2 != NULL && g_llm_task == l2, "出去的就是这个 L2");
    CHECK(l2 && l2->sink == EDC_SINK_SESSION_MEMORY, "sink = SESSION_MEMORY");
    {
        /* 材料 = 组记忆 + focus + 现有条目索引 */
        struct edc_turn *turn = l2 ? l2->active_turn : NULL;
        const char *material = NULL;
        if (turn && edc_queue_length(&turn->messages) > 0)
        {
            cJSON *m = turn->messages.data[turn->messages.head];
            material = cJSON_GetStringValue(cJSON_GetObjectItem(m, "content"));
        }
        CHECK(material && strstr(material, "bazelisk") != NULL, "带上了组记忆");
        CHECK(material && strstr(material, "Additional focus: keep the build flags") != NULL, "focus 原文在");
        CHECK(material && strstr(material, "[m1]") != NULL, "带上了现有条目索引");
    }

    TEST("curate: 已有 system task 在飞 ⇒ 拒绝 (一次一个)");
    g_llm_calls = 0;
    drive_command(cfg, sm, sched, "/session memo curate again");
    CHECK(g_llm_calls == 0, "没有再发请求");

    TEST("L2 压缩任务: 条目超上限 => due; start_compact 起一个同性质但有自己的提示词的任务");
    if (l2)
        edc_task_mark_completed(l2); /* 腾出“一次一个系统任务”的门 */
    while (sess->memo->entry_count <= EDC_SESSION_MEMORY_COMPACT_ENTRIES)
    {
        sds filler = seed_entry(sess, "填充", "s", NULL);
        sdsfree(filler);
    }
    CHECK(edc_session_memory_compact_due(sess), "超过条目上限 => compact_due");

    /* 一个“还在跑”的普通任务（另一个组，验证屏障是全 session 而不只是一个组） */
    struct edc_task_spec uspec;
    memset(&uspec, 0, sizeof(uspec));
    uspec.desc = "runner";
    uspec.input = "hello";
    uspec.channel_type = edc_channel_adapter;
    uspec.place = EDC_TASK_PLACE_FOREGROUND;
    uspec.take_foreground = true;
    struct edc_task *runner = edc_scheduler_task_create(sched, sess, &uspec);
    CHECK(runner != NULL, "另一个组里有个非终态任务");

    struct edc_task *ct = edc_session_memory_start_compact(sched, sess);
    CHECK(ct != NULL, "压缩任务起出来了");
    CHECK(ct && ct->sink == EDC_SINK_SESSION_MEMORY, "sink = SESSION_MEMORY（与策展同一性质）");
    CHECK(ct && ct->memo_compact, "带 memo_compact 标记（请求侧据此选提示词）");
    CHECK(ct && ct->group && edc_task_group_is_system(ct->group), "落在系统组里");
    {
        struct edc_turn *turn = ct ? ct->active_turn : NULL;
        const char *material = NULL;
        if (turn && edc_queue_length(&turn->messages) > 0)
        {
            cJSON *m = turn->messages.data[turn->messages.head];
            material = cJSON_GetStringValue(cJSON_GetObjectItem(m, "content"));
        }
        CHECK(material && strstr(material, "## Compact the session memory") != NULL, "压缩指令在材料里");
        CHECK(material && strstr(material, "id: m1") != NULL && strstr(material, "rev: ") != NULL,
              "全量 dump：每条的 id 与 rev 都在（update 要用）");
    }
    CHECK(strcmp(edc_session_memory_compact_prompt(), edc_session_memory_system_prompt()) != 0,
          "压缩有自己的系统提示词（与策展的不同）");
    CHECK(strstr(edc_session_memory_compact_prompt(), "SMALLER") != NULL, "它要的是变小");

    {
        /* 屏障：还在跑的任务都依赖这个压缩任务，且是 BARRIER（源无论怎么结束都放行） */
        bool barrier = false;
        struct edc_task_dep *dep = NULL;
        LL_FOREACH(runner->deps, dep)
        {
            if (dep->source == ct && dep->type == TASK_DEP_BARRIER && !dep->cancelled)
                barrier = true;
        }
        CHECK(barrier, "还在跑的 task 依赖了压缩任务（TASK_DEP_BARRIER）");

        /* 压缩任务结束 ⇒ 屏障作废、被攥着的任务放行（走 host 的统一结算路径） */
        /* 真的把它结算掉：READY 不能直接 COMPLETED（迁移矩阵），先过一步 AWAIT_EXEC，
         * 然后走 host 的统一结算（那里才会 notify_dependents）。 */
        CHECK(edc_task_await_exec(ct), "压缩任务被驱动");
        CHECK(edc_task_mark_completed(ct), "压缩任务完成");
        struct edc_advance_result res;
        memset(&res, 0, sizeof(res));
        res.outcome = EDC_STEP_DONE;
        res.changed = true;
        res.settled = true;
        edc_scheduler_apply_advance(sched, ct, NULL, &res);
        /* 源正常完成 ⇒ 边标记 completed；源取消/失败 ⇒ 边被作废(cancelled)。
         * 两种都不再攥着目标（BARRIER 的"怎么结束都放行"由 test_edc_task.c 的
         * 失败放行用例钉住）。 */
        bool holds = false;
        LL_FOREACH(runner->deps, dep)
        {
            if (dep->source == ct && !dep->cancelled && !dep->completed)
                holds = true;
        }
        CHECK(!holds, "压缩任务一结束就不再攥着它（边已 completed/cancelled，任务接着跑）");
    }

    CHECK(!edc_session_memory_compact_due(sess), "同尺寸不会立刻再来一轮（要再长才触发）");

    TEST("/session memo compact: 只压 session memory（材料里**没有** group memory）");
    g_llm_calls = 0;
    g_llm_task = NULL;
    drive_command(cfg, sm, sched, "/session memo compact");
    CHECK(g_llm_calls == 1, "压缩请求发出去了");
    {
        struct edc_task *ct2 = NULL;
        struct edc_task_group *gg = NULL;
        DL_FOREACH(sess->groups, gg)
        {
            struct edc_task *tt = NULL;
            LL_FOREACH2(gg->members, tt, group_next)
            {
                if (tt->sink == EDC_SINK_SESSION_MEMORY && tt->memo_compact && !edc_task_is_finished(tt))
                    ct2 = tt;
            }
        }
        CHECK(ct2 != NULL, "起的是压缩任务（带 memo_compact）");
        const char *material = NULL;
        if (ct2 && ct2->active_turn && edc_queue_length(&ct2->active_turn->messages) > 0)
        {
            cJSON *m = ct2->active_turn->messages.data[ct2->active_turn->messages.head];
            material = cJSON_GetStringValue(cJSON_GetObjectItem(m, "content"));
        }
        CHECK(material && strstr(material, "## Compact the session memory") != NULL, "材料里是压缩指令");
        CHECK(material && strstr(material, "id: m1") != NULL, "以及条目 dump（id/rev/内容）");
        CHECK(material && strstr(material, "### group") == NULL,
              "材料里**没有** group memory —— 压缩只动 session memory 自己");
    }

    printf("\n  %d passed, %d failed\n\n", passed, failed);
    edc_scheduler_free(sched);
    edc_session_free(sess);
    edc_config_free(cfg);
    return failed ? 1 : 0;
}
