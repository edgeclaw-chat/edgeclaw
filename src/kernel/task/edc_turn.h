#ifndef __BC_TURN_H__
#define __BC_TURN_H__
#include "edc_containers.h"
#include "edc_msg_part.h"
#include "sds.h"
#include <cJSON.h>
#include <stdbool.h>
#include <stdint.h>

/* ── A turn is METADATA. The messages are rows in <workspace>/edgeclaw.db ─────
 *
 * This used to hold an EdcQueue of messages, and every save rewrote the whole tree.
 * Now the store is the only copy: a turn carries the key (task_id, turn_ord) and the
 * three numbers the runtime needs to answer without reading rows
 * (msg_count / folded_prefix / memo_prefix). Reading a message is a store call; adding
 * one is a single INSERT.
 *
 * Consequence for callers: a message you read is a COPY you own (cJSON_Delete it). Never
 * keep a pointer to one thinking it is the shared instance -- the old code handed out
 * borrowed pointers from the queue, and there is no queue any more.
 */
struct edc_turn
{
    int64_t task_id;  /* owner task; with turn_ord this is the storage key */
    char *session_id; /* the ancestors a message row needs (FK chain), see
                       * edc_state_ensure_turn(). Borrowed from the task at creation. */
    int64_t group_id;
    /* Position in the owner's turn sequence. Committed turns take their index in
     * task->turns; the in-flight turn takes the tail (== the committed count). Assigned by
     * edc_task_new_active_turn() / edc_turn_new_for(), never changed afterwards -- the
     * rows are keyed by it. */
    int turn_ord;
    /* How many messages this turn has. Mirrors turns.msg_count, which is authoritative
     * across a restart; kept in step here so counting never costs a query. */
    int msg_count;
    /* How many leading messages of this turn the group memory already covers (L1
     * fold). Request assembly and the group history both start after them; set at
     * fold landing, persisted with the turn. 0 = nothing folded. */
    int folded_prefix;
    /* How many leading messages of this turn have already entered the session memory
     * (L2), by ANY path (fold-chained curation or the idle-group extraction). It is a
     * pure extraction watermark: it NEVER participates in request assembly, the group
     * history or the fold corpus. Invariant: folded_prefix <= memo_prefix <= msg_count,
     * and both writers (extraction landing, fold landing clamp) only ever raise it. */
    int memo_prefix;
};

/* A turn with no storage key yet: the caller must call edc_turn_set_key() before adding
 * any message. |session_id| / |group_id| are only needed to create the parent rows on
 * first append. */
extern struct edc_turn *edc_turn_new();
/* A turn bound to (task_id, turn_ord) -- the normal constructor. */
extern struct edc_turn *edc_turn_new_for(const char *session_id, int64_t group_id,
                                         int64_t task_id, int turn_ord);
/* Bind the key. Only for turns built before their position is known (fork snapshots). */
extern void edc_turn_set_key(struct edc_turn *turn, const char *session_id, int64_t group_id,
                             int64_t task_id, int turn_ord);

extern void edc_turn_free(struct edc_turn *turn);

/* ── Messages, through the store ──────────────────────────────────────────── */

/* Append one full message (takes ownership); cleans UTF-8 and fills ts/id/render, then
 * writes the row. Returns false when the store is not open -- the message is still
 * consumed, and the caller must NOT assume it landed. Deliberately loud (log_error):
 * losing a conversation silently is the one failure mode this refactor must not have. */
extern bool edc_turn_add_message(struct edc_turn *turn, cJSON *msg);
extern bool edc_turn_add_user_message(struct edc_turn *turn, const char *content);
/* The generic door for a PASS-produced message (an @file's contents, ...). */
extern bool edc_turn_add_extra_message(struct edc_turn *turn, int role, bool synthetic,
                                       const char *content);
extern bool edc_turn_add_assistant_message(struct edc_turn *turn, cJSON *msg);
/* A tool result in the LLM's context. */
extern bool edc_turn_add_tool_message(struct edc_turn *turn, const char *tool_call_id, const char *name,
                                      const char *content);
/* An assistant message asking for ONE tool call, in exactly the shape the provider
 * returns -- so a seeded call (a `!cmd` task) is indistinguishable from one the model
 * asked for. |args| is the JSON argument object, serialized into the API's `arguments`
 * STRING field; NULL = "{}". */
extern bool edc_turn_add_assistant_tool_call(struct edc_turn *turn, const char *call_id, const char *name,
                                             cJSON *args);

/* ── Reading them back ────────────────────────────────────────────────────── */

/* Message |idx| (0-based), a fresh object the CALLER owns, or NULL past the end. */
extern cJSON *edc_turn_msg_at(const struct edc_turn *turn, int idx);
/* [from, to) in one query. |to| < 0 means "to the end". A dense array the caller frees
 * with cJSON_Delete -- this is a query result, not a cache, so it must not outlive the
 * work that needed it. */
extern cJSON *edc_turn_msgs(const struct edc_turn *turn, int from, int to);
/* Token estimate of [from, msg_count): a SUM over the rows, never a re-estimate walk. */
extern long long edc_turn_tokens_from(const struct edc_turn *turn, int from);
/* Drop message |new_len| onwards (revert). */
extern bool edc_turn_truncate(struct edc_turn *turn, int new_len);
/* Remove every message (turn discarded). */
extern bool edc_turn_clear(struct edc_turn *turn);

/* Render one message as a prompt line for the fold corpus / extraction material:
 * "<ts>  <role>: <content>", plus one "  -> call <name>(<arguments>)" line per
 * assistant tool_call. The tool arguments are knowledge the content alone lacks, so
 * both materials keep them. Returns a fresh sds (caller frees). */
extern sds edc_turn_render_message(const cJSON *msg);
#endif
