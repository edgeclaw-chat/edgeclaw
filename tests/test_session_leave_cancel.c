/* Regression tests for the session "leave / teardown" cancel semantics:
 *
 *   切换 session (/session switch|new) **不取消**任何任务 —— session 像终端
 *   tab, 切走继续在后台执行 (见 edc_commands_handle_session_switch)。
 *   真正会取消任务的只有:
 *     - 进程退出: running_only=true, 只把该 session 里**正在执行**的任务
 *       (LLM_PROCESSING / TOOL_CALLING) 打成 CANCELLED; WAITING_INPUT /
 *       WAITING_DEPS 原样保留。退出前先落盘, 磁盘留在途态, 加载
 *       时统一收敛为 INTERRUPTED (用户可选择 resume)。
 *     - /session remove: running_only=false, 用户明确要移除 → 全取消。
 *
 * 覆盖三个层面:
 *   1. edc_scheduler_send_signal_to_session(..., running_only=true) 只命中指定
 *      session 的运行中任务: 不碰别的 session, 不碰终态 (COMPLETED), 不碰
 *      WAITING_INPUT; running_only=false 仍然全清;
 *   2. 信号 + save 之后反序列化回来: 终态 CANCELLED 原样保留; 未完成的
 *      WAITING_INPUT 落地后加载时统一收敛为 INTERRUPTED (由用户 resume 再判定);
 *   3. 重启安全网: 中断的在途任务 (LLM_PROCESSING) 加载后为 INTERRUPTED, 而不是
 *      变回 WAITING_INPUT 假装什么都没发生。
 *
 *  注: 加载归一化 (edc_state_serializer 的 normalize_interrupted_state) 现在把
 *  **所有非终态** (WAITING_INPUT / WAITING_DEPS / LLM_PROCESSING / TOOL_CALLING)
 *  统一变成 INTERRUPTED —— 打开后先全部停住, 由 /task resume 重新判定。
 */

#include "edc_channel_actor.h"
#include "test_ws.h"
#include "edc_compat.h"
#include "edc_config.h"
#include "edc_file_utils.h"
#include "edc_scheduler.h"
#include "edc_session.h"
#include "edc_session_manager.h"
#include "edc_state_serializer.h"
#include "edc_task.h"
#include "edc_task_group.h"
#include "edc_turn.h"
#include "utlist_fix.h"
#include "log.h"
#include <signal.h>
#include <stdio.h>
#include <string.h>
/* ── 单测的 session 仓库 ─────────────────────────────────────────
 * scheduler 现在**同包直调 store** (P3 的 `resolve_session` 端口已退休),
 * 所以测试给它的不是"解析函数"而是**仓库**: 一个只装着
 * 这些 session 的内存 manager (普通结构体, 没有构造函数 —— 与 poke 别的结构体同法)。
 * 标签为空 = 用活跃的那个 (active_index 指向第一个), 与真 store 的策略一致。
 * `ctx` 仍留给测试自己用 (它不再是读口的入参)。 */
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
/* Stub for _run_loop pulled in by edc_commands */
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

#define TEST_WS edc_test_workspace()

static struct edc_config *make_config(void)
{
    struct edc_config *cfg = g_new0(struct edc_config, 1);
    cfg->workspace = sdsnew(TEST_WS);
    return cfg;
}

static void clean_workspace(void)
{
    /* Wipe the rows, not the file: the store is open on this workspace. */
    edc_test_store_reset();
}

static struct edc_task *find_task(struct edc_session *session, const char *task_id)
{
    struct edc_task_group *g;
    DL_FOREACH(session->groups, g)
    {
        struct edc_task *t;
        LL_FOREACH2(g->members, t, group_next)
        {
            if (strcmp(t->task_id, task_id) == 0)
                return t;
        }
    }
    return NULL;
}

/* fixture 要摆出的相位。以前这里传旧状态枚举 —— 但"在哪一步"和"活着吗"已经是两件事,
 * 而且场景只能通过**合法路径**摆出来 (旧词里那种"直接 set 成 COMPLETED"已经不存在)。 */
typedef enum
{
    SCENE_READY,     /* 带着 input 就绪 (新建即此态) */
    SCENE_RUNNING,   /* 已发出 LLM 请求, 在等回话 */
    SCENE_COMPLETED,
    SCENE_CANCELLED,
} scene;

/* Build a task in `scene` through a legal transition chain (READY -> ... -> target). */
static struct edc_task *make_task(const char *id, const char *sid, const char *desc, scene target)
{
    struct edc_task *t = edc_task_new(id, sid, id, desc, edc_channel_adapter);
    /* 新模型: task 创建时就带着它的 input。fixture 必须装上一个 turn, 否则它就是
     * 一条 turn-less 残渣 —— 落盘时会被判据丢弃。 */
    struct edc_turn *turn = edc_turn_new();
    turn->task_id = strdup(id);
    edc_turn_add_user_message(turn, desc);
    t->active_turn = turn;
    switch (target)
    {
    case SCENE_READY:
        break;
    case SCENE_RUNNING:
        edc_task_await_exec(t);
        break;
    case SCENE_COMPLETED:
        edc_task_await_exec(t);
        edc_task_mark_completed(t);
        break;
    case SCENE_CANCELLED:
        edc_task_send_signal(t, TASK_SIGTERM);
        break;
    }
    return t;
}

/* ══════════════════════════════════════════════════════════════
 *  Test 1: leaving a session cancels only its *running* tasks
 *  ══════════════════════════════════════════════════════════════ */

static void test_signal_scoped_to_session(void)
{
    TEST("send_signal_to_session(running_only) cancels only running tasks of that session");
    clean_workspace();

    struct edc_config *cfg = make_config();

    /* Session A (bound to the scheduler) */
    struct edc_session *sa = edc_session_new(cfg, "sess-a", "A");
    struct edc_scheduler *sched_a = edc_scheduler_new(cfg);
    install_resolver(sched_a, sa);
    struct edc_task *a_run = make_task("a-run", "sess-a", "running", SCENE_RUNNING);
    struct edc_task_group *ga = edc_task_group_new(a_run);
    edc_session_add_group(sa, ga);
    struct edc_task *a_done = make_task("a-done", "sess-a", "done", SCENE_COMPLETED);
    edc_task_group_add_task(ga, a_done);
    /* 非运行态: 离开时必须原样保留, 不能被"离开即取消"误伤 */
    struct edc_task *a_wait = make_task("a-wait", "sess-a", "waiting input", SCENE_READY);
    edc_task_group_add_task(ga, a_wait);

    /* Session B, NOT bound to the scheduler */
    struct edc_session *sb = edc_session_new(cfg, "sess-b", "B");
    struct edc_scheduler *sched_b = edc_scheduler_new(cfg);
    install_resolver(sched_b, sb);
    struct edc_task *b_run = make_task("b-run", "sess-b", "other", SCENE_RUNNING);
    struct edc_task *b_wait = make_task("b-wait", "sess-b", "other wait", SCENE_READY);
    struct edc_task_group *gb = edc_task_group_new(b_run);
    edc_task_group_add_task(gb, b_wait);
    edc_session_add_group(sb, gb);

    /* Leaving session A (running_only=true): only A's running task goes. */
    edc_scheduler_send_signal_to_session(sched_a, sa, TASK_SIGTERM, true);

    CHECK(edc_task_is_cancelled(a_run), "target session running task -> CANCELLED");
    CHECK(edc_task_is_ready(a_wait), "READY task preserved on leave");
    CHECK(edc_task_is_completed(a_done), "terminal COMPLETED task untouched");
    CHECK(edc_task_awaits_exec(b_run), "other session's running task untouched");
    CHECK(edc_task_is_ready(b_wait), "other session's waiting task untouched");

    /* Contrast: cancel-all (running_only=false, used by /stop and teardown)
     * still wipes every non-terminal task, WAITING_INPUT included. */
    edc_scheduler_send_signal_to_session(sched_a, sa, TASK_SIGTERM, false);
    CHECK(edc_task_is_cancelled(a_wait), "cancel-all still cancels WAITING_INPUT");

    edc_scheduler_free(sched_a);
    edc_scheduler_free(sched_b);
    edc_session_free(sa);
    edc_session_free(sb);
    edc_config_free(cfg);
    clean_workspace();
}

/* ══════════════════════════════════════════════════════════════
 *  Test 2: leaving persists CANCELLED (round-trip through session.json)
 *  ══════════════════════════════════════════════════════════════ */

static void test_leave_persists_cancelled(void)
{
    TEST("cancel + save -> reload: CANCELLED stays, non-terminal -> INTERRUPTED");
    clean_workspace();

    struct edc_config *cfg = make_config();

    struct edc_session *sa = edc_session_new(cfg, "sess-lv", "Leave");
    struct edc_scheduler *sched_a = edc_scheduler_new(cfg);
    install_resolver(sched_a, sa);
    struct edc_task *a_run = make_task("t1", "sess-lv", "running", SCENE_RUNNING);
    struct edc_task_group *ga = edc_task_group_new(a_run);
    edc_session_add_group(sa, ga);
    struct edc_task *a_wait = make_task("t2", "sess-lv", "waiting", SCENE_READY);
    edc_task_group_add_task(ga, a_wait);
    edc_scheduler_set_foreground(sa, ga);

    /* "Leave" = cancel running + persist before switching away. */
    edc_scheduler_send_signal_to_session(sched_a, sa, TASK_SIGTERM, true);
    CHECK(edc_task_is_cancelled(a_run), "running task cancelled in memory");
    CHECK(edc_task_is_ready(a_wait), "READY left alone in memory");
    CHECK(edc_session_save_state(sa, sched_a), "save_state succeeds");

    /* Reload into a fresh session (simulates switch-back / restart). */
    struct edc_session *sa2 = edc_session_new(cfg, "sess-lv", "Leave");
    struct edc_scheduler *sched_a2 = edc_scheduler_new(cfg);
    install_resolver(sched_a2, sa2);
    CHECK(edc_session_load_state(sa2, sched_a2), "load_state succeeds");

    struct edc_task *rt = find_task(sa2, "t1");
    CHECK(rt != NULL, "cancelled task restored");
    CHECK(rt != NULL && edc_task_is_cancelled(rt), "terminal CANCELLED passes through unchanged");
    struct edc_task *rt2 = find_task(sa2, "t2");
    CHECK(rt2 != NULL && edc_task_is_interrupted(rt2), "a parked, unfinished task comes back INTERRUPTED");

    edc_scheduler_free(sched_a);
    edc_scheduler_free(sched_a2);
    edc_session_free(sa);
    edc_session_free(sa2);
    edc_config_free(cfg);
    clean_workspace();
}

/* ══════════════════════════════════════════════════════════════
 *  Test 3: restart safety net — an interrupted in-flight task must
 *          not come back as WAITING_INPUT (it was stopped, so CANCELLED)
 *  ══════════════════════════════════════════════════════════════ */

/* ══════════════════════════════════════════════════════════════
 *  Test 2b: "取消 + 落盘" 这一步**自己**就把盘写了
 *
 *  这一条以前靠一个调用计数探针 (`sched->save_session`) 钉住 —— 那个端口已退休
 *  (scheduler 同包直调 store), 于是这里改钉**真行为**:
 *  取消之后**不额外调** save_state, 盘上也要有它。
 * ══════════════════════════════════════════════════════════════ */

static void test_cancel_persists_without_explicit_save(void)
{
    TEST("cancel(persist) 自己落盘: 不额外调 save_state 也能 reload 出 CANCELLED");
    clean_workspace();

    struct edc_config *cfg = make_config();
    struct edc_session *sa = edc_session_new(cfg, "sess-lv2", "Leave2");
    struct edc_scheduler *sched_a = edc_scheduler_new(cfg);
    install_resolver(sched_a, sa);

    struct edc_task *a_run = make_task("t1", "sess-lv2", "running", SCENE_RUNNING);
    struct edc_task_group *ga = edc_task_group_new(a_run);
    edc_session_add_group(sa, ga);
    edc_scheduler_set_foreground(sa, ga);

    /* 走 `/task stop` 那条路: 它是 persist=true 的信号 (executor 里落盘)。 */
    edc_scheduler_send_signal_to_task(sched_a, sa, "t1", TASK_SIGTERM);
    CHECK(edc_task_is_cancelled(a_run), "running 任务在内存里 CANCELLED");

    /* 关键: 这里**不**调 edc_session_save_state —— 落盘必须是取消那一步做的。 */
    struct edc_session *sa2 = edc_session_new(cfg, "sess-lv2", "Leave2");
    struct edc_scheduler *sched_a2 = edc_scheduler_new(cfg);
    install_resolver(sched_a2, sa2);
    CHECK(edc_session_load_state(sa2, sched_a2), "load_state succeeds");

    struct edc_task *rt = find_task(sa2, "t1");
    CHECK(rt != NULL, "任务从盘上回来了 (即: 取消那一步自己落了盘)");
    CHECK(rt != NULL && edc_task_is_cancelled(rt), "而且读回来是 CANCELLED");

    edc_scheduler_free(sched_a);
    edc_scheduler_free(sched_a2);
    edc_session_free(sa);
    edc_session_free(sa2);
    edc_config_free(cfg);
    clean_workspace();
}

static void test_interrupted_restores_cancelled(void)
{
    TEST("interrupted in-flight task reloads as INTERRUPTED (resume re-derives)");
    clean_workspace();

    struct edc_config *cfg = make_config();

    struct edc_session *sa = edc_session_new(cfg, "sess-dg", "Degrade");
    struct edc_scheduler *sched_a = edc_scheduler_new(cfg);
    install_resolver(sched_a, sa);
    struct edc_task *a_run = make_task("t1", "sess-dg", "running", SCENE_RUNNING);
    struct edc_task_group *ga = edc_task_group_new(a_run);
    edc_session_add_group(sa, ga);

    /* No explicit cancel — simulate a hard kill that never got to persist. */
    CHECK(edc_task_awaits_exec(a_run), "in-flight before save");
    CHECK(edc_session_save_state(sa, sched_a), "save_state succeeds");

    struct edc_session *sa2 = edc_session_new(cfg, "sess-dg", "Degrade");
    struct edc_scheduler *sched_a2 = edc_scheduler_new(cfg);
    install_resolver(sched_a2, sa2);
    CHECK(edc_session_load_state(sa2, sched_a2), "load_state succeeds");

    struct edc_task *rt = find_task(sa2, "t1");
    CHECK(rt != NULL && edc_task_is_interrupted(rt), "restored as INTERRUPTED");

    edc_scheduler_free(sched_a);
    edc_scheduler_free(sched_a2);
    edc_session_free(sa);
    edc_session_free(sa2);
    edc_config_free(cfg);
    clean_workspace();
}

int main(void)
{
    edc_test_workspace(); /* open the disposable store before anything writes */
    log_info("=== session leave-cancel semantics tests ===");
    log_info("");

    test_signal_scoped_to_session();
    test_leave_persists_cancelled();
    test_cancel_persists_without_explicit_save();
    test_interrupted_restores_cancelled();

    log_info("");
    log_info("=== Results: %d passed, %d failed ===", test_passed, test_failed);
    return test_failed > 0 ? 1 : 0;
}
