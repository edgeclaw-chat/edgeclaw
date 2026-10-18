#include "edc_task.h"
#include "edc_compat.h"
#include "edc_context.h"
#include "edc_session.h"
#include "edc_task_group.h"
#include "utlist_fix.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* The old state-name mapping is retired (R3b): the display name has one source,
 * edc_task_state_str, derived from (step, life). */

/* ── State-transition validation ───────────────────────────── */

/* ── step / life: names, parsing, mapping ──────────────────── */

const char *edc_task_step_name(edc_task_step step)
{
    switch (step)
    {
    case EDC_TASKSTEP_READY:
        return "READY";
    case EDC_TASKSTEP_AWAIT_DEPS:
        return "AWAIT_DEPS";
    case EDC_TASKSTEP_AWAIT_EXEC:
        return "AWAIT_EXEC";
    case EDC_TASKSTEP_AWAIT_TOOLS:
        return "AWAIT_TOOLS";
    case EDC_TASKSTEP_AWAIT_RESOURCE:
        return "AWAIT_RESOURCE";
    case EDC_TASKSTEP_DONE:
        return "DONE";
    }
    return "READY";
}

const char *edc_task_life_name(edc_task_life life)
{
    switch (life)
    {
    case EDC_LIFE_ALIVE:
        return "ALIVE";
    case EDC_LIFE_INTERRUPTED:
        return "INTERRUPTED";
    case EDC_LIFE_SUSPENDED:
        return "SUSPENDED";
    case EDC_LIFE_COMPLETED:
        return "COMPLETED";
    case EDC_LIFE_CANCELLED:
        return "CANCELLED";
    case EDC_LIFE_ERROR:
        return "ERROR";
    }
    return "ALIVE";
}

edc_task_step edc_task_classify_step(const char *name)
{
    if (!name)
        return EDC_TASKSTEP_READY;
    if (strcmp(name, "AWAIT_DEPS") == 0)
        return EDC_TASKSTEP_AWAIT_DEPS;
    /* Accept the legacy "AWAIT_INPUT": older v5 files and the v4->v5 migration
     * wrote it for a parked task, and it means READY. */
    if (strcmp(name, "AWAIT_INPUT") == 0)
        return EDC_TASKSTEP_READY;
    if (strcmp(name, "AWAIT_EXEC") == 0)
        return EDC_TASKSTEP_AWAIT_EXEC;
    if (strcmp(name, "AWAIT_TOOLS") == 0)
        return EDC_TASKSTEP_AWAIT_TOOLS;
    if (strcmp(name, "AWAIT_RESOURCE") == 0)
        return EDC_TASKSTEP_AWAIT_RESOURCE;
    if (strcmp(name, "DONE") == 0)
        return EDC_TASKSTEP_DONE;
    return EDC_TASKSTEP_READY;
}

edc_task_life edc_task_classify_life(const char *name)
{
    if (!name)
        return EDC_LIFE_ALIVE;
    if (strcmp(name, "INTERRUPTED") == 0)
        return EDC_LIFE_INTERRUPTED;
    /* Legacy: only the old load-time collapse ever wrote "SUSPENDED", and what it meant
     * was exactly what INTERRUPTED now says (the user pause had no producer even then).
     * When a real pause lands, this mapping has to be revisited. */
    if (strcmp(name, "SUSPENDED") == 0)
        return EDC_LIFE_INTERRUPTED;
    if (strcmp(name, "COMPLETED") == 0)
        return EDC_LIFE_COMPLETED;
    if (strcmp(name, "CANCELLED") == 0)
        return EDC_LIFE_CANCELLED;
    if (strcmp(name, "ERROR") == 0)
        return EDC_LIFE_ERROR;
    return EDC_LIFE_ALIVE;
}

/* sink name + classify, matching step/life. Not persisted today, so _name serves
 * logs and _classify has no caller yet; kept as one family. */

const char *edc_task_sink_name(edc_sink sink)
{
    switch (sink)
    {
    case EDC_SINK_CHANNEL:
        return "CHANNEL";
    case EDC_SINK_GROUP_MEMORY:
        return "GROUP_MEMORY";
    case EDC_SINK_SESSION_MEMORY:
        return "SESSION_MEMORY";
    }
    return "CHANNEL";
}

edc_sink edc_task_classify_sink(const char *name)
{
    if (!name)
        return EDC_SINK_CHANNEL;
    if (strcmp(name, "GROUP_MEMORY") == 0)
        return EDC_SINK_GROUP_MEMORY;
    if (strcmp(name, "SESSION_MEMORY") == 0)
        return EDC_SINK_SESSION_MEMORY;
    return EDC_SINK_CHANNEL;
}

const char *edc_task_memo_kind_name(edc_memo_kind kind)
{
    switch (kind)
    {
    case EDC_MEMO_KIND_COMPACT:
        return "COMPACT";
    case EDC_MEMO_KIND_EXTRACT:
        return "EXTRACT";
    case EDC_MEMO_KIND_CURATE:
    default:
        return "CURATE";
    }
}

edc_memo_kind edc_task_classify_memo_kind(const char *name)
{
    if (!name)
        return EDC_MEMO_KIND_CURATE;
    if (strcmp(name, "COMPACT") == 0)
        return EDC_MEMO_KIND_COMPACT;
    if (strcmp(name, "EXTRACT") == 0)
        return EDC_MEMO_KIND_EXTRACT;
    return EDC_MEMO_KIND_CURATE;
}

/* The display name (snapshot / logs / command text), derived straight from
 * (step, life) with life speaking first, matching the old order. WAITING_INPUT is
 * never returned. */
const char *edc_task_state_str(const struct edc_task *task)
{
    if (!task)
        return "IDLE";
    switch (task->life)
    {
    case EDC_LIFE_INTERRUPTED:
        return "INTERRUPTED";
    case EDC_LIFE_SUSPENDED:
        return "SUSPENDED";
    case EDC_LIFE_COMPLETED:
        return "COMPLETED";
    case EDC_LIFE_CANCELLED:
        return "CANCELLED";
    case EDC_LIFE_ERROR:
        return "ERROR";
    case EDC_LIFE_ALIVE:
        break;
    }
    switch (task->step)
    {
    case EDC_TASKSTEP_READY:
        return "IDLE";
    case EDC_TASKSTEP_AWAIT_DEPS:
        return "WAITING_DEPS";
    case EDC_TASKSTEP_AWAIT_EXEC:
        return "LLM_PROCESSING";
    case EDC_TASKSTEP_AWAIT_TOOLS:
        return "TOOL_CALLING";
    case EDC_TASKSTEP_AWAIT_RESOURCE:
        return "WAITING_DEPS"; /* no counterpart in the old enum */
    case EDC_TASKSTEP_DONE:
        break;
    }
    /* Fallback: the malformed DONE-but-ALIVE combination. No path produces it. */
    return "IDLE";
}

static bool edc_task_is_valid_transition(edc_task_step from_step, edc_task_life from_life, edc_task_step to_step,
                                         edc_task_life to_life);

/* A transition states both "which step" and "alive?" at once; the state machine
 * validates (step, life), so it never loses the phase. */
/* Low-level primitives: state the target directly. Used by the scheduler's
 * unified entry and the executor's port. */
bool edc_task_move(struct edc_task *task, edc_task_step step, edc_task_life life)
{
    if (!task)
        return false;
    if (!edc_task_is_valid_transition(task->step, task->life, step, life))
        return false;
    task->step = step;
    task->life = life;
    return true;
}

/* Intent-named write points: the caller says what it wants rather than assembling
 * a target state. Returns false when the transition is rejected. */
bool edc_task_await_exec(struct edc_task *task)
{
    return edc_task_move(task, EDC_TASKSTEP_AWAIT_EXEC, EDC_LIFE_ALIVE);
}

bool edc_task_await_tools(struct edc_task *task)
{
    return edc_task_move(task, EDC_TASKSTEP_AWAIT_TOOLS, EDC_LIFE_ALIVE);
}

bool edc_task_mark_ready(struct edc_task *task)
{
    return edc_task_move(task, EDC_TASKSTEP_READY, EDC_LIFE_ALIVE);
}

bool edc_task_mark_waiting_deps(struct edc_task *task)
{
    return edc_task_move(task, EDC_TASKSTEP_AWAIT_DEPS, EDC_LIFE_ALIVE);
}

bool edc_task_mark_completed(struct edc_task *task)
{
    return edc_task_move(task, EDC_TASKSTEP_DONE, EDC_LIFE_COMPLETED);
}

/* Failure is BROADCAST to dependents (defined in the dependency section below). */
static void task_notify_dependents_failed(struct edc_task *task);

/* Failure = set ERROR and broadcast to every dependent, so "a task failed"
 * propagates no matter who initiated it. Symmetric with send_signal: neither does
 * host bookkeeping, they only record the moved tasks. */
bool edc_task_mark_failed(struct edc_task *task)
{
    if (!task)
        return false;
    /* A failed self-transition = it was not failing (terminal/illegal phase), so nothing is broadcast -- idempotent. */
    if (!edc_task_move(task, EDC_TASKSTEP_DONE, EDC_LIFE_ERROR))
        return false;
    task_notify_dependents_failed(task);
    return true;
}

bool edc_task_mark_cancelled(struct edc_task *task)
{
    return edc_task_move(task, EDC_TASKSTEP_DONE, EDC_LIFE_CANCELLED);
}

/* Stopping speaks ONLY about life: it stops at whatever step it was on -- exactly what
 * the step/life split preserves (the old TASK_SUSPENDED meant the same, it just could not
 * say so). */
bool edc_task_mark_suspended(struct edc_task *task)
{
    return task && edc_task_move(task, task->step, EDC_LIFE_SUSPENDED);
}

/* An interruption speaks about life only (keeps the step) AND is broadcast like a
 * failure: the process is gone, so its dependents are in the same situation as if this
 * task had errored -- a JOIN dependent is cancelled (it can come back once this one is
 * resumed), a SEQUENCE edge is voided and lets the task behind run. */
bool edc_task_mark_interrupted(struct edc_task *task)
{
    if (!task)
        return false;
    /* A self-transition = it was already interrupted (or the row is illegal), so
     * nothing is broadcast -- idempotent. */
    if (!edc_task_move(task, task->step, EDC_LIFE_INTERRUPTED))
        return false;
    task_notify_dependents_failed(task);
    return true;
}

/* Transition table, keyed by (step, life). */
static bool edc_task_is_valid_transition(edc_task_step from_step, edc_task_life from_life, edc_task_step to_step,
                                         edc_task_life to_life)
{
    /* No transition within the same (step, life). */
    if (from_step == to_step && from_life == to_life)
        return false;

    switch (from_life)
    {
    case EDC_LIFE_COMPLETED:
        return false; /* terminal */
    case EDC_LIFE_CANCELLED:
        /* Resume: cancel only stops the task, so CANCELLED shares the target group
         * with SUSPENDED / ERROR. */
        return (to_life == EDC_LIFE_ALIVE &&
                (to_step == EDC_TASKSTEP_READY || to_step == EDC_TASKSTEP_AWAIT_DEPS ||
                 to_step == EDC_TASKSTEP_AWAIT_EXEC || to_step == EDC_TASKSTEP_AWAIT_TOOLS)) ||
               (to_step == EDC_TASKSTEP_DONE && to_life == EDC_LIFE_CANCELLED);
    case EDC_LIFE_SUSPENDED:
        /* resume re-decides: back to dep-wait / ready / re-request LLM / continue the tool chain, or cancel */
    case EDC_LIFE_INTERRUPTED:
        /* Same as a user pause: nothing was decided, so resume re-decides. */
    case EDC_LIFE_ERROR:
        /* continue the failed round (same target set as SUSPENDED), or cancel. */
        return (to_life == EDC_LIFE_ALIVE &&
                (to_step == EDC_TASKSTEP_READY || to_step == EDC_TASKSTEP_AWAIT_DEPS ||
                 to_step == EDC_TASKSTEP_AWAIT_EXEC || to_step == EDC_TASKSTEP_AWAIT_TOOLS)) ||
               (to_step == EDC_TASKSTEP_DONE && to_life == EDC_LIFE_CANCELLED);
    case EDC_LIFE_ALIVE:
        break;
    }

    /* An interruption keeps the step it was on and may land on any of them: unlike a
     * user pause (restricted to the rows below), the process went down mid-request. */
    if (to_life == EDC_LIFE_INTERRUPTED)
        return to_step == from_step;

    switch (from_step)
    {
    /* READY = parked with input, waiting to be dispatched: dispatch / hit a
     * dependency / cancel / suspend / failure. Failure is allowed so a drive that
     * cannot start any I/O fails instead of being rescanned forever. */
    case EDC_TASKSTEP_READY:
        return (to_life == EDC_LIFE_ALIVE &&
                (to_step == EDC_TASKSTEP_AWAIT_EXEC || to_step == EDC_TASKSTEP_AWAIT_DEPS)) ||
               (to_step == EDC_TASKSTEP_DONE && (to_life == EDC_LIFE_CANCELLED || to_life == EDC_LIFE_ERROR)) ||
               (to_step == EDC_TASKSTEP_READY && to_life == EDC_LIFE_SUSPENDED);
    case EDC_TASKSTEP_AWAIT_DEPS:
    case EDC_TASKSTEP_AWAIT_RESOURCE: /* no such step in the old vocabulary (mirrors to WAITING_DEPS): same row */
        /* deps satisfied -> back to ready; or cancel / fail */
        return (to_step == EDC_TASKSTEP_READY && to_life == EDC_LIFE_ALIVE) ||
               (to_step == EDC_TASKSTEP_DONE && (to_life == EDC_LIFE_CANCELLED || to_life == EDC_LIFE_ERROR));
    case EDC_TASKSTEP_AWAIT_EXEC:
        /* AWAIT_DEPS: a fold in flight holds the group; the executor parks a task
         * that still owes a dependency instead of sending the next request. */
        return (to_life == EDC_LIFE_ALIVE &&
                (to_step == EDC_TASKSTEP_AWAIT_TOOLS || to_step == EDC_TASKSTEP_AWAIT_DEPS)) ||
               (to_step == EDC_TASKSTEP_DONE &&
                (to_life == EDC_LIFE_COMPLETED || to_life == EDC_LIFE_CANCELLED || to_life == EDC_LIFE_ERROR));
    case EDC_TASKSTEP_AWAIT_TOOLS:
        return (to_life == EDC_LIFE_ALIVE &&
                (to_step == EDC_TASKSTEP_AWAIT_EXEC || to_step == EDC_TASKSTEP_AWAIT_DEPS)) ||
               (to_step == EDC_TASKSTEP_DONE &&
                (to_life == EDC_LIFE_COMPLETED || to_life == EDC_LIFE_CANCELLED || to_life == EDC_LIFE_ERROR)) ||
               (to_step == EDC_TASKSTEP_AWAIT_TOOLS && to_life == EDC_LIFE_SUSPENDED); /* suspend mid-run */
    case EDC_TASKSTEP_DONE:
        /* Malformed: DONE yet ALIVE (display name folds to IDLE, see edc_task_state_str's
         * fallback). No path produces it; reuse the READY row to avoid behaviour
         * outside default. */
        return (to_life == EDC_LIFE_ALIVE &&
                (to_step == EDC_TASKSTEP_AWAIT_EXEC || to_step == EDC_TASKSTEP_AWAIT_DEPS)) ||
               (to_step == EDC_TASKSTEP_DONE && to_life == EDC_LIFE_CANCELLED) ||
               (to_step == EDC_TASKSTEP_DONE && to_life == EDC_LIFE_SUSPENDED);
    }
    return false;
}

/* Write entries that bypass the state machine (initial state, fixtures): no
 * validation, straight to (step, life). */
void edc_task_force_move(struct edc_task *task, edc_task_step step, edc_task_life life)
{
    if (!task)
        return;
    task->step = step;
    task->life = life;
}

/* ── Lifetime ──────────────────────────────────────────────── */

struct edc_task *edc_task_new(const char *task_id, const char *session_id, const char *group_id, const char *desc,
                              edc_channel_type channel_type)
{
    struct edc_task *task = g_new0(struct edc_task, 1);
    task->task_id = strdup(task_id);
    task->session_id = strdup(session_id);
    task->group_id = strdup(group_id);
    task->description = strdup(desc ? desc : "");
    /* A new task is "parked with input, waiting to be dispatched" -- there is no
     * "waiting for user input" phase, so it starts at (READY, ALIVE) (the old
     * vocabulary spelled TASK_IDLE/TASK_WAITING_INPUT on two rows). */
    edc_task_force_move(task, EDC_TASKSTEP_READY, EDC_LIFE_ALIVE);
    /* The output axis defaults to a user task (output goes to the user); written
     * explicitly so correctness does not depend on enum order. */
    task->sink = EDC_SINK_CHANNEL;
    task->is_group_leader = 0;
    task->channel_type = channel_type;

    edc_queue_init(&task->turns);

    return task;
}

struct edc_task *edc_task_fork(struct edc_task *parent, const char *task_id, const char *desc,
                               uint32_t fork_point_completion_seq, edc_channel_type channel_type)
{
    if (!parent)
        return NULL;

    char auto_id[64];
    if (!task_id)
    {
        static int fork_seq = 0;
        snprintf(auto_id, sizeof(auto_id), "ft%d", ++fork_seq);
        task_id = auto_id;
    }

    struct edc_task *child = edc_task_new(task_id, parent->session_id, parent->group_id, desc, channel_type);
    child->forked_from = parent;
    child->fork_point_completion_seq = fork_point_completion_seq;
    /* Like edc_task_new, a child lands at READY (= IDLE's mirror); it no longer forces WAITING_INPUT. */
    return child;
}

/* ── fork snapshot ────────────────────────────────────────── */

struct edc_turn *edc_task_snapshot_turns(struct edc_task *task, struct edc_session *session,
                                         const char *snapshot_task_id)
{
    (void)session; /* the turn holds its own messages now */
    if (!task)
        return NULL;

    struct edc_turn *snap = edc_turn_new();
    if (snapshot_task_id)
        snap->task_id = strdup(snapshot_task_id);

    for (size_t qi = task->turns.head; qi < task->turns.tail; qi++)
    {
        struct edc_turn *turn = task->turns.data[qi];
        if (!turn)
            continue;

        for (size_t mj = turn->messages.head; mj < turn->messages.tail; mj++)
            edc_queue_push(&snap->messages, cJSON_Duplicate((cJSON *)turn->messages.data[mj], 1));
    }

    if (edc_queue_length(&snap->messages) == 0)
    {
        edc_turn_free(snap);
        return NULL;
    }
    return snap;
}

static void edc_task_free_turns(struct edc_task *task)
{
    struct edc_turn *turn;
    while ((turn = edc_queue_pop(&task->turns)) != NULL)
    {
        edc_turn_free(turn);
    }

    if (task->active_turn)
    {
        edc_turn_free(task->active_turn);
        task->active_turn = NULL;
    }

    edc_queue_destroy(&task->turns);
}

static void edc_task_free_deps(struct edc_task *task)
{
    struct edc_task_dep *dep, *tmp;

    LL_FOREACH_SAFE(task->deps, dep, tmp)
    {
        if (dep->source)
        {
            struct edc_task_dep *sd, *sd_tmp;
            LL_FOREACH_SAFE(dep->source->dependents, sd, sd_tmp)
            {
                if (sd->target == task)
                {
                    LL_DELETE(dep->source->dependents, sd);
                    free(sd);
                }
            }
        }
        LL_DELETE(task->deps, dep);
        free(dep);
    }

    LL_FOREACH_SAFE(task->dependents, dep, tmp)
    {
        if (dep->target)
        {
            struct edc_task_dep *td, *td_tmp;
            LL_FOREACH_SAFE(dep->target->deps, td, td_tmp)
            {
                if (td->source == task)
                {
                    LL_DELETE(dep->target->deps, td);
                    free(td);
                }
            }
            dep->target->deps_total--;
        }
        LL_DELETE(task->dependents, dep);
        free(dep);
    }
}

void edc_task_free(struct edc_task *task)
{
    if (!task)
        return;

    edc_task_free_turns(task);
    edc_task_free_deps(task);
    edc_task_tool_calls_clear(task);
    /* The side-effect move list not taken by the host (in-memory only): cb = NULL
     * just clears it. The task is being freed, so the list is meaningless -- the debt
     * only mattered to a still-live host. */
    edc_task_drain_moves(task, NULL, NULL);

    free(task->task_id);
    free(task->group_id);
    free(task->session_id);
    free(task->description);

    if (task->raw_input)
        sdsfree(task->raw_input);

    if (task->output)
        sdsfree(task->output);
    free(task->fold_cut_task_id);
    free(task->memo_cut_task_id);

    if (task->redirect_stdout)
        sdsfree(task->redirect_stdout);
    if (task->redirect_stderr)
        sdsfree(task->redirect_stderr);

    if (task->todos)
        cJSON_Delete(task->todos);

    free(task);
}

/* ── Turn ──────────────────────────────────────────────────── */

void edc_task_add_turn(struct edc_task *task, struct edc_turn *turn)
{
    if (!task || !turn)
        return;
    edc_queue_push(&task->turns, turn);
}

void edc_task_commit_active_turn(struct edc_task *task)
{
    if (!task || !task->active_turn)
        return;
    /* Committing a turn = every tool this round is done (records removed on response); clear the ledger */
    edc_task_tool_calls_clear(task);
    edc_queue_push(&task->turns, task->active_turn);
    task->active_turn = NULL;
}

size_t edc_task_message_count(const struct edc_task *task)
{
    if (!task)
        return 0;
    size_t n = 0;
    for (size_t i = task->turns.head; i < task->turns.tail; i++)
    {
        struct edc_turn *turn = (struct edc_turn *)task->turns.data[i];
        if (turn)
            n += edc_queue_length(&turn->messages);
    }
    if (task->active_turn)
        n += edc_queue_length(&task->active_turn->messages);
    return n;
}

/* Append a role=="user" message's content to *out (joined with '\n'; creates *out
 * when NULL). */
static void append_user_msg(const cJSON *msg, sds *out)
{
    if (!msg || !out)
        return;
    const char *role = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "role"));
    if (!role || strcmp(role, "user") != 0)
        return;
    const char *content = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "content"));
    if (!content)
        return;
    *out = *out ? sdscatprintf(*out, "\n%s", content) : sdsnew(content);
}

/* Collect a turn's user inputs (joined with '\n'). The turn holds its own messages. */
static void collect_user_msgs(const struct edc_turn *turn, sds *out)
{
    if (!turn || !out)
        return;
    for (size_t j = turn->messages.head; j < turn->messages.tail; j++)
        append_user_msg((const cJSON *)turn->messages.data[j], out);
}

sds edc_task_user_input(const struct edc_task *task, const cJSON *session_messages)
{
    (void)session_messages; /* turns hold messages now; no session message array needed */
    if (!task)
        return NULL;
    sds out = NULL;
    for (size_t i = task->turns.head; i < task->turns.tail; i++)
        collect_user_msgs((const struct edc_turn *)task->turns.data[i], &out);
    collect_user_msgs(task->active_turn, &out);
    return out;
}

/* ── Todo sync throttle ────────────────────────────────────── */

void edc_task_note_todo_sync(struct edc_task *task)
{
    if (!task)
        return;
    task->todo_sync_gap = 0;
}

bool edc_task_llm_request_poke(struct edc_task *task)
{
    if (!task)
        return false;
    task->todo_sync_gap++;
    if (task->todo_sync_gap >= EDC_TODO_SYNC_REMIND_GAP)
    {
        /* At the threshold: this request appends the reminder once, then resets the
         * count -- avoids repeating it every request (until the model calls todowrite
         * or the gap grows again). */
        task->todo_sync_gap = 0;
        return true;
    }
    return false;
}

/* ── Token accounting ──────────────────────────────────────── */

void edc_task_add_usage(struct edc_task *task, long long prompt_tokens, long long completion_tokens,
                        long long total_tokens, long long cached_tokens)
{
    if (!task)
        return;

    /* All zero = the provider gave no usage (or an error response); write nothing, keep the counters clean */
    if (prompt_tokens <= 0 && completion_tokens <= 0 && total_tokens <= 0 && cached_tokens <= 0)
        return;

    if (prompt_tokens < 0)
        prompt_tokens = 0;
    if (completion_tokens < 0)
        completion_tokens = 0;
    if (cached_tokens < 0)
        cached_tokens = 0;
    if (total_tokens <= 0)
        total_tokens = prompt_tokens + completion_tokens;

    task->tokens_prompt += prompt_tokens;
    task->tokens_completion += completion_tokens;
    task->tokens_total += total_tokens;
    task->tokens_cached += cached_tokens;

    /* How big the conversation is *now*, remembered rather than summed: what this
     * round sent plus what it got back -- where the next request starts from, so
     * it is the group's context width in the sidebar. The all-zero guard above
     * keeps a provider that reports no usage from erasing it. */
    if (total_tokens > 0)
    {
        task->last_total_tokens = total_tokens;
        /* The anchor's position, recorded before the response is folded into the
         * turn (exec_done appends the assistant message after this call), so it
         * describes the request as it was sent. */
        task->last_fold_epoch = task->group ? task->group->fold_epoch : 0;
        task->last_hist_len =
            (task->group && task->group->history) ? cJSON_GetArraySize(task->group->history) : 0;
        task->last_turn_len = task->active_turn ? (int)task->active_turn->messages.tail : 0;
    }
}

/* ── L2 session memory: notice delivery ────────────────────── */

void edc_task_memo_commit(struct edc_task *task)
{
    if (!task)
        return;
    /* Called only after "this request really came back" (see the executor's response
     * entry): the model read the notice, so the watermark advances. Cancel/failure do
     * not call it, so the next request resends it -- idempotent and harmless. */
    if (task->memo_notice_seq > task->memo_seen_seq)
        task->memo_seen_seq = task->memo_notice_seq;
}

/* ── Dependencies (IPC) ────────────────────────────────────── */

/* DFS cycle detection: if 'source' can reach 'target', adding target->source
 * would create a deadlock. */
#define EDC_TASK_MAX_DEPTH 64

static bool edc_task_can_reach(struct edc_task *from, struct edc_task *to, int depth)
{
    if (depth > EDC_TASK_MAX_DEPTH)
        return false;
    if (from == to)
        return true;

    struct edc_task_dep *dep;
    LL_FOREACH(from->deps, dep)
    {
        /* A VOIDED edge imposes no order anymore (its source ended and released us), so it
         * cannot deadlock: counting it would report a cycle for a pair that is free to run
         * in the other direction now -- e.g. re-queueing the stopped task behind the one
         * that took its place. */
        if (dep->cancelled)
            continue;
        if (edc_task_can_reach(dep->source, to, depth + 1))
            return true;
    }
    return false;
}

/* deps_total / deps_completed are the "active edge" counts: a voided edge is not a
 * constraint anymore and does not count (which is exactly what task_void_barrier_dep
 * leaves behind). Recomputing them from the flags keeps the two in step after a re-arm. */
static void task_recount_deps(struct edc_task *task)
{
    size_t total = 0, completed = 0;
    struct edc_task_dep *dep;
    LL_FOREACH(task->deps, dep)
    {
        if (dep->cancelled)
            continue;
        total++;
        if (dep->completed)
            completed++;
    }
    task->deps_total = total;
    task->deps_completed = completed;
}

bool edc_task_add_dependency(struct edc_task *target, struct edc_task *source, edc_task_dep_type type)
{
    if (!target || !source || target == source)
        return false;

    /* Cycle detection: reject when source can reach target */
    if (edc_task_can_reach(source, target, 0))
        return false;

    /* An edge to this source is the pair's dedup key. Still live -> nothing to do
     * (idempotent). VOIDED -> the order it stood for is over, but asking for it again is a
     * NEW constraint on the same pair: resume re-queues a task behind the one it was
     * queued behind. Re-arm it in place; leaving it voided would keep it out of
     * deps_total and let the target jump the queue it just rejoined. */
    struct edc_task_dep *existing;
    LL_FOREACH(target->deps, existing)
    {
        if (existing->source == source)
            break;
    }
    if (existing && !existing->cancelled)
        return false;
    if (existing)
    {
        existing->cancelled = false;
        existing->type = type;
        struct edc_task_dep *sd;
        LL_FOREACH(source->dependents, sd)
        {
            if (sd->target == target)
            {
                sd->cancelled = false;
                sd->type = type;
                break;
            }
        }
        task_recount_deps(target);

        if (edc_task_is_completed(source))
            edc_task_on_dep_completed(target, source);
        else if (edc_task_is_ready(target))
            edc_task_mark_waiting_deps(target);
        return true;
    }

    /* Add a node to target->deps */
    struct edc_task_dep *dep_t = g_new0(struct edc_task_dep, 1);
    dep_t->source = source;
    dep_t->target = target;
    dep_t->type = type;
    LL_APPEND(target->deps, dep_t);

    /* Add the matching node to source->dependents */
    struct edc_task_dep *dep_s = g_new0(struct edc_task_dep, 1);
    dep_s->source = source;
    dep_s->target = target;
    dep_s->type = type;
    LL_APPEND(source->dependents, dep_s);

    target->deps_total++;

    /* If source is already completed, mark the dep completed at once */
    if (edc_task_is_completed(source))
    {
        edc_task_on_dep_completed(target, source);
    }
    /* If target just gained a dependency, go to WAITING_DEPS. READY's mirror is IDLE;
     * with WAITING_INPUT retired both share the landing spot, no longer parallel. */
    else if (edc_task_is_ready(target))
    {
        edc_task_mark_waiting_deps(target);
    }

    return true;
}

bool edc_task_depends_on(const struct edc_task *target, const struct edc_task *source)
{
    if (!target || !source)
        return false;
    struct edc_task_dep *dep;
    LL_FOREACH(target->deps, dep)
    {
        if (dep->source == source && !dep->cancelled)
            return true;
    }
    return false;
}

bool edc_task_has_unresolved_deps(const struct edc_task *task){
    if (!task)
        return false;
    struct edc_task_dep *dep;
    LL_FOREACH(task->deps, dep)
    {
        if (!dep->completed && !dep->cancelled)
            return true;
    }
    return false;
}

/* Ledger: the side-effect move recorder (defined in the dependency section below). */
static void task_record_move(struct edc_task *source, struct edc_task *moved);

void edc_task_on_dep_completed(struct edc_task *target, struct edc_task *source)
{
    if (!target || !source)
        return;

    struct edc_task_dep *found = NULL;
    struct edc_task_dep *dep;
    LL_FOREACH(target->deps, dep)
    {
        if (dep->source == source && !dep->completed && !dep->cancelled)
        {
            found = dep;
            dep->completed = true;
            target->deps_completed++;

            /* Also mark the matching node in source->dependents */
            struct edc_task_dep *sd;
            LL_FOREACH(source->dependents, sd)
            {
                if (sd->target == target && !sd->completed)
                {
                    sd->completed = true;
                    break;
                }
            }
            break;
        }
    }

    /* `||` INVERTS the rule: the source SUCCEEDING is exactly the case in which the dependent
     * must not run. Cancel it -- but only while it is still waiting: a task the user already
     * resumed is theirs to run, and this same path replays on load (see deser_deps). The edge
     * stays completed, so an explicit resume afterwards does run it.
     * NB a skipped task cancels its own dependents; `a || b ; c` is fine (the `;` edge is a
     * BARRIER and releases c), while `a || b && c` takes c down with b -- unlike a shell,
     * which would pass a's success through the skipped b. */
    if (found && found->type == TASK_DEP_UNLESS)
    {
        if (edc_task_awaits_deps(target))
        {
            edc_task_step step_before = target->step;
            edc_task_life life_before = target->life;
            edc_task_send_signal(target, TASK_SIGTERM);
            if (target->step != step_before || target->life != life_before)
                task_record_move(source, target);
        }
        return;
    }

    if (target->deps_completed >= target->deps_total && edc_task_awaits_deps(target))
    {
        /* Completion also moves the dependent's phase, so it owes the same
         * bookkeeping and is recorded here (idempotent when nothing moved). */
        edc_task_step step_before = target->step;
        edc_task_life life_before = target->life;
        edc_task_mark_ready(target);
        if (target->step != step_before || target->life != life_before)
            task_record_move(source, target);
    }
}

/* Side-effect move report (see edc_task.h). */

/* Record a debt: |moved|'s phase was moved as a side effect of an operation
 * |source| started. The debt is charged to the INITIATOR -- the host is about to
 * settle it and clears the list along the way. */
static void task_record_move(struct edc_task *source, struct edc_task *moved)
{
    if (!source || !moved)
        return;
    struct edc_task_move_report *r = malloc(sizeof(*r));
    if (!r)
        return; /* a failed record only loses one notice; no phase changes */
    r->task = moved;
    r->next = source->moved_report;
    source->moved_report = r;
}

/* Void a RELEASING edge to a source that has terminated: the constraint it stood for is
 * over, so drop the edge and let target run. Two kinds release their target -- BARRIER (the
 * source ending either way is all it needed) and UNLESS (`||`, where a source that did NOT
 * succeed is exactly why the dependent exists). JOIN / SEQUENCE keep their edge and take the
 * target with them instead. */
static void task_void_dep(struct edc_task_dep *dep, struct edc_task *target, struct edc_task *source)
{
    dep->cancelled = true;

    /* Also mark the matching node in source->dependents */
    struct edc_task_dep *sd;
    LL_FOREACH(source->dependents, sd)
    {
        if (sd->target == target && !sd->cancelled)
        {
            sd->cancelled = true;
            break;
        }
    }

    /* Recompute: count only non-cancelled dependencies */
    size_t active_deps = 0;
    struct edc_task_dep *d;
    LL_FOREACH(target->deps, d)
    {
        if (!d->cancelled)
            active_deps++;
    }
    target->deps_total = active_deps;

    if (target->deps_total == 0 && edc_task_awaits_deps(target))
    {
        /* Voiding the last invalid dependency moves this task from AWAIT_DEPS to
         * READY -- also a phase change (the sidebar's dep-wait clears), also recorded. */
        edc_task_step step_before = target->step;
        edc_task_life life_before = target->life;
        edc_task_mark_ready(target);
        if (target->step != step_before || target->life != life_before)
            task_record_move(source, target);
    }
}

void edc_task_drain_moves(struct edc_task *task, void (*cb)(void *ctx, struct edc_task *moved), void *ctx)
{
    if (!task)
        return;
    /* Drain to empty: the callback may FILL more (settling itself moves other tasks).
     * cb = NULL just clears the list (used when freeing the task). */
    while (task->moved_report)
    {
        struct edc_task_move_report *r = task->moved_report;
        task->moved_report = r->next;
        if (cb)
            cb(ctx, r->task);
        free(r);
    }
}

void edc_task_on_dep_cancelled(struct edc_task *target, struct edc_task *source)
{
    if (!target || !source)
        return;

    /* Find this still-waiting dependency; done/cancelled/absent -> nothing to do */
    struct edc_task_dep *dep = NULL;
    LL_FOREACH(target->deps, dep)
    {
        if (dep->source == source && !dep->completed && !dep->cancelled)
            break;
    }
    if (!dep)
        return;

    /* Two kinds are released by the source ending badly: BARRIER (a fold / curation round
     * that died must not take the tasks it was holding with it) and UNLESS (`||` -- "run the
     * right-hand side only if the left one did not succeed", so this IS its turn). */
    if (dep->type == TASK_DEP_BARRIER || dep->type == TASK_DEP_UNLESS)
    {
        task_void_dep(dep, target, source);
        return;
    }

    /* A dependency the user authored -- JOIN or SEQUENCE, which differ only in whether the
     * source's output would have been injected: the source produced nothing, so this
     * dependent cannot continue => cancel it in a cascade. The EDGE STAYS (cancel does not
     * detach it), so a later resume sends it back to dep-wait until the source really
     * produces. */
    edc_task_step step_before = target->step;
    edc_task_life life_before = target->life;
    edc_task_send_signal(target, TASK_SIGTERM);
    /* The cascade cancel is also a PHASE CHANGE -- this task now enters a terminal
     * state while the host only named the source task. Record a debt (repeated
     * cancels are idempotent: no real move, no record). */
    if (target->step != step_before || target->life != life_before)
        task_record_move(source, target);
}

/* Handle a failed source, symmetric with cancellation (neither will produce
 * again), only the terminal state differs. Both ERROR and CANCELLED are resumable. */
void edc_task_on_dep_failed(struct edc_task *target, struct edc_task *source)
{
    if (!target || !source)
        return;

    /* Find this still-waiting dependency; done/voided/absent -> nothing to do */
    struct edc_task_dep *dep = NULL;
    LL_FOREACH(target->deps, dep)
    {
        if (dep->source == source && !dep->completed && !dep->cancelled)
            break;
    }
    if (!dep)
        return;

    /* Released by the source ending badly -- see on_dep_cancelled (BARRIER and `||`). */
    if (dep->type == TASK_DEP_BARRIER || dep->type == TASK_DEP_UNLESS)
    {
        task_void_dep(dep, target, source);
        return;
    }

    /* Cascade failure, and the EDGE STAYS. The dependency is unsatisfiable *now* -- not
     * forever: the source is ERROR / CANCELLED / INTERRUPTED and all of them are resumable.
     * Detaching the edge would silently give up what this task is waiting for (its output
     * for JOIN, its turn for SEQUENCE); keeping it means a later resume returns this task to
     * dep-wait, and when the source really produces its completed edge releases this one
     * automatically (edc_task_on_dep_completed -> mark_ready -> the scan drives it). */
    edc_task_step step_before = target->step;
    edc_task_life life_before = target->life;

    /* Write the reason into output, so the host has a notice instead of a false generic
     * "LLM request failed". The wording follows what actually happened to the source: an
     * interrupted one did not fail and can still be resumed into producing. */
    const char *what = edc_task_is_interrupted(source) ? "was interrupted" : "failed";
    const char *why = (dep->type == TASK_DEP_JOIN)
                          ? "so the output it was waiting for is not available -- resume that one to get it"
                          : "so this task cannot go on -- resume that one first";
    if (source->task_id)
        edc_task_set_output(target, sdscatprintf(sdsempty(), "[failed: its dependency %s %s, %s]", source->task_id,
                                                 what, why));
    else
        edc_task_set_output(target, sdscatprintf(sdsempty(), "[failed: a dependency %s, %s]", what, why));

    edc_task_mark_failed(target); /* edge not detached: see above */
    /* The cascade failure is also a PHASE CHANGE -- this task now enters a terminal
     * state while the host only named the source task. Record a debt (repeated
     * failures are idempotent: no real move, no record). */
    if (target->step != step_before || target->life != life_before)
        task_record_move(source, target);
}

/* Failure broadcast: snapshot the dependents before processing, since a JOIN
 * cascade recursively unlinks nodes from that list. The cascade never frees the
 * tasks themselves, so raw pointers are safe. */
static void task_notify_dependents_failed(struct edc_task *task)
{
    if (!task)
        return;

    size_t n = 0;
    struct edc_task_dep *dep;
    LL_FOREACH(task->dependents, dep)
    n++;
    if (n == 0)
        return;

    struct edc_task **targets = malloc(n * sizeof(*targets));
    if (!targets)
        return; /* a failed snapshot only loses one broadcast; no phase changes */
    size_t i = 0;
    LL_FOREACH(task->dependents, dep)
    targets[i++] = dep->target;
    for (size_t k = 0; k < i; k++)
        edc_task_on_dep_failed(targets[k], task);
    free(targets);
}

void edc_task_notify_dependents(struct edc_task *task)
{
    if (!task)
        return;

    struct edc_task_dep *dep, *tmp;
    LL_FOREACH_SAFE(task->dependents, dep, tmp)
    {
        if (!dep->cancelled && !dep->completed)
        {
            dep->completed = true;
            edc_task_on_dep_completed(dep->target, task);
        }
    }
}

/* ── Signals ───────────────────────────────────────────────── */

bool edc_task_is_running(const struct edc_task *task)
{
    /* Running = ALIVE and awaiting the executor/tool reply; stopped-while-awaiting tools
     * (INTERRUPTED / SUSPENDED) does not count. */
    return edc_task_awaits_exec(task) || edc_task_awaits_tools(task);
}

void edc_task_send_signal(struct edc_task *task, edc_task_signal sig)
{
    if (!task)
        return;

    task->cancelled_request = 1;

    if (sig == TASK_SIGTERM)
    {
        /* Snapshot the dependent list first: a cascade cancel writes to them (voided
         * edges) and the list itself may change during recursion -- taking it out
         * before iterating is safest. */
        size_t n = 0;
        struct edc_task_dep *dep;
        LL_FOREACH(task->dependents, dep)
        n++;
        if (n > 0)
        {
            struct edc_task **targets = malloc(n * sizeof(*targets));
            if (targets)
            {
                size_t i = 0;
                LL_FOREACH(task->dependents, dep)
                targets[i++] = dep->target;
                for (size_t k = 0; k < i; k++)
                    edc_task_on_dep_cancelled(targets[k], task);
                free(targets);
            }
        }
    }

    /* It does NOT touch its own deps: cancel = stop, not delete/reclaim -- the task can
     * still resume and its dependencies are part of its state. Detaching the edges would
     * let resume run "as if it never depended", silently dropping the input it was owed.
     * Keeping them => on resume the gate counts unsatisfied edges and returns it to
     * dep-wait until the source really produces (or is deleted). */

    /* Cancelled: chasing in-flight tool records is pointless; drop the ledger */
    edc_task_tool_calls_clear(task);
    edc_task_mark_cancelled(task);
}

/* ── Incomplete tool-call table (resume ledger) ────────────── */

static void edc_task_tool_call_record_free(struct edc_tool_call_record *rec)
{
    if (!rec)
        return;
    if (rec->call_id)
        sdsfree(rec->call_id);
    if (rec->name)
        sdsfree(rec->name);
    if (rec->args)
        cJSON_Delete(rec->args);
    free(rec);
}

struct edc_tool_call_record *edc_task_tool_call_add(struct edc_task *task, const char *call_id, const char *name,
                                                    cJSON *args)
{
    if (!task)
        return NULL;
    struct edc_tool_call_record *rec = g_new0(struct edc_tool_call_record, 1);
    rec->call_id = call_id ? sdsnew(call_id) : NULL;
    rec->name = name ? sdsnew(name) : NULL;
    rec->args = args ? cJSON_Duplicate(args, 1) : NULL;
    rec->status = TOOL_CALL_PENDING;
    LL_APPEND(task->tool_calls, rec);
    return rec;
}

struct edc_tool_call_record *edc_task_tool_call_find(struct edc_task *task, const char *call_id)
{
    if (!task || !call_id)
        return NULL;
    struct edc_tool_call_record *rec;
    LL_FOREACH(task->tool_calls, rec)
    {
        if (rec->call_id && strcmp(rec->call_id, call_id) == 0)
            return rec;
    }
    return NULL;
}

struct edc_tool_call_record *edc_task_tool_call_mark_executing(struct edc_task *task, const char *call_id)
{
    struct edc_tool_call_record *rec = edc_task_tool_call_find(task, call_id);
    if (rec)
        rec->status = TOOL_CALL_EXECUTING;
    return rec;
}

bool edc_task_tool_call_remove(struct edc_task *task, const char *call_id)
{
    if (!task || !call_id)
        return false;
    struct edc_tool_call_record *rec, *tmp;
    LL_FOREACH_SAFE(task->tool_calls, rec, tmp)
    {
        if (rec->call_id && strcmp(rec->call_id, call_id) == 0)
        {
            LL_DELETE(task->tool_calls, rec);
            edc_task_tool_call_record_free(rec);
            return true;
        }
    }
    return false;
}

void edc_task_tool_calls_clear(struct edc_task *task)
{
    if (!task)
        return;
    struct edc_tool_call_record *rec, *tmp;
    LL_FOREACH_SAFE(task->tool_calls, rec, tmp)
    {
        LL_DELETE(task->tool_calls, rec);
        edc_task_tool_call_record_free(rec);
    }
    task->tool_calls = NULL;
}

bool edc_task_has_pending_tool_calls(const struct edc_task *task)
{
    return task && task->tool_calls != NULL;
}

/* Repair dangling tool_calls (pre-drive; see edc_task.h). Idempotent. */

static const char *const kInterruptedToolNote =
    "[interrupted: this tool was still running when the process restarted; it did not produce a result]";

/* Same text, for the executor: a tool-only task resumed into the repair has no result
 * of its own, and this is what its output has to say about that. */
const char *edc_task_interrupted_tool_note(void)
{
    return kInterruptedToolNote;
}

static bool task_turn_has_tool_response(struct edc_turn *turn, size_t upto, const char *call_id)
{
    for (size_t i = 0; i < upto; i++)
    {
        cJSON *m = turn->messages.data[i];
        cJSON *role = cJSON_GetObjectItem(m, "role");
        cJSON *tcid = cJSON_GetObjectItem(m, "tool_call_id");
        if (role && cJSON_IsString(role) && strcmp(role->valuestring, "tool") == 0 && tcid && cJSON_IsString(tcid) &&
            strcmp(tcid->valuestring, call_id) == 0)
            return true;
    }
    return false;
}

void edc_task_repair_dangling_tool_calls(struct edc_task *task)
{
    struct edc_turn *turn = (task && task->active_turn) ? task->active_turn : NULL;
    if (!turn)
        return;

    /* Scan only the pre-existing messages: tool messages added here are not iterated.
     * The queue reallocs on append, so the base pointer is re-read each round (do not
     * cache the array's first address). */
    size_t n = turn->messages.tail;
    for (size_t i = 0; i < n; i++)
    {
        cJSON *m = turn->messages.data[i];
        cJSON *role = cJSON_GetObjectItem(m, "role");
        cJSON *tcs = cJSON_GetObjectItem(m, "tool_calls");
        if (!role || !cJSON_IsString(role) || strcmp(role->valuestring, "assistant") != 0 || !tcs ||
            !cJSON_IsArray(tcs))
            continue;

        cJSON *tc = NULL;
        cJSON_ArrayForEach(tc, tcs)
        {
            cJSON *id = cJSON_GetObjectItem(tc, "id");
            if (!id || !cJSON_IsString(id) || !id->valuestring || !*id->valuestring)
                continue;

            const struct edc_tool_call_record *rec = edc_task_tool_call_find(task, id->valuestring);
            if (rec && rec->status != TOOL_CALL_EXECUTING)
                continue; /* PENDING: let the drive section really dispatch; do not fake a response */
            if (task_turn_has_tool_response(turn, n, id->valuestring))
                continue; /* already has a result (idempotent) */

            cJSON *fn = cJSON_GetObjectItem(tc, "function");
            cJSON *nm = fn ? cJSON_GetObjectItem(fn, "name") : NULL;
            edc_turn_add_tool_message(turn, id->valuestring, (nm && cJSON_IsString(nm)) ? nm->valuestring : "",
                                      kInterruptedToolNote);
            /* If the ledger still holds this one (INTERRUPTED / SUSPENDED / ERROR path: EXECUTING
             * dispatched, result never returned), remove it too -- it now has a
             * response and must not count as incomplete. */
            edc_task_tool_call_remove(task, id->valuestring);
        }
    }
}

/* ── Output ────────────────────────────────────────────────── */

void edc_task_set_output(struct edc_task *task, sds output)
{
    if (!task)
        return;
    if (task->output)
        sdsfree(task->output);
    task->output = output;
}

sds edc_task_get_output(struct edc_task *task)
{
    if (!task)
        return NULL;
    return task->output;
}

void edc_task_set_completion_seq(struct edc_task *task, uint32_t seq)
{
    if (!task)
        return;
    task->completion_seq = seq;
}

/* ── Folding ───────────────────────────────────────────────── */

/* Foldable = finished, not yet folded, and not itself a fold (sink != GROUP_MEMORY).
 * The `sink` condition keeps the fold task (and the whole system group) out of the
 * fold account: a task whose output is group memory is not conversation to compress.
 * (It used to be a `lifetime` condition; the fold task is now an ordinary persisted
 * task, so the exclusion rides `sink`, which is what actually says "my output is
 * memory".) */
bool edc_task_is_memory_sink(const struct edc_task *task)
{
    return task && (task->sink == EDC_SINK_GROUP_MEMORY || task->sink == EDC_SINK_SESSION_MEMORY);
}

bool edc_task_is_foldable(struct edc_task *task)
{
    return task && !edc_task_is_memory_sink(task) && edc_task_is_completed(task) &&
           edc_task_unfolded_msg_count(task) > 0;
}

size_t edc_task_turn_msg_count(struct edc_task *task)
{
    if (!task)
        return 0;

    size_t n = 0;
    for (size_t qi = task->turns.head; qi < task->turns.tail; qi++)
    {
        struct edc_turn *turn = task->turns.data[qi];
        n += edc_queue_length(&turn->messages);
    }
    return n;
}

size_t edc_task_unfolded_msg_count(struct edc_task *task)
{
    if (!task)
        return 0;

    size_t n = 0;
    for (size_t qi = task->turns.head; qi < task->turns.tail; qi++)
    {
        struct edc_turn *turn = task->turns.data[qi];
        size_t len = edc_queue_length(&turn->messages);
        size_t folded = (turn->folded_prefix > 0) ? (size_t)turn->folded_prefix : 0;
        if (folded > len)
            folded = len;
        n += len - folded;
    }
    return n;
}

bool edc_task_has_folded_turns(struct edc_task *task)
{
    if (!task)
        return false;

    for (size_t qi = task->turns.head; qi < task->turns.tail; qi++)
    {
        struct edc_turn *turn = task->turns.data[qi];
        if (turn->folded_prefix > 0)
            return true;
    }
    return false;
}

/* ── Context building ──────────────────────────────────────── *
 *
 * A per-task custom system prompt used to live here (`edc_task_set_prompt` +
 * `task->prompt` / `task->prompt_name`). It was a stub -- every caller passed a NULL
 * body -- and has been removed: reusing a prompt is now the `/prompt` command's job
 * (docs/prompt-library.md). */

cJSON *edc_task_get_context_messages(struct edc_task *task, struct edc_config *config)
{
    if (!task)
        return cJSON_CreateArray();

    cJSON *messages = cJSON_CreateArray();

    /* 1. System prompt */
    if (config)
    {
        sds default_prompt = edc_context_build_system_prompt(config, NULL);
        cJSON *sys_msg = cJSON_CreateObject();
        cJSON_AddStringToObject(sys_msg, "role", "system");
        cJSON_AddStringToObject(sys_msg, "content", default_prompt);
        cJSON_AddItemToArray(messages, sys_msg);
        sdsfree(default_prompt);
    }

    /* 2. committed turn messages */
    for (size_t qi = task->turns.head; qi < task->turns.tail; qi++)
    {
        struct edc_turn *turn = task->turns.data[qi];
        for (size_t mj = turn->messages.head; mj < turn->messages.tail; mj++)
        {
            cJSON *item = turn->messages.data[mj];
            cJSON_AddItemReferenceToArray(messages, item);
        }
    }

    /* 3. inject completed dependency tasks' output */
    struct edc_task_dep *dep;
    LL_FOREACH(task->deps, dep)
    {
        if (dep->completed && dep->source->output)
        {
            sds note = sdscatprintf(sdsempty(), "[subtask %s result]:\n%s", dep->source->description, dep->source->output);
            cJSON *sys = cJSON_CreateObject();
            cJSON_AddStringToObject(sys, "role", "system");
            cJSON_AddStringToObject(sys, "content", note);
            cJSON_AddItemToArray(messages, sys);
            sdsfree(note);
        }
    }

    return messages;
}

/* ── Relations / outbound (see edc_task.h) ─────────────────── */

struct edc_session *edc_task_session(const struct edc_task *task)
{
    if (!task || !task->group)
        return NULL;
    return task->group->session;
}

void edc_task_flush_redirects(struct edc_task *task)
{
    if (!task)
        return;

    if (task->redirect_stdout)
    {
        FILE *f = fopen(task->redirect_stdout, task->redirect_append ? "a" : "w");
        if (f)
        {
            if (task->output)
                fputs(task->output, f);
            fclose(f);
        }
    }
    if (task->redirect_stderr)
    {
        /* `2>>` appends; `2>` truncates. (It used to always truncate -- that was half of the
         * `2>>` bug; the other half is that nothing set the flag yet, see the lexer work.) */
        FILE *f = fopen(task->redirect_stderr, task->redirect_stderr_append ? "a" : "w");
        if (f)
        {
            if (task->output)
                fputs(task->output, f);
            fclose(f);
        }
    }
}
