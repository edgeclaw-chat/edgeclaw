/* Cross-session dependency persistence across a restart (S3).
 *
 * On save, each session.json only holds its own tasks, and a dep edge records
 * just the source task id. On load, the source of a cross-session edge is not in
 * the loaded session, so deser records it as a pending edge and the app
 * reconciles it afterwards (lazy-loading the owner session). An edge that cannot
 * be restored must HOLD its target task — never let it run with an incomplete
 * dependency view (the resume gate).
 *
 * Scenarios:
 *   1. A depends on B; both on disk. After restart the edge is restored by
 *      lazy-loading B, A's task stays INTERRUPTED, resume waits for the dep, and
 *      A wakes when B completes.
 *   2. B's state file is gone. The edge cannot be restored → the task is held
 *      and /task resume is refused.
 */

#include "edc_session_store.h"
#include "test_ws.h"
#include "edc_channel_actor.h"
#include "edc_commands.h"
#include "edc_compat.h"
#include "edc_config.h"
#include "edc_scheduler.h"
#include "edc_session.h"
#include "edc_state_store.h"
#include "edc_session_manager.h"
#include "edc_task.h"
#include "edc_task_group.h"
#include "edc_turn.h"
#include "log.h"
#include "utlist_fix.h"
#include <cJSON.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "test_fs_util.h"

volatile sig_atomic_t _run_loop = 1;

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


/* 测试环境 = 真零件本身 (没有伞对象可造)。 */
struct env
{
    struct edc_config *cfg;
    struct edc_session_manager *sm;
    edc_actor_t *channel_actor;
    struct edc_scheduler *sched;
};

static void send_cmd(struct env *e, const char *content)
{
    struct edc_channel_message *msg =
        edc_channel_message_new(edc_channel_adapter, EDC_CHANNEL_MESSAGE_USER_TYPE, sdsnew(content));
    edc_commands_handler(e->cfg, e->sm, e->sched,
                         (struct edc_reply_to){.channel_actor = e->channel_actor, .type = msg->type}, msg);
    edc_channel_message_free(msg);
}


static void env_open(struct env *e, const char *ws)
{
    e->cfg = g_new0(struct edc_config, 1);
    e->cfg->workspace = sdsnew(ws);
    e->sm = edc_session_manager_new(e->cfg);
    struct edc_session *sa = edc_session_manager_get_active(e->sm);
    e->channel_actor = edc_channel_actor_new();
    e->sched = edc_scheduler_new(e->cfg);
}

static void env_close(struct env *e)
{
    edc_scheduler_free(e->sched);
    edc_channel_actor_free(e->channel_actor);
    edc_session_manager_free(e->sm);
    edc_config_free(e->cfg);
}

/* 新模型: task 创建时就带着它的 input —— 没有 turn-less 的任务, 落盘判据会丢掉
 * 那种残渣。fixture 用它给任务装上 input。 */
static void give_input(struct edc_task *t)
{
    struct edc_turn *turn = edc_turn_new();
    turn->task_id = t->task_id;
    edc_turn_add_user_message(turn, "input");
    t->active_turn = turn;
}

/* Build A (depends on B's task) + B on disk, then tear everything down so the
 * next open is a cold start. |delete_b_state| removes B's session.json to make
 * the dependency unrestorable. */
static void build_and_save(const char *ws, bool delete_b_state, sds *out_at_id, sds *out_bt_id)
{
    char cmd[512];
    /* the workspace is created once and the store stays open on it: no rm here */


    struct edc_config *cfg = g_new0(struct edc_config, 1);
    cfg->workspace = sdsnew(ws);
    struct edc_session_manager *sm = edc_session_manager_new(cfg);
    struct edc_session *sa = edc_session_manager_get_active(sm);
    sa->state_loaded = true;
    struct edc_session *sb = edc_session_manager_create_ex(sm, "B", NULL);
    sm->active_index = edc_session_manager_index_of(sm, edc_id_str(sa->session_id));
    /* Kept by VALUE: the manager (and with it `sb`) is freed below, and this id is used
     * after that. Reading it through the freed session was a use-after-free that happened
     * to work while ids were strings. */
    const int64_t b_id = sb->session_id;

    edc_actor_t *channel_actor = edc_channel_actor_new();
    struct edc_scheduler *sched = edc_scheduler_new(cfg);

    /* A task in B, unfinished. */
    struct edc_task *bt = edc_scheduler_task_new(sched, sb, "b-task", edc_channel_adapter);
    edc_scheduler_create_group(sched, sb, bt);
    /* 新模型: task 创建时就带着它的 input (没装 turn 的任务落盘时会被丢弃):
     * 装上 input 停到 INTERRUPTED —— 本用例要验的
     * 是跨会话依赖的恢复, 所以 B 这条任务必须真的出现在它自己的 session.json 里。 */
    give_input(bt);
    edc_task_mark_interrupted(bt);
    edc_session_save_state(sb, sched);
    *out_bt_id = sdsnew(edc_id_str(bt->task_id));

    /* A task in A that depends on B's task (cross-session edge). */
    struct edc_task *at = edc_scheduler_task_new(sched, sa, "a-task", edc_channel_adapter);
    edc_scheduler_create_group(sched, sa, at);
    edc_task_add_dependency(at, bt, TASK_DEP_SEQUENCE);
    edc_session_save_state(sa, sched);
    *out_at_id = sdsnew(edc_id_str(at->task_id));

    edc_scheduler_free(sched);
    edc_channel_actor_free(channel_actor);
    edc_session_manager_free(sm);
    edc_config_free(cfg);

    /* "B's state is gone" now means "its rows are gone", not "its session.json was
     * deleted" -- persistence moved to edgeclaw.db, so removing the legacy file would
     * leave B perfectly restorable and the scenario would test nothing. */
    if (delete_b_state)
    {
        struct edc_config *forget_cfg = g_new0(struct edc_config, 1);
        forget_cfg->workspace = sdsnew(ws);
        if (edc_state_store_open(forget_cfg))
            edc_state_store_forget(b_id);
        sdsfree(forget_cfg->workspace);
        free(forget_cfg);
    }
}

static void scenario_restore(const char *ws)
{
    sds at_id = NULL, bt_id = NULL;
    build_and_save(ws, false, &at_id, &bt_id);

    TEST("restart restores a cross-session dep by lazy-loading the owner");
    struct env e;
    env_open(&e, ws);
    edc_session_store_load_session(e.sm, e.sched, edc_session_manager_get_active(e.sm));

    struct edc_task *at = edc_session_manager_find_task_any(e.sm, (int64_t)strtoll(at_id, NULL, 10), 0);
    struct edc_task *bt = edc_session_manager_find_task_any(e.sm, (int64_t)strtoll(bt_id, NULL, 10), 0);
    CHECK(at != NULL, "A's task restored");
    CHECK(bt != NULL, "B's task loaded on demand");
    CHECK(at && at->deps_total == 1, "cross-session edge restored");
    CHECK(at && at->deps && at->deps->source == bt, "edge points at B's task");
    CHECK(at && edc_task_is_interrupted(at), "A's task held as INTERRUPTED, not run");
    CHECK(!edc_scheduler_task_has_unresolved_deps(e.sched, at), "gate open once the edge is restored");

    sds resume_cmd = sdscatprintf(sdsempty(), "/task resume %s", at_id);
    send_cmd(&e, resume_cmd);
    sdsfree(resume_cmd);
    CHECK(edc_task_awaits_deps(at), "resume legitimately waits for the unfinished dep");

    edc_task_mark_completed(bt);
    edc_task_notify_dependents(bt);
    CHECK(edc_task_is_ready(at), "A wakes when B completes");
    env_close(&e);

    sdsfree(at_id);
    sdsfree(bt_id);
}

static void scenario_unresolvable(const char *ws)
{
    sds at_id = NULL, bt_id = NULL;
    build_and_save(ws, true /* B's state file removed */, &at_id, &bt_id);

    TEST("an unrestorable dep holds the task (resume gate)");
    struct env e;
    env_open(&e, ws);
    edc_session_store_load_session(e.sm, e.sched, edc_session_manager_get_active(e.sm));

    struct edc_task *at = edc_session_manager_find_task_any(e.sm, (int64_t)strtoll(at_id, NULL, 10), 0);
    CHECK(at != NULL, "A's task restored");
    CHECK(edc_session_manager_find_task_any(e.sm, (int64_t)strtoll(bt_id, NULL, 10), 0) == NULL, "B's task unavailable");
    CHECK(at && edc_scheduler_task_has_unresolved_deps(e.sched, at), "task held by an unresolved dep");
    int64_t missing = at ? edc_scheduler_first_unresolved_dep(e.sched, at) : 0;
    CHECK(missing > 0 && bt_id && strcmp(edc_id_str(missing), bt_id) == 0, "held on the missing source id");

    sds resume_cmd = sdscatprintf(sdsempty(), "/task resume %s", at_id);
    send_cmd(&e, resume_cmd);
    sdsfree(resume_cmd);
    CHECK(edc_task_is_interrupted(at), "resume refused; it stays INTERRUPTED behind the resume gate");

    /* Removing the session that owns the missing source must not dangle: the
     * pending entry is stored by id, so the task simply stays held. */
    send_cmd(&e, "/session remove B");
    CHECK(at != NULL, "held task survives removing the source session");
    CHECK(at && edc_scheduler_task_has_unresolved_deps(e.sched, at), "still held after the source session is gone");
    env_close(&e);

    sdsfree(at_id);
    sdsfree(bt_id);
}

/* A→B→C: reconciling A's dep on B loads B, whose own dep on C loads C. Validates
 * the fixpoint loop. */
static void scenario_multihop(const char *ws)
{
    char cmd[512];
    /* the workspace is created once and the store stays open on it: no rm here */


    struct edc_config *cfg = g_new0(struct edc_config, 1);
    cfg->workspace = sdsnew(ws);
    struct edc_session_manager *sm = edc_session_manager_new(cfg);
    struct edc_session *sa = edc_session_manager_get_active(sm);
    sa->state_loaded = true;
    struct edc_session *sb = edc_session_manager_create_ex(sm, "B", NULL);
    struct edc_session *sc = edc_session_manager_create_ex(sm, "C", NULL);
    sm->active_index = edc_session_manager_index_of(sm, edc_id_str(sa->session_id));

    edc_actor_t *channel_actor = edc_channel_actor_new();
    struct edc_scheduler *sched = edc_scheduler_new(cfg);

    struct edc_task *ct = edc_scheduler_task_new(sched, sc, "c", edc_channel_adapter);
    edc_scheduler_create_group(sched, sc, ct);
    give_input(ct);
    edc_session_save_state(sc, sched);

    struct edc_task *bt = edc_scheduler_task_new(sched, sb, "b", edc_channel_adapter);
    edc_scheduler_create_group(sched, sb, bt);
    give_input(bt);
    edc_task_add_dependency(bt, ct, TASK_DEP_SEQUENCE);
    edc_session_save_state(sb, sched);

    struct edc_task *at = edc_scheduler_task_new(sched, sa, "a", edc_channel_adapter);
    edc_scheduler_create_group(sched, sa, at);
    give_input(at);
    edc_task_add_dependency(at, bt, TASK_DEP_SEQUENCE);
    edc_session_save_state(sa, sched);

    sds at_id = sdsnew(edc_id_str(at->task_id)), bt_id = sdsnew(edc_id_str(bt->task_id)),
        ct_id = sdsnew(edc_id_str(ct->task_id));

    edc_scheduler_free(sched);
    edc_channel_actor_free(channel_actor);
    edc_session_manager_free(sm);
    edc_config_free(cfg);

    TEST("restart restores a multi-hop chain across three sessions");
    struct env e;
    env_open(&e, ws);
    edc_session_store_load_session(e.sm, e.sched, edc_session_manager_get_active(e.sm));
    struct edc_task *a2 = edc_session_manager_find_task_any(e.sm, (int64_t)strtoll(at_id, NULL, 10), 0);
    struct edc_task *b2 = edc_session_manager_find_task_any(e.sm, (int64_t)strtoll(bt_id, NULL, 10), 0);
    struct edc_task *c2 = edc_session_manager_find_task_any(e.sm, (int64_t)strtoll(ct_id, NULL, 10), 0);
    CHECK(a2 && b2 && c2, "all three tasks loaded on demand");
    CHECK(a2 && a2->deps && a2->deps->source == b2, "A→B restored");
    CHECK(b2 && b2->deps && b2->deps->source == c2, "B→C restored");
    CHECK(a2 && !edc_scheduler_task_has_unresolved_deps(e.sched, a2), "A's gate open");
    CHECK(b2 && !edc_scheduler_task_has_unresolved_deps(e.sched, b2), "B's gate open");
    env_close(&e);

    sdsfree(at_id);
    sdsfree(bt_id);
    sdsfree(ct_id);
}

/* These scenarios each need their own workspace, and they are named paths rather than
 * mkdtemp ones -- so a previous run (possibly written by a build with a different schema
 * version) is still sitting there. Wipe before use: a stale database makes the run fail on
 * a version mismatch that has nothing to do with what is being tested. */
static void wipe(const char *ws)
{
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "rm -rf '%s'; mkdir -p '%s'", ws, ws);
    if (test_sh(cmd) != 0)
        fprintf(stderr, "warning: could not reset %s\n", ws);
}

int main(void)
{
    edc_test_workspace(); /* open the disposable store before anything writes */
    wipe("/tmp/test_cross_dep_restart_a");
    wipe("/tmp/test_cross_dep_restart_b");
    wipe("/tmp/test_cross_dep_restart_c");
    scenario_restore("/tmp/test_cross_dep_restart_a");
    scenario_unresolvable("/tmp/test_cross_dep_restart_b");
    scenario_multihop("/tmp/test_cross_dep_restart_c");

    log_info("passed=%d failed=%d", test_passed, test_failed);
    return test_failed == 0 ? 0 : 1;
}
