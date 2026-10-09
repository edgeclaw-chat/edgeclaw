/* edc_executor.h -- steps a task from within (a stateless advance function).
 *
 * Division of labour: the scheduler owns everything *between* tasks (readiness,
 * the dependency DAG, resources); the executor owns everything *within* a task —
 * given an event, decide the next step and trigger I/O through the ops.
 *
 * It keeps nothing between calls: phase, tool ledger, accumulated output and
 * budget live on the task, which is what makes a resume possible. Outward is a
 * call (ops), inward is an event (edc_event); this header knows neither the llm
 * actor nor the toolbox actor.
 */
#ifndef EDC_EXECUTOR_H
#define EDC_EXECUTOR_H

#include "edc_config.h"
#include "edc_task.h"
#include "sds.h"
#include "cJSON.h"
#include <stdbool.h>
#include <stdint.h>

/* Forward-declared because several prototypes below only use the pointer; without
 * it the `struct edc_executor *` in a prototype would form its own type. */
struct edc_executor;

/* Inbound events. */
typedef enum
{
    EDC_EVENT_START,        /* drive a task from its current state (the only entry) */
    EDC_EVENT_EXEC_DONE,    /* an LLM response arrived */
    EDC_EVENT_TOOL_STARTED, /* a tool actually started */
    EDC_EVENT_TOOL_DONE,    /* a tool finished */
    EDC_EVENT_SIGNAL,       /* cancel/interrupt */
} edc_event_kind;

/* START carries no payload: the task's own state (dangling tool response,
 * PENDING ledger, active turn) answers what comes next. The *between-tasks*
 * check (deps unsatisfied) is not here -- that is the scheduler's gate. START is
 * the single implementation of "make this task run". */

/* Normalized view of an LLM response; `reason` uses the enum below, not the
 * actor's. Built on the stack by the caller; `msg` and `tool_calls` are consumed
 * by advance (set to NULL on hand-off). */
typedef enum
{
    EDC_REASON_STOP = 0,   /* finished normally */
    EDC_REASON_TOOL_CALL,  /* wants to call tools */
    EDC_REASON_OTHER,      /* sensitive / network / context_exceeded / unknown */
    EDC_REASON_LENGTH,     /* the provider cut the answer at max_tokens */
} edc_reason;

/* Normalized tool call for ledger registration. All fields are borrowed and
 * valid only during the advance. */
struct edc_tool_call
{
    const char *id;
    const char *name;
    cJSON *args; /* deep-copied when added to the ledger */
};

struct edc_event_exec
{
    bool failed;              /* network/protocol error */
    const char *error_message;/* non-NULL when failed */
    edc_reason reason;
    sds content;              /* final text, may be NULL */
    cJSON *msg;               /* assistant message; ownership moves into the turn */
    /* This round's tool calls, passed straight through to dispatch_tools as an
     * opaque payload (ownership moves with the dispatch), plus a normalized view
     * for the ledger. */
    void *tool_calls_raw;
    const struct edc_tool_call *calls;
    int n_calls;
    /* Token accounting; every path (error / tool_calls / stop / unknown) counts. */
    int prompt_tokens;
    int completion_tokens;
    int total_tokens;
    int cached_tokens;
};

struct edc_event_tool_done
{
    const char *call_id;
    const char *name;
    sds result;
    bool has_next; /* another tool in the chain -> do not send an LLM request yet */
};

/* A signal is an event too: it changes task state directly, and a late completion
 * must stay harmless. */
struct edc_event_signal
{
    edc_task_signal sig;
};

struct edc_event
{
    edc_event_kind kind;
    const char *request_id; /* the id the task was registered under */
    union
    {
        struct edc_event_exec exec; /* EXEC_DONE */
        const char *call_id;        /* TOOL_STARTED */
        struct edc_event_tool_done tool_done;
        struct edc_event_signal signal;
    } as;
};

typedef enum
{
    EDC_STEP_BLOCKED, /* dispatched or waiting: LLM running / tool running / deps */
    EDC_STEP_DONE,    /* this stage finished (task COMPLETED) */
    EDC_STEP_ERROR,   /* failed (task ERROR) */
} edc_step_outcome;

/* A summary of what *already happened* on the task, not a list of commands: the
 * caller decides who to tell. "Start the next I/O" is the executor's job (else it
 * did not advance); "tell the world" is the scheduler's (else it is doing
 * business). */
struct edc_advance_result
{
    edc_step_outcome outcome;

    /* step or life changed during this advance (snapshotted at entry, compared at
     * exit, so it covers signals too). */
    bool changed;

    /* This advance *decided* the task ends (COMPLETED / ERROR / CANCELLED). Given
     * by the branch explicitly, not derived from `changed`: the move may be
     * rejected while the decision still stands. */
    bool settled;

    /* Text to deliver, borrowed for as long as it takes the caller to handle this
     * result (delivery happens right then); NULL when there is nothing. */
    const char *notice;
};

/* Outbound ops (kernel -> world). `ctx` is always the scheduler, passed through.
 *
 * Admission test: does it *start the next I/O*? That is the executor's job.
 * Reporting the result to the outside, and the state machine, are not. This is a
 * responsibility test, not a location one -- `send_llm` and `send_reply` sit in
 * the same package cycle, and only the responsibility test separates them
 * (`send_reply` starts nothing; the task advances without it).
 *
 * The two I/O steps of a task are the LLM request and the tool chain; redispatch
 * is the same tool step fed from the PENDING ledger instead of this round's
 * response.
 */
struct edc_executor_ops
{
    /* Dispatch an LLM request. Returns whether it was really sent: an uninstalled
     * port yields false, so "cannot make progress" is a fact the executor can see
     * (it fails the task with a reason rather than leaving a phantom). */
    bool (*send_llm)(void *ctx, struct edc_task *task, void *request_id);

    /* Dispatch this round's tool call chain. */
    void (*dispatch_tools)(void *ctx, struct edc_task *task, void *tool_calls, void *request_id);

    /* Re-dispatch the PENDING tool chain from the ledger (START's resume branch).
     * Returns how many requests were posted; <= 0 fails that branch. */
    int (*redispatch_tools)(void *ctx, struct edc_task *task, void *request_id);
};

/* Boundary adapters. Actor message payloads (llm response, toolbox request, tool
 * emit) are understood only here, so the scheduler need not include the actor
 * headers. Payloads travel as `void *`: the adaptation is a type-agnostic calling
 * convention. */

/* BC_ACTOR_MSG_LLM_RESPONSE */
struct edc_advance_result edc_executor_advance_llm_resp(struct edc_executor *ex, struct edc_task *task,
                                                       void *request_id, void *resp);

/* BC_ACTOR_MSG_TOOL_RESPONSE */
struct edc_advance_result edc_executor_advance_tool_resp(struct edc_executor *ex, struct edc_task *task,
                                                         void *request_id, void *req);

/* Display fields of TOOL_EMIT, extracted here so the "should it be shown" decision
 * can stay in the scheduler while the struct knowledge stays here. */
struct edc_tool_emit_view
{
    const char *header;
    const char *content;
    edc_render_pref_t render_pref;
};
bool edc_executor_view_tool_emit(void *emit_msg, struct edc_tool_emit_view *out);

struct edc_executor
{
    const struct edc_executor_ops *ops;
    void *ctx; /* passed through to the ops (= scheduler) */
    /* Process config, read-only and not owned (the app owns it). */
    const struct edc_config *config;
};

void edc_executor_init(struct edc_executor *ex, const struct edc_executor_ops *ops, void *ctx,
                       const struct edc_config *config);

/* Take an event, decide the next step and trigger its side effects. The task must
 * already be resolved and not cancelled (that check stays in the scheduler). For
 * START the scheduler must also have passed the between-tasks gate. `ev` is
 * non-const: `msg` / `tool_calls_raw` ownership moves, and advance NULLs them out.
 * The caller must hand the result to the policy point. */
struct edc_advance_result edc_executor_advance(struct edc_executor *ex, struct edc_task *task, struct edc_event *ev);

#endif /* EDC_EXECUTOR_H */
