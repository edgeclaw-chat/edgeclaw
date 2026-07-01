#include "edc_channel_telegram.h"
#include "edc_channel_actor.h"
#include "cJSON.h"
#include "log.h"
#include <glib.h>
#include "sds.h"

static const char *telegram_api_host = "api.telegram.org";
static sds telegram_build_path(struct channel_conf *conf, const char *method, const char *args[])
{
    sds url = sdsempty();
    url = sdscatprintf(url, "/bot%s/%s", conf->telegram.bot_key, method);
    if (!args)
    {
        return url;
    }

    url = sdscat(url, "?");

    const char **p = args;
    while (TRUE)
    {
        url = sdscatprintf(url, "%s", *p);
        ++p;
        if (!p || !(*p))
        {
            break;
        }
        url = sdscat(url, "&");
    }

    return url;
}

struct edc_channel_telegram_req
{
    edc_actor_t *actor;
    const char *host;
    sds path;
    sds url;
    const char *proxy;
    int method; // 0: GET, 1: POST
    sds body;
    bool connected;
    sds resp;
    uint64_t create_time;
    struct edc_channel_telegram_req *next;
};

static void __bc_channel_telegram_req_free(struct edc_channel_telegram_req *req, bool free_next)
{
    sdsfree(req->url);
    sdsfree(req->path);
    if (req->body)
    {
        sdsfree(req->body);
    }
    if (req->resp)
    {
        sdsfree(req->resp);
    }

    if (free_next && req->next)
    {
        __bc_channel_telegram_req_free(req->next, free_next);
    }
    free(req);
}

static void edc_channel_telegram_req_free_all(struct edc_channel_telegram_req *req)
{
    __bc_channel_telegram_req_free(req, TRUE);
}

static void edc_channel_telegram_req_free(struct edc_channel_telegram_req *req)
{
    __bc_channel_telegram_req_free(req, FALSE);
}

static void edc_channel_telegram_make_http_request(struct mg_connection *c, struct edc_channel_telegram_req *req)
{
    if (req->method == 0)
    {
        mg_printf(c, "GET %s HTTP/1.1\r\nHOST: %s\r\n\r\n", req->path, req->host);
    }
    else if (req->method == 1)
    {
        mg_printf(c,
                  "POST %s HTTP/1.1\r\n"
                  "Host: %s\r\n"
                  "Content-Type: application/x-www-form-urlencoded\r\n" // 关键改动
                  "Content-Length: %d\r\n"
                  "Connection: close\r\n\r\n"
                  "%s",
                  req->path, req->host, (int)sdslen(req->body), req->body);
    }
    else
    {
        log_error("unknown method: %d", req->method);
    }
}

static void edc_channel_actor_telegram_conn_fn(struct mg_connection *c, int ev, void *ev_data)
{
    struct edc_channel_telegram_req *req = ev == BC_CONN_ERROR ? ev_data : c->fn_data;

    if (ev == MG_EV_HTTP_MSG)
    {
        edc_actor_t *actor = req->actor;
        edc_channel_actor_state_t *state = actor->state;
        struct mg_http_message *hm = (struct mg_http_message *)ev_data;
        sds buf = sdsempty();

        buf = sdscatlen(buf, hm->body.buf, hm->body.len);
        req->resp = buf;

        edc_actor_msg_t *msg = edc_actor_msg_new_sub_type(BC_ACTOR_MSG_CHANNEL_RECV, edc_channel_telegram, NULL, NULL, req,
                                                        (edc_actor_data_free_fn)edc_channel_telegram_req_free);
        edc_actor_send(actor, msg);
        c->is_closing = 1;
        state->telegram.polling = FALSE;
    }
    else if (ev == MG_EV_TLS_HS)
    {
        edc_channel_telegram_make_http_request(c, req);
    }
    else if (ev == BC_CONN_ERROR)
    {
        edc_actor_t *actor = req->actor;
        edc_channel_actor_state_t *state = actor->state;
        log_error("channel telegram http async connect failed.");
        edc_channel_telegram_req_free_all(req);
        state->telegram.polling = FALSE;
    }
    else if (ev == MG_EV_CLOSE)
    {
    }
    else if (ev == BC_CONN_TIME_OUT || ev == MG_EV_ERROR)
    {
        edc_actor_t *actor = req->actor;
        edc_channel_actor_state_t *state = actor->state;
        edc_channel_telegram_req_free_all(req);
        state->telegram.polling = FALSE;
    }
}

static bool edc_channel_actor_telegram_send_req(edc_actor_t *actor, struct edc_channel_telegram_req *req)
{
    edc_channel_actor_state_t *state = (edc_channel_actor_state_t *)actor->state;
    edc_http_async_connect(state->network, req->url, req->proxy, edc_channel_actor_telegram_conn_fn, req, 60);

    return TRUE;
}

#define TELEGRAM_MAX_MSG_LEN 4096
static struct edc_channel_telegram_req *edc_channel_actor_send_message_chunk_req(edc_actor_t *actor, const char *chat_id,
                                                                               const char *content, size_t len)
{
    edc_channel_actor_state_t *state = (edc_channel_actor_state_t *)actor->state;

    sds path = telegram_build_path(&state->config, "sendMessage", NULL);
    char *escaped_content = g_uri_escape_bytes((uint8_t *)content, len, NULL);
    sds post_fields = sdscatprintf(sdsempty(), "chat_id=%s&text=%s", chat_id, escaped_content);
    free(escaped_content);

    struct edc_channel_telegram_req *req = g_new0(struct edc_channel_telegram_req, 1);
    const char *proxy = g_getenv("https_proxy");

    req->actor = actor;
    req->proxy = proxy;
    req->path = path;
    req->host = telegram_api_host;
    req->connected = FALSE;
    req->url = sdscatprintf(sdsempty(), "https://%s%s", req->host, req->path);
    req->body = sdsnewlen(post_fields, sdslen(post_fields));
    sdsfree(post_fields);
    req->method = 1;
    req->next = NULL;

    return req;
}

static size_t find_utf8_boundary(const char *content, size_t max_len)
{
    if (strlen(content) <= max_len)
        return max_len;

    for (int i = max_len; i >= 0; i--)
    {
        if ((content[i] & 0xC0) != 0x80)
            return (size_t)i;
    }

    return max_len;
}

static bool edc_channel_actor_send_message(edc_actor_t *actor, const struct edc_channel_message *msg)
{
    const char *content = msg->content;
    const char *chat_id = msg->channel->telegram.chat_id;
    size_t n = sdslen(msg->content);
    if (n < TELEGRAM_MAX_MSG_LEN)
    {
        struct edc_channel_telegram_req *req = edc_channel_actor_send_message_chunk_req(actor, chat_id, content, n);
        return edc_channel_actor_telegram_send_req(actor, req);
    }

    size_t offset = 0;

    struct edc_channel_telegram_req *req = g_new0(struct edc_channel_telegram_req, 1);
    struct edc_channel_telegram_req *p = req;

    while (offset < n)
    {
        size_t remaining = n - offset;
        size_t chunk_len;

        if (remaining > TELEGRAM_MAX_MSG_LEN)
        {
            chunk_len = find_utf8_boundary(content + offset, TELEGRAM_MAX_MSG_LEN);
        }
        else
        {
            chunk_len = remaining;
        }

        p->next = edc_channel_actor_send_message_chunk_req(actor, chat_id, content + offset, chunk_len);
        p = p->next;

        offset += chunk_len;
    }

    p = req;
    req = req->next;
    edc_channel_telegram_req_free(p);
    if (req)
    {
        edc_channel_actor_telegram_send_req(actor, req);
    }

    return TRUE;
}

static struct edc_channel_message *process_user_input(edc_channel_actor_state_t *state, cJSON *root)
{
    struct channel_conf *conf = &state->config;

    cJSON *results = cJSON_GetObjectItem(root, "result");
    if (!results || !cJSON_IsArray(results) || cJSON_GetArraySize(results) == 0)
    {
        log_info("No new messages");
        return NULL;
    }

    cJSON *last_update = cJSON_GetArrayItem(results, cJSON_GetArraySize(results) - 1);
    cJSON *update_id_json = cJSON_GetObjectItem(last_update, "update_id");
    if (!update_id_json || !cJSON_IsNumber(update_id_json))
    {
        log_error("No update_id in update");
        return NULL;
    }

    cJSON *message = cJSON_GetObjectItem(last_update, "message");
    if (!message)
    {
        log_error("No message in update");
        return NULL;
    }

    cJSON *from = cJSON_GetObjectItem(message, "from");
    if (!from || !cJSON_IsObject(from))
    {
        log_error("No from in message");
        return NULL;
    }

    cJSON *user_id = cJSON_GetObjectItem(from, "id");
    if (!user_id || !cJSON_IsNumber(user_id))
    {
        log_error("No user id in message");
        return NULL;
    }

    cJSON *chat = cJSON_GetObjectItem(message, "chat");
    if (!chat)
    {
        log_error("No chat in message");
        return NULL;
    }

    cJSON *chat_id_json = cJSON_GetObjectItem(chat, "id");
    if (!chat_id_json || !cJSON_IsNumber(chat_id_json))
    {
        log_error("No chat id in message");
        return NULL;
    }

    cJSON *text = cJSON_GetObjectItem(message, "text");
    if (!text || !cJSON_IsString(text))
    {
        log_error("No text in message");
        return NULL;
    }

    if (conf->telegram.user_id != (int64_t)user_id->valuedouble)
    {
        log_error("user id not match");
        return NULL;
    }

    struct edc_channel_message *msg = edc_channel_telegram_message_new(
        sdscatprintf(sdsempty(), "%.0lf", chat_id_json->valuedouble), sdsnew(cJSON_GetStringValue(text)));

    state->telegram.update_id = 1 + (int64_t)update_id_json->valuedouble;

    return msg;
}

static struct edc_channel_message *edc_channel_handle_telegram_user_input(edc_actor_t *actor,
                                                                        const struct edc_channel_telegram_req *req)
{
    cJSON *input = cJSON_Parse(req->resp);
    if (input == NULL)
    {
        log_error("parse input failed: %s", (char *)req->resp);
        return NULL;
    }

    struct edc_channel_message *msg = process_user_input(actor->state, input);
    cJSON_Delete(input);
    if (!msg)
    {
        log_error("process user input failed");
        return NULL;
    }

    return msg;
}

static void edc_channel_handle_telegram_reply(edc_actor_t *actor, struct edc_channel_telegram_req *req)
{
    cJSON *result = cJSON_Parse(req->resp);

    if (result == NULL)
    {
        log_error("reply to user failed. parse input failed: %s", (char *)req->resp);
        return;
    }

    cJSON *ok = cJSON_GetObjectItem(result, "ok");
    if (!ok || !cJSON_IsTrue(ok))
    {
        cJSON *desc = cJSON_GetObjectItem(result, "description");
        log_error("telegram api error: %s", desc && cJSON_IsString(desc) ? desc->valuestring : "unknown");
    }

    if (req->next)
    {
        edc_channel_actor_telegram_send_req(actor, req->next);
    }

    cJSON_Delete(result);
}

static void edc_channel_actor_telegram_handle_send(edc_actor_t *actor, edc_actor_msg_t *msg)
{
    struct edc_channel_message *channel_message = msg->data;
    edc_channel_actor_send_message(actor, channel_message);
}

static struct edc_channel_message *edc_channel_actor_telegram_handle_recv(edc_actor_t *actor, edc_actor_msg_t *msg)
{
    struct edc_channel_telegram_req *req = msg->data;
    struct edc_channel_message *message = NULL;
    if (req->method == 0)
    {
        message = edc_channel_handle_telegram_user_input(actor, req);
    }
    else
    {
        edc_channel_handle_telegram_reply(actor, (struct edc_channel_telegram_req *)req);
    }

    return message;
}

static void edc_channel_actor_telegram_poll(uv_timer_t *timer)
{
    edc_channel_actor_state_t *state = container_of(timer, edc_channel_actor_state_t, telegram.poll_timer);
    edc_actor_t *actor = state->actor;

    if (state->telegram.polling)
    {
        return;
    }

    sds args[] = {sdscatprintf(sdsempty(), "offset=%" PRId64, state->telegram.update_id), NULL};
    sds path = telegram_build_path(&state->config, "getUpdates", (const char **)args);
    sdsfree(args[0]);

    struct edc_channel_telegram_req *req = calloc(1, sizeof(struct edc_channel_telegram_req));
    const char *proxy = getenv("https_proxy");

    req->actor = actor;
    req->proxy = proxy;
    req->path = path;
    req->host = telegram_api_host;
    req->connected = 0;
    req->url = sdscatprintf(sdsempty(), "https://%s%s", req->host, req->path);
    req->body = NULL;
    req->method = 0;

    edc_channel_actor_telegram_send_req(actor, req);
    state->telegram.polling = TRUE;
}

static void edc_channel_actor_telegram_handler(edc_actor_t *actor, edc_actor_msg_t *msg)
{
    edc_channel_actor_state_t *state = (edc_channel_actor_state_t *)actor->state;
    switch (msg->type)
    {
    case BC_ACTOR_MSG_START: {
        uv_timer_init(actor->loop, &state->telegram.poll_timer);
        uv_timer_start(&state->telegram.poll_timer, edc_channel_actor_telegram_poll, 1000, 1000);
        break;
    }
    case BC_ACTOR_MSG_CHANNEL_SEND:
        edc_channel_actor_telegram_handle_send(actor, msg);
        break;

    case BC_ACTOR_MSG_CHANNEL_RECV: {
        struct edc_channel_message *message = edc_channel_actor_telegram_handle_recv(actor, msg);
        if (message)
        {
            edc_actor_msg_t *forward_msg =
                edc_actor_msg_new_sub_type(BC_ACTOR_MSG_CHANNEL_RECV, edc_channel_telegram, actor, msg->request_id,
                                          message, (edc_actor_data_free_fn)edc_channel_message_free);
            edc_actor_send(state->observer, forward_msg);
        }
    }

    break;
    default:
        log_error("unknown message type: %d", msg->type);
        break;
    }
}

void edc_channel_telegram_init(edc_actor_t *channel_actor)
{
    edc_channel_actor_state_t *state = channel_actor->state;
    state->telegram.update_id = -1;
    state->telegram.polling = FALSE;
    edc_channel_actor_add(channel_actor, edc_channel_telegram, edc_channel_actor_telegram_handler);
}
