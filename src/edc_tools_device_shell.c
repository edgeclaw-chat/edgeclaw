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

struct device_shell_ctx
{
    struct edc_toolbox_call_ctx *tb_ctx;
    void (*callback)(struct edc_toolbox_call_ctx *, GString *, GError *);
    char *command;
    char *agent_ip;
    uint16_t agent_port;
    int timeout_sec;
};

static void device_shell_ctx_free(struct device_shell_ctx *ctx)
{
    g_free(ctx->command);
    g_free(ctx->agent_ip);
    g_free(ctx);
}

static err_t recv_until_close(struct netconn *c, GString *buf)
{
    struct netbuf *nb;
    u8_t *p;
    u16_t chunk;
    err_t err;

    while ((err = netconn_recv(c, &nb)) == ERR_OK) {
        netbuf_data(nb, (void **)&p, &chunk);
        g_string_append_len(buf, (gchar *)p, chunk);
        netbuf_delete(nb);
    }
    return err;
}

static void device_shell_thread(gpointer data, gpointer user_data)
{
    struct device_shell_ctx *ctx = data;
    (void)user_data;

    GString *result = g_string_new(NULL);
    GError *error = NULL;

    ip_addr_t addr;
    if (!ip4addr_aton(ctx->agent_ip, &addr)) {
        error = g_error_new(BC_TOOL_ERROR, BC_TOOL_ARG_INVALID,
                            "device_shell: invalid device_ip in config: %s", ctx->agent_ip);
        ctx->callback(ctx->tb_ctx, NULL, error);
        device_shell_ctx_free(ctx);
        return;
    }

    struct netconn *c = netconn_new(NETCONN_TCP);
    if (!c) {
        error = g_error_new(BC_TOOL_ERROR, BC_TOOL_EXEC_ERROR,
                            "device_shell: netconn_new failed");
        ctx->callback(ctx->tb_ctx, NULL, error);
        device_shell_ctx_free(ctx);
        return;
    }

    err_t err = netconn_connect(c, &addr, ctx->agent_port);
    if (err != ERR_OK) {
        netconn_delete(c);
        error = g_error_new(BC_TOOL_ERROR, BC_TOOL_EXEC_ERROR,
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
    free(req_body);

    GString *resp_buf = g_string_new(NULL);
    recv_until_close(c, resp_buf);
    netconn_close(c);
    netconn_delete(c);

    char *body = strstr(resp_buf->str, "\r\n\r\n");
    if (!body) {
        g_string_free(resp_buf, TRUE);
        error = g_error_new(BC_TOOL_ERROR, BC_TOOL_EXEC_ERROR,
                            "device_shell: invalid HTTP response");
        ctx->callback(ctx->tb_ctx, NULL, error);
        device_shell_ctx_free(ctx);
        return;
    }
    body += 4;

    cJSON *resp_json = cJSON_Parse(body);
    g_string_free(resp_buf, TRUE);

    if (!resp_json) {
        error = g_error_new(BC_TOOL_ERROR, BC_TOOL_EXEC_ERROR,
                            "device_shell: invalid JSON response");
        ctx->callback(ctx->tb_ctx, NULL, error);
        device_shell_ctx_free(ctx);
        return;
    }

    cJSON *ec = cJSON_GetObjectItem(resp_json, "exit_code");
    cJSON *so = cJSON_GetObjectItem(resp_json, "stdout");
    cJSON *se = cJSON_GetObjectItem(resp_json, "stderr");

    int exit_code = ec ? ec->valueint : -1;

    if (so && cJSON_IsString(so)) g_string_append(result, so->valuestring);
    if (se && cJSON_IsString(se) && se->valuestring[0]) {
        if (result->len > 0) g_string_append(result, "\n");
        g_string_append_printf(result, "STDERR:\n%s", se->valuestring);
    }
    if (result->len > 0)
        g_string_append_printf(result, "\nDone (exit code %d)", exit_code);
    else
        g_string_append_printf(result, "Done (exit code %d)", exit_code);

    cJSON_Delete(resp_json);

    if (exit_code != 0) {
        error = g_error_new(BC_TOOL_ERROR, BC_TOOL_EXEC_ERROR, "%s", result->str);
        ctx->callback(ctx->tb_ctx, NULL, error);
        g_string_free(result, TRUE);
    } else {
        ctx->callback(ctx->tb_ctx, result, NULL);
    }

    device_shell_ctx_free(ctx);
}

static void
edc_tool_device_shell_exec_async(struct edc_toolbox_call_ctx *tb_ctx, struct edc_tool *tool,
                                 cJSON *args,
                                 void (*callback)(struct edc_toolbox_call_ctx *, GString *, GError *))
{
    (void)tool;

    cJSON *cmd = cJSON_GetObjectItem(args, "command");
    if (!cmd || !cJSON_IsString(cmd)) {
        GError *error = g_error_new(BC_TOOL_ERROR, BC_TOOL_ARG_INVALID,
                                    "device_shell: missing 'command' parameter");
        callback(tb_ctx, NULL, error);
        return;
    }

    struct edc_config *conf = tb_ctx->tb->conf;
    if (!conf->device_ip) {
        GError *error = g_error_new(BC_TOOL_ERROR, BC_TOOL_ARG_INVALID,
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
    ctx->command = g_strdup(cJSON_GetStringValue(cmd));
    ctx->agent_ip = g_strdup(conf->device_ip);
    ctx->agent_port = (uint16_t)(conf->device_port > 0 ? conf->device_port : 5000);
    ctx->timeout_sec = timeout_sec;

    g_thread_new("device_shell", (GThreadFunc)device_shell_thread, ctx);
}

struct edc_tool *edc_tool_create_device_shell(struct edc_toolbox *tb)
{
    GSList *params = NULL;
    params = g_slist_append(params,
        edc_tool_param_new("command", edc_param_str_type("Shell command to execute on remote device", NULL), TRUE));
    params = g_slist_append(params,
        edc_tool_param_new("timeout", edc_param_int_type("Timeout in seconds (default 30)", 1, 3600), FALSE));

    struct edc_tool *tool = edc_tool_new("device_shell",
        "Execute a shell command on a remote device via lwIP PPPoS agent", params,
        edc_tool_device_shell_exec_async);
    edc_toolbox_register(tb, tool);
    return tool;
}

void edc_tools_device_shell_register(struct edc_toolbox *tb)
{
    edc_tool_create_device_shell(tb);
}
