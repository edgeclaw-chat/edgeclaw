#include "edc_llm_request.h"
#include "edc_actor.h"
#include "edc_context.h"
#include "edc_group_fold.h"
#include "edc_llm_actor.h"
#include "edc_scheduler.h"
#include "edc_session.h"
#include "edc_session_memory.h"
#include <stdbool.h>
#include "edc_session_manager.h"
#include "edc_session_store.h"
#include "edc_task.h"
#include "edc_task_group.h"
#include "edc_turn.h"
#include <edc_shell_vars.h>
#include "log.h"
#include "sds.h"
#include "utlist_fix.h"
#include <cJSON.h>
#include <string.h>

/* Splice the dependency material INTO the task's own input: rewrite the active turn's
 * first user message in place -- so it persists with the turn (no longer a derived block
 * that appears in one request only). Returns false when there is no user message to
 * splice into (an abnormal shape; the caller falls back to appending a system message so
 * the material is not lost). */
static bool append_to_task_input(struct edc_task *task, const char *block)
{
    if (!task || !task->active_turn || !block || !*block)
        return false;
    struct edc_turn *turn = task->active_turn;
    for (size_t i = turn->messages.head; i < turn->messages.tail; i++)
    {
        cJSON *msg = turn->messages.data[i];
        const char *role = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "role"));
        if (!role || strcmp(role, "user") != 0)
            continue;
        const char *old = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "content"));
        sds joined = sdscatprintf(sdsempty(), "%s%s%s", old ? old : "", (old && *old) ? "\n\n" : "", block);
        cJSON_ReplaceItemInObject(msg, "content", cJSON_CreateString(joined));
        sdsfree(joined);
        return true;
    }
    return false;
}

/* ── LLM request assembly (moved from src/app/edc_app.c) ────
 * This module has one public entry, edc_llm_request_send(); it holds no assembly-root
 * private state:
 *   - kernel-side inputs (group history / fold watermark / deps / active_turn / todo
 *     throttle / config / session store) all come via sched and task;
 *   - outside kernel there are only two, passed in by the assembly root as parameters
 *     (see edc_llm_request.h).
 * Three invariants: does not know struct edc_app / does not know the toolbox / does not
 * depend back on app.
 *
 * $VAR's "value semantics" ($$ / $LAST / $N / $NAME) are a static resolver private to
 * this file, its data source = sched->sessions (the store handle). The shell side does
 * only two things -- recognize the syntax and set has_deferred_vars
 * (edc_shell_parse.c:88 / edc_shell_ast.c:216); the actual substitution is deferred to
 * here because the values come from other tasks' output. */

static sds edc_llm_var_resolver(const char *name, void *user_data)
{
    struct edc_scheduler *sched = user_data;
    if (!sched)
        return NULL;

    /* $$ -> the active task id */
    if (strcmp(name, "$") == 0)
    {
        struct edc_task *active = edc_session_get_active_task(edc_session_manager_get_active(sched->sessions));
        return active ? sdsnew(active->task_id) : NULL;
    }

    /* $LAST -> the last completed task's output */
    if (strcmp(name, "LAST") == 0)
    {
        struct edc_task *last = NULL;
        uint32_t highest_seq = 0;
        struct edc_task_group *g;
        struct edc_session *active = edc_session_manager_get_active(sched->sessions);
        DL_FOREACH(active ? active->groups : NULL, g)
        {
            struct edc_task *t;
            LL_FOREACH2(g->members, t, group_next)
            {
                if (edc_task_is_completed(t) && t->completion_seq > highest_seq)
                {
                    highest_seq = t->completion_seq;
                    last = t;
                }
            }
        }
        return (last && last->output) ? sdsdup(last->output) : NULL;
    }

    /* $NUMBER or $NAME -> look up by task id / job number (global; lazily activates the owning session if needed) */
    struct edc_task *t = edc_session_store_resolve_task(sched->sessions, sched, name, NULL);
    return (t && t->output) ? sdsdup(t->output) : NULL;
}

/* ── Callback: send an LLM request ─────────────────────────── */

/* Ensure the group-history slice start does not land on a tool message. OpenAI requires
 * every role=tool message to be preceded by a matching assistant(tool_calls); a window
 * start inside an assistant(tool_calls)+tool sequence orphans the tool message and
 * errors with "Messages with role 'tool' must be a response to a preceding message with
 * 'tool_calls'". Move the start back to the first non-tool message (its assistant),
 * keeping the sequence whole. */
static size_t edc_llm_history_safe_start(cJSON *history, size_t start, size_t lower_bound)
{
    while (start > lower_bound)
    {
        cJSON *m = cJSON_GetArrayItem(history, start);
        cJSON *role = cJSON_GetObjectItem(m, "role");
        if (role && cJSON_IsString(role) && strcmp(role->valuestring, "tool") == 0)
        {
            start--;
        }
        else
        {
            break;
        }
    }
    return start;
}

void edc_llm_request_send(struct edc_scheduler *sched, struct edc_task *task, cJSON *tool_schemas,
                          edc_actor_t *llm_actor, void *request_id)
{
    if (!sched || !task || !llm_actor)
        return;

    /* Group-level context: the system part (group memory + system prompt, pristine, no
     * todo) is assembled by edc_context_append_system_part; the rest is all unfolded
     * history + task-specific material (dep outputs / $VAR expansion). The todo reminder
     * is not in the head -- nor sent every request: it is throttled by
     * edc_task_llm_request_poke and generated only when EDC_TODO_SYNC_REMIND_GAP requests
     * have passed without a todowrite, recomputed via edc_context_make_todo_reminder and
     * appended at the absolute tail (req->tail_system; the llm actor appends after
     * history + the current turn). */
    cJSON *msgs = cJSON_CreateArray();

    /* A sink = GROUP_MEMORY fold task wants ONE self-contained summary call: the fold
     * instruction (system) + its own turn (the fold prompt), nothing else. So it does not
     * use group context -- especially not group history[0]: that slot is not persisted
     * and would be refilled with the main prompt at load after a restart
     * (edc_task_group_rebuild_history), loading the summarizer with EdgeClaw's persona /
     * MEMORY.md / tool instructions it has no tools for. The fold prompt carries all its
     * corpus and needs no group history. */
    const bool is_memory_sink = edc_task_is_memory_sink(task);

    sds pristine = NULL;
    /* The owning session's workdir: when a background session's task resumes, {{cwd}} in
     * the system prompt must be its own directory (no owner => no directory; do not
     * guess "the current one"). */
    struct edc_session *own = (task->group && task->group->session) ? task->group->session : NULL;
    const char *workdir = own ? edc_session_get_workdir(own) : NULL;
    if (is_memory_sink)
    {
        cJSON *sys = cJSON_CreateObject();
        cJSON_AddStringToObject(sys, "role", "system");
        cJSON_AddStringToObject(sys, "content", task->sink == EDC_SINK_SESSION_MEMORY
                                                    ? edc_session_memory_system_prompt()
                                                    : edc_group_fold_system_prompt());
        cJSON_AddItemToArray(msgs, sys);
    }
    else
    {
        edc_context_append_system_part(msgs, sched->config, task->group, workdir, &pristine);
    }

    /* The pristine prompt built on the fly when the group has no history: write it back
     * to group history[0] (shared by all the group's tasks, kept pristine). */
    if (pristine)
    {
        if (task->group)
        {
            cJSON *sys_hist = cJSON_CreateObject();
            cJSON_AddStringToObject(sys_hist, "role", "system");
            cJSON_AddStringToObject(sys_hist, "content", pristine);
            /* the group system prompt belongs to no task */
            edc_task_group_history_add_owned(task->group, sys_hist);
        }
        sdsfree(pristine);
    }

    if (!is_memory_sink && task->group && cJSON_GetArraySize(task->group->history) > 0)
    {
        size_t total = cJSON_GetArraySize(task->group->history);
        size_t end = total;

        /* Unfolded-history start: skip the system prompt (history[0]). Folded messages
         * are not in history -- they are pruned at fold landing (see
         * edc_task_group_rebuild_history) -- so no watermark arithmetic is needed here. */
        size_t hist_begin = 1;
        if (hist_begin > end)
            hist_begin = end;
        /* A watermark boundary can also land on a tool message; move back to a safe start */
        hist_begin = edc_llm_history_safe_start(task->group->history, hist_begin, 1);

        /* Inject the whole unfolded history [hist_begin, end): the fold uses a
         * "half" strategy (see edc_group_fold's L1 fold) and compresses only the oldest
         * half when triggered, so the remaining unfolded messages are exactly the recent
         * detail the model should see -- inject them all, no truncation. The request
         * size ceiling comes from config's context_window (fold trigger = window -
         * reserve; triggering compresses it back). */
        for (size_t i = hist_begin; i < end; i++)
        {
            cJSON_AddItemReferenceToArray(msgs, cJSON_GetArrayItem(task->group->history, i));
        }
    }

    /* Inject completed dependency tasks' output (user inputs + final output) */
    struct edc_task_dep *dep;
    LL_FOREACH(task->deps, dep)
    {
        /* Print completed AND cancelled: a voided edge (source terminal) has one field
         * false and the other true; reading completed alone would misread it as "still
         * waiting". */
        log_info("send_llm task=%s dep src=%s completed=%d cancelled=%d output=%s", task->task_id,
                 dep->source ? dep->source->task_id : "(null)", dep->completed, dep->cancelled,
                 (dep->source && dep->source->output) ? "yes" : "none");
        if (dep->completed && dep->source && !dep->injected && dep->type == TASK_DEP_JOIN)
        {
            dep->injected = true; /* already spliced -- the material is in the input, not spliced again */

            /* Splice the material INTO the target's own input rather than inserting a
             * pair of messages into the request. Rewrite the active turn's first user
             * message in place => it persists with the turn and reads more directly:
             * "my input carries the subtask's output". (Locality: only a JOIN hung at
             * birth reaches here -- the target is never driven while the edge is
             * unsatisfied.)
             * A memory-type source (fold / curation) has as "input" the whole corpus (a
             * fold prompt can be hundreds of KB) that says the same thing as its output
             * => splice only its output (for L2 curation, the just-landed group
             * memory). */
            if (dep->source->output)
            {
                const bool source_is_memory = edc_task_is_memory_sink(dep->source);
                sds block = source_is_memory
                                ? sdsnew(dep->source->output)
                                : sdscatprintf(sdsempty(), "[subtask %s result]:\n%s",
                                               dep->source->description ? dep->source->description : "(task)",
                                               dep->source->output);
                if (!append_to_task_input(task, block))
                {
                    log_warn("  -> task %s has no input message to splice into; appending a system message instead",
                             task->task_id ? task->task_id : "(null)");
                    cJSON *sys = cJSON_CreateObject();
                    cJSON_AddStringToObject(sys, "role", "system");
                    cJSON_AddStringToObject(sys, "content", block);
                    cJSON_AddItemToArray(msgs, sys);
                }
                sdsfree(block);
            }
            log_info("  -> spliced the output of task %s into the task input", dep->source->task_id);
        }
    }

    /* expand deferred $VAR references in the current turn's user messages */
    if (task->has_deferred_vars && task->active_turn)
    {
        for (size_t mj = task->active_turn->messages.head; mj < task->active_turn->messages.tail; mj++)
        {
            cJSON *msg = task->active_turn->messages.data[mj];
            cJSON *role = cJSON_GetObjectItem(msg, "role");
            if (role && cJSON_IsString(role) && strcmp(role->valuestring, "user") == 0)
            {
                cJSON *content = cJSON_GetObjectItem(msg, "content");
                if (content && cJSON_IsString(content))
                {
                    sds expanded = edc_shell_expand_vars(content->valuestring, edc_llm_var_resolver, sched, NULL);
                    if (expanded)
                    {
                        cJSON_ReplaceItemInObject(msg, "content", cJSON_CreateString(expanded));
                        sdsfree(expanded);
                    }
                }
            }
        }
        task->has_deferred_vars = 0;
    }

    /* A sink = GROUP_MEMORY system task (fold) wants ONE clean summary call: its
     * "mirror" is the system group head (the fold instruction) + its own turn (the fold
     * prompt). So it gets no tool definitions -- the summarizer has no tools and must not
     * be dragged into a tool loop (the old implementation sent an empty tools field; the
     * only reason for this difference is keeping that request byte-comparable). */
    cJSON *tools = (!is_memory_sink) ? tool_schemas : NULL;
    struct edc_llm_actor_req *req = edc_llm_actor_req_new(msgs, tools,
                                                          task->active_turn ? &task->active_turn->messages : NULL);
    /* The summary's output budget (a): paired with the corpus cap
     * EDC_FOLD_MAX_CORPUS_TOKENS, keeping a single fold's compression ratio achievable
     * (<= 4:1). This is the only exception to "requests never send max_tokens". */
    if (is_memory_sink)
        req->max_tokens = (task->sink == EDC_SINK_SESSION_MEMORY) ? EDC_SESSION_MEMORY_MAX_TOKENS
                                                                  : EDC_FOLD_SUMMARY_MAX_TOKENS;
    /* If this turn's prefix is already in memory (a fold cut inside the turn), send from after it. */
    if (task->active_turn && task->active_turn->folded_prefix > 0)
        req->turn_begin = (size_t)task->active_turn->folded_prefix;
    req->history_owned = true;
    /* Absolute tail: the todo reminder -- no longer every request (avoids nagging); only
     * generated when EDC_TODO_SYNC_REMIND_GAP requests have passed without a todowrite
     * (edc_task_llm_request_poke returns true at the threshold and has reset the count).
     * Two parts: CURRENT TASK SYSTEM STATE + INSTRUCTIONS FOR THIS TURN; a list => rows +
     * cadence, none => "no list yet" + planning guide. The llm actor appends it after
     * history + turn, so every request carries the latest state. req owns it and frees it
     * with the request (a copy; task/group untouched). */
    req->tail_parts = NULL;
    if (!is_memory_sink)
    {
        /* [0] L2 session-memory notice: entries created/updated since this task last
         * REALLY read it, one per line (cap 10). A request-time temporary -- written into
         * no message array, not persisted; the watermark advances after the response via
         * edc_task_memo_commit (cancel/failure do not advance => resent). No memory
         * content is resident (the system prompt keeps one constant hint line, see
         * edc_context.c). */
        struct edc_session *own = (task->group && task->group->session) ? task->group->session : NULL;
        if (own && own->memo)
        {
            uint64_t max_injected = 0;
            sds notice = edc_session_memory_notice(own->memo, task->memo_seen_seq, 10, &max_injected);
            if (notice)
            {
                cJSON *sys = cJSON_CreateObject();
                cJSON_AddStringToObject(sys, "role", "system");
                cJSON_AddStringToObject(sys, "content", notice);
                if (!req->tail_parts)
                    req->tail_parts = cJSON_CreateArray();
                cJSON_AddItemToArray(req->tail_parts, sys);
                task->memo_notice_seq = max_injected; /* promoted to memo_seen_seq only after the response */
                sdsfree(notice);
            }
        }
        /* [1] todo reminder (throttled): the cadence for "a user task at work". A summary
         * call must not get it (again, to keep the fold request byte-comparable). */
        if (edc_task_llm_request_poke(task))
        {
            cJSON *todo = edc_context_make_todo_reminder(task);
            if (todo)
            {
                if (!req->tail_parts)
                    req->tail_parts = cJSON_CreateArray();
                cJSON_AddItemToArray(req->tail_parts, todo);
            }
        }
    }

    edc_actor_send(llm_actor,
                   edc_actor_msg_new(BC_ACTOR_MSG_LLM_REQUEST, edc_scheduler_get_actor(sched), request_id, req,
                                     (edc_actor_data_free_fn)edc_llm_actor_req_free));
}

/* ── Ports: the request assembler provides `send_llm` ────────
 *
 * How the body is assembled and whom it is posted to is this file's knowledge (see
 * edc_llm_request.h), so "install the port onto the scheduler" is here too -- the
 * provider wires it. The two values the kernel cannot get (tool definitions + the LLM
 * endpoint mailbox) are handed in once at assembly time; every request then leaves from
 * here and the assembly root never appears on a runtime path. Port signatures unchanged.
 *
 * Both values live for the process (created and freed by the assembly root, read at
 * runtime only). */
static edc_actor_t *g_llm_actor = NULL;
static cJSON *g_tool_schemas = NULL;

static void edc_llm_request_cb_send_llm(struct edc_scheduler *sched, struct edc_task *task, void *request_id)
{
    edc_llm_request_send(sched, task, g_tool_schemas, g_llm_actor, request_id);
}

void edc_llm_request_bind_scheduler(struct edc_scheduler *sched, edc_actor_t *llm_actor, cJSON *tool_schemas)
{
    if (!sched)
        return;

    g_llm_actor = llm_actor;
    g_tool_schemas = tool_schemas;
    sched->send_llm = edc_llm_request_cb_send_llm;
}
