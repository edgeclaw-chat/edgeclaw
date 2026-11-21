/* L2 extraction: an idle user group's unfolded history -> session memory.
 *
 * The third L2 write path (fold-chained curation and manual `/session memo` are the
 * other two). This file pins the design's contract: the due predicate, the material
 * (raw history + full index, NO old group memory), the cut + whole-turn cursor, the
 * corpus cap and its multi-round continuation, watermark idempotence, the fold
 * landing clamp, failure/backoff, and the tri-state serialization. */

#include "edc_compat.h"
#include "test_ws.h"
#include "edc_config.h"
#include "edc_context.h"
#include "edc_executor.h"
#include "edc_group_fold.h"
#include "edc_llm_actor.h"
#include "edc_scheduler.h"
#include "edc_service.h"
#include "edc_session.h"
#include "edc_session_manager.h"
#include "edc_session_memory.h"
#include "edc_state_store.h"
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

#define OLD_TS "2000-01-01 00:00:00"    /* far in the past: old material, not old silence */
#define RECENT_TS "2099-01-01 00:00:00" /* in the future: definitely not idle */

static struct edc_config *make_config(void)
{
    struct edc_config *cfg = g_new0(struct edc_config, 1);
    cfg->workspace = sdsnew(edc_test_workspace());
    return cfg;
}

/* Append a committed turn of |n| identical user messages carrying |ts|. */
static void give_turn(struct edc_task *t, const char *text, const char *ts, int n)
{
    struct edc_turn *turn = edc_turn_new();
    for (int i = 0; i < n; i++)
    {
        cJSON *m = cJSON_CreateObject();
        cJSON_AddStringToObject(m, "role", "user");
        cJSON_AddStringToObject(m, "content", text);
        if (ts)
            cJSON_AddStringToObject(m, "ts", ts);
        edc_turn_add_message(turn, m);
    }
    edc_task_add_turn(t, turn);
}

/* Pretend the group has been idle for longer than the extraction window.
 *
 * The idle clock starts when the last member goes terminal (`idle_since`), NOT at the
 * newest message: a helper that finishes its member now leaves a group that is idle for
 * ~0s, whatever the messages say. Tests that want "this group has been sitting idle for
 * a while" say so here instead of backdating message timestamps. */
static void idle_long_ago(struct edc_task_group *group)
{
    if (!group)
        return;
    group->busy_members = 0;
    group->idle_since = time(NULL) - (EDC_MEMO_EXTRACT_IDLE_SECS + 60);
}

/* A user group whose single (leader) member is finished and holds one |n|-message
 * committed turn. It starts out idle-for-a-while, which is the state most tests are
 * about; the ones that care about the clock set it themselves. */
static struct edc_task_group *make_finished_user_group(struct edc_session *session, const char *gid, const char *text,
                                                       const char *ts, int n)
{
    struct edc_task *leader = edc_task_new(edc_test_id(gid), session->session_id, edc_test_id(gid), text, edc_channel_adapter);
    struct edc_task_group *group = edc_task_group_new(leader);
    edc_session_add_group(session, group);
    give_turn(leader, text, ts, n);
    edc_task_await_exec(leader);
    edc_task_mark_completed(leader);
    leader->completion_seq = 1;
    idle_long_ago(group);
    return group;
}

/* The two-stage extract starts L1 (EXTRACT_SUMMARY) + chained L2 (CURATE with a cut).
 * find_extract_task returns the L1 by default (the round's entry point); the L2 can be
 * found with find_extract_curate_task. */
static struct edc_task *find_extract_task(struct edc_session *session)
{
    struct edc_task_group *sg = edc_session_find_system_group(session);
    if (!sg)
        return NULL;
    struct edc_task *t;
    LL_FOREACH2(sg->members, t, group_next)
    {
        if (t->sink == EDC_SINK_SESSION_MEMORY && t->memo_kind == EDC_MEMO_KIND_EXTRACT_SUMMARY &&
            !edc_task_is_finished(t))
            return t;
    }
    return NULL;
}

static struct edc_task *find_extract_curate_task(struct edc_session *session)
{
    struct edc_task_group *sg = edc_session_find_system_group(session);
    if (!sg)
        return NULL;
    struct edc_task *t;
    LL_FOREACH2(sg->members, t, group_next)
    {
        if (t->sink == EDC_SINK_SESSION_MEMORY && t->memo_kind == EDC_MEMO_KIND_CURATE &&
            t->memo_cut_task_id && !edc_task_is_finished(t))
            return t;
    }
    return NULL;
}

static const char *task_input(struct edc_task *t)
{
    if (!t || !t->active_turn || t->active_turn->msg_count == 0)
        return NULL;
    cJSON *m = edc_turn_msg_at(t->active_turn, 0);
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
    struct edc_session *session = edc_session_new(cfg, edc_test_session_id("sess-due"), "Due");
    struct edc_task_group *group = make_finished_user_group(session, "g1", "some work", OLD_TS, 2);
    time_t now = time(NULL);

    CHECK(edc_session_memory_extract_due(group, cfg, now), "idle + pending => due");
    CHECK(!edc_session_memory_extract_due(group, cfg, now - 1000), "before the idle window elapses => not due");
    CHECK(!edc_session_memory_extract_due(NULL, cfg, now), "NULL group => not due");

    /* Not idle: a member still running. */
    struct edc_task *busy = edc_task_new(edc_test_id("2"), session->session_id, edc_test_id("g1"), "busy", edc_channel_adapter);
    edc_task_group_add_task(group, busy);
    CHECK(group->busy_members == 1, "attaching a live member makes the group busy");
    CHECK(!edc_session_memory_extract_due(group, cfg, now), "a running member => not due");
    edc_task_await_exec(busy);
    edc_task_mark_completed(busy);
    busy->completion_seq = 2;
    CHECK(group->busy_members == 0, "the last member finishing empties the busy count");
    idle_long_ago(group); /* finishing it now restarts the clock; say it was long ago */
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
    struct edc_session *session = edc_session_new(cfg, edc_test_session_id("sess-mat"), "Mat");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, session);

    struct edc_task_group *group = make_finished_user_group(session, "g1", "hello world", OLD_TS, 1);
    edc_state_store_group_memory_save(session->session_id, group->group_id,
                                      "# Old group memory\n- SECRET_REMEMBERED_FACT", 0, 0, 0);
    group->memory_tokens = 30;
    group->memory_updated_at = (uint64_t)time(NULL);

    /* One existing entry, so the index has a row to carry. */
    sds id = edc_session_memory_create_entry(session->memo, "Existing entry", "a summary", "detail", NULL);
    edc_session_memory_add_event(session->memo, id, EDC_MEMO_CREATED, "Existing entry", "a summary");
    sdsfree(id);

    struct edc_session *out = NULL;
    CHECK(edc_session_memory_extract_try(sched, sched->sessions, cfg, &out), "an extraction round started");
    CHECK(out == session, "reported the owning session");

    struct edc_task *task = find_extract_task(session);
    CHECK(task != NULL, "the L1 extract-summary task is in the system group");
    CHECK(task && task->memo_kind == EDC_MEMO_KIND_EXTRACT_SUMMARY, "memo_kind = EXTRACT_SUMMARY");
    CHECK(task && task->sink_group == group, "knows which group it is extracting");
    CHECK(task && task->memo_cut_task_id && task->memo_cut_task_id == edc_test_id("g1"), "cut lands on the member");
    /* The chained L2 (CURATE + cut) must exist to do the actual memory writes. */
    CHECK(find_extract_curate_task(session) != NULL, "the L2 curate task is chained");

    const char *material = task_input(task);
    CHECK(material != NULL, "the prompt is attached");
    CHECK(material && strstr(material, "## Group history to compress into knowledge candidates") != NULL,
          "L1 material header present (knowledge candidates)");
    CHECK(material && strstr(material, "hello world") != NULL, "the raw history is in the material");
    CHECK(material && strstr(material, "2000-01-01 00:00:00  user: hello world") != NULL,
          "each material line carries the message timestamp");
    CHECK(material && strstr(material, "memory last updated") != NULL,
          "the group memory's last-updated time is stated (its text is not included)");
    CHECK(material && strstr(material, "memory last updated unknown") == NULL,
          "the group memory time is a real time, not unknown");
    /* New design: the entry index is NOT in L1's material -- it belongs to L2's prompt,
     * and the old group memory text is never included. */
    CHECK(material && strstr(material, "## Existing session entries") == NULL, "L1 has no entry index");
    CHECK(material && strstr(material, "SECRET_REMEMBERED_FACT") == NULL,
          "the OLD group memory is NOT in the material");
    /* L2 carries the index. */
    struct edc_task *l2 = find_extract_curate_task(session);
    const char *l2mat = task_input(l2);
    CHECK(l2mat && strstr(l2mat, "## Existing session entries") != NULL, "L2 carries the entry index");
    CHECK(l2mat && strstr(l2mat, "Existing entry") != NULL, "L2's index has the existing row");

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
    struct edc_session *session = edc_session_new(cfg, edc_test_session_id("sess-cap"), "Cap");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, session);

    struct edc_task *leader = edc_task_new(edc_test_id("g1"), session->session_id, edc_test_id("g1"), "big", edc_channel_adapter);
    struct edc_task_group *group = edc_task_group_new(leader);
    edc_session_add_group(session, group);
    edc_task_await_exec(leader);
    edc_task_mark_completed(leader);
    leader->completion_seq = 1;
    idle_long_ago(group); /* idle long enough for the window; the material is what this test checks */

    /* One committed turn with two ~75k-token messages: together over the 131k cap,
     * each comfortably under it => round 1 takes the first, round 2 the second. */
    struct edc_turn *turn = edc_turn_new();
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
        edc_turn_add_message(turn, m);
    }
    sdsfree(a);
    sdsfree(b);
    edc_task_add_turn(leader, turn);

    /* Round 1: only the first message fits under the cap. */
    struct edc_session *out = NULL;
    CHECK(edc_session_memory_extract_try(sched, sched->sessions, cfg, &out), "round 1 started");
    struct edc_task *t1 = find_extract_task(session);       /* L1 summary */
    struct edc_task *c1 = find_extract_curate_task(session); /* L2 curate, carries cut */
    CHECK(t1 != NULL && c1 != NULL, "round 1 has both L1 summary and L2 curate");
    CHECK(t1 && t1->memo_cut_turn == 0 && t1->memo_cut_prefix == 1, "L1 cut = turn 0, first message only");
    CHECK(c1 && c1->memo_cut_prefix == 1, "L2 carries the same cut");
    const char *m1 = task_input(t1);
    CHECK(m1 && strstr(m1, "MARK_A") != NULL, "round 1 carries the first message");
    CHECK(m1 && strstr(m1, "MARK_B") == NULL, "round 1 does NOT carry the second (over the cap)");

    /* Landing happens on the L2 curate task, never on L1. And folded_prefix must stay
     * untouched -- the group's request context is not pruned by an extract. */
    uint32_t folded_before = turn->folded_prefix;
    CHECK(edc_session_memory_extract_land(c1), "round 1 landed (via L2)");
    CHECK(turn->memo_prefix == 1, "watermark advanced to 1");
    CHECK(turn->folded_prefix == folded_before, "folded_prefix untouched by extract landing");
    CHECK(edc_task_group_extractable_msg_count(group) == 1, "one message still pending");
    finish_task(t1);
    finish_task(c1);

    /* Round 2: the remainder. */
    out = NULL;
    CHECK(edc_session_memory_extract_try(sched, sched->sessions, cfg, &out), "round 2 started");
    struct edc_task *t2 = find_extract_task(session);
    struct edc_task *c2 = find_extract_curate_task(session);
    CHECK(t2 != NULL && t2 != t1, "round 2 is a fresh L1 task");
    CHECK(c2 != NULL && c2 != c1, "round 2 is a fresh L2 task");
    CHECK(t2 && t2->memo_cut_prefix == 2, "round 2's cut covers the whole turn");
    const char *m2 = task_input(t2);
    CHECK(m2 && strstr(m2, "MARK_B") != NULL, "round 2 carries the second message");
    CHECK(m2 && strstr(m2, "MARK_A") == NULL, "round 2 does NOT re-send the first (cursor)");

    CHECK(edc_session_memory_extract_land(c2), "round 2 landed (via L2)");
    CHECK(turn->memo_prefix == 2, "watermark at the end");
    CHECK(edc_task_group_extractable_msg_count(group) == 0, "nothing pending");
    finish_task(t2);
    finish_task(c2);

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
    struct edc_session *session = edc_session_new(cfg, edc_test_session_id("sess-clamp"), "Clamp");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, session);
    sched->send_llm = stub_send_llm;
    g_send_llm_calls = 0;

    struct edc_task_group *group = make_finished_user_group(session, "g1", "only work", OLD_TS, 2);
    struct edc_turn *turn = group->leader->turns.data[group->leader->turns.head];

    struct edc_task *fold = edc_group_fold_start(sched, session, group, NULL);
    CHECK(fold != NULL, "a fold started");
    /* The fold corpus is the task's own input; capture it before the reply commits the turn. */
    const char *fold_prompt = NULL;
    if (fold->active_turn && fold->active_turn->msg_count > 0)
    {
        cJSON *m = edc_turn_msg_at(fold->active_turn, 0);
        fold_prompt = cJSON_GetStringValue(cJSON_GetObjectItem(m, "content"));
    }
    CHECK(fold_prompt && strstr(fold_prompt, OLD_TS) != NULL, "the fold corpus carries each message's timestamp");
    edc_scheduler_schedule(sched);
    CHECK(g_send_llm_calls == 1, "the fold request was dispatched");

    struct edc_llm_actor_resp resp;
    memset(&resp, 0, sizeof(resp));
    resp.reason = BC_LLM_FINISH_REASON_STOP;
    resp.content = sdsnew("# memory\n- folded");
    void *fold_req = fold->current_request_id;
    struct edc_advance_result fr = edc_executor_advance_llm_resp(&sched->executor, fold, fold_req, &resp);
    edc_scheduler_apply_advance(sched, fold, fold_req, &fr);

    {
        sds landed = edc_state_store_group_memory_load(session->session_id, group->group_id, NULL, NULL, NULL);
        CHECK(landed != NULL, "the fold landed a memory (DB)");
        if (landed) sdsfree(landed);
    }
    CHECK(turn->folded_prefix > 0, "the fold advanced the folded watermark");
    CHECK(turn->memo_prefix >= turn->folded_prefix, "memo_prefix was clamped up to folded_prefix (folded => on L2)");
    CHECK(group->memory_created_at > 0 && group->memory_updated_at > 0, "the landing stamps the group memory times");
    CHECK(group->memory_covers_to > 0, "covers_to is the newest folded message's time");

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
    struct edc_session *session = edc_session_new(cfg, edc_test_session_id("sess-fail"), "Fail");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, session);

    struct edc_task_group *group = make_finished_user_group(session, "g1", "work", OLD_TS, 2);
    struct edc_turn *turn = group->leader->turns.data[group->leader->turns.head];

    /* A task whose cut does not resolve: the watermark must not move. */
    struct edc_task *task =
        edc_scheduler_spawn_system_task_as(sched, session, "extract session memory", EDC_SINK_SESSION_MEMORY);
    task->memo_kind = EDC_MEMO_KIND_EXTRACT;
    task->sink_group = group;
    task->memo_cut_task_id = edc_test_id("does-not-exist");
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
    struct edc_session *session = edc_session_new(cfg, edc_test_session_id("sess-ser"), "Ser");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, session);

    struct edc_task *leader = edc_task_new(edc_test_id("1"), session->session_id, edc_test_id("1"), "leader", edc_channel_adapter);
    struct edc_task_group *group = edc_task_group_new(leader);
    edc_session_add_group(session, group);
    give_turn(leader, "x", NULL, 3);
    struct edc_turn *turn = leader->turns.data[leader->turns.head];
    turn->folded_prefix = 1;
    turn->memo_prefix = 2;
    edc_task_mark_ready(leader);
    edc_task_mark_interrupted(leader);
    leader->memo_kind = EDC_MEMO_KIND_COMPACT;

    CHECK(edc_state_store_flush(session, sched), "flush returns ok");

    struct edc_session *s2 = edc_session_new(cfg, edc_test_session_id("sess-ser"), "Ser");
    struct edc_scheduler *sched2 = edc_scheduler_new(cfg);
    install_resolver(sched2, s2);
    CHECK(edc_state_store_load(s2, sched2), "round-trips through the store");
    struct edc_task *r = edc_session_find_task(s2, edc_test_id("1"));
    CHECK(r != NULL, "task restored");
    CHECK(r && r->memo_kind == EDC_MEMO_KIND_COMPACT, "memo_kind=COMPACT round-trips");
    CHECK(r && ((struct edc_turn *)r->turns.data[r->turns.head])->memo_prefix == 2, "turn memo_prefix round-trips");
    CHECK(r && ((struct edc_turn *)r->turns.data[r->turns.head])->folded_prefix == 1, "turn folded_prefix round-trips");
    edc_session_free(s2);
    edc_scheduler_free(sched2);

    /* The v7-archive case is gone with the format it described: there is no JSON snapshot to
     * hand-edit any more, and an old DATABASE is handled by edc_state_migrate.c, not by the
     * loader. What the old test checked here -- "memo_compact reads as COMPACT",
     * "a turn without memo_prefix normalizes it up to folded_prefix" -- is now a property of
     * that migration step. */

    edc_config_free(cfg);
}

static void test_material_keeps_tool_calls(void)
{
    TEST("material: an assistant tool call renders its name + arguments");

    struct edc_config *cfg = make_config();
    struct edc_session *session = edc_session_new(cfg, edc_test_session_id("sess-tc"), "TC");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, session);

    struct edc_task *leader = edc_task_new(edc_test_id("g1"), session->session_id, edc_test_id("g1"), "work", edc_channel_adapter);
    struct edc_task_group *group = edc_task_group_new(leader);
    edc_session_add_group(session, group);

    struct edc_turn *turn = edc_turn_new();
    edc_turn_add_user_message(turn, "list the dir");
    cJSON *args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "cmd", "ls -la");
    edc_turn_add_assistant_tool_call(turn, "call_1", "exec_shell", args);
    cJSON_Delete(args);
    edc_turn_add_tool_message(turn, "call_1", "exec_shell", "total 0");
    /* edc_turn_add_* stamps ts = now; force it old so the group is "idle". */
    for (int mi = 0; mi < turn->msg_count; mi++)
    {
        /* edc_turn_msg_at hands out a COPY: the edit has to be written back or the row
         * keeps "now" and the group never looks idle. */
        cJSON *m = edc_turn_msg_at(turn, mi);
        if (!m)
            continue;
        cJSON_ReplaceItemInObject(m, "ts", cJSON_CreateString(OLD_TS));
        edc_msg_append(turn->task_id, turn->turn_ord, mi, m, 0);
        cJSON_Delete(m);
    }
    edc_task_add_turn(leader, turn);
    edc_task_await_exec(leader);
    edc_task_mark_completed(leader);
    leader->completion_seq = 1;
    idle_long_ago(group);

    struct edc_session *out = NULL;
    CHECK(edc_session_memory_extract_try(sched, sched->sessions, cfg, &out), "extraction started");
    const char *material = task_input(find_extract_task(session));
    CHECK(material && strstr(material, "call exec_shell({\"cmd\":\"ls -la\"})") != NULL,
          "the tool call's name + arguments are in the material");
    CHECK(material && strstr(material, "tool: total 0") != NULL, "the tool result is still there");

    edc_session_free(session);
    edc_scheduler_free(sched);
    edc_config_free(cfg);
}

/* ══════════════════════════════════════════════════════════════
 *  Test 8: the extract-scan background service drives extraction
 *  ══════════════════════════════════════════════════════════════ */

static void test_extract_scan_service(void)
{
    TEST("service: extract-scan starts an extraction for an idle group on its tick");

    struct edc_config *cfg = make_config();
    struct edc_session *session = edc_session_new(cfg, edc_test_session_id("sess-scan"), "Scan");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, session);

    make_finished_user_group(session, "g1", "idle work", OLD_TS, 1);

    edc_scheduler_services_start(sched);
    CHECK(edc_service_find(sched, "extract-scan") != NULL, "extract-scan is registered");
    CHECK(find_extract_task(session) == NULL, "nothing started yet");

    edc_services_pump(sched, 1000); /* before the first interval elapses */
    CHECK(find_extract_task(session) == NULL, "not due before the interval");
    edc_services_pump(sched, 60000); /* the service interval is 60s */
    CHECK(find_extract_task(session) != NULL, "the scan started an extraction");

    edc_session_free(session);
    edc_scheduler_free(sched);
    edc_config_free(cfg);
}

/* ══════════════════════════════════════════════════════════════
 *  Test 9: the idle clock starts when the group BECOMES idle
 *  ══════════════════════════════════════════════════════════════ */

static void test_idle_clock_starts_when_the_group_goes_idle(void)
{
    TEST("idle: the window runs from the last member going terminal, not from the newest message");

    struct edc_config *cfg = make_config();
    struct edc_session *session = edc_session_new(cfg, edc_test_session_id("sess-clock"), "Clock");
    time_t now = time(NULL);

    /* Ancient messages, member finished just now: the group became idle NOW, so the
     * window has not elapsed. The old message clock would have said "silent for 25
     * years" and started a round immediately. */
    struct edc_task_group *group = make_finished_user_group(session, "g1", "ancient work", OLD_TS, 2);
    group->idle_since = now; /* make_finished_user_group backdates; this test is about the clock */
    CHECK(edc_task_group_extractable_msg_count(group) == 2, "there IS material waiting");
    CHECK(!edc_session_memory_extract_due(group, cfg, now), "ancient messages, freshly idle => NOT due");
    CHECK(!edc_session_memory_extract_due(group, cfg, now + EDC_MEMO_EXTRACT_IDLE_SECS - 1), "one second early => NOT due");
    CHECK(edc_session_memory_extract_due(group, cfg, now + EDC_MEMO_EXTRACT_IDLE_SECS), "the window elapsed => due");

    /* The case the message clock got wrong: one member runs for the whole window with no
     * messages coming in. The group is BUSY the entire time, so nothing is extracted no
     * matter how old the newest message is. */
    struct edc_task *long_runner =
        edc_task_new(edc_test_id("2"), session->session_id, edc_test_id("g1"), "long tool call", edc_channel_adapter);
    edc_task_group_add_task(group, long_runner);
    edc_task_await_exec(long_runner);
    CHECK(group->busy_members == 1, "one live member => busy");
    CHECK(group->idle_since == 0, "busy => the idle clock is not running");
    const time_t long_after = now + EDC_MEMO_EXTRACT_IDLE_SECS * 10;
    CHECK(!edc_session_memory_extract_due(group, cfg, long_after), "a task that has been running for 10 windows => NOT due");

    /* Now it stops -- at |long_after| the group only BECOMES idle. The clock starts here,
     * so the window has to elapse again from this moment. */
    edc_task_mark_cancelled(long_runner);
    CHECK(group->busy_members == 0, "cancelled => nobody running");
    CHECK(group->idle_since != 0, "the stop stamped the idle clock");
    CHECK(!edc_session_memory_extract_due(group, cfg, time(NULL)), "just stopped => NOT due (the user may still be there)");

    idle_long_ago(group); /* ... and only a full window later does it become due */
    CHECK(edc_session_memory_extract_due(group, cfg, time(NULL)), "a full window after the stop => due");

    /* Nobody has observed this group idle yet (a mid-load group): refuse rather than
     * guess. The next scan's recount stamps it. */
    group->busy_members = 0;
    group->idle_since = 0;
    CHECK(!edc_session_memory_extract_due(group, cfg, now), "no idle stamp => NOT due (do not guess)");

    /* Consistency of the two fields, driven only through the real transitions. */
    struct edc_task *again =
        edc_task_new(edc_test_id("3"), session->session_id, edc_test_id("g1"), "another", edc_channel_adapter);
    edc_task_group_add_task(group, again);
    CHECK(group->busy_members == 1 && group->idle_since == 0, "attach => busy, clock cleared");
    edc_task_await_exec(again);
    CHECK(group->busy_members == 1, "a step change inside a running member keeps it busy");
    edc_task_mark_completed(again);
    CHECK(group->busy_members == 0, "the last member finishing empties the count");
    CHECK(group->idle_since != 0, "and stamps the clock");
    edc_task_group_remove_task(group, again);
    CHECK(group->busy_members == 0, "detaching a finished member keeps the count at zero");

    edc_session_free(session);
    edc_config_free(cfg);
}

/* ══════════════════════════════════════════════════════════════
 *  Test 7: the recency rule is in every memory prompt
 *  ══════════════════════════════════════════════════════════════ */

static void test_prompts_have_recency_rule(void)
{
    TEST("prompts: the recency rule is in curate / extract / compact");

    CHECK(strstr(edc_session_memory_system_prompt(), "TIME IS A HINT") != NULL, "curator prompt carries the rule");
    CHECK(strstr(edc_session_memory_extract_prompt(), "TIME IS A HINT") != NULL, "extract prompt carries the rule");
    CHECK(strstr(edc_session_memory_compact_prompt(), "updated LATER") != NULL,
          "compact prompt prefers the newer on merge");
}

/* ══════════════════════════════════════════════════════════════
 *  New two-stage extract: the L1 summary must never prune the group.
 * ══════════════════════════════════════════════════════════════ */

static void test_two_stage_never_prunes_group(void)
{
    TEST("two-stage: L1 summary touches no watermark / history; only L2 landing advances memo_prefix");

    struct edc_config *cfg = make_config();
    struct edc_session *session = edc_session_new(cfg, edc_test_session_id("sess-2stage"), "TwoStage");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, session);

    struct edc_task_group *group = make_finished_user_group(session, "g1", "durable fact", OLD_TS, 1);
    struct edc_turn *turn = group->leader->turns.data[group->leader->turns.head];
    int history_before = edc_task_group_history_count(group);

    struct edc_session *out = NULL;
    CHECK(edc_session_memory_extract_try(sched, sched->sessions, cfg, &out), "two-stage round started");
    struct edc_task *l1 = find_extract_task(session);
    struct edc_task *l2 = find_extract_curate_task(session);
    CHECK(l1 && l2, "both stages exist");
    CHECK(l1 && l1->memo_kind == EDC_MEMO_KIND_EXTRACT_SUMMARY, "L1 is EXTRACT_SUMMARY");

    /* L1 finishing alone must NOT advance memo_prefix, folded_prefix or prune history. */
    uint32_t folded_before = turn->folded_prefix;
    uint32_t memo_before = turn->memo_prefix;
    finish_task(l1);
    CHECK(turn->memo_prefix == memo_before, "L1 done leaves memo_prefix alone");
    CHECK(turn->folded_prefix == folded_before, "L1 done leaves folded_prefix alone");
    CHECK(edc_task_group_history_count(group) == history_before, "L1 done does not prune group history");

    /* Only the L2 landing advances memo_prefix -- and still never folded_prefix/history. */
    CHECK(edc_session_memory_extract_land(l2), "L2 landing advances");
    CHECK(turn->memo_prefix > memo_before, "L2 landing advances memo_prefix");
    CHECK(turn->folded_prefix == folded_before, "L2 landing leaves folded_prefix alone");
    CHECK(edc_task_group_history_count(group) == history_before, "L2 landing does not prune group history");

    edc_session_free(session);
    edc_scheduler_free(sched);
    edc_config_free(cfg);
}

static void test_extract_budget_is_independent(void)
{
    TEST("extract budget: extract failure shrinks only extract budget, not fold; progress resets only extract");

    struct edc_config *cfg = make_config();
    struct edc_session *session = edc_session_new(cfg, edc_test_session_id("sess-budget"), "Budget");
    struct edc_task_group *group = make_finished_user_group(session, "g1", "work", OLD_TS, 1);
    /* Give the fold budget a non-default value so we can prove extract leaves it alone. */
    group->fold_corpus_budget = 32768;

    edc_session_memory_extract_note_failure(group);
    CHECK(group->extract_summary_budget > 0, "extract failure shrinks the extract budget");
    CHECK(group->fold_corpus_budget == 32768, "extract failure leaves the fold budget untouched");

    edc_session_memory_extract_note_progress(group);
    CHECK(group->extract_summary_budget == 0, "extract progress resets the extract budget to full");
    CHECK(group->fold_corpus_budget == 32768, "extract progress leaves the fold budget untouched");

    edc_session_free(session);
    edc_config_free(cfg);
}

/* Direct coverage of the extract budget helpers (public API, every branch). */
static void test_extract_budget_helpers(void)
{
    TEST("extract budget helpers: cap clamp, shrink halving/floor, NULL guards");

    /* NULL guards. */
    CHECK(edc_group_extract_corpus_cap(NULL) == EDC_FOLD_MAX_CORPUS_TOKENS,
          "corpus_cap(NULL) = full cap");
    edc_group_extract_shrink_budget(NULL); /* must not crash */

    struct edc_config *cfg = make_config();
    struct edc_session *session = edc_session_new(cfg, edc_test_session_id("sess-budget-helpers"), "BudgetHelpers");
    struct edc_task_group *group = make_finished_user_group(session, "g1", "work", OLD_TS, 1);

    /* Default (=0) means full cap. */
    group->extract_summary_budget = 0;
    CHECK(edc_group_extract_corpus_cap(group) == EDC_FOLD_MAX_CORPUS_TOKENS,
          "budget 0 -> full cap");
    /* Clamp: below floor and above max. */
    group->extract_summary_budget = EDC_FOLD_MIN_CORPUS_TOKENS / 2;
    CHECK(edc_group_extract_corpus_cap(group) == EDC_FOLD_MIN_CORPUS_TOKENS,
          "budget below floor clamps to floor");
    group->extract_summary_budget = EDC_FOLD_MAX_CORPUS_TOKENS * 2;
    CHECK(edc_group_extract_corpus_cap(group) == EDC_FOLD_MAX_CORPUS_TOKENS,
          "budget above max clamps to max");

    /* Shrink halves, and floors at EDC_FOLD_MIN_CORPUS_TOKENS. */
    group->extract_summary_budget = 65536;
    edc_group_extract_shrink_budget(group);
    CHECK(group->extract_summary_budget == 32768, "shrink halves 65536 -> 32768");
    edc_group_extract_shrink_budget(group);
    CHECK(group->extract_summary_budget == EDC_FOLD_MIN_CORPUS_TOKENS,
          "shrink floors at minimum (32768/2=16384)");
    edc_group_extract_shrink_budget(group);
    CHECK(group->extract_summary_budget == EDC_FOLD_MIN_CORPUS_TOKENS,
          "shrink stays at floor (does not underflow)");

    /* Shrink from default also halves from the full cap. */
    group->extract_summary_budget = 0;
    edc_group_extract_shrink_budget(group);
    CHECK(group->extract_summary_budget == EDC_FOLD_MAX_CORPUS_TOKENS / 2,
          "shrink from default halves the full cap");

    edc_session_free(session);
    edc_config_free(cfg);
}

/* ══════════════════════════════════════════════════════════════
 *  main
 *  ══════════════════════════════════════════════════════════════ */

int main(void)
{
    edc_test_workspace(); /* open the disposable store before anything writes */
    test_due_predicate();
    test_material_excludes_group_memory();
    test_cap_multi_round_and_idempotence();
    test_fold_landing_clamps_memo_prefix();
    test_failure_backoff_and_bad_cut();
    test_serialization_tristate_and_v7();
    test_extract_scan_service();
    test_material_keeps_tool_calls();
    test_idle_clock_starts_when_the_group_goes_idle();
    test_prompts_have_recency_rule();
    test_two_stage_never_prunes_group();
    test_extract_budget_is_independent();
    test_extract_budget_helpers();

    log_info("  ====== %d passed, %d failed ======", test_passed, test_failed);
    return test_failed ? 1 : 0;
}
