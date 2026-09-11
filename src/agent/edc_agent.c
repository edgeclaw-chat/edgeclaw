#include "edc_agent.h"
#include "edc_actor.h"
#include "edc_channel_actor.h"
#include "edc_compat.h"
#include "edc_config.h"
#include "edc_context.h"
#include "edc_file_utils.h"
#include "edc_init_env.h"
#include "edc_llm_actor.h"
#include "edc_memory_actor.h"
#include "edc_sandbox.h"
#include "edc_secret.h"
#include "edc_session.h"
#include "edc_session_manager.h"
#include "edc_state_serializer.h"
#include "edc_task.h"
#include "edc_task_group.h"
#include "edc_toolbox_actor.h"
#include "edc_turn.h"
#include "log.h"
#include "sds.h"
#include "uthash.h"
#include "utlist_fix.h"
#include <dirent.h>
#include <edc_shell.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CLI_HISTORY_MAX 1000

/* ── 命令处理钩子 (由外部注册，避免 agent → shell 依赖) ───── */

typedef bool (*edc_agent_command_fn)(struct edc_agent *, struct edc_actor *, struct edc_channel_message *);
static edc_agent_command_fn g_command_handler = NULL;

void edc_agent_set_command_handler(edc_agent_command_fn fn)
{
    g_command_handler = fn;
}

/* ── 辅助: 获取 agent ──────────────────────────────────────── */

static struct edc_agent *edc_scheduler_agent(struct edc_scheduler *sched)
{
    return sched ? sched->ctx : NULL;
}

/* ── Shell 变量解析回调 ──────────────────────────────────── */

static sds edc_agent_var_resolver(const char *name, void *user_data)
{
    struct edc_agent *agent = user_data;
    if (!agent) return NULL;

    /* $$ → 当前活跃 task ID */
    if (strcmp(name, "$") == 0)
    {
        struct edc_task *active =
            edc_session_get_active_task(edc_session_manager_get_active(agent->session_manager));
        return active ? sdsnew(active->task_id) : NULL;
    }

    /* $LAST → 最后一个完成 task 的 output */
    if (strcmp(name, "LAST") == 0)
    {
        struct edc_task *last = NULL;
        uint32_t highest_seq = 0;
        struct edc_task_group *g;
        DL_FOREACH(agent->scheduler->session->groups, g)
        {
            struct edc_task *t;
            LL_FOREACH2(g->members, t, group_next)
            {
                if (t->state == TASK_COMPLETED && t->completion_seq > highest_seq)
                {
                    highest_seq = t->completion_seq;
                    last = t;
                }
            }
        }
        return (last && last->output) ? sdsdup(last->output) : NULL;
    }

    /* $NUMBER 或 $NAME → 按 task ID / job number 查找 */
    struct edc_task *t =
        edc_session_find_task(edc_session_manager_get_active(agent->session_manager), name);
    return (t && t->output) ? sdsdup(t->output) : NULL;
}

/* ── 回调: 发送 LLM 请求 ──────────────────────────────────── */

/* 确保 group history 切片起点不落在 tool 消息上。
 * OpenAI 要求每个 role=tool 消息前面必须有对应的 assistant(tool_calls) 消息；
 * 若窗口起点正好切在 assistant(tool_calls)+tool 序列中间，会导致 tool 消息孤儿化，
 * 从而报 "Messages with role 'tool' must be a response to a preceding message with 'tool_calls'"。
 * 这里把起点向前回退到第一个非 tool 消息（即其 assistant），保证序列完整。 */
static size_t edc_agent_history_safe_start(cJSON *history, size_t start, size_t lower_bound)
{
    while (start > lower_bound)
    {
        cJSON *m = cJSON_GetArrayItem(history, start);
        cJSON *role = cJSON_GetObjectItem(m, "role");
        if (role && cJSON_IsString(role) && strcmp(role->valuestring, "tool") == 0)
        {
            start--;
        }
        else
        {
            break;
        }
    }
    return start;
}

static void edc_agent_cb_send_llm(struct edc_scheduler *sched, struct edc_task *task, void *request_id)
{
    struct edc_agent *agent = edc_scheduler_agent(sched);
    if (!agent || !task)
        return;

    /* Group 级上下文: system 部分 (group memory + system prompt, 纯净、不含
     * todo) 由 edc_context_append_system_part 统一组装; 余下为全部未折叠历史
     * + task 专属 (dep outputs / $VAR 展开)。todo 状态提醒不放在
     * 头部——也不每轮请求都塞: 由 edc_task_llm_request_poke 节流, 只有"隔了
     * EDC_TODO_SYNC_REMIND_GAP 条对话 (LLM 请求) 都没调用过 todowrite"
     * 时, 才经 edc_context_make_todo_reminder 重新计算并追加到消息列表绝对
     * 队尾 (req->tail_system, llm actor 在 history + 当前 turn 之后发出)。 */
    cJSON *msgs = cJSON_CreateArray();

    sds pristine = NULL;
    edc_context_append_system_part(msgs, agent->config, task->group, &pristine);

    /* group 无历史时 context 现拼的纯净 prompt: 写回 group history[0] (组内
     * 所有 task 共享、保持纯净) */
    if (pristine)
    {
        if (task->group)
        {
            cJSON *sys_hist = cJSON_CreateObject();
            cJSON_AddStringToObject(sys_hist, "role", "system");
            cJSON_AddStringToObject(sys_hist, "content", pristine);
            /* 组 system prompt 不属于任何 task */
            edc_task_group_history_add_owned(task->group, sys_hist, NULL);
        }
        sdsfree(pristine);
    }

    if (task->group && cJSON_GetArraySize(task->group->history) > 0)
    {
        size_t total = cJSON_GetArraySize(task->group->history);
        size_t end = total;

        /* 未折叠历史起点: 跳过 system prompt (history[0]) + 已折叠消息 */
        size_t hist_begin = 1 + task->group->fold_cursor;
        if (hist_begin > end)
            hist_begin = end;
        /* fold_cursor 边界也可能切在 tool 消息上, 回退到安全边界 */
        hist_begin = edc_agent_history_safe_start(task->group->history, hist_begin, 1);

        /* 注入全部未折叠历史 [hist_begin, end): fold 采用"半量"策略 (见
         * edc_memory_actor 的 L1 fold), 触发时只压最老的一半, 剩下的未折叠
         * 消息正是模型该看到的近期细节, 因此整段注入、不再截断。请求体量的
         * 上界由 group_fold_msg_threshold 决定 (fold 触发即把它压回去)。 */
        for (size_t i = hist_begin; i < end; i++)
        {
            cJSON_AddItemReferenceToArray(msgs, cJSON_GetArrayItem(task->group->history, i));
        }
    }

    /* 注入已完成依赖 task 的输出 (user inputs + final output) */
    struct edc_task_dep *dep;
    LL_FOREACH(task->deps, dep)
    {
        log_info("send_llm task=%s dep src=%s completed=%d output=%s", task->task_id,
                 dep->source ? dep->source->task_id : "(null)", dep->completed,
                 (dep->source && dep->source->output) ? "yes" : "none");
        if (dep->completed && dep->source && !dep->injected && dep->type == TASK_DEP_JOIN)
        {
            dep->injected = true;

            /* inject source task's final output only (mainstream subagent pattern) */
            if (dep->source->output)
            {
                /* find the last user message from source to form a Q&A pair */
                sds last_user = NULL;
                for (size_t qi = dep->source->turns.head; qi < dep->source->turns.tail; qi++)
                {
                    struct edc_turn *turn = dep->source->turns.data[qi];
                    for (size_t mj = turn->messages.head; mj < turn->messages.tail; mj++)
                    {
                        cJSON *msg = turn->messages.data[mj];
                        cJSON *role = cJSON_GetObjectItem(msg, "role");
                        if (role && cJSON_IsString(role) && strcmp(role->valuestring, "user") == 0)
                        {
                            cJSON *content = cJSON_GetObjectItem(msg, "content");
                            if (content && cJSON_IsString(content))
                            {
                                if (last_user)
                                    sdsfree(last_user);
                                last_user = sdsnew(content->valuestring);
                            }
                        }
                    }
                }

                if (last_user)
                {
                    cJSON *usr = cJSON_CreateObject();
                    cJSON_AddStringToObject(usr, "role", "user");
                    cJSON_AddStringToObject(usr, "content", last_user);
                    cJSON_AddItemToArray(msgs, usr);
                    sdsfree(last_user);
                }

                cJSON *asst = cJSON_CreateObject();
                cJSON_AddStringToObject(asst, "role", "assistant");
                cJSON_AddStringToObject(asst, "content", dep->source->output);
                cJSON_AddItemToArray(msgs, asst);
            }

            log_info("  -> injected final output from task %s", dep->source->task_id);
        }
    }

    /* expand deferred $VAR references in the current turn's user messages */
    if (task->has_deferred_vars && task->active_turn)
    {
        for (size_t mj = task->active_turn->messages.head; mj < task->active_turn->messages.tail; mj++)
        {
            cJSON *msg = task->active_turn->messages.data[mj];
            cJSON *role = cJSON_GetObjectItem(msg, "role");
            if (role && cJSON_IsString(role) && strcmp(role->valuestring, "user") == 0)
            {
                cJSON *content = cJSON_GetObjectItem(msg, "content");
                if (content && cJSON_IsString(content))
                {
                    sds expanded = edc_shell_expand_vars(content->valuestring, edc_agent_var_resolver, agent, NULL);
                    if (expanded)
                    {
                        cJSON_ReplaceItemInObject(msg, "content", cJSON_CreateString(expanded));
                        sdsfree(expanded);
                    }
                }
            }
        }
        task->has_deferred_vars = 0;
    }

    struct edc_llm_actor_req *req = edc_llm_actor_req_new(msgs, agent->toolbox ? agent->toolbox->schemas : NULL,
                                                          task->active_turn ? &task->active_turn->messages : NULL);
    req->history_owned = true;
    /* 绝对队尾: todo reminder —— 不再每轮请求都塞 (避免唠叨), 只在
     * "隔了 EDC_TODO_SYNC_REMIND_GAP 条对话 (LLM 请求) 都没调用过
     * todowrite" 时生成 (edc_task_llm_request_poke 达阈值返回 true,
     * 内部已清零重计)。两段式: CURRENT TASK SYSTEM STATE 状态段 +
     * INSTRUCTIONS FOR THIS TURN 指令段; 有清单→清单行+逐轮同步节拍, 无清
     * 单→"尚无清单"+建计划引导。llm actor 在 history + turn 之后追加, 每
     * 轮请求都是最新状态。req 持有并随请求释放 (副本, 不改 task/group)。 */
    req->tail_system = NULL;
    if (edc_task_llm_request_poke(task))
        req->tail_system = edc_context_make_todo_reminder(task);

    edc_actor_send(agent->llm_actor,
                   edc_actor_msg_new(BC_ACTOR_MSG_LLM_REQUEST, edc_scheduler_get_actor(sched), request_id, req,
                                     (edc_actor_data_free_fn)edc_llm_actor_req_free));
}

/* ── 回调: 发送 Channel 回复 ────────────────────────────────── */

static void edc_agent_cb_send_reply(struct edc_scheduler *sched, struct edc_task *task, const char *content,
                                    edc_channel_message_type message_type, void *request_id)
{
    struct edc_agent *agent = edc_scheduler_agent(sched);
    if (!agent || !task || !content)
        return;

    /* only active task outputs to terminal */
    if (!sched->session || edc_session_get_active_task(sched->session) != task)
        return;

    struct edc_channel_message *reply = edc_channel_message_new(task->channel_type, message_type, sdsnew(content));
    /* Tag the message with its producing task so the frontend can show which
     * task a chat message belongs to (interleaved tasks in one session). */
    if (task->task_id)
        reply->task_id = sdsnew(task->task_id);
    /* 发送方显式声明渲染偏好，渲染端(CLI/TUI)不再猜测：
     * 只有 LLM 回复 (RESP) 声明按 Markdown 渲染；思考/系统等内部文本保持
     * 默认 PLAIN（零值），原样显示。 */
    if (message_type == EDC_CHANNEL_MESSAGE_RESP_TYPE)
        reply->render_pref = EDC_RENDER_MARKDOWN;

    edc_actor_send(agent->channel_actor, edc_actor_msg_new_sub_type(BC_ACTOR_MSG_CHANNEL_SEND, reply->type,
                                                                    edc_scheduler_get_actor(sched), request_id, reply,
                                                                    (edc_actor_data_free_fn)edc_channel_message_free));
}

/* ── 回调: 发送 Tool 消息 ───────────────────────────────────── */
/* scheduler 已确认 task == session->active_task 后才走到这里，直接构造带
 * header 的 TOOL 消息发给 channel_actor。 */

static void edc_agent_cb_send_tool_message(struct edc_scheduler *sched, struct edc_task *task, const char *header,
                                           const char *content, edc_render_pref_t render_pref, void *request_id)
{
    struct edc_agent *agent = edc_scheduler_agent(sched);
    if (!agent || !task || !content)
        return;

    struct edc_channel_message *reply =
        edc_channel_message_new(task->channel_type, EDC_CHANNEL_MESSAGE_TOOL_TYPE, sdsnew(content));
    reply->render_pref = render_pref;
    if (task->task_id)
        reply->task_id = sdsnew(task->task_id);
    if (header && *header)
        reply->header = sdsnew(header);

    edc_actor_send(agent->channel_actor, edc_actor_msg_new_sub_type(BC_ACTOR_MSG_CHANNEL_SEND, reply->type,
                                                                    edc_scheduler_get_actor(sched), request_id, reply,
                                                                    (edc_actor_data_free_fn)edc_channel_message_free));
}

/* ── 回调: 分发 Tool Calls ────────────────────────────────── */

static void edc_agent_handle_tool_calls(struct edc_agent *agent, struct edc_task *task, edc_channel_type call_from_type,
                                        struct edc_llm_actor_resp *resp, void *request_id, edc_actor_t *from_actor);

static void edc_agent_cb_dispatch_tools(struct edc_scheduler *sched, struct edc_task *task, void *llm_resp,
                                        void *request_id)
{
    struct edc_agent *agent = edc_scheduler_agent(sched);
    if (!agent || !task || !llm_resp)
        return;

    struct edc_llm_actor_resp *resp = llm_resp;

    /* 思维内容回显 */
    if ((resp->reasoning_content && sdslen(resp->reasoning_content) > 0) ||
        (resp->content && sdslen(resp->content) > 0))
    {
        sds content = (resp->content && sdslen(resp->content) > 0) ? resp->content : resp->reasoning_content;
        sds text = sdscatprintf(sdsempty(), "%s", content);
        edc_agent_cb_send_reply(sched, task, text, EDC_CHANNEL_MESSAGE_THOUGHT_TYPE, request_id);
        sdsfree(text);
    }

    edc_agent_handle_tool_calls(agent, task, task->channel_type, resp, request_id, edc_scheduler_get_actor(sched));
}

/* ── 工具分发逻辑 ──────────────────────────────────────────── */

static void edc_agent_handle_tool_calls(struct edc_agent *agent, struct edc_task *task, edc_channel_type call_from_type,
                                        struct edc_llm_actor_resp *resp, void *request_id, edc_actor_t *from_actor)
{
    struct edc_toolbox_actor_req *req = edc_toolbox_actor_req_new(NULL, NULL, NULL, call_from_type, task);
    struct edc_toolbox_actor_req *head = req;

    if (!resp->tool_calls)
        return;
    for (unsigned i = 0; i < utarray_len(resp->tool_calls); i++)
    {
        struct edc_llm_chat_tool_call *tool_call = (struct edc_llm_chat_tool_call *)utarray_eltptr(resp->tool_calls, i);
        if (tool_call->type == BC_LLM_TOOL_CALL_TYPE_FUNCTION)
        {
            req->next = edc_toolbox_actor_req_new(tool_call->function.name, tool_call->function.arguments,
                                                  tool_call->id, call_from_type, task);
            req = req->next;
            tool_call->function.arguments = NULL;
            tool_call->function.name = NULL;
            tool_call->id = NULL;
        }
    }

    req = head->next;
    edc_toolbox_actor_req_free(head);
    if (req)
    {
        edc_actor_send(agent->toolbox_actor, edc_actor_msg_new(BC_ACTOR_MSG_TOOL_REQUEST, from_actor, request_id, req,
                                                               (edc_actor_data_free_fn)edc_toolbox_actor_req_free));
    }
}

/* resume: 把 task 工具账本里仍为 PENDING (尚未执行) 的记录重建 toolbox 请求链
 * 并投递。EXECUTING 的记录由 scheduler 在 resume 时补中断响应, 不在此列。
 * 返回投递的条数。 */
static int edc_agent_cb_redispatch_tools(struct edc_scheduler *sched, struct edc_task *task, void *request_id)
{
    struct edc_agent *agent = edc_scheduler_agent(sched);
    if (!agent || !task || !agent->toolbox_actor)
        return 0;

    /* dummy head, 与原 dispatch 路径一致 (空 req 不入链) */
    struct edc_toolbox_actor_req *head = edc_toolbox_actor_req_new(NULL, NULL, NULL, task->channel_type, task);
    struct edc_toolbox_actor_req *tail = head;
    int count = 0;

    struct edc_tool_call_record *rec;
    for (rec = task->tool_calls; rec; rec = rec->next)
    {
        if (rec->status != TOOL_CALL_PENDING)
            continue;
        tail->next = edc_toolbox_actor_req_new(rec->name ? sdsdup(rec->name) : sdsempty(),
                                               rec->args ? cJSON_Duplicate(rec->args, 1) : NULL,
                                               rec->call_id ? sdsdup(rec->call_id) : NULL, task->channel_type, task);
        tail = tail->next;
        count++;
    }

    struct edc_toolbox_actor_req *chain = head->next;
    edc_toolbox_actor_req_free(head);
    if (!chain)
        return 0;

    edc_actor_send(agent->toolbox_actor,
                   edc_actor_msg_new(BC_ACTOR_MSG_TOOL_REQUEST, edc_scheduler_get_actor(sched), request_id, chain,
                                     (edc_actor_data_free_fn)edc_toolbox_actor_req_free));
    return count;
}

/* ── 回调: 命令处理 ────────────────────────────────────────── */

static bool edc_agent_cb_handle_command(struct edc_scheduler *sched, struct edc_actor *actor,
                                        struct edc_channel_message *msg)
{
    struct edc_agent *agent = edc_scheduler_agent(sched);
    if (!agent || !msg)
        return false;
    return g_command_handler ? g_command_handler(agent, actor, msg) : false;
}

/* ── 回调: Memory Compress ──────────────────────────────────── */

static void edc_agent_cb_trigger_memory(struct edc_scheduler *sched)
{
    struct edc_agent *agent = edc_scheduler_agent(sched);
    if (!agent || !agent->memory_actor || !sched->session)
        return;

    int msg_threshold = agent->config ? agent->config->group_fold_msg_threshold
                                      : EDC_CONFIG_DEFAULT_GROUP_FOLD_MSG_THRESHOLD;
    if (msg_threshold <= 0)
        msg_threshold = EDC_CONFIG_DEFAULT_GROUP_FOLD_MSG_THRESHOLD;

    /* L1 折叠触发: 组内存在可折叠 task 且未折叠消息条数 ≥ 阈值。
     * 触发时机只由对话条数驱动 —— 不再看可折叠 task 个数。 */
    struct edc_task_group *g;
    DL_FOREACH(sched->session->groups, g)
    {
        size_t foldable = edc_task_group_count_foldable(g);
        size_t unfolded = edc_task_group_unfolded_msg_count(g);
        bool should_fold = foldable > 0 && unfolded >= (size_t)msg_threshold;

        if (should_fold)
        {
            log_info("L1 fold trigger: group %s foldable=%zu unfolded=%zu (msg_thr=%d)",
                     g->group_id, foldable, unfolded, msg_threshold);
            edc_actor_send(agent->memory_actor,
                           edc_actor_msg_new(BC_ACTOR_MSG_GROUP_FOLD, edc_scheduler_get_actor(sched), NULL, g, NULL));
            break; /* 一次只折叠一个 group */
        }
        else if (foldable > 0 || unfolded > 0)
        {
            log_debug("L1 fold check: group %s foldable=%zu unfolded=%zu", g->group_id, foldable, unfolded);
        }
    }
}

/* ── 回调: 保存 Session ────────────────────────────────────── */

static void edc_agent_cb_save_session(struct edc_scheduler *sched)
{
    struct edc_agent *agent = edc_scheduler_agent(sched);
    if (!agent || !agent->session_manager)
        return;

    /* Save history for all sessions */
    edc_session_manager_save_all(agent->session_manager);

    /* Save state for the current (active) session */
    struct edc_session *active = edc_session_manager_get_active(agent->session_manager);
    if (active)
        edc_session_save_state(active, sched);
}

/* ── 回调: task 树快照回传 ─────────────────────────────────── */

static void edc_agent_cb_snapshot_ready(struct edc_scheduler *sched, sds serialized)
{
    struct edc_agent *agent = edc_scheduler_agent(sched);
    if (!agent || !serialized || !agent->channel_actor)
    {
        if (serialized)
            sdsfree(serialized);
        return;
    }

    /* 通过 channel adapter 回传: 子类型 edc_channel_adapter 会让 channel
     * actor 把消息路由到 TUI 注册的 on_actor_message_send_fn。 */
    struct edc_channel_message *msg =
        edc_channel_message_new(edc_channel_adapter, EDC_CHANNEL_MESSAGE_TASK_SNAPSHOT_TYPE, serialized);

    edc_actor_send(agent->channel_actor,
                   edc_actor_msg_new_sub_type(BC_ACTOR_MSG_CHANNEL_SEND, edc_channel_adapter,
                                              edc_scheduler_get_actor(sched), NULL, msg,
                                              (edc_actor_data_free_fn)edc_channel_message_free));
}

void edc_agent_remove_all_turns(struct edc_agent *agent)
{
    if (!agent || !agent->scheduler)
        return;
    edc_scheduler_send_signal_to_all(agent->scheduler, TASK_SIGTERM);
}

/* 用户 /stop: 取消当前 session 全部非终结 turns 后立即保存状态, 使 CANCELLED
 * 跨重启保留。
 * 与 remove_all_turns 不同 —— 后者只剩进程 teardown 一个调用点, 那里故意不落盘
 * (teardown 若落盘会把退出瞬间所有非终结任务都写死)。
 * 注: 目标不是当前 active session 时 (如 /session stop <other>), 走
 * edc_scheduler_send_signal_to_session(sched, target, sig, true), 因为此时
 * sched->session 不是目标。 */
void edc_agent_stop_all_turns(struct edc_agent *agent)
{
    if (!agent || !agent->scheduler)
        return;
    edc_scheduler_send_signal_to_all(agent->scheduler, TASK_SIGTERM);
    if (agent->scheduler->save_session)
        agent->scheduler->save_session(agent->scheduler);
}

/* "离开 session" (/session switch): 只取消当前 session 里**正在执行**的任务
 * (LLM_PROCESSING / TOOL_CALLING), 其余 (WAITING_INPUT / WAITING_DEPS /
 * SUSPENDED) 原样保留 —— task 是什么状态就是什么状态, 离开不等于把它们都停掉。
 * 取消后立即落盘。目标始终是当前 active session。 */
void edc_agent_stop_running_turns(struct edc_agent *agent)
{
    if (!agent || !agent->scheduler)
        return;
    edc_scheduler_send_signal_to_session(agent->scheduler, agent->scheduler->session, TASK_SIGTERM, true);
    if (agent->scheduler->save_session)
        agent->scheduler->save_session(agent->scheduler);
}

/* ── 生命周期 ──────────────────────────────────────────────── */

struct edc_agent *edc_agent_new(bool setup, int sandbox_override)
{
    sds edc_home = edc_config_resolve_bc_home();

    if (!edc_init_env(edc_home, setup))
    {
        log_error("Failed to initialize environment");
        sdsfree(edc_home);
        return NULL;
    }

    if (!edc_secret_init(false, edc_home))
    {
        log_error("Failed to initialize secret module");
        sdsfree(edc_home);
        return NULL;
    }

    struct edc_agent *agent = g_new0(struct edc_agent, 1);
    agent->history_capacity = 8;
    agent->history = malloc((size_t)agent->history_capacity * sizeof(sds));
    agent->config = edc_config_load(edc_home);

    /* The master key is intentionally NOT wiped here: the /connect command
     * re-encrypts the API key when the user updates the provider at runtime,
     * so the key must stay available (it remains mlock()ed). */

    /* The master switch was loaded from config.json (default on). The CLI flag,
     * when given, overrides it for this run only: sandbox_override < 0 keeps the
     * persisted value, 0 forces off, 1 forces on. It is deliberately not
     * written back -- only the in-TUI /sandbox command persists the state. */
    if (sandbox_override >= 0)
        agent->config->enable_sandbox = (sandbox_override != 0);
    if (agent->config->enable_sandbox)
    {
        edc_sandbox_init(agent->config);
    }

    agent->channel_actor = edc_channel_actor_new(&agent->config->channel, agent->config->workspace);
    agent->llm_actor = edc_llm_actor_new(agent->config);
    agent->memory_actor =
        edc_memory_actor_new(agent->config->workspace, agent->config->memory_compress_threshold, agent->llm_actor);
    agent->session_manager = edc_session_manager_new(agent->config);
    if (!agent->session_manager)
    {
        log_error("failed to create session manager");
        edc_agent_free(agent);
        return NULL;
    }

    struct edc_session *session = edc_session_manager_get_active(agent->session_manager);

    /* 创建 scheduler */
    agent->scheduler = edc_scheduler_new(session, agent->config);
    agent->scheduler->ctx = agent;

    /* 注入回调 */
    agent->scheduler->send_llm = edc_agent_cb_send_llm;
    agent->scheduler->send_reply = edc_agent_cb_send_reply;
    agent->scheduler->send_tool_message = edc_agent_cb_send_tool_message;
    agent->scheduler->dispatch_tools = edc_agent_cb_dispatch_tools;
    agent->scheduler->redispatch_tools = edc_agent_cb_redispatch_tools;
    agent->scheduler->handle_command = edc_agent_cb_handle_command;
    agent->scheduler->trigger_memory = edc_agent_cb_trigger_memory;
    agent->scheduler->save_session = edc_agent_cb_save_session;
    agent->scheduler->snapshot_ready = edc_agent_cb_snapshot_ready;

    /* Phase 4: restore saved task/group state for active session */
    {
        struct edc_session *active = edc_session_manager_get_active(agent->session_manager);
        if (active)
            edc_session_load_state(active, agent->scheduler);
    }

    edc_channel_actor_set_observer(agent->channel_actor, edc_scheduler_get_actor(agent->scheduler));

    agent->runner = edc_actor_runner_new();
    edc_file_utils_init(&agent->runner->loop);
    edc_actor_runner_add(agent->runner, edc_scheduler_get_actor(agent->scheduler));
    edc_actor_runner_add(agent->runner, agent->channel_actor);
    edc_actor_runner_add(agent->runner, agent->llm_actor);
    edc_actor_runner_add(agent->runner, agent->memory_actor);

    agent->toolbox = edc_toolbox_new(agent->config, edc_scheduler_get_actor(agent->scheduler), agent->channel_actor);
    agent->toolbox_actor = edc_toolbox_actor_new(agent->toolbox);
    edc_actor_runner_add(agent->runner, agent->toolbox_actor);

    edc_actor_runner_start(agent->runner);

    sdsfree(edc_home);
    return agent;
}

void edc_agent_add_history(struct edc_agent *agent, const char *line)
{
    if (!agent || !line || !*line)
        return;
    if (agent->history_count > 0 && strcmp(agent->history[agent->history_count - 1], line) == 0)
        return;

    if (agent->history_count >= agent->history_capacity)
    {
        agent->history_capacity *= 2;
        agent->history = realloc(agent->history, (size_t)agent->history_capacity * sizeof(sds));
    }
    agent->history[agent->history_count++] = sdsnew(line);
}

bool edc_agent_set_llm_config(struct edc_agent *agent, const char *api_url, const char *model, const char *api_key)
{
    if (!agent || !agent->config)
        return false;

    /* Persist first: if the file cannot be written, leave the running agent
     * untouched so memory and disk stay in agreement. */
    if (!edc_config_update_llm(agent->config->edc_home, api_url, model, api_key))
        return false;

    struct llm_api_conf *c = &agent->config->llm_api_conf;
    if (api_url && api_url[0])
    {
        sdsfree(c->api_url);
        c->api_url = sdsnew(api_url);
    }
    if (model && model[0])
    {
        sdsfree(c->model);
        c->model = sdsnew(model);
    }
    if (api_key && api_key[0])
    {
        sdsfree(c->api_key);
        c->api_key = sdsnew(api_key);
    }

    /* The llm_actor keeps its own copy of the provider settings and runs on the
     * actor-runner thread, so hand it the new values through its mailbox. */
    struct edc_llm_api_conf_update *u = calloc(1, sizeof(*u));
    if (!u)
        return true; /* config is already updated; actor refresh is best-effort */
    u->api_url = api_url ? sdsnew(api_url) : NULL;
    u->model = model ? sdsnew(model) : NULL;
    u->api_key = api_key ? sdsnew(api_key) : NULL;
    edc_actor_send(agent->llm_actor, edc_actor_msg_new(BC_ACTOR_MSG_LLM_SET_CONFIG, NULL, NULL, u,
                                                       (edc_actor_data_free_fn)edc_llm_api_conf_update_free));
    return true;
}

bool edc_agent_set_sandbox_enabled(struct edc_agent *agent, bool enable)
{
    if (!agent || !agent->config)
        return false;

    /* Persist first (mirrors edc_agent_set_llm_config): if config.json cannot
     * be written, leave the running agent untouched so memory and disk agree. */
    if (!edc_config_update_sandbox_enabled(agent->config->edc_home, enable))
        return false;

    agent->config->enable_sandbox = enable;
    return true;
}

/* ── sandbox path whitelist (ro/rw) management ─────────────────────────── */

static bool sds_path_list_contains(sds *arr, const char *p)
{
    if (!arr)
        return false;
    for (int i = 0; arr[i]; i++)
        if (strcmp(arr[i], p) == 0)
            return true;
    return false;
}

static void sds_path_list_add(sds **arrp, const char *p)
{
    if (sds_path_list_contains(*arrp, p))
        return;
    int n = 0;
    if (*arrp)
        while ((*arrp)[n])
            n++;
    *arrp = realloc(*arrp, (size_t)(n + 2) * sizeof(sds));
    (*arrp)[n] = sdsnew(p);
    (*arrp)[n + 1] = NULL;
}

static void sds_path_list_remove(sds **arrp, const char *p)
{
    if (!arrp || !*arrp)
        return;
    int n = 0;
    for (int i = 0; (*arrp)[i]; i++)
    {
        if (strcmp((*arrp)[i], p) == 0)
        {
            sdsfree((*arrp)[i]);
            continue;
        }
        (*arrp)[n++] = (*arrp)[i];
    }
    (*arrp)[n] = NULL;
}

bool edc_agent_set_sandbox_path(struct edc_agent *agent, const char *path, edc_sandbox_path_kind kind, bool add,
                                sds *msg)
{
    if (msg)
        *msg = NULL;
    if (!agent || !agent->config)
    {
        if (msg)
            *msg = sdsnew("agent not ready");
        return false;
    }
    if (!path || path[0] != '/')
    {
        if (msg)
            *msg = sdsnew("path must be absolute");
        return false;
    }

    char *resolved = realpath(path, NULL);
    if (!resolved)
    {
        if (msg)
            *msg = sdscatprintf(sdsempty(), "no such directory: %s", path);
        return false;
    }
    struct stat st;
    if (stat(resolved, &st) != 0 || !S_ISDIR(st.st_mode))
    {
        if (msg)
            *msg = sdscatprintf(sdsempty(), "not a directory: %s", resolved);
        free(resolved);
        return false;
    }

    /* Persist first (mirrors edc_agent_set_sandbox_enabled): on failure leave
     * the running config untouched so memory and disk agree. */
    if (!edc_config_update_sandbox_path(agent->config->edc_home, resolved, kind, add))
    {
        if (msg)
            *msg = sdsnew("failed to write config.json");
        free(resolved);
        return false;
    }

    /* A path lives in exactly one list; mirror the persist step in memory. */
    sds **ro = &agent->config->sandbox_ro_paths;
    sds **rw = &agent->config->sandbox_rw_paths;
    sds **dn = &agent->config->sandbox_deny_paths;
    sds **target = (kind == EDC_SANDBOX_PATH_RW) ? rw : (kind == EDC_SANDBOX_PATH_DENY) ? dn : ro;

    if (add)
    {
        sds_path_list_add(target, resolved);
        if (target != ro)
            sds_path_list_remove(ro, resolved);
        if (target != rw)
            sds_path_list_remove(rw, resolved);
        if (target != dn)
            sds_path_list_remove(dn, resolved);
    }
    else
    {
        sds_path_list_remove(target, resolved);
    }

    free(resolved);
    return true;
}

void edc_agent_free(struct edc_agent *agent)
{
    if (!agent)
        return;

    edc_actor_runner_free(agent->runner);
    edc_channel_actor_free(agent->channel_actor);
    edc_toolbox_actor_free(agent->toolbox_actor);
    edc_llm_actor_free(agent->llm_actor);
    edc_memory_actor_free(agent->memory_actor);

    /* 退出前完整落盘 (history + index + task/group state): 让中断任务以
     * LLM_PROCESSING/TOOL_CALLING + 工具账本原样保存, 而不是被下面的
     * remove_all_turns 先取消成 CANCELLED。加载时统一收敛为 SUSPENDED。
     * 放在 actor runner 停掉之后调用, 避免与 actor 线程并发写。 */
    if (agent->scheduler && agent->scheduler->save_session)
        agent->scheduler->save_session(agent->scheduler);

    edc_agent_remove_all_turns(agent);

    edc_scheduler_free(agent->scheduler);

    if (agent->session_manager)
        edc_session_manager_free(agent->session_manager);
    if (agent->config)
        edc_config_free(agent->config);
    if (agent->toolbox)
        edc_toolbox_free(agent->toolbox);

    for (int i = 0; i < agent->history_count; i++)
        sdsfree(agent->history[i]);
    free(agent->history);

    edc_secret_cleanup();
    free(agent);
}
