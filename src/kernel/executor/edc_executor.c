/* edc_executor.c -- steps a task from within. See edc_executor.h. */
#include "edc_executor.h"
#include "edc_config.h"
#include "edc_exec_io.h"
#include "edc_llm_error_kind.h"
#include "edc_task_group.h"
#include "edc_turn.h"
#include "log.h"
#include "sds.h"
#include "utlist_fix.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void edc_executor_init(struct edc_executor *ex, const struct edc_executor_ops *ops, void *ctx,
                       const struct edc_config *config)
{
    ex->ops = ops;
    ex->ctx = ctx;
    ex->config = config;
}

/* Read-only environment fact: max_iter is the run budget (it decides whether to
 * continue), so it stays on the advance path rather than behind a port. */
static int exec_max_iter(const struct edc_executor *ex)
{
    return (ex->config && ex->config->max_iter > 0) ? ex->config->max_iter : 0;
}

/* Compact token count for a one-line notice: 980 / 65.5k / 1.0M. */
static void fmt_tokens(long long n, char *buf, size_t cap)
{
    if (n >= 1000000)
        snprintf(buf, cap, "%.1fM", (double)n / 1000000.0);
    else if (n >= 1000)
        snprintf(buf, cap, "%.1fk", (double)n / 1000.0);
    else
        snprintf(buf, cap, "%lld", n);
}

/* The only thing a user can act on once the context no longer fits: the numbers and
 * the next step. `prompt` is what the provider counted (0 = it reported none),
 * `window` comes from the config (0 = unknown), `what` says which of the three
 * overflows this is. Caller owns the returned sds. */
static sds exec_overflow_notice(const struct edc_executor *ex, long long prompt_tokens, const char *what)
{
    char num[32];
    char win[32];
    int window = (ex->config) ? ex->config->context_window : 0;
    if (prompt_tokens > 0)
        fmt_tokens(prompt_tokens, num, sizeof(num));
    else
        snprintf(num, sizeof(num), "unknown");
    if (window > 0)
        fmt_tokens(window, win, sizeof(win));
    else
        snprintf(win, sizeof(win), "unknown");
    return sdscatprintf(sdsempty(),
                        "context overflow (%s): this request held about %s tokens and the model window is %s — "
                        "run /group fold to tighten the context, then retry (or switch to a model with a larger "
                        "window)",
                        what, num, win);
}

/* START: drive a task. The caller does not say whether to send a request or to
 * re-dispatch tools -- the task's own state answers, in this order:
 *   1. fill in dangling tool responses (idempotent);
 *   2. a tool-only task (`!cmd`): dispatch its seeded call, or finish if it is already
 *      answered (it never reaches the LLM branches);
 *   3. PENDING tools in the ledger -> re-dispatch them;
 *   4. an active turn -> send an LLM request;
 *   5. neither -> fall back to ready.
 * The between-tasks part (unsatisfied deps) is the scheduler's gate, checked
 * before driving. Both failure exits name a reason and fail the task, since a
 * host missing a port must not leave phantom "running" tasks. */
static struct edc_advance_result exec_io_failed(struct edc_task *task, const char *reason)
{
    edc_task_set_output(task, sdsnew(reason));
    edc_task_mark_failed(task);
    task->current_request_id = NULL;
    return (struct edc_advance_result){.outcome = EDC_STEP_ERROR, .settled = true, .notice = task->output};
}

/* The one exit a tool-only task has (`!cmd`): its seeded call has been answered (or
 * repaired into an interrupted response), so record the result, share the turn and
 * settle. Not a failure path -- the call ran and the result IS the output.
 *
 * Order matches the STOP branch of exec_done: the turn is handed to the group history
 * BEFORE committing it (committing clears active_turn and the ledger).
 *
 * notice stays NULL: the tool already showed the output through its own TOOL_EMIT, and a
 * second (RESP) copy would be rendered as Markdown -- wrong for command output. */
static struct edc_advance_result exec_tool_only_finish(struct edc_task *task, const char *output)
{
    /* (DONE, COMPLETED) is only reachable from a RUNNING step (AWAIT_EXEC / AWAIT_TOOLS); from
     * READY, or from any non-ALIVE life, the matrix allows only cancel. On the repair path the
     * task may be in either, so step into the running one first (AWAIT_EXEC is the only door). */
    bool running = task->life == EDC_LIFE_ALIVE &&
                   (task->step == EDC_TASKSTEP_AWAIT_EXEC || task->step == EDC_TASKSTEP_AWAIT_TOOLS);
    if (!running && !edc_task_move(task, EDC_TASKSTEP_AWAIT_EXEC, EDC_LIFE_ALIVE))
        return exec_io_failed(task, "[failed: this task cannot be settled (state machine)]");

    edc_task_set_output(task, sdsnew(output ? output : ""));
    edc_task_flush_redirects(task);
    if (task->active_turn)
        edc_task_group_history_add_turn(task->group, task->active_turn);
    edc_task_commit_active_turn(task);
    edc_task_move(task, EDC_TASKSTEP_DONE, EDC_LIFE_COMPLETED);
    return (struct edc_advance_result){.outcome = EDC_STEP_DONE, .settled = true, .notice = NULL};
}

static struct edc_advance_result exec_start(struct edc_executor *ex, struct edc_task *task, struct edc_event *ev)
{
    const struct edc_executor_ops *ops = ex->ops;
    void *req_id = (void *)ev->request_id;

    /* 1. The API rejects a request whose assistant tool_calls lack responses. */
    edc_task_repair_dangling_tool_calls(task);

    /* 2. A tool-only task (`!cmd`): its whole job is the one call it was seeded with, and
     * the LLM is never asked anything -- so this branch replaces the LLM round entirely.
     * (Placed before the general PENDING branch because a tool-only task is never a
     * "continue the conversation" task; the paths do not overlap, this only makes the
     * intent explicit.) */
    if (task->tool_only)
    {
        /* The ledger is empty: the call was already answered. On resume that is the
         * repair above -- an EXECUTING record that lost its result got a synthetic tool
         * response added to the turn. The turn is therefore complete, and the task is
         * over; it must NOT fall through to branch 3 and ask the model. */
        if (!edc_task_has_pending_tool_calls(task))
            return exec_tool_only_finish(task, edc_task_interrupted_tool_note());

        /* Dispatch the seeded call. The state machine routes tool dispatch through
         * AWAIT_EXEC ("READY -> AWAIT_EXEC -> AWAIT_TOOLS, never READY -> AWAIT_TOOLS";
         * see edc_task_is_valid_transition), which is also what this means: the task
         * produces the call, then awaits its result. */
        if (!edc_task_move(task, EDC_TASKSTEP_AWAIT_EXEC, EDC_LIFE_ALIVE) ||
            !edc_task_move(task, EDC_TASKSTEP_AWAIT_TOOLS, EDC_LIFE_ALIVE))
            return exec_io_failed(task, "[failed: this task cannot start its tool call (state machine)]");
        if (!ops->redispatch_tools || ops->redispatch_tools(ex->ctx, task, req_id) <= 0)
            return exec_io_failed(task, "[failed: the tool call could not be dispatched (no toolbox port)]");
        return (struct edc_advance_result){.outcome = EDC_STEP_BLOCKED};
    }

    /* 3. Re-dispatch the ledger's PENDING tools. */
    if (edc_task_has_pending_tool_calls(task))
    {
        if (edc_task_move(task, EDC_TASKSTEP_AWAIT_TOOLS, EDC_LIFE_ALIVE) && ops->redispatch_tools &&
            ops->redispatch_tools(ex->ctx, task, req_id) > 0)
            return (struct edc_advance_result){.outcome = EDC_STEP_BLOCKED};

        /* Cannot re-dispatch: fail rather than stay a "runnable" task the scan
         * would retry forever. */
        return exec_io_failed(task, "[failed: the tool chain could not be re-dispatched (no toolbox port)]");
    }

    /* 4. An active turn is waiting: send an inference request. */
    if (task->active_turn)
    {
        /* Rejected by the state machine: do not send a request nobody will claim. */
        if (!edc_task_move(task, EDC_TASKSTEP_AWAIT_EXEC, EDC_LIFE_ALIVE))
            return exec_io_failed(task, "[failed: this task cannot be moved to AWAIT_EXEC (state machine)]");
        if (!ops->send_llm(ex->ctx, task, req_id))
            return exec_io_failed(task, "[failed: no LLM port is installed, so this task cannot be started]");
        task->llm_rounds++; /* this round starts consuming the run budget */
        return (struct edc_advance_result){.outcome = EDC_STEP_BLOCKED};
    }

    /* 5. Nothing to run: fall back to ready. */
    edc_task_move(task, EDC_TASKSTEP_READY, EDC_LIFE_ALIVE);
    return (struct edc_advance_result){.outcome = EDC_STEP_BLOCKED};
}

/* The task layer's failure class, from the executor's LLM error kind. Deliberately a
 * mapping rather than a shared enum: the task layer must not depend on the LLM package. */
static edc_task_fail exec_llm_fail_class(edc_llm_error_kind kind)
{
    switch (kind)
    {
    case EDC_LLM_ERR_AUTH:
        return EDC_FAIL_AUTH;
    case EDC_LLM_ERR_CONTEXT_OVERFLOW:
        return EDC_FAIL_CONTEXT_OVERFLOW;
    case EDC_LLM_ERR_RATE_LIMIT:
        return EDC_FAIL_RATE_LIMIT;
    case EDC_LLM_ERR_TRANSPORT:
        return EDC_FAIL_TRANSPORT;
    case EDC_LLM_ERR_OTHER:
        break;
    }
    return EDC_FAIL_OTHER;
}

static struct edc_advance_result exec_done(struct edc_executor *ex, struct edc_task *task, struct edc_event *ev)
{
    const struct edc_executor_ops *ops = ex->ops;
    struct edc_event_exec *e = &ev->as.exec;
    void *req_id = (void *)ev->request_id;

    /* Before the branches, so every path (stop, tool_calls, error) counts. */
    edc_task_add_usage(task, e->prompt_tokens, e->completion_tokens, e->total_tokens, e->cached_tokens);
    /* L2: this request's tail notice was read by the model (the response came
     * back), so the watermark advances. A cancelled/failed request never reaches
     * here, so that notice is resent with the next request (idempotent). */
    edc_task_memo_commit(task);

    if (e->failed)
    {
        const char *notice = e->error_message ? e->error_message : "LLM request failed";

        /* A TRANSPORT failure (no HTTP response at all: DNS/TCP/TLS/timeout) is re-sent
         * ONCE automatically -- a transient network blip should not kill the task. Stay
         * in AWAIT_EXEC and re-send the SAME request (same request_id, so the reply still
         * routes back); a second transport failure falls through and fails it. A failure
         * that DID get an HTTP response is never retried here. */
        if (!task->net_retried && edc_task_is_running(task) &&
            edc_llm_error_classify(e->http_status, notice) == EDC_LLM_ERR_TRANSPORT &&
            ops->send_llm(ex->ctx, task, req_id))
        {
            task->net_retried = true;
            task->llm_rounds++; /* the retry consumes a run-budget round */
            log_warn("task %s: LLM transport failure (%s) — re-sending once", task->task_id, notice);
            return (struct edc_advance_result){.outcome = EDC_STEP_BLOCKED};
        }

        task->current_request_id = NULL;
        /* WHY it failed, when that is knowable: a doomed retry (bad key, overflowing
         * context) should say so instead of looking like a fresh start. */
        const edc_task_fail fail = exec_llm_fail_class(edc_llm_error_classify(e->http_status, notice));
        /* Fails and broadcasts to dependents through the task layer's entry. */
        if (edc_task_mark_failed(task))
            edc_task_set_fail(task, fail);
        if (e->content)
            edc_task_set_output(task, sdsnew(e->content));
        edc_task_flush_redirects(task);
        /* Delivery is the host's job: the error text is presentation, not advance. */
        /* (1) the explicit overflow: the provider refused the request because it did
         * not fit. Keep its own words -- they carry the real numbers -- and add the
         * next step. Throttling is classified out in front of this, since "rate
         * limit + fold your context" would be wrong advice. A fold task gets no
         * advice at all: it *is* the fold, and nothing here recurses. */
        if (!edc_task_is_memory_sink(task) &&
            edc_llm_error_classify(e->http_status, notice) == EDC_LLM_ERR_CONTEXT_OVERFLOW)
        {
            sds advice = exec_overflow_notice(ex, e->prompt_tokens, "the provider refused the request");
            edc_task_set_output(task, sdscatprintf(sdsempty(), "%s\n\n%s", notice, advice));
            sdsfree(advice);
            return (struct edc_advance_result){.outcome = EDC_STEP_ERROR, .settled = true, .notice = task->output};
        }
        return (struct edc_advance_result){.outcome = EDC_STEP_ERROR, .settled = true, .notice = notice};
    }

    if (e->reason == EDC_REASON_TOOL_CALL)
    {
        /* A FOLD task has no tools, so a tool call in its response means the provider
         * misbehaved: refuse the summary, keep the group memory, let the next trigger
         * retry. (pi §3.) A CURATION task (SESSION_MEMORY) is deliberately tool-driven
         * and falls through to the normal tool loop
         * below. */
        if (task->sink == EDC_SINK_GROUP_MEMORY)
        {
            task->current_request_id = NULL;
            edc_task_mark_failed(task);
            const bool curate = (task->sink == EDC_SINK_SESSION_MEMORY);
            log_error("%s: task %s asked for a tool call — discarded, nothing landed",
                      curate ? "L2 memo" : "L1 fold", task->task_id ? task->task_id : "(null)");
            return (struct edc_advance_result){
                .outcome = EDC_STEP_ERROR,
                .settled = true,
                .notice = curate
                              ? "Curation attempted to call a tool — discarded, the session memory is unchanged"
                              : "Summarization attempted to call a tool — discarded, the group memory is unchanged"};
        }
        edc_task_move(task, EDC_TASKSTEP_AWAIT_TOOLS, EDC_LIFE_ALIVE);
        if (task->active_turn)
        {
            /* Ownership moves into the turn; NULL the source pointer. */
            edc_turn_add_assistant_message(task->active_turn, e->msg);
            e->msg = NULL;
        }
        /* Register as PENDING before dispatching, since the dispatch takes over
         * the name/args/id. */
        for (int i = 0; i < e->n_calls; i++)
        {
            const struct edc_tool_call *tc = &e->calls[i];
            edc_task_tool_call_add(task, tc->id, tc->name, tc->args);
        }
        /* keep task registered — tool responses use same request_id */
        if (ops->dispatch_tools)
            ops->dispatch_tools(ex->ctx, task, e->tool_calls_raw, req_id);
        return (struct edc_advance_result){.outcome = EDC_STEP_BLOCKED};
    }

    if (e->reason == EDC_REASON_STOP)
    {
        task->current_request_id = NULL;

        if (task->active_turn)
        {
            /* The final reply is rendered as Markdown. */
            if (e->msg)
                cJSON_ReplaceItemInObjectCaseSensitive(e->msg, "render", cJSON_CreateString("markdown"));
            edc_turn_add_assistant_message(task->active_turn, e->msg);
            e->msg = NULL;

            /* Share the turn into group history, except for a sink=GROUP_MEMORY
             * task: its turn is a fold prompt, and mixing it in would feed the
             * next fold the previous prompt and answer. */
            if (!edc_task_is_memory_sink(task))
                edc_task_group_history_add_turn(task->group, task->active_turn);
        }
        edc_task_commit_active_turn(task);
        if (e->content)
            edc_task_set_output(task, sdsnew(e->content));
        edc_task_flush_redirects(task);
        edc_task_move(task, EDC_TASKSTEP_DONE, EDC_LIFE_COMPLETED);

        /* The advance ends here; completion seq, dirty, dependents, delivery,
         * fold and persistence are the host's, in sched_apply_advance. notice
         * borrows e->content and may be NULL. */
        struct edc_advance_result r = {.outcome = EDC_STEP_DONE, .settled = true, .notice = e->content};

        /* (2) the silent overflow: the provider took a request that was already past
         * the window. The task is fine and must complete -- but the next turn will
         * not fit, so the host is told on the side instead of the reply being edited
         * or the task being failed. Only judgeable with a configured window (`0` means
         * unknown, and then there is no claim to make); `prompt_tokens` is compared
         * against it directly, never plus `cached_tokens`, which is a subset of it. */
        int window = (ex->config) ? ex->config->context_window : 0;
        if (window > 0 && e->prompt_tokens > window)
        {
            sds advice = exec_overflow_notice(ex, e->prompt_tokens, "the provider accepted it anyway");
            snprintf(r.warning, sizeof(r.warning), "%s", advice);
            sdsfree(advice);
        }
        return r;
    }

    /* Anything else is ERROR. A truncated summary is the one case worth naming:
     * pi §3 refuses a checkpoint written by a `length` response, because half a
     * summary would silently replace a whole one. The task fails here, so the
     * fold never reaches deliver_memory and the old memory survives. */
    task->current_request_id = NULL;
    edc_task_mark_failed(task);
    const char *notice = "Internal error: unknown finish reason";
    int window = (ex->config) ? ex->config->context_window : 0;

    if (edc_task_is_memory_sink(task))
    {
        /* A summary/curation that could not be finished. A FOLD keeps the old group
         * memory; a CURATION is tool-driven, so anything it already wrote through the
         * memory tools stays (there is no batch to roll back).
         * docs/session-memory-write-path.md). */
        const bool curate = (task->sink == EDC_SINK_SESSION_MEMORY);
        log_error("%s: task %s did not finish cleanly (reason=%d) — stopped early",
                  curate ? "L2 memo" : "L1 fold", task->task_id ? task->task_id : "(null)", (int)e->reason);
        if (e->reason == EDC_REASON_LENGTH)
            notice = curate ? "Curation was truncated (max_tokens) — it stopped early; entries it already wrote remain"
                            : "Summary was truncated (max_tokens) — discarded, the group memory is unchanged";
        else if (e->reason == EDC_REASON_CONTEXT_EXCEEDED)
            notice = curate
                         ? "Curation overflowed the model window — it stopped early; entries it already wrote remain"
                         : "Summarization overflowed the model window — discarded, the group memory is unchanged";
    }
    /* (3) the truncated overflow: `length` with an empty completion is what a full
     * window looks like on providers that do not say the word. A plain `length`
     * (completion > 0) is a real truncation and keeps the generic line. */
    else if (e->reason == EDC_REASON_LENGTH && e->completion_tokens == 0 && window > 0 &&
             (long long)e->prompt_tokens * 100 >= (long long)window * 99)
    {
        sds advice = exec_overflow_notice(ex, e->prompt_tokens, "the answer was cut at max_tokens");
        edc_task_set_output(task, advice);
        notice = task->output;
    }
    /* The provider named the window itself (mapped from `context_length_exceeded`):
     * a signal we used to throw away as "unknown finish reason". */
    else if (e->reason == EDC_REASON_CONTEXT_EXCEEDED)
    {
        edc_task_set_output(task, exec_overflow_notice(ex, e->prompt_tokens, "the provider named the window"));
        notice = task->output;
    }
    return (struct edc_advance_result){.outcome = EDC_STEP_ERROR, .settled = true, .notice = notice};
}

/* Budget exhausted: synthesize a STOP event so the full completion path (turn
 * committed, task COMPLETED, dependents notified, fold and persist) runs. */
static struct edc_advance_result stop_at_iteration_cap(struct edc_executor *ex, struct edc_task *task,
                                                       const char *request_id)
{
    int max_iter = exec_max_iter(ex);

    sds notice = sdscatprintf(
        sdsempty(),
        "[stopped: reached max_iter=%d LLM/tool rounds without a final answer — iteration budget exhausted. "
        "Send a follow-up message to continue.]",
        max_iter);

    cJSON *assistant = cJSON_CreateObject();
    cJSON_AddStringToObject(assistant, "role", "assistant");
    cJSON_AddStringToObject(assistant, "content", notice);

    struct edc_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.kind = EDC_EVENT_EXEC_DONE;
    ev.request_id = request_id;
    ev.as.exec.reason = EDC_REASON_STOP;
    ev.as.exec.content = notice;  /* the STOP branch sdsnew-copies it; it is not held */
    ev.as.exec.msg = assistant;   /* ownership moves to active_turn, like a real response */

    struct edc_advance_result r = edc_executor_advance(ex, task, &ev);

    /* The nested STOP copied the notice into task->output, which outlives this
     * function; point at that and free the local copy. */
    r.outcome = EDC_STEP_ERROR; /* budget exhaustion returns ERROR */
    r.notice = task->output;
    sdsfree(notice);
    return r;
}

static struct edc_advance_result tool_done(struct edc_executor *ex, struct edc_task *task, struct edc_event *ev)
{
    const struct edc_executor_ops *ops = ex->ops;
    const struct edc_event_tool_done *t = &ev->as.tool_done;

    if (task->active_turn && t->result)
        edc_turn_add_tool_message(task->active_turn, t->call_id, t->name, t->result);

    edc_task_tool_call_remove(task, t->call_id);

    if (!t->has_next)
    {
        /* A tool-only task (`!cmd`) exists only to issue the one call it was seeded with,
         * so its result settles the task: the turn is complete (assistant(tool_calls) +
         * this tool message) and the model is never asked to continue. */
        if (task->tool_only)
            return exec_tool_only_finish(task, t->result);

        /* Do not continue the chain once the budget is spent (max_iter <= 0 means
         * unlimited). */
        int max_iter = exec_max_iter(ex);
        if (max_iter > 0 && task->llm_rounds >= max_iter)
        {
            return stop_at_iteration_cap(ex, task, ev->request_id); /* already on a terminal path */
        }

        /* A dependency is still outstanding (the fold barrier held this task): the
         * tools already ran and their results are recorded, so only the next LLM
         * request is held. Park at AWAIT_DEPS -- the scan re-drives it when the dep
         * completes. Not an I/O failure. */
        if (edc_task_has_unresolved_deps(task))
        {
            edc_task_move(task, EDC_TASKSTEP_AWAIT_DEPS, EDC_LIFE_ALIVE);
            return (struct edc_advance_result){.outcome = EDC_STEP_BLOCKED, .changed = true};
        }

        edc_task_move(task, EDC_TASKSTEP_AWAIT_EXEC, EDC_LIFE_ALIVE);
        if (!ops->send_llm(ex->ctx, task, (void *)ev->request_id))
            return exec_io_failed(task, "[failed: no LLM port is installed, so the tool chain cannot continue]");
        task->llm_rounds++; /* sending again after the tool chain consumes another round */
        return (struct edc_advance_result){.outcome = EDC_STEP_BLOCKED};
    }
    return (struct edc_advance_result){.outcome = EDC_STEP_BLOCKED};
}

static struct edc_advance_result tool_started(struct edc_executor *ex, struct edc_task *task, struct edc_event *ev)
{
    const char *call_id = ev->as.call_id;
    if (call_id)
        edc_task_tool_call_mark_executing(task, call_id);
    return (struct edc_advance_result){.outcome = EDC_STEP_BLOCKED};
}

/* SIGNAL: the executor only applies the event to the task; unregistering the
 * in-flight request and persisting are the host's job. */
static struct edc_advance_result exec_signal(struct edc_executor *ex, struct edc_task *task, struct edc_event *ev)
{
    (void)ex;
    edc_task_send_signal(task, ev->as.signal.sig);

    bool cancelled = edc_task_is_cancelled(task);
    return (struct edc_advance_result){.outcome = cancelled ? EDC_STEP_DONE : EDC_STEP_BLOCKED,
                                       .settled = cancelled};
}

struct edc_advance_result edc_executor_advance(struct edc_executor *ex, struct edc_task *task, struct edc_event *ev)
{
    if (!ex || !task || !ev)
        return (struct edc_advance_result){.outcome = EDC_STEP_BLOCKED};

    /* Compare entry and exit so `changed` covers every path, including the signal
     * path that never calls edc_task_move. */
    edc_task_step step_before = task->step;
    edc_task_life life_before = task->life;

    struct edc_advance_result r;
    switch (ev->kind)
    {
    case EDC_EVENT_START:
        r = exec_start(ex, task, ev);
        break;
    case EDC_EVENT_EXEC_DONE:
        r = exec_done(ex, task, ev);
        break;
    case EDC_EVENT_TOOL_DONE:
        r = tool_done(ex, task, ev);
        break;
    case EDC_EVENT_TOOL_STARTED:
        r = tool_started(ex, task, ev);
        break;
    case EDC_EVENT_SIGNAL:
        r = exec_signal(ex, task, ev);
        break;
    default:
        r = (struct edc_advance_result){.outcome = EDC_STEP_BLOCKED};
        break;
    }

    r.changed = (task->step != step_before) || (task->life != life_before);
    return r;
}

/* Boundary adapters: the only place that knows the actor payloads. Passing them as
 * void* keeps the scheduler free of those struct names. */

struct edc_advance_result edc_executor_advance_llm_resp(struct edc_executor *ex, struct edc_task *task, void *request_id,
                                                        void *resp)
{
    struct edc_llm_actor_resp *r = resp;
    if (!r)
        return (struct edc_advance_result){.outcome = EDC_STEP_BLOCKED};

    struct edc_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.kind = EDC_EVENT_EXEC_DONE;
    ev.request_id = request_id;
    ev.as.exec.failed = (r->error != NULL);
    ev.as.exec.error_message = r->error ? r->error->message : NULL;
    /* The status travels with the event: the classifier tells a 400 (overflow) from a
     * 429 (throttling) by it, and the error text alone cannot always do that. */
    ev.as.exec.http_status = r->http_status;
    ev.as.exec.reason = (r->reason == BC_LLM_FINISH_REASON_STOP)                ? EDC_REASON_STOP
                        : (r->reason == BC_LLM_FINISH_REASON_TOOL_CALL)         ? EDC_REASON_TOOL_CALL
                        : (r->reason == BC_LLM_FINISH_REASON_LENGTH)            ? EDC_REASON_LENGTH
                        : (r->reason == BC_LLM_FINISH_REASON_CONTEXT_EXCEEDED)  ? EDC_REASON_CONTEXT_EXCEEDED
                                                                               : EDC_REASON_OTHER;
    ev.as.exec.content = r->content;
    ev.as.exec.msg = r->msg;
    ev.as.exec.tool_calls_raw = r; /* opaque: dispatch_tools receives resp itself */
    ev.as.exec.prompt_tokens = (int)r->usage.prompt_tokens;
    ev.as.exec.completion_tokens = (int)r->usage.completion_tokens;
    ev.as.exec.total_tokens = (int)r->usage.total_tokens;
    ev.as.exec.cached_tokens = (int)r->usage.cached_tokens;

    /* Normalized view for the ledger, FUNCTION entries only; all fields borrowed. */
    struct edc_tool_call *calls = NULL;
    if (r->tool_calls)
    {
        unsigned n = utarray_len(r->tool_calls);
        calls = n ? calloc(n, sizeof(*calls)) : NULL;
        int m = 0;
        for (unsigned i = 0; calls && i < n; i++)
        {
            struct edc_llm_chat_tool_call *tc = utarray_eltptr(r->tool_calls, i);
            if (tc && tc->type == BC_LLM_TOOL_CALL_TYPE_FUNCTION)
            {
                calls[m].id = tc->id;
                calls[m].name = tc->function.name;
                calls[m].args = tc->function.arguments;
                m++;
            }
        }
        ev.as.exec.calls = calls;
        ev.as.exec.n_calls = m;
    }

    struct edc_advance_result out = edc_executor_advance(ex, task, &ev);

    /* Write the possibly-consumed msg back (NULL when taken over); leave
     * r->tool_calls alone. */
    r->msg = ev.as.exec.msg;
    free(calls);
    return out;
}

struct edc_advance_result edc_executor_advance_tool_resp(struct edc_executor *ex, struct edc_task *task, void *request_id,
                                                void *req)
{
    struct edc_toolbox_actor_req *tr = req;
    if (!tr)
        return (struct edc_advance_result){.outcome = EDC_STEP_BLOCKED};

    struct edc_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.kind = EDC_EVENT_TOOL_DONE;
    ev.request_id = request_id;
    ev.as.tool_done.call_id = tr->call_id;
    ev.as.tool_done.name = tr->name;
    ev.as.tool_done.result = tr->result;
    ev.as.tool_done.has_next = (tr->next != NULL);

    return edc_executor_advance(ex, task, &ev);
}

bool edc_executor_view_tool_emit(void *emit_msg, struct edc_tool_emit_view *out)
{
    struct edc_toolbox_emit_msg *emit = emit_msg;
    if (!emit || !out)
        return false;
    out->header = emit->header;
    out->content = emit->content;
    out->render_pref = emit->render_pref;
    return true;
}
