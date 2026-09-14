#include "edc_memory_actor.h"
#include "cJSON.h"
#include "edc_actor.h"
#include "edc_compat.h"
#include "edc_error.h"
#include "edc_file_utils.h"
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
    free(state);
    edc_actor_free(actor);
}

/* ══════════════════════════════════════════════════════════════
 * L1 折叠: task turns → group memory
 * 触发 (edc_agent_cb_trigger_memory) 只看未折叠消息数 ≥ 阈值 (默认 200);
 * 真正折叠时按半量策略: 只把最老的一半未折叠消息 (≈ 阈值/2, 即 ~100 条)
 * 交给 LLM 做摘要, 新的一半保持未折叠细节 —— 每次总是一半记忆被压缩、
 * 一半记忆留细节, 上下文里仍有明细消息可读, 不会整批全被摘要化。
 * 完成后置 turns_folded 并推进折叠水位; max_seq 只覆盖被选中的最老
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

    prompt =
        sdscat(prompt, "\nMerge the tasks above into the Group Memory. Output ONLY the merged Group Memory as concise "
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
    cJSON_AddStringToObject(
        system, "content", "You are a memory summarizer. Merge new task history into a concise Group Memory markdown.");

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

    edc_actor_send(state->llm_actor, edc_actor_msg_new(BC_ACTOR_MSG_LLM_REQUEST, actor, msg->request_id, req,
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
            if (edc_task_is_completed(t) && !t->turns_folded && t->completion_seq <= state->fold_max_seq)
            {
                t->turns_folded = true;
                folded_tasks++;
                folded_msgs += edc_task_turn_msg_count(t);
            }
        }
        group->folded_through_completion_seq = state->fold_max_seq;
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

    /* 折叠成功后通知 scheduler 持久化 (turns_folded/memory/折叠水位 写回 session.json) */
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
    case BC_ACTOR_MSG_GROUP_FOLD:
        if (!state->fold_group)
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

edc_actor_t *edc_memory_actor_new(const char *workspace, edc_actor_t *llm_actor)
{
    struct edc_memory_actor_state *state = g_new0(struct edc_memory_actor_state, 1);
    state->workspace = workspace;
    state->llm_actor = llm_actor;
    return edc_actor_new("edc_memory_actor", edc_memory_actor_handler, state);
}
