#include "edc_agent.h"
#include "edc_actor.h"
#include "edc_channel_actor.h"
#include "edc_config.h"
#include "edc_init_env.h"
#include "edc_llm_actor.h"
#include "edc_memory_actor.h"
#include "edc_network_async.h"
#include "edc_sandbox.h"
#include "edc_secret.h"
#include "edc_session.h"
#include "edc_toolbox_actor.h"
#include "edc_turn.h"
#include "log.h"
#include <edc_commands.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

static void edc_agent_handle_tool_calls(struct edc_agent *agent, struct edc_channel *call_from,
                                       struct edc_llm_actor_resp *resp, gpointer request_id)
{
    struct edc_toolbox_actor_req *req = edc_toolbox_actor_req_new(NULL, NULL, NULL, NULL);
    struct edc_toolbox_actor_req *head = req;

    for (int i = 0; i < resp->tool_calls->len; i++)
    {
        struct edc_llm_chat_tool_call *tool_call = &g_array_index(resp->tool_calls, struct edc_llm_chat_tool_call, i);
        if (tool_call->type == BC_LLM_TOOL_CALL_TYPE_FUNCTION)
        {
            req->next = edc_toolbox_actor_req_new(tool_call->function.name->str, tool_call->function.arguments,
                                                 tool_call->id->str, call_from);
            req = req->next;

            tool_call->function.arguments = NULL;
            g_string_free(tool_call->function.name, FALSE);
            tool_call->function.name = NULL;
            g_string_free(tool_call->id, FALSE);
            tool_call->id = NULL;
        }
    }

    req = head->next;
    edc_toolbox_actor_req_free(head);
    if (req)
    {
        edc_actor_send(agent->toolbox_actor, edc_actor_msg_new(BC_ACTOR_MSG_TOOL_REQUEST, agent->scheduler, request_id,
                                                             req, (edc_actor_data_free_fn)edc_toolbox_actor_req_free));
    }
}

struct edc_channel_turn
{
    struct edc_turn *turn;
    struct edc_channel *channel;
};

struct edc_channel_turn *edc_channel_turn_new(struct edc_channel *channel)
{
    struct edc_channel_turn *turn = g_new0(struct edc_channel_turn, 1);
    turn->turn = edc_turn_new();
    turn->channel = channel;
    return turn;
}

void edc_channel_turn_free(struct edc_channel_turn *turn)
{
    if (turn->channel)
    {
        edc_channel_free(turn->channel);
    }

    if (turn->turn)
    {
        edc_turn_free(turn->turn);
    }

    g_free(turn);
}

static void handle_llm_resp(struct edc_agent *agent, edc_actor_t *actor, edc_actor_msg_t *msg)
{
    struct edc_channel_turn *turn = g_hash_table_lookup(agent->turns, msg->request_id);
    if (!turn)
    {
        log_info("turn %p is not exists. remove now.", msg->request_id);
        return;
    }

    struct edc_llm_actor_resp *resp = msg->data;
    struct edc_channel_message *reply = NULL;

    if (resp->error)
    {
        log_error("[ERROR]: %s", resp->error->message);
        reply = edc_channel_message_new(turn->channel, g_strdup(resp->error->message));
        turn->channel = NULL;

        log_info("agent remove turn reqid = %p", msg->request_id);
        g_hash_table_remove(agent->turns, msg->request_id);
        edc_channel_turn_free(turn);
        goto exit;
    }

    if (resp->reason == BC_LLM_FINISH_REASON_TOOL_CALL)
    {
        if ((resp->reasoning_content && resp->reasoning_content->len > 0) || (resp->content && resp->content->len > 0))
        {
            gchar *content =
                (resp->content && resp->content->len > 0) ? resp->content->str : resp->reasoning_content->str;

            reply = edc_channel_ref_message_new(turn->channel, g_strdup_printf("[思考]: %s", content));
        }

        edc_turn_add_assistant_message(turn->turn, resp->msg);
        edc_agent_handle_tool_calls(agent, turn->channel, resp, msg->request_id);
        resp->msg = NULL;
    }
    else if (resp->reason == BC_LLM_FINISH_REASON_STOP)
    {
        reply = edc_channel_message_new(turn->channel, g_strdup(resp->content->str));
        turn->channel = NULL;

        edc_turn_add_assistant_message(turn->turn, resp->msg);
        resp->msg = NULL;
        log_info("agent remove turn reqid = %p", msg->request_id);
        g_hash_table_remove(agent->turns, msg->request_id);
        edc_session_add_turn(agent->session, turn->turn);
        edc_channel_turn_free(turn);
        edc_session_save_history(agent->session);
    }
    else
    {
        reply = edc_channel_message_new(
            turn->channel, g_strdup_printf("Internal error, edc_llm_chat unknown stop reason: %d", resp->reason));
        turn->channel = NULL;

        log_info("agent remove turn reqid = %p", msg->request_id);
        g_hash_table_remove(agent->turns, msg->request_id);
        edc_channel_turn_free(turn);
    }

exit:
    if (reply)
    {
        edc_actor_send(agent->channel_actor,
                      edc_actor_msg_new_sub_type(BC_ACTOR_MSG_CHANNEL_SEND, reply->channel->type, actor, msg->request_id,
                                                reply, (edc_actor_data_free_fn)edc_channel_message_free));
    }
}

static void handle_tool_resp(struct edc_agent *agent, edc_actor_t *actor, edc_actor_msg_t *msg)
{
    struct edc_toolbox_actor_req *req = msg->data;
    struct edc_channel_turn *turn = g_hash_table_lookup(agent->turns, msg->request_id);

    if (!turn)
    {
        log_info("turn %p is not exists. remove now.", msg->request_id);
        g_hash_table_remove(agent->turns, msg->request_id);
        return;
    }

    if (req->result)
    {
        edc_turn_add_tool_message(turn->turn, req->call_id, req->name, req->result->str);
    }

    if (!req->next)
    {
        struct edc_llm_actor_req *req = edc_llm_actor_req_new(edc_session_get_messages(agent->session),
                                                            agent->toolbox->schemas, turn->turn->messages);
        edc_actor_msg_t *llm_msg = edc_actor_msg_new(BC_ACTOR_MSG_LLM_REQUEST, actor, msg->request_id, req,
                                                   (edc_actor_data_free_fn)edc_llm_actor_req_free);

        edc_session_add_turn(agent->session, turn->turn);
        edc_actor_send(agent->llm_actor, llm_msg);
    }
}

static void handle_user_input(struct edc_agent *agent, edc_actor_t *actor, edc_actor_msg_t *msg)
{
    struct edc_channel_message *input = msg->data;
    if (edc_commands_handler(agent, actor, input))
    {
        return;
    }

    log_info("agent insert turn reqid = %p", msg->request_id);
    struct edc_channel_turn *turn = edc_channel_turn_new(input->channel);
    edc_turn_add_user_message(turn->turn, input->content);
    g_hash_table_insert(agent->turns, msg->request_id, turn);
    input->channel = NULL;

    struct edc_llm_actor_req *req =
        edc_llm_actor_req_new(edc_session_get_messages(agent->session), agent->toolbox->schemas, turn->turn->messages);
    edc_actor_msg_t *llm_msg = edc_actor_msg_new(BC_ACTOR_MSG_LLM_REQUEST, actor, msg->request_id, req,
                                               (edc_actor_data_free_fn)edc_llm_actor_req_free);

    edc_actor_send(agent->llm_actor, llm_msg);
}

static void edc_agent_scheduler_handler(struct edc_actor *actor, struct edc_actor_msg *msg)
{
    struct edc_agent *agent = actor->state;
    switch (msg->type)
    {
    case BC_ACTOR_MSG_CHANNEL_RECV:
        handle_user_input(agent, actor, msg);
        break;

    case BC_ACTOR_MSG_LLM_RESPONSE:
        handle_llm_resp(agent, actor, msg);
        break;

    case BC_ACTOR_MSG_TOOL_RESPONSE:
        handle_tool_resp(agent, actor, msg);
        break;
    case BC_ACTOR_MSG_MEMORY_DONE:
        edc_session_reload(agent->session, TRUE);
        log_info("memory compress done. reload session, message count = %zu",
                 edc_session_get_message_count(agent->session));
    default:
        break;
    }

    if (edc_session_get_message_count(agent->session) > agent->config->memory_compress_threshold &&
        !edc_memory_actor_is_compressing(agent->memory_actor)) // msg + system prompt
    {
        log_info("message count = %zu, start compress.", edc_session_get_message_count(agent->session));
        edc_actor_send(agent->memory_actor,
                      edc_actor_msg_new(BC_ACTOR_MSG_MEMORY_COMPRESS, agent->scheduler, NULL, NULL, NULL));
    }

    edc_actor_msg_free(msg);
}

struct edc_agent *edc_agent_new(gboolean setup, gboolean enable_sandbox)
{
    gchar *edc_home = edc_config_resolve_bc_home();

    if (!edc_init_env(edc_home, setup))
    {
        log_error("Failed to initialize environment");
        g_free(edc_home);
        return NULL;
    }

    if (!edc_secret_init(FALSE, edc_home))
    {
        log_error("Failed to initialize secret module");
        g_free(edc_home);
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

    gchar *pem = g_build_filename(agent->config->workspace, "ssl", "cacert.pem", NULL);
    agent->network = edc_network_async_new(pem);
    g_free(pem);

    agent->scheduler = edc_actor_new("schuler", edc_agent_scheduler_handler, agent);
    agent->channel_actor = edc_channel_actor_new(&agent->config->channel, agent->network);
    agent->llm_actor = edc_llm_actor_new(&agent->config->llm_api_conf, agent->network);
    agent->memory_actor = edc_memory_actor_new(agent->config->workspace, agent->config->memory_compress_threshold, agent->llm_actor);
    agent->session = edc_session_new(agent->config);

    edc_channel_actor_set_observer(agent->channel_actor, agent->scheduler);

    agent->turns = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, NULL);

    agent->runner = edc_actor_runner_new();
    edc_actor_runner_add(agent->runner, agent->scheduler);
    edc_actor_runner_add(agent->runner, agent->channel_actor);
    edc_actor_runner_add(agent->runner, agent->llm_actor);
    edc_actor_runner_add(agent->runner, agent->memory_actor);

    agent->toolbox = edc_toolbox_new(agent->config, agent->scheduler, agent->channel_actor);
    agent->toolbox_actor = edc_toolbox_actor_new(agent->toolbox);
    edc_actor_runner_add(agent->runner, agent->toolbox_actor);

    edc_actor_runner_start(agent->runner);

    g_free(edc_home);
    return agent;
}

void edc_agent_remove_all_turns(struct edc_agent *agent)
{
    GList *turns = g_hash_table_get_values(agent->turns);
    GList *p = turns;
    while (p)
    {
        struct edc_channel_turn *turn = p->data;
        edc_channel_turn_free(turn);
        p = g_list_next(p);
    }

    g_list_free(turns);
    g_hash_table_remove_all(agent->turns);
}

void edc_agent_free(struct edc_agent *agent)
{
    if (!agent)
        return;

    if (agent->network)
    {
        edc_network_async_free(agent->network);
    }

    edc_actor_runner_free(agent->runner);
    edc_channel_actor_free(agent->channel_actor);
    edc_toolbox_actor_free(agent->toolbox_actor);
    edc_llm_actor_free(agent->llm_actor);
    edc_memory_actor_free(agent->memory_actor);
    edc_actor_free(agent->scheduler);

    edc_agent_remove_all_turns(agent);
    g_hash_table_destroy(agent->turns);

    if (agent->session)
    {
        edc_session_free(agent->session);
    }

    if (agent->config)
    {
        edc_config_free(agent->config);
    }

    if (agent->toolbox)
    {
        edc_toolbox_free(agent->toolbox);
    }

    edc_secret_cleanup();

    g_free(agent);
}
