/* Session state load guardrails (schema v4 only).
 *
 * Older schemas are dropped now that every workspace is migrated, so the
 * contract is:
 *   a. a fresh save_state writes `version: 4` and round-trips (no legacy
 *      backup files are produced any more);
 *   b. a well-formed but outdated `version: 3` file is REJECTED and quarantined
 *      as `session.json.broken` (no silent overwrite of unknown data); the
 *      v3 -> v4 one-shot migration (tools/migrate_session_v3_to_v4.py) must run
 *      before an older file can be loaded;
 *   c. a corrupt session.json is quarantined the same way — the v1→v2 incident
 *      lesson: a failed load must never be followed by an empty-state save that
 *      clobbers the original;
 *   d. a session that was never LOADED in this run is an id/name-only shell
 *      built from sessions.json, and the exit save must skip it — writing the
 *      shell would clobber the newer on-disk state (groups, tasks and the bound
 *      working directory), which is how a switched-to session could come back
 *      unbound and run its tools in the launch directory.
 */

#include "edc_channel_actor.h"
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
#include "log.h"
#include "sds.h"
#include "utlist_fix.h"
#include <limits.h>
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

int main(void)
{
    const char *ws = "/tmp/test_state_guardrail_ws";
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", ws);
    system(cmd);

    struct edc_config *cfg = g_new0(struct edc_config, 1);
    cfg->workspace = sdsnew(ws);
    cfg->channel.type = edc_channel_adapter;

    const char *sid = "s-guard";
    char dir[256];
    snprintf(dir, sizeof(dir), "%s/%s", ws, sid);
    snprintf(cmd, sizeof(cmd), "mkdir -p %s", dir);
    system(cmd);

    sds json_path = sdscatprintf(sdsempty(), "%s/session.json", dir);
    sds bak_path = sdscatprintf(sdsempty(), "%s/session.json.v2.bak", dir);
    sds broken_path = sdscatprintf(sdsempty(), "%s/session.json.broken", dir);

    /* ── Phase 1: v4 round-trip ───────────────────────────────────────── */
    TEST("v4 state round-trips and produces no legacy backup");
    {
        struct edc_session *s = edc_session_new(cfg, sid, "Guard");
        struct edc_scheduler *sched = edc_scheduler_new(s, cfg);

        struct edc_task *t = edc_task_new("g-task", sid, "g-task", "guard me", edc_channel_adapter);
        t->is_group_leader = 1;
        edc_task_set_state(t, TASK_WAITING_INPUT);
        edc_task_set_state(t, TASK_LLM_PROCESSING);
        edc_task_set_state(t, TASK_COMPLETED);
        edc_session_add_group(s, edc_task_group_new(t));

        struct edc_turn *turn = edc_turn_new();
        turn->task_id = strdup("g-task");
        edc_turn_add_user_message(turn, "hello v3");
        edc_task_add_turn(t, turn);

        CHECK(edc_session_save_state(s, sched), "save_state succeeds");
        edc_scheduler_free(sched);
        edc_session_free(s);
    }
    {
        EdcError *err = NULL;
        sds txt = edc_file_read(json_path, &err);
        CHECK(txt != NULL, "state file written");
        if (txt)
        {
            cJSON *root = cJSON_Parse(txt);
            CHECK(edc_state_json_version(root) == 4, "on-disk version is 4");
            cJSON_Delete(root);
            sdsfree(txt);
        }
        if (err)
            edc_error_free(err);
        CHECK(!file_exists(bak_path), "no .v2.bak is produced any more");
    }
    {
        struct edc_session *s = edc_session_new(cfg, sid, "Guard");
        struct edc_scheduler *sched = edc_scheduler_new(s, cfg);
        CHECK(edc_session_load_state(s, sched), "v3 file loads");
        struct edc_task *t = edc_session_find_task(s, "g-task");
        CHECK(t != NULL, "task restored");
        struct edc_turn *rt = t ? (struct edc_turn *)t->turns.data[t->turns.head] : NULL;
        CHECK(rt && edc_queue_length(&rt->messages) == 1, "1 message restored");
        edc_scheduler_free(sched);
        edc_session_free(s);
    }

    /* ── Phase 2: an outdated version is rejected, not silently upgraded ─ */
    TEST("version 3 file is rejected and quarantined");
    {
        EdcError *err = NULL;
        sds txt = edc_file_read(json_path, &err);
        if (err)
            edc_error_free(err);
        CHECK(txt != NULL, "read back the v4 file to downgrade it");
        if (txt)
        {
            cJSON *root = cJSON_Parse(txt);
            cJSON_ReplaceItemInObject(root, "version", cJSON_CreateNumber(3));
            char *out = cJSON_Print(root);
            write_str(json_path, out);
            free(out);
            cJSON_Delete(root);
            sdsfree(txt);
        }

        struct edc_session *s = edc_session_new(cfg, sid, "Guard");
        struct edc_scheduler *sched = edc_scheduler_new(s, cfg);
        CHECK(!edc_session_load_state(s, sched), "load_state fails for version 3");
        CHECK(file_exists(broken_path), "v3 file renamed to .broken");
        CHECK(!file_exists(json_path), "no session.json left behind");
        edc_scheduler_free(sched);
        edc_session_free(s);
    }

    /* ── Phase 3: corrupt JSON is quarantined ─────────────────────────── */
    TEST("corrupt session.json is quarantined");
    write_str(json_path, "{ this is not valid json");
    {
        struct edc_session *s = edc_session_new(cfg, sid, "Guard");
        struct edc_scheduler *sched = edc_scheduler_new(s, cfg);
        CHECK(!edc_session_load_state(s, sched), "load_state fails on corrupt JSON");
        CHECK(file_exists(broken_path), "corrupt file renamed to .broken");
        CHECK(!file_exists(json_path), "no session.json left behind");
        edc_scheduler_free(sched);
        edc_session_free(s);
    }

    /* ── Phase 4: the exit save must not clobber an unloaded session ──── */
    TEST("exit save keeps an unloaded session's on-disk state");
    {
        /* A manager holding A (loaded) and B (created from the index, never
         * activated → an id/name-only shell). */
        struct edc_session_manager *sm = edc_session_manager_new(cfg);
        struct edc_session *a = edc_session_manager_get_active(sm);
        CHECK(a != NULL, "manager has an active session A");
        a->state_loaded = true;

        struct edc_session *b = edc_session_manager_create_ex(sm, "B", NULL);
        CHECK(b != NULL, "session B created (becomes active)");
        sm->active_index = edc_session_manager_index_of(sm, a->session_id);

        /* B's REAL state on disk, as a previous run left it: a bound workdir
         * and a completed task in a group. */
        char bdir[64];
        snprintf(bdir, sizeof(bdir), "/tmp/edc_guard_b_XXXXXX");
        char *made = mkdtemp(bdir);
        CHECK(made != NULL, "mkdtemp for B's workdir");

        char breal[PATH_MAX];
        CHECK(made && realpath(bdir, breal) != NULL, "realpath(B's workdir)");

        struct edc_session *b_real = edc_session_new_with_workdir(cfg, b->session_id, "B", bdir);
        CHECK(b_real != NULL, "B's real state session built");
        struct edc_task *bt =
            edc_task_new("b-task", b->session_id, "b-task", "real work", edc_channel_adapter);
        bt->is_group_leader = 1;
        edc_task_set_state(bt, TASK_WAITING_INPUT);
        edc_task_set_state(bt, TASK_LLM_PROCESSING);
        edc_task_set_state(bt, TASK_COMPLETED);
        edc_session_add_group(b_real, edc_task_group_new(bt));
        CHECK(edc_session_save_state(b_real, NULL), "B's real state written to disk");
        edc_session_free(b_real);

        /* The in-memory B is the shell: unbound, no groups. */
        CHECK(!b->state_loaded, "in-memory B is unloaded (never activated this run)");
        CHECK(edc_session_get_workdir(b) == NULL, "in-memory B shell has no workdir");
        CHECK(b->groups == NULL, "in-memory B shell has no groups");

        /* The exit save. */
        edc_session_manager_save_loaded_states(sm, NULL);

        /* B's file must still describe the real session. */
        struct edc_session *b2 = edc_session_new(cfg, b->session_id, "B");
        struct edc_scheduler *sc = edc_scheduler_new(b2, cfg);
        CHECK(edc_session_load_state(b2, sc), "B's file still loads");
        const char *wd = edc_session_get_workdir(b2);
        CHECK(wd != NULL && strcmp(wd, breal) == 0, "B's bound workdir survived the exit save");
        CHECK(edc_session_find_task(b2, "b-task") != NULL, "B's task survived the exit save");
        edc_scheduler_free(sc);
        edc_session_free(b2);

        /* Positive control: a loaded session is still written. */
        char apath[512];
        snprintf(apath, sizeof(apath), "%s/%s/session.json", ws, a->session_id);
        CHECK(file_exists(apath), "loaded session A is still saved");

        edc_session_manager_free(sm);
        rmdir(bdir);
    }

    sdsfree(json_path);
    sdsfree(bak_path);
    sdsfree(broken_path);
    sdsfree(cfg->workspace);
    free(cfg);

    log_info("passed=%d failed=%d", test_passed, test_failed);
    return test_failed == 0 ? 0 : 1;
}
