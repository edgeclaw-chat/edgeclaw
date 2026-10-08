#include "edc_task.h"
#include "edc_compat.h"
#include "edc_context.h"
#include "edc_session.h"
#include "edc_task_group.h"
#include "utlist_fix.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* The old state-name mapping is retired (R3b): the display name has one source,
 * edc_task_state_str, derived from (step, life). */

/* ── 状态转换校验 ──────────────────────────────────────────── */

/* ── step / life: 名称、解析、映射 ─────────────────────────── */

const char *edc_task_step_name(edc_task_step step)
{
    switch (step)
    {
    case EDC_TASKSTEP_READY:
        return "READY";
    case EDC_TASKSTEP_AWAIT_DEPS:
        return "AWAIT_DEPS";
    case EDC_TASKSTEP_AWAIT_EXEC:
        return "AWAIT_EXEC";
    case EDC_TASKSTEP_AWAIT_TOOLS:
        return "AWAIT_TOOLS";
    case EDC_TASKSTEP_AWAIT_RESOURCE:
        return "AWAIT_RESOURCE";
    case EDC_TASKSTEP_DONE:
        return "DONE";
    }
    return "READY";
}

const char *edc_task_life_name(edc_task_life life)
{
    switch (life)
    {
    case EDC_LIFE_ALIVE:
        return "ALIVE";
    case EDC_LIFE_SUSPENDED:
        return "SUSPENDED";
    case EDC_LIFE_COMPLETED:
        return "COMPLETED";
    case EDC_LIFE_CANCELLED:
        return "CANCELLED";
    case EDC_LIFE_ERROR:
        return "ERROR";
    }
    return "ALIVE";
}

edc_task_step edc_task_classify_step(const char *name)
{
    if (!name)
        return EDC_TASKSTEP_READY;
    if (strcmp(name, "AWAIT_DEPS") == 0)
        return EDC_TASKSTEP_AWAIT_DEPS;
    /* Accept the legacy "AWAIT_INPUT": older v5 files and the v4->v5 migration
     * wrote it for a parked task, and it means READY. */
    if (strcmp(name, "AWAIT_INPUT") == 0)
        return EDC_TASKSTEP_READY;
    if (strcmp(name, "AWAIT_EXEC") == 0)
        return EDC_TASKSTEP_AWAIT_EXEC;
    if (strcmp(name, "AWAIT_TOOLS") == 0)
        return EDC_TASKSTEP_AWAIT_TOOLS;
    if (strcmp(name, "AWAIT_RESOURCE") == 0)
        return EDC_TASKSTEP_AWAIT_RESOURCE;
    if (strcmp(name, "DONE") == 0)
        return EDC_TASKSTEP_DONE;
    return EDC_TASKSTEP_READY;
}

edc_task_life edc_task_classify_life(const char *name)
{
    if (!name)
        return EDC_LIFE_ALIVE;
    if (strcmp(name, "SUSPENDED") == 0)
        return EDC_LIFE_SUSPENDED;
    if (strcmp(name, "COMPLETED") == 0)
        return EDC_LIFE_COMPLETED;
    if (strcmp(name, "CANCELLED") == 0)
        return EDC_LIFE_CANCELLED;
    if (strcmp(name, "ERROR") == 0)
        return EDC_LIFE_ERROR;
    return EDC_LIFE_ALIVE;
}

/* sink name + classify, matching step/life. Not persisted today, so _name serves
 * logs and _classify has no caller yet; kept as one family. */

const char *edc_task_sink_name(edc_sink sink)
{
    switch (sink)
    {
    case EDC_SINK_CHANNEL:
        return "CHANNEL";
    case EDC_SINK_GROUP_MEMORY:
        return "GROUP_MEMORY";
    }
    return "CHANNEL";
}

edc_sink edc_task_classify_sink(const char *name)
{
    if (!name)
        return EDC_SINK_CHANNEL;
    if (strcmp(name, "GROUP_MEMORY") == 0)
        return EDC_SINK_GROUP_MEMORY;
    return EDC_SINK_CHANNEL;
}

/* The display name (snapshot / logs / command text), derived straight from
 * (step, life) with life speaking first, matching the old order. WAITING_INPUT is
 * never returned. */
const char *edc_task_state_str(const struct edc_task *task)
{
    if (!task)
        return "IDLE";
    switch (task->life)
    {
    case EDC_LIFE_SUSPENDED:
        return "SUSPENDED";
    case EDC_LIFE_COMPLETED:
        return "COMPLETED";
    case EDC_LIFE_CANCELLED:
        return "CANCELLED";
    case EDC_LIFE_ERROR:
        return "ERROR";
    case EDC_LIFE_ALIVE:
        break;
    }
    switch (task->step)
    {
    case EDC_TASKSTEP_READY:
        return "IDLE";
    case EDC_TASKSTEP_AWAIT_DEPS:
        return "WAITING_DEPS";
    case EDC_TASKSTEP_AWAIT_EXEC:
        return "LLM_PROCESSING";
    case EDC_TASKSTEP_AWAIT_TOOLS:
        return "TOOL_CALLING";
    case EDC_TASKSTEP_AWAIT_RESOURCE:
        return "WAITING_DEPS"; /* 旧枚举没有对应值 */
    case EDC_TASKSTEP_DONE:
        break;
    }
    /* 兜底: DONE 但仍 ALIVE 的畸形组合。没有任何路径产出它。 */
    return "IDLE";
}

static bool edc_task_is_valid_transition(edc_task_step from_step, edc_task_life from_life, edc_task_step to_step,
                                         edc_task_life to_life);

/* A transition states both "which step" and "alive?" at once; the state machine
 * validates (step, life), so it never loses the phase. */
/* Low-level primitives: state the target directly. Used by the scheduler's
 * unified entry and the executor's port. */
bool edc_task_move(struct edc_task *task, edc_task_step step, edc_task_life life)
{
    if (!task)
        return false;
    if (!edc_task_is_valid_transition(task->step, task->life, step, life))
        return false;
    task->step = step;
    task->life = life;
    return true;
}

/* Intent-named write points: the caller says what it wants rather than assembling
 * a target state. Returns false when the transition is rejected. */
bool edc_task_await_exec(struct edc_task *task)
{
    return edc_task_move(task, EDC_TASKSTEP_AWAIT_EXEC, EDC_LIFE_ALIVE);
}

bool edc_task_await_tools(struct edc_task *task)
{
    return edc_task_move(task, EDC_TASKSTEP_AWAIT_TOOLS, EDC_LIFE_ALIVE);
}

bool edc_task_mark_ready(struct edc_task *task)
{
    return edc_task_move(task, EDC_TASKSTEP_READY, EDC_LIFE_ALIVE);
}

bool edc_task_mark_waiting_deps(struct edc_task *task)
{
    return edc_task_move(task, EDC_TASKSTEP_AWAIT_DEPS, EDC_LIFE_ALIVE);
}

bool edc_task_mark_completed(struct edc_task *task)
{
    return edc_task_move(task, EDC_TASKSTEP_DONE, EDC_LIFE_COMPLETED);
}

/* 失败要**广播**给依赖者 (定义在依赖那一段里, 见下方)。 */
static void task_notify_dependents_failed(struct edc_task *task);

/* Failure = set ERROR and broadcast to every dependent, so "a task failed"
 * propagates no matter who initiated it. Symmetric with send_signal: neither does
 * host bookkeeping, they only record the moved tasks. */
bool edc_task_mark_failed(struct edc_task *task)
{
    if (!task)
        return false;
    /* 自转移失败 = 它本来就没失败 (终态/非法相位), 那就什么都不该广播 —— 幂等。 */
    if (!edc_task_move(task, EDC_TASKSTEP_DONE, EDC_LIFE_ERROR))
        return false;
    task_notify_dependents_failed(task);
    return true;
}

bool edc_task_mark_cancelled(struct edc_task *task)
{
    return edc_task_move(task, EDC_TASKSTEP_DONE, EDC_LIFE_CANCELLED);
}

/* 挂起**只说 life**: 停在哪一步就停在哪一步 —— 这正是 step/life 拆分要保住的东西
 * (旧词 TASK_SUSPENDED 也是这个语义, 只是说不出口)。 */
bool edc_task_mark_suspended(struct edc_task *task)
{
    return task && edc_task_move(task, task->step, EDC_LIFE_SUSPENDED);
}

/* Transition table, keyed by (step, life). */
static bool edc_task_is_valid_transition(edc_task_step from_step, edc_task_life from_life, edc_task_step to_step,
                                         edc_task_life to_life)
{
    /* No transition within the same (step, life). */
    if (from_step == to_step && from_life == to_life)
        return false;

    switch (from_life)
    {
    case EDC_LIFE_COMPLETED:
        return false; /* terminal */
    case EDC_LIFE_CANCELLED:
        /* Resume: cancel only stops the task, so CANCELLED shares the target group
         * with SUSPENDED / ERROR. */
        return (to_life == EDC_LIFE_ALIVE &&
                (to_step == EDC_TASKSTEP_READY || to_step == EDC_TASKSTEP_AWAIT_DEPS ||
                 to_step == EDC_TASKSTEP_AWAIT_EXEC || to_step == EDC_TASKSTEP_AWAIT_TOOLS)) ||
               (to_step == EDC_TASKSTEP_DONE && to_life == EDC_LIFE_CANCELLED);
    case EDC_LIFE_SUSPENDED:
        /* resume 重新判定: 回依赖等待 / 就绪 / 重新请求 LLM / 继续工具链, 或取消 */
    case EDC_LIFE_ERROR:
        /* 失败的那一轮续跑 (与 SUSPENDED 同一组目标), 或取消。 */
        return (to_life == EDC_LIFE_ALIVE &&
                (to_step == EDC_TASKSTEP_READY || to_step == EDC_TASKSTEP_AWAIT_DEPS ||
                 to_step == EDC_TASKSTEP_AWAIT_EXEC || to_step == EDC_TASKSTEP_AWAIT_TOOLS)) ||
               (to_step == EDC_TASKSTEP_DONE && to_life == EDC_LIFE_CANCELLED);
    case EDC_LIFE_ALIVE:
        break;
    }

    switch (from_step)
    {
    /* READY = parked with input, waiting to be dispatched: dispatch / hit a
     * dependency / cancel / suspend / failure. Failure is allowed so a drive that
     * cannot start any I/O fails instead of being rescanned forever. */
    case EDC_TASKSTEP_READY:
        return (to_life == EDC_LIFE_ALIVE &&
                (to_step == EDC_TASKSTEP_AWAIT_EXEC || to_step == EDC_TASKSTEP_AWAIT_DEPS)) ||
               (to_step == EDC_TASKSTEP_DONE && (to_life == EDC_LIFE_CANCELLED || to_life == EDC_LIFE_ERROR)) ||
               (to_step == EDC_TASKSTEP_READY && to_life == EDC_LIFE_SUSPENDED);
    case EDC_TASKSTEP_AWAIT_DEPS:
    case EDC_TASKSTEP_AWAIT_RESOURCE: /* 旧词表里没有这一步 (镜像折成 WAITING_DEPS): 沿用同一行 */
        /* 依赖齐了 → 回就绪; 或取消 / 失败 */
        return (to_step == EDC_TASKSTEP_READY && to_life == EDC_LIFE_ALIVE) ||
               (to_step == EDC_TASKSTEP_DONE && (to_life == EDC_LIFE_CANCELLED || to_life == EDC_LIFE_ERROR));
    case EDC_TASKSTEP_AWAIT_EXEC:
        /* AWAIT_DEPS: a fold in flight holds the group; the executor parks a task
         * that still owes a dependency instead of sending the next request. */
        return (to_life == EDC_LIFE_ALIVE &&
                (to_step == EDC_TASKSTEP_AWAIT_TOOLS || to_step == EDC_TASKSTEP_AWAIT_DEPS)) ||
               (to_step == EDC_TASKSTEP_DONE &&
                (to_life == EDC_LIFE_COMPLETED || to_life == EDC_LIFE_CANCELLED || to_life == EDC_LIFE_ERROR));
    case EDC_TASKSTEP_AWAIT_TOOLS:
        return (to_life == EDC_LIFE_ALIVE &&
                (to_step == EDC_TASKSTEP_AWAIT_EXEC || to_step == EDC_TASKSTEP_AWAIT_DEPS)) ||
               (to_step == EDC_TASKSTEP_DONE &&
                (to_life == EDC_LIFE_COMPLETED || to_life == EDC_LIFE_CANCELLED || to_life == EDC_LIFE_ERROR)) ||
               (to_step == EDC_TASKSTEP_AWAIT_TOOLS && to_life == EDC_LIFE_SUSPENDED); /* 跑到一半挂起 */
    case EDC_TASKSTEP_DONE:
        /* 畸形组合: DONE 却还 ALIVE (显示名折成 IDLE, 见 edc_task_state_str 的兜底)。
         * 没有任何路径产出它; 沿用 READY 一行, 免得落到 default 之外的行为。 */
        return (to_life == EDC_LIFE_ALIVE &&
                (to_step == EDC_TASKSTEP_AWAIT_EXEC || to_step == EDC_TASKSTEP_AWAIT_DEPS)) ||
               (to_step == EDC_TASKSTEP_DONE && to_life == EDC_LIFE_CANCELLED) ||
               (to_step == EDC_TASKSTEP_DONE && to_life == EDC_LIFE_SUSPENDED);
    }
    return false;
}

/* Write entries that bypass the state machine (initial state, fixtures): no
 * validation, straight to (step, life). */
void edc_task_force_move(struct edc_task *task, edc_task_step step, edc_task_life life)
{
    if (!task)
        return;
    task->step = step;
    task->life = life;
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
    /* 新任务: "带着 input 停着, 等着被派发" —— 没有"等用户输入"这个相位,
     * 所以直接置 (READY, ALIVE) (旧词写作 TASK_IDLE/TASK_WAITING_INPUT 两行)。 */
    edc_task_force_move(task, EDC_TASKSTEP_READY, EDC_LIFE_ALIVE);
    /* The output axis defaults to a user task (output goes to the user); written
     * explicitly so correctness does not depend on enum order. */
    task->sink = EDC_SINK_CHANNEL;
    task->is_group_leader = 0;
    task->channel_type = channel_type;

    edc_queue_init(&task->turns);

    return task;
}

struct edc_task *edc_task_fork(struct edc_task *parent, const char *task_id, const char *desc,
                               uint32_t fork_point_completion_seq, edc_channel_type channel_type)
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
    child->fork_point_completion_seq = fork_point_completion_seq;
    /* 子任务与 edc_task_new 一样落在 READY (= IDLE 镜像); 不再强制 WAITING_INPUT。 */
    return child;
}

/* ── fork 快照 ────────────────────────────────────────────── */

struct edc_turn *edc_task_snapshot_turns(struct edc_task *task, struct edc_session *session,
                                         const char *snapshot_task_id)
{
    (void)session; /* turn 现在自己持有消息 */
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

        for (size_t mj = turn->messages.head; mj < turn->messages.tail; mj++)
            edc_queue_push(&snap->messages, cJSON_Duplicate((cJSON *)turn->messages.data[mj], 1));
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
    /* 没被宿主取走的连带改动名单 (纯内存): cb = NULL 只清空。任务都要释放了,
     * 名单也就没有意义 —— 记的债本来只对"还活着的宿主"有意义。 */
    edc_task_drain_moves(task, NULL, NULL);

    free(task->task_id);
    free(task->group_id);
    free(task->session_id);
    free(task->description);

    if (task->raw_input)
        sdsfree(task->raw_input);

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

    if (task->todos)
        cJSON_Delete(task->todos);

    free(task);
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

/* 收集一个 turn 里的 user 输入（'\n' 连接）。turn 自己持有消息。 */
static void collect_user_msgs(const struct edc_turn *turn, sds *out)
{
    if (!turn || !out)
        return;
    for (size_t j = turn->messages.head; j < turn->messages.tail; j++)
        append_user_msg((const cJSON *)turn->messages.data[j], out);
}

sds edc_task_user_input(const struct edc_task *task, const cJSON *session_messages)
{
    (void)session_messages; /* turn 持有消息, 不再需要 session 消息数组 */
    if (!task)
        return NULL;
    sds out = NULL;
    for (size_t i = task->turns.head; i < task->turns.tail; i++)
        collect_user_msgs((const struct edc_turn *)task->turns.data[i], &out);
    collect_user_msgs(task->active_turn, &out);
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

    /* How big the conversation is *now*, remembered rather than summed: what this
     * round sent plus what it got back -- where the next request starts from, so
     * it is the group's context width in the sidebar. The all-zero guard above
     * keeps a provider that reports no usage from erasing it. */
    if (total_tokens > 0)
        task->last_total_tokens = total_tokens;
}

/* ── 依赖 (IPC) ────────────────────────────────────────────── */

/* DFS cycle detection: if 'source' can reach 'target', adding target->source
 * would create a deadlock. */
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
    if (edc_task_is_completed(source))
    {
        edc_task_on_dep_completed(target, source);
    }
    /* 若 target 刚获得依赖，进入 WAITING_DEPS。
     * READY 的镜像就是 IDLE; 退休 WAITING_INPUT 后两者同落点, 不再并列。 */
    else if (edc_task_is_ready(target))
    {
        edc_task_mark_waiting_deps(target);
    }

    return true;
}

bool edc_task_has_unresolved_deps(const struct edc_task *task)
{
    if (!task)
        return false;
    struct edc_task_dep *dep;
    LL_FOREACH(task->deps, dep)
    {
        if (!dep->completed && !dep->cancelled)
            return true;
    }
    return false;
}

/* 台账: 连带改动的记账函数 (定义在依赖那一段里, 见下方)。 */
static void task_record_move(struct edc_task *source, struct edc_task *moved);

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

    if (target->deps_completed >= target->deps_total && edc_task_awaits_deps(target))
    {
        /* Completion also moves the dependent's phase, so it owes the same
         * bookkeeping and is recorded here (idempotent when nothing moved). */
        edc_task_step step_before = target->step;
        edc_task_life life_before = target->life;
        edc_task_mark_ready(target);
        if (target->step != step_before || target->life != life_before)
            task_record_move(source, target);
    }
}

/* Side-effect move report (see edc_task.h). */

/* 记一笔债务: |moved| 的相位被 |source| 发起的这次操作连带动过了。
 * 债记在**发起者**头上 —— 宿主正要结算的就是它, 清账时顺手带走。 */
static void task_record_move(struct edc_task *source, struct edc_task *moved)
{
    if (!source || !moved)
        return;
    struct edc_task_move_report *r = malloc(sizeof(*r));
    if (!r)
        return; /* 记账失败只丢一次通知, 不改变任何相位 */
    r->task = moved;
    r->next = source->moved_report;
    source->moved_report = r;
}

/* Unlink target's edge to source on both lists. Used when a JOIN source failed:
 * the edge can never be satisfied, and keeping it would send target back to
 * AWAIT_DEPS on resume. Chosen over marking it cancelled because a cancelled flag
 * is replayed on load and would turn a resumable ERROR into CANCELLED. */
static void task_unlink_dep(struct edc_task *target, struct edc_task *source)
{
    if (!target || !source)
        return;

    struct edc_task_dep *dep, *tmp;
    LL_FOREACH_SAFE(target->deps, dep, tmp)
    {
        if (dep->source == source)
        {
            LL_DELETE(target->deps, dep);
            free(dep);
            if (target->deps_total > 0)
                target->deps_total--;
            break;
        }
    }

    struct edc_task_dep *sd, *sd_tmp;
    LL_FOREACH_SAFE(source->dependents, sd, sd_tmp)
    {
        if (sd->target == target)
        {
            LL_DELETE(source->dependents, sd);
            free(sd);
            break;
        }
    }
}

/* Void target's SEQUENCE edge to a source that has terminated and will never
 * produce: order is done, so drop the edge and let target run. Shared by cancel
 * and failure. */
static void task_void_sequence_dep(struct edc_task_dep *dep, struct edc_task *target, struct edc_task *source)
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

    /* 重算: 只计算未取消的依赖 */
    size_t active_deps = 0;
    struct edc_task_dep *d;
    LL_FOREACH(target->deps, d)
    {
        if (!d->cancelled)
            active_deps++;
    }
    target->deps_total = active_deps;

    if (target->deps_total == 0 && edc_task_awaits_deps(target))
    {
        /* 抹掉最后一条失效依赖 → 这个 task 从 AWAIT_DEPS 转 READY, 同样是相位改动
         * (侧栏的依赖等待就解除了), 同样记债。 */
        edc_task_step step_before = target->step;
        edc_task_life life_before = target->life;
        edc_task_mark_ready(target);
        if (target->step != step_before || target->life != life_before)
            task_record_move(source, target);
    }
}

void edc_task_drain_moves(struct edc_task *task, void (*cb)(void *ctx, struct edc_task *moved), void *ctx)
{
    if (!task)
        return;
    /* 取到空为止: 回调里可能**又填进来** (清账本身还会连带改动别的 task)。
     * cb = NULL 时只清空名单 (释放 task 时用)。 */
    while (task->moved_report)
    {
        struct edc_task_move_report *r = task->moved_report;
        task->moved_report = r->next;
        if (cb)
            cb(ctx, r->task);
        free(r);
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
        /* JOIN: the source's output never arrives, so this task cannot run with a
         * complete input and is cascaded into cancellation. Return immediately: the
         * recursive cancel has already cleared deps. */
        edc_task_step step_before = target->step;
        edc_task_life life_before = target->life;
        edc_task_send_signal(target, TASK_SIGTERM);
        /* 级联取消也是**相位改动** —— 这个 task 从此进终态, 宿主却只点了源 task 的
         * 名。记一笔债 (重复取消是幂等的: 相位没真动就不记)。 */
        if (target->step != step_before || target->life != life_before)
            task_record_move(source, target);
        return;
    }

    /* SEQUENCE(`;`): 只是顺序约束，不需要源 output —— 抹去这条依赖即可，
     * 本 task 仍可运行。 */
    task_void_sequence_dep(dep, target, source);
}

/* Handle a failed source, symmetric with cancellation (neither will produce
 * again), only the terminal state differs. Both ERROR and CANCELLED are resumable. */
void edc_task_on_dep_failed(struct edc_task *target, struct edc_task *source)
{
    if (!target || !source)
        return;

    /* 找到这条尚在等待的依赖，按类型决定语义 (完成/已作废/不存在 → 无事可做) */
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
        /* JOIN: cascade the failure; unlink the edge first or resume cannot
         * recover. */
        edc_task_step step_before = target->step;
        edc_task_life life_before = target->life;

        /* Write the reason into output ("whose output will never arrive"), so the
         * host has a notice instead of a false generic "LLM request failed". */
        if (source->task_id)
            edc_task_set_output(target, sdscatprintf(sdsempty(), "[failed: its dependency %s failed, so the output "
                                                                 "it was waiting for will never arrive]",
                                                     source->task_id));
        else
            edc_task_set_output(target, sdsnew("[failed: a dependency failed, so the output it was waiting for will "
                                               "never arrive]"));

        task_unlink_dep(target, source);
        edc_task_mark_failed(target);
        /* 级联失败也是**相位改动** —— 这个 task 从此进终态, 宿主却只点了源 task 的
         * 名。记一笔债 (重复失败是幂等的: 相位没真动就不记)。 */
        if (target->step != step_before || target->life != life_before)
            task_record_move(source, target);
        return;
    }

    /* SEQUENCE(`;`): 与取消那条路逐字同构 —— 源死在前头, 顺序已走完, 抹边放行。 */
    task_void_sequence_dep(dep, target, source);
}

/* Failure broadcast: snapshot the dependents before processing, since a JOIN
 * cascade recursively unlinks nodes from that list. The cascade never frees the
 * tasks themselves, so raw pointers are safe. */
static void task_notify_dependents_failed(struct edc_task *task)
{
    if (!task)
        return;

    size_t n = 0;
    struct edc_task_dep *dep;
    LL_FOREACH(task->dependents, dep)
    n++;
    if (n == 0)
        return;

    struct edc_task **targets = malloc(n * sizeof(*targets));
    if (!targets)
        return; /* 快照失败只丢一次广播, 不改变任何相位 */
    size_t i = 0;
    LL_FOREACH(task->dependents, dep)
    targets[i++] = dep->target;
    for (size_t k = 0; k < i; k++)
        edc_task_on_dep_failed(targets[k], task);
    free(targets);
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
    /* Running = ALIVE and awaiting the executor/tool reply; suspended-while-awaiting
     * tools does not count. */
    return edc_task_awaits_exec(task) || edc_task_awaits_tools(task);
}

void edc_task_send_signal(struct edc_task *task, edc_task_signal sig)
{
    if (!task)
        return;

    task->cancelled_request = 1;

    if (sig == TASK_SIGTERM)
    {
        /* Snapshot the dependents first: a JOIN cascade recursively unlinks nodes
         * from task->dependents, which LL_FOREACH_SAFE cannot survive. */
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
    edc_task_mark_cancelled(task);
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

/* Repair dangling tool_calls (pre-drive; see edc_task.h). Idempotent. */

static const char *const kInterruptedToolNote =
    "[interrupted: this tool was still running when the process restarted; it did not produce a result]";

static bool task_turn_has_tool_response(struct edc_turn *turn, size_t upto, const char *call_id)
{
    for (size_t i = 0; i < upto; i++)
    {
        cJSON *m = turn->messages.data[i];
        cJSON *role = cJSON_GetObjectItem(m, "role");
        cJSON *tcid = cJSON_GetObjectItem(m, "tool_call_id");
        if (role && cJSON_IsString(role) && strcmp(role->valuestring, "tool") == 0 && tcid && cJSON_IsString(tcid) &&
            strcmp(tcid->valuestring, call_id) == 0)
            return true;
    }
    return false;
}

void edc_task_repair_dangling_tool_calls(struct edc_task *task)
{
    struct edc_turn *turn = (task && task->active_turn) ? task->active_turn : NULL;
    if (!turn)
        return;

    /* 只扫**修补之前**的既有消息: 补进去的 tool 消息不参与遍历。队列会因追加而
     * realloc, 所以 base 指针每轮重取 (不能缓存数组首地址)。 */
    size_t n = turn->messages.tail;
    for (size_t i = 0; i < n; i++)
    {
        cJSON *m = turn->messages.data[i];
        cJSON *role = cJSON_GetObjectItem(m, "role");
        cJSON *tcs = cJSON_GetObjectItem(m, "tool_calls");
        if (!role || !cJSON_IsString(role) || strcmp(role->valuestring, "assistant") != 0 || !tcs ||
            !cJSON_IsArray(tcs))
            continue;

        cJSON *tc = NULL;
        cJSON_ArrayForEach(tc, tcs)
        {
            cJSON *id = cJSON_GetObjectItem(tc, "id");
            if (!id || !cJSON_IsString(id) || !id->valuestring || !*id->valuestring)
                continue;

            const struct edc_tool_call_record *rec = edc_task_tool_call_find(task, id->valuestring);
            if (rec && rec->status != TOOL_CALL_EXECUTING)
                continue; /* PENDING: 交给驱动段真派发, 不要造假响应 */
            if (task_turn_has_tool_response(turn, n, id->valuestring))
                continue; /* 已经有结果了 (幂等) */

            cJSON *fn = cJSON_GetObjectItem(tc, "function");
            cJSON *nm = fn ? cJSON_GetObjectItem(fn, "name") : NULL;
            edc_turn_add_tool_message(turn, id->valuestring, (nm && cJSON_IsString(nm)) ? nm->valuestring : "",
                                      kInterruptedToolNote);
            /* 账本里若还留着这条 (SUSPENDED / ERROR 路径: EXECUTING 已派发、结果未回),
             * 一并删掉 —— 它已经有响应了, 不该再被当成未完成。 */
            edc_task_tool_call_remove(task, id->valuestring);
        }
    }
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

void edc_task_set_completion_seq(struct edc_task *task, uint32_t seq)
{
    if (!task)
        return;
    task->completion_seq = seq;
}

/* ── 折叠 ────────────────────────────────────────────────── */

/* Foldable = finished, not yet folded, and not itself a fold (sink != GROUP_MEMORY).
 * The `sink` condition keeps the fold task (and the whole system group) out of the
 * fold account: a task whose output is group memory is not conversation to compress.
 * (It used to be a `lifetime` condition; the fold task is now an ordinary persisted
 * task, so the exclusion rides `sink`, which is what actually says "my output is
 * memory".) */
bool edc_task_is_foldable(struct edc_task *task)
{
    return task && task->sink != EDC_SINK_GROUP_MEMORY && edc_task_is_completed(task) && !task->turns_folded;
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

/* ── 关系 / 出站 (见 edc_task.h) ───────────────────────────── */

struct edc_session *edc_task_session(const struct edc_task *task)
{
    if (!task || !task->group)
        return NULL;
    return task->group->session;
}

void edc_task_flush_redirects(struct edc_task *task)
{
    if (!task)
        return;

    if (task->redirect_stdout)
    {
        FILE *f = fopen(task->redirect_stdout, task->redirect_append ? "a" : "w");
        if (f)
        {
            if (task->output)
                fputs(task->output, f);
            fclose(f);
        }
    }
    if (task->redirect_stderr)
    {
        FILE *f = fopen(task->redirect_stderr, "w");
        if (f)
        {
            if (task->output)
                fputs(task->output, f);
            fclose(f);
        }
    }
}
