#include "edc_runtime.h"
#include "edc_actor.h"
#include "edc_channel_actor.h"
#include "edc_compat.h"
#include "edc_config.h"
#include "edc_context.h"
#include "edc_file_utils.h"
#include "edc_init_env.h"
#include "edc_llm_actor.h"
#include "edc_group_fold.h"
#include "edc_sandbox.h"
#include "edc_secret.h"
#include "edc_session.h"
#include "edc_session_manager.h"
#include "edc_session_store.h"
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
#include <edc_shell_vars.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ── 命令处理钩子 (由外部注册，避免 agent → shell 依赖) ───── */

/* ── 装配根: 定义**只**在这里 (不透明) ──────────────────────
 * 外部读法: 边界经访问器, userland 收自己那几件 (见 edc_runtime.h)。 */
struct edc_runtime
{
    struct edc_config *config;
    struct edc_session_manager *session_manager;
    struct edc_toolbox *toolbox;
    struct edc_scheduler *scheduler;

    edc_actor_t *channel_actor;
    edc_actor_t *llm_actor;
    edc_actor_t *toolbox_actor;

    edc_actor_runner_t *runner;
};

typedef bool (*edc_runtime_command_fn)(struct edc_config *cfg, struct edc_session_manager *sm,
                                      struct edc_scheduler *sched, struct edc_reply_to to,
                                      struct edc_channel_message *message);
static edc_runtime_command_fn g_command_handler = NULL;

void edc_runtime_set_command_handler(edc_runtime_command_fn fn)
{
    g_command_handler = fn;
}

/* ── 辅助: 获取 agent ──────────────────────────────────────── */

static struct edc_runtime *edc_runtime_from_sched(struct edc_scheduler *sched)
{
    return sched ? sched->ctx : NULL;
}

/* 跨 session task 解析 / 依赖对账 / session 加载 → edc_session_store.{c,h}
 * (store 自己的读口与生命周期, 显式收 (session_manager, scheduler))。 */
/* ── Shell 变量解析回调 ──────────────────────────────────── */

static sds edc_runtime_var_resolver(const char *name, void *user_data)
{
    struct edc_runtime *agent = user_data;
    if (!agent)
        return NULL;

    /* $$ → 当前活跃 task ID */
    if (strcmp(name, "$") == 0)
    {
        struct edc_task *active = edc_session_get_active_task(edc_session_manager_get_active(agent->session_manager));
        return active ? sdsnew(active->task_id) : NULL;
    }

    /* $LAST → 最后一个完成 task 的 output */
    if (strcmp(name, "LAST") == 0)
    {
        struct edc_task *last = NULL;
        uint32_t highest_seq = 0;
        struct edc_task_group *g;
        struct edc_session *active = edc_session_manager_get_active(agent->session_manager);
        DL_FOREACH(active ? active->groups : NULL, g)
        {
            struct edc_task *t;
            LL_FOREACH2(g->members, t, group_next)
            {
                if (edc_task_is_completed(t) && t->completion_seq > highest_seq)
                {
                    highest_seq = t->completion_seq;
                    last = t;
                }
            }
        }
        return (last && last->output) ? sdsdup(last->output) : NULL;
    }

    /* $NUMBER 或 $NAME → 按 task ID / job number 查找 (全局; 必要时按需激活属主 session) */
    struct edc_task *t = edc_session_store_resolve_task(agent->session_manager, agent->scheduler, name, NULL);
    return (t && t->output) ? sdsdup(t->output) : NULL;
}

/* ── 回调: 发送 LLM 请求 ──────────────────────────────────── */

/* 确保 group history 切片起点不落在 tool 消息上。
 * OpenAI 要求每个 role=tool 消息前面必须有对应的 assistant(tool_calls) 消息；
 * 若窗口起点正好切在 assistant(tool_calls)+tool 序列中间，会导致 tool 消息孤儿化，
 * 从而报 "Messages with role 'tool' must be a response to a preceding message with 'tool_calls'"。
 * 这里把起点向前回退到第一个非 tool 消息（即其 assistant），保证序列完整。 */
static size_t edc_runtime_history_safe_start(cJSON *history, size_t start, size_t lower_bound)
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

static void edc_runtime_cb_send_llm(struct edc_scheduler *sched, struct edc_task *task, void *request_id)
{
    struct edc_runtime *agent = edc_runtime_from_sched(sched);
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
    /* 属主 session 的 workdir: 后台 session 里的任务续跑时系统提示里的
     * {{cwd}} 必须是它自己的目录 (P3 ③: 没有属主就没有目录, 不猜"当前的")。 */
    struct edc_session *own = (task->group && task->group->session) ? task->group->session : NULL;
    const char *workdir = own ? edc_session_get_workdir(own) : NULL;
    edc_context_append_system_part(msgs, agent->config, task->group, workdir, &pristine);

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
            edc_task_group_history_add_owned(task->group, sys_hist);
        }
        sdsfree(pristine);
    }

    if (task->group && cJSON_GetArraySize(task->group->history) > 0)
    {
        size_t total = cJSON_GetArraySize(task->group->history);
        size_t end = total;

        /* 未折叠历史起点: 跳过 system prompt (history[0]) + 已折叠消息。
         * 已折叠消息数 = 折叠水位 (completion_seq) 之前的 task 的消息总数。 */
        size_t hist_begin = 1 + edc_task_group_folded_msg_count(task->group);
        if (hist_begin > end)
            hist_begin = end;
        /* 水位边界也可能切在 tool 消息上, 回退到安全边界 */
        hist_begin = edc_runtime_history_safe_start(task->group->history, hist_begin, 1);

        /* 注入全部未折叠历史 [hist_begin, end): fold 采用"半量"策略 (见
         * edc_group_fold 的 L1 fold), 触发时只压最老的一半, 剩下的未折叠
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
                    sds expanded = edc_shell_expand_vars(content->valuestring, edc_runtime_var_resolver, agent, NULL);
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

    /* sink = GROUP_MEMORY 的 system task (fold) 要的是**一次干净的摘要调用**:
     * 它的"镜像"就是 系统组头部 (折叠说明) + 它自己的 turn (折叠 prompt)。
     * 所以不给它工具定义 —— 摘要器没有工具可用, 也不该被工具循环带走 (旧实现
     * 发给 LLM 的请求里 tools 字段是空的, 这条差别的唯一理由就是保持那份请求
     * 逐字节可比)。 */
    const bool is_memory_sink = (task->sink == EDC_SINK_GROUP_MEMORY);
    cJSON *tools = (!is_memory_sink && agent->toolbox) ? agent->toolbox->schemas : NULL;
    struct edc_llm_actor_req *req = edc_llm_actor_req_new(msgs, tools,
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
    /* 待办提醒是给"正在干活的用户任务"的节拍; 摘要调用不该被它插尾
     * (同样是为了让折叠请求与旧实现逐字节一致)。 */
    if (!is_memory_sink && edc_task_llm_request_poke(task))
        req->tail_system = edc_context_make_todo_reminder(task);

    edc_actor_send(agent->llm_actor,
                   edc_actor_msg_new(BC_ACTOR_MSG_LLM_REQUEST, edc_scheduler_get_actor(sched), request_id, req,
                                     (edc_actor_data_free_fn)edc_llm_actor_req_free));
}

/* ── 回调: 发送 Channel 回复 ────────────────────────────────── */

static void edc_runtime_cb_send_reply(struct edc_scheduler *sched, struct edc_task *task, const char *content,
                                    edc_channel_message_type message_type, void *request_id)
{
    struct edc_runtime *agent = edc_runtime_from_sched(sched);
    if (!agent || !task || !content)
        return;

    /* 属主 session: 被切到后台的 session 的产出照发, 由前端按 session_id 静默写进
     * 它自己的显示流。P3 ③: **"显示中的 session 只显示 active task"这条展示规则
     * 移到了前端** —— core 只把事实声明出去 (from_active_task), 不再问"谁在看"
     * (它已经不知道了: 那份"当前 session"的副本已删)。 */
    struct edc_session *own = (task->group && task->group->session) ? task->group->session : NULL;
    if (!own)
    {
        log_warn("dropping %s from task %s: no owning session", "reply", task->task_id ? task->task_id : "?");
        return;
    }

    struct edc_channel_message *reply = edc_channel_message_new(task->channel_type, message_type, sdsnew(content));
    /* Tag the message with its producing task so the frontend can show which
     * task a chat message belongs to (interleaved tasks in one session). */
    if (task->task_id)
        reply->task_id = sdsnew(task->task_id);
    /* And with its owning session, so a background session's output lands in
     * that session's transcript rather than the one on screen. */
    reply->session_id = sdsnew(own->session_id);
    /* 事实声明: 这是它 session 的 active task 吗 (前端据此做展示闸门)。 */
    reply->from_active_task = edc_session_get_active_task(own) == task;
    /* 发送方显式声明渲染偏好，渲染端(CLI/TUI)不再猜测：
     * 只有 LLM 回复 (RESP) 声明按 Markdown 渲染；思考/系统等内部文本保持
     * 默认 PLAIN（零值），原样显示。 */
    if (message_type == EDC_CHANNEL_MESSAGE_RESP_TYPE)
        reply->render_pref = EDC_RENDER_MARKDOWN;

    edc_actor_send(agent->channel_actor,
                   edc_actor_msg_new_sub_type(BC_ACTOR_MSG_CHANNEL_SEND, reply->type, edc_scheduler_get_actor(sched),
                                              request_id, reply, (edc_actor_data_free_fn)edc_channel_message_free));
}

/* ── 回调: 发送 Tool 消息 ───────────────────────────────────── */
/* 构造带 header 的 TOOL 消息发给 channel_actor。展示闸门 (只显示显示中的 session
 * 的 active task) 在前端 —— 这里只声明事实 (from_active_task)。 */

static void edc_runtime_cb_send_tool_message(struct edc_scheduler *sched, struct edc_task *task, const char *header,
                                           const char *content, edc_render_pref_t render_pref, void *request_id)
{
    struct edc_runtime *agent = edc_runtime_from_sched(sched);
    if (!agent || !task || !content)
        return;

    struct edc_channel_message *reply =
        edc_channel_message_new(task->channel_type, EDC_CHANNEL_MESSAGE_TOOL_TYPE, sdsnew(content));
    reply->render_pref = render_pref;
    if (task->task_id)
        reply->task_id = sdsnew(task->task_id);
    /* Owning session (see edc_runtime_cb_send_reply): routes a background
     * session's tool output into its own transcript. */
    {
        struct edc_session *own = (task->group && task->group->session) ? task->group->session : NULL;
        if (own && own->session_id)
            reply->session_id = sdsnew(own->session_id);
        reply->from_active_task = own && edc_session_get_active_task(own) == task;
    }
    if (header && *header)
        reply->header = sdsnew(header);

    edc_actor_send(agent->channel_actor,
                   edc_actor_msg_new_sub_type(BC_ACTOR_MSG_CHANNEL_SEND, reply->type, edc_scheduler_get_actor(sched),
                                              request_id, reply, (edc_actor_data_free_fn)edc_channel_message_free));
}

/* ── 回调: 分发 Tool Calls ────────────────────────────────── */

static void edc_runtime_handle_tool_calls(struct edc_runtime *agent, struct edc_task *task, edc_channel_type call_from_type,
                                        struct edc_llm_actor_resp *resp, void *request_id, edc_actor_t *from_actor);

static void edc_runtime_cb_dispatch_tools(struct edc_scheduler *sched, struct edc_task *task, void *llm_resp,
                                        void *request_id)
{
    struct edc_runtime *agent = edc_runtime_from_sched(sched);
    if (!agent || !task || !llm_resp)
        return;

    struct edc_llm_actor_resp *resp = llm_resp;

    /* 思维内容回显 */
    if ((resp->reasoning_content && sdslen(resp->reasoning_content) > 0) ||
        (resp->content && sdslen(resp->content) > 0))
    {
        sds content = (resp->content && sdslen(resp->content) > 0) ? resp->content : resp->reasoning_content;
        sds text = sdscatprintf(sdsempty(), "%s", content);
        edc_runtime_cb_send_reply(sched, task, text, EDC_CHANNEL_MESSAGE_THOUGHT_TYPE, request_id);
        sdsfree(text);
    }

    edc_runtime_handle_tool_calls(agent, task, task->channel_type, resp, request_id, edc_scheduler_get_actor(sched));
}

/* ── 工具分发逻辑 ──────────────────────────────────────────── */

static void edc_runtime_handle_tool_calls(struct edc_runtime *agent, struct edc_task *task, edc_channel_type call_from_type,
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
static int edc_runtime_cb_redispatch_tools(struct edc_scheduler *sched, struct edc_task *task, void *request_id)
{
    struct edc_runtime *agent = edc_runtime_from_sched(sched);
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

static bool edc_runtime_cb_handle_command(struct edc_scheduler *sched, struct edc_actor *actor,
                                        const struct edc_sched_input *input)
{
    struct edc_runtime *agent = edc_runtime_from_sched(sched);
    /* 命令层 (shell) 说的是 channel 消息: 原始消息由 core 不透明透传到这里。 */
    struct edc_channel_message *msg = input ? input->origin : NULL;
    if (!agent || !msg)
        return false;
    struct edc_reply_to to = {agent->channel_actor, actor, msg->type};
    return g_command_handler ? g_command_handler(agent->config, agent->session_manager, agent->scheduler, to, msg) : false;
}

/* ── session 标签 → session 对象 (store 读口, P3) ─────────────
 *
 * 规则 3: "哪个是当前的"只有一份权威 = session_manager.active_index; scheduler
 * 不知道它。入站消息带的是一个 **session 标签** (不透明作用域), scheduler 需要
 * 对象时经这个口解析 —— 于是 scheduler 不再持有那份副本。
 *
 * 标签为空 = "调用方没指定" (crontab 定时任务、老渠道) → 给活跃 session, 行为与
 * 老实现一致; 标签给定但找不到 → NULL, 调用方拒绝这条输入 (不猜、不落到别的
 * session: 老实现里"切错台"的家族事故就是这么来的)。 */
static struct edc_session *edc_runtime_cb_resolve_session(void *ctx, const char *session_id)
{
    return edc_session_store_resolve_session(((struct edc_runtime *)ctx)->session_manager, session_id);
}

/* ── 回调: Memory Compress ──────────────────────────────────── */

static void edc_runtime_cb_trigger_memory(struct edc_scheduler *sched)
{
    struct edc_runtime *agent = edc_runtime_from_sched(sched);
    if (!agent || !agent->session_manager)
        return;

    int msg_threshold =
        agent->config ? agent->config->group_fold_msg_threshold : EDC_CONFIG_DEFAULT_GROUP_FOLD_MSG_THRESHOLD;
    if (msg_threshold <= 0)
        msg_threshold = EDC_CONFIG_DEFAULT_GROUP_FOLD_MSG_THRESHOLD;

    /* L1 折叠触发: 组内存在可折叠 task 且未折叠消息条数 ≥ 阈值。
     * 触发时机只由对话条数驱动 —— 不再看可折叠 task 个数。
     * 扫描**所有** session: 后台 session 里的任务完成后也要能触发折叠, 否则
     * 只有切回来才折叠。一次只折叠一个组。
     *
     * 折叠本身是一次 system task (见 edc_group_fold.c): 这里只决定"该折了",
     * 任务由 edc_group_fold_start 建、派发, 它的产出经 sink=GROUP_MEMORY 落到
     * 目标组的记忆里, 跑完按 lifetime=TRANSIENT 被回收。 */
    struct edc_session_manager *sm = agent->session_manager;
    for (int si = 0; si < sm->session_count; si++)
    {
        struct edc_session *sess = sm->sessions[si];
        if (!sess)
            continue;
        /* 这个 session 已经有折叠在飞: 跳过 (别让两次折叠互相看见半成品记忆)。 */
        if (edc_scheduler_has_pending_system_task(sess))
            continue;

        struct edc_task_group *g;
        DL_FOREACH(sess->groups, g)
        {
            size_t foldable = edc_task_group_count_foldable(g);
            size_t unfolded = edc_task_group_unfolded_msg_count(g);

            if (edc_group_fold_due(g, msg_threshold))
            {
                log_info("L1 fold trigger: group %s foldable=%zu unfolded=%zu (msg_thr=%d)", g->group_id, foldable,
                         unfolded, msg_threshold);
                edc_group_fold_start(sched, sess, g);
                return; /* 一次只折叠一个 group */
            }
            if (foldable > 0 || unfolded > 0)
            {
                log_debug("L1 fold check: group %s foldable=%zu unfolded=%zu", g->group_id, foldable, unfolded);
            }
        }
    }
}

/* ── 回调: 保存 Session ────────────────────────────────────── */

static void edc_runtime_cb_save_session(struct edc_scheduler *sched, struct edc_session *session)
{
    struct edc_runtime *agent = edc_runtime_from_sched(sched);
    if (!agent || !agent->session_manager)
        return;

    /* Save history for all sessions */
    edc_session_manager_save_all(agent->session_manager);

    /* Save state for the requested session (NULL = the active one). Background
     * task completions pass the owning session so their progress is persisted
     * even while a different session is displayed. */
    struct edc_session *target = session ? session : edc_session_manager_get_active(agent->session_manager);
    if (target)
        edc_session_save_state(target, sched);
}

/* ── 回调: task 树快照 (①b: 事件 vs 应答) ─────────────────────
 *
 * 两条线分得很清:
 *   - `snapshot_reply`   = 前端**点名要**的那棵树 (拉取的应答);
 *   - `snapshot_changed` = "某个 session 的树变了" 的**事件** (很小, 不带树)。
 * 走同一条通道 (channel adapter → TUI 回调), 靠 message_type 区分。 */

/* 出站投递 (publish / 活跃 session 通报) → edc_channel_actor.{c,h}
 * —— 建消息 + 发 BC_ACTOR_MSG_CHANNEL_SEND 是**通道自己的**词汇。 */

/* ── 回调: task 树快照 (①b: 事件 vs 应答) ─────────────────────
 *
 * 两条线分得很清:
 *   - `snapshot_reply`   = 前端**点名要**的那棵树 (拉取的应答);
 *   - `snapshot_changed` = "某个 session 的树变了" 的**事件** (很小, 不带树)。
 * 走同一条通道 (channel adapter → TUI 回调), 靠 message_type 区分。 */
static void edc_runtime_cb_snapshot_reply(struct edc_scheduler *sched, sds serialized)
{
    struct edc_runtime *agent = edc_runtime_from_sched(sched);
    if (!serialized)
        return;
    struct edc_reply_to to = {agent ? agent->channel_actor : NULL, edc_scheduler_get_actor(sched),
                              EDC_CHANNEL_MESSAGE_TASK_SNAPSHOT_TYPE};
    edc_channel_publish(to, NULL, serialized);
}

/* "session X 的树变了" —— 事件, 不带树。前端收到后**只**在自己正显示 X 时去拉。 */
static void edc_runtime_cb_snapshot_changed(struct edc_scheduler *sched, const char *session_id, const char *reason)
{
    struct edc_runtime *agent = edc_runtime_from_sched(sched);
    struct edc_reply_to to = {agent ? agent->channel_actor : NULL, edc_scheduler_get_actor(sched),
                              EDC_CHANNEL_MESSAGE_TASK_TREE_CHANGED_TYPE};
    edc_channel_publish(to, session_id, sdsnew(reason ? reason : ""));
}


/* 会话信号与落盘 (stop_all_turns / remove_all_turns) → edc_session_store.{c,h} */
/* ── 测试/嵌入式装配缝 ──────────────────────────────────────── */

struct edc_runtime *edc_runtime_attach(struct edc_config *cfg, struct edc_session_manager *sm,
                                       struct edc_scheduler *sched, edc_actor_t *channel_actor)
{
    struct edc_runtime *rt = g_new0(struct edc_runtime, 1);
    rt->config = cfg;
    rt->session_manager = sm;
    rt->scheduler = sched;
    rt->channel_actor = channel_actor;
    if (sched)
        sched->ctx = rt; /* 回调上下文指回装配根 (scheduler 只透传, 不认识它) */
    return rt;
}

/* ── 边界访问器 ────────────────────────────────────────────── */

struct edc_config *edc_runtime_config(const struct edc_runtime *rt)
{
    return rt ? rt->config : NULL;
}

struct edc_session_manager *edc_runtime_sessions(const struct edc_runtime *rt)
{
    return rt ? rt->session_manager : NULL;
}

edc_actor_t *edc_runtime_channel_actor(const struct edc_runtime *rt)
{
    return rt ? rt->channel_actor : NULL;
}

/* ── 生命周期 ──────────────────────────────────────────────── */

struct edc_runtime *edc_runtime_new(bool setup, int sandbox_override)
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

    struct edc_runtime *agent = g_new0(struct edc_runtime, 1);
    agent->config = edc_config_load(edc_home);

    /* edc_config_load() returns NULL on a missing/corrupt config.json or when
     * the stored API key cannot be decrypted (e.g. wrong passphrase / master
     * key). Everything below dereferences agent->config, so bail out cleanly
     * instead of crashing. The concrete reason is already logged by the loader. */
    if (!agent->config)
    {
        log_error("Failed to load configuration (%s); aborting startup", edc_home);
        free(agent);
        sdsfree(edc_home);
        return NULL;
    }

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

    agent->channel_actor = edc_channel_actor_new();
    agent->llm_actor = edc_llm_actor_new(agent->config);
    agent->session_manager = edc_session_manager_new(agent->config);
    if (!agent->session_manager)
    {
        log_error("failed to create session manager");
        edc_runtime_free(agent);
        return NULL;
    }

    /* 创建 scheduler —— 不再把 active session 递给它 (P3 ③: scheduler 不持有
     * "当前 session"; 每个入口显式带 session, 或经 resolve_session 读口解析)。 */
    agent->scheduler = edc_scheduler_new(agent->config);
    agent->scheduler->ctx = agent;

    /* 注入回调 */
    agent->scheduler->send_llm = edc_runtime_cb_send_llm;
    agent->scheduler->send_reply = edc_runtime_cb_send_reply;
    agent->scheduler->send_tool_message = edc_runtime_cb_send_tool_message;
    agent->scheduler->dispatch_tools = edc_runtime_cb_dispatch_tools;
    agent->scheduler->redispatch_tools = edc_runtime_cb_redispatch_tools;
    agent->scheduler->handle_command = edc_runtime_cb_handle_command;
    agent->scheduler->trigger_memory = edc_runtime_cb_trigger_memory;
    agent->scheduler->save_session = edc_runtime_cb_save_session;
    agent->scheduler->snapshot_reply = edc_runtime_cb_snapshot_reply;
    agent->scheduler->snapshot_changed = edc_runtime_cb_snapshot_changed;
    agent->scheduler->resolve_session = edc_runtime_cb_resolve_session;

    /* Phase 4: restore saved task/group state for active session */
    {
        struct edc_session *active = edc_session_manager_get_active(agent->session_manager);
        if (active)
            edc_session_store_load_session(agent->session_manager, agent->scheduler, active);
    }

    edc_channel_actor_set_observer(agent->channel_actor, edc_scheduler_get_actor(agent->scheduler));

    agent->runner = edc_actor_runner_new();
    edc_file_utils_init(&agent->runner->loop);
    edc_actor_runner_add(agent->runner, edc_scheduler_get_actor(agent->scheduler));
    edc_actor_runner_add(agent->runner, agent->channel_actor);
    edc_actor_runner_add(agent->runner, agent->llm_actor);

    agent->toolbox = edc_toolbox_new(agent->config, edc_scheduler_get_actor(agent->scheduler), agent->channel_actor);
    agent->toolbox_actor = edc_toolbox_actor_new(agent->toolbox);
    edc_actor_runner_add(agent->runner, agent->toolbox_actor);

    edc_actor_runner_start(agent->runner);

    sdsfree(edc_home);
    return agent;
}

/* CLI 的 !n / !! 历史 → edc_cli_history.{c,h}
 * (前端状态: 进程一份, 不是运行时装配; 见那里的头文件说明) */
bool edc_runtime_set_llm_config(struct edc_runtime *agent, const char *api_url, const char *model, const char *api_key)
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


/* ── sandbox path whitelist (ro/rw) management ─────────────────────────── */

/* sandbox 配置的写口 (只碰 config) → edc_config.{c,h}
 * —— 它们与 config 的读写是同一个概念, 不该挂在 runtime 上。 */
void edc_runtime_free(struct edc_runtime *agent)
{
    if (!agent)
        return;

    edc_actor_runner_free(agent->runner);
    edc_channel_actor_free(agent->channel_actor);
    edc_toolbox_actor_free(agent->toolbox_actor);
    edc_llm_actor_free(agent->llm_actor);

    /* 退出 = 关闭进程, 不是用户要 remove session: 尽量保留原状态。把所有
     * session 完整落盘 (history + state + task/group), 让中断任务以
     * LLM_PROCESSING/TOOL_CALLING + 工具账本**原样**保存, 加载时统一收敛为
     * SUSPENDED, 用户可选择是否 resume —— 因此不写 CANCELLED, 也不在取消后
     * 再落盘。放在 actor runner 停掉之后调用, 避免与 actor 线程并发写。 */
    if (agent->scheduler)
        edc_session_store_save_all_sessions_state(agent->session_manager, agent->scheduler);

    edc_session_store_remove_all_turns(agent->session_manager, agent->scheduler);

    edc_scheduler_free(agent->scheduler);

    if (agent->session_manager)
        edc_session_manager_free(agent->session_manager);
    if (agent->config)
        edc_config_free(agent->config);
    if (agent->toolbox)
        edc_toolbox_free(agent->toolbox);

    edc_secret_cleanup();
    free(agent);
}
