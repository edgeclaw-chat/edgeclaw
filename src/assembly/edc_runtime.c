#include "edc_runtime.h"
#include "edc_actor.h"
#include "edc_channel_actor.h"
#include "edc_compat.h"
#include "edc_config.h"
#include "edc_context.h"
#include "edc_file_utils.h"
#include "edc_init_env.h"
#include "edc_llm_actor.h"
#include "edc_llm_request.h"
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

    /* shutdown() 幂等闸门 (见 edc_runtime_shutdown): "落盘"只能做一次 ——
     * 重复跑一遍等于把刚恢复的状态又写一次。 */
    bool shutdown_done;
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
/* ── 回调: 发送 LLM 请求 (实现见 //src/actor:llm_request) ─────────
 * 装配根只交两样 kernel 拿不到的值: 工具定义 (所有权归 toolbox) 与 LLM 端点
 * 邮箱; 其余 (config / session 仓库 / group 状态) 都在 sched 或 task 上。 */
static void edc_runtime_cb_send_llm(struct edc_scheduler *sched, struct edc_task *task, void *request_id)
{
    struct edc_runtime *rt = edc_runtime_from_sched(sched);
    edc_llm_request_send(sched, task, (rt && rt->toolbox) ? rt->toolbox->schemas : NULL,
                         rt ? rt->llm_actor : NULL, request_id);
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
    struct edc_reply_to to = {.channel_actor = agent->channel_actor, .actor = actor, .type = msg->type};
    return g_command_handler ? g_command_handler(agent->config, agent->session_manager, agent->scheduler, to, msg) : false;
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
    struct edc_reply_to to = {.channel_actor = agent ? agent->channel_actor : NULL,
                              .actor = edc_scheduler_get_actor(sched),
                              .type = EDC_CHANNEL_MESSAGE_TASK_SNAPSHOT_TYPE};
    edc_channel_publish(to, NULL, serialized);
}

/* "session X 的树变了" —— 事件, 不带树。前端收到后**只**在自己正显示 X 时去拉。 */
static void edc_runtime_cb_snapshot_changed(struct edc_scheduler *sched, const char *session_id, const char *reason)
{
    struct edc_runtime *agent = edc_runtime_from_sched(sched);
    struct edc_reply_to to = {.channel_actor = agent ? agent->channel_actor : NULL,
                              .actor = edc_scheduler_get_actor(sched),
                              .type = EDC_CHANNEL_MESSAGE_TASK_TREE_CHANGED_TYPE};
    edc_channel_publish(to, session_id, sdsnew(reason ? reason : ""));
}


/* 会话信号与落盘 (stop_all_turns / remove_all_turns) → edc_session_store.{c,h} */

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
     * "当前 session"; 每个入口显式带 session, 标签解析交由 store 的同包读口)。 */
    agent->scheduler = edc_scheduler_new(agent->config);
    agent->scheduler->ctx = agent;
    /* session 仓库句柄: 标签解析、落盘、折叠扫描都同包直调 store (与 config 同形) */
    agent->scheduler->sessions = agent->session_manager;

    /* 注入回调 */
    agent->scheduler->send_llm = edc_runtime_cb_send_llm;
    agent->scheduler->send_reply = edc_runtime_cb_send_reply;
    agent->scheduler->send_tool_message = edc_runtime_cb_send_tool_message;
    agent->scheduler->dispatch_tools = edc_runtime_cb_dispatch_tools;
    agent->scheduler->redispatch_tools = edc_runtime_cb_redispatch_tools;
    agent->scheduler->handle_command = edc_runtime_cb_handle_command;
    agent->scheduler->snapshot_reply = edc_runtime_cb_snapshot_reply;
    agent->scheduler->snapshot_changed = edc_runtime_cb_snapshot_changed;

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

/* CLI 的 !n / !! 历史 → src/shell/edc_cli_history.{c,h}
 * (shell 自己的命令历史, 进程一份, 不是运行时装配; 见那里的头文件说明) */
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
void edc_runtime_shutdown(struct edc_runtime *rt)
{
    if (!rt || rt->shutdown_done)
        return;

    /* 1. 先停 actor (含 runner) —— 落盘必须在之后, 否则会与 actor 线程并发写。 */
    edc_actor_runner_free(rt->runner);
    rt->runner = NULL;

    /* 这三个 free **不接受 NULL** (会解引用 `actor->state`), 所以逐个判空:
     * `edc_runtime_new` 的失败路径上 toolbox_actor 还没建出来, 以前会在这里空指针崩。*/
    if (rt->channel_actor)
    {
        edc_channel_actor_free(rt->channel_actor);
        rt->channel_actor = NULL;
    }
    if (rt->toolbox_actor)
    {
        edc_toolbox_actor_free(rt->toolbox_actor);
        rt->toolbox_actor = NULL;
    }
    if (rt->llm_actor)
    {
        edc_llm_actor_free(rt->llm_actor);
        rt->llm_actor = NULL;
    }

    /* 2. 退出 = 关闭进程, 不是用户要 remove session: 尽量保留原状态。把所有
     * session 完整落盘 (history + state + task/group), 让中断任务以
     * LLM_PROCESSING/TOOL_CALLING + 工具账本**原样**保存, 加载时统一收敛为
     * SUSPENDED, 用户可选择是否 resume —— 因此不写 CANCELLED, 也不在取消后
     * 再落盘。 */
    if (rt->scheduler)
        edc_session_store_save_all_sessions_state(rt->session_manager, rt->scheduler);

    /* 3. 在途 turn 不再属于任何活着的 turn: 清掉 (sched 为空时这两个函数自己返回)。 */
    edc_session_store_remove_all_turns(rt->session_manager, rt->scheduler);

    rt->shutdown_done = true; /* 幂等: 上面的落盘只能做一次 */
}

void edc_runtime_free(struct edc_runtime *rt)
{
    if (!rt)
        return;

    /* 先补齐"关进程"该做的 (停 actor + 落盘 + 清在途 turn) —— 保持老的
     * "销毁即落盘"行为; 想分两步走的调用方显式先 `edc_runtime_shutdown()`。
     * 位置在**释放零件之前**: 落盘要用到 scheduler / session_manager。 */
    edc_runtime_shutdown(rt);

    edc_scheduler_free(rt->scheduler);

    if (rt->session_manager)
        edc_session_manager_free(rt->session_manager);
    if (rt->config)
        edc_config_free(rt->config);
    if (rt->toolbox)
        edc_toolbox_free(rt->toolbox);

    edc_secret_cleanup();
    free(rt);
}
