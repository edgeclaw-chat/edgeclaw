#include "sds.h"
#include "edc_agent.h"
#include "edc_actor.h"
#include "edc_compat.h"
#include "edc_file_utils.h"
#include "uthash.h"
#include "edc_channel_actor.h"
#include "edc_config.h"
#include "edc_context.h"
#include "edc_init_env.h"
#include "edc_llm_actor.h"
#include "edc_memory_actor.h"
#include "edc_sandbox.h"
#include "edc_secret.h"
#include "edc_session.h"
#include "edc_task.h"
#include "edc_task_group.h"
#include "edc_toolbox_actor.h"
#include "edc_turn.h"
#include "utlist_fix.h"
#include "log.h"
#include <edc_commands.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

/* ── 辅助: 获取 agent ──────────────────────────────────────── */

static struct edc_agent *edc_scheduler_agent(struct edc_scheduler *sched)
{
    return sched ? sched->ctx : NULL;
}

/* ── 回调: 发送 LLM 请求 ──────────────────────────────────── */

static void edc_agent_cb_send_llm(struct edc_scheduler *sched, struct edc_task *task,
                                   void *request_id)
{
    struct edc_agent *agent = edc_scheduler_agent(sched);
    if (!agent || !task) return;

    /* Group 级上下文: group history (可共享) + task 专属 (dep outputs) */
    cJSON *msgs;
    if (task->group && cJSON_GetArraySize(task->group->history) > 0)
    {
        msgs = cJSON_CreateArray();
        /* fork 上下文切片: 只取 fork 点之前的 group history */
        size_t end = task->forked_from ? task->fork_point_msg_count
                                       : (size_t)cJSON_GetArraySize(task->group->history);
        if (end > (size_t)cJSON_GetArraySize(task->group->history))
            end = (size_t)cJSON_GetArraySize(task->group->history);
        for (size_t i = 0; i < end; i++)
        {
            cJSON *item = cJSON_GetArrayItem(task->group->history, i);
            cJSON_AddItemReferenceToArray(msgs, item);
        }
    }
    else
    {
        msgs = cJSON_CreateArray();
        /* 若 group 无历史，注入 system prompt */
        if (agent->config)
        {
            sds prompt = edc_context_build_system_prompt(agent->config);
            cJSON *sys = cJSON_CreateObject();
            cJSON_AddStringToObject(sys, "role", "system");
            cJSON_AddStringToObject(sys, "content", prompt);
            cJSON_AddItemToArray(msgs, sys);
            sdsfree(prompt);
        }
    }

    /* 注入已完成依赖 task 的输出 (user inputs + final output) */
    struct edc_task_dep *dep;
    LL_FOREACH(task->deps, dep)
    {
        log_info("send_llm task=%s dep src=%s completed=%d output=%s",
                 task->task_id,
                 dep->source ? dep->source->task_id : "(null)",
                 dep->completed,
                 (dep->source && dep->source->output) ? "yes" : "none");
        if (dep->completed && dep->source && !dep->injected)
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
                                if (last_user) sdsfree(last_user);
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

    struct edc_llm_actor_req *req =
        edc_llm_actor_req_new(msgs,
                              agent->toolbox ? agent->toolbox->schemas : NULL,
                              task->active_turn ? &task->active_turn->messages : NULL);
    req->history_owned = true;

    edc_actor_send(agent->llm_actor,
                   edc_actor_msg_new(BC_ACTOR_MSG_LLM_REQUEST,
                                     edc_scheduler_get_actor(sched),
                                     request_id, req,
                                     (edc_actor_data_free_fn)edc_llm_actor_req_free));
}

/* ── 回调: 发送 Channel 回复 ────────────────────────────────── */

static void edc_agent_cb_send_reply(struct edc_scheduler *sched, struct edc_task *task,
                                     const char *content, void *request_id)
{
    struct edc_agent *agent = edc_scheduler_agent(sched);
    if (!agent || !task || !task->channel || !content) return;

    /* only active task outputs to terminal */
    if (!sched->session || edc_session_get_active_task(sched->session) != task)
        return;

    struct edc_channel_message *reply =
        edc_channel_message_new(edc_channel_copy(task->channel), sdsnew(content));

    edc_actor_send(agent->channel_actor,
                   edc_actor_msg_new_sub_type(BC_ACTOR_MSG_CHANNEL_SEND,
                                              reply->channel->type,
                                              edc_scheduler_get_actor(sched),
                                              request_id, reply,
                                              (edc_actor_data_free_fn)edc_channel_message_free));
}

/* ── 回调: 分发 Tool Calls ────────────────────────────────── */

static void edc_agent_handle_tool_calls(struct edc_agent *agent, struct edc_channel *call_from,
                                         struct edc_llm_actor_resp *resp, void *request_id,
                                         edc_actor_t *from_actor);

static void edc_agent_cb_dispatch_tools(struct edc_scheduler *sched, struct edc_task *task,
                                         void *llm_resp, void *request_id)
{
    struct edc_agent *agent = edc_scheduler_agent(sched);
    if (!agent || !task || !llm_resp) return;

    struct edc_llm_actor_resp *resp = llm_resp;

    /* 思维内容回显 */
    if ((resp->reasoning_content && sdslen(resp->reasoning_content) > 0) ||
        (resp->content && sdslen(resp->content) > 0))
    {
        sds content = (resp->content && sdslen(resp->content) > 0)
                          ? resp->content
                          : resp->reasoning_content;
        sds text = sdscatprintf(sdsempty(), "[思考]: %s", content);
        edc_agent_cb_send_reply(sched, task, text, request_id);
        sdsfree(text);
    }

    edc_agent_handle_tool_calls(agent, task->channel, resp, request_id,
                                 edc_scheduler_get_actor(sched));
}

/* ── 工具分发逻辑 ──────────────────────────────────────────── */

static void edc_agent_handle_tool_calls(struct edc_agent *agent, struct edc_channel *call_from,
                                         struct edc_llm_actor_resp *resp, void *request_id,
                                         edc_actor_t *from_actor)
{
    struct edc_toolbox_actor_req *req = edc_toolbox_actor_req_new(NULL, NULL, NULL, NULL);
    struct edc_toolbox_actor_req *head = req;

    for (unsigned i = 0; i < utarray_len(resp->tool_calls); i++)
    {
        struct edc_llm_chat_tool_call *tool_call =
            (struct edc_llm_chat_tool_call *)utarray_eltptr(resp->tool_calls, i);
        if (tool_call->type == BC_LLM_TOOL_CALL_TYPE_FUNCTION)
        {
            req->next = edc_toolbox_actor_req_new(tool_call->function.name,
                                                  tool_call->function.arguments,
                                                  tool_call->id, call_from);
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
        edc_actor_send(agent->toolbox_actor,
                       edc_actor_msg_new(BC_ACTOR_MSG_TOOL_REQUEST, from_actor,
                                         request_id, req,
                                         (edc_actor_data_free_fn)edc_toolbox_actor_req_free));
    }
}

/* ── 回调: 命令处理 ────────────────────────────────────────── */

static bool edc_agent_cb_handle_command(struct edc_scheduler *sched,
                                         struct edc_actor *actor,
                                         struct edc_channel_message *msg)
{
    struct edc_agent *agent = edc_scheduler_agent(sched);
    if (!agent || !msg) return false;
    return edc_commands_handler(agent, actor, msg);
}

/* ── 回调: Memory Compress ──────────────────────────────────── */

static void edc_agent_cb_trigger_memory(struct edc_scheduler *sched)
{
    struct edc_agent *agent = edc_scheduler_agent(sched);
    if (!agent || !agent->memory_actor) return;

    edc_actor_send(agent->memory_actor,
                   edc_actor_msg_new(BC_ACTOR_MSG_MEMORY_COMPRESS,
                                     edc_scheduler_get_actor(sched),
                                     NULL, NULL, NULL));
}

/* ── 回调: 保存 Session ────────────────────────────────────── */

static void edc_agent_cb_save_session(struct edc_scheduler *sched)
{
    struct edc_agent *agent = edc_scheduler_agent(sched);
    if (!agent || !agent->session) return;
    edc_session_save_history(agent->session);
}

void edc_agent_remove_all_turns(struct edc_agent *agent)
{
    if (!agent || !agent->scheduler) return;
    edc_scheduler_send_signal_to_all(agent->scheduler, TASK_SIGTERM);
}

/* ── 生命周期 ──────────────────────────────────────────────── */

struct edc_agent *edc_agent_new(bool setup, bool enable_sandbox)
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
    agent->config = edc_config_load(edc_home);

    edc_secret_lockdown();

    agent->config->enable_sandbox = enable_sandbox;
    if (enable_sandbox)
    {
        edc_sandbox_init(agent->config);
    }

    agent->channel_actor = edc_channel_actor_new(&agent->config->channel, agent->config->workspace);
    agent->llm_actor = edc_llm_actor_new(&agent->config->llm_api_conf);
    agent->memory_actor = edc_memory_actor_new(agent->config->workspace,
                                                agent->config->memory_compress_threshold,
                                                agent->llm_actor);
    agent->session = edc_session_new(agent->config);

    /* 创建 scheduler */
    agent->scheduler = edc_scheduler_new(agent->session, agent->config);
    agent->scheduler->ctx = agent;

    /* 注入回调 */
    agent->scheduler->send_llm       = edc_agent_cb_send_llm;
    agent->scheduler->send_reply     = edc_agent_cb_send_reply;
    agent->scheduler->dispatch_tools = edc_agent_cb_dispatch_tools;
    agent->scheduler->handle_command = edc_agent_cb_handle_command;
    agent->scheduler->trigger_memory = edc_agent_cb_trigger_memory;
    agent->scheduler->save_session   = edc_agent_cb_save_session;

    edc_channel_actor_set_observer(agent->channel_actor,
                                    edc_scheduler_get_actor(agent->scheduler));

    agent->runner = edc_actor_runner_new();
    edc_file_utils_init(&agent->runner->loop);
    edc_actor_runner_add(agent->runner, edc_scheduler_get_actor(agent->scheduler));
    edc_actor_runner_add(agent->runner, agent->channel_actor);
    edc_actor_runner_add(agent->runner, agent->llm_actor);
    edc_actor_runner_add(agent->runner, agent->memory_actor);

    agent->toolbox = edc_toolbox_new(agent->config,
                                      edc_scheduler_get_actor(agent->scheduler),
                                      agent->channel_actor);
    agent->toolbox_actor = edc_toolbox_actor_new(agent->toolbox);
    edc_actor_runner_add(agent->runner, agent->toolbox_actor);

    edc_actor_runner_start(agent->runner);

    sdsfree(edc_home);
    return agent;
}

void edc_agent_free(struct edc_agent *agent)
{
    if (!agent) return;

    edc_actor_runner_free(agent->runner);
    edc_channel_actor_free(agent->channel_actor);
    edc_toolbox_actor_free(agent->toolbox_actor);
    edc_llm_actor_free(agent->llm_actor);
    edc_memory_actor_free(agent->memory_actor);

    edc_scheduler_free(agent->scheduler);

    edc_agent_remove_all_turns(agent);

    if (agent->session) edc_session_free(agent->session);
    if (agent->config)  edc_config_free(agent->config);
    if (agent->toolbox) edc_toolbox_free(agent->toolbox);

    edc_secret_cleanup();
    free(agent);
}
