#include "edc_turn.h"
#include "edc_task.h"  /* edc_id_str() for log lines */
#include "edc_state_store.h"
#include "edc_token_estimate.h"
#include "edc_datetime_utils.h"
#include "edc_compat.h"
#include "edc_string_utils.h"
#include "log.h"

#include <inttypes.h> /* PRId64 / PRIu64 */
#include <stdio.h>
#include <time.h>

/* A stable, session-unique identity for one message.
 *
 * Why an id at all: the storage key (task_id, turn_ord, msg_ord) is POSITIONAL -- it says
 * where a message sits, not which message it is, and it cannot be referenced from outside
 * (a recall, a citation, a "which message did that come from" audit). This adds the
 * identity. It lives INSIDE the message JSON so it travels with the message through every
 * copy and is written back unchanged after a restart -- a regenerated id would silently
 * break the store's upsert, which keys on it.
 *
 * Format: "m<unix_ms>_<seq>". Milliseconds + a per-process sequence, so two messages added
 * in the same millisecond still differ. Across restarts the milliseconds differ, which is
 * why the sequence can restart at 0. It is also sortable, which makes logs readable -- a
 * raw UUID would not be.
 *
 * NOT sent to the model: the request builder allowlists wire keys (see
 * kWireMessageKeys in edc_llm_actor.c) and drops anything else, so this field cannot leak
 * into an API payload. */
static uint64_t g_msg_seq;

static sds edc_message_new_id(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
    {
        ts.tv_sec = time(NULL);
        ts.tv_nsec = 0;
    }
    const uint64_t ms = (uint64_t)ts.tv_sec * 1000ULL +
                                  (uint64_t)(ts.tv_nsec / 1000000L);
    return sdscatprintf(sdsempty(), "m%" PRIu64 "_%" PRIu64, ms, g_msg_seq++);
}

void edc_turn_set_key(struct edc_turn *turn, int64_t session_id, int64_t group_id,
                      int64_t task_id, int turn_ord)
{
    if (!turn)
        return;
    turn->task_id = task_id;
    turn->session_id = session_id;
    turn->group_id = group_id;
    turn->turn_ord = turn_ord;
}

bool edc_turn_add_message(struct edc_turn *turn, cJSON *msg)
{
    if (!turn || !msg)
    {
        if (msg)
            cJSON_Delete(msg);
        return false;
    }

    cJSON *content = cJSON_GetObjectItem(msg, "content");
    sds valid_utf8 = edc_str_utf8_make_valid(cJSON_GetStringValue(content), -1);
    cJSON *valid_content = cJSON_CreateString(valid_utf8);
    sdsfree(valid_utf8);
    cJSON_ReplaceItemInObject(msg, "content", valid_content);

    /* A message carries its own timestamp and render preference, persisted with it --
     * no parallel timestamps queue (which was lost on restart and needed indices aligned
     * with messages). */
    if (!cJSON_HasObjectItem(msg, "ts"))
    {
        sds ts = edc_datetime_timestamp();
        cJSON_AddStringToObject(msg, "ts", ts ? ts : "");
        sdsfree(ts);
    }
    /* Identity, assigned once at creation and never regenerated (see edc_message_new_id).
     * Absent on messages restored from an older store, so it is a conditional add: a
     * message that already has one keeps it, which is what keeps the store's upsert key
     * stable across a restart. */
    if (!cJSON_HasObjectItem(msg, "id"))
    {
        sds id = edc_message_new_id();
        cJSON_AddStringToObject(msg, "id", id);
        sdsfree(id);
    }
    if (!cJSON_HasObjectItem(msg, "render"))
        cJSON_AddStringToObject(msg, "render", "plain");

    /* The write. Position is msg_count: turns only ever grow at the tail, so this is the
     * same msg_ord the store's unique index expects, and a replayed append overwrites
     * instead of duplicating. The parent rows are created here because the FK chain has to
     * exist before the message row can (see edc_state_ensure_turn). */
    const int ord = turn->msg_count;
    const bool ok = edc_state_ensure_turn(turn->session_id, turn->group_id, turn->task_id,
                                          turn->turn_ord) &&
                    edc_msg_append(turn->task_id, turn->turn_ord, ord, msg,
                                   edc_token_estimate_msg(msg));
    if (ok)
    {
        turn->msg_count++;
        /* Invariant from the header: neither watermark may exceed the message count. */
        if (turn->memo_prefix > turn->msg_count)
            turn->memo_prefix = turn->msg_count;
        if (turn->folded_prefix > turn->msg_count)
            turn->folded_prefix = turn->msg_count;
    }
    else
    {
        /* Loud, and not fatal to the caller: the conversation would otherwise look fine
         * while quietly missing a message, which is the failure this refactor must not
         * introduce. */
        log_error("turn: message NOT persisted (task=%s turn=%d ord=%d) -- is the state "
                  "store open?",
                  edc_id_str(turn->task_id), turn->turn_ord, ord);
    }

    cJSON_Delete(msg); /* the row owns the JSON now */
    return ok;
}

bool edc_turn_add_user_message(struct edc_turn *turn, const char *content)
{
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "role", "user");
    cJSON_AddStringToObject(msg, "content", content);
    return edc_turn_add_message(turn, msg);
}

bool edc_turn_add_extra_message(struct edc_turn *turn, int role, bool synthetic, const char *content)
{
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "role", role == EDC_MSG_SYSTEM ? "system" : "user");
    cJSON_AddStringToObject(msg, "content", content);
    if (synthetic)
        cJSON_AddTrueToObject(msg, "synthetic");
    return edc_turn_add_message(turn, msg);
}

bool edc_turn_add_assistant_message(struct edc_turn *turn, cJSON *msg)
{
    return edc_turn_add_message(turn, msg);
}

bool edc_turn_add_tool_message(struct edc_turn *turn, const char *tool_call_id, const char *name, const char *content)
{
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "role", "tool");
    cJSON_AddStringToObject(msg, "tool_call_id", tool_call_id);
    cJSON_AddStringToObject(msg, "name", name);
    cJSON_AddStringToObject(msg, "content", content);
    return edc_turn_add_message(turn, msg);
}

bool edc_turn_add_assistant_tool_call(struct edc_turn *turn, const char *call_id, const char *name, cJSON *args)
{
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "role", "assistant");

    /* `arguments` is a STRING in the API, not an object. */
    char *args_s = args ? cJSON_PrintUnformatted(args) : NULL;
    cJSON *call = cJSON_CreateObject();
    cJSON_AddStringToObject(call, "id", call_id ? call_id : "");
    cJSON_AddStringToObject(call, "type", "function");
    cJSON *fn = cJSON_CreateObject();
    cJSON_AddStringToObject(fn, "name", name ? name : "");
    cJSON_AddStringToObject(fn, "arguments", args_s ? args_s : "{}");
    if (args_s)
        cJSON_free(args_s);
    cJSON_AddItemToObject(call, "function", fn);

    cJSON *calls = cJSON_CreateArray();
    cJSON_AddItemToArray(calls, call);
    cJSON_AddItemToObject(msg, "tool_calls", calls);

    /* No "content": edc_turn_add_message fills it with "" (the API accepts an empty
     * content next to tool_calls). */
    return edc_turn_add_message(turn, msg);
}

struct edc_turn *edc_turn_new()
{
    return g_new0(struct edc_turn, 1);
}

struct edc_turn *edc_turn_new_for(int64_t session_id, int64_t group_id,
                                  int64_t task_id, int turn_ord)
{
    struct edc_turn *turn = edc_turn_new();
    edc_turn_set_key(turn, session_id, group_id, task_id, turn_ord);
    return turn;
}

void edc_turn_free(struct edc_turn *turn)
{
    if (!turn)
        return;
    free(turn);
}

cJSON *edc_turn_msg_at(const struct edc_turn *turn, int idx)
{
    if (!turn || idx < 0 || idx >= turn->msg_count)
        return NULL;
    return edc_msg_get(turn->task_id, turn->turn_ord, idx);
}

cJSON *edc_turn_msgs(const struct edc_turn *turn, int from, int to)
{
    if (!turn)
        return NULL;
    if (to < 0)
        to = turn->msg_count;
    if (from < 0)
        from = 0;
    if (to <= from)
        return NULL;

    struct edc_msg_span span;
    if (!edc_msg_span_load(turn->task_id, turn->turn_ord, from, to, &span))
        return NULL;

    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < span.n; i++)
    {
        if (span.items[i])
            cJSON_AddItemToArray(arr, span.items[i]); /* transfer ownership */
    }
    /* What is left in the span is NULL slots (and the backing array). */
    free(span.items);

    if (cJSON_GetArraySize(arr) == 0)
    {
        cJSON_Delete(arr);
        return NULL;
    }
    return arr;
}

int64_t edc_turn_tokens_from(const struct edc_turn *turn, int from)
{
    if (!turn)
        return 0;
    return edc_msg_turn_tokens_from(turn->task_id, turn->turn_ord, from);
}

sds edc_turn_render_message(const cJSON *msg)
{
    if (!msg)
        return sdsempty();
    const char *role = cJSON_GetStringValue(cJSON_GetObjectItem((cJSON *)msg, "role"));
    const char *content = cJSON_GetStringValue(cJSON_GetObjectItem((cJSON *)msg, "content"));
    const char *ts = cJSON_GetStringValue(cJSON_GetObjectItem((cJSON *)msg, "ts"));
    sds line = sdscatprintf(sdsempty(), "%s  %s: %s", (ts && ts[0]) ? ts : "?", role ? role : "?",
                            content ? content : "");

    /* Assistant tool calls: name + arguments are knowledge the content alone lacks (a
     * tool-call message has empty content), so the fold corpus and the extraction
     * material keep them. */
    cJSON *calls = cJSON_GetObjectItem((cJSON *)msg, "tool_calls");
    if (calls && cJSON_IsArray(calls))
    {
        cJSON *call;
        cJSON_ArrayForEach(call, calls)
        {
            cJSON *fn = cJSON_GetObjectItem(call, "function");
            const char *name = fn ? cJSON_GetStringValue(cJSON_GetObjectItem(fn, "name")) : NULL;
            const char *args = fn ? cJSON_GetStringValue(cJSON_GetObjectItem(fn, "arguments")) : NULL;
            line = sdscatprintf(line, "\n  -> call %s(%s)", name ? name : "?", args ? args : "{}");
        }
    }
    return line;
}
