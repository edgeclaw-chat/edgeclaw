#include "edc_task.h"
#include "edc_compat.h"
#include "edc_context.h"
#include "utlist_fix.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ── 状态名映射 ────────────────────────────────────────────── */

const char *edc_task_state_name(edc_task_state state)
{
    switch (state)
    {
    case TASK_IDLE:
        return "IDLE";
    case TASK_WAITING_DEPS:
        return "WAITING_DEPS";
    case TASK_WAITING_INPUT:
        return "WAITING_INPUT";
    case TASK_LLM_PROCESSING:
        return "LLM_PROCESSING";
    case TASK_TOOL_CALLING:
        return "TOOL_CALLING";
    case TASK_SUSPENDED:
        return "SUSPENDED";
    case TASK_COMPLETED:
        return "COMPLETED";
    case TASK_CANCELLED:
        return "CANCELLED";
    case TASK_ERROR:
        return "ERROR";
    default:
        return "UNKNOWN";
    }
}

edc_task_state edc_task_classify_state(const char *name)
{
    if (!name)
        return TASK_IDLE;
    if (strcasecmp(name, "idle") == 0)
        return TASK_IDLE;
    if (strcasecmp(name, "waiting_deps") == 0)
        return TASK_WAITING_DEPS;
    if (strcasecmp(name, "waiting_input") == 0)
        return TASK_WAITING_INPUT;
    if (strcasecmp(name, "llm_processing") == 0)
        return TASK_LLM_PROCESSING;
    if (strcasecmp(name, "tool_calling") == 0)
        return TASK_TOOL_CALLING;
    if (strcasecmp(name, "suspended") == 0)
        return TASK_SUSPENDED;
    if (strcasecmp(name, "completed") == 0)
        return TASK_COMPLETED;
    if (strcasecmp(name, "cancelled") == 0)
        return TASK_CANCELLED;
    if (strcasecmp(name, "error") == 0)
        return TASK_ERROR;
    return TASK_IDLE;
}

/* ── 状态转换校验 ──────────────────────────────────────────── */

static bool edc_task_is_valid_transition(edc_task_state from, edc_task_state to)
{
    switch (from)
    {
    case TASK_IDLE:
        return to == TASK_WAITING_DEPS || to == TASK_WAITING_INPUT || to == TASK_CANCELLED;
    case TASK_WAITING_DEPS:
        return to == TASK_WAITING_INPUT || to == TASK_CANCELLED || to == TASK_ERROR;
    case TASK_WAITING_INPUT:
        return to == TASK_LLM_PROCESSING || to == TASK_CANCELLED || to == TASK_WAITING_DEPS;
    case TASK_LLM_PROCESSING:
        return to == TASK_TOOL_CALLING || to == TASK_COMPLETED || to == TASK_CANCELLED || to == TASK_ERROR;
    case TASK_TOOL_CALLING:
        return to == TASK_LLM_PROCESSING || to == TASK_COMPLETED || to == TASK_CANCELLED || to == TASK_ERROR ||
               to == TASK_SUSPENDED;
    case TASK_SUSPENDED:
        return to == TASK_LLM_PROCESSING || to == TASK_TOOL_CALLING || to == TASK_CANCELLED; /* resume */
    case TASK_COMPLETED:
        return false; /* terminal */
    case TASK_CANCELLED:
        return to == TASK_WAITING_INPUT; /* resume */
    case TASK_ERROR:
        return to == TASK_WAITING_INPUT || to == TASK_CANCELLED; /* resume or cancel */
    default:
        return false;
    }
}

/* ── 生命周期 ──────────────────────────────────────────────── */

struct edc_task *edc_task_new(const char *task_id, const char *session_id, const char *group_id, const char *desc,
                              edc_channel_type channel_type)
{
    struct edc_task *task = g_new0(struct edc_task, 1);
    task->task_id = strdup(task_id);
    task->session_id = strdup(session_id);
    task->group_id = strdup(group_id);
    task->description = strdup(desc ? desc : "");
    task->state = TASK_IDLE;
    task->is_group_leader = 0;
    task->channel_type = channel_type;

    edc_queue_init(&task->turns);

    return task;
}

struct edc_task *edc_task_fork(struct edc_task *parent, const char *task_id, const char *desc,
                               size_t fork_point_msg_count, edc_channel_type channel_type)
{
    if (!parent)
        return NULL;

    char auto_id[64];
    if (!task_id)
    {
        static int fork_seq = 0;
        snprintf(auto_id, sizeof(auto_id), "ft%d", ++fork_seq);
        task_id = auto_id;
    }

    struct edc_task *child = edc_task_new(task_id, parent->session_id, parent->group_id, desc, channel_type);
    child->forked_from = parent;
    child->fork_point_msg_count = fork_point_msg_count;
    child->state = TASK_WAITING_INPUT;
    return child;
}

static void edc_task_free_turns(struct edc_task *task)
{
    struct edc_turn *turn;
    while ((turn = edc_queue_pop(&task->turns)) != NULL)
    {
        edc_turn_free(turn);
    }

    if (task->active_turn)
    {
        edc_turn_free(task->active_turn);
        task->active_turn = NULL;
    }

    edc_queue_destroy(&task->turns);
}

static void edc_task_free_deps(struct edc_task *task)
{
    struct edc_task_dep *dep, *tmp;

    LL_FOREACH_SAFE(task->deps, dep, tmp)
    {
        if (dep->source)
        {
            struct edc_task_dep *sd, *sd_tmp;
            LL_FOREACH_SAFE(dep->source->dependents, sd, sd_tmp)
            {
                if (sd->target == task)
                {
                    LL_DELETE(dep->source->dependents, sd);
                    free(sd);
                }
            }
        }
        LL_DELETE(task->deps, dep);
        free(dep);
    }

    LL_FOREACH_SAFE(task->dependents, dep, tmp)
    {
        if (dep->target)
        {
            struct edc_task_dep *td, *td_tmp;
            LL_FOREACH_SAFE(dep->target->deps, td, td_tmp)
            {
                if (td->source == task)
                {
                    LL_DELETE(dep->target->deps, td);
                    free(td);
                }
            }
            dep->target->deps_total--;
        }
        LL_DELETE(task->dependents, dep);
        free(dep);
    }
}

void edc_task_free(struct edc_task *task)
{
    if (!task)
        return;

    edc_task_free_turns(task);
    edc_task_free_deps(task);

    free(task->task_id);
    free(task->group_id);
    free(task->session_id);
    free(task->description);

    if (task->prompt)
        sdsfree(task->prompt);
    if (task->prompt_name)
        sdsfree(task->prompt_name);
    if (task->output)
        sdsfree(task->output);

    if (task->redirect_stdout)
        sdsfree(task->redirect_stdout);
    if (task->redirect_stderr)
        sdsfree(task->redirect_stderr);

    if (task->todolist)
        cJSON_Delete(task->todolist);

    free(task);
}

/* ── 状态机 ────────────────────────────────────────────────── */

bool edc_task_set_state(struct edc_task *task, edc_task_state new_state)
{
    if (!task)
        return false;

    if (!edc_task_is_valid_transition(task->state, new_state))
    {
        return false;
    }

    task->state = new_state;
    return true;
}

/* ── Turn ──────────────────────────────────────────────────── */

void edc_task_add_turn(struct edc_task *task, struct edc_turn *turn)
{
    if (!task || !turn)
        return;
    edc_queue_push(&task->turns, turn);
}

void edc_task_commit_active_turn(struct edc_task *task)
{
    if (!task || !task->active_turn)
        return;
    edc_queue_push(&task->turns, task->active_turn);
    task->active_turn = NULL;
}

/* ── 依赖 (IPC) ────────────────────────────────────────────── */

/*
 * DFS cycle detection: check if 'source' can reach 'target' by following
 * existing dependency edges. If yes, adding target→source would create a
 * cycle, causing permanent deadlock.
 */
#define EDC_TASK_MAX_DEPTH 64

static bool edc_task_can_reach(struct edc_task *from, struct edc_task *to, int depth)
{
    if (depth > EDC_TASK_MAX_DEPTH)
        return false;
    if (from == to)
        return true;

    struct edc_task_dep *dep;
    LL_FOREACH(from->deps, dep)
    {
        if (edc_task_can_reach(dep->source, to, depth + 1))
            return true;
    }
    return false;
}

bool edc_task_add_dependency(struct edc_task *target, struct edc_task *source, edc_task_dep_type type)
{
    if (!target || !source || target == source)
        return false;

    /* 环检测: source 能到达 target 则拒绝 */
    if (edc_task_can_reach(source, target, 0))
        return false;

    /* 检查是否已有相同依赖 */
    struct edc_task_dep *existing;
    LL_FOREACH(target->deps, existing)
    {
        if (existing->source == source)
            return false;
    }

    /* 在 target->deps 中添加节点 */
    struct edc_task_dep *dep_t = g_new0(struct edc_task_dep, 1);
    dep_t->source = source;
    dep_t->target = target;
    dep_t->type = type;
    LL_APPEND(target->deps, dep_t);

    /* 在 source->dependents 中添加对应节点 */
    struct edc_task_dep *dep_s = g_new0(struct edc_task_dep, 1);
    dep_s->source = source;
    dep_s->target = target;
    dep_s->type = type;
    LL_APPEND(source->dependents, dep_s);

    target->deps_total++;

    /* 若 source 已完成，立即标记 dep 为 completed */
    if (source->state == TASK_COMPLETED)
    {
        edc_task_on_dep_completed(target, source);
    }
    /* 若 target 刚获得依赖，进入 WAITING_DEPS */
    else if (target->state == TASK_IDLE || target->state == TASK_WAITING_INPUT)
    {
        edc_task_set_state(target, TASK_WAITING_DEPS);
    }

    return true;
}

void edc_task_on_dep_completed(struct edc_task *target, struct edc_task *source)
{
    if (!target || !source)
        return;

    struct edc_task_dep *dep;
    LL_FOREACH(target->deps, dep)
    {
        if (dep->source == source && !dep->completed && !dep->cancelled)
        {
            dep->completed = true;
            target->deps_completed++;

            /* 同时标记 source->dependents 中的对应节点 */
            struct edc_task_dep *sd;
            LL_FOREACH(source->dependents, sd)
            {
                if (sd->target == target && !sd->completed)
                {
                    sd->completed = true;
                    break;
                }
            }
            break;
        }
    }

    if (target->deps_completed >= target->deps_total && target->state == TASK_WAITING_DEPS)
    {
        edc_task_set_state(target, TASK_WAITING_INPUT);
    }
}

void edc_task_on_dep_cancelled(struct edc_task *target, struct edc_task *source)
{
    if (!target || !source)
        return;

    struct edc_task_dep *dep;
    LL_FOREACH(target->deps, dep)
    {
        if (dep->source == source && !dep->completed && !dep->cancelled)
        {
            dep->cancelled = true;

            /* 同时标记 source->dependents 中的对应节点 */
            struct edc_task_dep *sd;
            LL_FOREACH(source->dependents, sd)
            {
                if (sd->target == target && !sd->cancelled)
                {
                    sd->cancelled = true;
                    break;
                }
            }
            break;
        }
    }

    /* 重算: 只计算未取消的依赖 */
    size_t active_deps = 0;
    LL_FOREACH(target->deps, dep)
    {
        if (!dep->cancelled)
            active_deps++;
    }
    target->deps_total = active_deps;

    if (target->deps_total == 0 && target->state == TASK_WAITING_DEPS)
    {
        edc_task_set_state(target, TASK_WAITING_INPUT);
    }
}

void edc_task_notify_dependents(struct edc_task *task)
{
    if (!task)
        return;

    struct edc_task_dep *dep, *tmp;
    LL_FOREACH_SAFE(task->dependents, dep, tmp)
    {
        if (!dep->cancelled && !dep->completed)
        {
            dep->completed = true;
            edc_task_on_dep_completed(dep->target, task);
        }
    }
}

/* ── 信号 ──────────────────────────────────────────────────── */

void edc_task_send_signal(struct edc_task *task, edc_task_signal sig)
{
    if (!task)
        return;

    task->cancelled_request = 1;

    if (sig == TASK_SIGTERM)
    {
        struct edc_task_dep *dep, *tmp;
        LL_FOREACH_SAFE(task->dependents, dep, tmp)
        {
            edc_task_on_dep_cancelled(dep->target, task);
        }
    }

    /* 移除自己的 deps (不再等待别人) */
    {
        struct edc_task_dep *dep, *tmp;
        LL_FOREACH_SAFE(task->deps, dep, tmp)
        {
            if (dep->source)
            {
                struct edc_task_dep *sd, *sd_tmp;
                LL_FOREACH_SAFE(dep->source->dependents, sd, sd_tmp)
                {
                    if (sd->target == task)
                    {
                        LL_DELETE(dep->source->dependents, sd);
                        free(sd);
                    }
                }
            }
            LL_DELETE(task->deps, dep);
            free(dep);
        }
        task->deps = NULL;
        task->deps_total = 0;
    }

    edc_task_set_state(task, TASK_CANCELLED);
}

/* ── 输出 ──────────────────────────────────────────────────── */

void edc_task_set_output(struct edc_task *task, sds output)
{
    if (!task)
        return;
    if (task->output)
        sdsfree(task->output);
    task->output = output;
}

sds edc_task_get_output(struct edc_task *task)
{
    if (!task)
        return NULL;
    return task->output;
}

/* ── Prompt ────────────────────────────────────────────────── */

void edc_task_set_prompt(struct edc_task *task, const char *prompt_name, const char *prompt_text)
{
    if (!task)
        return;

    if (task->prompt_name)
    {
        sdsfree(task->prompt_name);
        task->prompt_name = NULL;
    }
    if (task->prompt)
    {
        sdsfree(task->prompt);
        task->prompt = NULL;
    }

    if (prompt_name)
        task->prompt_name = sdsnew(prompt_name);
    if (prompt_text)
        task->prompt = sdsnew(prompt_text);
}

/* ── 上下文构建 ────────────────────────────────────────────── */

cJSON *edc_task_get_context_messages(struct edc_task *task, struct edc_config *config)
{
    if (!task)
        return cJSON_CreateArray();

    cJSON *messages = cJSON_CreateArray();

    /* 1. System prompt */
    if (task->prompt)
    {
        cJSON *sys_msg = cJSON_CreateObject();
        cJSON_AddStringToObject(sys_msg, "role", "system");
        cJSON_AddStringToObject(sys_msg, "content", task->prompt);
        cJSON_AddItemToArray(messages, sys_msg);
    }
    else if (config)
    {
        sds default_prompt = edc_context_build_system_prompt(config);
        cJSON *sys_msg = cJSON_CreateObject();
        cJSON_AddStringToObject(sys_msg, "role", "system");
        cJSON_AddStringToObject(sys_msg, "content", default_prompt);
        cJSON_AddItemToArray(messages, sys_msg);
        sdsfree(default_prompt);
    }

    /* 2. 已完成的 turn 消息 */
    for (size_t qi = task->turns.head; qi < task->turns.tail; qi++)
    {
        struct edc_turn *turn = task->turns.data[qi];
        for (size_t mj = turn->messages.head; mj < turn->messages.tail; mj++)
        {
            cJSON *item = turn->messages.data[mj];
            cJSON_AddItemReferenceToArray(messages, item);
        }
    }

    /* 3. 已完成依赖 task 的 output 注入 */
    struct edc_task_dep *dep;
    LL_FOREACH(task->deps, dep)
    {
        if (dep->completed && dep->source->output)
        {
            sds note = sdscatprintf(sdsempty(), "[子任务 %s 结果]:\n%s", dep->source->description, dep->source->output);
            cJSON *sys = cJSON_CreateObject();
            cJSON_AddStringToObject(sys, "role", "system");
            cJSON_AddStringToObject(sys, "content", note);
            cJSON_AddItemToArray(messages, sys);
            sdsfree(note);
        }
    }

    return messages;
}
