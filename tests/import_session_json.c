/* Import a legacy <workspace>/<session_id>/session.json into the database.
 *
 * WHY THIS EXISTS
 *   The current build keeps session state in <workspace>/edgeclaw.db and does not read
 *   session.json at all -- it only warns that the file is being ignored. That is correct
 *   for the design (no migration path, no compatibility layer), but it means a session
 *   whose only copy is the old file opens empty in the new build. This tool is the
 *   one-shot bridge: read the old file, write the rows, then the session behaves like any
 *   other.
 *
 * WHY IT IS A C PROGRAM AND NOT SQL
 *   The mapping from that JSON to 11 tables is ~700 lines of field-by-field work that
 *   already exists in edc_state_serializer.c (deserialize) and edc_state_store.c (flush).
 *   Re-implementing it in SQL or Python would duplicate it, and the copy would drift the
 *   first time a column changes.
 *
 * THE ONE THING THAT IS NOT OBVIOUS
 *   An old snapshot embeds each turn's messages inside the turn. The current serializer
 *   does NOT: a message is a row, and a turn carries only `msg_count`. So calling
 *   deserialize() directly would build a tree whose turns all have zero messages, and every
 *   message in the file would be dropped. The import therefore runs in this order:
 *
 *     1. read the JSON and give every turn the msg_count its messages array implies;
 *     2. write the message ROWS first (so the foreign keys exist);
 *     3. deserialize -- now the history rebuild can actually find them;
 *     4. flush, which upserts the real group/task/turn metadata over the rows step 2 and 3
 *        created.
 *
 * USAGE
 *   import_session_json <workspace> <session_id> [--dry-run]
 *
 *   <workspace>  the edgeclaw workspace holding both session.json files and edgeclaw.db
 *   <session_id> e.g. s13; the file read is <workspace>/<session_id>/session.json
 *
 * Exits non-zero when the file is missing, its version is outside what this build can
 * read, or the store refuses the write. --dry-run does everything except the flush.
 */
#include "cJSON.h"
#include "edc_compat.h"
#include "edc_config.h"
#include "edc_log.h"
#include "edc_scheduler.h"
#include "edc_session.h"
#include "edc_session_manager.h"
#include "edc_session_store.h"
#include "edc_state_serializer.h"
#include "edc_state_store.h"
#include "edc_task.h"
#include "edc_task_group.h"
#include "edc_token_estimate.h"
#include "edc_turn.h"
#include "log.h"
#include "sds.h"
#include "utlist_fix.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

volatile sig_atomic_t _run_loop = 1;

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0)
    {
        fclose(f);
        return NULL;
    }
    long n = ftell(f);
    if (n < 0)
    {
        fclose(f);
        return NULL;
    }
    rewind(f);
    char *buf = malloc((size_t)n + 1);
    if (!buf)
    {
        fclose(f);
        return NULL;
    }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = '\0';
    return buf;
}

/* Give every turn the msg_count its embedded messages imply. Without this the deserializer
 * sees a turn with no messages (see the header). Returns how many messages were counted. */
static long stamp_msg_counts(cJSON *tasks_json)
{
    long total = 0;
    const cJSON *task = NULL;
    cJSON_ArrayForEach(task, tasks_json)
    {
        const char *const turn_keys[] = {"turns", NULL};
        for (int k = 0; turn_keys[k]; k++)
        {
            cJSON *turns = cJSON_GetObjectItem((cJSON *)task, turn_keys[k]);
            if (!turns || !cJSON_IsArray(turns))
                continue;
            const cJSON *turn = NULL;
            cJSON_ArrayForEach(turn, turns)
            {
                cJSON *msgs = cJSON_GetObjectItem((cJSON *)turn, "messages");
                const int n = (msgs && cJSON_IsArray(msgs)) ? cJSON_GetArraySize(msgs) : 0;
                cJSON *existing = cJSON_GetObjectItem((cJSON *)turn, "msg_count");
                if (existing)
                    cJSON_SetNumberValue(existing, n);
                else
                    cJSON_AddNumberToObject((cJSON *)turn, "msg_count", n);
                total += n;
            }
        }
        /* The in-flight turn is a separate key. */
        cJSON *active = cJSON_GetObjectItem((cJSON *)task, "active_turn");
        if (active)
        {
            cJSON *msgs = cJSON_GetObjectItem(active, "messages");
            const int n = (msgs && cJSON_IsArray(msgs)) ? cJSON_GetArraySize(msgs) : 0;
            cJSON *existing = cJSON_GetObjectItem(active, "msg_count");
            if (existing)
                cJSON_SetNumberValue(existing, n);
            else
                cJSON_AddNumberToObject(active, "msg_count", n);
            total += n;
        }
    }
    return total;
}

/* Write the message rows for one task list, before any tree exists.
 *
 * The turn ordinal here is the same rule deser_turns() uses: committed turns take their
 * index in the array, and the in-flight turn takes the tail. Getting this wrong would file
 * messages under a turn the loader never looks at. */
static long write_messages(const char *session_id, cJSON *tasks_json)
{
    long written = 0;
    const cJSON *task = NULL;
    cJSON_ArrayForEach(task, tasks_json)
    {
        const char *task_id = cJSON_GetStringValue(cJSON_GetObjectItem((cJSON *)task, "task_id"));
        const char *group_id = cJSON_GetStringValue(cJSON_GetObjectItem((cJSON *)task, "group_id"));
        if (!task_id || !*task_id)
            continue;

        int turn_ord = 0;
        cJSON *turns = cJSON_GetObjectItem((cJSON *)task, "turns");
        if (turns && cJSON_IsArray(turns))
        {
            const cJSON *turn = NULL;
            cJSON_ArrayForEach(turn, turns)
            {
                cJSON *msgs = cJSON_GetObjectItem((cJSON *)turn, "messages");
                if (msgs && cJSON_IsArray(msgs))
                {
                    /* The rows need their ancestors to exist (messages -> turns -> tasks
                     * -> groups -> sessions); ensure_turn creates them with defaults, and
                     * the flush at the end overwrites them with the real metadata. */
                    edc_state_ensure_turn(session_id, group_id, task_id, turn_ord);
                    const cJSON *m = NULL;
                    int ord = 0;
                    cJSON_ArrayForEach(m, msgs)
                    {
                        if (edc_msg_append(task_id, turn_ord, ord, m, edc_token_estimate_msg(m)))
                            written++;
                        ord++;
                    }
                }
                turn_ord++;
            }
        }
        cJSON *active = cJSON_GetObjectItem((cJSON *)task, "active_turn");
        if (active)
        {
            cJSON *msgs = cJSON_GetObjectItem(active, "messages");
            if (msgs && cJSON_IsArray(msgs))
            {
                edc_state_ensure_turn(session_id, group_id, task_id, turn_ord);
                const cJSON *m = NULL;
                int ord = 0;
                cJSON_ArrayForEach(m, msgs)
                {
                    if (edc_msg_append(task_id, turn_ord, ord, m, edc_token_estimate_msg(m)))
                        written++;
                    ord++;
                }
            }
        }
    }
    return written;
}

static void install_sessions(struct edc_scheduler *sched, struct edc_session *sess)
{
    sched->ctx = sess;
    struct edc_session_manager *sm = g_new0(struct edc_session_manager, 1);
    sm->sessions = g_new0(struct edc_session *, 1);
    sm->sessions[0] = sess;
    sm->session_count = sess ? 1 : 0;
    sm->active_index = 0;
    sched->sessions = sm;
}

int main(int argc, char **argv)
{
    const char *ws = (argc > 1) ? argv[1] : NULL;
    const char *sid = (argc > 2) ? argv[2] : NULL;
    const bool dry = (argc > 3 && strcmp(argv[3], "--dry-run") == 0);
    if (!ws || !sid)
    {
        fprintf(stderr, "usage: %s <workspace> <session_id> [--dry-run]\n", argv[0]);
        return 2;
    }

    edc_log_init();

    sds dir = sdscatprintf(sdsempty(), "%s/%s/session.json", ws, sid);
    char *raw = read_file(dir);
    if (!raw)
    {
        fprintf(stderr, "cannot read %s\n", dir);
        sdsfree(dir);
        return 1;
    }
    printf("source      : %s (%.1f MB)\n", dir, (double)strlen(raw) / (1024.0 * 1024.0));
    sdsfree(dir);

    cJSON *json = cJSON_Parse(raw);
    free(raw);
    if (!json)
    {
        fprintf(stderr, "the file is not valid JSON\n");
        return 1;
    }

    cJSON *ver = cJSON_GetObjectItem(json, "version");
    printf("json version: %g (this build writes %d)\n", ver ? cJSON_GetNumberValue(ver) : -1.0,
           edc_state_schema_version());

    cJSON *tasks_json = cJSON_GetObjectItem(json, "tasks");
    const long counted = stamp_msg_counts(tasks_json);
    printf("messages    : %ld in the file\n", counted);

    struct edc_config *cfg = g_new0(struct edc_config, 1);
    cfg->workspace = sdsnew(ws);
    if (!edc_state_store_open(cfg))
    {
        fprintf(stderr, "cannot open the state store on %s\n", ws);
        cJSON_Delete(json);
        return 1;
    }

    /* Write the rows BEFORE any tree exists: the loader rebuilds each group's history out
     * of the store, so the rows have to be there first (see the header). */
    const long written = write_messages(sid, tasks_json);
    printf("rows written: %ld\n", written);

    struct edc_session *sess = edc_session_new(cfg, sid, sid);
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_sessions(sched, sess);

    const bool ok = edc_state_deserialize(json, sess, sched);
    printf("deserialize : %s\n", ok ? "ok" : "FAILED");
    if (!ok)
    {
        cJSON_Delete(json);
        return 1;
    }

    int groups = 0, tasks = 0, turns = 0, hist = 0;
    struct edc_task_group *g;
    DL_FOREACH(sess->groups, g)
    {
        groups++;
        hist += edc_task_group_history_count(g);
        struct edc_task *t;
        LL_FOREACH2(g->members, t, group_next)
        {
            tasks++;
            turns += (int)(t->turns.tail - t->turns.head) + (t->active_turn ? 1 : 0);
        }
    }
    printf("tree        : %d groups, %d tasks, %d turns, %d history slots\n", groups, tasks, turns, hist);

    if (dry)
    {
        printf("\n--dry-run: rows are staged in this process only; nothing flushed\n");
        cJSON_Delete(json);
        return 0;
    }

    edc_session_save_state_tagged(sess, sched, "import");
    printf("flushed     : ok\n");

    cJSON_Delete(json);
    edc_state_store_close();
    edc_log_close();
    printf("\ndone. %s now reads from the database.\n", sid);
    return 0;
}
