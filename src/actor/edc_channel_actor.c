#include "edc_channel_actor.h"

#include "edc_payload.h"
#include "edc_actor.h"
#include "edc_compat.h"
#include "edc_config.h"
#include "edc_scheduler.h"
#include "edc_session.h"
#include "edc_task.h"
#include "edc_task_group.h"
#include "log.h"
#include "sds.h"
#include <inttypes.h> /* PRId64 */
#include <stdio.h>
#include <stdlib.h>

/* The only channel actor: registered by edc_channel_actor_bind_scheduler (see the
 * "ports" section at the end of the file). Kept here because edc_channel_actor_free
 * must clear it -- so no dangling pointer is left after release. */
static edc_actor_t *g_channel = NULL;

/* ── Boundary translation: channel message -> core's inbound input ──
 *
 * struct edc_sched_input is core's vocabulary (edc_scheduler.h); this file knows BOTH
 * vocabularies -- so core need not know the channel message's shape nor include this
 * header. The input TAKES OWNERSHIP of the message: releasing the input releases the
 * message too (origin is only passed through, but its lifetime is managed here -- else
 * it would leak, or the scheduler would have to know its type). */
struct edc_sched_input *edc_channel_input_new(struct edc_channel_message *msg)
{
    if (!msg)
        return NULL;
    struct edc_sched_input *input = calloc(1, sizeof(*input));
    if (!input)
    { /* OOM: drop this input (the scheduler returns on a NULL payload); do not leak the message */
        edc_channel_message_free(msg);
        return NULL;
    }
    input->session_id = msg->session_id;
    input->content = msg->content;
    input->type = msg->type;
    input->origin = msg;
    return input;
}

void edc_channel_input_free(struct edc_sched_input *input)
{
    if (!input)
        return;
    struct edc_channel_message *msg = input->origin;
    free(input);
    if (msg)
        edc_channel_message_free(msg);
}

/* Only one channel (adapter): an outbound message (CHANNEL_SEND) goes straight to that
 * callback. This used to look up the implementation in a handler hash by msg->sub_type
 * (= channel type) -- the multi-channel mechanism, deleted with the extra channels. */
static void edc_channel_actor_handle(edc_actor_t *actor, edc_actor_msg_t *msg)
{
    edc_channel_actor_state_t *state = actor->state;

    if (msg->type == BC_ACTOR_MSG_CHANNEL_SEND && state->adapter.on_actor_message_send_fn)
        state->adapter.on_actor_message_send_fn(state->adapter.instance, msg->data);

    edc_actor_msg_free(msg);
}

void edc_channel_adapter_init(edc_actor_t *channel_actor, void *instance,
                              void (*on_actor_message_send_fn)(void *, struct edc_channel_message *))
{
    edc_channel_actor_state_t *state = channel_actor->state;
    state->adapter.instance = instance;
    state->adapter.on_actor_message_send_fn = on_actor_message_send_fn;
}

struct edc_channel_message *edc_channel_message_new(edc_channel_type type, edc_channel_message_type message_type,
                                                    sds content)
{
    struct edc_channel_message *message = g_new0(struct edc_channel_message, 1);
    message->type = type;
    message->message_type = message_type;
    message->content = content;
    return message;
}

void edc_channel_message_free(struct edc_channel_message *msg)
{
    if (!msg)
        return;
    if (msg->header)
        sdsfree(msg->header);
    if (msg->task_id)
        sdsfree(msg->task_id);
    if (msg->session_id)
        sdsfree(msg->session_id);
    if (msg->payload)
        edc_payload_free(msg->payload);
    if (msg->mid)
        sdsfree(msg->mid);
    if (msg->task_desc)
        sdsfree(msg->task_desc);
    sdsfree(msg->content);
    free(msg);
}

edc_actor_t *edc_channel_actor_new(void)
{
    edc_channel_actor_state_t *state = calloc(1, sizeof(edc_channel_actor_state_t));

    state->observer = NULL;

    edc_actor_t *actor = edc_actor_new("edc_channel_actor", edc_channel_actor_handle, state);
    state->actor = actor;

    state->adapter.instance = NULL;
    state->adapter.on_actor_message_send_fn = NULL;
    return actor;
}

void edc_channel_actor_free(edc_actor_t *actor)
{
    edc_channel_actor_state_t *state = (edc_channel_actor_state_t *)actor->state;

    if (g_channel == actor) /* the port provider's registration slot: leave no dangling pointer */
        g_channel = NULL;
    free(state);
    edc_actor_free(actor);
}

void edc_channel_actor_set_observer(edc_actor_t *actor, edc_actor_t *observer)
{
    edc_channel_actor_state_t *state = (edc_channel_actor_state_t *)actor->state;
    state->observer = observer;
}

/* ── Outbound: message -> channel (frontend) ─────────────────
 *
 * Build a channel message and send BC_ACTOR_MSG_CHANNEL_SEND to this actor -- the
 * CHANNEL's own vocabulary (message shape + send path both belong here). It used to
 * live in edc_app.c only because that happened to hold the channel_actor; now any module
 * holding the channel handle can deliver. */

/* The single send exit: publish and the two send_task_* all go through here, so
 * BC_ACTOR_MSG_CHANNEL_SEND appears exactly once in the file. */
static void edc_channel_send_msg(struct edc_reply_to to, struct edc_channel_message *msg, void *request_id)
{
    edc_actor_send(to.channel_actor,
                   edc_actor_msg_new_sub_type(BC_ACTOR_MSG_CHANNEL_SEND, edc_channel_adapter, to.actor, request_id, msg,
                                              (edc_actor_data_free_fn)edc_channel_message_free));
}

void edc_channel_publish(struct edc_reply_to to, const char *session_id, sds content)
{
    if (!to.channel_actor)
    {
        if (content)
            sdsfree(content);
        return;
    }
    struct edc_channel_message *msg = edc_channel_message_new(edc_channel_adapter, to.type, content);
    if (session_id && *session_id)
        msg->session_id = sdsnew(session_id);

    edc_channel_send_msg(to, msg, NULL);
}

/* ── One task output -> channel message ──────────────────────
 *
 * Owning session: a backgrounded session's output is still sent; the frontend silently
 * writes it into its own display stream by session_id. The display rule "the displayed
 * session shows only the active task" moved TO THE FRONTEND -- core only declares the
 * fact (from_active_task) and no longer asks "who is watching" (it no longer knows: the
 * "current session" copy is gone). */
void edc_channel_send_task_reply(struct edc_reply_to to, struct edc_task *task, const char *content, void *request_id)
{
    if (!to.channel_actor || !task || !content)
        return;

    struct edc_session *own = (task->group && task->group->session) ? task->group->session : NULL;
    if (!own)
    {
        log_warn("dropping %s from task %s: no owning session", "reply", edc_id_str(task->task_id));
        return;
    }

    struct edc_channel_message *reply = edc_channel_message_new(task->channel_type, to.type, sdsnew(content));
    /* Tag the message with its producing task so the frontend can show which
     * task a chat message belongs to (interleaved tasks in one session). */
    if (task->task_id)
        reply->task_id = sdscatprintf(sdsempty(), "%" PRId64, task->task_id);
    /* And with its owning session, so a background session's output lands in
     * that session's transcript rather than the one on screen.
     *
     * edc_id_str(), NOT edc_id_str(): this string goes to the frontend, which
     * derives the transcript path from it (<ws>/<session_id>/chat.jsonl) and compares it
     * against the id it displays. A session's text form is "s<id>" -- a bare number would
     * point at a directory that does not exist. */
    reply->session_id = sdsnew(edc_id_str(own->session_id));
    /* Declare the fact: is this its session's active task (the frontend's display gate)? */
    reply->from_active_task = edc_session_get_active_task(own) == task;
    /* The sender explicitly declares the render preference; the renderer (CLI/TUI) no
     * longer guesses: only an LLM reply (RESP) declares Markdown; thoughts/system and
     * other internal text keep the default PLAIN (0) and show as-is. */
    if (to.type == EDC_CHANNEL_MESSAGE_RESP_TYPE)
        reply->render_pref = EDC_RENDER_MARKDOWN;

    edc_channel_send_msg(to, reply, request_id);
}

/* A TOOL message with a header. The display gate (show only the displayed session's
 * active task) is in the frontend -- this only declares the fact (from_active_task). */
void edc_channel_send_task_tool_message(struct edc_reply_to to, struct edc_task *task, const char *header,
                                        const char *content, edc_render_pref_t render_pref, void *request_id)
{
    if (!to.channel_actor || !task || !content)
        return;

    struct edc_channel_message *reply =
        edc_channel_message_new(task->channel_type, EDC_CHANNEL_MESSAGE_TOOL_TYPE, sdsnew(content));
    reply->render_pref = render_pref;
    if (task->task_id)
        reply->task_id = sdscatprintf(sdsempty(), "%" PRId64, task->task_id);
    /* Owning session (see edc_channel_send_task_reply): routes a background
     * session's tool output into its own transcript. */
    {
        struct edc_session *own = (task->group && task->group->session) ? task->group->session : NULL;
        if (own && own->session_id > 0)
            reply->session_id = sdsnew(edc_id_str(own->session_id));
        reply->from_active_task = own && edc_session_get_active_task(own) == task;
    }
    if (header && *header)
        reply->header = sdsnew(header);

    edc_channel_send_msg(to, reply, request_id);
}

/* The same trace as the scheduler's EDGECLAW_SNAPSHOT_TRACE (the other half of the same
 * path: a tab-switch event does not go through the scheduler's dirty list but is sent
 * directly from here) -- evidence tooling must see it, or "how did the frontend learn
 * about the switch" is broken in the trace. */
static void edc_channel_trace_active_session(const char *session_id)
{
    const char *path = getenv("EDGECLAW_SNAPSHOT_TRACE");
    if (!path || !*path)
        return;
    FILE *f = fopen(path, "a");
    if (!f)
        return;
    fprintf(f, "notify reason=active session=%s\n", session_id ? session_id : "?");
    fclose(f);
}

/* The active session changed (user /session switch|new): tell the frontend to display
 * that tree.
 *
 * This broadcasts a FACT -- "active_index is now X" (rule 3's sole authority is the
 * session_manager) -- emitted by userland (the command layer) after a successful switch.
 * It is not "the kernel deciding who sees what": the frontend still decides whether to
 * follow (it is merely asked to display the tab the user just clicked), and it still
 * pulls the snapshot itself. */
void edc_channel_notify_active_session(struct edc_actor *channel_actor, const char *session_id)
{
    edc_channel_trace_active_session(session_id);
    /* Tab-switch notice: emitted by the channel in its OWN name (no inbound sender). */
    struct edc_reply_to to = {.channel_actor = channel_actor, .actor = channel_actor,
                              .type = EDC_CHANNEL_MESSAGE_TASK_TREE_CHANGED_TYPE};
    edc_channel_publish(to, session_id, sdsnew("active"));
}

/* ── Ports: the channel provides the four outbound ports ─────
 *
 * The message shape and delivery path are already in this file (above), so "install the
 * ports onto the scheduler" is here too: the assembly root calls
 * edc_channel_actor_bind_scheduler once at assembly time and never appears on this path
 * again. Port signatures are unchanged; kernel call sites and test fakes are unaffected.
 *
 * The channel actor is THE ONLY ONE in the process (no multi-channel since 33027bc), so
 * a provider just registers itself at bind -- no need to hang a "channel handle" on the
 * kernel's scheduler (which would make core know boundary things again). `.actor` is
 * still the scheduler actor: task output is sent in the scheduler's name, verbatim as
 * the old assembly-root thunk. */
static struct edc_reply_to edc_channel_to(struct edc_scheduler *sched, edc_channel_message_type type)
{
    struct edc_reply_to to = {.channel_actor = g_channel, .actor = edc_scheduler_get_actor(sched), .type = type};
    return to;
}

/* Like edc_channel_publish, plus the header: the payload reply names the mid it answers. */
void edc_channel_publish_keyed(struct edc_reply_to to, const char *header, sds content)
{
    if (!to.channel_actor)
    {
        if (content)
            sdsfree(content);
        return;
    }
    struct edc_channel_message *msg = edc_channel_message_new(edc_channel_adapter, to.type, content);
    if (header && *header)
        msg->header = sdsnew(header);
    edc_channel_send_msg(to, msg, NULL);
}

static void edc_channel_cb_send_reply(struct edc_scheduler *sched, struct edc_task *task, const char *content,
                                      edc_channel_message_type message_type, void *request_id)
{
    edc_channel_send_task_reply(edc_channel_to(sched, message_type), task, content, request_id);
}

static void edc_channel_cb_send_tool_message(struct edc_scheduler *sched, struct edc_task *task, const char *header,
                                             const char *content, edc_render_pref_t render_pref, void *request_id)
{
    edc_channel_send_task_tool_message(edc_channel_to(sched, EDC_CHANNEL_MESSAGE_TOOL_TYPE), task, header, content,
                                       render_pref, request_id);
}

static void edc_channel_cb_snapshot_reply(struct edc_scheduler *sched, sds serialized)
{
    if (!serialized)
        return;
    edc_channel_publish(edc_channel_to(sched, EDC_CHANNEL_MESSAGE_TASK_SNAPSHOT_TYPE), NULL, serialized);
}

static void edc_channel_cb_snapshot_changed(struct edc_scheduler *sched, const char *session_id, const char *reason)
{
    edc_channel_publish(edc_channel_to(sched, EDC_CHANNEL_MESSAGE_TASK_TREE_CHANGED_TYPE), session_id,
                        sdsnew(reason ? reason : ""));
}

static void edc_channel_cb_message_payload(struct edc_scheduler *sched, const char *mid, sds payload)
{
    edc_channel_publish_keyed(edc_channel_to(sched, EDC_CHANNEL_MESSAGE_MESSAGE_PAYLOAD_TYPE), mid, payload);
}

void edc_channel_actor_bind_scheduler(edc_actor_t *actor, struct edc_scheduler *sched)
{
    if (!actor || !sched)
        return;

    g_channel = actor;
    sched->send_reply = edc_channel_cb_send_reply;
    sched->send_tool_message = edc_channel_cb_send_tool_message;
    sched->snapshot_reply = edc_channel_cb_snapshot_reply;
    sched->snapshot_changed = edc_channel_cb_snapshot_changed;
    sched->message_payload_reply = edc_channel_cb_message_payload;
}
