#include "edc_string.h"
#include "edc_strutil.h"
#include "edc_error.h"
#include "edc_llm_actor.h"
#include "edc_actor.h"
#include "edc_network_async.h"
#include "log.h"
#include "mongoose.h"
#include "utarray.h"
struct edc_llm_actor_req *edc_llm_actor_req_new(cJSON *msg_lst, cJSON *tools, EdcQueue *turn)
{
    struct edc_llm_actor_req *req = g_new0(struct edc_llm_actor_req, 1);
    req->history = msg_lst;
    req->tools = tools;
    req->turn = turn;
    return req;
};

void edc_llm_actor_req_free(struct edc_llm_actor_req *req)
{
    if (!req)
        return;
    free(req);
}

struct edc_llm_actor_http_req
{
    edc_actor_t *actor;
    const char *body;
    struct llm_api_conf *api_conf;
    edc_actor_t *from;
    sds response;
    void* request_id;
};

static struct edc_llm_actor_http_req *edc_llm_actor_http_req_new(edc_actor_t *actor, const char *body,
                                                               struct llm_api_conf *api_conf, edc_actor_t *from,
                                                               void* request_id)
{
    struct edc_llm_actor_http_req *req = g_new0(struct edc_llm_actor_http_req, 1);
    req->actor = actor;
    req->body = body;
    req->api_conf = api_conf;
    req->from = from;
    req->response = NULL;
    req->request_id = request_id;
    return req;
}

static void edc_llm_actor_http_req_free(struct edc_llm_actor_http_req *req)
{
    free((char *)req->body);
    if (req->response)
    {
        edc_str_free(req->response);
    }
    free(req);
}

static void edc_llm_actor_make_http_request(struct mg_connection *c)
{
    struct edc_llm_actor_http_req *req = c->fn_data;
    struct mg_str host = mg_url_host(req->api_conf->api_url);

    mg_printf(c, "POST %s HTTP/1.1\r\n", mg_url_uri(req->api_conf->api_url));
    mg_printf(c, "Host: %.*s\r\n", (int)host.len, host.buf);
    mg_printf(c, "Content-Type: application/json\r\n");
    if (req->api_conf->api_key)
    {
        mg_printf(c, "Authorization: Bearer %s\r\n", req->api_conf->api_key);
    }
    mg_printf(c, "Content-Length: %d\r\n\r\n", (int)strlen(req->body));
    mg_printf(c, "%s", req->body);
}

static void edc_llm_actor_on_http_fn(struct mg_connection *c, int ev, void *ev_data)
{
    struct edc_llm_actor_http_req *req = ev == BC_CONN_ERROR ? ev_data : c->fn_data;

    if (ev == MG_EV_HTTP_MSG)
    {
        struct mg_http_message *hm = (struct mg_http_message *)ev_data;
        sds buf = edc_str_new(NULL);
        edc_str_catn(buf, hm->body.buf, hm->body.len);
        req->response = buf;

        edc_actor_send(req->actor, edc_actor_msg_new(BC_ACTOR_MSG_LLM_RESPONSE, NULL, req->request_id, req,
                                                   (edc_actor_data_free_fn)edc_llm_actor_http_req_free));
        c->is_closing = 1;
    }
    else if (c->is_tls && ev == MG_EV_TLS_HS)
    {
        edc_llm_actor_make_http_request(c);
    }
    else if (!c->is_tls && ev == MG_EV_CONNECT)
    {
        edc_llm_actor_make_http_request(c);
    }
    else if (ev == BC_CONN_ERROR)
    {
        log_error("llm actor http async conn error");
        if (req->from)
        {
            struct edc_llm_actor_resp *resp = edc_llm_actor_resp_new();
            resp->error = edc_error_new(EDC_ERR_FAILED, "LLM API connection error");
            edc_actor_send(req->from, edc_actor_msg_new(BC_ACTOR_MSG_LLM_RESPONSE, NULL, req->request_id, resp,
                                                       (edc_actor_data_free_fn)edc_llm_actor_resp_free));
        }
        edc_llm_actor_http_req_free(req);
    }
    else if (ev == MG_EV_CLOSE)
    {
        if (req->response == NULL)
        {
            if (req->from)
            {
                struct edc_llm_actor_resp *resp = edc_llm_actor_resp_new();
                resp->error = edc_error_new(EDC_ERR_FAILED, "LLM API connection closed unexpectedly");
                edc_actor_send(req->from, edc_actor_msg_new(BC_ACTOR_MSG_LLM_RESPONSE, NULL, req->request_id, resp,
                                                           (edc_actor_data_free_fn)edc_llm_actor_resp_free));
            }
            edc_llm_actor_http_req_free(req);
        }
    }
    else if (ev == BC_CONN_TIME_OUT || ev == MG_EV_ERROR)
    {
        const char *err_msg = ev == BC_CONN_TIME_OUT ? "LLM API request timed out" : "LLM API request error";
        if (req->from)
        {
            struct edc_llm_actor_resp *resp = edc_llm_actor_resp_new();
            resp->error = edc_error_new(EDC_ERR_FAILED, "%s", err_msg);
            edc_actor_send(req->from, edc_actor_msg_new(BC_ACTOR_MSG_LLM_RESPONSE, NULL, req->request_id, resp,
                                                       (edc_actor_data_free_fn)edc_llm_actor_resp_free));
        }
        edc_llm_actor_http_req_free(req);
    }
}

static void edc_llm_actor_messages_add_turn(void* data, void* user_data)
{
    cJSON *item = data;
    cJSON *messages = user_data;
    cJSON_AddItemReferenceToArray(messages, item);
}

static void edc_llm_actor_send_req(edc_actor_t *actor, edc_actor_t *from, struct edc_llm_actor_req *req,
                                  void* request_id)
{
    edc_llm_actor_state_t *state = (edc_llm_actor_state_t *)actor->state;

    cJSON *body = cJSON_CreateObject();

    cJSON_AddStringToObject(body, "model", state->api_conf.model);

    cJSON *messages = cJSON_AddArrayToObject(body, "messages");
    for (int i = 0; i < cJSON_GetArraySize(req->history); ++i)
    {
        cJSON *item = cJSON_GetArrayItem(req->history, i);
        cJSON_AddItemReferenceToArray(messages, item);
    }

    if (req->turn)
    {
        for (size_t qi = req->turn->head; qi < req->turn->tail; qi++)
        {
            edc_llm_actor_messages_add_turn(req->turn->data[qi], messages);
        }
    }

    if (req->tools)
    {
        cJSON_AddItemReferenceToObject(body, "tools", req->tools);
    }

    const char *body_s = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);

    const char *proto = mg_url_is_ssl(state->api_conf.api_url) ? "https://" : "http://";
    struct mg_str host = mg_url_host(state->api_conf.api_url);
    int port = (int)mg_url_port(state->api_conf.api_url);
    char *url = edc_strdup_printf("%s%.*s:%d", proto, (int)host.len, host.buf, port);

    edc_http_async_connect(state->network, url, NULL, edc_llm_actor_on_http_fn,
                          edc_llm_actor_http_req_new(actor, body_s, &state->api_conf, from, request_id), 60 * 5);
    free(url);
}

static UT_icd ut_tool_call_icd = {sizeof(struct edc_llm_chat_tool_call), NULL, NULL, NULL};

static bool edc_llm_actor_parse_resp(cJSON *response, struct edc_llm_actor_resp *resp)
{
    resp->error = NULL;
    cJSON *choices = cJSON_GetObjectItem(response, "choices");
    if (!choices || !cJSON_IsArray(choices) || cJSON_GetArraySize(choices) == 0)
    {
        char *err_msg = cJSON_Print(response);
        log_error("resp format error choice 0 is not exist: %s", response);
        resp->error = edc_error_new(EDC_ERR_INVAL, "resp format choice 0 is not exist: %s", err_msg);
        free(err_msg);

        if (choices)
        {
            cJSON_Delete(choices);
        }

        return FALSE;
    }

    cJSON *choice = cJSON_GetArrayItem(choices, 0);
    if (!choice)
    {
        return FALSE;
    }

    cJSON *finish_reason = cJSON_GetObjectItem(choice, "finish_reason");
    if (finish_reason && cJSON_IsString(finish_reason))
    {
        const char *finish_reason_s = cJSON_GetStringValue(finish_reason);
        if (strcmp(finish_reason_s, "stop") == 0)
        {
            resp->reason = BC_LLM_FINISH_REASON_STOP;
        }
        else if (strcmp(finish_reason_s, "tool_calls") == 0)
        {
            resp->reason = BC_LLM_FINISH_REASON_TOOL_CALL;
        }
        else if (strcmp(finish_reason_s, "length") == 0)
        {
            resp->reason = BC_LLM_FINISH_REASON_LENGTH;
        }
        else if (strcmp(finish_reason_s, "sensitive") == 0)
        {
            resp->reason = BC_LLM_FINISH_REASON_SENSITIVE;
        }
        else if (strcmp(finish_reason_s, "network_error") == 0)
        {
            resp->reason = BC_LLM_FINISH_REASON_NETWORK_ERROR;
        }
        else if (strcmp(finish_reason_s, "context_length_exceeded") == 0)
        {
            resp->reason = BC_LLM_FINISH_REASON_CONTEXT_EXCEEDED;
        }
        else
        {
            resp->reason = BC_LLM_FINISH_REASON_UNKNOWN;
        }
    }
    else
    {
        resp->reason = BC_LLM_FINISH_REASON_UNKNOWN;
    }

    if (resp->reason == BC_LLM_FINISH_REASON_UNKNOWN)
    {
        char *s = cJSON_Print(choice);
        log_error("llm unkown stop reason: %s", s);
        free(s);
    }

    cJSON *index = cJSON_GetObjectItem(choice, "index");
    if (index && cJSON_IsNumber(index))
    {
        resp->index = index->valueint;
    }

    cJSON *message = cJSON_DetachItemFromObject(choice, "message");
    if (!message)
    {
        char *err_msg = cJSON_Print(choice);
        resp->error = edc_error_new(EDC_ERR_INVAL, "resp format message is not exist: %s", err_msg);
        free(err_msg);
        return FALSE;
    }

    cJSON *content = cJSON_GetObjectItem(message, "content");
    if (content && cJSON_IsString(content))
    {
        resp->content = edc_str_new(content->valuestring);
    }

    cJSON *reasoning_content = cJSON_GetObjectItem(message, "reasoning_content");
    if (reasoning_content && cJSON_IsString(reasoning_content))
    {
        resp->reasoning_content = edc_str_new(reasoning_content->valuestring);
    }

    cJSON *role = cJSON_GetObjectItem(message, "role");
    if (role && cJSON_IsString(role))
    {
        if (strcmp(role->valuestring, "user") == 0)
        {
            resp->role = BC_LLM_CHAT_ROLE_USER;
        }
        else if (strcmp(role->valuestring, "assistant") == 0)
        {
            resp->role = BC_LLM_CHAT_ROLE_ASSISTANT;
        }
        else if (strcmp(role->valuestring, "system") == 0)
        {
            resp->role = BC_LLM_CHAT_ROLE_SYSTEM;
        }
    }

    cJSON *tool_calls = cJSON_GetObjectItem(message, "tool_calls");
    if (tool_calls && cJSON_IsArray(tool_calls))
    {
        utarray_new(resp->tool_calls, &ut_tool_call_icd);
        int num_tool_calls = cJSON_GetArraySize(tool_calls);

        for (int i = 0; i < num_tool_calls; i++)
        {
            cJSON *tool_call = cJSON_GetArrayItem(tool_calls, i);
            if (!tool_call)
                continue;

            struct edc_llm_chat_tool_call tc = {0};

            cJSON *id = cJSON_GetObjectItem(tool_call, "id");
            if (id && cJSON_IsString(id))
            {
                tc.id = edc_str_new(id->valuestring);
            }

            cJSON *tc_index = cJSON_GetObjectItem(tool_call, "index");
            if (tc_index && cJSON_IsNumber(tc_index))
            {
                tc.index = (size_t)tc_index->valueint;
            }

            cJSON *type = cJSON_GetObjectItem(tool_call, "type");
            if (type && cJSON_IsString(type))
            {
                if (strcmp(type->valuestring, "function") == 0)
                {
                    tc.type = BC_LLM_TOOL_CALL_TYPE_FUNCTION;
                }
            }

            cJSON *function = cJSON_GetObjectItem(tool_call, "function");

            if (function && cJSON_IsObject(function))
            {
                cJSON *name = cJSON_GetObjectItem(function, "name");
                if (name && cJSON_IsString(name))
                {
                    tc.function.name = edc_str_new(name->valuestring);
                }

                cJSON *arguments = cJSON_GetObjectItem(function, "arguments");
                if (arguments && cJSON_IsString(arguments))
                {
                    tc.function.arguments = cJSON_Parse(arguments->valuestring);
                }
            }

            utarray_push_back(resp->tool_calls, &tc);
        }
    }

    resp->msg = message;

    return TRUE;
}

static void edc_llm_actor_handle_respone(edc_actor_t *actor, edc_actor_msg_t *msg)
{
    struct edc_llm_actor_http_req *req = (struct edc_llm_actor_http_req *)msg->data;
    sds buf = req->response;

    struct edc_llm_actor_resp *resp = edc_llm_actor_resp_new();
    cJSON *body = cJSON_Parse(buf);
    if (!body)
    {
        resp->error = edc_error_new(EDC_ERR_FAILED, "resp format error: %s", buf);
        goto exit;
    }

    bool ok = edc_llm_actor_parse_resp(body, resp);
    if (!ok)
    {
        log_error("parse resp error: %s", resp->error->message);
        goto exit;
    }

exit:
    cJSON_Delete(body);
    if (req->from)
    {
        edc_actor_send(req->from, edc_actor_msg_new(BC_ACTOR_MSG_LLM_RESPONSE, actor, msg->request_id, resp,
                                                  (edc_actor_data_free_fn)edc_llm_actor_resp_free));
    }
}

static void edc_llm_actor_handler(edc_actor_t *actor, edc_actor_msg_t *msg)
{
    switch (msg->type)
    {
    case BC_ACTOR_MSG_LLM_REQUEST: {
        struct edc_llm_actor_req *req = msg->data;
        edc_llm_actor_send_req(actor, msg->from, req, msg->request_id);
        break;
    }
    case BC_ACTOR_MSG_LLM_RESPONSE: {
        edc_llm_actor_handle_respone(actor, msg);
        break;
    }
    default:
        break;
    }

    edc_actor_msg_free(msg);
}

struct edc_llm_actor_resp *edc_llm_actor_resp_new()
{
    struct edc_llm_actor_resp *resp = g_new0(struct edc_llm_actor_resp, 1);
    resp->content = NULL;
    resp->reasoning_content = NULL;
    resp->content = NULL;
    resp->index = 0;
    resp->reason = BC_LLM_FINISH_REASON_UNKNOWN;
    resp->tool_calls = NULL;
    resp->role = BC_LLM_CHAT_ROLE_ASSISTANT;
    resp->error = NULL;

    return resp;
}

void edc_llm_actor_resp_free(struct edc_llm_actor_resp *resp)
{
    if (!resp)
        return;

    if (resp->error)
    {
        edc_error_free(resp->error);
        resp->error = NULL;
    }

    if (resp->content)
    {
        edc_str_free(resp->content);
    }

    if (resp->reasoning_content)
    {
        edc_str_free(resp->reasoning_content);
    }

    if (resp->tool_calls)
    {
        for (unsigned i = 0; i < utarray_len(resp->tool_calls); ++i)
        {
            struct edc_llm_chat_tool_call call = *(struct edc_llm_chat_tool_call*)utarray_eltptr(resp->tool_calls, i);
            if (call.id)
            {
                edc_str_free(call.id);
            }

            if (call.function.arguments)
            {
                cJSON_Delete(call.function.arguments);
            }

            if (call.function.name)
            {
                edc_str_free(call.function.name);
            }
        }

        utarray_free(resp->tool_calls);
    }

    if (resp->msg)
    {
        cJSON_Delete(resp->msg);
    }

    free(resp); // FIX: free the resp struct itself
}

edc_actor_t *edc_llm_actor_new(struct llm_api_conf *api_conf, edc_network_async_t *network)
{
    edc_llm_actor_state_t *state = g_new0(edc_llm_actor_state_t, 1);
    state->api_conf = *api_conf;
    state->network = network;

    edc_actor_t *actor = edc_actor_new("edc_llm_actor", edc_llm_actor_handler, state);
    return actor;
}

void edc_llm_actor_free(edc_actor_t *actor)
{
    edc_llm_actor_state_t *state = actor->state;
    free(state);
    edc_actor_free(actor);
}
