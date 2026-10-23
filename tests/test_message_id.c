/* test_message_id: the per-message identity, and the guarantee that it stays off the wire.
 *
 * Two properties, both of which fail silently if broken:
 *
 *  1. Every message created through edc_turn_add_* gets an id, the ids differ, and a
 *     message that already carries one keeps it. The last part is what makes the store's
 *     upsert safe: if a reload regenerated ids, every flush would insert the whole
 *     conversation again.
 *
 *  2. The id does NOT reach the model. The request builder allowlists wire keys, so any
 *     new store-only field is dropped -- this test pins that behaviour for `id`
 *     specifically, because leaking it to OpenAI is a 400.
 */

#include "cJSON.h"
#include "test_ws.h"
#include "edc_compat.h"
#include "edc_turn.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Provided by src/actor (the request builder's allowlist probe). */
extern bool edc_llm_wire_key_is_standard(const char *key);

static int g_failed = 0;
static int g_passed = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "    FAIL: %s\n", msg); g_failed++; } \
    else { printf("    ok: %s\n", msg); g_passed++; } \
} while (0)

static const char *msg_id(const cJSON *m)
{
    return cJSON_GetStringValue(cJSON_GetObjectItem((cJSON *)m, "id"));
}

int main(void)
{
    edc_test_workspace(); /* open the disposable store before anything writes */
    struct edc_turn *turn = edc_turn_new();

    printf("\n── every message gets an id ──\n");
    edc_turn_add_user_message(turn, "first");
    edc_turn_add_user_message(turn, "second");
    edc_turn_add_tool_message(turn, "call_1", "exec_shell", "output");
    edc_turn_add_assistant_message(turn, cJSON_Parse("{\"content\":\"hi\"}"));

    /* Walk the queue: it is a ring buffer, so read it the way the code does. */
    const char *ids[8] = {0};
    int n = 0;
    for (int i = 0; i < turn->msg_count && n < 8; i++)
    {
        const cJSON *m = (const cJSON *)edc_turn_msg_at(turn, i);
        ids[n] = msg_id(m);
        printf("        message %d id=%s\n", n, ids[n] ? ids[n] : "(none)");
        n++;
    }

    CHECK(n == 4, "4 messages queued");
    for (int i = 0; i < n; i++)
    {
        char buf[64];
        snprintf(buf, sizeof(buf), "message %d has an id", i);
        CHECK(ids[i] && ids[i][0], buf);
    }
    CHECK(n >= 2 && ids[0] && ids[1] && strcmp(ids[0], ids[1]) != 0, "ids are distinct");

    /* Format: m<ms>_<seq> -- sortable and parseable, so logs stay readable. */
    CHECK(ids[0] && ids[0][0] == 'm' && strchr(ids[0], '_') != NULL,
          "id has the m<ms>_<seq> shape");

    printf("\n── an existing id survives (this is what keeps the upsert stable) ──\n");
    {
        cJSON *m = cJSON_Parse("{\"role\":\"user\",\"content\":\"kept\",\"id\":\"m1_0\"}");
        edc_turn_add_message(turn, m); /* consumes m: read it back from the store */
        cJSON *kept = edc_turn_msg_at(turn, turn->msg_count - 1);
        CHECK(kept && msg_id(kept) && strcmp(msg_id(kept), "m1_0") == 0,
              "a message that already has an id keeps it verbatim");
        cJSON_Delete(kept);
    }

    printf("\n── the id must not reach the model ──\n");
    CHECK(edc_llm_wire_key_is_standard("role"), "role is a wire key");
    CHECK(edc_llm_wire_key_is_standard("content"), "content is a wire key");
    CHECK(!edc_llm_wire_key_is_standard("id"), "id is NOT a wire key (gets stripped)");
    CHECK(!edc_llm_wire_key_is_standard("mid"), "mid is NOT a wire key");
    CHECK(!edc_llm_wire_key_is_standard("ts"), "ts is NOT a wire key");
    CHECK(!edc_llm_wire_key_is_standard("render"), "render is NOT a wire key");

    edc_turn_free(turn);

    printf("\n=== %d passed, %d failed ===\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
