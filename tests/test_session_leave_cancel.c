/* Regression tests for "离开即取消" semantics:
 *
 *   离开一个 session (/session switch / /session new / /session stop) 时, 只把
 *   该 session 里**正在执行**的任务 (LLM_PROCESSING / TOOL_CALLING) 打成
 *   CANCELLED 并落盘 —— 中断 = stop = cancelled。WAITING_INPUT / WAITING_DEPS /
 *   SUSPENDED 等非运行态任务原样保留: task 是什么状态就是什么状态, 离开不等于
 *   把它们都停掉。
 *
 * 覆盖三个层面:
 *   1. edc_scheduler_send_signal_to_session(..., running_only=true) 只命中指定
 *      session 的运行中任务: 不碰别的 session, 不碰终态 (COMPLETED), 不碰
 *      WAITING_INPUT; running_only=false (显式 /stop) 仍然全清;
 *   2. 信号 + save 之后反序列化回来确实得到 CANCELLED (WAITING_INPUT 保持原状);
 *   3. 重启安全网: 即使没来得及显式取消, 在途任务也不会变回 WAITING_INPUT。
 */

#include "edc_channel_actor.h"
#include "edc_compat.h"
#include "edc_config.h"
#include "edc_file_utils.h"
#include "edc_scheduler.h"
#include "edc_session.h"
#include "edc_state_serializer.h"
#include "edc_task.h"
#include "edc_task_group.h"
#include "utlist_fix.h"
#include "log.h"
#include <signal.h>
#include <stdio.h>
#include <string.h>

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

#define TEST_WS "/tmp/test_edc_leave_cancel_ws"

static struct edc_config *make_config(void)
{
    struct edc_config *cfg = g_new0(struct edc_config, 1);
    cfg->workspace = sdsnew(TEST_WS);
    return cfg;
}

static void clean_workspace(void)
{
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", TEST_WS);
    system(cmd);
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

/* Build a task in `target` state through a legal transition chain
 * (IDLE -> WAITING_INPUT -> ... -> target). */
static struct edc_task *make_task(const char *id, const char *sid, const char *desc, edc_task_state target)
{
    struct edc_task *t = edc_task_new(id, sid, id, desc, edc_channel_adapter);
    edc_task_set_state(t, TASK_WAITING_INPUT);
    if (target == TASK_LLM_PROCESSING)
    {
        edc_task_set_state(t, TASK_LLM_PROCESSING);
    }
    else if (target == TASK_COMPLETED)
    {
        edc_task_set_state(t, TASK_LLM_PROCESSING);
        edc_task_set_state(t, TASK_COMPLETED);
    }
    else if (target == TASK_CANCELLED)
    {
        edc_task_send_signal(t, TASK_SIGTERM);
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
    struct edc_scheduler *sched_a = edc_scheduler_new(sa, cfg);
    struct edc_task *a_run = make_task("a-run", "sess-a", "running", TASK_LLM_PROCESSING);
    struct edc_task_group *ga = edc_task_group_new(a_run);
    edc_session_add_group(sa, ga);
    struct edc_task *a_done = make_task("a-done", "sess-a", "done", TASK_COMPLETED);
    edc_task_group_add_task(ga, a_done);
    /* 非运行态: 离开时必须原样保留, 不能被"离开即取消"误伤 */
    struct edc_task *a_wait = make_task("a-wait", "sess-a", "waiting input", TASK_WAITING_INPUT);
    edc_task_group_add_task(ga, a_wait);

    /* Session B, NOT bound to the scheduler */
    struct edc_session *sb = edc_session_new(cfg, "sess-b", "B");
    struct edc_scheduler *sched_b = edc_scheduler_new(sb, cfg);
    struct edc_task *b_run = make_task("b-run", "sess-b", "other", TASK_LLM_PROCESSING);
    struct edc_task *b_wait = make_task("b-wait", "sess-b", "other wait", TASK_WAITING_INPUT);
    struct edc_task_group *gb = edc_task_group_new(b_run);
    edc_task_group_add_task(gb, b_wait);
    edc_session_add_group(sb, gb);

    /* Leaving session A (running_only=true): only A's running task goes. */
    edc_scheduler_send_signal_to_session(sched_a, sa, TASK_SIGTERM, true);

    CHECK(a_run->state == TASK_CANCELLED, "target session running task -> CANCELLED");
    CHECK(a_wait->state == TASK_WAITING_INPUT, "WAITING_INPUT task preserved on leave");
    CHECK(a_done->state == TASK_COMPLETED, "terminal COMPLETED task untouched");
    CHECK(b_run->state == TASK_LLM_PROCESSING, "other session's running task untouched");
    CHECK(b_wait->state == TASK_WAITING_INPUT, "other session's waiting task untouched");

    /* Contrast: cancel-all (running_only=false, used by /stop and teardown)
     * still wipes every non-terminal task, WAITING_INPUT included. */
    edc_scheduler_send_signal_to_session(sched_a, sa, TASK_SIGTERM, false);
    CHECK(a_wait->state == TASK_CANCELLED, "cancel-all still cancels WAITING_INPUT");

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
    TEST("cancel + save -> reload gives CANCELLED (WAITING_INPUT untouched)");
    clean_workspace();

    struct edc_config *cfg = make_config();

    struct edc_session *sa = edc_session_new(cfg, "sess-lv", "Leave");
    struct edc_scheduler *sched_a = edc_scheduler_new(sa, cfg);
    struct edc_task *a_run = make_task("t1", "sess-lv", "running", TASK_LLM_PROCESSING);
    struct edc_task_group *ga = edc_task_group_new(a_run);
    edc_session_add_group(sa, ga);
    struct edc_task *a_wait = make_task("t2", "sess-lv", "waiting", TASK_WAITING_INPUT);
    edc_task_group_add_task(ga, a_wait);
    sched_a->foreground_group = ga;

    /* "Leave" = cancel running + persist before switching away. */
    edc_scheduler_send_signal_to_session(sched_a, sa, TASK_SIGTERM, true);
    CHECK(a_run->state == TASK_CANCELLED, "running task cancelled in memory");
    CHECK(a_wait->state == TASK_WAITING_INPUT, "WAITING_INPUT left alone in memory");
    CHECK(edc_session_save_state(sa, sched_a), "save_state succeeds");

    /* Reload into a fresh session (simulates switch-back / restart). */
    struct edc_session *sa2 = edc_session_new(cfg, "sess-lv", "Leave");
    struct edc_scheduler *sched_a2 = edc_scheduler_new(sa2, cfg);
    CHECK(edc_session_load_state(sa2, sched_a2), "load_state succeeds");

    struct edc_task *rt = find_task(sa2, "t1");
    CHECK(rt != NULL, "cancelled task restored");
    CHECK(rt != NULL && rt->state == TASK_CANCELLED, "restored as CANCELLED (not WAITING_INPUT)");
    struct edc_task *rt2 = find_task(sa2, "t2");
    CHECK(rt2 != NULL && rt2->state == TASK_WAITING_INPUT, "WAITING_INPUT restored verbatim");

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

static void test_interrupted_restores_cancelled(void)
{
    TEST("interrupted in-flight task reloads as CANCELLED (no WAITING_INPUT degrade)");
    clean_workspace();

    struct edc_config *cfg = make_config();

    struct edc_session *sa = edc_session_new(cfg, "sess-dg", "Degrade");
    struct edc_scheduler *sched_a = edc_scheduler_new(sa, cfg);
    struct edc_task *a_run = make_task("t1", "sess-dg", "running", TASK_LLM_PROCESSING);
    struct edc_task_group *ga = edc_task_group_new(a_run);
    edc_session_add_group(sa, ga);

    /* No explicit cancel — simulate a hard kill that never got to persist. */
    CHECK(a_run->state == TASK_LLM_PROCESSING, "in-flight before save");
    CHECK(edc_session_save_state(sa, sched_a), "save_state succeeds");

    struct edc_session *sa2 = edc_session_new(cfg, "sess-dg", "Degrade");
    struct edc_scheduler *sched_a2 = edc_scheduler_new(sa2, cfg);
    CHECK(edc_session_load_state(sa2, sched_a2), "load_state succeeds");

    struct edc_task *rt = find_task(sa2, "t1");
    CHECK(rt != NULL && rt->state == TASK_CANCELLED, "restored as CANCELLED, not WAITING_INPUT");

    edc_scheduler_free(sched_a);
    edc_scheduler_free(sched_a2);
    edc_session_free(sa);
    edc_session_free(sa2);
    edc_config_free(cfg);
    clean_workspace();
}

int main(void)
{
    log_info("=== session leave-cancel semantics tests ===");
    log_info("");

    test_signal_scoped_to_session();
    test_leave_persists_cancelled();
    test_interrupted_restores_cancelled();

    log_info("");
    log_info("=== Results: %d passed, %d failed ===", test_passed, test_failed);
    return test_failed > 0 ? 1 : 0;
}
