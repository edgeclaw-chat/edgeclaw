#include "edc_tools.h"
#include "edc_toolbox.h"
#include "edc_tools_device_shell.h"
#include "edc_config.h"
#include "log.h"
#include <edc_param_types.h>

#include "lwip/api.h"
#include "lwip/ip_addr.h"
#include "lwip/err.h"
#include <cJSON.h>
#include <glib.h>
#include "sds.h"
#include "edc_error.h"
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

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

static int send_all(int fd, const void *buf, size_t len)
{
    size_t total = 0;
    while (total < len)
    {
        ssize_t n = send(fd, (const char *)buf + total, len - total, 0);
        if (n <= 0)
            return -1;
        total += (size_t)n;
    }
    return 0;
}

static int recv_until_close_socket(int fd, sds *buf)
{
    char tmp[4096];
    ssize_t n;
    while ((n = recv(fd, tmp, sizeof(tmp), 0)) > 0)
        *buf = sdscatlen(*buf, tmp, n);
    return (n == 0) ? 0 : -1;
}

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
    if (g_ascii_strncasecmp(resp_buf, "HTTP/", 5) == 0) {
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

static void device_shell_thread_socket(struct device_shell_ctx *ctx)
{
    sds result = sdsempty();
    EdcError *error = NULL;

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        sdsfree(result);
        error = edc_error_new(EDC_ERR_EXEC,
                            "device_shell: socket() failed: %s", strerror(errno));
        ctx->callback(ctx->tb_ctx, NULL, error);
        device_shell_ctx_free(ctx);
        return;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(ctx->agent_port);
    if (inet_pton(AF_INET, ctx->agent_ip, &addr.sin_addr) != 1) {
        close(sock);
        sdsfree(result);
        error = edc_error_new(EDC_ERR_INVAL,
                            "device_shell: invalid device_ip: %s", ctx->agent_ip);
        ctx->callback(ctx->tb_ctx, NULL, error);
        device_shell_ctx_free(ctx);
        return;
    }

    struct timeval tv = {.tv_sec = 5, .tv_usec = 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(sock);
        sdsfree(result);
        error = edc_error_new(EDC_ERR_EXEC,
                            "device_shell: connect to %s:%u failed: %s",
                            ctx->agent_ip, ctx->agent_port, strerror(errno));
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

    char hdr[512];
    int hdr_len = snprintf(hdr, sizeof(hdr),
        "POST /exec HTTP/1.1\r\n"
        "Host: %s:%u\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n",
        ctx->agent_ip, ctx->agent_port, strlen(req_body));

    send_all(sock, hdr, (size_t)hdr_len);
    send_all(sock, req_body, strlen(req_body));
    cJSON_free(req_body);

    tv.tv_sec = ctx->timeout_sec;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    sds resp_buf = sdsempty();
    int recv_ret = recv_until_close_socket(sock, &resp_buf);
    close(sock);

    if (recv_ret != 0) {
        sdsfree(resp_buf);
        sdsfree(result);
        error = edc_error_new(EDC_ERR_EXEC,
                            "device_shell: recv failed: %s", strerror(errno));
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

    char hdr[512];
    int hdr_len = snprintf(hdr, sizeof(hdr),
        "POST /exec HTTP/1.1\r\n"
        "Host: %s:%u\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n",
        ctx->agent_ip, ctx->agent_port, strlen(req_body));

    netconn_write_partly(c, hdr, (size_t)hdr_len, NETCONN_COPY, NULL);
    netconn_write_partly(c, req_body, strlen(req_body), NETCONN_COPY, NULL);
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

static void device_shell_thread(void* data, void* user_data)
{
    struct device_shell_ctx *ctx = data;
    (void)user_data;

    struct edc_config *conf = ctx->tb_ctx->tb->conf;

    if (conf->device_transport && conf->device_transport[0]) {
        if (g_ascii_strcasecmp(conf->device_transport, "tty") == 0)
            device_shell_thread_lwip(ctx);
        else
            device_shell_thread_socket(ctx);
    } else if (conf->device_pty && conf->device_pty[0]) {
        device_shell_thread_lwip(ctx);
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

    struct device_shell_ctx *ctx = g_new0(struct device_shell_ctx, 1);
    ctx->tb_ctx = tb_ctx;
    ctx->callback = callback;
    ctx->command = sdsnew(cJSON_GetStringValue(cmd));
    ctx->agent_ip = sdsnew(conf->device_ip);
    ctx->agent_port = (uint16_t)(conf->device_port > 0 ? conf->device_port : 5000);
    ctx->timeout_sec = timeout_sec;

    g_thread_new("device_shell", (GThreadFunc)device_shell_thread, ctx);
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
