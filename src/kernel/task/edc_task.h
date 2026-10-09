#ifndef __BC_TASK_H__
#define __BC_TASK_H__

#include "edc_config.h"
#include "edc_containers.h"
#include "edc_task_types.h"
#include "edc_turn.h"
#include "sds.h"
#include <cJSON.h>
#include <stdbool.h>
#include <stddef.h>

struct edc_session;
struct edc_config;
struct edc_task;
struct edc_task_move_report;

/* "Where is it" (step) and "is it alive" (life) are two axes kept apart, so a
 * phase survives being persisted instead of being coarse-grained into one value.
 * A suspended task keeps its step: (AWAIT_TOOLS, SUSPENDED) is not running and is
 * not awaiting tools. The two are orthogonal. */
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
    EDC_LIFE_SUSPENDED, /* Ctrl+Z, or nothing is running after a restart */
    EDC_LIFE_COMPLETED,
    EDC_LIFE_CANCELLED,
    EDC_LIFE_ERROR,
} edc_task_life;

extern const char *edc_task_step_name(edc_task_step step);
extern const char *edc_task_life_name(edc_task_life life);
extern edc_task_step edc_task_classify_step(const char *name);
extern edc_task_life edc_task_classify_life(const char *name);

/* Two more orthogonal axes: where the output goes, and whether the task must
 * survive a restart. Defaults (CHANNEL, PERSISTENT) describe a user task; only a
 * system task flips them, and that combination is the whole reason a fold needs
 * neither a private protocol nor a hidden flag. */
typedef enum
{
    EDC_SINK_CHANNEL,      /* output goes back to the user */
    EDC_SINK_GROUP_MEMORY, /* output becomes the group's memory (fold) */
} edc_sink;

extern const char *edc_task_sink_name(edc_sink sink);
extern edc_sink edc_task_classify_sink(const char *name);


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

struct edc_task
{
    char *task_id;    /* PID */
    char *group_id;   /* PGID -- the leader's task_id */
    char *session_id; /* SID */

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

    struct edc_task_group *group;

    EdcQueue turns;               /* FIFO: struct edc_turn* */
    struct edc_turn *active_turn; /* the turn in progress */

    void *current_request_id; /* request_id sent to an actor */
    int cancelled_request;    /* request marked cancelled */

    /* LLM requests issued for the current turn, incremented by the scheduler on
     * every send_llm. At max_iter the chain is no longer continued. Reset to 0
     * when a new input starts. max_iter <= 0 means unlimited. */
    int llm_rounds;

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

    edc_channel_type channel_type;

    cJSON *todos; /* JSON Array */

    /* LLM requests since the last successful todowrite; at the threshold a single
     * system reminder is appended to the request and the counter resets. */
    int todo_sync_gap;

    struct edc_task *forked_from;       /* parent (NULL = not a fork) */
    uint32_t fork_point_completion_seq; /* parent's completion watermark at the fork point */

    sds prompt;      /* custom system prompt (NULL = default) */
    sds prompt_name; /* e.g. "code-reviewer" */

    struct edc_task_dep *deps;       /* whom I depend on */
    struct edc_task_dep *dependents; /* who depends on me */
    size_t deps_total;
    size_t deps_completed;
    sds output; /* final output, injected into a JOIN dependent */

    uint32_t completion_seq; /* completion order (0 = not finished) */
    int has_deferred_vars;   /* $VAR references to expand */

    struct edc_tool_call_record *tool_calls;

    sds redirect_stdout;
    sds redirect_stderr;
    bool redirect_append; /* true = >>, false = > */

    /* Output target for a sink=GROUP_MEMORY task: which group's memory to write,
     * and where this round's cut went (P2a-4). The cut is one point in the group's
     * content order: the member it falls in, its turn, and how many leading messages
     * of that turn the memory covers. Everything before it is folded whole, nothing
     * after it is touched. A NULL task id means "no cut" (a fold task written by an
     * older binary): the landing then has nothing to apply and reports it.
     * Filled by the folding side; the task itself does not interpret them. */
    struct edc_task_group *sink_group;
    char *fold_cut_task_id;
    uint32_t fold_cut_turn;
    uint32_t fold_cut_prefix;

    /* Tasks a cancel or a dependency change moved as a side effect (cascaded JOIN,
     * released SEQUENCE). The task layer does not know the host, so it records them
     * here for the host to settle through its unified entry. Memory-only. */
    struct edc_task_move_report *moved_report;

    struct edc_task *group_next;  /* next member within the group */
    struct edc_task *prev, *next; /* session-wide list */
};

extern struct edc_task *edc_task_new(const char *task_id, const char *session_id, const char *group_id,
                                     const char *desc, edc_channel_type channel_type);
extern struct edc_task *edc_task_fork(struct edc_task *parent, const char *task_id, const char *desc,
                                      uint32_t fork_point_completion_seq, edc_channel_type channel_type);
extern void edc_task_free(struct edc_task *task);

/** Deep-copy all of the task's turn messages into one synthetic snapshot turn.
 *  Handles both flushed and unflushed messages. NULL when the task has none; the
 *  caller frees the turn. Used by fork rule 2 to recover a parent's own turns
 *  after L1 folding. */
extern struct edc_turn *edc_task_snapshot_turns(struct edc_task *task, struct edc_session *session,
                                                const char *snapshot_task_id);

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
extern bool edc_task_mark_cancelled(struct edc_task *task);
extern bool edc_task_mark_suspended(struct edc_task *task); /* moves life only */

/* Low-level primitives stating the target state directly. Most callers use the
 * intent functions above; only the scheduler's unified entry and the executor's
 * ports state the target. The `force` variant skips the transition table. */
extern bool edc_task_move(struct edc_task *task, edc_task_step step, edc_task_life life);
extern void edc_task_force_move(struct edc_task *task, edc_task_step step, edc_task_life life);

/* Read-side predicates. Each carries a life condition because SUSPENDED does not
 * move step: a task suspended while awaiting tools is (AWAIT_TOOLS, SUSPENDED) and
 * must not be mistaken for running or awaiting tools. */
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
static inline bool edc_task_is_suspended(const struct edc_task *task)
{
    return task && task->life == EDC_LIFE_SUSPENDED;
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
 * is missing. These are the tasks `/task join` can inject into. */
static inline bool edc_task_is_parked(const struct edc_task *task)
{
    return edc_task_is_ready(task) || edc_task_awaits_deps(task);
}
/* `/task resume` gate: suspended, failed, or cancelled. A cancelled task is
 * resumable because cancelling only stops it; the resume re-derives the step from
 * the messages and ledger. */
static inline bool edc_task_is_resumable(const struct edc_task *task)
{
    return task && (task->life == EDC_LIFE_SUSPENDED || task->life == EDC_LIFE_ERROR ||
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
struct edc_task_move_report
{
    struct edc_task *task; /* a task moved as a side effect (not owned) */
    struct edc_task_move_report *next;
};

/** Take and hand each side-effect-moved task to |cb|, then clear the list. The
 *  callback may recurse (clearing can move more tasks), so the list is drained
 *  until empty. */
extern void edc_task_drain_moves(struct edc_task *task, void (*cb)(void *ctx, struct edc_task *moved), void *ctx);

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

extern void edc_task_set_prompt(struct edc_task *task, const char *prompt_name, const char *prompt_text);

extern cJSON *edc_task_get_context_messages(struct edc_task *task, struct edc_config *config);

#endif
