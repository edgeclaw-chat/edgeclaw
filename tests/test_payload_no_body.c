/* test_payload_no_body: edc_task_user_payload must not read messages.body.
 *
 * The sidebar snapshot asks every task for its folded-paste payload. The only fields that
 * question needs -- role, synthetic and payload -- are all separate COLUMNS in the store.
 * The old path reached them through edc_msg_get -> msg_from_columns, which unconditionally
 * cJSON_Parse()s the whole body (the message text, possibly a long read_file output) just
 * to discover role=="user". That was the top CPU hotspot after the token fix.
 *
 * The fix asks SQLite for the payload column directly and never touches body. This test
 * installs a SQLite authorizer that counts SQLITE_READ of the messages.body column, calls
 * edc_task_user_payload, and asserts zero body reads. Against the old implementation the
 * count is >0 (RED); with the lightweight query it is 0 (GREEN). A payload-content
 * assertion guards against a vacuous pass (returning NULL / empty without reading anything).
 */

#include "sqlite3.h"
#include "cJSON.h"
#include "test_ws.h"
#include "edc_compat.h"
#include "edc_config.h"
#include "edc_state_store.h"
#include "edc_task.h"
#include "edc_turn.h"
#include "sds.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failed = 0;
static int g_passed = 0;

#define CHECK(cond, msg)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        if (cond)                                                                                                      \
        {                                                                                                              \
            printf("  ok   %s\n", msg);                                                                                \
            g_passed++;                                                                                                \
        }                                                                                                              \
        else                                                                                                           \
        {                                                                                                              \
            printf("  FAIL %s\n", msg);                                                                                \
            g_failed++;                                                                                                \
        }                                                                                                              \
    } while (0)

/* Authorizer: count reads of the messages.body column. arg1=table, arg2=column for SQLITE_READ. */
struct body_read_ctx
{
    int count;
};

static int count_body_reads(void *userdata, int action, const char *arg1, const char *arg2,
                            const char *db_name, const char *trigger)
{
    (void)db_name;
    (void)trigger;
    struct body_read_ctx *ctx = (struct body_read_ctx *)userdata;
    if (action == SQLITE_READ && arg1 && strcmp(arg1, "messages") == 0 && arg2 && strcmp(arg2, "body") == 0)
        ctx->count++;
    return SQLITE_OK;
}

int main(void)
{
    edc_test_workspace();

    const int64_t id = edc_test_id("payload-task");
    const int64_t sid = edc_test_session_id("s-payload");
    struct edc_task *task = edc_task_new(id, sid, id, "payload task", edc_channel_adapter);
    struct edc_turn *turn = edc_turn_new_for(sid, id, id, 0);
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "role", "user");
    /* A deliberately long body: this is what the old path parsed in full for nothing. */
    char *big = (char *)malloc(8192);
    memset(big, 'x', 8191);
    big[8191] = '\0';
    cJSON_AddStringToObject(msg, "content", big);
    free(big);
    cJSON *payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "line-1", "folded original");
    cJSON_AddItemToObject(msg, "payload", payload);
    CHECK(edc_turn_add_message(turn, msg), "append user message with payload"); /* takes ownership */
    task->active_turn = turn;

    struct body_read_ctx ctx = {0};
    sqlite3 *db = edc_state_store_handle();
    CHECK(db != NULL, "store handle available");
    sqlite3_set_authorizer(db, count_body_reads, &ctx);

    cJSON *got = edc_task_user_payload(task);

    sqlite3_set_authorizer(db, NULL, NULL); /* restore before any teardown query */

    CHECK(got != NULL, "payload was returned (anti-vacuous guard)");
    if (got)
    {
        const cJSON *v = cJSON_GetObjectItem(got, "line-1");
        CHECK(v != NULL && cJSON_IsString(v) && strcmp(v->valuestring, "folded original") == 0,
              "payload content is correct");
        cJSON_Delete(got);
    }

    CHECK(ctx.count == 0, "edc_task_user_payload never read messages.body");

    edc_task_free(task);

    /* Semantic guard: if the ACTIVE turn has a user message but no payload, the lookup must
     * stop there -- it must NOT fall through to an older committed turn's payload. The old
     * turn_user_message-based code behaved this way; the lightweight query must too. */
    {
        const int64_t id2 = edc_test_id("payload-task-2");
        struct edc_task *t2 = edc_task_new(id2, sid, id2, "no-payload task", edc_channel_adapter);

        struct edc_turn *old_turn = edc_turn_new_for(sid, id2, id2, 1);
        cJSON *old_msg = cJSON_CreateObject();
        cJSON_AddStringToObject(old_msg, "role", "user");
        cJSON_AddStringToObject(old_msg, "content", "old typed line");
        cJSON *old_payload = cJSON_CreateObject();
        cJSON_AddStringToObject(old_payload, "line-old", "old folded original");
        cJSON_AddItemToObject(old_msg, "payload", old_payload);
        CHECK(edc_turn_add_message(old_turn, old_msg), "append committed user message with payload");
        edc_task_add_turn(t2, old_turn);

        struct edc_turn *active = edc_turn_new_for(sid, id2, id2, 2);
        cJSON *plain = cJSON_CreateObject();
        cJSON_AddStringToObject(plain, "role", "user");
        cJSON_AddStringToObject(plain, "content", "no payload here");
        CHECK(edc_turn_add_message(active, plain), "append active user message without payload");
        t2->active_turn = active;

        cJSON *wrong = edc_task_user_payload(t2);
        CHECK(wrong == NULL, "active turn's payload-less user message does not fall through to an older turn");
        if (wrong)
            cJSON_Delete(wrong);

        edc_task_free(t2);
    }

    edc_test_store_down(NULL);

    printf("\n%d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
