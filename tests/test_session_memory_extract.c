/* L2 extraction: an idle user group's unfolded history -> session memory.
 *
 * The third L2 write path (fold-chained curation and manual `/session memo` are the
 * other two). This file pins the design's contract: the due predicate, the material
 * (raw history + full index, NO old group memory), the cut + whole-turn cursor, the
 * corpus cap and its multi-round continuation, watermark idempotence, the fold
 * landing clamp, failure/backoff, and the tri-state serialization. */

#include "edc_compat.h"
#include "edc_config.h"
#include "edc_context.h"
#include "edc_executor.h"
#include "edc_group_fold.h"
#include "edc_llm_actor.h"
#include "edc_scheduler.h"
#include "edc_session.h"
#include "edc_session_manager.h"
#include "edc_session_memory.h"
#include "edc_state_serializer.h"
#include "edc_task.h"
#include "edc_task_group.h"
#include "edc_turn.h"
#include "log.h"
#include "utlist_fix.h"
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

volatile sig_atomic_t _run_loop = 1;

static void install_resolver(struct edc_scheduler *sched, struct edc_session *sess)
{
    struct edc_session_manager *sm = g_new0(struct edc_session_manager, 1);
    sm->sessions = g_new0(struct edc_session *, 1);
    sm->sessions[0] = sess;
    sm->session_count = sess ? 1 : 0;
    sm->active_index = 0;
    sched->sessions = sm; /* 测试进程内不释放: 与测试同生共死 */
}

/* ══════════════════════════════════════════════════════════════
 *  Test framework
 *  ══════════════════════════════════════════════════════════════ */

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

/* ══════════════════════════════════════════════════════════════
 *  Helpers
 *  ══════════════════════════════════════════════════════════════ */

#define OLD_TS "2000-01-01 00:00:00"    /* far in the past: definitely idle */
#define RECENT_TS "2099-01-01 00:00:00" /* in the future: definitely not idle */

static struct edc_config *make_config(void)
{
    struct edc_config *cfg = g_new0(struct edc_config, 1);
    cfg->workspace = sdsnew("/tmp/test_memo_extract_workspace");
    return cfg;
}

/* Append a committed turn of |n| identical user messages carrying |ts|. */
static void give_turn(struct edc_task *t, const char *text, const char *ts, int n)
{
    struct edc_turn *turn = edc_turn_new();
    turn->task_id = strdup(t->task_id);
    for (int i = 0; i < n; i++)
    {
        cJSON *m = cJSON_CreateObject();
        cJSON_AddStringToObject(m, "role", "user");
        cJSON_AddStringToObject(m, "content", text);
        if (ts)
            cJSON_AddStringToObject(m, "ts", ts);
        edc_queue_push(&turn->messages, m);
    }
    edc_task_add_turn(t, turn);
}

/* A user group whose single (leader) member is finished and holds one |n|-message
 * committed turn. */
static struct edc_task_group *make_finished_user_group(struct edc_session *session, const char *gid, const char *text,
                                                       const char *ts, int n)
{
    struct edc_task *leader = edc_task_new(gid, session->session_id, gid, text, edc_channel_adapter);
    struct edc_task_group *group = edc_task_group_new(leader);
    edc_session_add_group(session, group);
    give_turn(leader, text, ts, n);
    edc_task_await_exec(leader);
    edc_task_mark_completed(leader);
    leader->completion_seq = 1;
    return group;
}

static struct edc_task *find_extract_task(struct edc_session *session)
{
    struct edc_task_group *sg = edc_session_find_system_group(session);
    if (!sg)
        return NULL;
    struct edc_task *t;
    LL_FOREACH2(sg->members, t, group_next)
    {
        if (t->sink == EDC_SINK_SESSION_MEMORY && t->memo_kind == EDC_MEMO_KIND_EXTRACT && !edc_task_is_finished(t))
            return t;
    }
    return NULL;
}

static const char *task_input(struct edc_task *t)
{
    if (!t || !t->active_turn || edc_queue_length(&t->active_turn->messages) == 0)
        return NULL;
    cJSON *m = t->active_turn->messages.data[t->active_turn->messages.head];
    return cJSON_GetStringValue(cJSON_GetObjectItem(m, "content"));
}

static void finish_task(struct edc_task *t)
{
    edc_task_force_move(t, EDC_TASKSTEP_DONE, EDC_LIFE_COMPLETED);
}

static int g_send_llm_calls = 0;
static void stub_send_llm(struct edc_scheduler *sched, struct edc_task *task, void *request_id)
{
    (void)sched;
    (void)task;
    (void)request_id;
    g_send_llm_calls++;
}

/* ══════════════════════════════════════════════════════════════
 *  Test 1: the due predicate
 *  ══════════════════════════════════════════════════════════════ */

static void test_due_predicate(void)
{
    TEST("due: idle + something extractable, refused while busy / folded / capped");

    struct edc_config *cfg = make_config();
    struct edc_session *session = edc_session_new(cfg, "sess-due", "Due");
    struct edc_task_group *group = make_finished_user_group(session, "g1", "some work", OLD_TS, 2);
    time_t now = time(NULL);

    CHECK(edc_session_memory_extract_due(group, cfg, now), "idle + pending => due");
    CHECK(edc_session_memory_extract_due(group, cfg, now - 1000), "idle clock not yet elapsed => not due");
    CHECK(!edc_session_memory_extract_due(NULL, cfg, now), "NULL group => not due");

    /* Not idle: a member still running. */
    struct edc_task *busy = edc_task_new("2", session->session_id, "g1", "busy", edc_channel_adapter);
    edc_task_group_add_task(group, busy);
    CHECK(!edc_session_memory_extract_due(group, cfg, now), "a running member => not due");
    edc_task_await_exec(busy);
    edc_task_mark_completed(busy);
    busy->completion_seq = 2;
    CHECK(edc_session_memory_extract_due(group, cfg, now), "member finished => due again");

    /* Nothing extractable: the watermark covers every committed message. */
    struct edc_task *leader = group->leader;
    ((struct edc_turn *)leader->turns.data[leader->turns.head])->memo_prefix = 2;
    CHECK(edc_task_group_extractable_msg_count(group) == 0, "watermark at the end => nothing pending");
    CHECK(!edc_session_memory_extract_due(group, cfg, now), "nothing pending => not due");
    ((struct edc_turn *)leader->turns.data[leader->turns.head])->memo_prefix = 0;

    /* Fold has priority: a due fold suppresses extraction. */
    int small_window = (int)(edc_context_group_ctx_tokens(group, NULL) + EDC_FOLD_RESERVE_TOKENS - 1);
    cfg->context_window = small_window;
    CHECK(edc_group_fold_due(group, cfg), "window tight enough => fold is due");
    CHECK(!edc_session_memory_extract_due(group, cfg, now), "fold due => extraction refused (priority)");
    cfg->context_window = 0; /* unknown window => fold never due, extraction proceeds */

    /* The system group is never extracted. */
    group->kind = EDC_GROUP_KIND_SYSTEM;
    CHECK(!edc_session_memory_extract_due(group, cfg, now), "system group => not due");
    group->kind = EDC_GROUP_KIND_USER;

    /* Failure cap. */
    group->memo_extract_failures = EDC_MEMO_EXTRACT_MAX_FAILURES;
    CHECK(!edc_session_memory_extract_due(group, cfg, now), "at the failure cap => not due");
    edc_session_memory_extract_note_progress(group);
    CHECK(group->memo_extract_failures == 0, "note_progress clears the streak");
    CHECK(edc_session_memory_extract_due(group, cfg, now), "cleared => due again");

    edc_session_free(session);
    edc_config_free(cfg);
}

/* ══════════════════════════════════════════════════════════════
 *  Test 2: the material (raw history + index, NO group memory)
 *  ══════════════════════════════════════════════════════════════ */

static void test_material_excludes_group_memory(void)
{
    TEST("material: raw unfolded history + full index; the old group memory is NOT included");

    struct edc_config *cfg = make_config();
    struct edc_session *session = edc_session_new(cfg, "sess-mat", "Mat");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, session);

    struct edc_task_group *group = make_finished_user_group(session, "g1", "hello world", OLD_TS, 1);
    group->memory = sdsnew("# Old group memory\n- SECRET_REMEMBERED_FACT");

    /* One existing entry, so the index has a row to carry. */
    sds id = edc_session_memory_create_entry(session->memo, "Existing entry", "a summary", "detail", NULL);
    edc_session_memory_add_event(session->memo, id, EDC_MEMO_CREATED, "Existing entry", "a summary");
    sdsfree(id);

    struct edc_session *out = NULL;
    CHECK(edc_session_memory_extract_try(sched, sched->sessions, cfg, &out), "an extraction round started");
    CHECK(out == session, "reported the owning session");

    struct edc_task *task = find_extract_task(session);
    CHECK(task != NULL, "the extraction task is in the system group");
    CHECK(task && task->memo_kind == EDC_MEMO_KIND_EXTRACT, "memo_kind = EXTRACT");
    CHECK(task && task->sink_group == group, "knows which group it is extracting");
    CHECK(task && task->memo_cut_task_id && strcmp(task->memo_cut_task_id, "g1") == 0, "cut lands on the member");

    const char *material = task_input(task);
    CHECK(material != NULL, "the prompt is attached");
    CHECK(material && strstr(material, "## Group history to extract from") != NULL, "material header present");
    CHECK(material && strstr(material, "hello world") != NULL, "the raw history is in the material");
    CHECK(material && strstr(material, "## Existing session entries") != NULL, "the index section is present");
    CHECK(material && strstr(material, "Existing entry") != NULL, "an existing entry row is in the index");
    CHECK(material && strstr(material, "SECRET_REMEMBERED_FACT") == NULL,
          "the OLD group memory is NOT in the material (L2 index is the known-reference)");

    edc_session_free(session);
    edc_scheduler_free(sched);
    edc_config_free(cfg);
}

/* ══════════════════════════════════════════════════════════════
 *  Test 3: cut + corpus cap + multi-round continuation
 *  ══════════════════════════════════════════════════════════════ */

static void test_cap_multi_round_and_idempotence(void)
{
    TEST("cap: an over-cap turn is extracted in rounds; the cursor is idempotent");

    struct edc_config *cfg = make_config();
    struct edc_session *session = edc_session_new(cfg, "sess-cap", "Cap");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, session);

    struct edc_task *leader = edc_task_new("g1", session->session_id, "g1", "big", edc_channel_adapter);
    struct edc_task_group *group = edc_task_group_new(leader);
    edc_session_add_group(session, group);
    edc_task_await_exec(leader);
    edc_task_mark_completed(leader);
    leader->completion_seq = 1;

    /* One committed turn with two ~75k-token messages: together over the 131k cap,
     * each comfortably under it => round 1 takes the first, round 2 the second. */
    struct edc_turn *turn = edc_turn_new();
    turn->task_id = strdup("g1");
    sds a = sdsnewlen(NULL, 300000);
    memset(a, 'A', 300000);
    a = sdscat(a, " MARK_A");
    sds b = sdsnewlen(NULL, 300000);
    memset(b, 'B', 300000);
    b = sdscat(b, " MARK_B");
    const char *texts[2] = {a, b};
    for (int i = 0; i < 2; i++)
    {
        cJSON *m = cJSON_CreateObject();
        cJSON_AddStringToObject(m, "role", "user");
        cJSON_AddStringToObject(m, "content", texts[i]);
        cJSON_AddStringToObject(m, "ts", OLD_TS);
        edc_queue_push(&turn->messages, m);
    }
    sdsfree(a);
    sdsfree(b);
    edc_task_add_turn(leader, turn);

    /* Round 1: only the first message fits under the cap. */
    struct edc_session *out = NULL;
    CHECK(edc_session_memory_extract_try(sched, sched->sessions, cfg, &out), "round 1 started");
    struct edc_task *t1 = find_extract_task(session);
    CHECK(t1 != NULL, "round 1 task exists");
    CHECK(t1 && t1->memo_cut_turn == 0 && t1->memo_cut_prefix == 1, "cut = turn 0, first message only");
    const char *m1 = task_input(t1);
    CHECK(m1 && strstr(m1, "MARK_A") != NULL, "round 1 carries the first message");
    CHECK(m1 && strstr(m1, "MARK_B") == NULL, "round 1 does NOT carry the second (over the cap)");

    CHECK(edc_session_memory_extract_land(t1), "round 1 landed");
    CHECK(turn->memo_prefix == 1, "watermark advanced to 1");
    CHECK(edc_task_group_extractable_msg_count(group) == 1, "one message still pending");
    finish_task(t1);

    /* Round 2: the remainder. */
    out = NULL;
    CHECK(edc_session_memory_extract_try(sched, sched->sessions, cfg, &out), "round 2 started");
    struct edc_task *t2 = find_extract_task(session);
    CHECK(t2 != NULL && t2 != t1, "round 2 is a fresh task");
    CHECK(t2 && t2->memo_cut_prefix == 2, "round 2's cut covers the whole turn");
    const char *m2 = task_input(t2);
    CHECK(m2 && strstr(m2, "MARK_B") != NULL, "round 2 carries the second message");
    CHECK(m2 && strstr(m2, "MARK_A") == NULL, "round 2 does NOT re-send the first (cursor)");

    CHECK(edc_session_memory_extract_land(t2), "round 2 landed");
    CHECK(turn->memo_prefix == 2, "watermark at the end");
    CHECK(edc_task_group_extractable_msg_count(group) == 0, "nothing pending");
    finish_task(t2);

    /* Idempotent: a rescan with nothing pending starts nothing. */
    out = NULL;
    CHECK(!edc_session_memory_extract_try(sched, sched->sessions, cfg, &out), "no pending => no new round");
    CHECK(find_extract_task(session) == NULL, "no extraction task was created");

    edc_session_free(session);
    edc_scheduler_free(sched);
    edc_config_free(cfg);
}

/* ══════════════════════════════════════════════════════════════
 *  Test 4: the fold landing clamps the memo watermark up
 *  ══════════════════════════════════════════════════════════════ */

static void test_fold_landing_clamps_memo_prefix(void)
{
    TEST("fold landing: what is folded is in L2 (memo_prefix clamped up to folded_prefix)");

    struct edc_config *cfg = make_config();
    struct edc_session *session = edc_session_new(cfg, "sess-clamp", "Clamp");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, session);
    sched->send_llm = stub_send_llm;
    g_send_llm_calls = 0;

    struct edc_task_group *group = make_finished_user_group(session, "g1", "only work", OLD_TS, 2);
    struct edc_turn *turn = group->leader->turns.data[group->leader->turns.head];

    struct edc_task *fold = edc_group_fold_start(sched, session, group, NULL);
    CHECK(fold != NULL, "a fold started");
    edc_scheduler_schedule(sched);
    CHECK(g_send_llm_calls == 1, "the fold request was dispatched");

    struct edc_llm_actor_resp resp;
    memset(&resp, 0, sizeof(resp));
    resp.reason = BC_LLM_FINISH_REASON_STOP;
    resp.content = sdsnew("# memory\n- folded");
    void *fold_req = fold->current_request_id;
    struct edc_advance_result fr = edc_executor_advance_llm_resp(&sched->executor, fold, fold_req, &resp);
    edc_scheduler_apply_advance(sched, fold, fold_req, &fr);

    CHECK(group->memory != NULL, "the fold landed a memory");
    CHECK(turn->folded_prefix > 0, "the fold advanced the folded watermark");
    CHECK(turn->memo_prefix >= turn->folded_prefix, "memo_prefix was clamped up to folded_prefix (folded => on L2)");

    edc_session_free(session);
    edc_scheduler_free(sched);
    edc_config_free(cfg);
}

/* ══════════════════════════════════════════════════════════════
 *  Test 5: failure accounting + no advance on a bad cut
 *  ══════════════════════════════════════════════════════════════ */

static void test_failure_backoff_and_bad_cut(void)
{
    TEST("failure: a bad cut advances nothing and counts a failure; the cap pauses automation");

    struct edc_config *cfg = make_config();
    struct edc_session *session = edc_session_new(cfg, "sess-fail", "Fail");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, session);

    struct edc_task_group *group = make_finished_user_group(session, "g1", "work", OLD_TS, 2);
    struct edc_turn *turn = group->leader->turns.data[group->leader->turns.head];

    /* A task whose cut does not resolve: the watermark must not move. */
    struct edc_task *task =
        edc_scheduler_spawn_system_task_as(sched, session, "extract session memory", EDC_SINK_SESSION_MEMORY);
    task->memo_kind = EDC_MEMO_KIND_EXTRACT;
    task->sink_group = group;
    task->memo_cut_task_id = strdup("does-not-exist");
    task->memo_cut_turn = 0;
    task->memo_cut_prefix = 2;

    CHECK(!edc_session_memory_extract_land(task), "an unresolvable cut lands nothing");
    CHECK(turn->memo_prefix == 0, "watermark unchanged");
    CHECK(group->memo_extract_failures == 1, "one failure recorded");

    edc_session_memory_extract_note_failure(group);
    edc_session_memory_extract_note_failure(group);
    CHECK(group->memo_extract_failures == EDC_MEMO_EXTRACT_MAX_FAILURES, "at the cap");
    CHECK(!edc_session_memory_extract_due(group, cfg, time(NULL)), "cap pauses automatic extraction");

    edc_session_memory_extract_note_progress(group);
    CHECK(group->memo_extract_failures == 0, "progress clears the streak");

    edc_session_free(session);
    edc_scheduler_free(sched);
    edc_config_free(cfg);
}

/* ══════════════════════════════════════════════════════════════
 *  Test 6: tri-state serialization roundtrip + v7 compatibility
 *  ══════════════════════════════════════════════════════════════ */

static void test_serialization_tristate_and_v7(void)
{
    TEST("serialize: memo_kind round-trips; a v7 archive (memo_compact) reads as COMPACT");

    struct edc_config *cfg = make_config();
    struct edc_session *session = edc_session_new(cfg, "sess-ser", "Ser");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, session);

    struct edc_task *leader = edc_task_new("1", session->session_id, "1", "leader", edc_channel_adapter);
    struct edc_task_group *group = edc_task_group_new(leader);
    edc_session_add_group(session, group);
    give_turn(leader, "x", NULL, 3);
    struct edc_turn *turn = leader->turns.data[leader->turns.head];
    turn->folded_prefix = 1;
    turn->memo_prefix = 2;
    edc_task_mark_ready(leader);
    edc_task_mark_suspended(leader);
    leader->memo_kind = EDC_MEMO_KIND_COMPACT;

    cJSON *json = edc_state_serialize(session, sched);
    CHECK(json != NULL, "serialize returns non-NULL");

    struct edc_session *s2 = edc_session_new(cfg, "sess-ser", "Ser");
    struct edc_scheduler *sched2 = edc_scheduler_new(cfg);
    install_resolver(sched2, s2);
    CHECK(edc_state_deserialize(json, s2, sched2), "v8 round-trips");
    struct edc_task *r = edc_session_find_task(s2, "1");
    CHECK(r != NULL, "task restored");
    CHECK(r && r->memo_kind == EDC_MEMO_KIND_COMPACT, "memo_kind=COMPACT round-trips");
    CHECK(r && ((struct edc_turn *)r->turns.data[r->turns.head])->memo_prefix == 2, "turn memo_prefix round-trips");
    CHECK(r && ((struct edc_turn *)r->turns.data[r->turns.head])->folded_prefix == 1, "turn folded_prefix round-trips");
    edc_session_free(s2);
    edc_scheduler_free(sched2);

    /* A v7 archive: the task carries the old `memo_compact` bool and the turn has no
     * `memo_prefix` at all. The loader must read COMPACT and normalize the turn's
     * memo_prefix up to folded_prefix (what the old binary implicitly assumed). */
    cJSON_ReplaceItemInObject(json, "version", cJSON_CreateNumber(7));
    cJSON *tasks = cJSON_GetObjectItem(json, "tasks");
    cJSON *tj = NULL;
    cJSON_ArrayForEach(tj, tasks)
    {
        if (cJSON_GetObjectItem(tj, "memo_kind"))
        {
            cJSON_DeleteItemFromObject(tj, "memo_kind");
            cJSON_AddBoolToObject(tj, "memo_compact", true);
        }
        cJSON *turns = cJSON_GetObjectItem(tj, "turns");
        cJSON *turnj = NULL;
        cJSON_ArrayForEach(turnj, turns)
        {
            cJSON_DeleteItemFromObject(turnj, "memo_prefix");
        }
    }

    struct edc_session *s3 = edc_session_new(cfg, "sess-ser", "Ser");
    struct edc_scheduler *sched3 = edc_scheduler_new(cfg);
    install_resolver(sched3, s3);
    CHECK(edc_state_deserialize(json, s3, sched3), "v7 archive loads");
    struct edc_task *r3 = edc_session_find_task(s3, "1");
    CHECK(r3 != NULL, "v7 task restored");
    CHECK(r3 && r3->memo_kind == EDC_MEMO_KIND_COMPACT, "v7 memo_compact=true reads as COMPACT");
    CHECK(r3 && ((struct edc_turn *)r3->turns.data[r3->turns.head])->memo_prefix ==
                    ((struct edc_turn *)r3->turns.data[r3->turns.head])->folded_prefix,
          "v7 missing memo_prefix normalizes to folded_prefix");
    edc_session_free(s3);
    edc_scheduler_free(sched3);

    cJSON_Delete(json);
    edc_session_free(session);
    edc_scheduler_free(sched);
    edc_config_free(cfg);
}

/* ══════════════════════════════════════════════════════════════
 *  main
 *  ══════════════════════════════════════════════════════════════ */

int main(void)
{
    test_due_predicate();
    test_material_excludes_group_memory();
    test_cap_multi_round_and_idempotence();
    test_fold_landing_clamps_memo_prefix();
    test_failure_backoff_and_bad_cut();
    test_serialization_tristate_and_v7();

    log_info("  ====== %d passed, %d failed ======", test_passed, test_failed);
    return test_failed ? 1 : 0;
}
