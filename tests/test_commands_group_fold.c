/* `/group fold [id] [focus]` (决定 8): 手动折一次。
 *
 * 钉住四件:
 *   ① **越过触发判据** —— 阈值/窗口都没到也照折 (手动就是要立刻折);
 *   ② id 省略 = 前台组; 给了 id 就按它解析 (组 id = 组头那条 task id);
 *   ③ focus 原文 (含空格) 进摘要 prompt 的 "Additional focus:";
 *   ④ 屏障一条不松 —— 已有折叠在飞时不起第二次。
 *
 * harness 抄 test_commands_task_new.c: 走公开命令入口 edc_commands_handler, 末尾自己
 * 调 edc_scheduler_schedule (消息边界就是 tick)。没有通道 actor ⇒ 回话只给类型
 * (收不到), 所以断言看**行为**: 谁被折、prompt 是什么。 */
#include "edc_channel_actor.h"
#include "edc_commands.h"
#include "edc_executor.h"
#include "edc_llm_actor.h"
#include "edc_compat.h"
#include "edc_config.h"
#include "edc_group_fold.h"
#include "edc_scheduler.h"
#include "edc_session.h"
#include "edc_session_manager.h"
#include "edc_task.h"
#include "edc_task_group.h"
#include "edc_turn.h"
#include "utlist_fix.h"
#include <cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Stub for _run_loop pulled in by edc_commands (transitively linked). */
volatile int _run_loop = 1;

static int passed = 0, failed = 0;
#define TEST(n)                                                                                                        \
    do                                                                                                                 \
    {                                                                                                                  \
        printf("  TEST: %-55s ", n);                                                                                   \
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
    sched->sessions = sm; /* 测试进程内不释放: 与测试同生共死 */
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
    edc_scheduler_schedule(sched); /* 消息末尾的扫描 = "这条消息处理完了" */
}

/* 一个可折叠的组: leader 已完成 + 带一个 turn (两条消息)。 */
static struct edc_task_group *make_group(struct edc_session *sess, const char *id)
{
    struct edc_task *leader = edc_task_new(id, sess->session_id, id, "leader work", edc_channel_adapter);
    struct edc_task_group *g = edc_task_group_new(leader);
    edc_session_add_group(sess, g);
    struct edc_turn *turn = edc_turn_new();
    turn->task_id = strdup(leader->task_id);
    for (int i = 0; i < 2; i++)
    {
        cJSON *m = cJSON_CreateObject();
        cJSON_AddStringToObject(m, "role", "user");
        cJSON_AddStringToObject(m, "content", "old work");
        edc_queue_push(&turn->messages, m);
    }
    edc_task_add_turn(leader, turn);
    edc_task_await_exec(leader);
    edc_task_mark_completed(leader);
    leader->completion_seq = 1;
    leader->output = sdsnew("old work");
    return g;
}

/* 把一个在飞的折叠跑完 (喂一条 STOP 应答), 让屏障松开 —— 后面的用例才有干净起点。 */
static void finish_fold(struct edc_scheduler *sched, struct edc_task *fold)
{
    /* 折叠没起来时这里必须只是 no-op —— 否则变异体 (比如"手动也判 due") 会空指针崩,
     * 而不是干净地红在断言上 (bc021ce 的教训)。 */
    if (!fold)
        return;
    void *rid = fold->current_request_id;
    struct edc_llm_actor_resp resp;
    memset(&resp, 0, sizeof(resp));
    resp.reason = BC_LLM_FINISH_REASON_STOP;
    resp.content = sdsnew("# merged");
    struct edc_advance_result res = edc_executor_advance_llm_resp(&sched->executor, fold, rid, &resp);
    edc_scheduler_apply_advance(sched, fold, rid, &res);
    sdsfree(resp.content);
}

/* 折叠任务 turn 里第一条消息的内容 (折叠 prompt)。 */
static const char *fold_prompt_of(struct edc_task *fold)
{
    if (!fold || !fold->active_turn || edc_queue_length(&fold->active_turn->messages) == 0)
        return NULL;
    cJSON *m = fold->active_turn->messages.data[fold->active_turn->messages.head];
    cJSON *content = m ? cJSON_GetObjectItem(m, "content") : NULL;
    return (content && cJSON_IsString(content)) ? content->valuestring : NULL;
}

int main(void)
{
    struct edc_config *cfg = g_new0(struct edc_config, 1);
    cfg->workspace = sdsnew("/tmp/test_commands_group_fold_ws");
    /* 窗口宽到不可能自动折: 这个文件只验手动路径 (due 那条判据由 test_group_fold 覆盖)。 */
    cfg->context_window = 1024 * 1024;
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    struct edc_session *sess = edc_session_new(cfg, "sess-foldcmd", "FoldCmd");
    install_resolver(sched, sess);
    sched->send_llm = mock_send_llm;

    struct edc_task_group *g1 = make_group(sess, "11");
    edc_scheduler_set_foreground(sess, g1);

    TEST("判据没到 (对照): 窗口宽 ⇒ 自动触发不会折");
    CHECK(!edc_group_fold_due(g1, cfg), "due = false");
    CHECK(!edc_scheduler_has_pending_system_task(sess), "还没有折叠任务");

    TEST("/group fold (无 id): 前台组, 越过判据立刻折");
    drive_command(cfg, sched->sessions, sched, "/group fold");
    CHECK(g_llm_calls == 1, "折叠请求发出去了 (判据没到也折)");
    CHECK(g_llm_task && g_llm_task->sink == EDC_SINK_GROUP_MEMORY, "出去的是折叠任务 (sink=GROUP_MEMORY)");
    CHECK(g_llm_task && g_llm_task->sink_group == g1, "折的是前台组");
    CHECK(fold_prompt_of(g_llm_task) && strstr(fold_prompt_of(g_llm_task), "<group-transcript>") != NULL,
          "带的是折叠 prompt (不是普通对话)");
    CHECK(fold_prompt_of(g_llm_task) && strstr(fold_prompt_of(g_llm_task), "Additional focus:") == NULL,
          "没给 focus ⇒ 没有 Additional focus 行");
    finish_fold(sched, g_llm_task); /* 屏障松开 (折叠没起来时是 no-op), 后面才有干净起点 */

    TEST("/group fold <id> <focus>: 按 id 找到那个组, focus 原文进 prompt");
    struct edc_task_group *g2 = make_group(sess, "12");
    CHECK(g2 != g1, "第二个组");
    g_llm_calls = 0;
    g_llm_task = NULL;
    drive_command(cfg, sched->sessions, sched, "/group fold 12 keep the build flags as they are");
    CHECK(g_llm_calls == 1, "请求发出去了");
    CHECK(g_llm_task && g_llm_task->sink_group == g2, "折的是 id 指定的那个组 (不是前台组)");
    const char *p = fold_prompt_of(g_llm_task);
    CHECK(p && strstr(p, "Additional focus: keep the build flags as they are\n") != NULL,
          "focus 原文 (含空格) 追加在指令之后, 到行尾");
    finish_fold(sched, g_llm_task);

    TEST("/group fold <不存在的 id>: 不起折叠 (也不偷偷折前台组)");
    g_llm_calls = 0;
    g_llm_task = NULL;
    drive_command(cfg, sched->sessions, sched, "/group fold no-such-group whatever");
    CHECK(g_llm_calls == 0, "没有请求");

    TEST("屏障: 同一个组已有折叠在飞 ⇒ 第二次 /group fold 不起");
    struct edc_task_group *g3 = make_group(sess, "13");
    g_llm_calls = 0;
    g_llm_task = NULL;
    drive_command(cfg, sched->sessions, sched, "/group fold 13");
    CHECK(g_llm_calls == 1, "第一次起来了 (这个组确实有可折的内容)");
    g_llm_calls = 0;
    g_llm_task = NULL;
    drive_command(cfg, sched->sessions, sched, "/group fold 13");
    CHECK(g_llm_calls == 0, "第二次没有请求 (一次一个 session)");
    (void)g3;

    edc_session_free(sess);
    edc_scheduler_free(sched);
    edc_config_free(cfg);

    printf("\n  %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
