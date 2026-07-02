#include "edc_tools.h"
#include "edc_toolbox.h"
#include "edc_tools_device_shell.h"
#include "edc_config.h"
#include "edc_compat.h"
#include "log.h"
#include <edc_param_types.h>

#ifndef _WIN32
#include "lwip/api.h"
#include "lwip/ip_addr.h"
#include "lwip/err.h"
#endif
#include <cJSON.h>
#include <uv.h>
#include "sds.h"
#include "edc_error.h"
#include <string.h>
#include <stdlib.h>

struct device_shell_ctx
{
    struct edc_toolbox_call_ctx *tb_ctx;
    void (*callback)(struct edc_toolbox_call_ctx *, sds , EdcError *);
    sds command;
    sds agent_ip;
    uint16_t agent_port;
    int timeout_sec;
};

static void device_shell_ctx_free(struct device_shell_ctx *ctx)
{
    sdsfree(ctx->command);
    sdsfree(ctx->agent_ip);
    free(ctx);
}

#ifndef _WIN32
static err_t recv_until_close(struct netconn *c, sds *buf)
{
    struct netbuf *nb;
    u8_t *p;
    u16_t chunk;
    err_t err;

    while ((err = netconn_recv(c, &nb)) == ERR_OK) {
        netbuf_data(nb, (void **)&p, &chunk);
        *buf = sdscatlen(*buf, (char *)p, chunk);
        netbuf_delete(nb);
    }
    return err;
}
#endif

static void process_shell_response(sds resp_buf, sds *result, EdcError **error)
{
    char *body = strstr(resp_buf, "\r\n\r\n");
    if (!body) {
        *error = edc_error_new(EDC_ERR_EXEC,
                            "device_shell: invalid HTTP response");
        return;
    }
    body += 4;

    int http_status = 0;
    if (strncasecmp(resp_buf, "HTTP/", 5) == 0) {
        const char *sp = strchr(resp_buf, ' ');
        if (sp) http_status = atoi(sp + 1);
    }
    if (http_status != 200) {
        *error = edc_error_new(EDC_ERR_EXEC,
                            "device_shell: HTTP %d", http_status);
        return;
    }

    cJSON *resp_json = cJSON_Parse(body);
    if (!resp_json) {
        *error = edc_error_new(EDC_ERR_EXEC,
                            "device_shell: invalid JSON response");
        return;
    }

    cJSON *ec = cJSON_GetObjectItem(resp_json, "exit_code");
    cJSON *so = cJSON_GetObjectItem(resp_json, "stdout");
    cJSON *se = cJSON_GetObjectItem(resp_json, "stderr");

    int exit_code = ec ? ec->valueint : -1;

    if (so && cJSON_IsString(so)) *result = sdscat(*result, so->valuestring);
    if (se && cJSON_IsString(se) && se->valuestring[0]) {
        if (sdslen(*result) > 0) *result = sdscat(*result, "\n");
        *result = sdscatprintf(*result, "STDERR:\n%s", se->valuestring);
    }
    if (sdslen(*result) > 0)
        *result = sdscatprintf(*result, "\nDone (exit code %d)", exit_code);
    else
        *result = sdscatprintf(*result, "Done (exit code %d)", exit_code);

    cJSON_Delete(resp_json);

    if (exit_code != 0)
        *error = edc_error_new(EDC_ERR_EXEC, "%s", *result);
}

struct device_shell_uv_state {
    uv_loop_t loop;
    uv_tcp_t tcp;
    uv_connect_t connect_req;
    uv_write_t write_req;
    uv_timer_t timeout_timer;

    struct device_shell_ctx *shell_ctx;
    sds resp_buf;
    sds header;
    sds body_sds;

    bool timed_out;
    EdcError *error;
};

static void device_shell_uv_close_cb(uv_handle_t *handle)
{
    (void)handle;
}

static void device_shell_uv_alloc(uv_handle_t *handle, size_t suggested, uv_buf_t *buf)
{
    (void)handle;
    size_t len = suggested > 0 ? suggested : 4096;
    buf->base = malloc(len);
    buf->len = buf->base ? len : 0;
}

static void device_shell_uv_on_timeout(uv_timer_t *timer)
{
    struct device_shell_uv_state *state = timer->data;
    if (state->error || state->timed_out)
        return;
    state->timed_out = true;
    if (!uv_is_closing((uv_handle_t *)&state->tcp))
        uv_close((uv_handle_t *)&state->tcp, device_shell_uv_close_cb);
}

static void device_shell_uv_on_read(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf)
{
    struct device_shell_uv_state *state = stream->data;

    if (state->error || state->timed_out) {
        free(buf->base);
        return;
    }

    if (nread > 0) {
        state->resp_buf = sdscatlen(state->resp_buf, buf->base, nread);
        free(buf->base);
        return;
    }

    free(buf->base);
    uv_timer_stop(&state->timeout_timer);
    uv_close((uv_handle_t *)stream, device_shell_uv_close_cb);

    if (nread < 0 && nread != UV_EOF) {
        state->error = edc_error_new(EDC_ERR_EXEC,
                            "device_shell: recv failed: %s", uv_strerror((int)nread));
    }

    uv_stop(&state->loop);
}

static void device_shell_uv_on_write(uv_write_t *req, int status)
{
    struct device_shell_uv_state *state = req->data;

    if (state->error || state->timed_out)
        return;

    if (status < 0) {
        state->error = edc_error_new(EDC_ERR_EXEC,
                            "device_shell: write failed: %s", uv_strerror(status));
        uv_timer_stop(&state->timeout_timer);
        uv_close((uv_handle_t *)&state->tcp, device_shell_uv_close_cb);
        uv_stop(&state->loop);
        return;
    }

    int r = uv_read_start((uv_stream_t *)&state->tcp,
                          device_shell_uv_alloc, device_shell_uv_on_read);
    if (r < 0) {
        state->error = edc_error_new(EDC_ERR_EXEC,
                            "device_shell: read start failed: %s", uv_strerror(r));
        uv_timer_stop(&state->timeout_timer);
        uv_close((uv_handle_t *)&state->tcp, device_shell_uv_close_cb);
        uv_stop(&state->loop);
    }
}

static void device_shell_uv_on_connect(uv_connect_t *req, int status)
{
    struct device_shell_uv_state *state = req->data;

    if (state->error || state->timed_out)
        return;

    if (status < 0) {
        state->error = edc_error_new(EDC_ERR_EXEC,
                            "device_shell: connect failed: %s", uv_strerror(status));
        uv_timer_stop(&state->timeout_timer);
        uv_close((uv_handle_t *)&state->tcp, device_shell_uv_close_cb);
        uv_stop(&state->loop);
        return;
    }

    uv_timer_stop(&state->timeout_timer);
    uv_timer_start(&state->timeout_timer, device_shell_uv_on_timeout,
                   (uint64_t)state->shell_ctx->timeout_sec * 1000, 0);

    uv_buf_t bufs[2];
    bufs[0].base = state->header;
    bufs[0].len = sdslen(state->header);
    bufs[1].base = state->body_sds;
    bufs[1].len = sdslen(state->body_sds);

    int r = uv_write(&state->write_req, (uv_stream_t *)&state->tcp,
                     bufs, 2, device_shell_uv_on_write);
    if (r < 0) {
        state->error = edc_error_new(EDC_ERR_EXEC,
                            "device_shell: write failed: %s", uv_strerror(r));
        uv_timer_stop(&state->timeout_timer);
        uv_close((uv_handle_t *)&state->tcp, device_shell_uv_close_cb);
        uv_stop(&state->loop);
    }
}

static void device_shell_uv_close_walk(uv_handle_t *handle, void *arg)
{
    (void)arg;
    if (!uv_is_closing(handle))
        uv_close(handle, device_shell_uv_close_cb);
}

static void device_shell_thread_socket(struct device_shell_ctx *ctx)
{
    sds result = sdsempty();
    EdcError *error = NULL;

    struct device_shell_uv_state *state = calloc(1, sizeof(*state));
    if (!state) {
        sdsfree(result);
        error = edc_error_new(EDC_ERR_EXEC,
                            "device_shell: failed to allocate state");
        ctx->callback(ctx->tb_ctx, NULL, error);
        device_shell_ctx_free(ctx);
        return;
    }

    state->shell_ctx = ctx;
    state->resp_buf = sdsempty();
    state->tcp.data = state;
    state->connect_req.data = state;
    state->write_req.data = state;
    state->timeout_timer.data = state;

    uv_loop_init(&state->loop);
    uv_tcp_init(&state->loop, &state->tcp);
    uv_timer_init(&state->loop, &state->timeout_timer);

    cJSON *req_json = cJSON_CreateObject();
    cJSON_AddStringToObject(req_json, "command", ctx->command);
    if (ctx->timeout_sec > 0)
        cJSON_AddNumberToObject(req_json, "timeout", ctx->timeout_sec);
    char *req_body = cJSON_PrintUnformatted(req_json);
    cJSON_Delete(req_json);

    state->header = sdscatprintf(sdsempty(),
        "POST /exec HTTP/1.1\r\n"
        "Host: %s:%u\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n",
        ctx->agent_ip, ctx->agent_port, strlen(req_body));

    state->body_sds = sdsnewlen(req_body, strlen(req_body));
    cJSON_free(req_body);

    struct sockaddr_in addr;
    uv_ip4_addr(ctx->agent_ip, ctx->agent_port, &addr);

    uv_timer_start(&state->timeout_timer, device_shell_uv_on_timeout, 5000, 0);

    int r = uv_tcp_connect(&state->connect_req, &state->tcp,
                           (const struct sockaddr *)&addr,
                           device_shell_uv_on_connect);
    if (r < 0) {
        error = edc_error_new(EDC_ERR_EXEC,
                            "device_shell: connect failed: %s", uv_strerror(r));
        uv_timer_stop(&state->timeout_timer);
        uv_close((uv_handle_t *)&state->tcp, device_shell_uv_close_cb);
        uv_close((uv_handle_t *)&state->timeout_timer, device_shell_uv_close_cb);
        uv_run(&state->loop, UV_RUN_DEFAULT);
        uv_loop_close(&state->loop);

        sdsfree(state->header);
        sdsfree(state->body_sds);
        sdsfree(state->resp_buf);
        free(state);
        sdsfree(result);
        ctx->callback(ctx->tb_ctx, NULL, error);
        device_shell_ctx_free(ctx);
        return;
    }

    uv_run(&state->loop, UV_RUN_DEFAULT);

    uv_walk(&state->loop, device_shell_uv_close_walk, NULL);
    uv_run(&state->loop, UV_RUN_DEFAULT);

    if (state->error) {
        error = state->error;
        sdsfree(state->resp_buf);
    } else if (state->timed_out) {
        sdsfree(state->resp_buf);
        error = edc_error_new(EDC_ERR_EXEC,
                            "device_shell: request timed out after %ds",
                            ctx->timeout_sec);
    } else {
        process_shell_response(state->resp_buf, &result, &error);
        sdsfree(state->resp_buf);
    }

    sdsfree(state->header);
    sdsfree(state->body_sds);
    uv_loop_close(&state->loop);
    free(state);

    if (error) {
        ctx->callback(ctx->tb_ctx, NULL, error);
        sdsfree(result);
    } else {
        ctx->callback(ctx->tb_ctx, result, NULL);
    }

    device_shell_ctx_free(ctx);
}

#ifndef _WIN32
static void device_shell_thread_lwip(struct device_shell_ctx *ctx)
{
    sds result = sdsempty();
    EdcError *error = NULL;

    ip_addr_t addr;
    if (!ip4addr_aton(ctx->agent_ip, &addr)) {
        sdsfree(result);
        error = edc_error_new(EDC_ERR_INVAL,
                            "device_shell: invalid device_ip in config: %s", ctx->agent_ip);
        ctx->callback(ctx->tb_ctx, NULL, error);
        device_shell_ctx_free(ctx);
        return;
    }

    struct netconn *c = netconn_new(NETCONN_TCP);
    if (!c) {
        sdsfree(result);
        error = edc_error_new(EDC_ERR_EXEC,
                            "device_shell: netconn_new failed");
        ctx->callback(ctx->tb_ctx, NULL, error);
        device_shell_ctx_free(ctx);
        return;
    }

    err_t err = netconn_connect(c, &addr, ctx->agent_port);
    if (err != ERR_OK) {
        netconn_delete(c);
        sdsfree(result);
        error = edc_error_new(EDC_ERR_EXEC,
                            "device_shell: connect to %s:%u failed (%d)",
                            ctx->agent_ip, ctx->agent_port, err);
        ctx->callback(ctx->tb_ctx, NULL, error);
        device_shell_ctx_free(ctx);
        return;
    }

    cJSON *req_json = cJSON_CreateObject();
    cJSON_AddStringToObject(req_json, "command", ctx->command);
    if (ctx->timeout_sec > 0)
        cJSON_AddNumberToObject(req_json, "timeout", ctx->timeout_sec);
    char *req_body = cJSON_PrintUnformatted(req_json);
    cJSON_Delete(req_json);

    sds hdr = sdscatprintf(sdsempty(),
        "POST /exec HTTP/1.1\r\n"
        "Host: %s:%u\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n",
        ctx->agent_ip, ctx->agent_port, strlen(req_body));

    netconn_write_partly(c, hdr, sdslen(hdr), NETCONN_COPY, NULL);
    netconn_write_partly(c, req_body, strlen(req_body), NETCONN_COPY, NULL);
    sdsfree(hdr);
    cJSON_free(req_body);

    netconn_set_recvtimeout(c, ctx->timeout_sec * 1000);

    sds resp_buf = sdsempty();
    err_t recv_err = recv_until_close(c, &resp_buf);
    netconn_close(c);
    netconn_delete(c);

    if (recv_err == ERR_TIMEOUT) {
        sdsfree(resp_buf);
        sdsfree(result);
        error = edc_error_new(EDC_ERR_EXEC,
                            "device_shell: request timed out after %ds", ctx->timeout_sec);
        ctx->callback(ctx->tb_ctx, NULL, error);
        device_shell_ctx_free(ctx);
        return;
    }

    process_shell_response(resp_buf, &result, &error);
    sdsfree(resp_buf);

    if (error) {
        ctx->callback(ctx->tb_ctx, NULL, error);
        sdsfree(result);
    } else {
        ctx->callback(ctx->tb_ctx, result, NULL);
    }

    device_shell_ctx_free(ctx);
}
#endif

static void device_shell_thread(void *data)
{
    struct device_shell_ctx *ctx = data;
    struct edc_config *conf = ctx->tb_ctx->tb->conf;

    if (conf->device_transport && conf->device_transport[0]) {
        if (strcasecmp(conf->device_transport, "tty") == 0)
#ifndef _WIN32
            device_shell_thread_lwip(ctx);
        else
#endif
            device_shell_thread_socket(ctx);
    } else if (conf->device_pty && conf->device_pty[0]) {
#ifndef _WIN32
        device_shell_thread_lwip(ctx);
#else
        device_shell_thread_socket(ctx);
#endif
    } else {
        device_shell_thread_socket(ctx);
    }
}

static void
edc_tool_device_shell_exec_async(struct edc_toolbox_call_ctx *tb_ctx, struct edc_tool *tool,
                                 cJSON *args,
                                 void (*callback)(struct edc_toolbox_call_ctx *, sds , EdcError *))
{
    (void)tool;

    cJSON *cmd = cJSON_GetObjectItem(args, "command");
    if (!cmd || !cJSON_IsString(cmd)) {
        EdcError *error = edc_error_new(EDC_ERR_INVAL,
                                    "device_shell: missing 'command' parameter");
        callback(tb_ctx, NULL, error);
        return;
    }

    struct edc_config *conf = tb_ctx->tb->conf;
    if (!conf->device_ip) {
        EdcError *error = edc_error_new(EDC_ERR_INVAL,
                                    "device_shell: device_ip not configured in config.json");
        callback(tb_ctx, NULL, error);
        return;
    }

    int timeout_sec = 30;
    cJSON *timeout_json = cJSON_GetObjectItem(args, "timeout");
    if (timeout_json && cJSON_IsNumber(timeout_json) && timeout_json->valueint > 0)
        timeout_sec = timeout_json->valueint;

    struct device_shell_ctx *ctx = calloc(1, sizeof(struct device_shell_ctx));
    ctx->tb_ctx = tb_ctx;
    ctx->callback = callback;
    ctx->command = sdsnew(cJSON_GetStringValue(cmd));
    ctx->agent_ip = sdsnew(conf->device_ip);
    ctx->agent_port = (uint16_t)(conf->device_port > 0 ? conf->device_port : 5000);
    ctx->timeout_sec = timeout_sec;

    uv_thread_t thread;
    uv_thread_create(&thread, device_shell_thread, ctx);
}

struct edc_tool *edc_tool_create_device_shell(struct edc_toolbox *tb)
{
    struct edc_tool_param *params = NULL;
    LL_APPEND(params, edc_tool_param_new("command", edc_param_str_type("Shell command to execute on remote device", NULL), TRUE));
    LL_APPEND(params, edc_tool_param_new("timeout", edc_param_int_type("Timeout in seconds (default 30)", 1, 3600), FALSE));

    struct edc_tool *tool = edc_tool_new("device_shell",
        "Execute a shell command on a remote device agent (PPPoS or TCP socket)", params,
        edc_tool_device_shell_exec_async);
    edc_toolbox_register(tb, tool);
    return tool;
}

void edc_tools_device_shell_register(struct edc_toolbox *tb)
{
    edc_tool_create_device_shell(tb);
}
