/* P3: the three overflow detection points and the one notice they share. The
 * provider cannot be made to overflow on demand, so each point is driven through
 * the real executor with the response shape it looks for: a 400 (the body is
 * verbatim from the probe, 2.5.1), a `stop` whose usage is already past the
 * configured window, and a `length` with an empty completion. Throttling is the
 * counterexample that must stay unclassified -- "fold your context" is wrong advice
 * for a 429. */

#include "edc_actor.h"
#include "test_ws.h"
#include "edc_compat.h"
#include "edc_config.h"
#include "edc_exec_io.h"
#include "edc_executor.h"
#include "edc_scheduler.h"
#include "edc_session.h"
#include "edc_session_manager.h"
#include "edc_task.h"
#include "edc_task_group.h"
#include "edc_turn.h"
#include "log.h"
#include "utlist_fix.h"
#include <signal.h>
#include <stdio.h>
#include <string.h>

/* edc_commands is pulled in transitively and references it (same as its siblings). */
volatile sig_atomic_t _run_loop = 1;

static int test_failed = 0;
static int test_passed = 0;

#define TEST(name) log_info("  TEST: %s", name)
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

/* The in-memory session store handed to the scheduler; an empty label means the
 * active session (active_index points at the first). */
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

/* The 400 the real provider returned when the probe pushed 2M tokens at it
 * (verbatim, request id elided). */
static const char *kProvider400 =
    "LLM API error (HTTP 400): This model's maximum context length is 1048576 tokens. However, you requested "
    "2011367 tokens (2011366 in the messages, 1 in the completion). Please reduce the length of the messages or "
    "completion. (request_id: 0000)";

/* `window` = 0 is the config default ("unknown"), and the only thing that decides
 * whether the silent / truncation branches can be judged at all. */
struct fix
{
    struct edc_config *cfg;
    struct edc_session *sess;
    struct edc_scheduler *sched;
    struct edc_task *task;
};

static void give_input(struct edc_task *task, const char *text)
{
    struct edc_turn *turn = edc_turn_new();
    turn->task_id = task->task_id;
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "role", "user");
    cJSON_AddStringToObject(msg, "content", text);
    edc_turn_add_message(turn, msg);
    edc_task_add_turn(task, turn);
    edc_task_await_exec(task); /* 停在"已派发, 等回话" */
}

static void fix_up(struct fix *f, int window)
{
    memset(f, 0, sizeof(*f));
    f->cfg = g_new0(struct edc_config, 1);
    f->cfg->workspace = sdsnew(edc_test_workspace());
    f->cfg->context_window = window;
    f->sess = edc_session_new(f->cfg, edc_test_session_id("sess-overflow"), "Overflow");
    f->sched = edc_scheduler_new(f->cfg);
    install_resolver(f->sched, f->sess);

    f->task = edc_task_new(edc_test_id("t-1"), edc_test_session_id("sess-overflow"), edc_test_id("t-1"), "user task", edc_channel_adapter);
    edc_session_add_group(f->sess, edc_task_group_new(f->task));
    give_input(f->task, "hello");
}

static void fix_down(struct fix *f)
{
    edc_session_free(f->sess);
    edc_scheduler_free(f->sched);
    edc_config_free(f->cfg);
}

static void clear_event(struct edc_event *ev)
{
    memset(ev, 0, sizeof(*ev));
    ev->kind = EDC_EVENT_EXEC_DONE;
}

static struct edc_advance_result drive(struct fix *f, struct edc_event *ev)
{
    return edc_executor_advance(&f->sched->executor, f->task, ev);
}

/* ══ send_reply 捕获: 谁在什么之后说了什么 ═══════════════════════════════ */
#define MAX_REPLIES 4
static int g_reply_calls = 0;
static char g_reply_text[MAX_REPLIES][256];
static edc_channel_message_type g_reply_type[MAX_REPLIES];

static void record_send_reply(struct edc_scheduler *sched, struct edc_task *task, const char *content,
                              edc_channel_message_type message_type, void *request_id)
{
    (void)sched;
    (void)task;
    (void)request_id;
    if (g_reply_calls < MAX_REPLIES)
    {
        snprintf(g_reply_text[g_reply_calls], sizeof(g_reply_text[0]), "%s", content ? content : "");
        g_reply_type[g_reply_calls] = message_type;
    }
    g_reply_calls++;
}

static void reply_capture_reset(void)
{
    g_reply_calls = 0;
    memset(g_reply_text, 0, sizeof(g_reply_text));
    memset(g_reply_type, 0, sizeof(g_reply_type));
}

/* ══════════════════════════════════════════════════════════════
 *  ① 显式: provider 报错
 * ══════════════════════════════════════════════════════════════ */

static void test_explicit_overflow(void)
{
    TEST("① 显式: 400 + provider 原话 ⇒ 失败 + 提示带数字与下一步");

    struct fix f;
    fix_up(&f, 1048576);

    struct edc_event ev;
    clear_event(&ev);
    ev.as.exec.failed = true;
    ev.as.exec.http_status = 400;
    ev.as.exec.error_message = kProvider400;
    ev.as.exec.prompt_tokens = 2011366;
    struct edc_advance_result r = drive(&f, &ev);

    CHECK(r.outcome == EDC_STEP_ERROR, "溢出仍然失败 (裁决 #10: 不自动恢复)");
    CHECK(edc_task_has_failed(f.task), "任务 life=ERROR");
    CHECK(r.notice && strstr(r.notice, "maximum context length is 1048576") != NULL,
          "provider 原话保留 (真数字在它里面)");
    CHECK(r.notice && strstr(r.notice, "context overflow") != NULL, "带上溢出判定");
    CHECK(r.notice && strstr(r.notice, "1.0M") != NULL, "窗口数字来自 config (1048576 => 1.0M)");
    CHECK(r.notice && strstr(r.notice, "2.0M") != NULL, "请求大小来自 usage (2011366 => 2.0M)");
    CHECK(r.notice && strstr(r.notice, "/group fold") != NULL, "带下一步");
    CHECK(r.warning[0] == '\0', "失败路径不走 warning (它本来就在 ERROR 上)");
    fix_down(&f);
}

static void test_rate_limit_is_not_overflow(void)
{
    TEST("① 反例: 429 限流 ⇒ 原话原样, 一个字不加");

    struct fix f;
    fix_up(&f, 1048576);

    struct edc_event ev;
    clear_event(&ev);
    ev.as.exec.failed = true;
    ev.as.exec.http_status = 429;
    ev.as.exec.error_message = "LLM API error (HTTP 429): rate limit exceeded, retry in 20s";
    ev.as.exec.prompt_tokens = 2011366; /* 大得足以像溢出, 但状态码说不是 */
    struct edc_advance_result r = drive(&f, &ev);

    CHECK(r.outcome == EDC_STEP_ERROR, "限流也是失败");
    CHECK(r.notice && strcmp(r.notice, "LLM API error (HTTP 429): rate limit exceeded, retry in 20s") == 0,
          "原话逐字节透传");
    CHECK(r.notice && strstr(r.notice, "context overflow") == NULL, "不判成溢出 (pi 的显式反例)");
    CHECK(r.notice && strstr(r.notice, "/group fold") == NULL, "更不叫它去折叠上下文");
    fix_down(&f);
}

/* ══════════════════════════════════════════════════════════════
 *  ② 静默: 成功但已过窗
 * ══════════════════════════════════════════════════════════════ */

static void test_silent_overflow(void)
{
    TEST("② 静默: stop 但 prompt > 窗口 ⇒ 任务成功 + 另给一条提示");

    struct fix f;
    fix_up(&f, 128000);

    struct edc_event ev;
    clear_event(&ev);
    ev.as.exec.reason = EDC_REASON_STOP;
    ev.as.exec.content = "the answer";
    ev.as.exec.prompt_tokens = 130000;
    ev.as.exec.completion_tokens = 10;
    struct edc_advance_result r = drive(&f, &ev);

    CHECK(r.outcome == EDC_STEP_DONE && edc_task_is_completed(f.task), "任务照常成功 (静默溢出不失败)");
    CHECK(r.notice && strcmp(r.notice, "the answer") == 0, "回复本身没被改");
    CHECK(r.warning[0] != '\0', "另有一条系统提示");
    CHECK(strstr(r.warning, "context overflow") != NULL, "说得明白");
    CHECK(strstr(r.warning, "128.0k") != NULL, "窗口数字来自 config");
    CHECK(strstr(r.warning, "130.0k") != NULL, "请求大小来自 usage");
    CHECK(strstr(r.warning, "/group fold") != NULL, "带下一步");
    fix_down(&f);
}

static void test_silent_needs_a_window(void)
{
    TEST("② 窗口未知 (0) ⇒ 不触发 (拿什么比?)");

    struct fix f;
    fix_up(&f, 0);

    struct edc_event ev;
    clear_event(&ev);
    ev.as.exec.reason = EDC_REASON_STOP;
    ev.as.exec.content = "the answer";
    ev.as.exec.prompt_tokens = 2000000; /* 比上面那个还大, 但窗口未知 */
    struct edc_advance_result r = drive(&f, &ev);

    CHECK(r.outcome == EDC_STEP_DONE, "照常成功");
    CHECK(r.warning[0] == '\0', "没有比较对象就不下结论");
    fix_down(&f);
}

static void test_warning_goes_out_as_a_system_message(void)
{
    TEST("② 投递: 先回复、后系统提示 (apply_advance)");

    struct fix f;
    fix_up(&f, 128000);
    f.sched->send_reply = record_send_reply;
    reply_capture_reset();

    struct edc_event ev;
    clear_event(&ev);
    ev.as.exec.reason = EDC_REASON_STOP;
    ev.as.exec.content = "the answer";
    ev.as.exec.prompt_tokens = 130000;
    void *req = f.task->current_request_id;
    struct edc_advance_result r = drive(&f, &ev);
    edc_scheduler_apply_advance(f.sched, f.task, req, &r);

    CHECK(g_reply_calls == 2, "两条消息: 回复 + 提示");
    if (g_reply_calls >= 2)
    {
        CHECK(g_reply_type[0] == EDC_CHANNEL_MESSAGE_RESP_TYPE, "先是回复");
        CHECK(strstr(g_reply_text[0], "the answer") != NULL, "回复内容原样");
        CHECK(g_reply_type[1] == EDC_CHANNEL_MESSAGE_SYS_TYPE, "再是系统提示");
        CHECK(strstr(g_reply_text[1], "context overflow") != NULL, "提示内容");
    }
    fix_down(&f);
}

/* ══════════════════════════════════════════════════════════════
 *  ③ 截断 + 找回 context_length_exceeded
 * ══════════════════════════════════════════════════════════════ */

static void test_truncated_overflow(void)
{
    TEST("③ 截断: length + completion==0 + prompt >= 99%窗口 ⇒ 溢出; 普通 length 不算");

    /* 正例: 窗口被填满, 答案一个字都没出来 */
    struct fix f;
    fix_up(&f, 128000);
    struct edc_event ev;
    clear_event(&ev);
    ev.as.exec.reason = EDC_REASON_LENGTH;
    ev.as.exec.prompt_tokens = 127000; /* >= 99% * 128000 = 126720 */
    ev.as.exec.completion_tokens = 0;
    struct edc_advance_result r = drive(&f, &ev);

    CHECK(r.outcome == EDC_STEP_ERROR, "落 ERROR");
    CHECK(r.notice && strstr(r.notice, "context overflow") != NULL, "认成溢出");
    CHECK(r.notice && strstr(r.notice, "max_tokens") != NULL, "说清是被 max_tokens 切了");
    CHECK(r.notice && strstr(r.notice, "127.0k") != NULL, "带请求大小");
    CHECK(r.notice && strstr(r.notice, "/group fold") != NULL, "带下一步");
    fix_down(&f);

    /* 反例一: 真被截断 (completion > 0) —— 那是长度问题, 不是窗口问题 */
    struct fix g;
    fix_up(&g, 128000);
    clear_event(&ev);
    ev.as.exec.reason = EDC_REASON_LENGTH;
    ev.as.exec.prompt_tokens = 127000;
    ev.as.exec.completion_tokens = 5;
    r = drive(&g, &ev);
    CHECK(r.outcome == EDC_STEP_ERROR, "照旧失败");
    CHECK(r.notice && strcmp(r.notice, "Internal error: unknown finish reason") == 0, "不判成溢出");
    fix_down(&g);

    /* 反例二: completion==0 但 prompt 远没填满窗口 */
    struct fix h;
    fix_up(&h, 128000);
    clear_event(&ev);
    ev.as.exec.reason = EDC_REASON_LENGTH;
    ev.as.exec.prompt_tokens = 1000;
    ev.as.exec.completion_tokens = 0;
    r = drive(&h, &ev);
    CHECK(r.notice && strstr(r.notice, "context overflow") == NULL, "不满窗就不是溢出");
    fix_down(&h);
}

static void test_context_exceeded_reason_is_reclaimed(void)
{
    TEST("④ context_length_exceeded: 以前当 'unknown finish reason' 丢掉的信号");

    struct fix f;
    fix_up(&f, 128000);

    struct edc_event ev;
    clear_event(&ev);
    ev.as.exec.reason = EDC_REASON_CONTEXT_EXCEEDED;
    ev.as.exec.prompt_tokens = 127000;
    struct edc_advance_result r = drive(&f, &ev);

    CHECK(r.outcome == EDC_STEP_ERROR, "落 ERROR");
    CHECK(r.notice && strstr(r.notice, "context overflow") != NULL, "认成溢出");
    CHECK(r.notice && strstr(r.notice, "the provider named the window") != NULL, "说清是 provider 自己说的");
    CHECK(r.notice && strstr(r.notice, "unknown finish reason") == NULL, "不再是那句没用的兜底");
    fix_down(&f);
}

/* 词表那一格必须真的接上: 从 actor 的 resp 出发 (真边界适配器), 而不是直接摆一个
 * EDC_REASON_* 值 -- 否则"context_length_exceeded 这个信号找回来没有"测不到。 */
static void test_context_exceeded_survives_the_boundary(void)
{
    TEST("④ 边界: actor 的 finish_reason=context_length_exceeded 能穿过适配器");

    struct fix f;
    fix_up(&f, 128000);

    struct edc_llm_actor_resp resp;
    memset(&resp, 0, sizeof(resp));
    resp.reason = BC_LLM_FINISH_REASON_CONTEXT_EXCEEDED;
    resp.usage.prompt_tokens = 127000;
    struct edc_advance_result r = edc_executor_advance_llm_resp(&f.sched->executor, f.task, NULL, &resp);

    CHECK(r.outcome == EDC_STEP_ERROR, "落 ERROR");
    CHECK(r.notice && strstr(r.notice, "context overflow") != NULL, "穿过适配器后仍是溢出 (没被压成 OTHER)");
    CHECK(r.notice && strstr(r.notice, "unknown finish reason") == NULL, "不再是那句没用的兜底");
    fix_down(&f);
}

/* 状态码必须真的随事件走: 分类器靠它把 400 (溢出) 和 429 (限流) 分开, 而它是由
 * advance_llm_resp 从 resp 搬进事件的 —— 绕过适配器直接摆事件就测不到这一段。 */
static void test_status_code_survives_the_boundary(void)
{
    TEST("① 边界: resp.http_status=400 能穿过适配器 (丢掉就只有 TRANSPORT)");

    struct fix f;
    fix_up(&f, 1048576);

    struct edc_llm_actor_resp resp;
    memset(&resp, 0, sizeof(resp));
    resp.reason = BC_LLM_FINISH_REASON_UNKNOWN;
    resp.http_status = 400;
    resp.error = edc_error_new(EDC_ERR_FAILED, "%s", kProvider400);
    resp.usage.prompt_tokens = 2011366;
    struct edc_advance_result r = edc_executor_advance_llm_resp(&f.sched->executor, f.task, NULL, &resp);

    CHECK(r.outcome == EDC_STEP_ERROR, "落 ERROR");
    CHECK(r.notice && strstr(r.notice, "maximum context length is 1048576") != NULL, "provider 原话还在");
    CHECK(r.notice && strstr(r.notice, "context overflow") != NULL, "状态码到了 ⇒ 判成溢出并给建议");
    CHECK(r.notice && strstr(r.notice, "/group fold") != NULL, "带下一步");
    edc_error_free(resp.error);
    fix_down(&f);
}

static void test_fold_task_keeps_the_fold_wording(void)
{
    TEST("折叠任务自己超窗: 沿用折叠话术, 不给'去折叠'的建议, 不递归");

    struct fix f;
    fix_up(&f, 128000);
    struct edc_task *fold = edc_scheduler_spawn_system_task(f.sched, f.sess, "fold group memory");
    CHECK(fold != NULL && fold->sink == EDC_SINK_GROUP_MEMORY, "折叠任务就位");
    if (fold)
    {
        give_input(fold, "summarize the group");
        struct edc_event ev;
        clear_event(&ev);
        ev.as.exec.reason = EDC_REASON_CONTEXT_EXCEEDED;
        ev.as.exec.prompt_tokens = 127000;
        struct edc_advance_result r = edc_executor_advance(&f.sched->executor, fold, &ev);

        CHECK(r.outcome == EDC_STEP_ERROR, "照旧失败");
        CHECK(r.notice && strstr(r.notice, "group memory is unchanged") != NULL, "折叠话术");
        CHECK(r.notice && strstr(r.notice, "/group fold") == NULL, "不叫它自己再去折 (一律不递归)");
        CHECK(r.notice && strstr(r.notice, "the provider named the window") == NULL, "用户任务那套话术不串台");

        /* 同一个护栏在失败路径 (显式 400) 上: 折叠任务不拿"去折叠"的建议 (它*就是*折叠) */
        struct fix g;
        fix_up(&g, 1048576);
        struct edc_task *fold2 = edc_scheduler_spawn_system_task(g.sched, g.sess, "fold group memory");
        if (fold2)
        {
            give_input(fold2, "summarize the group");
            struct edc_event ev2;
            clear_event(&ev2);
            ev2.as.exec.failed = true;
            ev2.as.exec.http_status = 400;
            ev2.as.exec.error_message = kProvider400;
            ev2.as.exec.prompt_tokens = 2011366;
            struct edc_advance_result r2 = edc_executor_advance(&g.sched->executor, fold2, &ev2);
            CHECK(r2.outcome == EDC_STEP_ERROR, "照旧失败");
            CHECK(r2.notice && strstr(r2.notice, "maximum context length is") != NULL, "provider 原话还在");
            CHECK(r2.notice && strstr(r2.notice, "/group fold") == NULL, "没给'去 /group fold'的建议");
        }
        fix_down(&g);
    }
    fix_down(&f);
}

/* ══════════════════════════════════════════════════════════════
 *  传输失败自动重发一次 (no HTTP response => TRANSPORT)
 * ══════════════════════════════════════════════════════════════ */

static int g_send_llm_calls = 0;
static void stub_send_llm(struct edc_scheduler *sched, struct edc_task *task, void *request_id)
{
    (void)sched;
    (void)task;
    (void)request_id;
    g_send_llm_calls++;
}

static void test_transport_failure_retried_once(void)
{
    TEST("传输失败(无响应): 自动重发一次; 第二次才失败");

    struct fix f;
    fix_up(&f, 1048576);
    f.sched->send_llm = stub_send_llm;
    g_send_llm_calls = 0;

    struct edc_event ev;
    clear_event(&ev);
    ev.as.exec.failed = true;   /* 没有 HTTP 响应 */
    ev.as.exec.http_status = 0; /* => TRANSPORT */
    ev.as.exec.error_message = "request timed out after 300000 ms";

    struct edc_advance_result r1 = drive(&f, &ev);
    CHECK(r1.outcome == EDC_STEP_BLOCKED, "第一次: 不失败, 重发");
    CHECK(!edc_task_has_failed(f.task), "任务还活着");
    CHECK(edc_task_is_running(f.task), "仍在 AWAIT_EXEC (还能接回话)");
    CHECK(g_send_llm_calls == 1, "重发了 1 次请求");

    struct edc_advance_result r2 = drive(&f, &ev);
    CHECK(r2.outcome == EDC_STEP_ERROR, "第二次传输失败: 不再重发, 失败");
    CHECK(edc_task_has_failed(f.task), "任务 life=ERROR");
    CHECK(g_send_llm_calls == 1, "没有第三次请求");

    fix_down(&f);
}

static void test_http_failure_is_not_retried(void)
{
    TEST("有 HTTP 响应的失败(401)不在此处重发");

    struct fix f;
    fix_up(&f, 1048576);
    f.sched->send_llm = stub_send_llm;
    g_send_llm_calls = 0;

    struct edc_event ev;
    clear_event(&ev);
    ev.as.exec.failed = true;
    ev.as.exec.http_status = 401;
    ev.as.exec.error_message = "LLM API error (HTTP 401): invalid api key";

    struct edc_advance_result r = drive(&f, &ev);
    CHECK(r.outcome == EDC_STEP_ERROR, "直接失败, 不重发");
    CHECK(edc_task_has_failed(f.task), "任务 life=ERROR");
    CHECK(g_send_llm_calls == 0, "一次重发都没有");

    fix_down(&f);
}

int main(void)
{
    edc_test_workspace(); /* open the disposable store before anything writes */
    log_info("=== test_executor_overflow ===");

    test_explicit_overflow();
    test_rate_limit_is_not_overflow();
    test_silent_overflow();
    test_silent_needs_a_window();
    test_warning_goes_out_as_a_system_message();
    test_truncated_overflow();
    test_context_exceeded_reason_is_reclaimed();
    test_context_exceeded_survives_the_boundary();
    test_status_code_survives_the_boundary();
    test_fold_task_keeps_the_fold_wording();
    test_transport_failure_retried_once();
    test_http_failure_is_not_retried();

    log_info("=== %d passed, %d failed ===", test_passed, test_failed);
    printf("%d passed, %d failed\n", test_passed, test_failed);
    return test_failed == 0 ? 0 : 1;
}
