/* Task-id index (workspace/<session>/tasks.idx).
 *
 * session.json is large (every turn embeds its messages), so cross-session
 * lookups must not parse it. save_state also writes a tiny tasks.idx listing the
 * session's task ids; the session manager resolves an unloaded session's task id
 * from that sidecar.
 *
 * Verified here:
 *   a. save_state writes tasks.idx containing the task id;
 *   b. with session.json corrupted but tasks.idx present, the lookup still
 *      resolves the owner — proving it did NOT parse session.json;
 *   c. when tasks.idx is absent (old workspace), the lookup falls back to parsing
 *      session.json once and rewrites tasks.idx.
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

static char *read_all(const char *p)
{
    FILE *f = fopen(p, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    size_t rd = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[rd] = '\0';
    return buf;
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
    CHECK(edc_session_save_state(sb, agent.scheduler), "B state saved");

    sds bt_id = sdsnew(bt->task_id);
    sds b_dir = sdsnew(sb->history_dir);
    sds idx_path = edc_path_join(b_dir, "tasks.idx", NULL);
    sds json_path = edc_path_join(b_dir, "session.json", NULL);

    TEST("save_state writes a tiny tasks.idx with the task id");
    CHECK(file_exists(idx_path), "tasks.idx exists");
    char *idx = read_all(idx_path);
    sds expect = sdscatprintf(sdsempty(), "%s\n", bt_id);
    CHECK(idx && strcmp(idx, expect) == 0, "tasks.idx contains exactly the task id");
    free(idx);
    sdsfree(expect);

    edc_scheduler_free(agent.scheduler);
    edc_channel_actor_free(agent.channel_actor);
    edc_session_manager_save_all(sm); /* persist sessions.json so a fresh manager sees B */
    edc_session_manager_free(sm);
    edc_config_free(cfg);

    /* ── Phase 2 (fallback): tasks.idx missing, session.json intact ───── */
    unlink(idx_path);
    TEST("missing tasks.idx → migrate from session.json and rewrite the sidecar");
    {
        struct edc_config *c = g_new0(struct edc_config, 1);
        struct edc_session_manager *m = open_sm(c, ws);
        struct edc_session *b = edc_session_manager_find(m, "B");
        CHECK(b != NULL && !b->state_loaded, "B present but unloaded");
        struct edc_session *owner = edc_session_manager_session_for_task(m, bt_id);
        CHECK(owner == b, "owner resolved via the session.json fallback");
        CHECK(file_exists(idx_path), "tasks.idx rewritten during migration");
        edc_session_manager_free(m);
        edc_config_free(c);
    }

    /* ── Phase 3 (index): session.json corrupt, tasks.idx present ─────── */
    write_str(json_path, "{ this is not valid json");
    TEST("tasks.idx present → owner resolved without parsing session.json");
    {
        struct edc_config *c = g_new0(struct edc_config, 1);
        struct edc_session_manager *m = open_sm(c, ws);
        struct edc_session *b = edc_session_manager_find(m, "B");
        CHECK(b != NULL, "B present");
        struct edc_session *owner = edc_session_manager_session_for_task(m, bt_id);
        CHECK(owner == b, "owner resolved from tasks.idx despite corrupt session.json");
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
