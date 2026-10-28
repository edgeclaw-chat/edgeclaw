#ifndef __BC_TASK_H__
#define __BC_TASK_H__

#include "edc_config.h"
#include "edc_containers.h"
#include "edc_fold_files.h"
#include "edc_task_types.h"
#include "edc_turn.h"
#include "sds.h"
#include <cJSON.h>
#include <inttypes.h> /* PRId64 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stddef.h>

struct edc_session;
struct edc_config;
struct edc_task;
struct edc_task_move_report;

/* "Where is it" (step) and "is it alive" (life) are two axes kept apart, so a
 * phase survives being persisted instead of being coarse-grained into one value.
 * A stopped task keeps its step: (AWAIT_TOOLS, INTERRUPTED) is not running and is not
 * awaiting tools. The two are orthogonal. */
typedef enum
{
    EDC_TASKSTEP_READY,          /* ready to run */
    EDC_TASKSTEP_AWAIT_DEPS,     /* waiting on dependencies */
    EDC_TASKSTEP_AWAIT_EXEC,     /* waiting on the executor's LLM reply */
    EDC_TASKSTEP_AWAIT_TOOLS,    /* waiting on tool results */
    EDC_TASKSTEP_AWAIT_RESOURCE, /* waiting on a resource slot (reserved) */
    EDC_TASKSTEP_DONE,           /* this step is over */
} edc_task_step;

typedef enum
{
    EDC_LIFE_ALIVE,
    /* The loop stopped while this was running: the process went down mid-flight and no
     * decision has been made about it yet. Like ERROR it can be resumed, and like ERROR
     * it propagates to its dependents (see edc_task_mark_interrupted). It never holds the
     * front: it will not advance on its own. */
    EDC_LIFE_INTERRUPTED,
    EDC_LIFE_COMPLETED,
    EDC_LIFE_CANCELLED,
    EDC_LIFE_ERROR,
} edc_task_life;

extern const char *edc_task_step_name(edc_task_step step);
extern const char *edc_task_life_name(edc_task_life life);
extern edc_task_step edc_task_classify_step(const char *name);
extern edc_task_life edc_task_classify_life(const char *name);

/* WHY a task is over. |life| says which terminal bucket it landed in; |end| says what put
 * it there -- the fact the coarse buckets cannot carry, because they are not one thing:
 * CANCELLED is "the user stopped it" OR "a `||` branch was skipped" OR "a dependency was
 * cancelled", and ERROR is "it failed by itself" OR "a dependency never produced". Without
 * this axis every consumer (sidebar, /task status, the restart hint, any future
 * auto-release of cascaded work) has to guess, and the guess differs per caller.
 *
 * Invariant: EDC_END_NONE exactly while the task can still move on its own (ALIVE), and
 * for a COMPLETED one (it ended by succeeding, which the life already says). Every move
 * back to ALIVE clears the reason -- resume therefore wipes the slate without each writer
 * having to remember it.
 *
 * The enum itself lives in edc_task_types.h (the shell<->app seam); see edc_task_end. */

extern const char *edc_task_end_name(edc_task_end end);
extern edc_task_end edc_task_classify_end(const char *name);

/** Record WHY a failure happened (EDC_FAIL_NONE ignores). Only call it on a task that is
 *  in EDC_END_FAILED; any move back to ALIVE clears it with the rest of the reason. */
extern void edc_task_set_fail(struct edc_task *task, edc_task_fail fail);

/* Two more orthogonal axes: where the output goes, and whether the task must
 * survive a restart. Defaults (CHANNEL, PERSISTENT) describe a user task; only a
 * system task flips them, and that combination is the whole reason a fold needs
 * neither a private protocol nor a hidden flag. */
typedef enum
{
    EDC_SINK_CHANNEL,        /* output goes back to the user */
    EDC_SINK_GROUP_MEMORY,   /* output becomes the group's memory (L1 fold) */
    EDC_SINK_SESSION_MEMORY, /* output becomes session memory entries (L2 curation) */
} edc_sink;

extern const char *edc_task_sink_name(edc_sink sink);
extern void edc_task_memo_commit(struct edc_task *task);
extern edc_sink edc_task_classify_sink(const char *name);

/* Which kind of SESSION_MEMORY round a system task is. All three share the sink and
 * the memory tools, so the request picks the system prompt by this tag, and the
 * landing step dispatches on it. Three states replaced the old `memo_compact` bool
 * (which could only say "compact vs everything else"). */
typedef enum
{
    EDC_MEMO_KIND_CURATE = 0, /* fold-chained curation (the default, and what an old
                               * file without the key reads back as) */
    EDC_MEMO_KIND_COMPACT,    /* entry-cap compaction: make the memory smaller */
    EDC_MEMO_KIND_EXTRACT,    /* idle group's unfolded history -> session memory */
} edc_memo_kind;
extern const char *edc_task_memo_kind_name(edc_memo_kind kind);
extern edc_memo_kind edc_task_classify_memo_kind(const char *name);

/* Its output is not for the user but written into some memory (fold / curation).
 * The shared shape of these tasks: a self-contained request (its own description +
 * its own turn), no completion order (`completion_seq = 0`), not part of the
 * conversation (excluded from group history / fold corpus / ctx width). */
extern bool edc_task_is_memory_sink(const struct edc_task *task);


typedef enum
{
    TASK_SIGTERM, /* graceful cancel: cancel in-flight requests, notify dependents */
    TASK_SIGKILL, /* force cancel: no notification, deps_total untouched */
} edc_task_signal;

/* edc_task_dep_type is defined in edc_task_types.h */

struct edc_task_dep
{
    struct edc_task *source;
    struct edc_task *target; /* the dependent task (this) */
    edc_task_dep_type type;
    bool completed;
    bool cancelled;
    bool injected;
    struct edc_task_dep *next;
};

/* Resume ledger of tool calls that have not finished. List order is execution
 * order, so at most the head can be EXECUTING; a record is removed as soon as its
 * result arrives. */
typedef enum
{
    TOOL_CALL_PENDING = 0,   /* not dispatched */
    TOOL_CALL_EXECUTING = 1, /* dispatched, no result yet */
} edc_tool_call_status;

struct edc_tool_call_record
{
    sds call_id; /* LLM-side tool_call_id, for matching the response */
    sds name;
    cJSON *args; /* kept to rebuild the request on resume; may be NULL */
    edc_tool_call_status status;
    struct edc_tool_call_record *next;
};

/* Render an id for a log line ("%s ...", edc_id_str(task->task_id)).
 *
 * Ids are numbers now and the log format strings are all %s, so this keeps those calls
 * unchanged instead of rewriting every message to %lld. A small rotating buffer works
 * because a single log call never needs more than a few ids at once; it is NOT thread
 * safe, and logging already assumes that. */
static inline const char *edc_id_str(int64_t id)
{
    static char bufs[4][24];
    static int next;
    char *b = bufs[next];
    next = (next + 1) % 4;
    if (id > 0)
        snprintf(b, sizeof(bufs[0]), "%" PRId64, id);
    else
        snprintf(b, sizeof(bufs[0]), "(none)");
    return b;
}

struct edc_task
{
    /* Numeric ids. The database assigns task_id (INTEGER PRIMARY KEY on INSERT) and
     * group_id is the leader's task_id, so both are integers. They used to be strings from
     * a process-wide counter whose high-water mark had to be persisted in .task_seq --
     * an open/write/rename per allocation, for something SQLite does on INSERT.
     * 0 = not assigned yet (a task under construction, or "no such group"). */
    int64_t task_id;    /* PID */
    int64_t group_id;   /* PGID -- the leader's task_id */
    int64_t session_id; /* SID: the sessions table key, a number like task_id */

    char *description; /* user-visible */
    /* The raw text the user typed, before the shell parser (the parsed form is
     * the turn's first user message). Kept so the kernel can bind a task-created
     * event to that echo; memory-only. NULL when the task was not created from a
     * user input (fold's system task, a fork child, a test fixture). */
    sds raw_input;
    /* The single source of truth for "where is this task". */
    edc_task_step step;
    edc_task_life life;

    edc_sink sink;

    int is_group_leader;
    /* Created as a background task (`&` / `--bg`): it runs without taking the screen, and
     * it keeps that property across a restart -- resuming it must not put it on screen
     * either. Persisted. */
    bool is_background;

    /* A "tool-only" task (`!cmd`): its whole job is to issue the one tool call it was
     * created with. Its step producer is the tool chain, never the LLM -- when that
     * call's result is in, the task settles instead of asking the model to continue.
     * Persisted: after a restart the same task must not suddenly turn into an LLM round
     * (the executor reads this flag on both the resume and the tool-done path). */
    bool tool_only;

    /* This system task is a SESSION_MEMORY round (sink SESSION_MEMORY): curation,
     * compaction or extraction -- they share the sink and the tools, so the request
     * picks the system prompt by this tag and the landing step dispatches on it.
     * Persisted (tri-state; see edc_memo_kind). */
    edc_memo_kind memo_kind;

    struct edc_task_group *group;

    EdcQueue turns;               /* FIFO: struct edc_turn* */
    struct edc_turn *active_turn; /* the turn in progress */

    void *current_request_id; /* request_id sent to an actor */
    int cancelled_request;    /* request marked cancelled */

    /* LLM requests issued for the current turn, incremented by the scheduler on
     * every send_llm. At max_iter the chain is no longer continued. Reset to 0
     * when a new input starts. max_iter <= 0 means unlimited. */
    int llm_rounds;

    /* One automatic re-send after a TRANSPORT failure (no HTTP response at all:
     * DNS/TCP/TLS/timeout). Set on the first retry, so a persistent outage still fails
     * the task instead of looping. In-memory only (not persisted). */
    bool net_retried;

    /* Usage accumulated over the task's whole lifetime, for the sidebar; not used
     * for scheduling. */
    long long tokens_prompt;
    long long tokens_completion;
    long long tokens_total;
    long long tokens_cached;

    /* How big the conversation had grown after the most recent reply: that
     * response's usage.total_tokens (its prompt *and* its completion) -- what was
     * sent plus what came back. Deliberately NOT accumulated: it answers "how big
     * is this conversation now", while the counters above answer "what has it
     * cost so far". 0 = no response with usage has come back yet.
     *
     * It is the provider's count for that round trip, not a measurement of a
     * request: it is a sum of two counts, so a provider that folds reasoning
     * content into completion_tokens (which need not be sent back) makes it a
     * slight over-estimate of what the next request will carry. */
    long long last_total_tokens;

    /* Where that reply sat in the group's context, so the tail appended after it
     * can be estimated instead of guessed: the group history length and the
     * active turn's message count when the reply came back, and the group's fold
     * epoch then.
     *
     * Two ways an anchor is unusable, both falling back to estimating the whole
     * context: its epoch is behind the group's (a fold landed after it, so it
     * counted a prefix that no longer gets sent), or hist_len == 0 (no position
     * recorded: an older file read back, or no reply yet). */
    uint32_t last_fold_epoch;
    int last_hist_len;
    int last_turn_len;

    /* ── L2 (session memory): watermarks ─────────────────────────
     * `memo_seen_seq` = how far this task has "seen" the session memory event
     * table. Assembly takes the `seq > memo_seen_seq` difference as a patch:
     * it advances in the bookkeeping after a request
     * is sent (cancel/failure do NOT advance, so the same patch is resent).
     *
     * Birth point adopted from the session: `memo_seen_seq = session->memo->next_seq`
     * -- a new task gets the full index on its first round, so a "patch" says
     * nothing to it. Old archives (migration) read back 0, and the migrated table
     * is empty too (next_seq = 0), so both agree.
     *
     * `last_prefix_tokens` = estimated tokens of the session memory block + patch
     * at anchor time (P4b onward): the ctx estimate adds
     * `max(0, prefix_now - last_prefix_tokens)`, else a growing resident block folds
     * systematically late. P4a only persists this field, no readers yet. */
    uint64_t memo_seen_seq;
    uint64_t last_prefix_tokens;

    /* Largest event seq the last request INCLUDED (in-memory only). It is promoted
     * to `memo_seen_seq` only once the response really returns
     * (`edc_task_memo_commit`) -- a cancelled/failed request loses its notification,
     * the watermark stays, and the next request resends the same entry. */
    uint64_t memo_notice_seq;

    edc_channel_type channel_type;

    cJSON *todos; /* JSON Array */

    /* LLM requests since the last successful todowrite; at the threshold a single
     * system reminder is appended to the request and the counter resets. */
    int todo_sync_gap;

    struct edc_task *forked_from;       /* parent (NULL = not a fork) */
    uint32_t fork_point_completion_seq; /* parent's completion watermark at the fork point */

    struct edc_task_dep *deps;       /* whom I depend on */
    struct edc_task_dep *dependents; /* who depends on me */
    size_t deps_total;
    size_t deps_completed;
    sds output; /* final output, injected into a JOIN dependent */

    uint32_t completion_seq; /* completion order (0 = not finished) */
    int has_deferred_vars;   /* {{…}} task references to expand */

    /* Why it ended (see edc_task_end). EDC_END_NONE while it can still move, and for a
     * task that ended by succeeding. Cleared by any move back to ALIVE. */
    edc_task_end end;
    /* EDC_END_BLOCKED only: the dependency that never produced, and what THAT one's end
     * was (so "blocked by 7 (interrupted)" survives 7 being deleted later). Informational:
     * nothing branches on them yet. */
    char *blocked_by;
    edc_task_end blocked_end;

    /* WHY it failed, when that is knowable (only with end == EDC_END_FAILED): a doomed
     * retry must be distinguishable from a transient one. Cleared with `end`. */
    edc_task_fail fail;

    struct edc_tool_call_record *tool_calls;

    sds redirect_stdout;
    sds redirect_stderr;
    bool redirect_append; /* true = >>, false = > */
    /* The same for stderr (`2>>`). `redirect_append` above is shared by whichever stream is
     * redirected; a line that appends to BOTH streams needs to say it per stream. */
    bool redirect_stderr_append;

    /* Output target for a sink=GROUP_MEMORY task: which group's memory to write,
     * and where this round's cut went (P2a-4). The cut is one point in the group's
     * content order: the member it falls in, its turn, and how many leading messages
     * of that turn the memory covers. Everything before it is folded whole, nothing
     * after it is touched. A NULL task id means "no cut" (a fold task written by an
     * older binary): the landing then has nothing to apply and reports it.
     * Filled by the folding side; the task itself does not interpret them. */
    struct edc_task_group *sink_group;
    int64_t fold_cut_task_id; /* 0 = none */
    uint32_t fold_cut_turn;
    uint32_t fold_cut_prefix;
    /* Files named by the tool calls in THIS fold's corpus, collected while the prompt is
     * built and merged into the group's list at landing (R5b "file tracking"). A fold drops
     * the turns it summarizes, so without this the model keeps a narrative of what happened
     * but loses the list of files it read and changed. Owned by the task. */
    struct edc_fold_files *fold_files;

    /* Where one EXTRACT round's cut went, exactly like the fold cut above: the member
     * it falls in, that member's turn index, and how many leading messages of that
     * turn the round took. Everything earlier in content order is covered whole;
     * nothing later is touched. Only committed turns are ever cut (extraction never
     * reads an in-flight turn), so there is no active-turn sentinel. A NULL task id
     * means no cut was recorded (the landing then advances nothing and counts one
     * failure). Filled by the extraction side; the task does not interpret them. */
    int64_t memo_cut_task_id; /* 0 = none */
    uint32_t memo_cut_turn;
    uint32_t memo_cut_prefix;

    /* Tasks a cancel or a dependency change moved as a side effect (cascaded JOIN,
     * released SEQUENCE). The task layer does not know the host, so it records them
     * here for the host to settle through its unified entry. Memory-only. */
    struct edc_task_move_report *moved_report;

    struct edc_task *group_next;  /* next member within the group */
    struct edc_task *prev, *next; /* session-wide list */
};

extern struct edc_task *edc_task_new(int64_t task_id, int64_t session_id, int64_t group_id,
                                     const char *desc, edc_channel_type channel_type);
/* Park the frontend's folded-paste map on this task's own user message, as a `payload`
 * object inside the message JSON. It rides with the message, so fork (cJSON_Duplicate in
 * edc_task_snapshot_turns) and the state serializer carry it without knowing about it.
 *
 * The bytes are for the store and the UI, NOT for the model: the LLM request builder
 * strips the field when it puts a message on the wire (edc_llm_actor.c). |payload| empty
 * or no own user message = nothing happens. */
struct edc_payload;
/* Park the frontend's user-message identity (`mid`) and its folded-paste map (`payload`) on
 * this task's own user message, as `mid` / `payload` fields of the message JSON. Both ride
 * with the message, so fork (cJSON_Duplicate in edc_task_snapshot_turns) and the state
 * serializer carry them without knowing about them.
 *
 * They are for the store and the UI, NOT for the model: the LLM request builder strips
 * them when a message goes on the wire (edc_llm_actor.c). |payload| empty and |mid| NULL =
 * nothing happens. */
extern void edc_task_set_user_origin(struct edc_task *task, const char *mid, const struct edc_payload *payload);

/** The folded-paste map parked on this task's user message with |mid|, or NULL.
 *  Read-only scan of the committed turns plus the in-flight one -- what the "payload by
 *  mid" pull answers from. RETURNS A COPY the caller must cJSON_Delete. */
extern cJSON *edc_task_find_user_payload(struct edc_task *task, const char *mid);

/** The folded-paste map of this task's own user message, or NULL. Used by the
 *  frontend to show the line the way it was typed: the message text is the expanded one.
 *  RETURNS A COPY the caller must cJSON_Delete. */
extern cJSON *edc_task_user_payload(struct edc_task *task);

extern struct edc_task *edc_task_fork(struct edc_task *parent, int64_t task_id, const char *desc,
                                      uint32_t fork_point_completion_seq, edc_channel_type channel_type);
extern void edc_task_free(struct edc_task *task);

/** Deep-copy all of the task's turn messages into one synthetic turn (turn 0 of the
 *  destination). Messages are written to the store under (dst_session_id, dst_group_id,
 *  dst_task_id) with fresh identities, so the source rows are untouched.
 *  NULL when the task has none; the caller owns the returned turn. Used by fork rule 2 to
 *  recover a parent's own turns after L1 folding. */
extern struct edc_turn *edc_task_snapshot_turns(struct edc_task *task, int64_t dst_session_id,
                                                int64_t dst_group_id, int64_t dst_task_id,
                                                int dst_turn_ord);

/* State machine write points, named by intent: the caller says what it wants, and
 * (step, life) is validated. Returns false when the transition is rejected. */
extern bool edc_task_await_exec(struct edc_task *task);        /* LLM request sent */
extern bool edc_task_await_tools(struct edc_task *task);       /* tool calls sent */
extern bool edc_task_mark_ready(struct edc_task *task);        /* back to ready */
extern bool edc_task_mark_waiting_deps(struct edc_task *task); /* blocked by a dependency */
extern bool edc_task_mark_completed(struct edc_task *task);
/** Set DONE/ERROR *and* broadcast to dependents (cascade JOIN failures, release
 *  SEQUENCE), symmetric with edc_task_send_signal. Any caller putting a task into
 *  ERROR gets the propagation for free. Returns false and does not broadcast when
 *  the self-transition is rejected. */
extern bool edc_task_mark_failed(struct edc_task *task);
/** A stop signal landed on it (user `/task stop`, an inbound signal event): life becomes
 *  CANCELLED and the reason is EDC_END_STOPPED -- NOT skipped, NOT blocked. */
extern bool edc_task_mark_cancelled(struct edc_task *task);
/** `||`: the source succeeded, so this branch is not taken. Life becomes CANCELLED with
 *  the reason EDC_END_SKIPPED, so a UI can tell it apart from a user stop. */
extern bool edc_task_mark_skipped(struct edc_task *task);
/** A dependency ended without producing and took this task with it. |life| is the bucket
 *  the cascade lands in (ERROR for a failed/interrupted source, CANCELLED for a cancelled
 *  one); the reason is EDC_END_BLOCKED plus which source (and that source's own end), so
 *  "cascaded" is never confused with "failed by itself". Broadcasts nothing. */
extern bool edc_task_mark_blocked(struct edc_task *task, edc_task_life life, const struct edc_task *source);

/** The process went down while this task was in flight: keeps the step it was on and
 *  broadcasts to its dependents exactly like a failure (a JOIN dependent is cancelled,
 *  a SEQUENCE edge is voided and lets the one behind run). Idempotent. */
extern bool edc_task_mark_interrupted(struct edc_task *task);

/* Low-level primitives stating the target state directly. Most callers use the
 * intent functions above; only the scheduler's unified entry and the executor's
 * ports state the target. The `force` variant skips the transition table. */
extern bool edc_task_move(struct edc_task *task, edc_task_step step, edc_task_life life);
extern void edc_task_force_move(struct edc_task *task, edc_task_step step, edc_task_life life);

/* Read-side predicates. Each carries a life condition because a stop does not move
 * step: a task stopped while awaiting tools is (AWAIT_TOOLS, INTERRUPTED) and must not be
 * mistaken for running or awaiting tools. */
static inline bool edc_task_is_alive(const struct edc_task *task)
{
    return task && task->life == EDC_LIFE_ALIVE;
}
static inline bool edc_task_is_ready(const struct edc_task *task)
{
    return task && task->life == EDC_LIFE_ALIVE && task->step == EDC_TASKSTEP_READY;
}
static inline bool edc_task_awaits_deps(const struct edc_task *task)
{
    return task && task->life == EDC_LIFE_ALIVE && task->step == EDC_TASKSTEP_AWAIT_DEPS;
}
static inline bool edc_task_is_completed(const struct edc_task *task)
{
    return task && task->life == EDC_LIFE_COMPLETED;
}
static inline bool edc_task_is_cancelled(const struct edc_task *task)
{
    return task && task->life == EDC_LIFE_CANCELLED;
}
static inline bool edc_task_has_failed(const struct edc_task *task)
{
    return task && task->life == EDC_LIFE_ERROR;
}
static inline bool edc_task_is_interrupted(const struct edc_task *task)
{
    return task && task->life == EDC_LIFE_INTERRUPTED;
}
/* Terminal = no more advancing on its own: COMPLETED / CANCELLED / ERROR, and
 * INTERRUPTED (the process went down mid-flight). All of them need a decision or a
 * resume before they move again, so none of them holds a group's front. */
static inline bool edc_task_is_finished(const struct edc_task *task)
{
    return edc_task_is_completed(task) || edc_task_is_cancelled(task) || edc_task_has_failed(task) ||
           edc_task_is_interrupted(task);
}
static inline bool edc_task_awaits_exec(const struct edc_task *task)
{
    return task && task->life == EDC_LIFE_ALIVE && task->step == EDC_TASKSTEP_AWAIT_EXEC;
}
static inline bool edc_task_awaits_tools(const struct edc_task *task)
{
    return task && task->life == EDC_LIFE_ALIVE && task->step == EDC_TASKSTEP_AWAIT_TOOLS;
}
/* Parked = ready or waiting on deps: input and output are in place, only an event
 * is missing. A JOIN dependency injects its output into a parked task. */
static inline bool edc_task_is_parked(const struct edc_task *task)
{
    return edc_task_is_ready(task) || edc_task_awaits_deps(task);
}
/* `/task resume` gate: interrupted, failed, or cancelled -- by a stop, by its own failure,
 * or by a dependency (all of them only stopped the task, so the resume re-derives the step
 * from the messages and the ledger). One exclusion: a task that `||` SKIPPED never ran
 * because the shell decided it should not, so it is NOT resumable -- the reason says so,
 * the life (CANCELLED) cannot. */
static inline bool edc_task_is_resumable(const struct edc_task *task)
{
    /* INTERRUPTED is in here for the same reason ERROR is: the loop did not finish it and
     * nothing was decided about it, so /task resume picks it up where it stopped. */
    return task && task->end != EDC_END_SKIPPED &&
           (task->life == EDC_LIFE_INTERRUPTED || task->life == EDC_LIFE_ERROR ||
            task->life == EDC_LIFE_CANCELLED);
}

/* Outward vocabulary for the snapshot's "state" field, logs and command text.
 * The words are frozen (a TUI colour table, `/task status` and logs read them) and
 * are derived from (step, life); life speaks first. */
extern const char *edc_task_state_str(const struct edc_task *task);

/* Distance since the last todowrite, in LLM requests: at the threshold one system
 * reminder is appended to the request and the counter resets. */
#define EDC_TODO_SYNC_REMIND_GAP 10

/** Reset the throttle after a successful todowrite. */
extern void edc_task_note_todo_sync(struct edc_task *task);

/** Count one more request; true when the threshold was reached and this request
 *  should carry a todo reminder (the counter is reset here). */
extern bool edc_task_llm_request_poke(struct edc_task *task);

/** Add one LLM response's usage to the task's token account. All-zero usage is a
 *  no-op; total <= 0 falls back to prompt + completion. */
extern void edc_task_add_usage(struct edc_task *task, long long prompt_tokens, long long completion_tokens,
                               long long total_tokens, long long cached_tokens);

extern void edc_task_add_turn(struct edc_task *task, struct edc_turn *turn);
extern void edc_task_commit_active_turn(struct edc_task *task);

/** Total messages the task holds: committed turns plus the in-flight active turn. */
extern size_t edc_task_message_count(const struct edc_task *task);

/** Every user input the task received, joined by '\n'; a fresh sds the caller
 *  frees, or NULL. */
extern sds edc_task_user_input(const struct edc_task *task, const cJSON *session_messages);

extern bool edc_task_add_dependency(struct edc_task *target, struct edc_task *source, edc_task_dep_type type);
extern void edc_task_on_dep_completed(struct edc_task *target, struct edc_task *source);
/* True while any dependency edge is still outstanding (not completed, not
 * cancelled). Used by the executor at a request boundary: a task that owes someone
 * parks instead of sending the next LLM request (the fold barrier). */
extern bool edc_task_has_unresolved_deps(const struct edc_task *task);
/** True when |target| already holds a LIVE edge to |source| (a row exists and has not
 *  been voided). The predicate the queue needs: "is this task already queued behind that
 *  one?" must not depend on who created the row -- a `;` chain puts it there before the
 *  queue asks, and `edc_task_add_dependency` dedups by source. */
extern bool edc_task_depends_on(const struct edc_task *target, const struct edc_task *source);

/** Drop ONE still-live edge on purpose -- the user's explicit "stop waiting for this" --
 *  and re-decide: the edge is voided (persisted, like a BARRIER release) and, if that was
 *  the last unsatisfied dependency, the task becomes READY. Returns false when there is no
 *  live edge to |source| (already completed, already dropped, or never existed).
 *  The door out of a dependency whose source can never produce again -- its owner session
 *  is gone, so nothing will ever release it. Exposed for `/task dep <id> --drop <source>`. */
extern bool edc_task_drop_dep(struct edc_task *target, struct edc_task *source);
extern void edc_task_on_dep_cancelled(struct edc_task *target, struct edc_task *source);
/** Handle a source that failed, symmetric with cancellation (the source will not
 *  produce anything either way): a JOIN edge cascades the failure to the dependent
 *  and is dropped; a SEQUENCE edge is dropped to release it. */
extern void edc_task_on_dep_failed(struct edc_task *target, struct edc_task *source);
extern void edc_task_notify_dependents(struct edc_task *task);

/** True only while the process is running this turn (LLM_PROCESSING /
 *  TOOL_CALLING). */
extern bool edc_task_is_running(const struct edc_task *task);

extern void edc_task_send_signal(struct edc_task *task, edc_task_signal sig);

/* Record of tasks moved as a side effect of cancelling or failing one task. The
 * host owes each of them the same bookkeeping (index, reap queue, dirty, settle),
 * but the task layer does not know the host, so it only records them; they are
 * cleared through the host's unified entry. The list is carried by the task that
 * initiated the operation. */
/* Why a task moved as a side effect. The host settles every moved task through one entry,
 * but only ONE kind deserves a line in the chat: a task the user was told had ended, which
 * is now running again because its blocker produced. Queue advances (a group's next task)
 * and cascades (already announced by the failure notice) stay quiet. */
typedef enum
{
    EDC_MOVE_CASCADED,  /* a cancel / failure cascade took it down */
    EDC_MOVE_RELEASED,  /* a dependency satisfied it and it is READY again */
    EDC_MOVE_UNBLOCKED, /* it was BLOCKED and its blocker produced: worth saying */
} edc_task_move_kind;

struct edc_task_move_report
{
    struct edc_task *task; /* a task moved as a side effect (not owned) */
    edc_task_move_kind kind;
    /* Optional user-facing line the move deserves. Filled only for UNBLOCKED, because only
     * the task layer still knows which dependency did it (the move clears `blocked_by`).
     * Owned by the report. */
    sds notice;
    struct edc_task_move_report *next;
};

/** Take and hand each side-effect-moved task to |cb|, then clear the list. The
 *  callback may recurse (clearing can move more tasks), so the list is drained
 *  until empty. |cb| = NULL just clears the list (used when freeing the task). */
extern void edc_task_drain_moves(struct edc_task *task,
                                 void (*cb)(void *ctx, struct edc_task *moved, edc_task_move_kind kind,
                                            const char *notice),
                                 void *ctx);

/* Resume ledger of unfinished tool calls. Lifecycle: add (PENDING) when the LLM
 * returns tool_calls; mark_executing when the toolbox reports start; remove when
 * the result arrives; clear when the turn is committed or cancelled. */
extern struct edc_tool_call_record *edc_task_tool_call_add(struct edc_task *task, const char *call_id, const char *name,
                                                           cJSON *args);
extern struct edc_tool_call_record *edc_task_tool_call_find(struct edc_task *task, const char *call_id);
extern struct edc_tool_call_record *edc_task_tool_call_mark_executing(struct edc_task *task, const char *call_id);
extern bool edc_task_tool_call_remove(struct edc_task *task, const char *call_id);
extern void edc_task_tool_calls_clear(struct edc_task *task);
extern bool edc_task_has_pending_tool_calls(const struct edc_task *task);

/** The text written as a tool response when a dispatched call's result was lost (see
 *  edc_task_repair_dangling_tool_calls above). A tool-only task that resumes into that
 *  repair has no result of its own, so this is what its output says instead. */
extern const char *edc_task_interrupted_tool_note(void);

/** Fill in dangling tool responses before driving a task (idempotent).
 *
 *  An assistant message with `tool_calls` but no matching tool response is
 *  rejected by the API. The ledger cannot answer this because cancelling clears
 *  it, while the assistant message is already on disk: the messages are
 *  authoritative. The ledger is still read, but only to tell whether a call is
 *  still PENDING (to be re-dispatched) rather than interrupted. */
extern void edc_task_repair_dangling_tool_calls(struct edc_task *task);

extern void edc_task_set_output(struct edc_task *task, sds output);
extern sds edc_task_get_output(struct edc_task *task);

/* Sequence number by completion order within the owning container; the host issues
 * it, the task module only records it. 0 = not finished / not ordered. */
extern void edc_task_set_completion_seq(struct edc_task *task, uint32_t seq);

/** Owner session (= task->group->session), or NULL when there is no group. */
extern struct edc_session *edc_task_session(const struct edc_task *task);

/** Write the task's output to disk per its redirections (> / >> / 2>). */
extern void edc_task_flush_redirects(struct edc_task *task);

/** True when the task is completed and still has messages the memory does not
 *  cover (i.e. something left to fold). */
extern bool edc_task_is_foldable(struct edc_task *task);
/** Committed turn message count, used to advance the fold watermark. */
extern size_t edc_task_turn_msg_count(struct edc_task *task);
/** Committed turn messages not yet covered by the group memory (turn message
 *  count minus each turn's folded_prefix). */
extern size_t edc_task_unfolded_msg_count(struct edc_task *task);
/** True when any committed turn of this task has a folded prefix (its own
 *  conversation is therefore only partly in the group history). */
extern bool edc_task_has_folded_turns(struct edc_task *task);

extern cJSON *edc_task_get_context_messages(struct edc_task *task, struct edc_config *config);

#endif
