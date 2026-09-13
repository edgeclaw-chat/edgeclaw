/* v2 → v3 migration + load-failure guardrail.
 *
 * Covers the Step-6 contract:
 *   a. a v2 session.json (version 2, `todolist` key, no `message.render`) loads
 *      fine — the old keys are migrated in memory;
 *   b. the first successful load of a v2 file leaves `session.json.v2.bak`
 *      behind (and only once — idempotent), so the pre-migration original is
 *      never lost;
 *   c. the next save_state writes `version: 3`;
 *   d. a corrupt session.json makes load_state fail AND quarantines the file as
 *      `session.json.broken`, so a later save can never silently overwrite the
 *      (damaged but possibly recoverable) original — the v1→v2 incident lesson.
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

/* Turn a freshly serialized v3 tree into the v2 shape. */
static void downgrade_to_v2(cJSON *root)
{
    cJSON_ReplaceItemInObject(root, "version", cJSON_CreateNumber(2));

    /* v2 still carried the (dead) session-level messages array. */
    cJSON *sj = cJSON_GetObjectItem(root, "session");
    if (sj)
        cJSON_AddItemToObject(sj, "messages", cJSON_CreateArray());

    cJSON *tasks = cJSON_GetObjectItem(root, "tasks");
    int n = cJSON_IsArray(tasks) ? cJSON_GetArraySize(tasks) : 0;
    for (int i = 0; i < n; i++)
    {
        cJSON *t = cJSON_GetArrayItem(tasks, i);

        /* todos[] → old todolist key */
        cJSON *todos = cJSON_DetachItemFromObject(t, "todos");
        if (todos)
            cJSON_AddItemToObject(t, "todolist", todos);

        /* drop message.render (v2 predates it) */
        cJSON *turns = cJSON_GetObjectItem(t, "turns");
        int tn = cJSON_IsArray(turns) ? cJSON_GetArraySize(turns) : 0;
        for (int j = 0; j < tn; j++)
        {
            cJSON *msgs = cJSON_GetObjectItem(cJSON_GetArrayItem(turns, j), "messages");
            int mn = cJSON_IsArray(msgs) ? cJSON_GetArraySize(msgs) : 0;
            for (int k = 0; k < mn; k++)
                cJSON_DeleteItemFromObject(cJSON_GetArrayItem(msgs, k), "render");
        }
    }
}

int main(void)
{
    const char *ws = "/tmp/test_v2_migration_ws";
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", ws);
    system(cmd);

    struct edc_config *cfg = g_new0(struct edc_config, 1);
    cfg->workspace = sdsnew(ws);
    cfg->channel.type = edc_channel_adapter;

    const char *sid = "s-mig";
    char dir[256];
    snprintf(dir, sizeof(dir), "%s/%s", ws, sid);
    snprintf(cmd, sizeof(cmd), "mkdir -p %s", dir);
    system(cmd);

    sds json_path = sdscatprintf(sdsempty(), "%s/session.json", dir);
    sds bak_path = sdscatprintf(sdsempty(), "%s/session.json.v2.bak", dir);
    sds broken_path = sdscatprintf(sdsempty(), "%s/session.json.broken", dir);

    /* ── Phase 1: build a state tree, then downgrade it to v2 ─────────── */
    {
        struct edc_session *s = edc_session_new(cfg, sid, "Mig");
        struct edc_scheduler *sched = edc_scheduler_new(s, cfg);

        struct edc_task *t = edc_task_new("m-task", sid, "m-task", "migrate me", edc_channel_adapter);
        t->is_group_leader = 1;
        edc_task_set_state(t, TASK_WAITING_INPUT);
        edc_task_set_state(t, TASK_LLM_PROCESSING);
        edc_task_set_state(t, TASK_COMPLETED);
        struct edc_task_group *g = edc_task_group_new(t);
        edc_session_add_group(s, g);

        struct edc_turn *turn = edc_turn_new();
        turn->task_id = strdup("m-task");
        edc_turn_add_user_message(turn, "hello v2");
        edc_task_add_turn(t, turn);

        cJSON *root = edc_state_serialize(s, sched);
        CHECK(edc_state_json_version(root) == 3, "fresh serialize is version 3");
        downgrade_to_v2(root);

        char *txt = cJSON_Print(root);
        CHECK(txt != NULL, "downgraded tree printed");
        write_str(json_path, txt);
        free(txt);
        cJSON_Delete(root);

        edc_scheduler_free(sched);
        edc_session_free(s);
    }

    /* ── Phase 2: load the v2 file (migrate + back up) ────────────────── */
    TEST("v2 session.json loads and is backed up");
    {
        struct edc_session *s = edc_session_new(cfg, sid, "Mig");
        struct edc_scheduler *sched = edc_scheduler_new(s, cfg);

        CHECK(edc_session_load_state(s, sched), "v2 file loads");
        CHECK(file_exists(bak_path), "session.json.v2.bak created");
        CHECK(file_exists(json_path), "original session.json still in place");

        struct edc_task *t = edc_session_find_task(s, "m-task");
        CHECK(t != NULL, "task restored from v2");
        struct edc_turn *rt = t ? (struct edc_turn *)t->turns.data[t->turns.head] : NULL;
        CHECK(rt && edc_queue_length(&rt->messages) == 1, "1 message restored");
        if (rt && edc_queue_length(&rt->messages) == 1)
        {
            cJSON *m = (cJSON *)rt->messages.data[rt->messages.head];
            CHECK(cJSON_GetStringValue(cJSON_GetObjectItem(m, "content")) != NULL &&
                      strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(m, "content")), "hello v2") == 0,
                  "message content preserved");
            CHECK(cJSON_GetObjectItem(m, "render") != NULL, "missing render defaulted");
            CHECK(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(m, "render")), "plain") == 0,
                  "render default is plain");
        }

        /* re-save as v3 and re-load: backup must NOT be overwritten/reset */
        CHECK(edc_session_save_state(s, sched), "re-save as v3");
        edc_scheduler_free(sched);
        edc_session_free(s);
    }

    TEST("saved file is version 3 and reload is idempotent");
    {
        EdcError *err = NULL;
        sds txt = edc_file_read(json_path, &err);
        CHECK(txt != NULL, "re-read session.json");
        if (txt)
        {
            cJSON *root = cJSON_Parse(txt);
            CHECK(edc_state_json_version(root) == 3, "on-disk version is now 3");
            cJSON_Delete(root);
            sdsfree(txt);
        }
        if (err)
            edc_error_free(err);

        struct edc_session *s = edc_session_new(cfg, sid, "Mig");
        struct edc_scheduler *sched = edc_scheduler_new(s, cfg);
        CHECK(edc_session_load_state(s, sched), "v3 file loads");
        CHECK(file_exists(bak_path), "v2 backup still present (idempotent)");
        CHECK(edc_session_find_task(s, "m-task") != NULL, "task still restored");
        edc_scheduler_free(sched);
        edc_session_free(s);
    }

    /* ── Phase 3: corrupt file → fail + quarantine ────────────────────── */
    TEST("corrupt session.json is quarantined as .broken");
    write_str(json_path, "{ this is not valid json");
    {
        struct edc_session *s = edc_session_new(cfg, sid, "Mig");
        struct edc_scheduler *sched = edc_scheduler_new(s, cfg);
        CHECK(!edc_session_load_state(s, sched), "load_state fails on corrupt JSON");
        CHECK(file_exists(broken_path), "session.json renamed to .broken");
        CHECK(!file_exists(json_path), "damaged original no longer at session.json");
        edc_scheduler_free(sched);
        edc_session_free(s);
    }

    sdsfree(json_path);
    sdsfree(bak_path);
    sdsfree(broken_path);
    sdsfree(cfg->workspace);
    free(cfg);

    log_info("passed=%d failed=%d", test_passed, test_failed);
    return test_failed == 0 ? 0 : 1;
}
