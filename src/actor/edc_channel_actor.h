#ifndef __BC_CHANNEL_ACTOR_H__
#define __BC_CHANNEL_ACTOR_H__

#include "cJSON.h"
#include "edc_actor.h"
#include "edc_config.h"
#include "sds.h"

/* ── The only channel implementation: adapter ────────────────
 *
 * The multi-channel era's implementations (telegram / feishu / qq / cli / email), its
 * registry, and the "find a handler by channel type" mechanism are all GONE: there is
 * only one adapter today, the TUI's path through edc_app
 * (edc_app_channel_adapter_init). So the actor holds no handler hash, only one send
 * callback.
 *
 * edc_channel_type still has only the value edc_channel_adapter, still carried on
 * task/message and WRITTEN TO DISK -- that part is format compatibility (see
 * edc_channel_to_json), not "there are other channels". */

struct edc_payload; /* folded paste blocks: placeholder -> real bytes (common/edc_payload.h) */

struct edc_channel_message
{
    sds content;
    sds header;     /* optional short description (e.g. executed command/tool name) shown above content */
    sds task_id;    /* id of the task that produced this message (NULL when the
                     * sender is not a task, e.g. user input or a snapshot).
                     * Rendering ends use it to label which task a chat message
                     * belongs to ("task 1"), so interleaved tasks stay readable. */
    sds session_id; /* id of the session that owns the producing task (NULL when
                     * the sender is not a task). Lets the frontend route a
                     * background session's output into *its own* transcript
                     * instead of the currently displayed one. */
    edc_channel_type type;
    edc_channel_message_type message_type;
    edc_render_pref_t render_pref; /* render preference declared by the sender; 0 = PLAIN (plain text) */
    /* A fact the sender declares: the LABEL to give a task created from this message when
     * that must differ from the content. The template paths set it to the line the user
     * typed (`/prompt review ars`) while the content is the expanded body -- so the task
     * reads as what was asked for, not as the body it turned into. NULL = derive the label
     * from the content, the usual way. */
    sds task_desc;
    /* The user input's own id (only on USER messages): the frontend generates it, the
     * store keeps it on the message, and the input history remembers it -- that is how a
     * recalled folded block finds its bytes back (request_member_payload). Opaque to the
     * kernel. */
    sds mid;
    /* The sender's folded paste blocks (a user input only). |content| is already the
     * EXPANDED text: this rides along as provenance (what was pasted, under which
     * placeholder) and is stored on the task's user message. NULL when nothing was
     * folded -- the ordinary case, and the reason no code needs to test for null
     * everywhere but the two places that care. Owned by this message. */
    struct edc_payload *payload;
    /* A fact the sender declares: start this input in a NEW group instead of the session's
     * foreground one. A non-interactive frontend sets it so its work cannot queue behind
     * (or deadlock with) whatever the interactive session is doing -- the group queue is
     * serial, so sharing a group with a live conversation means waiting for it. Not a
     * syntax: the `+` prefix is shell syntax and is gated by the shell switch, which is a
     * property of the config, not of the caller. */
    bool new_group;
    bool from_active_task;         /* A fact the sender declares: whether this output came from
                                    * its own session's active task. Declaration only -- the
                                    * kernel drops nothing based on it (it does not know who is
                                    * watching); the frontend uses it as the display gate: the
                                    * displayed session shows only the active task's output, the
                                    * rest stays silent. */
};

typedef struct
{
    edc_actor_t *actor;
    edc_actor_t *observer;
    struct
    {
        void *instance;
        void (*on_actor_message_send_fn)(void *instance, struct edc_channel_message *msg);
    } adapter;
} edc_channel_actor_state_t;

extern edc_actor_t *edc_channel_actor_new(void);
extern void edc_channel_actor_free(edc_actor_t *actor);
extern void edc_channel_actor_set_observer(edc_actor_t *actor, edc_actor_t *observer);

/* ── adapter: the only channel's socket ──────────────────────
 * The TUI uses it to receive BC_ACTOR_MSG_CHANNEL_SEND (the messages to show). */
extern void edc_channel_adapter_init(struct edc_actor *channel_actor, void *instance,
                                     void (*on_actor_message_send_fn)(void *, struct edc_channel_message *));

/* ── On-disk format compatibility: the task / crontab job "channel" key ──
 * Old files have it and new files WRITE IT TOO (dropping it would be a schema change);
 * its only possible value is adapter (the only channel), so the reader ignores its
 * content.
 * edc_channel_to_json() moved to core/edc_config.h (next to edc_channel_type): the
 * on-disk format is a low-level concern the kernel's state_serializer also needs, so it
 * should not pull in a boundary module's header. */

/* ── Where one message goes: "which channel, in whose name, which type" ──
 *
 * These three always travel together (outbound delivery needs them, so does a userland
 * reply), so they get a name -- instead of taking one slot each in dozens of signatures
 * and risking a swapped order. */
struct edc_reply_to
{
    struct edc_actor *channel_actor; /* whom to deliver to (the channel actor) */
    struct edc_actor *actor;         /* in whose name (the inbound sender; task output = the scheduler actor) */
    edc_channel_message_type type;   /* message type (routing key) */
};

/* ── Outbound: publish a message to the channel (frontend) ───
 * Ownership of |content| goes to this message (taken on success and failure alike). */
extern void edc_channel_publish(struct edc_reply_to to, const char *session_id, sds content);
/* Same, with a header (the payload reply names the mid it answers). */
extern void edc_channel_publish_keyed(struct edc_reply_to to, const char *header, sds content);

/* "the active session became X": emitted by the command layer after a successful
 * /session switch|new. */
extern void edc_channel_notify_active_session(struct edc_actor *channel_actor, const char *session_id);

/* ── Outbound: one TASK OUTPUT -> channel message ────────────
 *
 * Same family as edc_channel_publish (message shape + send path belong to the channel);
 * the difference is only "what this message describes":
 *   publish          = ready-made content (a snapshot / event notice / command reply);
 *   send_task_reply  = one round of a task's output (LLM reply / thought / system hint);
 *   send_task_tool_* = a task's tool output (with a header).
 * The latter two fill task_id / session_id / from_active_task / render_pref -- that is
 * the CHANNEL MESSAGE SHAPE, so the channel fills it, not the caller.
 *
 * The caller (scheduler / executor / toolbox) only declares "a reply is due".
 * to.type = this message's type (routing key); |content| ownership goes to the message,
 * taken here on the failure path. */
struct edc_task; /* fwd: appears only as a pointer */
extern void edc_channel_send_task_reply(struct edc_reply_to to, struct edc_task *task, const char *content,
                                        void *request_id);
extern void edc_channel_send_task_tool_message(struct edc_reply_to to, struct edc_task *task, const char *header,
                                               const char *content, edc_render_pref_t render_pref, void *request_id);

/* ── Ports: the channel provides the four outbound ports ─────
 *
 * The channel's message shape and delivery path live in this file, so "install the
 * ports onto the scheduler" is here too -- the provider wires it. The assembly root
 * calls it once at assembly time and never appears on a runtime path again
 * (send_reply / send_tool_message / snapshot_reply / snapshot_changed).
 *
 * Port signatures (edc_scheduler.h) are unchanged: kernel call sites and test fakes are
 * unaffected. */
struct edc_scheduler; /* fwd */
extern void edc_channel_actor_bind_scheduler(edc_actor_t *actor, struct edc_scheduler *sched);

extern void edc_channel_message_free(struct edc_channel_message *msg);
extern struct edc_channel_message *edc_channel_message_new(edc_channel_type type, edc_channel_message_type message_type,
                                                           sds content);

/* ── Boundary translation: channel message -> core's inbound input ──
 *
 * This is where BOTH vocabularies are known (the channel message + struct
 * edc_sched_input), so the translation belongs here -- dependency direction is
 * "adapter layer -> core". A producer (frontend / tool) builds the payload with it and
 * sends BC_ACTOR_MSG_CHANNEL_RECV to the scheduler; the scheduler side knows only the
 * latter.
 *
 * edc_channel_input_new TAKES OWNERSHIP of msg (besides the content it also
 * edc_channel_message_free()s it on release), so it can serve directly as the actor
 * msg's data_free_fn:
 * edc_actor_msg_new(..., edc_channel_input_new(msg), (edc_actor_data_free_fn)edc_channel_input_free). */
struct edc_sched_input;
extern struct edc_sched_input *edc_channel_input_new(struct edc_channel_message *msg);
extern void edc_channel_input_free(struct edc_sched_input *input);

#endif
