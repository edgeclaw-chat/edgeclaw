#include "edc_strutil.h"
#include "edc_channel_qq.h"
#include "edc_actor.h"
#include "edc_channel_actor.h"
#include "edc_network_async.h"
#include "cJSON.h"
#include "log.h"
#include <string.h>

static const char *BC_CHANNEL_QQ_GET_ACCESS_TOKEN_URL = "https://bots.qq.com/app/getAppAccessToken";
static const char *BC_CHANNEL_QQ_GET_WSS_ENDPOINT_URL = "https://api.sgroup.qq.com/gateway/bot";

static void edc_channel_qq_get_access_token_make_http_req(struct mg_connection *c)
{
    edc_actor_t *actor = c->fn_data;
    edc_channel_actor_state_t *state = actor->state;

    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "appId", state->config.qq.app_id);
    cJSON_AddStringToObject(req, "clientSecret", state->config.qq.app_secret);

    const char *body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);

    struct mg_str host = mg_url_host(BC_CHANNEL_QQ_GET_ACCESS_TOKEN_URL);
    mg_printf(c, "POST %s HTTP/1.1\r\n", mg_url_uri(BC_CHANNEL_QQ_GET_ACCESS_TOKEN_URL));
    mg_printf(c, "Host: %.*s\r\n", (int)host.len, host.buf);
    mg_printf(c, "Content-Type: application/json\r\n");
    mg_printf(c, "Content-Length: %d\r\n\r\n", (int)strlen(body));
    mg_printf(c, "%s", body);
    free((char *)body);
}

struct edc_channel_qq_network_req
{
    enum
    {
        BC_CHANNEL_QQ_WSS_CONN_REQ,
        BC_CHANNEL_QQ_HTTP_SEND_MESSAGE_REQ,
    } type;
    union {
        struct
        {
            edc_actor_t *actor;
            int shard;
            int64_t s;
            char *session_id;
            int heartbeat_interval;
            int64_t last_heartbeat_time;
            int64_t last_ack_time;
        } wss;

        struct
        {
            edc_actor_t *actor;
            char *body;
            char *url;
        } http_send_msg;
    };
};

struct edc_channel_qq_network_req *edc_channel_qq_network_wss_req_new(edc_actor_t *actor, int shard, char *session_id,
                                                                    int64_t s)
{
    struct edc_channel_qq_network_req *req = g_new0(struct edc_channel_qq_network_req, 1);
    req->type = BC_CHANNEL_QQ_WSS_CONN_REQ;
    req->wss.actor = actor;
    req->wss.shard = shard;
    req->wss.session_id = session_id;
    req->wss.s = s;
    req->wss.last_heartbeat_time = -1;
    req->wss.heartbeat_interval = 30;
    req->wss.last_ack_time = -1;
    return req;
}

struct edc_channel_qq_network_req *edc_channel_qq_network_send_msg_req_new(edc_actor_t *actor, char *body, char *url)
{

    struct edc_channel_qq_network_req *req = g_new0(struct edc_channel_qq_network_req, 1);
    req->type = BC_CHANNEL_QQ_HTTP_SEND_MESSAGE_REQ;
    req->http_send_msg.actor = actor;
    req->http_send_msg.body = body;
    req->http_send_msg.url = url;
    return req;
}

void edc_channel_qq_network_req_free(struct edc_channel_qq_network_req *req)
{
    if (req->type == BC_CHANNEL_QQ_HTTP_SEND_MESSAGE_REQ)
    {
        if (req->http_send_msg.body)
        {
            free(req->http_send_msg.body);
        }
        if (req->http_send_msg.url)
        {
            free(req->http_send_msg.url);
        }
    }
    else if (req->type == BC_CHANNEL_QQ_WSS_CONN_REQ)
    {
        if (req->wss.session_id)
        {
            free(req->wss.session_id);
        }
    }
    free(req);
}

struct edc_channel_qq_network_resp
{
    enum
    {
        BC_CHANNEL_QQ_GET_ACCESS_TOKEN_RESP,
        BC_CHANNEL_QQ_GET_WSS_ENDPOINT_RESP,
        BC_CHANNEL_QQ_WSS_DISPATCH_RESP,
        BC_CHANNEL_QQ_WSS_RECONN_RESP,
    } type;

    union {
        cJSON *data;
        struct
        {
            char *session_id;
            int64_t s;
            int shard;
        } reconn;
    };
    union {
        struct
        {
            int shard;
        } wss_conn;
    };
};

struct edc_channel_qq_network_resp *edc_channel_qq_get_access_token_network_resp_new(cJSON *data)
{
    struct edc_channel_qq_network_resp *resp = g_new0(struct edc_channel_qq_network_resp, 1);
    resp->data = data;
    resp->type = BC_CHANNEL_QQ_GET_ACCESS_TOKEN_RESP;
    return resp;
}

struct edc_channel_qq_network_resp *edc_channel_qq_get_wss_endpoint_network_resp_new(cJSON *data)
{
    struct edc_channel_qq_network_resp *resp = g_new0(struct edc_channel_qq_network_resp, 1);
    resp->data = data;
    resp->type = BC_CHANNEL_QQ_GET_WSS_ENDPOINT_RESP;
    return resp;
}

struct edc_channel_qq_network_resp *edc_channel_qq_wss_dispatch_network_resp_new(cJSON *data, int shard)
{
    struct edc_channel_qq_network_resp *resp = g_new0(struct edc_channel_qq_network_resp, 1);
    resp->data = data;
    resp->wss_conn.shard = shard;
    resp->type = BC_CHANNEL_QQ_WSS_DISPATCH_RESP;
    return resp;
}

struct edc_channel_qq_network_resp *edc_channel_qq_wss_reconn_resp_new(char *session_id, int64_t s, int shard)
{
    struct edc_channel_qq_network_resp *resp = g_new0(struct edc_channel_qq_network_resp, 1);
    resp->type = BC_CHANNEL_QQ_WSS_RECONN_RESP;
    resp->reconn.s = s;
    resp->reconn.session_id = session_id;
    resp->reconn.shard = shard;
    return resp;
}

void edc_channel_qq_network_resp_free(struct edc_channel_qq_network_resp *resp)
{
    switch (resp->type)
    {
    case BC_CHANNEL_QQ_WSS_RECONN_RESP:
        if (resp->reconn.session_id)
            free(resp->reconn.session_id);
        break;
    default:
        if (resp->data)
            cJSON_Delete(resp->data);
    }

    free(resp);
}

static void edc_channel_qq_on_get_access_http_msg(struct mg_connection *c, struct mg_http_message *hm)
{
    cJSON *resp = cJSON_ParseWithLength(hm->body.buf, hm->body.len);

    if (!resp || !cJSON_IsObject(resp))
    {
        log_error("qq channel: get access_token http resp format error: %.*s", (int)hm->body.len, hm->body.buf);
        return;
    }

    edc_actor_t *actor = c->fn_data;
    edc_actor_send(actor, edc_actor_msg_new_sub_type(BC_ACTOR_MSG_CHANNEL_RECV, edc_channel_qq, actor, NULL,
                                                   edc_channel_qq_get_access_token_network_resp_new(resp),
                                                   (edc_actor_data_free_fn)edc_channel_qq_network_resp_free));
}

static void edc_channel_qq_get_access_token_http_fn(struct mg_connection *c, int ev, void *ev_data)
{
    if (ev == MG_EV_TLS_HS)
    {
        edc_channel_qq_get_access_token_make_http_req(c);
    }
    else if (ev == MG_EV_HTTP_MSG)
    {
        edc_channel_qq_on_get_access_http_msg(c, ev_data);
        c->is_closing = 1;
    }
}

static void edc_channel_qq_start_get_access_token(edc_actor_t *actor)
{
    edc_channel_actor_state_t *state = actor->state;
    edc_http_async_connect(state->network, BC_CHANNEL_QQ_GET_ACCESS_TOKEN_URL, NULL,
                          edc_channel_qq_get_access_token_http_fn, actor, 20);
}

static bool edc_channel_qq_access_token_update(void *data)
{
    edc_actor_t *actor = data;
    edc_channel_actor_state_t *state = actor->state;
    free(state->qq.access_token);
    state->qq.access_token = NULL;
    state->qq.expires_in = -1;

    log_info("qq channel: access_token expired. update now.");
    edc_channel_qq_start_get_access_token(data);
    return G_SOURCE_REMOVE;
}

static bool edc_channel_qq_handle_get_access_token_resp(edc_actor_t *actor, edc_actor_msg_t *msg)
{
    struct edc_channel_qq_network_resp *data = msg->data;
    cJSON *resp = data->data;

    cJSON *access_token = cJSON_GetObjectItem(resp, "access_token");
    cJSON *expires_in = cJSON_GetObjectItem(resp, "expires_in");

    if (!access_token || !cJSON_IsString(access_token) || !expires_in || !cJSON_IsString(expires_in))
    {
        char *s = cJSON_Print(resp);
        log_error("qq channel: get access_token resp format error: %s", s);
        free(s);
        return FALSE;
    }

    edc_channel_actor_state_t *state = actor->state;
    state->qq.access_token = access_token->valuestring;
    state->qq.expires_in = atoi(expires_in->valuestring);
    // state->qq.expires_in = 60;
    access_token->valuestring = NULL;

    if (state->qq.expire_source)
    {
        g_source_unref(state->qq.expire_source);
    }

    state->qq.expire_source = g_timeout_source_new_seconds(state->qq.expires_in + 1);
    g_source_set_callback(state->qq.expire_source, edc_channel_qq_access_token_update, actor, NULL);
    g_source_attach(state->qq.expire_source, actor->context);

    log_info("qq channel: got access_token: %s, expires_in: %d", state->qq.access_token, (int)state->qq.expires_in);
    return TRUE;
}

static void edc_channel_qq_get_wss_endpoint_make_http_req(struct mg_connection *c)
{
    edc_actor_t *actor = c->fn_data;
    edc_channel_actor_state_t *state = actor->state;

    struct mg_str host = mg_url_host(BC_CHANNEL_QQ_GET_WSS_ENDPOINT_URL);
    mg_printf(c, "GET %s HTTP/1.1\r\n", mg_url_uri(BC_CHANNEL_QQ_GET_WSS_ENDPOINT_URL));
    mg_printf(c, "Host: %.*s\r\n", (int)host.len, host.buf);
    mg_printf(c, "Authorization: QQBot %s\r\n", state->qq.access_token);
    mg_printf(c, "X-Union-Appid: %s\r\n\r\n", state->config.qq.app_id);
}

static void edc_channel_qq_on_get_wss_endpoint_http_msg(struct mg_connection *c, struct mg_http_message *hm)
{
    cJSON *resp = cJSON_ParseWithLength(hm->body.buf, hm->body.len);
    if (!resp || !cJSON_IsObject(resp))
    {
        log_error("qq channel: got wss endpoint resp: %.*s", (int)hm->body.len, hm->body.buf);
        return;
    }

    log_debug("qq channel: got wss endpoint %.*s", (int)hm->body.len, hm->body.buf);

    edc_actor_t *actor = c->fn_data;
    edc_actor_send(actor, edc_actor_msg_new_sub_type(BC_ACTOR_MSG_CHANNEL_RECV, edc_channel_qq, actor, NULL,
                                                   edc_channel_qq_get_wss_endpoint_network_resp_new(resp),
                                                   (edc_actor_data_free_fn)edc_channel_qq_network_resp_free));
}

static void edc_channel_qq_get_wss_endpoint_http_fn(struct mg_connection *c, int ev, void *ev_data)
{
    if (ev == MG_EV_TLS_HS)
    {
        edc_channel_qq_get_wss_endpoint_make_http_req(c);
    }
    else if (ev == MG_EV_HTTP_MSG)
    {
        edc_channel_qq_on_get_wss_endpoint_http_msg(c, ev_data);
        c->is_closing = 1;
    }
}

static void edc_channel_qq_start_get_wss_endpoint(edc_actor_t *actor)
{
    edc_channel_actor_state_t *state = actor->state;
    edc_http_async_connect(state->network, BC_CHANNEL_QQ_GET_WSS_ENDPOINT_URL, NULL,
                          edc_channel_qq_get_wss_endpoint_http_fn, actor, 20);
}

static bool edc_channel_qq_handle_get_wss_endpoint_resp(edc_actor_t *actor, edc_actor_msg_t *msg)
{
    struct edc_channel_qq_network_resp *data = msg->data;
    edc_channel_actor_state_t *state = actor->state;

    cJSON *resp = data->data;

    cJSON *url = cJSON_GetObjectItem(resp, "url");
    cJSON *shards = cJSON_GetObjectItem(resp, "shards");

    if (!url || !shards || !cJSON_IsString(url) || !cJSON_IsNumber(shards))
    {
        char *s = cJSON_Print(resp);
        log_error("qq channel: get wss endpoint resp format error: %s", s);
        free(s);
        return FALSE;
    }

    if (state->qq.wss_url)
    {
        free(state->qq.wss_url);
    }
    state->qq.wss_url = url->valuestring;
    url->valuestring = NULL;

    state->qq.wss_shards = (int)cJSON_GetNumberValue(shards);
    log_info("qq channel: got wss endpoint url: %s, shards: %d", state->qq.wss_url, state->qq.wss_shards);
    return TRUE;
}

static bool edc_channel_qq_handle_wss_hello_message(struct edc_channel_qq_network_req *req, cJSON *data, int shard)
{
    cJSON *d = cJSON_GetObjectItem(data, "d");
    cJSON *heartbeat_interval = cJSON_GetObjectItem(d, "heartbeat_interval");
    if (!d || !cJSON_IsObject(d) || !heartbeat_interval || !cJSON_IsNumber(heartbeat_interval))
    {
        char *s = cJSON_Print(data);
        log_error("qq channel: wss hello message format error: %s", s);
        free(s);
        return FALSE;
    }

    req->wss.heartbeat_interval = (int)cJSON_GetNumberValue(heartbeat_interval) / 1000;
    req->wss.last_heartbeat_time = mg_millis();
    req->wss.last_ack_time = mg_millis();
    return TRUE;
}

#define BC_CHANNEL_QQ_INTENTS_ALL                                                                                      \
    ((1 << 0) | (1 << 1) | (1 << 9) | (1 << 10) | (1 << 12) | (1 << 18) | (1 << 19) | (1 << 25) | (1 << 26) |          \
     (1 << 27) | (1 << 28) | (1 << 29) | (1 << 30))

static void edc_channel_qq_wss_send_identify(struct mg_connection *c, edc_actor_t *actor, int shard)
{
    cJSON *identify = cJSON_CreateObject();
    cJSON_AddNumberToObject(identify, "op", 2);
    cJSON *d = cJSON_AddObjectToObject(identify, "d");

    edc_channel_actor_state_t *state = actor->state;
    char *token = edc_strdup_printf("QQBot %s", state->qq.access_token);
    cJSON_AddStringToObject(d, "token", token);
    free(token);
    cJSON_AddNumberToObject(d, "intents", BC_CHANNEL_QQ_INTENTS_ALL);

    cJSON *shards = cJSON_AddArrayToObject(d, "shard");
    cJSON_AddItemToArray(shards, cJSON_CreateNumber(shard));
    cJSON_AddItemToArray(shards, cJSON_CreateNumber(state->qq.wss_shards));

    char *payload = cJSON_PrintUnformatted(identify);
    cJSON_Delete(identify);
    mg_ws_send(c, payload, strlen(payload), WEBSOCKET_OP_BINARY);
    free(payload);
}

static void edc_channel_qq_wss_send_resume(struct mg_connection *c, edc_actor_t *actor, char *session_id, int64_t s)
{
    cJSON *resume = cJSON_CreateObject();
    cJSON_AddNumberToObject(resume, "op", 6);
    cJSON *d = cJSON_AddObjectToObject(resume, "d");

    edc_channel_actor_state_t *state = actor->state;
    char *token = edc_strdup_printf("QQBot %s", state->qq.access_token);
    cJSON_AddStringToObject(d, "token", token);
    free(token);

    cJSON_AddStringToObject(d, "session_id", session_id);
    cJSON_AddNumberToObject(d, "seq", s);
    char *payload = cJSON_PrintUnformatted(resume);
    cJSON_Delete(resume);

    mg_ws_send(c, payload, strlen(payload), WEBSOCKET_OP_BINARY);
    free(payload);
}

static void edc_channel_qq_handle_wss_dispatch_msg(struct mg_connection *c, struct edc_channel_qq_network_req *req,
                                                  cJSON *data)
{
    cJSON *s = cJSON_GetObjectItem(data, "s");
    char *tmp = NULL;

    if (!s || !cJSON_IsNumber(s))
    {
        goto format_error_exit;
    }

    req->wss.s = (int64_t)cJSON_GetNumberValue(s);

    char READY[] = "READY";
    cJSON *t = cJSON_GetObjectItem(data, "t");
    if (!t || !cJSON_IsString(t))
    {
        goto format_error_exit;
    }

    if (strncmp(cJSON_GetStringValue(t), READY, sizeof(READY) - 1) == 0)
    {
        cJSON *sess_id = cJSON_GetObjectItem(cJSON_GetObjectItem(data, "d"), "session_id");
        if (!sess_id || !cJSON_IsString(sess_id))
        {
            goto format_error_exit;
        }

        req->wss.session_id = strdup(cJSON_GetStringValue(sess_id));
        cJSON_Delete(data);
        log_info("qq channel wss conn READY, session_id = %s", req->wss.session_id);

        return;
    }

    edc_actor_send(req->wss.actor,
                  edc_actor_msg_new_sub_type(BC_ACTOR_MSG_CHANNEL_RECV, edc_channel_qq, req->wss.actor, NULL,
                                            edc_channel_qq_wss_dispatch_network_resp_new(data, req->wss.shard),
                                            (edc_actor_data_free_fn)edc_channel_qq_network_resp_free));

    return;
format_error_exit:
    tmp = cJSON_Print(data);
    log_info("qq channel: dispatch message format error: %s", tmp);
    free(tmp);
}

static void edc_channel_qq_handle_wss_reconnect(struct mg_connection *c, struct edc_channel_qq_network_req *req)
{
    edc_channel_actor_state_t *state = req->wss.actor->state;
    edc_network_async_reset_dns(&state->network->mgr);

    struct edc_channel_qq_network_resp *resp =
        edc_channel_qq_wss_reconn_resp_new(req->wss.session_id, req->wss.s, req->wss.shard);

    log_info("qq channel: conn session_id = %s, shard = %d close. and reconnect");
    req->wss.session_id = NULL;
    c->is_closing = 1;

    state->qq.conns = g_slist_remove(state->qq.conns, c);

    edc_actor_send(req->wss.actor,
                  edc_actor_msg_new_sub_type(BC_ACTOR_MSG_CHANNEL_RECV, edc_channel_qq, req->wss.actor, NULL, resp,
                                            (edc_actor_data_free_fn)edc_channel_qq_network_resp_free));
}

static void edc_channel_qq_on_wss_conn_msg(struct mg_connection *c, struct mg_ws_message *wm)
{
    struct edc_channel_qq_network_req *req = c->fn_data;
    cJSON *data = cJSON_ParseWithLength(wm->data.buf, wm->data.len);

    log_debug("qq channel: recv wss conn message: %.*s", (int)wm->data.len, wm->data.buf);
    char *tmp = NULL;

    if (!data || !cJSON_IsObject(data))
    {
        goto format_error_exit;
    }

    cJSON *op = cJSON_GetObjectItem(data, "op");

    if (!op || !cJSON_IsNumber(op))
    {
        goto format_error_exit;
    }

    int op_code = (int)cJSON_GetNumberValue(op);

#define BC_CHANNEL_QQ_OP_HELLO 10
#define BC_CHANNEL_QQ_OP_DISPATCH 0
#define BC_CHANNEL_QQ_OP_HEARTBEAT 1
#define BC_CHANNEL_QQ_OP_ACK 11
#define BC_CHANNEL_QQ_OP_RECONNECT 7

    switch (op_code)
    {
    case BC_CHANNEL_QQ_OP_HELLO:
        if (edc_channel_qq_handle_wss_hello_message(req, data, req->wss.shard))
        {
            if (req->wss.session_id)
            {
                edc_channel_qq_wss_send_resume(c, req->wss.actor, req->wss.session_id, req->wss.s);
            }
            else
            {
                edc_channel_qq_wss_send_identify(c, req->wss.actor, req->wss.shard);
            }
            cJSON_Delete(data);
        }
        break;
    case BC_CHANNEL_QQ_OP_HEARTBEAT:
        cJSON_Delete(data);
        break;
    case BC_CHANNEL_QQ_OP_DISPATCH:
        edc_channel_qq_handle_wss_dispatch_msg(c, req, data);
        break;
    case BC_CHANNEL_QQ_OP_RECONNECT:
        edc_channel_qq_handle_wss_reconnect(c, req);
        cJSON_Delete(data);
        break;
    case BC_CHANNEL_QQ_OP_ACK:
        log_info("recv server heartbeat ack");
        req->wss.last_ack_time = mg_millis();
        cJSON_Delete(data);
        break;
    default:
        cJSON_Delete(data);
        break;
    }

    return;

format_error_exit:
    tmp = cJSON_Print(data);
    log_error("qq channel: wss message format error: %s", tmp);
    free(tmp);
    cJSON_Delete(data);
}

static void edc_channel_qq_wss_send_heartbeat(struct mg_connection *c, struct edc_channel_qq_network_req *req)
{
    cJSON *hb = cJSON_CreateObject();
    cJSON_AddNumberToObject(hb, "op", BC_CHANNEL_QQ_OP_HEARTBEAT);
    cJSON_AddNumberToObject(hb, "d", req->wss.s);

    char *payload = cJSON_PrintUnformatted(hb);

    mg_ws_send(c, payload, strlen(payload), WEBSOCKET_OP_BINARY);
    cJSON_Delete(hb);
    free(payload);
}

#undef BC_CHANNEL_QQ_OP_HELLO
#undef BC_CHANNEL_QQ_OP_DISPATCH
#undef BC_CHANNEL_QQ_OP_HEARTBEAT

static void edc_channel_qq_wss_fn(struct mg_connection *c, int ev, void *ev_data)
{
    if (ev == MG_EV_WS_OPEN)
    {
        struct edc_channel_qq_network_req *req = c->fn_data;
        edc_channel_actor_state_t *state = req->wss.actor->state;
        state->qq.conns = g_slist_append(state->qq.conns, c);
    }
    else if (ev == MG_EV_POLL)
    {
        struct edc_channel_qq_network_req *req = c->fn_data;
        int secs = (mg_millis() - req->wss.last_heartbeat_time) / 1000;
        if (req->wss.heartbeat_interval > 0 && secs >= req->wss.heartbeat_interval)
        {
            edc_channel_qq_wss_send_heartbeat(c, req);
            req->wss.last_heartbeat_time = mg_millis();
            log_info("qq channel: send heart beat to server.");
        }

        int ack_secs = (mg_millis() - req->wss.last_ack_time) / 1000;
        if (req->wss.last_ack_time > 0 && ack_secs >= req->wss.heartbeat_interval * 3)
        {
            log_info("qq channel: server ack timeout, heartbeat_interval = %d. reconn now.",
                     req->wss.heartbeat_interval);
            edc_channel_qq_handle_wss_reconnect(c, req);
        }
    }
    else if (ev == MG_EV_WS_MSG)
    {
        edc_channel_qq_on_wss_conn_msg(c, ev_data);
    }
    else if (ev == BC_CONN_TIME_OUT || ev == MG_EV_ERROR)
    {
        edc_channel_qq_network_req_free(c->fn_data);
    }
    else if (ev == BC_CONN_ERROR)
    {
        edc_channel_qq_network_req_free(ev_data);
    }
    else if (ev == MG_EV_CLOSE)
    {
        edc_channel_qq_network_req_free(c->fn_data);
    }
}

static void edc_channel_qq_start_wss_conn_shard(edc_actor_t *actor, char *session_id, int64_t s, int shard)
{
    edc_channel_actor_state_t *state = actor->state;
    edc_wss_async_connect(state->network, state->qq.wss_url, edc_channel_qq_wss_fn,
                         edc_channel_qq_network_wss_req_new(actor, shard, session_id, s), -1);
}

static void edc_channel_qq_start_wss_conn(edc_actor_t *actor, char *session_id, int64_t s)
{
    edc_channel_actor_state_t *state = actor->state;
    GSList *p = state->qq.conns;
    while (p)
    {
        struct mg_connection *c = p->data;
        edc_network_async_close_conn(state->network, c);
        p = p->next;
    }

    if (state->qq.conns)
    {
        g_slist_free(state->qq.conns);
        state->qq.conns = NULL;
    }

    for (int i = 0; i < state->qq.wss_shards; ++i)
    {
        edc_channel_qq_start_wss_conn_shard(actor, session_id, s, i);
    }
}

static void edc_channel_qq_handle_wss_dispatch(edc_actor_t *actor, struct edc_channel_qq_network_resp *resp)
{
    char *tmp = NULL;
    cJSON *data = resp->data;
    cJSON *t = cJSON_GetObjectItem(data, "t");

    if (!t || !cJSON_IsString(t))
    {
        goto format_error_exit;
    }

    static const char C2C_MESSAGE_CREATE[] = "C2C_MESSAGE_CREATE";
    if (strncmp(cJSON_GetStringValue(t), C2C_MESSAGE_CREATE, sizeof(C2C_MESSAGE_CREATE) - 1))
    {
        log_info("qq channel: only C2C_MESSAGE_CREATE is supportted now.");
        return;
    }

    cJSON *d = cJSON_GetObjectItem(data, "d");
    if (!d || !cJSON_IsObject(d))
    {
        goto format_error_exit;
    }

    cJSON *id = cJSON_GetObjectItem(d, "id");
    cJSON *content = cJSON_GetObjectItem(d, "content");
    cJSON *author = cJSON_GetObjectItem(d, "author");

    if (!id || !content || !author || !cJSON_IsString(id) || !cJSON_IsString(content) || !cJSON_IsObject(author))
    {
        goto format_error_exit;
    }

    cJSON *author_id = cJSON_GetObjectItem(author, "user_openid");
    if (!author_id || !cJSON_IsString(author_id))
    {
        goto format_error_exit;
    }

    struct edc_channel_message *message =
        edc_channel_qq_message_new(id->valuestring, author_id->valuestring, content->valuestring);

    id->valuestring = NULL;
    author_id->valuestring = NULL;
    content->valuestring = NULL;

    edc_channel_actor_state_t *state = actor->state;
    edc_actor_msg_t *msg = edc_actor_msg_new(BC_ACTOR_MSG_CHANNEL_RECV, actor, NULL, message,
                                           (edc_actor_data_free_fn)edc_channel_message_free);
    edc_actor_send(state->observer, msg);

    return;

format_error_exit:
    tmp = cJSON_Print(data);
    log_error("qq channel: dispatch format error: %s", tmp);
    free(tmp);
}

static void edc_channel_qq_handle_recv(edc_actor_t *actor, edc_actor_msg_t *msg)
{
    struct edc_channel_qq_network_resp *resp = msg->data;
    if (resp->type == BC_CHANNEL_QQ_GET_ACCESS_TOKEN_RESP)
    {
        if (!edc_channel_qq_handle_get_access_token_resp(actor, msg))
        {
            return;
        }

        edc_channel_qq_start_get_wss_endpoint(actor);
    }
    else if (resp->type == BC_CHANNEL_QQ_GET_WSS_ENDPOINT_RESP)
    {
        if (!edc_channel_qq_handle_get_wss_endpoint_resp(actor, msg))
        {
            return;
        }

        edc_channel_qq_start_wss_conn(actor, NULL, 0);
    }
    else if (resp->type == BC_CHANNEL_QQ_WSS_DISPATCH_RESP)
    {
        edc_channel_qq_handle_wss_dispatch(actor, resp);
    }
    else if (resp->type == BC_CHANNEL_QQ_WSS_RECONN_RESP)
    {
        log_info("qq channel wss reconn, session_id = %s, s = %lld", resp->reconn.session_id, resp->reconn.s);
        edc_channel_qq_start_wss_conn_shard(actor, resp->reconn.session_id, resp->reconn.s, resp->reconn.shard);
        resp->reconn.session_id = NULL;
    }
}

static void edc_channel_qq_send_message_http_fn(struct mg_connection *c, int ev, void *ev_data)
{
    if (ev == MG_EV_TLS_HS)
    {
        struct edc_channel_qq_network_req *req = c->fn_data;
        edc_channel_actor_state_t *state = req->http_send_msg.actor->state;

        struct mg_str host = mg_url_host(req->http_send_msg.url);
        mg_printf(c, "POST %s HTTP/1.1\r\n", mg_url_uri(req->http_send_msg.url));
        mg_printf(c, "Host: %.*s\r\n", (int)host.len, host.buf);
        mg_printf(c, "Authorization: QQBot %s\r\n", state->qq.access_token);
        mg_printf(c, "X-Union-Appid: %s\r\n", state->config.qq.app_id);
        mg_printf(c, "Content-Type: application/json\r\n");
        mg_printf(c, "Content-Length: %d\r\n\r\n", (int)strlen(req->http_send_msg.body));
        mg_printf(c, "%s", req->http_send_msg.body);
    }
    else if (ev == MG_EV_HTTP_MSG)
    {
        struct mg_http_message *hm = ev_data;
        log_info("qq channel: http send message: %.*s", (int)hm->body.len, hm->body.buf);
        c->is_closing = 1;
        edc_channel_qq_network_req_free(c->fn_data);
    }
    else if (ev == BC_CONN_TIME_OUT || ev == MG_EV_ERROR)
    {
        edc_channel_qq_network_req_free(c->fn_data);
    }
    else if (ev == BC_CONN_ERROR)
    {
        edc_channel_qq_network_req_free(ev_data);
    }
}

static void edc_channel_qq_handle_send(edc_actor_t *actor, edc_actor_msg_t *msg)
{
    struct edc_channel_message *message = msg->data;
    edc_channel_actor_state_t *state = actor->state;

    char *send_c2c_url =
        edc_strdup_printf("https://api.sgroup.qq.com/v2/users/%s/messages", message->channel->qq.author_id);

    cJSON *req = cJSON_CreateObject();
    cJSON_AddNumberToObject(req, "msg_type", 2);
    cJSON_AddStringToObject(req, "msg_id", message->channel->qq.message_id);
    cJSON_AddNumberToObject(req, "msg_seq", message->channel->qq.seq_id);

    cJSON *md = cJSON_AddObjectToObject(req, "markdown");
    cJSON_AddStringToObject(md, "content", message->content);

    char *body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);

    edc_http_async_connect(state->network, send_c2c_url, NULL, edc_channel_qq_send_message_http_fn,
                          edc_channel_qq_network_send_msg_req_new(actor, body, send_c2c_url), 20);
}

static void edc_channel_qq_handler(edc_actor_t *actor, edc_actor_msg_t *msg)
{
    switch (msg->type)
    {
    case BC_ACTOR_MSG_START:
        edc_channel_qq_start_get_access_token(actor);
        break;
    case BC_ACTOR_MSG_CHANNEL_RECV:
        edc_channel_qq_handle_recv(actor, msg);
        break;
    case BC_ACTOR_MSG_CHANNEL_SEND:
        edc_channel_qq_handle_send(actor, msg);
    default:
        break;
    }
}

void edc_channel_qq_init(edc_actor_t *channel_actor)
{
    edc_channel_actor_state_t *state = channel_actor->state;
    state->qq.access_token = NULL;
    state->qq.expire_source = NULL;
    state->qq.expires_in = -1;
    state->qq.wss_shards = -1;
    state->qq.wss_url = NULL;
    state->qq.conns = NULL;
    edc_channel_actor_add(channel_actor, edc_channel_qq, edc_channel_qq_handler);
}
