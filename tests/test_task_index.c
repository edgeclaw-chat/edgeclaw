/* Cross-session task-id index (v3: single source of truth = session.json).
 *
 * v3 removed the per-session `tasks.idx` sidecar. The session manager builds the
 * task_id → session_id index lazily by parsing each *unloaded* session's
 * session.json once, then caches it in memory for the rest of the run.
 *
 * Verified here:
 *   a. save_state writes NO tasks.idx (only session.json);
 *   b. an unloaded session's task id resolves to its owner via session.json;
 *   c. with session.json corrupt, the id cannot be resolved (returns NULL) —
 *      there is no sidecar to fall back on any more.
 */

#include "edc_agent.h"
#include "edc_channel_actor.h"
#include "edc_compat.h"
#include "edc_config.h"
#include "edc_file_utils.h"
#include "edc_scheduler.h"
#include "edc_session.h"
#include "edc_session_manager.h"
#include "edc_task.h"
#include "edc_task_group.h"
#include "edc_turn.h"
#include "log.h"
#include "sds.h"
#include "utlist_fix.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

static bool file_exists(const char *p)
{
    FILE *f = fopen(p, "rb");
    if (f)
    {
        fclose(f);
        return true;
    }
    return false;
}

static void write_str(const char *p, const char *s)
{
    FILE *f = fopen(p, "wb");
    if (f)
    {
        fputs(s, f);
        fclose(f);
    }
}

/* Open a manager over an existing workspace (all sessions unloaded). */
static struct edc_session_manager *open_sm(struct edc_config *cfg, const char *ws)
{
    cfg->workspace = sdsnew(ws);
    return edc_session_manager_new(cfg);
}

int main(void)
{
    const char *ws = "/tmp/test_task_index_ws";
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "rm -rf %s && mkdir -p %s", ws, ws);
    system(cmd);

    /* ── Phase 1: create A + B, a task in B, save B's state ───────────── */
    struct edc_config *cfg = g_new0(struct edc_config, 1);
    struct edc_session_manager *sm = open_sm(cfg, ws);
    struct edc_session *sa = edc_session_manager_get_active(sm);
    sa->state_loaded = true;
    struct edc_session *sb = edc_session_manager_create_ex(sm, "B", NULL);
    sm->active_index = edc_session_manager_index_of(sm, sa->session_id);

    struct edc_agent agent = {0};
    agent.config = cfg;
    agent.session_manager = sm;
    agent.channel_actor = edc_channel_actor_new(&cfg->channel, cfg->workspace);
    agent.scheduler = edc_scheduler_new(sa, cfg);

    edc_scheduler_set_session(agent.scheduler, sb);
    struct edc_task *bt = edc_scheduler_create_task(agent.scheduler, "b-task", edc_channel_adapter);
    edc_scheduler_create_group(agent.scheduler, bt);
    /* 新模型: task 创建时就带着它的 input —— 没装 turn 的任务落盘时会被丢掉
     * (docs/retire-waiting-input.md §4)。装上 input 再停到 SUSPENDED
     * (= "建了、有输入、先不跑"), 它才会出现在 session.json 里; 本用例要验的正是
     * "从磁盘上的 session.json 反解出 task 的属主"。 */
    {
        struct edc_turn *turn = edc_turn_new();
        turn->task_id = strdup(bt->task_id);
        edc_turn_add_user_message(turn, "b-task input");
        bt->active_turn = turn;
        edc_task_mark_suspended(bt);
    }
    CHECK(edc_session_save_state(sb, agent.scheduler), "B state saved");

    sds bt_id = sdsnew(bt->task_id);
    sds b_dir = sdsnew(sb->history_dir);
    sds idx_path = edc_path_join(b_dir, "tasks.idx", NULL);
    sds json_path = edc_path_join(b_dir, "session.json", NULL);

    TEST("save_state writes session.json but no tasks.idx sidecar");
    CHECK(file_exists(json_path), "session.json exists");
    CHECK(!file_exists(idx_path), "tasks.idx is NOT written any more");

    edc_scheduler_free(agent.scheduler);
    edc_channel_actor_free(agent.channel_actor);
    edc_session_manager_save_all(sm); /* persist sessions.json so a fresh manager sees B */
    edc_session_manager_free(sm);
    edc_config_free(cfg);

    /* ── Phase 2: fresh manager → owner resolved from session.json ────── */
    TEST("unloaded session's task id resolves via session.json (lazy index)");
    {
        struct edc_config *c = g_new0(struct edc_config, 1);
        struct edc_session_manager *m = open_sm(c, ws);
        struct edc_session *b = edc_session_manager_find(m, "B");
        CHECK(b != NULL && !b->state_loaded, "B present but unloaded");
        struct edc_session *owner = edc_session_manager_session_for_task(m, bt_id);
        CHECK(owner == b, "owner resolved by parsing session.json");
        /* cached: a second lookup must still work */
        CHECK(edc_session_manager_session_for_task(m, bt_id) == b, "cached lookup stable");
        edc_session_manager_free(m);
        edc_config_free(c);
    }

    /* ── Phase 3: session.json corrupt → nothing to resolve from ──────── */
    write_str(json_path, "{ this is not valid json");
    TEST("corrupt session.json → unresolved (no sidecar fallback)");
    {
        struct edc_config *c = g_new0(struct edc_config, 1);
        struct edc_session_manager *m = open_sm(c, ws);
        struct edc_session *b = edc_session_manager_find(m, "B");
        CHECK(b != NULL, "B present");
        CHECK(edc_session_manager_session_for_task(m, bt_id) == NULL, "unresolvable without session.json");
        edc_session_manager_free(m);
        edc_config_free(c);
    }

    sdsfree(idx_path);
    sdsfree(json_path);
    sdsfree(b_dir);
    sdsfree(bt_id);

    log_info("passed=%d failed=%d", test_passed, test_failed);
    return test_failed == 0 ? 0 : 1;
}
