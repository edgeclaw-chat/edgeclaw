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

    TEST("/session memo compact: 立刻起 L2, 材料含所有组与 focus");
    g_llm_calls = 0;
    g_llm_task = NULL;
    drive_command(cfg, sm, sched, "/session memo compact keep the build flags");
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

    TEST("compact: 已有 system task 在飞 ⇒ 拒绝 (一次一个)");
    g_llm_calls = 0;
    drive_command(cfg, sm, sched, "/session memo compact again");
    CHECK(g_llm_calls == 0, "没有再发请求");

    printf("\n  %d passed, %d failed\n\n", passed, failed);
    edc_scheduler_free(sched);
    edc_session_free(sess);
    edc_config_free(cfg);
    return failed ? 1 : 0;
}
