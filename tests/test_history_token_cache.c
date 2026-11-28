/* test_history_token_cache: group-history token accounting is an in-memory SUM.
 *
 * The sidebar pulls a task-tree snapshot on every refresh and asks each group for its
 * current context token count, which sums the token estimates of the group's history.
 * The history stages message IDs (not messages), so the naive implementation collected the
 * ids and re-queried the store for their tokens on EVERY snapshot -- a store round trip and
 * (before test_msg_tokens_readonly's fix) a WAL commit each time, even though a message's
 * stored token estimate is immutable once appended.
 *
 * The fix caches each message's token alongside its id when the id is staged into the
 * history (edc_msg_ids_for_turn now fetches id+tokens in one query). This test proves the
 * sum no longer depends on the store: it builds a history from a real turn, records the
 * token sum, CLOSES the store, then asks for the sum again. With the cache the second sum
 * is identical (pure in-memory); with the old query-per-snapshot code the closed store
 * yields 0, so the assertion is RED before the fix and GREEN after.
 */

#include "cJSON.h"
#include "test_ws.h"
#include "edc_compat.h"
#include "edc_config.h"
#include "edc_state_store.h"
#include "edc_task.h"
#include "edc_task_group.h"
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

static cJSON *message(const char *content)
{
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "role", "user");
    cJSON_AddStringToObject(m, "content", content);
    return m;
}

int main(void)
{
    edc_test_workspace();

    struct edc_turn *turn = edc_test_turn_new();
    CHECK(turn != NULL, "turn created with a storage key");
    CHECK(edc_turn_add_message(turn, message("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")), "append message 0");
    CHECK(edc_turn_add_message(turn, message("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb")), "append message 1");
    CHECK(edc_turn_add_message(turn, message("cccccccccccc")), "append message 2");

    struct edc_task *leader = edc_task_new(edc_test_id("leader"), edc_test_session_id("s-cache"),
                                           edc_test_id("leader"), "leader", edc_channel_adapter);
    struct edc_task_group *group = edc_task_group_new(leader);
    CHECK(group != NULL, "group created with leader");

    /* Stage the turn's messages (id + token) into the history -- the same path the executor
     * and rebuild_history use. */
    edc_task_group_history_add_turn(group, turn);
    CHECK(edc_task_group_history_count(group) == 3, "history has 3 staged messages");

    const int64_t before = edc_task_group_history_tokens(group, 0, -1);
    CHECK(before > 0, "token sum is positive while store is open (anti-vacuous guard)");

    /* The critical step: kill the store. A cached, in-memory SUM must not care. */
    edc_state_store_close();

    const int64_t after = edc_task_group_history_tokens(group, 0, -1);
    CHECK(after == before,
          "history token sum is served from cache after store close (no per-snapshot query)");

    edc_task_group_free(group);
    edc_turn_free(turn);

    printf("\n%d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
