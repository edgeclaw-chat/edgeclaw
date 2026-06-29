#include "sds.h"
#include "edc_error.h"
#include "edc_memory_actor.h"
#include "edc_actor.h"
#include "edc_compat.h"
#include "edc_file_utils.h"
#include "edc_string_utils.h"
#include "edc_history.h"
#include "edc_llm_actor.h"
#include "cJSON.h"
#include "log.h"
#include <stdio.h>
#include <string.h>

void edc_memory_actor_free(edc_actor_t *actor)
{
    struct edc_memory_actor_state *state = (struct edc_memory_actor_state *)actor->state;
    if (state->compress_msg)
    {
        edc_actor_msg_free(state->compress_msg);
    }

    if (state->compress_msg_lst)
    {
        cJSON_Delete(state->compress_msg_lst);
    }

    if (state->compress_tools)
    {
        cJSON_Delete(state->compress_tools);
    }

    free(state);
    edc_actor_free(actor);
}

static sds edc_memory_get_tools_used(cJSON *msg)
{
    cJSON *tool_calls = cJSON_GetObjectItem(msg, "tool_calls");
    if (!tool_calls || !cJSON_IsArray(tool_calls))
    {
        return NULL;
    }

    int n = cJSON_GetArraySize(tool_calls);
    char **tools = EDC_NEW(char *, n + 1);

    int valid = 0;
    for (int i = 0; i < n; i++)
    {
        cJSON *tool_call = cJSON_GetArrayItem(tool_calls, i);
        cJSON *function = cJSON_GetObjectItem(tool_call, "function");
        if (!function || !cJSON_IsObject(function))
        {
            continue;
        }

        cJSON *name = cJSON_GetObjectItem(function, "name");
        if (!name || !cJSON_IsString(name))
        {
            continue;
        }

        tools[valid++] = cJSON_GetStringValue(name);
    }

    tools[valid] = NULL;
    sds tools_used = edc_str_joinv(", ", tools);
    free(tools);

    return tools_used;
}

static cJSON *edc_memory_save_tool()
{
    cJSON *func = cJSON_CreateObject();
    cJSON_AddStringToObject(func, "name", "save_memory");
    cJSON_AddStringToObject(func, "description", "Save the memory consolidation result to persistent storage.");
    cJSON *params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "type", "object");
    cJSON *properties = cJSON_CreateObject();

    cJSON *history_entry = cJSON_CreateObject();
    cJSON_AddStringToObject(history_entry, "type", "string");
    cJSON_AddStringToObject(history_entry, "description",
                            "A paragraph (2-5 sentences) summarizing key events/decisions/topics. "
                            "Start with [YYYY-MM-DD HH:MM]. Include detail useful for grep search.");

    cJSON_AddItemToObject(properties, "history_entry", history_entry);

    cJSON *memory_update = cJSON_CreateObject();
    cJSON_AddStringToObject(memory_update, "type", "string");
    cJSON_AddStringToObject(memory_update, "description",
                            "Full updated long-term memory as markdown. Include all existing "
                            "facts plus new ones. Return unchanged if nothing new.");

    cJSON_AddItemToObject(properties, "memory_update", memory_update);

    cJSON_AddItemToObject(params, "properties", properties);

    cJSON *required = cJSON_CreateArray();
    cJSON_AddItemToArray(required, cJSON_CreateString("history_entry"));
    cJSON_AddItemToArray(required, cJSON_CreateString("memory_update"));
    cJSON_AddItemToObject(params, "required", required);
    cJSON_AddItemToObject(func, "parameters", params);

    cJSON *tool = cJSON_CreateObject();
    cJSON_AddStringToObject(tool, "type", "function");
    cJSON_AddItemToObject(tool, "function", func);

    cJSON *tools = cJSON_CreateArray();
    cJSON_AddItemToArray(tools, tool);

    return tools;
}

static bool edc_memory_do_compress(edc_actor_t *actor, edc_actor_msg_t *msg, bool force)
{
    struct edc_memory_actor_state *state = actor->state;

    size_t compress_index = 0;
    if (!edc_history_get_info(state->workspace, &compress_index, NULL))
    {
        return FALSE;
    }

    cJSON *history = edc_history_load(state->workspace, compress_index);
    if (!history)
    {
        log_error("load history failed");
        return FALSE;
    }

    bool ret = TRUE;
    const size_t history_size = cJSON_GetArraySize(history);
    if (!force && history_size < state->history_window)
    {
        ret = FALSE;
        goto exit_free_history;
    }

    cJSON *half_window = cJSON_CreateArray();
    const int half_count = state->history_window / 2;
    for (int i = 0; i < half_count && cJSON_GetArraySize(history) > 0; i++)
    {
        cJSON *msg = cJSON_DetachItemFromArray(history, 0);
        cJSON_AddItemToArray(half_window, msg);
    }

    // Ensure compress boundary doesn't split an assistant(tool_calls) + tool sequence.
    // If the next message in history is a 'tool' message, pull it into the compressed window too.
    while (cJSON_GetArraySize(history) > 0)
    {
        cJSON *next_msg = cJSON_GetArrayItem(history, 0);
        cJSON *role = cJSON_GetObjectItem(next_msg, "role");
        if (role && cJSON_IsString(role) && strcmp(cJSON_GetStringValue(role), "tool") == 0)
        {
            cJSON *detached = cJSON_DetachItemFromArray(history, 0);
            cJSON_AddItemToArray(half_window, detached);
        }
        else
        {
            break;
        }
    }

    cJSON_Delete(history);
    history = half_window;

    state->compress_index = compress_index + cJSON_GetArraySize(history);
    sds user_message =
        sdsnew("Process this conversation. summarizing it. and call the save_memory tool with your "
                     "consolidation. NOTICE: !!!! you can only call save_memory tool\n\n");

    sds memory_file = edc_path_join(state->workspace, "memory", "MEMORY.md", NULL);
    EdcError *gerror = NULL;
    sds memory = edc_file_read(memory_file, &gerror);

    if (!memory)
    {
        if (gerror)
        {
            log_error("get memory file content failed: %s", gerror->message);
            edc_error_free(gerror);
        }
        ret = FALSE;
        sdsfree(memory_file);
        goto exit_free_user_message;
    }

    sdsfree(memory_file);

    user_message = sdscat(user_message, "## Current Long-term Memory\n");
    if (memory)
    {
        user_message = sdscat(user_message, memory);
    sdsfree(memory);
    }
    else
    {
        user_message = sdscat(user_message, "(empty)");
    }

    user_message = sdscat(user_message, "\n## Conversation History\n");
    for (int i = 0; i < cJSON_GetArraySize(history); i++)
    {
        cJSON *msg = cJSON_GetArrayItem(history, i);
        cJSON *role = cJSON_GetObjectItem(msg, "role");
        cJSON *content = cJSON_GetObjectItem(msg, "content");
        cJSON *timestamp = cJSON_GetObjectItem(msg, "timestamp");

        if (!role || !content)
        {
            continue;
        }

        if (!timestamp)
        {
            sds ts = edc_get_timestamp();
            cJSON_AddStringToObject(msg, "timestamp", ts);
            sdsfree(ts);
            timestamp = cJSON_GetObjectItem(msg, "timestamp");
        }

        if (!cJSON_IsString(role) || !cJSON_IsString(content) || !cJSON_IsString(timestamp))
        {
            continue;
        }

        sds line = sdsempty();
        sds tools_used = edc_memory_get_tools_used(msg);
        if (tools_used)
        {
            line = sdscatprintf(line, "[%s] %s[called: %s]: %s\n", cJSON_GetStringValue(timestamp),
                                   cJSON_GetStringValue(role), tools_used, cJSON_GetStringValue(content));
            sdsfree(tools_used);
        }
        else
        {
            line = sdscatprintf(line, "[%s] %s: %s\n", cJSON_GetStringValue(timestamp), cJSON_GetStringValue(role),
                                   cJSON_GetStringValue(content));
        }

        user_message = sdscat(user_message, line);
        sdsfree(line);
    }

    user_message = sdscat(user_message, "\n# Notice\n\n !!!!! You can only call save_memory tool.!!!!!");

    cJSON *system = cJSON_CreateObject();
    cJSON_AddStringToObject(system, "role", "system");
    cJSON_AddStringToObject(system, "content",
                            "You are a memory compressing agent. Call the save_memory tool with your consolidation of "
                            "the conversation. NOTICE: !!!! You can only call save_memory tool.!!!!!!!");

    cJSON *user = cJSON_CreateObject();
    cJSON_AddStringToObject(user, "role", "user");
    cJSON_AddStringToObject(user, "content", user_message);

    cJSON *messages = cJSON_CreateArray();
    cJSON_AddItemToArray(messages, system);
    cJSON_AddItemToArray(messages, user);

    struct edc_llm_actor_req *req = g_new0(struct edc_llm_actor_req, 1);
    req->history = messages;
    req->tools = state->compress_tools;
    state->compress_msg_lst = messages;

    edc_actor_msg_t *llm_msg = edc_actor_msg_new(BC_ACTOR_MSG_LLM_REQUEST, actor, msg->request_id, req,
                                               (edc_actor_data_free_fn)edc_llm_actor_req_free);
    edc_actor_send(state->llm_actor, llm_msg);

exit_free_user_message:
    sdsfree(user_message);
exit_free_history:
    cJSON_Delete(history);
    return ret;
}

static bool edc_memory_save_memory_file(const char *workspace, const char *memory)
{
    sds memory_file = edc_path_join(workspace, "memory", "MEMORY.md", NULL);
    EdcError *gerror = NULL;
    if (!edc_file_write(memory_file, memory, -1, &gerror))
    {
        log_error("save memory file failed: %s", gerror->message);
        sdsfree(memory_file);
        edc_error_free(gerror);
        return FALSE;
    }

    sdsfree(memory_file);
    return TRUE;
}

static bool edc_memory_save_history_file(const char *workspace, const char *history)
{
    sds history_file = edc_path_join(workspace, "memory", "HISTORY.md", NULL);
    FILE *fout = fopen(history_file, "a+");
    sdsfree(history_file);

    if (fout == NULL)
    {
        log_error("open history file failed");
        return FALSE;
    }

    fprintf(fout, "%s\n", history);
    fflush(fout);
    fclose(fout);
    return TRUE;
}

static bool edc_memory_do_memory_save(edc_actor_t *actor, const edc_actor_msg_t *msg)
{
    struct edc_memory_actor_state *state = actor->state;
    struct edc_llm_actor_resp *resp = msg->data;
    sds update_time = NULL;

    bool ret = TRUE;
    if (resp->error)
    {
        log_error("memory compress failed: %s", resp->error);
        ret = FALSE;
        goto exit_;
    }

    if (resp->reason != BC_LLM_FINISH_REASON_TOOL_CALL)
    {
        log_error("memory compress failed: reason is not tool call");
        ret = FALSE;
        goto exit_;
    }

    for (unsigned i = 0; i < utarray_len(resp->tool_calls); i++)
    {
        struct edc_llm_chat_tool_call *tool_call = (struct edc_llm_chat_tool_call*)utarray_eltptr(resp->tool_calls, i);
        char *s = cJSON_Print(tool_call->function.arguments);
        log_info("edc_memory_actor: call tool %s with args %s", tool_call->function.name, s);
        cJSON_free(s);
    }

    for (unsigned i = 0; i < utarray_len(resp->tool_calls); i++)
    {
        struct edc_llm_chat_tool_call *tool_call = (struct edc_llm_chat_tool_call*)utarray_eltptr(resp->tool_calls, i);
        if (strcmp(tool_call->function.name, "save_memory") != 0)
        {
            ret = FALSE;
            goto exit_;
        }

        cJSON *args = tool_call->function.arguments;
        cJSON *history_entry = cJSON_GetObjectItem(args, "history_entry");
        cJSON *memory_update = cJSON_GetObjectItem(args, "memory_update");
        if (!history_entry || !memory_update)
        {
            log_error("memory compress failed: missing history_entry or memory_update");
            ret = FALSE;
            goto exit_;
        }

        if (!cJSON_IsString(history_entry) || !cJSON_IsString(memory_update))
        {
            log_error("memory compress failed: history_entry or memory_update is not string");
            ret = FALSE;
            goto exit_;
        }

        char *history = cJSON_GetStringValue(history_entry);
        char *memory = cJSON_GetStringValue(memory_update);

        if (!memory || !history)
        {
            ret = FALSE;
            goto exit_;
        }

        if (!edc_memory_save_memory_file(state->workspace, memory))
        {
            ret = FALSE;
            goto exit_;
        }

        if (!edc_memory_save_history_file(state->workspace, history))
        {
            ret = FALSE;
            goto exit_;
        }
    }

    update_time = edc_get_timestamp();
    if (!edc_history_set_info(state->workspace, state->compress_index, update_time))
    {
        ret = FALSE;
        goto exit_;
    }

exit_:
    if (update_time)
        sdsfree(update_time);
    return ret;
}

static void edc_memory_actor_handler(edc_actor_t *actor, edc_actor_msg_t *msg)
{
    struct edc_memory_actor_state *state = (struct edc_memory_actor_state *)actor->state;
    switch (msg->type)
    {
    case BC_ACTOR_MSG_START:
        edc_actor_msg_free(msg);
        break;
    case BC_ACTOR_MSG_MEMORY_COMPRESS:
        if (!state->compressing)
        {
            state->compressing = TRUE;
            state->compress_msg = msg;
            if (!edc_memory_do_compress(actor, msg, msg->data != NULL))
            {
                state->compressing = FALSE;
                state->compress_msg = NULL;
                state->compress_index = 0;
                edc_actor_msg_free(msg);
            }
        }
        else
        {
            edc_actor_msg_free(msg);
        }
        break;
    case BC_ACTOR_MSG_LLM_RESPONSE: {
        if (edc_memory_do_memory_save(actor, msg))
        {
            if (state->compress_msg->from)
            {
                edc_actor_send(state->compress_msg->from, edc_actor_msg_new(BC_ACTOR_MSG_MEMORY_DONE, actor,
                                                                          state->compress_msg->request_id, NULL, NULL));
            }
        }

        state->compressing = FALSE;
        edc_actor_msg_free(msg);
        edc_actor_msg_free(state->compress_msg);
        cJSON_Delete(state->compress_msg_lst);
        state->compress_msg_lst = NULL;
        state->compress_msg = NULL;
        state->compress_index = 0;
    }
    break;
    default:
        break;
    }
}

edc_actor_t *edc_memory_actor_new(const char *workspace, const size_t history_window, edc_actor_t *llm_actor)
{
    struct edc_memory_actor_state *state = g_new0(struct edc_memory_actor_state, 1);
    state->workspace = workspace;
    state->llm_actor = llm_actor;
    state->compressing = FALSE;
    state->compress_msg_lst = NULL;
    state->compress_tools = edc_memory_save_tool();
    state->compress_index = 0;
    state->history_window = history_window;
    return edc_actor_new("edc_memory_actor", edc_memory_actor_handler, state);
}

bool edc_memory_actor_is_compressing(edc_actor_t *actor)
{
    struct edc_memory_actor_state *state = (struct edc_memory_actor_state *)actor->state;
    return state->compressing;
}
