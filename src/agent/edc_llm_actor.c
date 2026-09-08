#include "edc_llm_actor.h"
#include "edc_actor.h"
#include "edc_compat.h"
#include "edc_error.h"
#include "edc_file_utils.h"
#include "edc_https_client.h"
#include "log.h"
#include "sds.h"
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
    if (req->history_owned && req->history)
        cJSON_Delete(req->history);
    if (req->tail_system)
        cJSON_Delete(req->tail_system);
    free(req);
}

struct edc_llm_actor_http_req
{
    edc_actor_t *actor;
    const char *body;
    struct llm_api_conf *api_conf;
    edc_actor_t *from;
    sds response;
    void *request_id;
};

static struct edc_llm_actor_http_req *edc_llm_actor_http_req_new(edc_actor_t *actor, const char *body,
                                                                 struct llm_api_conf *api_conf, edc_actor_t *from,
                                                                 void *request_id)
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
    cJSON_free((char *)req->body);
    if (req->response)
    {
        sdsfree(req->response);
    }
    free(req);
}

static void edc_llm_actor_on_http_fn(struct edc_https_request *req, int ev, void *data)
{
    struct edc_llm_actor_http_req *httpreq = edc_https_request_get_priv(req);

    if (ev == EDC_HTTPS_EVENT_HS)
    {
        sds request = sdscatprintf(sdsempty(),
                                   "POST /%s HTTP/1.1\r\n"
                                   "Host: %s\r\n"
                                   "Content-Type: application/json\r\n"
                                   "Connection: close\r\n",
                                   edc_https_request_get_path(req), edc_https_request_get_host(req));

        if (httpreq->api_conf->api_key)
        {
            request = sdscatprintf(request, "Authorization: Bearer %s\r\n", httpreq->api_conf->api_key);
        }

        request = sdscatprintf(request, "Content-Length: %zu\r\n\r\n", strlen(httpreq->body));
        request = sdscatprintf(request, "%s", httpreq->body);

        edc_https_request_write(req, (unsigned char *)request, sdslen(request));
        sdsfree(request);
    }
    else if (ev == EDC_HTTPS_EVENT_MSG)
    {
        uv_buf_t *buf = data;
        if (!httpreq->response)
        {
            httpreq->response = sdsnewlen(buf->base, buf->len);
        }
        else
        {
            httpreq->response = sdscatlen(httpreq->response, buf->base, buf->len);
        }
        free(buf->base);
    }
    else if (ev == EDC_HTTPS_EVENT_CLOSE)
    {
        if (httpreq->response)
        {
            edc_actor_send(httpreq->actor,
                           edc_actor_msg_new(BC_ACTOR_MSG_LLM_RESPONSE, NULL, httpreq->request_id, httpreq,
                                             (edc_actor_data_free_fn)edc_llm_actor_http_req_free));
        }
        else
        {
            if (httpreq->from)
            {
                struct edc_llm_actor_resp *resp = edc_llm_actor_resp_new();
                resp->error = edc_error_new(EDC_ERR_FAILED, "LLM API connection closed unexpectedly");
                edc_actor_send(httpreq->from, edc_actor_msg_new(BC_ACTOR_MSG_LLM_RESPONSE, NULL, httpreq->request_id,
                                                                resp, (edc_actor_data_free_fn)edc_llm_actor_resp_free));
            }
            edc_llm_actor_http_req_free(httpreq);
        }
    }
    else if (ev == EDC_HTTPS_EVENT_ERROR)
    {
        log_error("llm actor https error: %s", (const char *)data);
    }
}

static void edc_llm_actor_send_req(edc_actor_t *actor, edc_actor_t *from, struct edc_llm_actor_req *req,
                                   void *request_id)
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
            cJSON *item = req->turn->data[qi];
            cJSON_AddItemReferenceToArray(messages, item);
        }
    }

    /* 绝对队尾: 运行时追加的 system 提醒 (如最新 todo reminder) —— 在已提交
     * 历史窗口与当前 turn 之后, 紧贴模型开始生成的位置, 每轮请求重新计算 */
    if (req->tail_system)
    {
        cJSON_AddItemReferenceToArray(messages, req->tail_system);
    }

    if (req->tools)
    {
        cJSON_AddItemReferenceToObject(body, "tools", req->tools);
    }

    /* diagnostic: log message count by role */
    {
        cJSON *msgs = cJSON_GetObjectItem(body, "messages");
        if (msgs)
        {
            int total = cJSON_GetArraySize(msgs);
            int n_user = 0, n_assistant = 0, n_system = 0, n_tool = 0;
            for (int i = 0; i < total; i++)
            {
                cJSON *m = cJSON_GetArrayItem(msgs, i);
                cJSON *role = cJSON_GetObjectItem(m, "role");
                if (role && cJSON_IsString(role))
                {
                    const char *rs = role->valuestring;
                    if (strcmp(rs, "user") == 0)
                        n_user++;
                    else if (strcmp(rs, "assistant") == 0)
                        n_assistant++;
                    else if (strcmp(rs, "system") == 0)
                        n_system++;
                    else if (strcmp(rs, "tool") == 0)
                        n_tool++;
                }
            }
            log_info("LLM request: %d msgs (sys=%d user=%d asst=%d tool=%d)", total, n_system, n_user, n_assistant,
                     n_tool);
        }
    }

    const char *body_s = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);

    struct edc_llm_actor_http_req *httpreq =
        edc_llm_actor_http_req_new(actor, body_s, &state->api_conf, from, request_id);

    edc_https_client_connect(state->client, state->api_conf.api_url, edc_llm_actor_on_http_fn, httpreq);
}

static UT_icd ut_tool_call_icd = {sizeof(struct edc_llm_chat_tool_call), NULL, NULL, NULL};

static bool edc_llm_actor_parse_resp(cJSON *response, struct edc_llm_actor_resp *resp)
{
    resp->error = NULL;
    cJSON *choices = cJSON_GetObjectItem(response, "choices");
    if (!choices || !cJSON_IsArray(choices) || cJSON_GetArraySize(choices) == 0)
    {
        char *err_msg = cJSON_Print(response);
        log_error("resp format error choice 0 is not exist: %s", err_msg);
        resp->error = edc_error_new(EDC_ERR_INVAL, "resp format choice 0 is not exist: %s", err_msg);
        cJSON_free(err_msg);
        return false;
    }

    cJSON *choice = cJSON_GetArrayItem(choices, 0);
    if (!choice)
    {
        return false;
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
        cJSON_free(s);
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
        cJSON_free(err_msg);
        return false;
    }

    cJSON *content = cJSON_GetObjectItem(message, "content");
    if (content && cJSON_IsString(content))
    {
        resp->content = sdsnew(content->valuestring);
    }

    cJSON *reasoning_content = cJSON_GetObjectItem(message, "reasoning_content");
    if (reasoning_content && cJSON_IsString(reasoning_content))
    {
        resp->reasoning_content = sdsnew(reasoning_content->valuestring);
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
                tc.id = sdsnew(id->valuestring);
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
                    tc.function.name = sdsnew(name->valuestring);
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

    return true;
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
        sdsfree(resp->content);
    }

    if (resp->reasoning_content)
    {
        sdsfree(resp->reasoning_content);
    }

    if (resp->tool_calls)
    {
        for (unsigned i = 0; i < utarray_len(resp->tool_calls); ++i)
        {
            struct edc_llm_chat_tool_call call = *(struct edc_llm_chat_tool_call *)utarray_eltptr(resp->tool_calls, i);
            if (call.id)
            {
                sdsfree(call.id);
            }

            if (call.function.arguments)
            {
                cJSON_Delete(call.function.arguments);
            }

            if (call.function.name)
            {
                sdsfree(call.function.name);
            }
        }

        utarray_free(resp->tool_calls);
    }

    if (resp->msg)
    {
        cJSON_Delete(resp->msg);
    }

    free(resp);
}

edc_actor_t *edc_llm_actor_new(struct edc_config *config)
{
    edc_llm_actor_state_t *state = g_new0(edc_llm_actor_state_t, 1);
    state->api_conf = config->llm_api_conf;
    sds cacert = edc_path_join(config->workspace, "ssl", "cacert.pem", NULL);
    state->client = edc_https_client_new(cacert);
    sdsfree(cacert);

    edc_actor_t *actor = edc_actor_new("edc_llm_actor", edc_llm_actor_handler, state);
    return actor;
}

void edc_llm_actor_free(edc_actor_t *actor)
{
    edc_llm_actor_state_t *state = actor->state;
    edc_https_client_free(state->client);
    free(state);
    edc_actor_free(actor);
}
