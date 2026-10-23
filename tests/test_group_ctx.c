#include "edc_context.h"
#include "test_ws.h"
#include "edc_group_fold.h"
#include "edc_task.h"
#include "edc_task_group.h"
#include "edc_turn.h"
#include "log.h"
#include <cJSON.h>
#include <stdio.h>
#include <string.h>

/* 组上下文规模的口径 (pi 的 anchor + 尾部估算)。两半都钉:
 *   - 锚的可用性: 位置信息 (last_hist_len)、折叠 epoch、折叠任务排除;
 *   - 尾部/兜底估算: 锚之后追加的历史、在跑 turn 的增量、没有锚时从零估一遍。
 * 全部用 ASCII 夹具: 40 个 'a' 的消息 = 8(结构) + (4(role)+40)/4 = 19。

 * 数值都是精确值 (估算器的公式是规格), 所以变异会精确变红。 */

static int test_failed = 0;
static int test_passed = 0;

#define TEST(name) log_info("  TEST: %s", name)
#define CHECK(cond, msg)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            log_error("    FAIL: %s", msg);                                                                            \
            test_failed++;                                                                                             \
        }                                                                                                              \
        else                                                                                                           \
            test_passed++;                                                                                             \
    } while (0)

/* 40 个 ASCII 字符的 user 消息 (估算 19)。 */
static cJSON *user_msg40(void)
{
    char json[128];
    char body[41];
    memset(body, 'a', 40);
    body[40] = '\0';
    snprintf(json, sizeof(json), "{\"role\":\"user\",\"content\":\"%s\"}", body);
    return cJSON_Parse(json);
}

static void add_history(struct edc_task_group *g, cJSON *msg)
{
    edc_task_group_history_add_owned(g, msg);
}

/* ── 1. 锚 + 尾部 ─────────────────────────────────────────── */

static void test_anchor_plus_tail(void)
{
    TEST("anchor: the provider's real usage, exact until something is appended");
    struct edc_task *t = edc_task_new("ctx-1", "s-1", "ctx-1", "L", edc_channel_adapter);
    struct edc_task_group *g = edc_task_group_new(t);
    add_history(g, user_msg40()); /* history[0] = system prompt 的位置 */

    edc_task_add_usage(t, 900, 100, 1000, 0);
    bool est = true;
    long long ctx = edc_context_group_ctx_tokens(g, &est);
    CHECK(ctx == 1000, "nothing appended yet: the number is the provider's own usage");
    CHECK(est == false, "and it is not marked as an estimate");

    add_history(g, user_msg40());
    est = false;
    ctx = edc_context_group_ctx_tokens(g, &est);
    CHECK(ctx == 1000 + 19, "appending a message after the reply adds its estimate to the anchor");
    CHECK(est == true, "once any estimate went in, the number is marked");

    edc_task_group_free(g);
}

/* ── 2. 锚什么时候失效 ────────────────────────────────────── */

static void test_anchor_validity(void)
{
    TEST("anchor: unusable once a fold lands, or without a position");
    struct edc_task *t = edc_task_new("ctx-2", "s-2", "ctx-2", "L", edc_channel_adapter);
    struct edc_task_group *g = edc_task_group_new(t);
    add_history(g, user_msg40());
    add_history(g, user_msg40());

    edc_task_add_usage(t, 900, 100, 1000, 0);
    bool est = true;
    CHECK(edc_context_group_ctx_tokens(g, &est) == 1000 && est == false, "baseline: the anchor holds");

    /* 折叠落地: 前缀被摘要换掉, 锚记的是换之前的那个大前缀。 */
    g->fold_epoch++;
    est = false;
    long long after_fold = edc_context_group_ctx_tokens(g, &est);
    CHECK(after_fold == 19 + 19, "a stale anchor is ignored: the fallback estimates what is left (system + kept)");
    CHECK(est == true, "and the fallback is an estimate");

    /* 没有位置信息 (老存档读回来的锚): 一样不可信。 */
    g->fold_epoch = t->last_fold_epoch;
    t->last_hist_len = 0;
    est = false;
    CHECK(edc_context_group_ctx_tokens(g, &est) == 19 + 19, "an anchor without a position is ignored too");
    CHECK(est == true, "the fallback is still an estimate");

    edc_task_group_free(g);
}

/* ── 3. 折叠任务不算会话 ──────────────────────────────────── */

static void test_fold_task_excluded(void)
{
    TEST("the fold task is not conversation: it never sets the width");
    struct edc_task *t = edc_task_new("ctx-3", "s-3", "ctx-3", "L", edc_channel_adapter);
    struct edc_task_group *g = edc_task_group_new(t);
    add_history(g, user_msg40());
    edc_task_add_usage(t, 900, 100, 1000, 0);
    long long base = edc_context_group_ctx_tokens(g, NULL);

    struct edc_task *fold = edc_task_new("ctx-3-fold", "s-3", "ctx-3", "fold", edc_channel_adapter);
    edc_task_group_add_task(g, fold);
    fold->sink = EDC_SINK_GROUP_MEMORY;
    edc_task_add_usage(fold, 999900, 100, 1000000, 0);
    CHECK(edc_context_group_ctx_tokens(g, NULL) == base, "a memory sink's huge usage does not widen the group");

    edc_task_group_free(g);
}

/* ── 4. 组 = 最宽的那个对话 ───────────────────────────────── */

static void test_widest_member(void)
{
    TEST("the group takes the widest member conversation");
    struct edc_task *t = edc_task_new("ctx-4", "s-4", "ctx-4", "L", edc_channel_adapter);
    struct edc_task_group *g = edc_task_group_new(t);
    add_history(g, user_msg40());
    edc_task_add_usage(t, 900, 100, 1000, 0);

    struct edc_task *t2 = edc_task_new("ctx-4b", "s-4", "ctx-4", "B", edc_channel_adapter);
    edc_task_group_add_task(g, t2);
    edc_task_add_usage(t2, 1900, 100, 2000, 0);

    bool est = true;
    CHECK(edc_context_group_ctx_tokens(g, &est) == 2000, "the larger anchor wins");
    CHECK(est == false, "and it is exact (nothing appended after that one)");

    edc_task_group_free(g);
}

/* ── 5. 兜底: 从零估一遍 ──────────────────────────────────── */

static void test_fallback_estimate(void)
{
    TEST("no anchor: estimate the next request (memory + system + kept + the running turn)");
    struct edc_task *t = edc_task_new("ctx-5", "s-5", "ctx-5", "L", edc_channel_adapter);
    struct edc_task_group *g = edc_task_group_new(t);
    add_history(g, user_msg40()); /* system prompt: 19 */

    bool est = false;
    CHECK(edc_context_group_ctx_tokens(g, &est) == 19, "system prompt alone");
    CHECK(est == true, "a from-scratch estimate is always marked");

    /* 一条已提交、未折叠的历史 */
    add_history(g, user_msg40());
    CHECK(edc_context_group_ctx_tokens(g, NULL) == 19 + 19, "unfolded history is counted");

    /* 一段记忆 (纯文本, 没有消息开销: 40 ASCII -> 10) */
    char body[41];
    memset(body, 'a', 40);
    body[40] = '\0';
    g->memory = sdsnew(body);
    CHECK(edc_context_group_ctx_tokens(g, NULL) == 10 + 19 + 19, "group memory is counted as text");

    /* 正在跑的成员带着自己的 turn */
    struct edc_turn *turn = edc_turn_new();
    edc_turn_add_user_message(turn, body);
    t->active_turn = turn;
    CHECK(edc_context_group_ctx_tokens(g, NULL) == 10 + 19 + 19 + 19, "the running turn is counted too");

    edc_task_group_free(g);
}

/* ── 6. 在跑的成员: 只算锚之后的 turn 增量 ────────────────── */

static void test_turn_delta(void)
{
    TEST("a running member's turn counts only what came after its last reply");
    struct edc_task *t = edc_task_new("ctx-6", "s-6", "ctx-6", "L", edc_channel_adapter);
    struct edc_task_group *g = edc_task_group_new(t);
    add_history(g, user_msg40());

    char body[41];
    memset(body, 'a', 40);
    body[40] = '\0';
    struct edc_turn *turn = edc_turn_new();
    edc_turn_add_user_message(turn, body); /* 请求发出时 turn 里已有 1 条 */
    t->active_turn = turn;

    edc_task_add_usage(t, 400, 100, 500, 0);
    bool est = true;
    CHECK(edc_context_group_ctx_tokens(g, &est) == 500, "the anchor covers the turn as it was sent");
    CHECK(est == false, "still exact");

    edc_turn_add_user_message(turn, body); /* 锚之后又长了一条 (工具结果等) */
    est = false;
    CHECK(edc_context_group_ctx_tokens(g, &est) == 500 + 19, "only the delta after the anchor is estimated");
    CHECK(est == true, "and it is an estimate now");

    edc_task_group_free(g);
}

/* ── 7. 折叠落地 => 锚集体失效 ────────────────────────────── */

static void test_fold_landing_invalidates(void)
{
    TEST("a fold landing bumps the epoch, so every anchor from before it is stale");
    struct edc_task *t = edc_task_new("ctx-8", "s-8", "ctx-8", "L", edc_channel_adapter);
    struct edc_task_group *g = edc_task_group_new(t);
    add_history(g, user_msg40());
    edc_task_add_usage(t, 900, 100, 1000, 0);
    CHECK(edc_context_group_ctx_tokens(g, NULL) == 1000, "baseline: the anchor is used");

    /* 折叠任务: sink_group 指向 g, 切点三元组记的是本轮折到哪。 */
    struct edc_task *fold = edc_task_new("ctx-8-fold", "s-8", "ctx-8", "fold", edc_channel_adapter);
    edc_task_group_add_task(g, fold);
    fold->sink = EDC_SINK_GROUP_MEMORY;
    fold->sink_group = g;
    fold->fold_cut_task_id = strdup("1");
    fold->fold_cut_turn = 0;
    fold->fold_cut_prefix = 0;
    edc_group_fold_deliver_memory(fold, "## memory");

    CHECK(g->fold_epoch == 1, "the fold landing bumps the group's epoch");
    bool est = false;
    long long ctx = edc_context_group_ctx_tokens(g, &est);
    CHECK(ctx < 1000 && est == true, "the anchor from before the fold is ignored (fallback estimate)");
    CHECK(ctx == (long long)strlen("## memory") / 4 + 19, "the fallback counts the new memory + system + kept");

    edc_task_group_free(g);
}

/* ── 8. 空组 ──────────────────────────────────────────────── */

static void test_empty_group(void)
{
    TEST("an empty group has no context");
    struct edc_task *t = edc_task_new("ctx-7", "s-7", "ctx-7", "L", edc_channel_adapter);
    struct edc_task_group *g = edc_task_group_new(t);
    bool est = true;
    CHECK(edc_context_group_ctx_tokens(g, &est) == 0, "no history, no memory, no turns -> 0");
    CHECK(est == false, "and nothing is claimed to be estimated");
    CHECK(edc_context_group_ctx_tokens(NULL, &est) == 0, "NULL group is 0");
    edc_task_group_free(g);
}

int main(void)
{
    edc_test_workspace(); /* open the disposable store before anything writes */
    log_info("=== group ctx: anchor + tail estimate ===");
    test_anchor_plus_tail();
    test_anchor_validity();
    test_fold_task_excluded();
    test_widest_member();
    test_fallback_estimate();
    test_turn_delta();
    test_fold_landing_invalidates();
    test_empty_group();

    printf("=== %d passed, %d failed ===\n", test_passed, test_failed);
    log_info("=== %d passed, %d failed ===", test_passed, test_failed);
    return test_failed ? 1 : 0;
}
