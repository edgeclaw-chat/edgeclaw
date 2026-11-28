/* test_msg_tokens_readonly: token accounting must be a READ, not a write.
 *
 * The sidebar pulls a task-tree snapshot on every refresh. For each group it asks for the
 * group's current context token count, which walks the group history and sums the stored
 * token estimates by message id (edc_msg_tokens_by_ids). That sum is semantically a pure
 * read -- it changes nothing. The old implementation staged the ids into a TEMP table
 * (CREATE + DELETE + INSERT) before the SUM, which opened a real write transaction and
 * committed to the WAL on every single snapshot. On a busy session that meant a WAL commit
 * just to display a number.
 *
 * The oracle here is sqlite3_total_changes: it counts rows changed by INSERT/UPDATE/DELETE.
 * A pure read must leave it untouched. With the old TEMP-table staging, calling the sum
 * bumps it by the number of staged ids, so this assertion is RED before the fix and GREEN
 * after. The value assertion is the anti-vacuous guard: it proves the ids really resolved
 * to rows (a no-op that reads nothing would also leave total_changes unchanged).
 */

#include "sqlite3.h"
#include "cJSON.h"
#include "test_ws.h"
#include "edc_compat.h"
#include "edc_config.h"
#include "edc_state_store.h"
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

    /* Three messages with different lengths so the token sum is a specific, non-round
     * number that cannot pass by accident. */
    CHECK(edc_turn_add_message(turn, message("aaaaaaaaaaaaaaaa")), "append message 0");
    CHECK(edc_turn_add_message(turn, message("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb")), "append message 1");
    CHECK(edc_turn_add_message(turn, message("cccccccc")), "append message 2");

    struct edc_msg_ids ids = {0};
    CHECK(edc_msg_ids_for_turn(turn->task_id, turn->turn_ord, 0, turn->msg_count, &ids),
          "resolve message ids from the store");
    CHECK(ids.n == 3, "all three messages resolved (anti-vacuous guard)");

    /* First call warms any caches and establishes the expected value. We assert on the
     * SECOND call's change counter, after the store is warm -- exactly the state a sidebar
     * refresh sees. */
    const int64_t expected = edc_msg_tokens_by_ids((const char *const *)ids.ids, ids.n);
    CHECK(expected > 0, "token sum is positive (ids really resolved to rows)");

    sqlite3 *db = edc_state_store_handle();
    CHECK(db != NULL, "store handle available");

    const int changes_before = sqlite3_total_changes(db);
    const int64_t got = edc_msg_tokens_by_ids((const char *const *)ids.ids, ids.n);
    const int changes_after = sqlite3_total_changes(db);

    CHECK(got == expected, "repeated sum returns the same value");
    CHECK(changes_after == changes_before,
          "token sum is a pure read: sqlite3_total_changes unchanged (no TEMP-table write)");

    edc_msg_ids_free(&ids);
    edc_turn_free(turn);
    edc_test_store_down(NULL);

    printf("\n%d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
