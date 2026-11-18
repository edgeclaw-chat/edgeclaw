/* End-to-end: UI page request -> scheduler actor handler -> DB read -> reply.
 *
 * test_chat_bubble_page.cpp pins the wire shape, test_chat_bubbles.c pins the
 * store API; THIS one pins that the handler is actually wired and reads the
 * store on the scheduler thread. Pure C: edc_scheduler.h pulls in sds.h, which
 * does not compile as C++ (SDS_HDR_VAR's implicit void* conversion).
 */
#include "edc_actor.h"
#include "edc_api.h"
#include "edc_compat.h"
#include "edc_config.h"
#include "edc_scheduler.h"
#include "edc_state_store.h"
#include "log.h"
#include "sds.h"
#include "test_ws.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failed = 0;
static int g_passed = 0;

#define CHECK(cond, ...)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            fprintf(stderr, "    FAIL: ");                                                                                \
            fprintf(stderr, __VA_ARGS__);                                                                                 \
            fprintf(stderr, "\n");                                                                                         \
            g_failed++;                                                                                                    \
        }                                                                                                                  \
        else                                                                                                               \
        {                                                                                                                  \
            printf("    ok: ");                                                                                            \
            printf(__VA_ARGS__);                                                                                           \
            printf("\n");                                                                                                  \
            g_passed++;                                                                                                    \
        }                                                                                                                  \
    } while (0)

static sds g_reply = NULL;
static char g_reply_sid[128] = {0};
static int g_replies = 0;

static void capture_page(struct edc_scheduler *sched, const char *session_id, sds payload)
{
    (void)sched;
    g_replies++;
    snprintf(g_reply_sid, sizeof(g_reply_sid), "%s", session_id ? session_id : "");
    if (g_reply)
        sdsfree(g_reply);
    g_reply = payload; /* owns it */
}

static char *request_json(const char *sid, int64_t before, int limit)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "session_id", sid);
    cJSON_AddNumberToObject(root, "before_seq", (double)before);
    cJSON_AddNumberToObject(root, "limit", limit);
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
}

static void drive(struct edc_scheduler *sched, const char *json)
{
    edc_actor_t *actor = edc_scheduler_get_actor(sched);
    edc_actor_msg_t *msg = edc_actor_msg_new(BC_ACTOR_MSG_CHAT_BUBBLE_PAGE, NULL, NULL, strdup(json),
                                             (edc_actor_data_free_fn)free);
    actor->handler(actor, msg); /* dispatches + frees msg */
}

/* Pull the decoded rows out of the reply into two parallel arrays. */
static int page_rows(int64_t *seqs, const char **lines, int cap, int *has_more_out)
{
    *has_more_out = 0;
    if (!g_reply)
        return -1;
    cJSON *root = cJSON_Parse(g_reply);
    if (!root)
        return -1;
    const cJSON *more = cJSON_GetObjectItem(root, "has_more");
    if (cJSON_IsBool(more))
        *has_more_out = cJSON_IsTrue(more) ? 1 : 0;
    const cJSON *arr = cJSON_GetObjectItem(root, "rows");
    int n = 0;
    if (cJSON_IsArray(arr))
    {
        const cJSON *item = NULL;
        cJSON_ArrayForEach(item, arr)
        {
            if (n >= cap)
                break;
            const cJSON *seq = cJSON_GetObjectItem(item, "seq");
            const cJSON *line = cJSON_GetObjectItem(item, "line");
            if (cJSON_IsNumber(seq) && cJSON_IsString(line) && line->valuestring)
            {
                seqs[n] = (int64_t)seq->valuedouble;
                lines[n] = line->valuestring;
                n++;
            }
        }
    }
    cJSON_Delete(root);
    return n;
}

int main(void)
{
    struct edc_config *cfg = calloc(1, sizeof(*cfg));
    cfg->workspace = sdsnew(edc_test_workspace());

    const char *sid = "sess-7";
    for (int i = 0; i < 25; i++)
    {
        char line[128];
        snprintf(line, sizeof(line), "sys\\t0\\t\\t\\tbubble %d", i);
        int64_t seq = 0;
        if (!edc_chat_bubble_append(sid, line, &seq) || seq != i + 1)
        {
            fprintf(stderr, "seed append #%d failed\n", i);
            return 2;
        }
    }

    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    if (!sched)
        return 2;
    sched->chat_bubble_page_reply = capture_page;

    int64_t seqs[32];
    const char *lines[32];
    int more = 0;

    printf("-- newest page --\n");
    {
        char *json = request_json(sid, (int64_t)INT64_MAX, 10);
        drive(sched, json);
        free(json);
        CHECK(g_replies == 1, "handler produced a reply, got %d", g_replies);
        CHECK(strcmp(g_reply_sid, sid) == 0, "reply names the requested session (got '%s', want '%s')", g_reply_sid, sid);
        int n = page_rows(seqs, lines, 32, &more);
        CHECK(n == 10, "newest page has 10 rows, got %d", n);
        if (n == 10)
            CHECK(seqs[0] == 16 && seqs[9] == 25, "newest page is seq 16..25 (got %lld..%lld)",
                  (long long)seqs[0], (long long)seqs[9]);
        CHECK(more, "has_more is true while older pages exist");
    }

    printf("-- older page by cursor --\n");
    {
        char *json = request_json(sid, 16, 10);
        drive(sched, json);
        free(json);
        CHECK(g_replies == 2, "second request produced a second reply");
        int n = page_rows(seqs, lines, 32, &more);
        CHECK(n == 10, "second page has 10 rows, got %d", n);
        if (n == 10)
            CHECK(seqs[0] == 6 && seqs[9] == 15, "second page is seq 6..15, no overlap (got %lld..%lld)",
                  (long long)seqs[0], (long long)seqs[9]);
    }

    printf("-- last page + unknown session + malformed request --\n");
    {
        char *json = request_json(sid, 6, 10);
        drive(sched, json);
        free(json);
        int n = page_rows(seqs, lines, 32, &more);
        CHECK(n == 5 && !more, "last page has 5 rows and has_more is false, got %d/%d", n, more);

        int before = g_replies;
        json = request_json("never-existed", (int64_t)INT64_MAX, 10);
        drive(sched, json);
        free(json);
        CHECK(g_replies == before + 1, "an unknown session still gets an honest empty page");
        n = page_rows(seqs, lines, 32, &more);
        CHECK(n == 0 && !more, "unknown session page is empty, not an error");

        before = g_replies;
        drive(sched, "{not json");
        CHECK(g_replies == before, "a malformed request is dropped without a reply");
        drive(sched, "{\"session_id\":\"s\",\"before_seq\":1,\"limit\":0}");
        CHECK(g_replies == before, "a zero limit is dropped without a reply");
    }

    if (g_reply)
        sdsfree(g_reply);
    edc_scheduler_free(sched);
    sdsfree(cfg->workspace);
    free(cfg);
    edc_test_store_down(NULL);

    printf("\n%d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
