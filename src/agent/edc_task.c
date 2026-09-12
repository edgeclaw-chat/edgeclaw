#include "edc_task.h"
#include "edc_compat.h"
#include "edc_context.h"
#include "edc_session.h"
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
        /* resume 重新判定: 回依赖等待 / 等待输入 / 重新处理 / 继续工具链, 或取消 */
        return to == TASK_WAITING_DEPS || to == TASK_WAITING_INPUT || to == TASK_LLM_PROCESSING ||
               to == TASK_TOOL_CALLING || to == TASK_CANCELLED;
    case TASK_COMPLETED:
        return false; /* terminal */
    case TASK_CANCELLED:
        return to == TASK_WAITING_INPUT; /* resume */
    case TASK_ERROR:
        /* resume 续跑失败的那一轮: 重新判定后可能回依赖等待 / 等输入 / 重新
         * 请求 LLM / 继续工具链 (与 SUSPENDED 同一组目标); 或取消。 */
        return to == TASK_WAITING_DEPS || to == TASK_WAITING_INPUT || to == TASK_LLM_PROCESSING ||
               to == TASK_TOOL_CALLING || to == TASK_CANCELLED;
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

/* ── fork 快照 ────────────────────────────────────────────── */

struct edc_turn *edc_task_snapshot_turns(struct edc_task *task, struct edc_session *session,
                                         const char *snapshot_task_id)
{
    if (!task)
        return NULL;

    struct edc_turn *snap = edc_turn_new();
    if (snapshot_task_id)
        snap->task_id = strdup(snapshot_task_id);

    for (size_t qi = task->turns.head; qi < task->turns.tail; qi++)
    {
        struct edc_turn *turn = task->turns.data[qi];
        if (!turn)
            continue;

        if (edc_queue_length(&turn->messages) > 0)
        {
            for (size_t mj = turn->messages.head; mj < turn->messages.tail; mj++)
                edc_queue_push(&snap->messages, cJSON_Duplicate((cJSON *)turn->messages.data[mj], 1));
        }
        else if (turn->range_valid && turn->msg_range_count > 0 && session)
        {
            size_t begin = turn->msg_range_start;
            size_t end = begin + turn->msg_range_count;
            size_t total = cJSON_GetArraySize(session->messages);
            for (size_t i = begin; i < end && i < total; i++)
            {
                cJSON *src = cJSON_GetArrayItem(session->messages, (int)i);
                if (!src)
                    break;
                edc_queue_push(&snap->messages, cJSON_Duplicate(src, 1));
            }
        }
    }

    if (edc_queue_length(&snap->messages) == 0)
    {
        edc_turn_free(snap);
        return NULL;
    }
    return snap;
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
    edc_task_tool_calls_clear(task);

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
    /* turn 提交 = 本轮所有工具都已完成 (记录已在响应时删除); 账本清空 */
    edc_task_tool_calls_clear(task);
    edc_queue_push(&task->turns, task->active_turn);
    task->active_turn = NULL;
}

size_t edc_task_message_count(const struct edc_task *task)
{
    if (!task)
        return 0;
    size_t n = 0;
    for (size_t i = task->turns.head; i < task->turns.tail; i++)
    {
        struct edc_turn *turn = (struct edc_turn *)task->turns.data[i];
        if (turn)
            n += edc_queue_length(&turn->messages);
    }
    if (task->active_turn)
        n += edc_queue_length(&task->active_turn->messages);
    return n;
}

/* 把一条 role=="user" 消息的 content 追加到 *out (以 '\n' 连接; *out 为
 * NULL 时新建)。 */
static void append_user_msg(const cJSON *msg, sds *out)
{
    if (!msg || !out)
        return;
    const char *role = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "role"));
    if (!role || strcmp(role, "user") != 0)
        return;
    const char *content = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "content"));
    if (!content)
        return;
    *out = *out ? sdscatprintf(*out, "\n%s", content) : sdsnew(content);
}

/* 收集一个 turn 里的 user 输入（'\n' 连接）。turn 未提交时消息还在
 * turn->messages; 一旦提交进 session（edc_session_add_turn 会把消息 pop 进
 * session->messages，只在 turn 上留下 msg_range 记录），本地队列就空了 ——
 * 此时回退到 session 消息数组按 range 读取，否则已完成任务的输入会丢失，
 * `/task status` 只剩 desc 看不到 input。 */
static void collect_user_msgs(const struct edc_turn *turn, const cJSON *session_messages, sds *out)
{
    if (!turn || !out)
        return;

    if (turn->messages.tail > turn->messages.head)
    {
        for (size_t j = turn->messages.head; j < turn->messages.tail; j++)
            append_user_msg((const cJSON *)turn->messages.data[j], out);
        return;
    }

    /* 已 flush 的 turn: 消息在 session 数组里, 按记录的 range 取回 */
    if (!turn->range_valid || !session_messages || !cJSON_IsArray(session_messages))
        return;
    for (size_t k = 0; k < turn->msg_range_count; k++)
    {
        cJSON *msg = cJSON_GetArrayItem(session_messages, (int)(turn->msg_range_start + k));
        append_user_msg(msg, out);
    }
}

sds edc_task_user_input(const struct edc_task *task, const cJSON *session_messages)
{
    if (!task)
        return NULL;
    sds out = NULL;
    for (size_t i = task->turns.head; i < task->turns.tail; i++)
        collect_user_msgs((const struct edc_turn *)task->turns.data[i], session_messages, &out);
    collect_user_msgs(task->active_turn, session_messages, &out);
    return out;
}

/* ── Todo 同步节流 ────────────────────────────────────────── */

void edc_task_note_todo_sync(struct edc_task *task)
{
    if (!task)
        return;
    task->todo_sync_gap = 0;
}

bool edc_task_llm_request_poke(struct edc_task *task)
{
    if (!task)
        return false;
    task->todo_sync_gap++;
    if (task->todo_sync_gap >= EDC_TODO_SYNC_REMIND_GAP)
    {
        /* 达阈值: 本次请求 append reminder 提醒一次, 清零重计 —— 避免后续
         * 每次请求都重复提醒 (直到模型调用 todowrite 或再次超距) */
        task->todo_sync_gap = 0;
        return true;
    }
    return false;
}

/* ── Token 统计 ────────────────────────────────────────────── */

void edc_task_add_usage(struct edc_task *task, long long prompt_tokens, long long completion_tokens,
                        long long total_tokens, long long cached_tokens)
{
    if (!task)
        return;

    /* 全 0 = provider 没给 usage (或错误响应), 不写任何字段, 保持计数干净 */
    if (prompt_tokens <= 0 && completion_tokens <= 0 && total_tokens <= 0 && cached_tokens <= 0)
        return;

    if (prompt_tokens < 0)
        prompt_tokens = 0;
    if (completion_tokens < 0)
        completion_tokens = 0;
    if (cached_tokens < 0)
        cached_tokens = 0;
    if (total_tokens <= 0)
        total_tokens = prompt_tokens + completion_tokens;

    task->tokens_prompt += prompt_tokens;
    task->tokens_completion += completion_tokens;
    task->tokens_total += total_tokens;
    task->tokens_cached += cached_tokens;
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

    /* 找到这条尚在等待的依赖，按类型决定语义 (完成/已取消/不存在 → 无事可做) */
    struct edc_task_dep *dep = NULL;
    LL_FOREACH(target->deps, dep)
    {
        if (dep->source == source && !dep->completed && !dep->cancelled)
            break;
    }
    if (!dep)
        return;

    if (dep->type == TASK_DEP_JOIN)
    {
        /* JOIN: 源的 output 永远不会来了 —— 这条依赖无法满足，本 task 也就
         * 无法作为完整输入运行，于是级联取消。edc_task_send_signal 会递归
         * 取消本 task 自己的依赖者，并清空它的 deps；因此 dep 随后即失效，
         * 必须立即 return，不能再解引用。 */
        edc_task_send_signal(target, TASK_SIGTERM);
        return;
    }

    /* SEQUENCE(`;`): 只是顺序约束，不需要源 output —— 抹去这条依赖即可，
     * 本 task 仍可运行。 */
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

    /* 重算: 只计算未取消的依赖 */
    size_t active_deps = 0;
    struct edc_task_dep *d;
    LL_FOREACH(target->deps, d)
    {
        if (!d->cancelled)
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

bool edc_task_is_running(const struct edc_task *task)
{
    if (!task)
        return false;
    return task->state == TASK_LLM_PROCESSING || task->state == TASK_TOOL_CALLING;
}

void edc_task_send_signal(struct edc_task *task, edc_task_signal sig)
{
    if (!task)
        return;

    task->cancelled_request = 1;

    if (sig == TASK_SIGTERM)
    {
        /* 先把依赖者快照成数组再逐个处理: JOIN 的级联取消会在回调里递归清理
         * 各 task 的 deps, 因而把节点从 task->dependents 上摘掉 —— 边遍历边
         * 改这条链表, LL_FOREACH_SAFE 也救不了 (它只缓存当前节点的 next,
         * 而 next 本身可能被更深的递归 freeing)。取消过程不会释放 task 本身
         * (只释放 dep 节点), 所以暂存裸指针是安全的。 */
        size_t n = 0;
        struct edc_task_dep *dep;
        LL_FOREACH(task->dependents, dep)
        n++;
        if (n > 0)
        {
            struct edc_task **targets = malloc(n * sizeof(*targets));
            if (targets)
            {
                size_t i = 0;
                LL_FOREACH(task->dependents, dep)
                targets[i++] = dep->target;
                for (size_t k = 0; k < i; k++)
                    edc_task_on_dep_cancelled(targets[k], task);
                free(targets);
            }
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

    /* 任务被取消: 在途工具追账已无意义, 丢弃账本 */
    edc_task_tool_calls_clear(task);
    edc_task_set_state(task, TASK_CANCELLED);
}

/* ── 未完成工具调用表 (resume 账本) ────────────────────────── */

static void edc_task_tool_call_record_free(struct edc_tool_call_record *rec)
{
    if (!rec)
        return;
    if (rec->call_id)
        sdsfree(rec->call_id);
    if (rec->name)
        sdsfree(rec->name);
    if (rec->args)
        cJSON_Delete(rec->args);
    free(rec);
}

struct edc_tool_call_record *edc_task_tool_call_add(struct edc_task *task, const char *call_id, const char *name,
                                                    cJSON *args)
{
    if (!task)
        return NULL;
    struct edc_tool_call_record *rec = g_new0(struct edc_tool_call_record, 1);
    rec->call_id = call_id ? sdsnew(call_id) : NULL;
    rec->name = name ? sdsnew(name) : NULL;
    rec->args = args ? cJSON_Duplicate(args, 1) : NULL;
    rec->status = TOOL_CALL_PENDING;
    LL_APPEND(task->tool_calls, rec);
    return rec;
}

struct edc_tool_call_record *edc_task_tool_call_find(struct edc_task *task, const char *call_id)
{
    if (!task || !call_id)
        return NULL;
    struct edc_tool_call_record *rec;
    LL_FOREACH(task->tool_calls, rec)
    {
        if (rec->call_id && strcmp(rec->call_id, call_id) == 0)
            return rec;
    }
    return NULL;
}

struct edc_tool_call_record *edc_task_tool_call_mark_executing(struct edc_task *task, const char *call_id)
{
    struct edc_tool_call_record *rec = edc_task_tool_call_find(task, call_id);
    if (rec)
        rec->status = TOOL_CALL_EXECUTING;
    return rec;
}

bool edc_task_tool_call_remove(struct edc_task *task, const char *call_id)
{
    if (!task || !call_id)
        return false;
    struct edc_tool_call_record *rec, *tmp;
    LL_FOREACH_SAFE(task->tool_calls, rec, tmp)
    {
        if (rec->call_id && strcmp(rec->call_id, call_id) == 0)
        {
            LL_DELETE(task->tool_calls, rec);
            edc_task_tool_call_record_free(rec);
            return true;
        }
    }
    return false;
}

void edc_task_tool_calls_clear(struct edc_task *task)
{
    if (!task)
        return;
    struct edc_tool_call_record *rec, *tmp;
    LL_FOREACH_SAFE(task->tool_calls, rec, tmp)
    {
        LL_DELETE(task->tool_calls, rec);
        edc_task_tool_call_record_free(rec);
    }
    task->tool_calls = NULL;
}

bool edc_task_has_pending_tool_calls(const struct edc_task *task)
{
    return task && task->tool_calls != NULL;
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

/* ── 折叠 ────────────────────────────────────────────────── */

bool edc_task_is_foldable(struct edc_task *task)
{
    return task && task->state == TASK_COMPLETED && !task->turns_folded;
}

size_t edc_task_turn_msg_count(struct edc_task *task)
{
    if (!task)
        return 0;

    size_t n = 0;
    for (size_t qi = task->turns.head; qi < task->turns.tail; qi++)
    {
        struct edc_turn *turn = task->turns.data[qi];
        n += edc_queue_length(&turn->messages);
    }
    return n;
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
        sds default_prompt = edc_context_build_system_prompt(config, NULL);
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
