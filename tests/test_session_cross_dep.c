/* Cross-session task dependencies (S0 + S1).
 *
 * Task ids are issued by a process-wide monotonic counter persisted in
 * <workspace>/.task_seq (edc_scheduler_gen_task_id), so an id is globally
 * unique across sessions and restarts. That makes a global lookup unambiguous,
 * which is what lets a task in one session depend on a task in another.
 *
 * This test drives the real command handlers to cover:
 *   - edc_session_manager_find_task_any() finds tasks in any loaded session and
 *     reports the owning session;
 *   - `/task new -d <id>` wires a dependency onto a task in another session;
 *   - an unknown `-d` id fails the command (nothing is created) — no silent
 *     drop and no fallback;
 *   - completing the source wakes the cross-session dependent;
 *   - `/task fg <id>` refuses a task that lives in another session.
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

#define TEST_WS "/tmp/test_cross_dep_ws"

static void send_cmd(struct edc_agent *agent, const char *content)
{
    struct edc_channel_message *msg =
        edc_channel_message_new(edc_channel_adapter, EDC_CHANNEL_MESSAGE_USER_TYPE, sdsnew(content));
    edc_commands_handler(agent, NULL, msg);
    edc_channel_message_free(msg);
}

static void switch_to(struct edc_agent *agent, const char *target)
{
    sds scmd = sdscatprintf(sdsempty(), "/session switch %s", target);
    send_cmd(agent, scmd);
    sdsfree(scmd);
}

static int count_tasks(struct edc_session *s)
{
    int n = 0;
    struct edc_task_group *g;
    DL_FOREACH(s->groups, g)
    {
        struct edc_task *t;
        LL_FOREACH2(g->members, t, group_next)
        n++;
    }
    return n;
}

int main(void)
{
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", TEST_WS);
    system(cmd);
    snprintf(cmd, sizeof(cmd), "mkdir -p %s", TEST_WS);
    system(cmd);

    struct edc_config *cfg = g_new0(struct edc_config, 1);
    cfg->workspace = sdsnew(TEST_WS);

    struct edc_session_manager *sm = edc_session_manager_new(cfg);
    struct edc_session *sa = edc_session_manager_get_active(sm);
    CHECK(sa != NULL, "manager created a default session A");
    sa->state_loaded = true;

    /* Second session; create_ex makes it active, so put A back in front. */
    struct edc_session *sb = edc_session_manager_create_ex(sm, "B", NULL);
    CHECK(sb != NULL, "second session B created");
    sm->active_index = edc_session_manager_index_of(sm, sa->session_id);

    struct edc_agent agent = {0};
    agent.config = cfg;
    agent.session_manager = sm;
    agent.channel_actor = edc_channel_actor_new(&cfg->channel, cfg->workspace);
    agent.scheduler = edc_scheduler_new(sa, cfg);

    /* ── Create a task in session B ─────────────────────────── */
    switch_to(&agent, sb->session_id);
    struct edc_task *bt = edc_scheduler_create_task(agent.scheduler, "b-task", edc_channel_adapter);
    edc_scheduler_create_group(agent.scheduler, bt);
    edc_session_set_active_task(sb, bt);
    switch_to(&agent, sa->session_id);
    CHECK(agent.scheduler->session == sa, "scheduler bound back to A");

    /* ── S0: global lookup ──────────────────────────────────── */
    TEST("find_task_any locates tasks across sessions");
    struct edc_session *owner = NULL;
    struct edc_task *found = edc_session_manager_find_task_any(sm, bt->task_id, &owner);
    CHECK(found == bt, "cross-session find returns B's task");
    CHECK(owner == sb, "owner session is B");
    CHECK(edc_session_manager_find_task_any(sm, "no-such-task", &owner) == NULL, "unknown id → NULL");
    CHECK(owner == NULL, "owner cleared on miss");

    /* ── S1a: cross-session -d dependency ───────────────────── */
    TEST("create a task with a cross-session -d dependency");
    int before = count_tasks(sa);
    sds scmd = sdscatprintf(sdsempty(), "/task new \"\"\"crossdep\"\"\" -d %s", bt->task_id);
    send_cmd(&agent, scmd);
    sdsfree(scmd);

    struct edc_task *at = edc_session_get_active_task(sa);
    CHECK(at != NULL && at != bt, "a new task was created in A");
    CHECK(count_tasks(sa) == before + 1, "exactly one task added to A");
    CHECK(at->deps_total == 1, "A's task has one dependency");
    CHECK(at->deps && at->deps->source == bt, "dependency points at B's task");
    CHECK(at->state == TASK_WAITING_DEPS, "dependent waits for its cross-session dep");

    /* ── S1a/D5: unknown id fails the command ───────────────── */
    TEST("unknown -d id fails the command (no task created)");
    before = count_tasks(sa);
    send_cmd(&agent, "/task new \"\"\"shouldfail\"\"\" -d no-such-task");
    CHECK(count_tasks(sa) == before, "no task created when dep is unknown");
    CHECK(edc_session_get_active_task(sa) == at, "active task unchanged after failure");

    /* ── runtime: completion wakes the dependent ────────────── */
    TEST("completing the source wakes the cross-session dependent");
    edc_task_set_state(bt, TASK_COMPLETED);
    edc_task_notify_dependents(bt);
    CHECK(at->state == TASK_WAITING_INPUT, "dependent became runnable after source completed");

    /* ── S1b: fg refuses a task in another session ──────────── */
    TEST("/task fg refuses a task in another session");
    struct edc_task_group *fg_before = edc_scheduler_get_foreground(agent.scheduler);
    sds fg_cmd = sdscatprintf(sdsempty(), "/task fg %s", bt->task_id);
    send_cmd(&agent, fg_cmd);
    sdsfree(fg_cmd);
    CHECK(edc_scheduler_get_foreground(agent.scheduler) == fg_before, "foreground group unchanged");

    /* ── S1a: /task chain is cross-session ──────────────────── */
    TEST("/task chain links tasks across sessions");
    struct edc_task *at2 = edc_scheduler_create_task(agent.scheduler, "chain-target", edc_channel_adapter);
    edc_scheduler_create_group(agent.scheduler, at2);
    size_t deps_before = at2->deps_total;
    sds chain_cmd = sdscatprintf(sdsempty(), "/task chain %s %s", bt->task_id, at2->task_id);
    send_cmd(&agent, chain_cmd);
    sdsfree(chain_cmd);
    CHECK(at2->deps_total == deps_before + 1, "chain added a cross-session dependency");

    /* ── S2: dependency onto a task in a session that was not activated ──
     * Simulate "the owner session exists on disk but was never activated this
     * run": save B's state, then drop B's live groups. The resolver must find
     * the id by probing B's session.json and load B on demand. */
    TEST("dependency onto a task in an unloaded session activates it on demand");
    switch_to(&agent, sb->session_id);
    struct edc_task *bt2 = edc_scheduler_create_task(agent.scheduler, "b2-task", edc_channel_adapter);
    edc_scheduler_create_group(agent.scheduler, bt2);
    edc_session_set_active_task(sb, bt2);
    sds bt2_id = sdsnew(bt2->task_id);
    CHECK(edc_session_save_state(sb, agent.scheduler), "B state saved to disk");
    switch_to(&agent, sa->session_id);

    /* Drop B's in-memory groups (freeing bt/bt2 also detaches their reverse
     * edges, so nothing dangles) and mark it unloaded. */
    struct edc_task_group *bg, *bgt;
    DL_FOREACH_SAFE(sb->groups, bg, bgt)
    {
        edc_session_remove_group(sb, bg);
        edc_task_group_free(bg);
    }
    sb->groups = NULL;
    sb->active_task = NULL;
    sb->foreground_group = NULL;
    sb->state_loaded = false;
    CHECK(edc_session_manager_find_task_any(sm, bt2_id, NULL) == NULL, "bt2 absent from memory");

    int before2 = count_tasks(sa);
    sds load_cmd = sdscatprintf(sdsempty(), "/task new \"\"\"needs-b2\"\"\" -d %s", bt2_id);
    send_cmd(&agent, load_cmd);
    sdsfree(load_cmd);

    struct edc_task *at3 = edc_session_get_active_task(sa);
    CHECK(count_tasks(sa) == before2 + 1, "a task was created in A");
    CHECK(at3 && at3->deps_total == 1 && at3->deps->source &&
              strcmp(at3->deps->source->task_id, bt2_id) == 0,
          "dependency resolved against B, loaded on demand");
    CHECK(sb->state_loaded, "B was activated on demand");
    CHECK(agent.scheduler->session == sa, "active session unchanged by the on-demand load");
    CHECK(edc_scheduler_get_foreground(agent.scheduler) == at3->group,
          "active session's foreground group is A's, not hijacked by the load");
    sdsfree(bt2_id);

    edc_scheduler_free(agent.scheduler);
    edc_channel_actor_free(agent.channel_actor);
    edc_session_manager_free(sm);
    edc_config_free(cfg);
    snprintf(cmd, sizeof(cmd), "rm -rf %s", TEST_WS);
    system(cmd);

    log_info("passed=%d failed=%d", test_passed, test_failed);
    return test_failed == 0 ? 0 : 1;
}
