#include "edc_channel_feishu.h"
#include "edc_actor.h"
#include "edc_channel_actor.h"
#include "edc_network_async.h"
#include "cJSON.h"
#include "lark_api.pb.h"
#include "log.h"
#include "pb_decode.h"
#include "pb_encode.h"
#include "string.h"
#include <inttypes.h>
#include <string.h>

#define FRAME_METHOD_CONTROL 0
#define FRAME_METHOD_DATA 1
static const char *GEN_WSS_ENDPOINT_URL = "https://open.feishu.cn/callback/ws/endpoint";
static const char *GET_TENANT_ACCESS_TOKEN_URL =
    "https://open.feishu.cn/open-apis/auth/v3/tenant_access_token/internal";
static const char *FEISHU_SEND_MESSAGE_URL = "https://open.feishu.cn/open-apis/im/v1/messages?receive_id_type=chat_id";

static void edc_channel_feishu_make_tenant_token_req(struct mg_connection *c)
{
    edc_actor_t *actor = c->fn_data;
    edc_channel_actor_state_t *state = actor->state;
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "app_id", state->config.feishu.app_id);
    cJSON_AddStringToObject(req, "app_secret", state->config.feishu.app_secret);
    char *body = cJSON_PrintUnformatted(req);

    struct mg_str host = mg_url_host(GET_TENANT_ACCESS_TOKEN_URL);
    mg_printf(c, "POST %s HTTP/1.1\r\n", mg_url_uri(GET_TENANT_ACCESS_TOKEN_URL));
    mg_printf(c, "Host: %.*s\r\n", (int)host.len, host.buf);
    mg_printf(c, "Content-Type: application/json\r\n");
    mg_printf(c, "Content-Length: %d\r\n\r\n", (int)strlen(body));
    mg_printf(c, "%s", body);

    cJSON_Delete(req);
    free(body);
}

struct edc_channel_feishu_resp
{
    enum
    {
        BC_CHANNEL_FEISHU_TENANT_TOKEN_RESP,
        BC_CHANNEL_FEISHU_WSS_ENDPOINT_RESP,
        BC_CHANNEL_FEISHU_WSS_EVENT_RESP,
        BC_CHANNEL_FEISHU_SEND_MSG_RESP,
    } type;

    union {
        cJSON *tenant_token_resp;
        cJSON *wss_endpoint_resp;
        cJSON *wss_event_resp;
        cJSON *send_msg_resp;
    };
};

struct edc_channel_feishu_wss_conn
{
    edc_actor_t *actor;
    uint64_t last_ping_time;
};

static struct edc_channel_feishu_wss_conn *edc_channel_feishu_wss_conn_new(edc_actor_t *actor)
{
    struct edc_channel_feishu_wss_conn *conn = g_new0(struct edc_channel_feishu_wss_conn, 1);
    conn->actor = actor;
    conn->last_ping_time = 0;
    return conn;
}

static void edc_channel_feishu_wss_conn_free(struct edc_channel_feishu_wss_conn *conn)
{
    free(conn);
}

static struct edc_channel_feishu_resp *edc_channel_feishu_tenant_token_resp_new(cJSON *resp)
{
    struct edc_channel_feishu_resp *feishu_resp = g_new0(struct edc_channel_feishu_resp, 1);
    feishu_resp->tenant_token_resp = resp;
    feishu_resp->type = BC_CHANNEL_FEISHU_TENANT_TOKEN_RESP;
    return feishu_resp;
}

static struct edc_channel_feishu_resp *edc_channel_feishu_wss_endpoint_resp_new(cJSON *data)
{
    struct edc_channel_feishu_resp *feishu_resp = g_new0(struct edc_channel_feishu_resp, 1);
    feishu_resp->wss_endpoint_resp = data;
    feishu_resp->type = BC_CHANNEL_FEISHU_WSS_ENDPOINT_RESP;
    return feishu_resp;
}

static struct edc_channel_feishu_resp *edc_channel_feishu_wss_event_resp_new(cJSON *data)
{
    struct edc_channel_feishu_resp *feishu_resp = g_new0(struct edc_channel_feishu_resp, 1);
    feishu_resp->wss_event_resp = data;
    feishu_resp->type = BC_CHANNEL_FEISHU_WSS_EVENT_RESP;
    return feishu_resp;
}

static struct edc_channel_feishu_resp *edc_channel_feishu_send_message_resp_new(cJSON *data)
{
    struct edc_channel_feishu_resp *feishu_resp = g_new0(struct edc_channel_feishu_resp, 1);
    feishu_resp->send_msg_resp = data;
    feishu_resp->type = BC_CHANNEL_FEISHU_SEND_MSG_RESP;
    return feishu_resp;
}

static void edc_channel_feishu_resp_free(struct edc_channel_feishu_resp *resp)
{
    switch (resp->type)
    {
    case BC_CHANNEL_FEISHU_TENANT_TOKEN_RESP:
        cJSON_Delete(resp->tenant_token_resp);
        break;
    case BC_CHANNEL_FEISHU_WSS_ENDPOINT_RESP:
        cJSON_Delete(resp->wss_endpoint_resp);
        break;
    case BC_CHANNEL_FEISHU_SEND_MSG_RESP:
        cJSON_Delete(resp->send_msg_resp);
        break;
    case BC_CHANNEL_FEISHU_WSS_EVENT_RESP:
        cJSON_Delete(resp->wss_event_resp);
        break;
    }

    free(resp);
}

static void edc_channel_feishu_handle_tenant_token_resp(struct mg_connection *c, struct mg_http_message *hm)
{
    edc_actor_t *actor = c->fn_data;
    cJSON *resp = cJSON_ParseWithLength(hm->body.buf, hm->body.len);
    edc_actor_send(actor, edc_actor_msg_new_sub_type(BC_ACTOR_MSG_CHANNEL_RECV, edc_channel_feishu, actor, NULL,
                                                   edc_channel_feishu_tenant_token_resp_new(resp),
                                                   (edc_actor_data_free_fn)edc_channel_feishu_resp_free));
}

static void edc_channel_feishu_tenant_token_fn(struct mg_connection *c, int ev, void *ev_data)
{
    if (ev == MG_EV_TLS_HS)
    {
        edc_channel_feishu_make_tenant_token_req(c);
    }
    else if (ev == MG_EV_HTTP_MSG)
    {
        edc_channel_feishu_handle_tenant_token_resp(c, ev_data);
        c->is_closing = 1;
    }
    else if (ev == BC_CONN_TIME_OUT || ev == MG_EV_ERROR)
    {
    }
    else if (ev == BC_CONN_ERROR)
    {
    }
}

static void edc_channel_feishu_make_tenant_wss_url_req(struct mg_connection *c, int ev)
{
    edc_actor_t *actor = c->fn_data;
    edc_channel_actor_state_t *state = actor->state;

    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "AppID", state->config.feishu.app_id);
    cJSON_AddStringToObject(req, "AppSecret", state->config.feishu.app_secret);
    char *body = cJSON_PrintUnformatted(req);

    struct mg_str host = mg_url_host(GEN_WSS_ENDPOINT_URL);
    mg_printf(c, "POST %s HTTP/1.1\r\n", mg_url_uri(GEN_WSS_ENDPOINT_URL));
    mg_printf(c, "Host: %.*s\r\n", (int)host.len, host.buf);
    mg_printf(c, "Content-Type: application/json\r\n");
    mg_printf(c, "Content-Length: %d\r\n\r\n", (int)strlen(body));
    mg_printf(c, "%s", body);

    cJSON_Delete(req);
    free(body);
}

static void edc_channel_feishu_handle_wss_endpoint_resp(struct mg_connection *c, struct mg_http_message *hm)
{
    edc_actor_t *actor = c->fn_data;

    cJSON *data = cJSON_ParseWithLength(hm->body.buf, hm->body.len);
    edc_actor_send(actor, edc_actor_msg_new_sub_type(BC_ACTOR_MSG_CHANNEL_RECV, edc_channel_feishu, actor, NULL,
                                                   edc_channel_feishu_wss_endpoint_resp_new(data),
                                                   (edc_actor_data_free_fn)edc_channel_feishu_resp_free));
}

static void edc_channel_feishu_wss_url_fn(struct mg_connection *c, int ev, void *ev_data)
{
    if (ev == MG_EV_TLS_HS)
    {
        edc_channel_feishu_make_tenant_wss_url_req(c, ev);
    }
    else if (ev == MG_EV_HTTP_MSG)
    {
        edc_channel_feishu_handle_wss_endpoint_resp(c, ev_data);
        c->is_closing = 1;
    }
    else if (ev == BC_CONN_TIME_OUT || ev == MG_EV_ERROR)
    {
    }
    else if (ev == BC_CONN_ERROR)
    {
    }
}

static void edc_channel_feishu_http_connect(edc_actor_t *actor, const char *url, mg_event_handler_t fn, void *fn_data)
{
    edc_channel_actor_state_t *state = actor->state;
    edc_http_async_connect(state->network, url, NULL, fn, fn_data, 60);
}

static void edc_channel_feishu_tenant_token_req(edc_actor_t *actor)
{
    edc_channel_feishu_http_connect(actor, GET_TENANT_ACCESS_TOKEN_URL, edc_channel_feishu_tenant_token_fn, actor);
}

static void edc_channel_feishu_wss_endpoint_req(edc_actor_t *actor)
{
    edc_channel_feishu_http_connect(actor, GEN_WSS_ENDPOINT_URL, edc_channel_feishu_wss_url_fn, actor);
}

static bool edc_channel_feishu_tenant_token_update(void *data)
{
    edc_actor_t *actor = data;
    edc_channel_actor_state_t *state = actor->state;
    log_info("edc_channel_feishu tenant_token expired. update now.");

    state->feishu.expire = -1;
        sdsfree(state->feishu.tenant_access_token);
        state->feishu.tenant_access_token = NULL;

    edc_channel_feishu_tenant_token_req(actor);
    return G_SOURCE_REMOVE;
}

static void edc_channel_feishu_handle_tenant_token_recv(edc_actor_t *actor, struct edc_channel_feishu_resp *resp)
{
    edc_channel_actor_state_t *state = actor->state;
    cJSON *data = resp->tenant_token_resp;

    if (!data)
    {
        log_error("feishu channel: get tenant_token failed. resp is NULL");
        return;
    }

    cJSON *code_item = cJSON_GetObjectItem(data, "code");
    if (!code_item || !cJSON_IsNumber(code_item))
    {
        char *tmp = cJSON_PrintUnformatted(data);
        log_error("feishu channel: get tenant_token failed. resp format error: %s", tmp);
        free(tmp);
        return;
    }

    int code = (int)cJSON_GetNumberValue(code_item);
    cJSON *msg = cJSON_GetObjectItem(data, "msg");
    if (code)
    {
        log_error("feishu channel: get tenant_token failed: %d %s", code, msg ? cJSON_GetStringValue(msg) : "");
        return;
    }

    cJSON *token = cJSON_GetObjectItem(data, "tenant_access_token");
    if (!token || !cJSON_IsString(token))
    {
        char *tmp = cJSON_PrintUnformatted(data);
        log_error("feishu channel: get tenant_token failed. resp format error: %s", tmp);
        free(tmp);
        return;
    }

    cJSON *expire = cJSON_GetObjectItem(data, "expire");
    if (!expire || !cJSON_IsNumber(expire))
    {
        char *tmp = cJSON_PrintUnformatted(data);
        log_error("feishu channel: get tenant_token failed. resp format error: %s", tmp);
        free(tmp);
        return;
    }

    state->feishu.expire = (int64_t)cJSON_GetNumberValue(expire);

    state->feishu.tenant_access_token = sdsnew(cJSON_GetStringValue(token));
    log_debug("get feishu tenant_access_token: %s, expire at: %" PRId64, state->feishu.tenant_access_token,
              state->feishu.expire);

    if (state->feishu.expire_source)
    {
        g_source_unref(state->feishu.expire_source);
    }
    state->feishu.expire_source = g_timeout_source_new_seconds(state->feishu.expire + 1);
    g_source_set_callback(state->feishu.expire_source, (GSourceFunc)edc_channel_feishu_tenant_token_update, actor, NULL);
    g_source_attach(state->feishu.expire_source, actor->context);
}

static bool edc_channel_feishu_parse_wss_endpoint_resp(edc_channel_actor_state_t *state, cJSON *body)
{
    cJSON *data = NULL;
    cJSON *code = cJSON_GetObjectItem(body, "code");
    if (!code || !cJSON_IsNumber(code))
    {
        goto format_err_exit;
    }

    int code_num = (int)cJSON_GetNumberValue(code);
    if (code_num)
    {
        cJSON *msg = cJSON_GetObjectItem(body, "msg");
        log_error("channel feishu: wss endpoint resp error: %d, %s", code_num, cJSON_GetStringValue(msg));
        return FALSE;
    }

    data = cJSON_GetObjectItem(body, "data");
    if (!data || !cJSON_IsObject(data))
    {
        goto format_err_exit;
    }

    cJSON *URL = cJSON_GetObjectItem(data, "URL");
    if (!URL || !cJSON_IsString(URL))
    {
        goto format_err_exit;
    }

    cJSON *client_config = cJSON_GetObjectItem(data, "ClientConfig");
    if (!client_config || !cJSON_IsObject(client_config))
    {
        goto format_err_exit;
    }

    cJSON *ping_interval = cJSON_GetObjectItem(client_config, "PingInterval");
    cJSON *reconnect_count = cJSON_GetObjectItem(client_config, "ReconnectCount");
    cJSON *reconnect_interval = cJSON_GetObjectItem(client_config, "ReconnectInterval");
    cJSON *reconnect_nonce = cJSON_GetObjectItem(client_config, "ReconnectNonce");

    if (!ping_interval || !cJSON_IsNumber(ping_interval) || !reconnect_count || !cJSON_IsNumber(reconnect_count) ||
        !reconnect_interval || !cJSON_IsNumber(reconnect_interval) || !reconnect_nonce ||
        !cJSON_IsNumber(reconnect_nonce))
    {
        goto format_err_exit;
    }

    if (state->feishu.wss_url)
    {
        sdsfree(state->feishu.wss_url);
    }
    state->feishu.wss_url = sdsnew(cJSON_GetStringValue(URL));
    state->feishu.wss_client_config.ping_interval = (int)cJSON_GetNumberValue(ping_interval);
    state->feishu.wss_client_config.reconnect_count = (int)cJSON_GetNumberValue(reconnect_count);
    state->feishu.wss_client_config.reconnect_interval = (int)cJSON_GetNumberValue(reconnect_interval);
    state->feishu.wss_client_config.reconnect_nonce = (int)cJSON_GetNumberValue(reconnect_nonce);

    const char *uri = mg_url_uri(state->feishu.wss_url);

    char service_id[128];
    struct mg_str path = mg_str(uri);
    mg_http_get_var(&path, "service_id", service_id, sizeof(service_id));

    state->feishu.wss_service_id = (int32_t)atoi(service_id);

    log_info("feishu channel: got wss endpoitn, url = %s, service id = %d, ping_interval: %d, reconnect_count: %d, "
             "reconnect_interval: %d, reconnect_nonce: %d",
             state->feishu.wss_url, (int)state->feishu.wss_service_id, state->feishu.wss_client_config.ping_interval,
             state->feishu.wss_client_config.reconnect_count, state->feishu.wss_client_config.reconnect_interval,
             state->feishu.wss_client_config.reconnect_nonce);

    return TRUE;

format_err_exit: {
    char *tmp = data ? cJSON_PrintUnformatted(data) : NULL;
    log_error("channel feishu: wss endpoint recv error json format: %s", tmp ? tmp : "(null)");
    free(tmp);
}
    return FALSE;
}

static size_t edc_channel_feishu_build_ping_frame(int32_t service_id, uint8_t *buf, size_t len)
{
    pbbp2_Frame frame = pbbp2_Frame_init_zero;
    frame.SeqID = 0;
    frame.LogID = 0;
    frame.service = service_id;
    frame.method = 0;
    frame.headers_count = 1;

    strncpy(frame.headers[0].key, "type", sizeof(frame.headers[0].key));
    strncpy(frame.headers[0].value, "ping", sizeof(frame.headers[0].value));

    pb_ostream_t os = pb_ostream_from_buffer(buf, len);
    if (!pb_encode(&os, pbbp2_Frame_fields, &frame))
    {
        log_error("feishu channel wss build ping frame error");
        return 0;
    }

    return os.bytes_written;
}

typedef struct
{
    uint8_t *data;
    size_t len;
} payload_buf_t;

static bool encode_payload_cb(pb_ostream_t *stream, const pb_field_t *field, void *const *arg)
{
    payload_buf_t *ctx = (payload_buf_t *)*arg;
    if (!pb_encode_tag_for_field(stream, field))
        return false;
    if (!pb_encode_varint(stream, ctx->len))
        return false;
    if (!pb_write(stream, ctx->data, ctx->len))
        return false;
    return true;
}

static size_t edc_channel_feishu_build_ack(pbbp2_Frame *in_frame, int32_t service_id, uint8_t *buf, size_t len)
{
    pbbp2_Frame frame = pbbp2_Frame_init_zero;
    frame.SeqID = in_frame->SeqID;
    frame.LogID = in_frame->LogID;
    frame.service = service_id;
    frame.method = FRAME_METHOD_DATA;
    frame.headers_count = in_frame->headers_count;
    memcpy(frame.headers, in_frame->headers, sizeof(pbbp2_Header) * in_frame->headers_count);

    const char ack_json[] = "{\"code\":200}";
    payload_buf_t payload = {.data = (uint8_t *)ack_json, .len = sizeof(ack_json) - 1};
    frame.payload.funcs.encode = encode_payload_cb;
    frame.payload.arg = &payload;

    pb_ostream_t stream = pb_ostream_from_buffer(buf, len);
    if (!pb_encode(&stream, pbbp2_Frame_fields, &frame))
    {
        log_error("feishu channel wss build ack frame failed: %s", PB_GET_ERROR(&stream));
        return 0;
    }

    return stream.bytes_written;
}

static void edc_channel_feishu_wss_send_ping(struct mg_connection *c, struct edc_channel_feishu_wss_conn *conn)
{
    edc_channel_actor_state_t *state = conn->actor->state;
    uint8_t buf[128];
    size_t len = edc_channel_feishu_build_ping_frame(state->feishu.wss_service_id, buf, sizeof(buf));
    if (len > 0)
    {
        mg_ws_send(c, buf, len, WEBSOCKET_OP_BINARY);
    }
}

static bool decode_payload_cb(pb_istream_t *stream, const pb_field_t *field, void **arg)
{
    payload_buf_t *ctx = (payload_buf_t *)*arg;
    size_t len = stream->bytes_left;
    ctx->data = malloc(len);
    if (!ctx->data)
        return false;
    if (!pb_read(stream, ctx->data, len))
    {
        free(ctx->data);
        ctx->data = NULL;
        return false;
    }
    ctx->len = len;
    return true;
}

static const char *edc_channel_feishu_wss_frame_get_header(pbbp2_Frame *frame, const char *key)
{
    for (int i = 0; i < frame->headers_count; i++)
    {
        if (strcmp(frame->headers[i].key, key) == 0)
        {
            return frame->headers[i].value;
        }
    }

    return NULL;
}

static void edc_channel_feishu_handle_event_payload(struct mg_connection *c, struct edc_channel_feishu_wss_conn *conn,
                                                   const uint8_t *payload, const size_t len)
{
    edc_actor_t *actor = conn->actor;
    cJSON *data = cJSON_ParseWithLength((const char *)payload, len);
    edc_actor_send(actor, edc_actor_msg_new_sub_type(BC_ACTOR_MSG_CHANNEL_RECV, edc_channel_feishu, actor, NULL,
                                                   edc_channel_feishu_wss_event_resp_new(data),
                                                   (edc_actor_data_free_fn)edc_channel_feishu_resp_free));
}

static void edc_channel_feishu_on_wss_message(struct mg_connection *c, struct edc_channel_feishu_wss_conn *conn,
                                             struct mg_ws_message *wm)
{
    edc_actor_t *actor = conn->actor;
    edc_channel_actor_state_t *state = actor->state;

    log_info("recv wss message %zu bytes", wm->data.len);

    pbbp2_Frame frame = pbbp2_Frame_init_zero;
    pb_istream_t is = pb_istream_from_buffer((pb_byte_t *)wm->data.buf, wm->data.len);
    payload_buf_t payload = {0};
    frame.payload.funcs.decode = decode_payload_cb;
    frame.payload.arg = &payload;

    if (!pb_decode(&is, pbbp2_Frame_fields, &frame))
    {
        log_error("channel feishu: wss endpoint recv error");
        goto exit;
    }

    // method: CONTROL
    const char *type = edc_channel_feishu_wss_frame_get_header(&frame, "type");
    if (frame.method == FRAME_METHOD_CONTROL)
    {
        if (type && !strcmp(type, "pong"))
        {
            log_info("recv pong frame from wss endpoint, payload: %s",
                     payload.data ? (const char *)payload.data : "NULL");
        }
    }
    else if (frame.method == FRAME_METHOD_DATA)
    {
        if (type && !strcmp(type, "event"))
        {
            edc_channel_feishu_handle_event_payload(c, conn, payload.data, payload.len);
            uint8_t buf[1024] = {0};
            size_t ack_len = edc_channel_feishu_build_ack(&frame, state->feishu.wss_service_id, buf, sizeof(buf));
            if (ack_len > 0)
            {
                mg_ws_send(c, buf, ack_len, WEBSOCKET_OP_BINARY);
                log_info("feishu channel: send ack %zu bytes", ack_len);
            }
        }
    }
exit:
    if (payload.data)
    {
        free(payload.data);
    }

#undef FRAME_METHOD_CONTROL
#undef FRAME_METHOD_DATA
}

static void edc_channel_feishu_connect_wss_endpoint_fn(struct mg_connection *c, int ev, void *ev_data)
{
    if (ev == MG_EV_OPEN)
    {
        // c->is_hexdumping = 1;
    }
    else if (ev == MG_EV_WS_OPEN)
    {
        struct edc_channel_feishu_wss_conn *conn = c->fn_data;
        conn->last_ping_time = mg_millis();
    }
    else if (ev == MG_EV_POLL)
    {
        struct edc_channel_feishu_wss_conn *conn = c->fn_data;
        edc_channel_actor_state_t *state = conn->actor->state;
        uint64_t seconds = (mg_millis() - conn->last_ping_time) / 1000;

        if (seconds > state->feishu.wss_client_config.ping_interval)
        {
            edc_channel_feishu_wss_send_ping(c, conn);
            log_info("feishu channel send ping to wss server.");
            conn->last_ping_time = mg_millis();
        }
    }
    else if (ev == MG_EV_WS_MSG)
    {
        edc_channel_feishu_on_wss_message(c, c->fn_data, ev_data);
    }
    else if (ev == MG_EV_CLOSE)
    {
        edc_channel_feishu_wss_conn_free(c->fn_data);
    }
}

static void edc_channel_feishu_wss_connect(edc_actor_t *actor)
{
    edc_channel_actor_state_t *state = actor->state;
    struct edc_channel_feishu_wss_conn *conn = edc_channel_feishu_wss_conn_new(actor);
    edc_wss_async_connect(state->network, state->feishu.wss_url, edc_channel_feishu_connect_wss_endpoint_fn, conn, -1);
}

static void edc_channel_feishu_handle_wss_endpoint_recv(edc_actor_t *actor, struct edc_channel_feishu_resp *resp)
{
    if (!edc_channel_feishu_parse_wss_endpoint_resp(actor->state, resp->wss_endpoint_resp))
    {
        return;
    }

    edc_channel_feishu_wss_connect(actor);
}

static sds edc_channel_feishu_detach_str(cJSON *s)
{
    char *val = s->valuestring;
    s->valuestring = NULL;
    sds result = sdsnew(val);
    free(val);
    return result;
}

static void edc_channel_feishu_handle_wss_event_recv(edc_actor_t *actor, struct edc_channel_feishu_resp *resp)
{
    char *tmp = NULL;
    edc_channel_actor_state_t *state = actor->state;
    cJSON *payload = resp->wss_event_resp;
    cJSON *header = cJSON_GetObjectItem(payload, "header");
    cJSON *message_content = NULL;

    if (!header || !cJSON_IsObject(header))
    {
        goto format_error_exit;
    }

    cJSON *event_type = cJSON_GetObjectItem(header, "event_type");
    if (!event_type || !cJSON_IsString(event_type))
    {
        goto format_error_exit;
    }

    const char im_message_recv_type[] = "im.message.receive_v1";
    if (strncmp(cJSON_GetStringValue(event_type), im_message_recv_type, sizeof(im_message_recv_type) - 1))
    {
        log_info("feishu channel: recv %s event. skip", cJSON_GetStringValue(event_type));
        return;
    }

    cJSON *event = cJSON_GetObjectItem(payload, "event");
    if (!event || !cJSON_IsObject(event))
    {
        goto format_error_exit;
    }

    cJSON *message = cJSON_GetObjectItem(event, "message");
    cJSON *sender = cJSON_GetObjectItem(event, "sender");
    if (!message || !sender || !cJSON_IsObject(message) || !cJSON_IsObject(sender))
    {
        goto format_error_exit;
    }

    cJSON *message_type = cJSON_GetObjectItem(message, "message_type");
    cJSON *content = cJSON_GetObjectItem(message, "content");
    cJSON *chat_type = cJSON_GetObjectItem(message, "chat_type");
    cJSON *chat_id = cJSON_GetObjectItem(message, "chat_id");
    cJSON *message_id = cJSON_GetObjectItem(message, "message_id");

    if (!message_type || !content || !chat_type || !chat_id || !message_id || !cJSON_IsString(message_type) ||
        !cJSON_IsString(content) || !cJSON_IsString(chat_type) || !cJSON_IsString(chat_id) ||
        !cJSON_IsString(message_id))
    {
        goto format_error_exit;
    }

    const char message_type_text[] = "text";
    if (strncmp(cJSON_GetStringValue(message_type), message_type_text, sizeof(message_type_text) - 1))
    {
        log_info("feishu channel: only supports text message: message_type = %s", cJSON_GetStringValue(message_type));
        return;
    }

    message_content = cJSON_Parse(cJSON_GetStringValue(content));
    if (!message_content || !cJSON_IsObject(message_content))
    {
        goto format_error_exit;
    }

    cJSON *message_content_text = cJSON_GetObjectItem(message_content, "text");
    if (!message_content_text || !cJSON_IsString(message_content_text))
    {
        goto format_error_exit;
    }

    cJSON *sender_type = cJSON_GetObjectItem(sender, "sender_type");
    cJSON *sender_id = cJSON_GetObjectItem(sender, "sender_id");
    if (!sender_type || !sender_id || !cJSON_IsString(sender_type) || !cJSON_IsObject(sender_id))
    {
        goto format_error_exit;
    }

    char sender_type_user[] = "user";
    if (strncmp(cJSON_GetStringValue(sender_type), sender_type_user, sizeof(sender_type_user) - 1))
    {
        log_info("feishu channel: only supports user sender");
    }

    cJSON *open_id = cJSON_GetObjectItem(sender_id, "open_id");
    if (!open_id || !cJSON_IsString(open_id))
    {
        goto format_error_exit;
    }

    struct edc_channel_message *channel_message =
        edc_channel_feishu_message_new(edc_channel_feishu_detach_str(chat_type), edc_channel_feishu_detach_str(chat_id),
                                      edc_channel_feishu_detach_str(message_id), edc_channel_feishu_detach_str(open_id),
                                      edc_channel_feishu_detach_str(message_content_text));

    edc_actor_send(state->observer,
                  edc_actor_msg_new_sub_type(BC_ACTOR_MSG_CHANNEL_RECV, edc_channel_feishu, actor, NULL, channel_message,
                                            (edc_actor_data_free_fn)edc_channel_message_free));

    goto exit;

format_error_exit:
    tmp = cJSON_Print(payload);
    log_error("feishu channel event payload format error: %s", tmp);
    free(tmp);

exit:
    if (message_content)
    {
        cJSON_Delete(message_content);
    }
}

static void edc_channel_feishu_handle_send_message_recv(edc_actor_t *actor, struct edc_channel_feishu_resp *resp)
{
    cJSON *body = resp->send_msg_resp;
    cJSON *code = cJSON_GetObjectItem(body, "code");
    char *tmp = NULL;
    if (!code || !cJSON_IsNumber(code))
    {
        goto format_error_exit;
    }

    int code_num = (int)code->valuedouble;
    if (code_num)
    {
        cJSON *msg = cJSON_GetObjectItem(body, "msg");
        if (!msg || !cJSON_IsString(msg))
        {
            goto format_error_exit;
        }

        log_error("feishu channel: send message error: %s", cJSON_GetStringValue(msg));
    }

    return;

format_error_exit:
    tmp = cJSON_Print(body);
    log_error("feishu channel: send message resp format error: %s", tmp);
    free(tmp);
    return;
}

static void edc_channel_feishu_handle_recv(edc_actor_t *actor, edc_actor_msg_t *msg)
{
    struct edc_channel_feishu_resp *resp = msg->data;
    if (resp->type == BC_CHANNEL_FEISHU_TENANT_TOKEN_RESP)
    {
        edc_channel_feishu_handle_tenant_token_recv(actor, resp);
    }
    else if (resp->type == BC_CHANNEL_FEISHU_WSS_ENDPOINT_RESP)
    {
        edc_channel_feishu_handle_wss_endpoint_recv(actor, resp);
    }
    else if (resp->type == BC_CHANNEL_FEISHU_WSS_EVENT_RESP)
    {
        edc_channel_feishu_handle_wss_event_recv(actor, resp);
    }
    else if (resp->type == BC_CHANNEL_FEISHU_SEND_MSG_RESP)
    {
        edc_channel_feishu_handle_send_message_recv(actor, resp);
    }
}

struct edc_channel_feishu_send_message_req
{
    edc_actor_t *actor;
    const char *body;
};

struct edc_channel_feishu_send_message_req *edc_channel_feishu_send_message_req_new(edc_actor_t *actor, const char *body)
{
    struct edc_channel_feishu_send_message_req *req = g_new0(struct edc_channel_feishu_send_message_req, 1);
    req->actor = actor;
    req->body = body;
    return req;
}

static void edc_channel_feishu_send_message_req_free(struct edc_channel_feishu_send_message_req *req)
{
    if (req->body)
        free((char *)req->body);
    free(req);
}

static void edc_channel_feishu_make_send_message_req(struct mg_connection *c)
{
    struct edc_channel_feishu_send_message_req *req = c->fn_data;
    edc_actor_t *actor = req->actor;
    edc_channel_actor_state_t *state = actor->state;

    struct mg_str host = mg_url_host(FEISHU_SEND_MESSAGE_URL);
    mg_printf(c, "POST %s HTTP/1.1\r\n", mg_url_uri(FEISHU_SEND_MESSAGE_URL));
    mg_printf(c, "Host: %.*s\r\n", (int)host.len, host.buf);
    mg_printf(c, "Authorization: Bearer %s\r\n", state->feishu.tenant_access_token);
    mg_printf(c, "Content-Type: application/json\r\n");
    mg_printf(c, "Content-Length: %d\r\n\r\n", (int)strlen(req->body));
    mg_printf(c, "%s", req->body);
}

static void edc_channel_feishu_handle_send_message_resp(struct mg_connection *c,
                                                       struct edc_channel_feishu_send_message_req *req,
                                                       struct mg_http_message *hm)
{
    cJSON *resp = cJSON_ParseWithLength(hm->body.buf, hm->body.len);
    if (!resp || !cJSON_IsObject(resp))
    {
        log_error("feishu channel send message resp format error: %.*s", (int)hm->body.len, hm->body.buf);
    }

    edc_actor_send(req->actor, edc_actor_msg_new_sub_type(BC_ACTOR_MSG_CHANNEL_RECV, edc_channel_feishu, req->actor, NULL,
                                                        edc_channel_feishu_send_message_resp_new(resp),
                                                        (edc_actor_data_free_fn)edc_channel_feishu_resp_free));
}

static void edc_channel_feishu_send_msg_conn_fn(struct mg_connection *c, int ev, void *ev_data)
{
    struct edc_channel_feishu_send_message_req *req = ev == BC_CONN_ERROR ? ev_data : c->fn_data;
    if (ev == MG_EV_TLS_HS)
    {
        edc_channel_feishu_make_send_message_req(c);
    }
    else if (ev == MG_EV_HTTP_MSG)
    {
        edc_channel_feishu_handle_send_message_resp(c, req, ev_data);
        edc_channel_feishu_send_message_req_free(req);
        c->is_closing = 1;
    }
    else if (ev == BC_CONN_TIME_OUT || ev == MG_EV_ERROR)
    {
        edc_channel_feishu_send_message_req_free(req);
    }
    else if (ev == BC_CONN_ERROR)
    {
        edc_channel_feishu_send_message_req_free(ev_data);
    }
    else if (ev == MG_EV_CLOSE)
    {
    }
}

static void edc_channel_feishu_handle_send(edc_actor_t *actor, edc_actor_msg_t *msg)
{
    struct edc_channel_message *message = msg->data;
    edc_channel_actor_state_t *state = actor->state;
    if (!state->feishu.tenant_access_token)
    {
        log_error("feishu channel: tenant_access_token is NULL. wait for setup");
        return;
    }

    cJSON *content = cJSON_CreateObject();
    cJSON_AddStringToObject(content, "text", message->content);
    char *content_text = cJSON_PrintUnformatted(content);

    cJSON *send_req = cJSON_CreateObject();
    cJSON_AddStringToObject(send_req, "receive_id", message->channel->feishu.chat_id);
    cJSON_AddStringToObject(send_req, "msg_type", "text");
    cJSON_AddStringToObject(send_req, "content", content_text);

    char *body = cJSON_PrintUnformatted(send_req);
    edc_channel_feishu_http_connect(actor, FEISHU_SEND_MESSAGE_URL, edc_channel_feishu_send_msg_conn_fn,
                                   edc_channel_feishu_send_message_req_new(actor, body));

    free(content_text);
    cJSON_Delete(content);
    cJSON_Delete(send_req);
}

static void edc_channel_feishu_handler(edc_actor_t *actor, edc_actor_msg_t *msg)
{
    switch (msg->type)
    {
    case BC_ACTOR_MSG_START:
        edc_channel_feishu_tenant_token_req(actor);
        edc_channel_feishu_wss_endpoint_req(actor);
        break;
    case BC_ACTOR_MSG_CHANNEL_RECV:
        edc_channel_feishu_handle_recv(actor, msg);
        break;
    case BC_ACTOR_MSG_CHANNEL_SEND:
        edc_channel_feishu_handle_send(actor, msg);
        break;
    default:
        break;
    }
}

void edc_channel_feishu_init(edc_actor_t *channel_actor)
{
    edc_channel_actor_state_t *state = channel_actor->state;
    state->feishu.tenant_access_token = NULL;
    state->feishu.expire_source = NULL;
    state->feishu.expire = -1;
    state->feishu.wss_url = NULL;
    edc_channel_actor_add(channel_actor, edc_channel_feishu, edc_channel_feishu_handler);
}
