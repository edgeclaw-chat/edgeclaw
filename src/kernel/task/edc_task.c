#include "edc_task.h"

#include "edc_payload.h"
#include "edc_compat.h"
#include "edc_context.h"
#include "edc_session.h"
#include "edc_task_group.h"
#include "edc_state_store.h"
#include "edc_token_estimate.h"
#include "edc_turn.h"
#include "utlist_fix.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

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
    /* Legacy string only -- "SUSPENDED" is no longer a state (the old load-time collapse
     * wrote it, and what it meant is exactly what INTERRUPTED says). Old archives must
     * still load, so the spelling keeps its meaning here and nowhere else. */
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
const char *edc_task_end_name(edc_task_end end)
{
    switch (end)
    {
    case EDC_END_INTERRUPTED:
        return "INTERRUPTED";
    case EDC_END_STOPPED:
        return "STOPPED";
    case EDC_END_FAILED:
        return "FAILED";
    case EDC_END_BLOCKED:
        return "BLOCKED";
    case EDC_END_SKIPPED:
        return "SKIPPED";
    case EDC_END_NONE:
        break;
    }
    return "NONE";
}

edc_task_end edc_task_classify_end(const char *name)
{
    if (!name)
        return EDC_END_NONE;
    if (strcmp(name, "INTERRUPTED") == 0)
        return EDC_END_INTERRUPTED;
    if (strcmp(name, "STOPPED") == 0)
        return EDC_END_STOPPED;
    if (strcmp(name, "FAILED") == 0)
        return EDC_END_FAILED;
    if (strcmp(name, "BLOCKED") == 0)
        return EDC_END_BLOCKED;
    if (strcmp(name, "SKIPPED") == 0)
        return EDC_END_SKIPPED;
    return EDC_END_NONE;
}

/* The persistence spelling of a dependency edge, and its inverse. Lives here rather than in
 * the serializer because the STORE writes and reads these directly now; every other enum in
 * this file already has the same name/classify pair. */
const char *edc_task_dep_type_name(edc_task_dep_type t)
{
    switch (t)
    {
    case TASK_DEP_JOIN:
        return "JOIN";
    case TASK_DEP_SEQUENCE:
        return "SEQUENCE";
    case TASK_DEP_BARRIER:
        return "BARRIER";
    case TASK_DEP_UNLESS:
        return "UNLESS";
    default:
        return "UNKNOWN";
    }
}

edc_task_dep_type edc_task_classify_dep_type(const char *name)
{
    if (!name)
        return TASK_DEP_JOIN;
    if (strcmp(name, "JOIN") == 0)
        return TASK_DEP_JOIN;
    if (strcmp(name, "SEQUENCE") == 0)
        return TASK_DEP_SEQUENCE;
    if (strcmp(name, "BARRIER") == 0)
        return TASK_DEP_BARRIER;
    if (strcmp(name, "UNLESS") == 0)
        return TASK_DEP_UNLESS;
    return TASK_DEP_JOIN;
}

const char *edc_task_fail_name(edc_task_fail fail)
{
    switch (fail)
    {
    case EDC_FAIL_AUTH:
        return "AUTH";
    case EDC_FAIL_CONTEXT_OVERFLOW:
        return "CONTEXT_OVERFLOW";
    case EDC_FAIL_RATE_LIMIT:
        return "RATE_LIMIT";
    case EDC_FAIL_TRANSPORT:
        return "TRANSPORT";
    case EDC_FAIL_OTHER:
        return "OTHER";
    case EDC_FAIL_NONE:
        break;
    }
    return "NONE";
}

edc_task_fail edc_task_classify_fail(const char *name)
{
    if (!name)
        return EDC_FAIL_NONE;
    if (strcmp(name, "AUTH") == 0)
        return EDC_FAIL_AUTH;
    if (strcmp(name, "CONTEXT_OVERFLOW") == 0)
        return EDC_FAIL_CONTEXT_OVERFLOW;
    if (strcmp(name, "RATE_LIMIT") == 0)
        return EDC_FAIL_RATE_LIMIT;
    if (strcmp(name, "TRANSPORT") == 0)
        return EDC_FAIL_TRANSPORT;
    if (strcmp(name, "OTHER") == 0)
        return EDC_FAIL_OTHER;
    return EDC_FAIL_NONE;
}

void edc_task_set_fail(struct edc_task *task, edc_task_fail fail)
{
    if (!task)
        return;
    task->fail = fail;
}

const char *edc_task_state_str(const struct edc_task *task)
{
    if (!task)
        return "IDLE";
    switch (task->life)
    {
    case EDC_LIFE_INTERRUPTED:
        return "INTERRUPTED";
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
    /* Read the old "can still advance" answer before the write, so the group's idle clock
     * is only touched when that answer actually changed. ALIVE -> ALIVE (a step change
     * inside a running task) leaves busy_members alone, and so does every terminal ->
     * terminal rewrite. */
    const bool was_finished = edc_task_is_finished(task);
    task->step = step;
    task->life = life;
    /* The reason for a past end belongs to that end: a task that can move again has no
     * end, so resuming wipes the slate here instead of in every writer. */
    if (life == EDC_LIFE_ALIVE)
    {
        task->end = EDC_END_NONE;
        task->fail = EDC_FAIL_NONE;
        task->blocked_end = EDC_END_NONE;
        if (task->blocked_by)
        {
            free(task->blocked_by);
            task->blocked_by = NULL;
        }
    }
    /* The one place a member's terminal-ness changes: tell the group, so `idle_since`
     * says when the group actually BECAME idle. Every runtime transition comes through
     * here (mark_completed / failed / cancelled / skipped / blocked / interrupted are
     * wrappers), which is why the recount can live in one spot. */
    if (task->group && !edc_task_is_memory_sink(task) && was_finished != edc_task_is_finished(task))
        edc_task_group_refresh_busy(task->group, time(NULL));
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
/* Cancel broadcast, declared here because mark_blocked (above the dependency section)
 * passes the cascade on. */
static void task_notify_dependents_cancelled(struct edc_task *task);

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
    task->end = EDC_END_FAILED;
    task_notify_dependents_failed(task);
    return true;
}

bool edc_task_mark_cancelled(struct edc_task *task)
{
    if (!edc_task_move(task, EDC_TASKSTEP_DONE, EDC_LIFE_CANCELLED))
        return false;
    task->end = EDC_END_STOPPED;
    return true;
}

bool edc_task_mark_skipped(struct edc_task *task)
{
    if (!edc_task_move(task, EDC_TASKSTEP_DONE, EDC_LIFE_CANCELLED))
        return false;
    task->end = EDC_END_SKIPPED;
    return true;
}

bool edc_task_mark_blocked(struct edc_task *task, edc_task_life life, const struct edc_task *source)
{
    if (!edc_task_move(task, EDC_TASKSTEP_DONE, life))
        return false;
    task->end = EDC_END_BLOCKED;
    task->blocked_end = source ? source->end : EDC_END_NONE;
    if (task->blocked_by)
    {
        free(task->blocked_by);
        task->blocked_by = NULL;
    }
    /* blocked_by is informational text that outlives the source task, so the numeric id is
     * rendered here instead of being stored as an id. strdup, not sdscatprintf: this field
     * is freed with free() everywhere else in the file, and an sds header would turn that
     * into an invalid free. */
    if (source && source->task_id > 0)
    {
        char idbuf[32];
        snprintf(idbuf, sizeof(idbuf), "%lld", (long long)source->task_id);
        task->blocked_by = strdup(idbuf);
    }
    /* A cascaded task will not produce either, so it passes the cascade on -- through the
     * SAME broadcast its life implies, exactly as the stop and failure paths do. */
    if (life == EDC_LIFE_CANCELLED)
        task_notify_dependents_cancelled(task);
    else
        task_notify_dependents_failed(task);
    return true;
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
    task->end = EDC_END_INTERRUPTED;
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
         * with ERROR. */
        return (to_life == EDC_LIFE_ALIVE &&
                (to_step == EDC_TASKSTEP_READY || to_step == EDC_TASKSTEP_AWAIT_DEPS ||
                 to_step == EDC_TASKSTEP_AWAIT_EXEC || to_step == EDC_TASKSTEP_AWAIT_TOOLS)) ||
               (to_step == EDC_TASKSTEP_DONE && to_life == EDC_LIFE_CANCELLED);
    case EDC_LIFE_INTERRUPTED:
        /* The process went down mid-flight: nothing was decided, so resume re-decides. */
    case EDC_LIFE_ERROR:
        /* continue the failed round (same target set as INTERRUPTED), or cancel. */
        return (to_life == EDC_LIFE_ALIVE &&
                (to_step == EDC_TASKSTEP_READY || to_step == EDC_TASKSTEP_AWAIT_DEPS ||
                 to_step == EDC_TASKSTEP_AWAIT_EXEC || to_step == EDC_TASKSTEP_AWAIT_TOOLS)) ||
               (to_step == EDC_TASKSTEP_DONE && to_life == EDC_LIFE_CANCELLED);
    case EDC_LIFE_ALIVE:
        break;
    }

    /* An interruption keeps the step it was on and may land on any of them: the process
     * went down mid-request. */
    if (to_life == EDC_LIFE_INTERRUPTED)
        return to_step == from_step;

    switch (from_step)
    {
    /* READY = parked with input, waiting to be dispatched: dispatch / hit a
     * dependency / cancel / failure. Failure is allowed so a drive that
     * cannot start any I/O fails instead of being rescanned forever. */
    case EDC_TASKSTEP_READY:
        return (to_life == EDC_LIFE_ALIVE &&
                (to_step == EDC_TASKSTEP_AWAIT_EXEC || to_step == EDC_TASKSTEP_AWAIT_DEPS)) ||
               (to_step == EDC_TASKSTEP_DONE && (to_life == EDC_LIFE_CANCELLED || to_life == EDC_LIFE_ERROR));
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
                (to_life == EDC_LIFE_COMPLETED || to_life == EDC_LIFE_CANCELLED || to_life == EDC_LIFE_ERROR));
    case EDC_TASKSTEP_DONE:
        /* Malformed: DONE yet ALIVE (display name folds to IDLE, see edc_task_state_str's
         * fallback). No path produces it; reuse the READY row to avoid behaviour
         * outside default. */
        return (to_life == EDC_LIFE_ALIVE &&
                (to_step == EDC_TASKSTEP_AWAIT_EXEC || to_step == EDC_TASKSTEP_AWAIT_DEPS)) ||
               (to_step == EDC_TASKSTEP_DONE && to_life == EDC_LIFE_CANCELLED);
    }
    return false;
}

/* Write entries that bypass the state machine (initial state, fixtures): no
 * validation, straight to (step, life). */
void edc_task_force_move(struct edc_task *task, edc_task_step step, edc_task_life life)
{
    if (!task)
        return;
    const bool was_finished = edc_task_is_finished(task);
    task->step = step;
    task->life = life;
    /* Same bookkeeping as edc_task_move: `/task resume` comes through here (INTERRUPTED
     * -> ALIVE via edc_task_resume), and a resumed member makes its group busy again. */
    if (task->group && !edc_task_is_memory_sink(task) && was_finished != edc_task_is_finished(task))
        edc_task_group_refresh_busy(task->group, time(NULL));
}

/* ── Lifetime ──────────────────────────────────────────────── */

struct edc_task *edc_task_new(int64_t task_id, int64_t session_id, int64_t group_id, const char *desc,
                              edc_channel_type channel_type)
{
    struct edc_task *task = g_new0(struct edc_task, 1);
    task->task_id = task_id;
    task->session_id = session_id;
    task->group_id = group_id;
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

static cJSON *turn_user_message_at(struct edc_turn *turn, int *out_ord)
{
    if (out_ord)
        *out_ord = -1;
    if (!turn)
        return NULL;
    for (int i = 0; i < turn->msg_count; i++)
    {
        cJSON *msg = edc_turn_msg_at(turn, i);
        if (!msg)
            continue;
        const char *role = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "role"));
        if (!role || strcmp(role, "user") != 0)
        {
            cJSON_Delete(msg);
            continue;
        }
        /* Pass output (synthetic) is not what the user typed: only their own line. */
        cJSON *synthetic = cJSON_GetObjectItem(msg, "synthetic");
        if (synthetic && cJSON_IsTrue(synthetic))
        {
            cJSON_Delete(msg);
            continue;
        }
        if (out_ord)
            *out_ord = i;
        return msg; /* the caller owns it */
    }
    return NULL;
}

static cJSON *turn_user_message(struct edc_turn *turn)
{
    return turn_user_message_at(turn, NULL);
}

void edc_task_set_user_origin(struct edc_task *task, const char *mid, const struct edc_payload *payload)
{
    if (!task || !task->active_turn)
        return;
    if (!mid && edc_payload_empty(payload))
        return;

    int ord = -1;
    cJSON *msg = turn_user_message_at(task->active_turn, &ord);
    if (!msg)
        return;

    if (mid && *mid)
    {
        cJSON_DeleteItemFromObject(msg, "mid");
        cJSON_AddStringToObject(msg, "mid", mid);
    }
    if (!edc_payload_empty(payload))
    {
        cJSON *map = cJSON_CreateObject();
        if (!map)
        {
            cJSON_Delete(msg);
            return;
        }
        for (int k = 0; k < payload->n; k++)
            cJSON_AddStringToObject(map, payload->items[k].key, payload->items[k].text);
        cJSON_DeleteItemFromObject(msg, "payload");
        cJSON_AddItemToObject(msg, "payload", map);
    }

    /* The row is the truth: a decoration that is not written back disappears on the next
     * read (and after a restart). Re-write at the same position, then drop the copy. */
    if (ord >= 0)
        edc_msg_append(task->active_turn->task_id, task->active_turn->turn_ord, ord, msg,
                       edc_token_estimate_msg(msg));
    cJSON_Delete(msg);
}

cJSON *edc_task_user_payload(struct edc_task *task)
{
    if (!task)
        return NULL;
    cJSON *msg = turn_user_message(task->active_turn);
    if (!msg)
    {
        for (size_t i = task->turns.tail; i > task->turns.head && !msg; i--)
            msg = turn_user_message((struct edc_turn *)task->turns.data[i - 1]);
    }
    if (!msg)
        return NULL;
    /* The message is a query result owned here; the caller gets its own copy of the
     * payload because the message itself does not survive this call any more. */
    cJSON *payload = cJSON_GetObjectItem(msg, "payload");
    cJSON *out = payload ? cJSON_Duplicate(payload, 1) : NULL;
    cJSON_Delete(msg);
    return out;
}

cJSON *edc_task_find_user_payload(struct edc_task *task, const char *mid)
{
    if (!task || !mid || !*mid)
        return NULL;

    /* The in-flight turn first (a message sent moments ago is what a recall right after
     * sending asks for), then the committed ones newest-first. */
    struct edc_turn *active = task->active_turn;
    if (active)
    {
        cJSON *msg = turn_user_message(active);
        if (msg)
        {
            const char *found = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "mid"));
            cJSON *payload = cJSON_GetObjectItem(msg, "payload");
            cJSON *out = (found && strcmp(found, mid) == 0 && payload) ? cJSON_Duplicate(payload, 1) : NULL;
            cJSON_Delete(msg);
            if (out)
                return out;
        }
    }
    for (size_t i = task->turns.tail; i > task->turns.head; i--)
    {
        cJSON *msg = turn_user_message((struct edc_turn *)task->turns.data[i - 1]);
        if (!msg)
            continue;
        const char *found = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "mid"));
        cJSON *payload = cJSON_GetObjectItem(msg, "payload");
        cJSON *out = (found && strcmp(found, mid) == 0 && payload) ? cJSON_Duplicate(payload, 1) : NULL;
        cJSON_Delete(msg);
        if (out)
            return out;
    }
    return NULL;
}

struct edc_task *edc_task_fork(struct edc_task *parent, int64_t task_id, const char *desc,
                               uint32_t fork_point_completion_seq, edc_channel_type channel_type)
{
    if (!parent)
        return NULL;

    /* task_id 0 = "the store assigns one": the caller persists the child and reads the id
     * back (see edc_scheduler_task_create). There is no in-memory id source any more. */
    struct edc_task *child = edc_task_new(task_id, parent->session_id, parent->group_id, desc, channel_type);
    child->forked_from = parent;
    child->fork_point_completion_seq = fork_point_completion_seq;
    /* Like edc_task_new, a child lands at READY (= IDLE's mirror); it no longer forces WAITING_INPUT. */
    return child;
}

/* ── fork snapshot ────────────────────────────────────────── */

struct edc_turn *edc_task_snapshot_turns(struct edc_task *task, int64_t dst_session_id,
                                         int64_t dst_group_id, int64_t dst_task_id,
                                         int dst_turn_ord)
{
    if (!task)
        return NULL;

    /* Messages are written to the store as they are appended, so the destination key must
     * be known up front -- including the ordinal, which is the caller's decision (the
     * destination may already have an in-flight turn at 0). */
    struct edc_turn *snap = edc_turn_new_for(dst_session_id, dst_group_id, dst_task_id, dst_turn_ord);

    for (size_t qi = task->turns.head; qi < task->turns.tail; qi++)
    {
        struct edc_turn *turn = task->turns.data[qi];
        if (!turn)
            continue;

        cJSON *arr = edc_turn_msgs(turn, 0, turn->msg_count);
        if (!arr)
            continue;
        while (cJSON_GetArraySize(arr) > 0)
        {
            cJSON *copy = cJSON_DetachItemFromArray(arr, 0);
            /* A copied message MUST NOT keep the source's identity: messages.id is the
             * primary key, so re-inserting the same id would replace the original row and
             * the parent would lose that message. Dropping the field makes
             * edc_turn_add_message mint a fresh one. */
            cJSON_DeleteItemFromObject(copy, "id");
            edc_turn_add_message(snap, copy);
        }
        cJSON_Delete(arr);
    }

    if (snap->msg_count == 0)
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

            /* Re-decide the dependent NOW. Dropping an edge is the one way a task can lose
             * its last unsatisfied dependency without any task producing -- and nothing else
             * would ever look at it again: it would sit in dep-wait forever, holding its
             * group's front (a waiting task has an active turn), stalling everything queued
             * behind it. Only a task that is still waiting can be released here; a task
             * that already ENDED (BLOCKED included) keeps its state and its reason -- it is
             * resumable, and "the dependency it was blocked on no longer exists" is not the
             * same statement as "it produced". */
            if (edc_task_awaits_deps(dep->target) && dep->target->deps_completed >= dep->target->deps_total)
                edc_task_mark_ready(dep->target); /* the message-end scan drives it from here */
        }
        LL_DELETE(task->dependents, dep);
        free(dep);
    }

    if (task->blocked_by)
    {
        free(task->blocked_by);
        task->blocked_by = NULL;
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

    free(task->description);

    if (task->raw_input)
        sdsfree(task->raw_input);

    if (task->output)
        sdsfree(task->output);
    edc_fold_files_free(task->fold_files);

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
    /* Position IS the storage key. A turn built before its position was known (a fork
     * snapshot, a test that constructs turns directly) is re-keyed here, and its message
     * rows move with it -- rebinding without moving would orphan the conversation. In
     * production the key is already correct at creation, so the move is a no-op. */
    /* Only a turn with a PLACEHOLDER owner is re-keyed. A turn that already names this
     * task was created with the ordinal its caller wanted (a fork snapshot parked at 1
     * next to the in-flight turn at 0), and the queue length is not the right answer for
     * it -- recomputing would move it onto the in-flight turn's rows. */
    if (turn->task_id == 0 || turn->task_id != task->task_id)
    {
        const int ord = (int)edc_queue_length(&task->turns);
        edc_msg_turn_move(turn->task_id, turn->turn_ord, task->session_id, task->group_id,
                          task->task_id, ord);
        edc_turn_set_key(turn, task->session_id, task->group_id, task->task_id, ord);
    }
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
            n += (size_t)turn->msg_count;
    }
    if (task->active_turn)
        n += (size_t)task->active_turn->msg_count;
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
    for (int j = 0; j < turn->msg_count; j++)
    {
        cJSON *msg = edc_turn_msg_at(turn, j);
        if (!msg)
            continue;
        append_user_msg(msg, out);
        cJSON_Delete(msg); /* it is a query result, not the stored instance */
    }
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
        task->last_hist_len = edc_task_group_history_count(task->group);
        task->last_turn_len = task->active_turn ? task->active_turn->msg_count : 0;
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
static void task_record_move(struct edc_task *source, struct edc_task *moved, edc_task_move_kind kind, sds notice);

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
     * stays completed, and the reason is recorded as SKIPPED so that (a) a UI can tell it
     * apart from a stop and (b) `/task resume` refuses it: the branch was not taken.
     * NB a skipped task cancels its own dependents; `a || b ; c` is fine (the `;` edge is a
     * BARRIER and releases c), while `a || b && c` takes c down with b -- unlike a shell,
     * which would pass a's success through the skipped b. */
    if (found && found->type == TASK_DEP_UNLESS)
    {
        if (edc_task_awaits_deps(target))
        {
            edc_task_step step_before = target->step;
            edc_task_life life_before = target->life;
            /* Not a stop: this branch is not taken. The reason says so, so no UI can
             * confuse it with what the user did by hand. */
            edc_task_mark_skipped(target);
            if (target->step != step_before || target->life != life_before)
                task_record_move(source, target, EDC_MOVE_CASCADED, NULL);
        }
        return;
    }

    /* A task that ended BECAUSE a dependency never produced is not a human decision: when
     * THAT dependency produces, put it back in dep-wait so the release just below (and the
     * message-end scan) drive it -- no `/task resume` needed. Two guards keep this from
     * overriding a decision:
     *   - only EDC_END_BLOCKED (a stop, its own failure and a `||` skip stay explicit);
     *   - only when the source completing NOW is the one that blocked it (blocked_by):
     *     some OTHER dependency finishing says nothing about whether it may run again. */
    char source_id_text[32];
    snprintf(source_id_text, sizeof(source_id_text), "%lld", (long long)source->task_id);
    if (target->end == EDC_END_BLOCKED && target->blocked_by && source->task_id > 0 &&
        strcmp(target->blocked_by, source_id_text) == 0 && !edc_task_awaits_deps(target))
    {
        /* The note the cascade wrote ("[failed: its dependency … ]") described a state that
         * is over: the task is about to move again, and leaving it there makes /task output
         * report a failure that no longer happened. Only that machine note is dropped --
         * a real output (a stopped or interrupted task's) is not touched. */
        edc_task_set_output(target, NULL);
        edc_task_step step_before = target->step;
        edc_task_life life_before = target->life;
        edc_task_mark_waiting_deps(target);
        if (target->step != step_before || target->life != life_before)
            /* This one IS announced: the user was told the task had ended, and now it runs
             * again because the dependency it was blocked on produced. A queue advance and
             * a cascade are routine / already announced, so they carry no notice. */
            task_record_move(source, target, EDC_MOVE_UNBLOCKED,
                             sdscatprintf(sdsempty(), "`%lld` is running again — its dependency `%lld` produced.",
                                          (long long)target->task_id, (long long)source->task_id));
    }

    if (target->deps_completed >= target->deps_total && edc_task_awaits_deps(target))
    {
        /* Completion also moves the dependent's phase, so it owes the same
         * bookkeeping and is recorded here (idempotent when nothing moved). */
        edc_task_step step_before = target->step;
        edc_task_life life_before = target->life;
        edc_task_mark_ready(target);
        if (target->step != step_before || target->life != life_before)
            task_record_move(source, target, EDC_MOVE_RELEASED, NULL);
    }
}

/* Side-effect move report (see edc_task.h). */

/* Record a debt: |moved|'s phase was moved as a side effect of an operation
 * |source| started. The debt is charged to the INITIATOR -- the host is about to
 * settle it and clears the list along the way. |notice| (optional) is a line the move
 * deserves and is OWNED by the report; |kind| says why, so the host can decide whether
 * the move is worth telling anyone. */
static void task_record_move(struct edc_task *source, struct edc_task *moved, edc_task_move_kind kind, sds notice)
{
    if (!source || !moved)
    {
        if (notice)
            sdsfree(notice);
        return;
    }
    struct edc_task_move_report *r = malloc(sizeof(*r));
    if (!r)
    {
        if (notice)
            sdsfree(notice);
        return; /* a failed record only loses one notice; no phase changes */
    }
    r->task = moved;
    r->kind = kind;
    r->notice = notice;
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

    /* Released when nothing unsatisfied is left -- not only when nothing is left at all:
     * a task may hold a COMPLETED edge plus this one, and "the last count is zero" then
     * never fires (that task would sit in dep-wait with everything satisfied). */
    if (target->deps_completed >= target->deps_total && edc_task_awaits_deps(target))
    {
        /* Voiding the last invalid dependency moves this task from AWAIT_DEPS to
         * READY -- also a phase change (the sidebar's dep-wait clears), also recorded. */
        edc_task_step step_before = target->step;
        edc_task_life life_before = target->life;
        edc_task_mark_ready(target);
        if (target->step != step_before || target->life != life_before)
            task_record_move(source, target, EDC_MOVE_RELEASED, NULL);
    }
}

bool edc_task_drop_dep(struct edc_task *target, struct edc_task *source)
{
    if (!target || !source)
        return false;
    struct edc_task_dep *dep;
    LL_FOREACH(target->deps, dep)
    {
        if (dep->source != source || dep->completed || dep->cancelled)
            continue;
        /* Same bookkeeping as any other release: void the row and re-decide. `cancelled`
         * is persisted, so the drop survives a restart (the constraint is gone, not the
         * record of it). */
        task_void_dep(dep, target, source);
        return true;
    }
    return false; /* no live edge: nothing was waited for */
}

void edc_task_drain_moves(struct edc_task *task,
                          void (*cb)(void *ctx, struct edc_task *moved, edc_task_move_kind kind, const char *notice),
                          void *ctx)
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
            cb(ctx, r->task, r->kind, r->notice);
        if (r->notice)
            sdsfree(r->notice);
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
     * produces. NOTE: mark_blocked, not send_signal -- this is not a stop by anyone, and
     * the reason has to survive (/task status, the restart hint, and any future
     * auto-release all read it). */
    edc_task_step step_before = target->step;
    edc_task_life life_before = target->life;
    edc_task_mark_blocked(target, EDC_LIFE_CANCELLED, source);
    /* The cascade cancel is also a PHASE CHANGE -- this task now enters a terminal
     * state while the host only named the source task. Record a debt (repeated
     * cancels are idempotent: no real move, no record). */
    if (target->step != step_before || target->life != life_before)
        task_record_move(source, target, EDC_MOVE_CASCADED, NULL);
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
        edc_task_set_output(target, sdscatprintf(sdsempty(), "[failed: its dependency %s %s, %s]", edc_id_str(source->task_id),
                                                 what, why));
    else
        edc_task_set_output(target, sdscatprintf(sdsempty(), "[failed: a dependency %s, %s]", what, why));

    edc_task_mark_blocked(target, EDC_LIFE_ERROR, source); /* edge not detached: see above */
    /* The cascade failure is also a PHASE CHANGE -- this task now enters a terminal
     * state while the host only named the source task. Record a debt (repeated
     * failures are idempotent: no real move, no record). */
    if (target->step != step_before || target->life != life_before)
        task_record_move(source, target, EDC_MOVE_CASCADED, NULL);
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

/* The CANCELLED twin of task_notify_dependents_failed: broadcast a cascade cancel. The
 * list is snapshotted first because a nested cascade writes back into it. Shared by the
 * stop path (send_signal) and by a dependency cascade (mark_blocked) so both keep the
 * recursion the cancel semantics were built on. */
static void task_notify_dependents_cancelled(struct edc_task *task)
{
    size_t n = 0;
    struct edc_task_dep *dep;
    LL_FOREACH(task->dependents, dep)
    n++;
    if (n == 0)
        return;
    struct edc_task **targets = malloc(n * sizeof(*targets));
    if (!targets)
        return; /* losing a notice is better than a partial cascade */
    size_t i = 0;
    LL_FOREACH(task->dependents, dep)
    targets[i++] = dep->target;
    for (size_t k = 0; k < i; k++)
        edc_task_on_dep_cancelled(targets[k], task);
    free(targets);
}

/* ── Signals ───────────────────────────────────────────────── */

bool edc_task_is_running(const struct edc_task *task)
{
    /* Running = ALIVE and awaiting the executor/tool reply; stopped-while-awaiting tools
     * (INTERRUPTED) does not count. */
    return edc_task_awaits_exec(task) || edc_task_awaits_tools(task);
}

void edc_task_send_signal(struct edc_task *task, edc_task_signal sig)
{
    if (!task)
        return;

    task->cancelled_request = 1;

    /* State FIRST, then tell the dependents: a cascade records what became of its source
     * (EDC_END_BLOCKED's blocked_end), so "cancelled" has to be the truth before anyone
     * is told about it. */
    if (sig == TASK_SIGTERM)
    {
        edc_task_mark_cancelled(task);
        /* Cancelled: chasing in-flight tool records is pointless; drop the ledger */
        edc_task_tool_calls_clear(task);
        task_notify_dependents_cancelled(task);
    }
    else
    {
        /* SIGKILL: force cancel -- same ledger drop, no notification (the host already
         * decided nothing behind it should hear about this). */
        edc_task_tool_calls_clear(task);
        edc_task_mark_cancelled(task);
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
        cJSON *m = edc_turn_msg_at(turn, (int)i);
        if (!m)
            continue;
        cJSON *role = cJSON_GetObjectItem(m, "role");
        cJSON *tcid = cJSON_GetObjectItem(m, "tool_call_id");
        const bool hit = role && cJSON_IsString(role) && strcmp(role->valuestring, "tool") == 0 && tcid &&
                         cJSON_IsString(tcid) && strcmp(tcid->valuestring, call_id) == 0;
        cJSON_Delete(m);
        if (hit)
            return true;
    }
    return false;
}

void edc_task_repair_dangling_tool_calls(struct edc_task *task)
{
    struct edc_turn *turn = (task && task->active_turn) ? task->active_turn : NULL;
    if (!turn)
        return;

    /* Scan only the pre-existing messages: tool messages added below are not iterated.
     * Each message is a query result owned here, so every exit path frees it. */
    size_t n = (size_t)turn->msg_count;
    for (size_t i = 0; i < n; i++)
    {
        cJSON *m = edc_turn_msg_at(turn, (int)i);
        if (!m)
            continue;
        cJSON *role = cJSON_GetObjectItem(m, "role");
        cJSON *tcs = cJSON_GetObjectItem(m, "tool_calls");
        if (!role || !cJSON_IsString(role) || strcmp(role->valuestring, "assistant") != 0 || !tcs ||
            !cJSON_IsArray(tcs))
        {
            cJSON_Delete(m);
            continue;
        }

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
            /* If the ledger still holds this one (INTERRUPTED / ERROR path: EXECUTING
             * dispatched, result never returned), remove it too -- it now has a
             * response and must not count as incomplete. */
            edc_task_tool_call_remove(task, id->valuestring);
        }
        cJSON_Delete(m);
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
        n += (size_t)turn->msg_count;
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
        size_t len = (size_t)turn->msg_count;
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
        cJSON *arr = edc_turn_msgs(turn, 0, turn->msg_count);
        if (arr)
        {
            /* Ownership moves into `messages`: the query result is freed below and must
             * not leave references behind. */
            while (cJSON_GetArraySize(arr) > 0)
                cJSON_AddItemToArray(messages, cJSON_DetachItemFromArray(arr, 0));
            cJSON_Delete(arr);
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
