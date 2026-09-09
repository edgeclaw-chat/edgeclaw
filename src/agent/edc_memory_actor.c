#include "edc_memory_actor.h"
#include "cJSON.h"
#include "edc_actor.h"
#include "edc_compat.h"
#include "edc_error.h"
#include "edc_file_utils.h"
#include "edc_history.h"
#include "edc_llm_actor.h"
#include "edc_string_utils.h"
#include "edc_task.h"
#include "edc_task_group.h"
#include "log.h"
#include "sds.h"
#include "utlist_fix.h"
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
        return false;
    }

    cJSON *history = edc_history_load(state->workspace, compress_index);
    if (!history)
    {
        log_error("load history failed");
        return false;
    }

    bool ret = true;
    const size_t history_size = cJSON_GetArraySize(history);
    if (!force && history_size < state->history_window)
    {
        ret = false;
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

    sds user_message = sdsnew("Process this conversation. summarizing it. and call the save_memory tool with your "
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
        ret = false;
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

    state->compress_index = compress_index + cJSON_GetArraySize(history);

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
        return false;
    }

    sdsfree(memory_file);
    return true;
}

static bool edc_memory_save_history_file(const char *workspace, const char *history)
{
    sds history_file = edc_path_join(workspace, "memory", "HISTORY.md", NULL);
    FILE *fout = fopen(history_file, "a+");
    sdsfree(history_file);

    if (fout == NULL)
    {
        log_error("open history file failed");
        return false;
    }

    fprintf(fout, "%s\n", history);
    fflush(fout);
    fclose(fout);
    return true;
}

static bool edc_memory_do_memory_save(edc_actor_t *actor, const edc_actor_msg_t *msg)
{
    struct edc_memory_actor_state *state = actor->state;
    struct edc_llm_actor_resp *resp = msg->data;
    sds update_time = NULL;

    bool ret = true;
    if (resp->error)
    {
        log_error("memory compress failed: %s", resp->error);
        ret = false;
        goto exit_;
    }

    if (resp->reason != BC_LLM_FINISH_REASON_TOOL_CALL)
    {
        log_error("memory compress failed: reason is not tool call");
        ret = false;
        goto exit_;
    }

    for (unsigned i = 0; i < utarray_len(resp->tool_calls); i++)
    {
        struct edc_llm_chat_tool_call *tool_call = (struct edc_llm_chat_tool_call *)utarray_eltptr(resp->tool_calls, i);
        char *s = cJSON_Print(tool_call->function.arguments);
        log_info("edc_memory_actor: call tool %s with args %s", tool_call->function.name, s);
        cJSON_free(s);
    }

    for (unsigned i = 0; i < utarray_len(resp->tool_calls); i++)
    {
        struct edc_llm_chat_tool_call *tool_call = (struct edc_llm_chat_tool_call *)utarray_eltptr(resp->tool_calls, i);
        if (strcmp(tool_call->function.name, "save_memory") != 0)
        {
            ret = false;
            goto exit_;
        }

        cJSON *args = tool_call->function.arguments;
        cJSON *history_entry = cJSON_GetObjectItem(args, "history_entry");
        cJSON *memory_update = cJSON_GetObjectItem(args, "memory_update");
        if (!history_entry || !memory_update)
        {
            log_error("memory compress failed: missing history_entry or memory_update");
            ret = false;
            goto exit_;
        }

        if (!cJSON_IsString(history_entry) || !cJSON_IsString(memory_update))
        {
            log_error("memory compress failed: history_entry or memory_update is not string");
            ret = false;
            goto exit_;
        }

        char *history = cJSON_GetStringValue(history_entry);
        char *memory = cJSON_GetStringValue(memory_update);

        if (!memory || !history)
        {
            ret = false;
            goto exit_;
        }

        if (!edc_memory_save_memory_file(state->workspace, memory))
        {
            ret = false;
            goto exit_;
        }

        if (!edc_memory_save_history_file(state->workspace, history))
        {
            ret = false;
            goto exit_;
        }
    }

    update_time = edc_get_timestamp();
    if (!edc_history_set_info(state->workspace, state->compress_index, update_time))
    {
        ret = false;
        goto exit_;
    }

exit_:
    if (update_time)
        sdsfree(update_time);
    return ret;
}

/* ══════════════════════════════════════════════════════════════
 * L1 折叠: task turns → group memory
 * 触发 (edc_agent_cb_trigger_memory) 只看未折叠消息数 ≥ 阈值 (默认 200);
 * 真正折叠时按半量策略: 只把最老的一半未折叠消息 (≈ 阈值/2, 即 ~100 条)
 * 交给 LLM 做摘要, 新的一半保持未折叠细节 —— 每次总是一半记忆被压缩、
 * 一半记忆留细节, 上下文里仍有明细消息可读, 不会整批全被摘要化。
 * 完成后置 turns_folded 并推进 fold_cursor; max_seq 只覆盖被选中的最老
 * task, 未选中的 task 保持 turns_folded=false, 下一轮再压。
 * 注意: 折叠只处理 turns, 绝不触碰 task->output (JOIN 仍可消费)。
 * ══════════════════════════════════════════════════════════════ */

static sds edc_memory_build_fold_prompt(struct edc_task_group *group, uint32_t *out_max_seq)
{
    if (!group)
        return NULL;

    struct edc_task **tasks = NULL;
    size_t n = 0, cap = 0;
    struct edc_task *t;
    LL_FOREACH2(group->members, t, group_next)
    {
        if (!edc_task_is_foldable(t))
            continue;
        if (n == cap)
        {
            cap = cap ? cap * 2 : 8;
            tasks = realloc(tasks, cap * sizeof(*tasks));
        }
        tasks[n++] = t;
    }

    if (n == 0)
    {
        log_debug("L1 fold: group %s has no foldable tasks", group->group_id);
        free(tasks);
        return NULL;
    }

    /* 按 completion_seq 升序 (插入排序, task 数通常很小) */
    for (size_t i = 1; i < n; i++)
    {
        struct edc_task *key = tasks[i];
        size_t j = i;
        while (j > 0 && tasks[j - 1]->completion_seq > key->completion_seq)
        {
            tasks[j] = tasks[j - 1];
            j--;
        }
        tasks[j] = key;
    }

    /* 半量策略: 触发后不是把攒满的未折叠消息整批全压, 而是只压最老的
     * 一半 (≈ unfolded/2; 攒满 200 即压 ~100), 剩下一半保持未折叠细节。
     * 折叠粒度为 task (整段 turns 一起摘要), 故按 completion_seq 升序取
     * "最老、且累计消息数不超过预算"的 task 前缀; max_seq 取边界 task,
     * finish 阶段只会折叠 seq ≤ max_seq 的 task, 新的一半自然留下。 */
    size_t budget = edc_task_group_unfolded_msg_count(group) / 2;
    size_t sel = 0, folded_msgs = 0, total_msgs = 0;
    for (size_t i = 0; i < n; i++)
        total_msgs += edc_task_turn_msg_count(tasks[i]);
    while (sel < n)
    {
        size_t m = edc_task_turn_msg_count(tasks[sel]);
        if (sel > 0 && folded_msgs + m > budget)
            break; /* 再加就超过一半 → 留给下一轮当细节 */
        folded_msgs += m;
        sel++;
    }
    if (sel == 0 && n > 0)
    {
        /* 兜底: 最老单个 task 就超预算 (正常触发时 unfolded ≥ 200, 不会走到) */
        folded_msgs = edc_task_turn_msg_count(tasks[0]);
        sel = 1;
    }
    log_info("L1 fold: group %s fold oldest %zu/%zu tasks (%zu/%zu msgs), keep %zu msgs as detail", group->group_id,
             sel, n, folded_msgs, total_msgs, total_msgs - folded_msgs);

    if (out_max_seq)
        *out_max_seq = tasks[sel - 1]->completion_seq;

    sds prompt = sdsempty();
    prompt = sdscat(prompt, "## Existing Group Memory\n");
    prompt = sdscat(prompt, (group->memory && sdslen(group->memory) > 0) ? group->memory : "(empty)");
    prompt = sdscat(prompt, "\n\n## New Completed Tasks\n");

    for (size_t i = 0; i < sel; i++)
    {
        struct edc_task *tk = tasks[i];
        prompt = sdscatprintf(prompt, "\n### [task %s] %s\n", tk->task_id, tk->description ? tk->description : "");

        for (size_t qi = tk->turns.head; qi < tk->turns.tail; qi++)
        {
            struct edc_turn *turn = tk->turns.data[qi];
            for (size_t mj = turn->messages.head; mj < turn->messages.tail; mj++)
            {
                cJSON *m = turn->messages.data[mj];
                cJSON *role = cJSON_GetObjectItem(m, "role");
                cJSON *content = cJSON_GetObjectItem(m, "content");
                if (role && content && cJSON_IsString(role) && cJSON_IsString(content))
                    prompt = sdscatprintf(prompt, "%s: %s\n", role->valuestring, content->valuestring);
            }
        }

        if (tk->output)
            prompt = sdscatprintf(prompt, "output: %s\n", tk->output);
    }

    prompt = sdscat(prompt,
                    "\nMerge the tasks above into the Group Memory. Output ONLY the merged Group Memory as concise "
                    "markdown.");

    free(tasks);
    return prompt;
}

static bool edc_memory_do_group_fold(edc_actor_t *actor, edc_actor_msg_t *msg)
{
    struct edc_memory_actor_state *state = actor->state;
    struct edc_task_group *group = msg->data;
    if (!group)
        return false;

    uint32_t max_seq = 0;
    sds prompt = edc_memory_build_fold_prompt(group, &max_seq);
    if (!prompt)
        return false;

    log_info("L1 fold start: group %s prompt=%zu bytes, max_seq=%u", group->group_id, sdslen(prompt), max_seq);

    cJSON *system = cJSON_CreateObject();
    cJSON_AddStringToObject(system, "role", "system");
    cJSON_AddStringToObject(system, "content",
                            "You are a memory summarizer. Merge new task history into a concise Group Memory markdown.");

    cJSON *user = cJSON_CreateObject();
    cJSON_AddStringToObject(user, "role", "user");
    cJSON_AddStringToObject(user, "content", prompt);
    sdsfree(prompt);

    cJSON *messages = cJSON_CreateArray();
    cJSON_AddItemToArray(messages, system);
    cJSON_AddItemToArray(messages, user);

    struct edc_llm_actor_req *req = edc_llm_actor_req_new(messages, NULL, NULL);
    req->history_owned = true;

    state->fold_group = group;
    state->fold_max_seq = max_seq;

    edc_actor_send(state->llm_actor,
                   edc_actor_msg_new(BC_ACTOR_MSG_LLM_REQUEST, actor, msg->request_id, req,
                                     (edc_actor_data_free_fn)edc_llm_actor_req_free));
    return true;
}

static void edc_memory_finish_group_fold(edc_actor_t *actor, edc_actor_msg_t *msg)
{
    struct edc_memory_actor_state *state = actor->state;
    struct edc_llm_actor_resp *resp = msg->data;
    struct edc_task_group *group = state->fold_group;
    bool success = false;

    if (group && resp && !resp->error && resp->content && sdslen(resp->content) > 0)
    {
        if (group->memory)
            sdsfree(group->memory);
        group->memory = sdsdup(resp->content);

        size_t folded_tasks = 0, folded_msgs = 0;
        struct edc_task *t;
        LL_FOREACH2(group->members, t, group_next)
        {
            if (t->state == TASK_COMPLETED && !t->turns_folded && t->completion_seq <= state->fold_max_seq)
            {
                t->turns_folded = true;
                folded_tasks++;
                folded_msgs += edc_task_turn_msg_count(t);
            }
        }
        group->fold_cursor += folded_msgs;
        success = true;
        log_info("L1 fold done: group %s folded %zu tasks (%zu msgs)", group->group_id, folded_tasks, folded_msgs);
    }
    else
    {
        if (!group)
            log_error("L1 fold failed: fold_group is NULL");
        else if (!resp)
            log_error("L1 fold failed: group %s no response", group->group_id);
        else if (resp->error)
            log_error("L1 fold failed: group %s LLM error: %s", group->group_id,
                      resp->error->message ? resp->error->message : "unknown");
        else
            log_error("L1 fold failed: group %s empty content", group->group_id);
    }

    /* 折叠成功后通知 scheduler 持久化 (turns_folded/memory/fold_cursor 写回 session.json) */
    if (success && state->fold_msg && state->fold_msg->from)
    {
        edc_actor_send(state->fold_msg->from,
                       edc_actor_msg_new(BC_ACTOR_MSG_GROUP_FOLD_DONE, actor, state->fold_msg->request_id, NULL, NULL));
    }
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
        if (!state->compressing && !state->fold_group)
        {
            state->compressing = true;
            state->compress_msg = msg;
            if (!edc_memory_do_compress(actor, msg, msg->data != NULL))
            {
                state->compressing = false;
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
    case BC_ACTOR_MSG_GROUP_FOLD:
        if (!state->fold_group && !state->compressing)
        {
            state->fold_group = msg->data;
            state->fold_msg = msg;
            if (!edc_memory_do_group_fold(actor, msg))
            {
                state->fold_group = NULL;
                state->fold_msg = NULL;
                edc_actor_msg_free(msg);
            }
        }
        else
        {
            edc_actor_msg_free(msg);
        }
        break;
    case BC_ACTOR_MSG_LLM_RESPONSE: {
        if (state->fold_group)
        {
            /* group fold 的 LLM 响应 */
            edc_memory_finish_group_fold(actor, msg);
            state->fold_group = NULL;
            edc_actor_msg_free(state->fold_msg);
            state->fold_msg = NULL;
            state->fold_max_seq = 0;
            edc_actor_msg_free(msg);
        }
        else if (state->compressing)
        {
            /* 全局压缩的 LLM 响应 */
            if (edc_memory_do_memory_save(actor, msg))
            {
                if (state->compress_msg->from)
                {
                    edc_actor_send(state->compress_msg->from,
                                   edc_actor_msg_new(BC_ACTOR_MSG_MEMORY_DONE, actor, state->compress_msg->request_id,
                                                     NULL, NULL));
                }
            }

            state->compressing = false;
            edc_actor_msg_free(msg);
            edc_actor_msg_free(state->compress_msg);
            cJSON_Delete(state->compress_msg_lst);
            state->compress_msg_lst = NULL;
            state->compress_msg = NULL;
            state->compress_index = 0;
        }
        else
        {
            edc_actor_msg_free(msg);
        }
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
    state->compressing = false;
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
