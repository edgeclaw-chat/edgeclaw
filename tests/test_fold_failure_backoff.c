/* Regression: a failed memory round must NOT re-send the same giant corpus.
 *
 * In the real DB (group 130) 33 consecutive fold tasks had byte-identical
 * ~321 KB prompts: every request hit the 300s non-streaming timeout, the
 * watermark (folded_prefix) never advanced, and the next round rebuilt the
 * exact same corpus from the same prefix. A transport blip became a
 * perpetual motion machine of 130k-token requests.
 *
 * Pinned here: each consecutive failure shrinks the corpus budget for that
 * group, so the next fold/extract prompt is strictly smaller and its cut
 * moves back; a successful landing restores the full budget. The budget
 * lives on the group (persisted), so a restart does not silently reset it.
 */

#include "test_ws.h"
#include "edc_compat.h"
#include "edc_config.h"
#include "edc_group_fold.h"
#include "edc_scheduler.h"
#include "edc_session.h"
#include "edc_session_manager.h"
#include "edc_session_memory.h"
#include "edc_task.h"
#include "edc_task_group.h"
#include "edc_turn.h"
#include "log.h"
#include "utlist_fix.h"
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

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

static void install_resolver(struct edc_scheduler *sched, struct edc_session *sess)
{
    struct edc_session_manager *sm = g_new0(struct edc_session_manager, 1);
    sm->sessions = g_new0(struct edc_session *, 1);
    sm->sessions[0] = sess;
    sm->session_count = sess ? 1 : 0;
    sm->active_index = 0;
    sched->sessions = sm;
}

static struct edc_config *make_config(void)
{
    struct edc_config *cfg = g_new0(struct edc_config, 1);
    cfg->workspace = sdsnew(edc_test_workspace());
    return cfg;
}

/* A leader + 8 finished members, each carrying one ~25k-token message.
 * The full 131072-token corpus cap takes ~5 of them; halving the cap has to
 * visibly move the cut back. Mirrors test_corpus_is_capped in test_group_fold.c. */
static struct edc_task_group *make_big_group(struct edc_session *session)
{
    struct edc_task *leader = edc_task_new(edc_test_id("1"), session->session_id, edc_test_id("1"), "leader",
                                           edc_channel_adapter);
    struct edc_task_group *group = edc_task_group_new(leader);
    edc_session_add_group(session, group);
    edc_task_await_exec(leader);
    edc_task_mark_completed(leader);
    leader->completion_seq = 1;
    struct edc_turn *lt = edc_turn_new();
    lt->task_id = leader->task_id;
    cJSON *lm = cJSON_CreateObject();
    cJSON_AddStringToObject(lm, "role", "user");
    cJSON_AddStringToObject(lm, "content", "tiny leader");
    edc_turn_add_message(lt, lm);
    edc_task_add_turn(leader, lt);

    for (uint32_t seq = 2; seq <= 9; seq++)
    {
        char id[8], tag[32];
        snprintf(id, sizeof(id), "%u", seq);
        snprintf(tag, sizeof(tag), "TAG%u:", seq);
        struct edc_task *t = edc_task_new(edc_test_id(id), session->session_id, group->group_id, id,
                                          edc_channel_adapter);
        edc_task_group_add_task(group, t);
        struct edc_turn *turn = edc_turn_new();
        turn->task_id = t->task_id;
        /* 100000 ASCII chars => ~25000 tokens by the project's estimator. */
        sds big = sdsnewlen(NULL, 100000);
        memset(big, 'A' + (seq % 26), 100000);
        big = sdscat(big, tag);
        cJSON *m = cJSON_CreateObject();
        cJSON_AddStringToObject(m, "role", "user");
        cJSON_AddStringToObject(m, "content", big);
        edc_turn_add_message(turn, m);
        sdsfree(big);
        edc_task_add_turn(t, turn);
        edc_task_await_exec(t);
        edc_task_mark_completed(t);
        t->completion_seq = seq;
    }
    return group;
}

static void test_fold_failure_shrinks_corpus(void)
{
    TEST("fold: consecutive failures shrink the corpus, a landing restores it");
    struct edc_config *cfg = make_config();
    struct edc_session *session = edc_session_new(cfg, edc_test_session_id("sess-fold-backoff"), "FoldBackoff");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, session);
    struct edc_task_group *group = make_big_group(session);

    struct edc_fold_cut cut0, cut1, cut2;
    sds p0 = edc_group_fold_build_prompt(group, NULL, &cut0);
    CHECK(p0 != NULL, "baseline prompt built");
    CHECK(group->fold_corpus_budget == 0, "a fresh group uses the full (default) corpus cap");
    if (!p0)
        return;
    size_t len0 = sdslen(p0);
    log_info("    baseline: %zu bytes, cut task=%" PRId64, len0, cut0.task_id);

    /* One failure: the next round must be strictly SMALLER and cut earlier. */
    edc_group_fold_note_failure(group);
    sds p1 = edc_group_fold_build_prompt(group, NULL, &cut1);
    CHECK(p1 != NULL, "prompt after 1 failure built");
    if (!p1)
        return;
    CHECK(sdslen(p1) < len0, "after 1 failure the fold prompt is smaller (no byte-identical retry)");
    CHECK(cut1.task_id != cut0.task_id || cut1.prefix < cut0.prefix, "after 1 failure the cut moved back");
    CHECK(group->fold_corpus_budget > 0 && group->fold_corpus_budget < EDC_FOLD_MAX_CORPUS_TOKENS,
          "the group records a reduced corpus budget");

    /* A second failure shrinks again. */
    edc_group_fold_note_failure(group);
    sds p2 = edc_group_fold_build_prompt(group, NULL, &cut2);
    CHECK(p2 != NULL, "prompt after 2 failures built");
    if (p2)
    {
        CHECK(sdslen(p2) < sdslen(p1), "after 2 failures the prompt shrinks again");
        CHECK(group->fold_corpus_budget < EDC_FOLD_MIN_CORPUS_TOKENS + EDC_FOLD_MAX_CORPUS_TOKENS,
              "budget keeps decaying");
    }

    /* Success restores the full budget: the next round is back to the baseline
     * cut/prompt (same unwound messages), not stuck at the reduced cap. */
    edc_group_fold_note_progress(group);
    CHECK(group->fold_corpus_budget == 0, "a successful landing clears the reduced budget");
    struct edc_fold_cut cut3;
    sds p3 = edc_group_fold_build_prompt(group, NULL, &cut3);
    CHECK(p3 && sdslen(p3) == len0, "after success the full corpus is used again");
    CHECK(cut3.task_id == cut0.task_id && cut3.prefix == cut0.prefix, "after success the cut is back to baseline");

    sdsfree(p0);
    sdsfree(p1);
    if (p2)
        sdsfree(p2);
    if (p3)
        sdsfree(p3);
    edc_session_free(session);
    edc_scheduler_free(sched);
    edc_config_free(cfg);
}

static void test_budget_floor(void)
{
    TEST("fold: the decayed corpus budget never collapses to zero");
    struct edc_config *cfg = make_config();
    struct edc_session *session = edc_session_new(cfg, edc_test_session_id("sess-fold-floor"), "FoldFloor");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, session);
    struct edc_task_group *group = make_big_group(session);

    for (int i = 0; i < 12; i++)
        edc_group_fold_note_failure(group);
    CHECK(group->fold_corpus_budget >= EDC_FOLD_MIN_CORPUS_TOKENS,
          "even after many failures the budget respects the floor");
    sds p = edc_group_fold_build_prompt(group, NULL, NULL);
    CHECK(p != NULL && sdslen(p) > 0, "the fold still produces a usable (small) corpus");
    if (p)
        sdsfree(p);

    edc_session_free(session);
    edc_scheduler_free(sched);
    edc_config_free(cfg);
}

static void test_extract_failure_shrinks_corpus(void)
{
    TEST("extract: a failed round shrinks the next round's material too");
    struct edc_config *cfg = make_config();
    struct edc_session *session = edc_session_new(cfg, edc_test_session_id("sess-extract-backoff"), "ExBackoff");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, session);

    /* One member, two ~75k-token messages: the full cap takes the first one,
     * a halved cap must take a prefix of it or nothing at all -- in either case
     * the material must be strictly smaller than the baseline. */
    struct edc_task *leader = edc_task_new(edc_test_id("e1"), session->session_id, edc_test_id("e1"), "big",
                                           edc_channel_adapter);
    struct edc_task_group *group = edc_task_group_new(leader);
    edc_session_add_group(session, group);
    edc_task_await_exec(leader);
    edc_task_mark_completed(leader);
    leader->completion_seq = 1;
    group->busy_members = 0;
    group->idle_since = time(NULL) - (EDC_MEMO_EXTRACT_IDLE_SECS + 60);
    struct edc_turn *turn = edc_turn_new();
    turn->task_id = leader->task_id;
    for (int i = 0; i < 2; i++)
    {
        sds big = sdsnewlen(NULL, 300000);
        memset(big, 'X', 300000);
        big = sdscatprintf(big, " MARK_%d", i);
        cJSON *m = cJSON_CreateObject();
        cJSON_AddStringToObject(m, "role", "user");
        cJSON_AddStringToObject(m, "content", big);
        cJSON_AddStringToObject(m, "ts", "2000-01-01 00:00:00");
        edc_turn_add_message(turn, m);
        sdsfree(big);
    }
    edc_task_add_turn(leader, turn);

    struct edc_session *out = NULL;
    CHECK(edc_session_memory_extract_try(sched, sched->sessions, cfg, &out), "baseline extract round started");
    struct edc_task_group *sg = edc_session_find_system_group(session);
    struct edc_task *t0 = sg ? sg->members : NULL;
    CHECK(t0 != NULL, "baseline extract task exists");
    if (!t0)
        return;
    /* The prompt is the first user message of the task's active turn. */
    cJSON *msg0 = t0->active_turn ? edc_turn_msg_at(t0->active_turn, 0) : NULL;
    const char *p0 = msg0 ? cJSON_GetStringValue(cJSON_GetObjectItem(msg0, "content")) : NULL;
    CHECK(p0 != NULL, "baseline material readable");
    size_t len0 = p0 ? strlen(p0) : 0;
    log_info("    baseline extract material: %zu bytes, cut prefix=%u", len0, t0->memo_cut_prefix);

    /* Simulate a failed round: count the failure, then finish the task so the
     * next scan can start a fresh one. */
    edc_session_memory_extract_note_failure(group);
    edc_task_mark_failed(t0);
    CHECK(group->fold_corpus_budget > 0, "the failure records a reduced corpus budget on the group");

    out = NULL;
    CHECK(edc_session_memory_extract_try(sched, sched->sessions, cfg, &out), "next extract round started");
    struct edc_task *t1 = NULL;
    struct edc_task *m = NULL;
    LL_FOREACH2(sg->members, m, group_next)
    {
        if (m != t0 && !edc_task_is_finished(m))
            t1 = m;
    }
    CHECK(t1 != NULL, "a fresh extract task was created");
    if (t1)
    {
        cJSON *msg1 = t1->active_turn ? edc_turn_msg_at(t1->active_turn, 0) : NULL;
        const char *p1 = msg1 ? cJSON_GetStringValue(cJSON_GetObjectItem(msg1, "content")) : NULL;
        CHECK(p1 && strlen(p1) < len0, "after a failure the extract material is smaller");
        CHECK(t1->memo_cut_prefix <= t0->memo_cut_prefix, "the extract cut did not advance past the old one");
    }

    /* Success restores the full budget. */
    edc_session_memory_extract_note_progress(group);
    CHECK(group->fold_corpus_budget == 0, "a successful extract landing clears the reduced budget");

    edc_session_free(session);
    edc_scheduler_free(sched);
    edc_config_free(cfg);
}

int main(void)
{
    edc_test_workspace();
    log_info("=== memory round failure backoff ===");
    test_fold_failure_shrinks_corpus();
    test_budget_floor();
    test_extract_failure_shrinks_corpus();
    log_info("=== Results: %d passed, %d failed ===", test_passed, test_failed);
    return test_failed > 0 ? 1 : 0;
}
