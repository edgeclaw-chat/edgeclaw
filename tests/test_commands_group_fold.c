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
#include "test_ws.h"
#include "edc_commands.h"
#include "edc_executor.h"
#include "edc_llm_actor.h"
#include "edc_compat.h"
#include "edc_config.h"
#include "edc_group_fold.h"
#include "edc_fold_files.h"
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

/* Fixture ids live at the TOP, descending. The store allocates real task ids with
 * AUTOINCREMENT, and an explicit insert raises its sequence to the inserted value -- so a
 * fixture in the low band would eventually be met by a database-allocated id (it was: the
 * fold's system task was handed the id of a fixture group). Descending keeps them apart.
 *
 * The band is capped at 1e15 because every id travels through cJSON, which stores numbers as
 * `double`: above 2^53 (~9e15) an integer stops being exactly representable, and the id read
 * back from the snapshot would no longer match the row it names. */
#define FOLD_FIXTURE_ID_BASE 1000000000000000LL
static int64_t fold_fixture_id(const char *name)
{
    return FOLD_FIXTURE_ID_BASE - edc_test_id(name);
}

/* 一个可折叠的组: leader 已完成 + 带一个 turn (两条消息)。 */
static struct edc_task_group *make_group(struct edc_session *sess, const char *id)
{
    struct edc_task *leader = edc_task_new(fold_fixture_id(id), sess->session_id, fold_fixture_id(id),
                                          "leader work", edc_channel_adapter);
    struct edc_task_group *g = edc_task_group_new(leader);
    edc_session_add_group(sess, g);
    /* The turn is created ALREADY bound to the leader's key. Building it first and then
     * assigning turn->task_id would leave the messages it wrote under the constructor's
     * placeholder key, where nothing looks for them afterwards. */
    struct edc_turn *turn = edc_turn_new_for(sess->session_id, leader->group_id, leader->task_id, 0);
    for (int i = 0; i < 2; i++)
    {
        cJSON *m = cJSON_CreateObject();
        cJSON_AddStringToObject(m, "role", "user");
        cJSON_AddStringToObject(m, "content", "old work");
        edc_turn_add_message(turn, m);
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

    /* L2 (P4d): 折叠落地会链出一个策展任务, 而被折组的成员现在等的是它。
     * 这个 harness 关心的是命令层, 所以这里把它也结算掉 —— 否则下个用例捕获到的
     * "最后一个 LLM 请求"会是 L2 的 (它的 sink_group 是上一个组, prompt 也不同)。 */
    struct edc_session *sess = (fold->group && fold->group->session) ? fold->group->session : NULL;
    struct edc_task_group *sys = sess ? edc_session_find_system_group(sess) : NULL;
    if (!sys)
        return;
    edc_scheduler_schedule(sched); /* 先让扫描把 L2 派发出去 (它才有 request_id 可结算) */
    struct edc_task *t = NULL;
    LL_FOREACH2(sys->members, t, group_next)
    {
        if (t->sink != EDC_SINK_SESSION_MEMORY || edc_task_is_completed(t) || edc_task_has_failed(t))
            continue;
        void *l2rid = t->current_request_id;
        struct edc_llm_actor_resp l2resp;
        memset(&l2resp, 0, sizeof(l2resp));
        l2resp.reason = BC_LLM_FINISH_REASON_STOP;
        l2resp.content = sdsnew("{\"ops\":[]}");
        struct edc_advance_result l2res = edc_executor_advance_llm_resp(&sched->executor, t, l2rid, &l2resp);
        edc_scheduler_apply_advance(sched, t, l2rid, &l2res);
        sdsfree(l2resp.content);
    }
}

/* 折叠任务 turn 里第一条消息的内容 (折叠 prompt)。 */
static const char *fold_prompt_of(struct edc_task *fold)
{
    if (!fold || !fold->active_turn || fold->active_turn->msg_count == 0)
        return NULL;
    cJSON *m = edc_turn_msg_at(fold->active_turn, 0);
    cJSON *content = m ? cJSON_GetObjectItem(m, "content") : NULL;
    return (content && cJSON_IsString(content)) ? content->valuestring : NULL;
}

int main(void)
{
    edc_test_workspace(); /* open the disposable store before anything writes */
    struct edc_config *cfg = g_new0(struct edc_config, 1);
    cfg->workspace = sdsnew(edc_test_workspace());
    /* 窗口宽到不可能自动折: 这个文件只验手动路径 (due 那条判据由 test_group_fold 覆盖)。 */
    cfg->context_window = 1024 * 1024;
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    struct edc_session *sess = edc_session_new(cfg, "sess-foldcmd", "FoldCmd");
    install_resolver(sched, sess);
    sched->send_llm = mock_send_llm;

    struct edc_task_group *g1 = make_group(sess, "fold-g11");
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
    struct edc_task_group *g2 = make_group(sess, "fold-g12");
    CHECK(g2 != g1, "第二个组");
    g_llm_calls = 0;
    g_llm_task = NULL;
    char fold12[128];
    snprintf(fold12, sizeof(fold12), "/group fold %s keep the build flags as they are",
             edc_id_str(fold_fixture_id("fold-g12")));
    drive_command(cfg, sched->sessions, sched, fold12);
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
    struct edc_task_group *g3 = make_group(sess, "fold-g13");
    g_llm_calls = 0;
    g_llm_task = NULL;
    char fold13[64];
    snprintf(fold13, sizeof(fold13), "/group fold %s", edc_id_str(fold_fixture_id("fold-g13")));
    drive_command(cfg, sched->sessions, sched, fold13);
    CHECK(g_llm_calls == 1, "第一次起来了 (这个组确实有可折的内容)");
    struct edc_task *pending_fold = g_llm_task; /* 后面 /task fg 那条用例拿它当系统组任务 */
    g_llm_calls = 0;
    g_llm_task = NULL;
    drive_command(cfg, sched->sessions, sched, fold13);
    CHECK(g_llm_calls == 0, "第二次没有请求 (一次一个 session)");
    (void)g3;

    TEST("/task fg refuses a task of the system group");
    struct edc_task_group *sys = edc_session_find_system_group(sess);
    CHECK(pending_fold && sys && pending_fold->group == sys, "那个在飞的折叠就在系统组里");
    if (pending_fold && sys)
    {
        struct edc_task_group *fg_before = edc_scheduler_get_foreground(sess);
        char cmd[128];
        snprintf(cmd, sizeof(cmd), "/task fg %s", edc_id_str(pending_fold->task_id));
        drive_command(cfg, sched->sessions, sched, cmd);
        CHECK(edc_scheduler_get_foreground(sess) == fg_before, "前台没动");
        CHECK(edc_scheduler_get_foreground(sess) != sys, "也没有变成系统组");
        CHECK(edc_session_get_active_task(sess) != pending_fold, "折叠任务也没变成 active");
    }

    TEST("/group info: 一个组的上下文 / 折叠数字");
    struct edc_task_group *gi = make_group(sess, "fold-g15");
    sds info = edc_commands_group_info_text(sess, gi, cfg);
    CHECK(info && strstr(info, "fold line") && strstr(info, "1032192"), "带折线 (window − 16384)");
    CHECK(info && strstr(info, "1 members"), "带成员数");
    CHECK(info && strstr(info, "never folded"), "从没折过时说得清楚");
    CHECK(info && strstr(info, "not due"), "内容还很少 ⇒ 不到线");
    CHECK(info && strstr(info, "0/3"), "带连续失败计数");
    CHECK(info && strstr(info, "1 foldable"), "带可折数");
    sdsfree(info);

    g_llm_calls = 0;
    g_llm_task = NULL;
    char info15[64];
    snprintf(info15, sizeof(info15), "/group info %s", edc_id_str(fold_fixture_id("fold-g15")));
    drive_command(cfg, sched->sessions, sched, info15);
    CHECK(g_llm_calls == 0, "只报告, 不折任何东西");
    g_llm_calls = 0;
    g_llm_task = NULL;
    drive_command(cfg, sched->sessions, sched, "/group info");
    CHECK(g_llm_calls == 0, "不带 id 看前台组, 也不折");

    TEST("/group files: 被折叠走过的文件（含空态与免责声明）");
    struct edc_task_group *gf = make_group(sess, "fold-g16");
    /* ① 从没折过 */
    sds files = edc_commands_group_files_text(gf);
    {
        /* The heading names the group by its id, which is the fixture band value now. */
        sds want = sdscatprintf(sdsempty(), "## Tracked files in `%s`",
                                edc_id_str(fold_fixture_id("fold-g16")));
        CHECK(files && strstr(files, want) != NULL, "带组标题");
        sdsfree(want);
    }
    CHECK(files && strstr(files, "never been folded"), "没折过就说没折过");
    CHECK(files && strstr(files, "not a live view"), "空态也带免责声明");
    sdsfree(files);

    /* ② 结构里有清单（折叠落地写出来的形状） */
    gf->memory = sdsnew("# Summary\n- work done");
    gf->fold_files = edc_fold_files_new();
    edc_fold_files_add_read(gf->fold_files, "src/a.c");
    edc_fold_files_add_read(gf->fold_files, "src/");
    edc_fold_files_add_modified(gf->fold_files, "src/b.c");
    files = edc_commands_group_files_text(gf);
    CHECK(files && strstr(files, "### Modified (1)"), "先列 modified 与其条数");
    CHECK(files && strstr(files, "### Read (2)"), "再列 read 与其条数");
    CHECK(files && strstr(files, "- `src/b.c`"), "改过的路径成条");
    CHECK(files && strstr(files, "- `src/a.c`"), "读过的路径成条");
    CHECK(files && strstr(files, "### Modified (1)") && strstr(files, "### Read (2)") &&
              strstr(files, "### Modified (1)") < strstr(files, "### Read (2)"),
          "modified 在 read 前面");
    CHECK(files && strstr(files, "not a live view") && strstr(files, "exec_shell"), "有内容也带免责声明");
    sdsfree(files);

    /* ③ 有 memory 但结构为空（旧档案 / 语料没碰文件）：不能说成"没折过"；
     *    同时钉住"文本块不再是数据源"——往 memory 里塞块不会凭空变成清单。 */
    gf->memory = sdsnew("# Summary\n- only prose <read-files>\nfake.c\n</read-files>\n");
    edc_fold_files_free(gf->fold_files);
    gf->fold_files = NULL;
    files = edc_commands_group_files_text(gf);
    CHECK(files && strstr(files, "No files recorded"), "有 memory 无结构 ⇒ 说没记录, 不误报");
    CHECK(files && strstr(files, "never been folded") == NULL, "不能说成没折过");
    CHECK(files && strstr(files, "fake.c") == NULL, "memory 里的文本块不再被当成清单解析");
    sdsfree(files);
    sdsfree(gf->memory);
    gf->memory = NULL;

    /* ④ NULL 组 / 命令路径不崩、不带 id 看前台组 */
    files = edc_commands_group_files_text(NULL);
    CHECK(files && strstr(files, "no such group"), "NULL 组说得清楚");
    sdsfree(files);
    g_llm_calls = 0;
    drive_command(cfg, sched->sessions, sched, "/group files 16");
    drive_command(cfg, sched->sessions, sched, "/group files");
    CHECK(g_llm_calls == 0, "只读, 不折任何东西");

    edc_session_free(sess);
    edc_scheduler_free(sched);
    edc_config_free(cfg);

    printf("\n  %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
