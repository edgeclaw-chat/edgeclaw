/* Cross-session dependency persistence across a restart (S3).
 *
 * On save, each session.json only holds its own tasks, and a dep edge records
 * just the source task id. On load, the source of a cross-session edge is not in
 * the loaded session, so deser records it as a pending edge and the agent
 * reconciles it afterwards (lazy-loading the owner session). An edge that cannot
 * be restored must HOLD its target task — never let it run with an incomplete
 * dependency view (the resume gate).
 *
 * Scenarios:
 *   1. A depends on B; both on disk. After restart the edge is restored by
 *      lazy-loading B, A's task stays SUSPENDED, resume waits for the dep, and
 *      A wakes when B completes.
 *   2. B's state file is gone. The edge cannot be restored → the task is held
 *      and /task resume is refused.
 */

#include "edc_agent.h"
#include "edc_channel_actor.h"
#include "edc_commands.h"
#include "edc_compat.h"
#include "edc_config.h"
#include "edc_scheduler.h"
#include "edc_session.h"
#include "edc_session_manager.h"
#include "edc_task.h"
#include "edc_task_group.h"
#include "log.h"
#include "utlist_fix.h"
#include <cJSON.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static void send_cmd(struct edc_agent *agent, const char *content)
{
    struct edc_channel_message *msg =
        edc_channel_message_new(edc_channel_adapter, EDC_CHANNEL_MESSAGE_USER_TYPE, sdsnew(content));
    edc_commands_handler(agent, NULL, msg);
    edc_channel_message_free(msg);
}

struct env
{
    struct edc_config *cfg;
    struct edc_session_manager *sm;
    struct edc_agent agent;
};

static void env_open(struct env *e, const char *ws)
{
    e->cfg = g_new0(struct edc_config, 1);
    e->cfg->workspace = sdsnew(ws);
    e->sm = edc_session_manager_new(e->cfg);
    struct edc_session *sa = edc_session_manager_get_active(e->sm);
    memset(&e->agent, 0, sizeof(e->agent));
    e->agent.config = e->cfg;
    e->agent.session_manager = e->sm;
    e->agent.channel_actor = edc_channel_actor_new(&e->cfg->channel, e->cfg->workspace);
    e->agent.scheduler = edc_scheduler_new(sa, e->cfg);
}

static void env_close(struct env *e)
{
    edc_scheduler_free(e->agent.scheduler);
    edc_channel_actor_free(e->agent.channel_actor);
    edc_session_manager_free(e->sm);
    edc_config_free(e->cfg);
}

/* Build A (depends on B's task) + B on disk, then tear everything down so the
 * next open is a cold start. |delete_b_state| removes B's session.json to make
 * the dependency unrestorable. */
static void build_and_save(const char *ws, bool delete_b_state, sds *out_at_id, sds *out_bt_id)
{
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s && mkdir -p %s", ws, ws);
    system(cmd);

    struct edc_config *cfg = g_new0(struct edc_config, 1);
    cfg->workspace = sdsnew(ws);
    struct edc_session_manager *sm = edc_session_manager_new(cfg);
    struct edc_session *sa = edc_session_manager_get_active(sm);
    sa->state_loaded = true;
    struct edc_session *sb = edc_session_manager_create_ex(sm, "B", NULL);
    sm->active_index = edc_session_manager_index_of(sm, sa->session_id);
    sds b_sid = sdsnew(sb->session_id);

    struct edc_agent agent = {0};
    agent.config = cfg;
    agent.session_manager = sm;
    agent.channel_actor = edc_channel_actor_new(&cfg->channel, cfg->workspace);
    agent.scheduler = edc_scheduler_new(sa, cfg);

    /* A task in B, unfinished. */
    edc_scheduler_set_session(agent.scheduler, sb);
    struct edc_task *bt = edc_scheduler_create_task(agent.scheduler, "b-task", edc_channel_adapter);
    edc_scheduler_create_group(agent.scheduler, bt);
    edc_session_save_state(sb, agent.scheduler);
    *out_bt_id = sdsnew(bt->task_id);

    /* A task in A that depends on B's task (cross-session edge). */
    edc_scheduler_set_session(agent.scheduler, sa);
    struct edc_task *at = edc_scheduler_create_task(agent.scheduler, "a-task", edc_channel_adapter);
    edc_scheduler_create_group(agent.scheduler, at);
    edc_task_add_dependency(at, bt, TASK_DEP_SEQUENCE);
    edc_session_save_state(sa, agent.scheduler);
    *out_at_id = sdsnew(at->task_id);

    edc_scheduler_free(agent.scheduler);
    edc_channel_actor_free(agent.channel_actor);
    edc_session_manager_free(sm);
    edc_config_free(cfg);

    if (delete_b_state)
    {
        snprintf(cmd, sizeof(cmd), "rm -f %s/%s/session.json", ws, b_sid);
        system(cmd);
    }
    sdsfree(b_sid);
}

static void scenario_restore(const char *ws)
{
    sds at_id = NULL, bt_id = NULL;
    build_and_save(ws, false, &at_id, &bt_id);

    TEST("restart restores a cross-session dep by lazy-loading the owner");
    struct env e;
    env_open(&e, ws);
    edc_agent_load_session(&e.agent, edc_session_manager_get_active(e.sm));

    struct edc_task *at = edc_session_manager_find_task_any(e.sm, at_id, NULL);
    struct edc_task *bt = edc_session_manager_find_task_any(e.sm, bt_id, NULL);
    CHECK(at != NULL, "A's task restored");
    CHECK(bt != NULL, "B's task loaded on demand");
    CHECK(at && at->deps_total == 1, "cross-session edge restored");
    CHECK(at && at->deps && at->deps->source == bt, "edge points at B's task");
    CHECK(at && at->state == TASK_SUSPENDED, "A's task held (SUSPENDED), not run");
    CHECK(!edc_scheduler_task_has_unresolved_deps(e.agent.scheduler, at), "gate open once the edge is restored");

    sds resume_cmd = sdscatprintf(sdsempty(), "/task resume %s", at_id);
    send_cmd(&e.agent, resume_cmd);
    sdsfree(resume_cmd);
    CHECK(at->state == TASK_WAITING_DEPS, "resume legitimately waits for the unfinished dep");

    edc_task_set_state(bt, TASK_COMPLETED);
    edc_task_notify_dependents(bt);
    CHECK(at->state == TASK_WAITING_INPUT, "A wakes when B completes");
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
    edc_agent_load_session(&e.agent, edc_session_manager_get_active(e.sm));

    struct edc_task *at = edc_session_manager_find_task_any(e.sm, at_id, NULL);
    CHECK(at != NULL, "A's task restored");
    CHECK(edc_session_manager_find_task_any(e.sm, bt_id, NULL) == NULL, "B's task unavailable");
    CHECK(at && edc_scheduler_task_has_unresolved_deps(e.agent.scheduler, at), "task held by an unresolved dep");
    const char *missing = at ? edc_scheduler_first_unresolved_dep(e.agent.scheduler, at) : NULL;
    CHECK(missing && bt_id && strcmp(missing, bt_id) == 0, "held on the missing source id");

    sds resume_cmd = sdscatprintf(sdsempty(), "/task resume %s", at_id);
    send_cmd(&e.agent, resume_cmd);
    sdsfree(resume_cmd);
    CHECK(at->state == TASK_SUSPENDED, "resume refused; task stays held");

    /* Removing the session that owns the missing source must not dangle: the
     * pending entry is stored by id, so the task simply stays held. */
    send_cmd(&e.agent, "/session remove B");
    CHECK(at != NULL, "held task survives removing the source session");
    CHECK(at && edc_scheduler_task_has_unresolved_deps(e.agent.scheduler, at), "still held after the source session is gone");
    env_close(&e);

    sdsfree(at_id);
    sdsfree(bt_id);
}

/* A→B→C: reconciling A's dep on B loads B, whose own dep on C loads C. Validates
 * the fixpoint loop. */
static void scenario_multihop(const char *ws)
{
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s && mkdir -p %s", ws, ws);
    system(cmd);

    struct edc_config *cfg = g_new0(struct edc_config, 1);
    cfg->workspace = sdsnew(ws);
    struct edc_session_manager *sm = edc_session_manager_new(cfg);
    struct edc_session *sa = edc_session_manager_get_active(sm);
    sa->state_loaded = true;
    struct edc_session *sb = edc_session_manager_create_ex(sm, "B", NULL);
    struct edc_session *sc = edc_session_manager_create_ex(sm, "C", NULL);
    sm->active_index = edc_session_manager_index_of(sm, sa->session_id);

    struct edc_agent agent = {0};
    agent.config = cfg;
    agent.session_manager = sm;
    agent.channel_actor = edc_channel_actor_new(&cfg->channel, cfg->workspace);
    agent.scheduler = edc_scheduler_new(sa, cfg);

    edc_scheduler_set_session(agent.scheduler, sc);
    struct edc_task *ct = edc_scheduler_create_task(agent.scheduler, "c", edc_channel_adapter);
    edc_scheduler_create_group(agent.scheduler, ct);
    edc_session_save_state(sc, agent.scheduler);

    edc_scheduler_set_session(agent.scheduler, sb);
    struct edc_task *bt = edc_scheduler_create_task(agent.scheduler, "b", edc_channel_adapter);
    edc_scheduler_create_group(agent.scheduler, bt);
    edc_task_add_dependency(bt, ct, TASK_DEP_SEQUENCE);
    edc_session_save_state(sb, agent.scheduler);

    edc_scheduler_set_session(agent.scheduler, sa);
    struct edc_task *at = edc_scheduler_create_task(agent.scheduler, "a", edc_channel_adapter);
    edc_scheduler_create_group(agent.scheduler, at);
    edc_task_add_dependency(at, bt, TASK_DEP_SEQUENCE);
    edc_session_save_state(sa, agent.scheduler);

    sds at_id = sdsnew(at->task_id), bt_id = sdsnew(bt->task_id), ct_id = sdsnew(ct->task_id);

    edc_scheduler_free(agent.scheduler);
    edc_channel_actor_free(agent.channel_actor);
    edc_session_manager_free(sm);
    edc_config_free(cfg);

    TEST("restart restores a multi-hop chain across three sessions");
    struct env e;
    env_open(&e, ws);
    edc_agent_load_session(&e.agent, edc_session_manager_get_active(e.sm));
    struct edc_task *a2 = edc_session_manager_find_task_any(e.sm, at_id, NULL);
    struct edc_task *b2 = edc_session_manager_find_task_any(e.sm, bt_id, NULL);
    struct edc_task *c2 = edc_session_manager_find_task_any(e.sm, ct_id, NULL);
    CHECK(a2 && b2 && c2, "all three tasks loaded on demand");
    CHECK(a2 && a2->deps && a2->deps->source == b2, "A→B restored");
    CHECK(b2 && b2->deps && b2->deps->source == c2, "B→C restored");
    CHECK(a2 && !edc_scheduler_task_has_unresolved_deps(e.agent.scheduler, a2), "A's gate open");
    CHECK(b2 && !edc_scheduler_task_has_unresolved_deps(e.agent.scheduler, b2), "B's gate open");
    env_close(&e);

    sdsfree(at_id);
    sdsfree(bt_id);
    sdsfree(ct_id);
}

int main(void)
{
    scenario_restore("/tmp/test_cross_dep_restart_a");
    scenario_unresolvable("/tmp/test_cross_dep_restart_b");
    scenario_multihop("/tmp/test_cross_dep_restart_c");

    log_info("passed=%d failed=%d", test_passed, test_failed);
    return test_failed == 0 ? 0 : 1;
}
