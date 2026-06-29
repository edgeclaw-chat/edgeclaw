#include "edc_tools.h"
#include "edc_toolbox.h"
#include "edc_tools_device_file.h"
#include "edc_config.h"
#include "log.h"
#include <edc_param_types.h>

#include "lwip/api.h"
#include "lwip/ip_addr.h"
#include "lwip/err.h"
#include <cJSON.h>
#include <glib.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#define DEVICE_FILE_CHUNK_SIZE (256u * 1024u)

struct device_file_ctx
{
    struct edc_toolbox_call_ctx *tb_ctx;
    void (*callback)(struct edc_toolbox_call_ctx *, GString *, GError *);
    char *agent_ip;
    uint16_t agent_port;

    char *remote_path;
    char *local_path;
    char *file_data;
    size_t file_data_len;
    int file_mode;

    bool is_upload;
    int timeout_sec;
};

static void device_file_ctx_free(struct device_file_ctx *ctx)
{
    free(ctx->agent_ip);
    free(ctx->remote_path);
    free(ctx->local_path);
    free(ctx->file_data);
    free(ctx);
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

static int recv_until_close_socket(int fd, GString *buf)
{
    char tmp[4096];
    ssize_t n;
    while ((n = recv(fd, tmp, sizeof(tmp), 0)) > 0)
        g_string_append_len(buf, tmp, n);
    return (n == 0) ? 0 : -1;
}

static err_t recv_until_close(struct netconn *c, GString *buf)
{
    struct netbuf *nb;
    u8_t *p;
    u16_t chunk;
    err_t err;

    while ((err = netconn_recv(c, &nb)) == ERR_OK)
    {
        netbuf_data(nb, (void **)&p, &chunk);
        g_string_append_len(buf, (char *)p, chunk);
        netbuf_delete(nb);
    }
    return err;
}

static void device_file_send_error(struct device_file_ctx *ctx, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char *msg = g_strdup_vprintf(fmt, ap);
    va_end(ap);

    GError *error = g_error_new(BC_TOOL_ERROR, BC_TOOL_EXEC_ERROR, "%s", msg);
    ctx->callback(ctx->tb_ctx, NULL, error);
    free(msg);
}

static int parse_json_response(GString *resp_buf, cJSON **out_json, GError **error)
{
    char *body = strstr(resp_buf->str, "\r\n\r\n");
    if (!body)
    {
        *error = g_error_new(BC_TOOL_ERROR, BC_TOOL_EXEC_ERROR, "device_file: invalid HTTP response");
        return -1;
    }
    body += 4;

    int http_status = 0;
    if (g_ascii_strncasecmp(resp_buf->str, "HTTP/", 5) == 0)
    {
        const char *sp = strchr(resp_buf->str, ' ');
        if (sp)
            http_status = atoi(sp + 1);
    }
    if (http_status != 200)
    {
        *error = g_error_new(BC_TOOL_ERROR, BC_TOOL_EXEC_ERROR, "device_file: HTTP %d", http_status);
        return -1;
    }

    *out_json = cJSON_Parse(body);
    if (!*out_json)
    {
        *error = g_error_new(BC_TOOL_ERROR, BC_TOOL_EXEC_ERROR, "device_file: invalid JSON response");
        return -1;
    }

    cJSON *error_item = cJSON_GetObjectItem(*out_json, "error");
    if (error_item && cJSON_IsString(error_item))
    {
        *error = g_error_new(BC_TOOL_ERROR, BC_TOOL_EXEC_ERROR, "device_file: %s", error_item->valuestring);
        cJSON_Delete(*out_json);
        *out_json = NULL;
        return -1;
    }

    return 0;
}

static int do_upload_chunk_socket(struct device_file_ctx *ctx, const char *b64_chunk,
                                  size_t raw_chunk_len, size_t offset)
{
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
        return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(ctx->agent_port);
    if (inet_pton(AF_INET, ctx->agent_ip, &addr.sin_addr) != 1)
    {
        close(sock);
        return -1;
    }

    struct timeval tv = {.tv_sec = 5, .tv_usec = 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        close(sock);
        return -1;
    }

    cJSON *req_json = cJSON_CreateObject();
    cJSON_AddStringToObject(req_json, "path", ctx->remote_path);
    cJSON_AddStringToObject(req_json, "data", b64_chunk);
    if (ctx->file_mode > 0)
        cJSON_AddNumberToObject(req_json, "mode", ctx->file_mode);
    if (offset > 0)
        cJSON_AddNumberToObject(req_json, "offset", (double)offset);
    char *req_body = cJSON_PrintUnformatted(req_json);
    cJSON_Delete(req_json);

    char hdr[512];
    int hdr_len = snprintf(hdr, sizeof(hdr),
                           "POST /file/upload HTTP/1.1\r\n"
                           "Host: %s:%u\r\n"
                           "Content-Type: application/json\r\n"
                           "Content-Length: %zu\r\n"
                           "Connection: close\r\n"
                           "\r\n",
                           ctx->agent_ip, ctx->agent_port, strlen(req_body));

    send_all(sock, hdr, (size_t)hdr_len);
    send_all(sock, req_body, strlen(req_body));
    free(req_body);

    tv.tv_sec = ctx->timeout_sec;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    GString *resp_buf = g_string_new(NULL);
    int recv_ret = recv_until_close_socket(sock, resp_buf);
    close(sock);

    if (recv_ret != 0)
    {
        g_string_free(resp_buf, TRUE);
        return -1;
    }

    cJSON *resp_json = NULL;
    GError *error = NULL;
    int ret = parse_json_response(resp_buf, &resp_json, &error);
    g_string_free(resp_buf, TRUE);
    if (ret != 0)
    {
        if (error)
            g_error_free(error);
        return -1;
    }

    cJSON_Delete(resp_json);
    return 0;
}

static int do_upload_chunk_lwip(struct device_file_ctx *ctx, const char *b64_chunk,
                                size_t raw_chunk_len, size_t offset)
{
    ip_addr_t addr;
    if (!ip4addr_aton(ctx->agent_ip, &addr))
        return -1;

    struct netconn *c = netconn_new(NETCONN_TCP);
    if (!c)
        return -1;

    err_t err = netconn_connect(c, &addr, ctx->agent_port);
    if (err != ERR_OK)
    {
        netconn_delete(c);
        return -1;
    }

    cJSON *req_json = cJSON_CreateObject();
    cJSON_AddStringToObject(req_json, "path", ctx->remote_path);
    cJSON_AddStringToObject(req_json, "data", b64_chunk);
    if (ctx->file_mode > 0)
        cJSON_AddNumberToObject(req_json, "mode", ctx->file_mode);
    if (offset > 0)
        cJSON_AddNumberToObject(req_json, "offset", (double)offset);
    char *req_body = cJSON_PrintUnformatted(req_json);
    cJSON_Delete(req_json);

    char hdr[512];
    int hdr_len = snprintf(hdr, sizeof(hdr),
                           "POST /file/upload HTTP/1.1\r\n"
                           "Host: %s:%u\r\n"
                           "Content-Type: application/json\r\n"
                           "Content-Length: %zu\r\n"
                           "Connection: close\r\n"
                           "\r\n",
                           ctx->agent_ip, ctx->agent_port, strlen(req_body));

    netconn_write_partly(c, hdr, (size_t)hdr_len, NETCONN_COPY, NULL);
    netconn_write_partly(c, req_body, strlen(req_body), NETCONN_COPY, NULL);
    free(req_body);

    netconn_set_recvtimeout(c, ctx->timeout_sec * 1000);

    GString *resp_buf = g_string_new(NULL);
    err_t recv_err = recv_until_close(c, resp_buf);
    netconn_close(c);
    netconn_delete(c);

    if (recv_err == ERR_TIMEOUT)
    {
        g_string_free(resp_buf, TRUE);
        return -1;
    }

    cJSON *resp_json = NULL;
    GError *error = NULL;
    int ret = parse_json_response(resp_buf, &resp_json, &error);
    g_string_free(resp_buf, TRUE);
    if (ret != 0)
    {
        if (error)
            g_error_free(error);
        return -1;
    }

    cJSON_Delete(resp_json);
    return 0;
}

static bool do_device_upload(struct device_file_ctx *ctx,
                                 int (*do_chunk)(struct device_file_ctx *, const char *, size_t, size_t))
{
    struct stat st;
    if (stat(ctx->local_path, &st) != 0 || !S_ISREG(st.st_mode))
    {
        device_file_send_error(ctx, "device_upload: cannot stat local file '%s'", ctx->local_path);
        return FALSE;
    }

    int fd = open(ctx->local_path, O_RDONLY);
    if (fd < 0)
    {
        device_file_send_error(ctx, "device_upload: cannot open local file '%s': %s",
                               ctx->local_path, strerror(errno));
        return FALSE;
    }

    size_t raw_len = (size_t)st.st_size;
    size_t chunk_buf_size = DEVICE_FILE_CHUNK_SIZE;
    char *chunk_buf = g_malloc(chunk_buf_size);
    size_t offset = 0;
    int chunk_idx = 0;
    GString *final_result = g_string_new(NULL);

    while (offset < raw_len)
    {
        size_t this_len = raw_len - offset;
        if (this_len > chunk_buf_size)
            this_len = chunk_buf_size;

        ssize_t n = read(fd, chunk_buf, this_len);
        if (n <= 0 || (size_t)n != this_len)
        {
            device_file_send_error(ctx, "device_upload: read failed at offset %zu", offset);
            free(chunk_buf);
            g_string_free(final_result, TRUE);
            close(fd);
            return FALSE;
        }

        char *b64 = g_base64_encode((const unsigned char *)chunk_buf, this_len);
        if (!b64)
        {
            device_file_send_error(ctx, "device_upload: base64 encode failed at offset %zu", offset);
            free(chunk_buf);
            g_string_free(final_result, TRUE);
            close(fd);
            return FALSE;
        }

        log_debug("device_upload: chunk %d offset=%zu len=%zu b64_len=%zu",
                  chunk_idx, offset, this_len, strlen(b64));

        int ret = do_chunk(ctx, b64, this_len, offset);
        free(b64);

        if (ret != 0)
        {
            device_file_send_error(ctx, "device_upload: chunk %d failed at offset %zu", chunk_idx, offset);
            free(chunk_buf);
            g_string_free(final_result, TRUE);
            close(fd);
            return FALSE;
        }

        offset += this_len;
        chunk_idx++;
    }

    free(chunk_buf);
    close(fd);

    g_string_printf(final_result, "Uploaded %zu bytes in %d chunk(s) to %s",
                    raw_len, chunk_idx, ctx->remote_path);

    ctx->callback(ctx->tb_ctx, final_result, NULL);
    return TRUE;
}

static int do_download_chunk_socket(struct device_file_ctx *ctx, size_t offset,
                                    size_t *out_total_size, size_t *out_chunk_len, int fd)
{
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
        return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(ctx->agent_port);
    if (inet_pton(AF_INET, ctx->agent_ip, &addr.sin_addr) != 1)
    {
        close(sock);
        return -1;
    }

    struct timeval tv = {.tv_sec = 5, .tv_usec = 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        close(sock);
        return -1;
    }

    cJSON *req_json = cJSON_CreateObject();
    cJSON_AddStringToObject(req_json, "path", ctx->remote_path);
    cJSON_AddNumberToObject(req_json, "offset", (double)offset);
    cJSON_AddNumberToObject(req_json, "length", (double)DEVICE_FILE_CHUNK_SIZE);
    char *req_body = cJSON_PrintUnformatted(req_json);
    cJSON_Delete(req_json);

    char hdr[512];
    int hdr_len = snprintf(hdr, sizeof(hdr),
                           "POST /file/download HTTP/1.1\r\n"
                           "Host: %s:%u\r\n"
                           "Content-Type: application/json\r\n"
                           "Content-Length: %zu\r\n"
                           "Connection: close\r\n"
                           "\r\n",
                           ctx->agent_ip, ctx->agent_port, strlen(req_body));

    send_all(sock, hdr, (size_t)hdr_len);
    send_all(sock, req_body, strlen(req_body));
    free(req_body);

    tv.tv_sec = ctx->timeout_sec;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    GString *resp_buf = g_string_new(NULL);
    int recv_ret = recv_until_close_socket(sock, resp_buf);
    close(sock);

    if (recv_ret != 0)
    {
        g_string_free(resp_buf, TRUE);
        return -1;
    }

    cJSON *resp_json = NULL;
    GError *error = NULL;
    if (parse_json_response(resp_buf, &resp_json, &error) != 0)
    {
        g_string_free(resp_buf, TRUE);
        if (error)
            g_error_free(error);
        return -1;
    }
    g_string_free(resp_buf, TRUE);

    cJSON *size_item = cJSON_GetObjectItem(resp_json, "size");
    cJSON *data_item = cJSON_GetObjectItem(resp_json, "data");
    cJSON *length_item = cJSON_GetObjectItem(resp_json, "length");

    size_t total_size = size_item ? (size_t)size_item->valuedouble : 0;
    size_t chunk_len = length_item ? (size_t)length_item->valuedouble : 0;

    if (data_item && cJSON_IsString(data_item) && chunk_len > 0)
    {
        size_t decoded_len = 0;
        char *decoded = (char *)g_base64_decode(data_item->valuestring, &decoded_len);
        if (decoded)
        {
            if (write(fd, decoded, decoded_len) != (ssize_t)decoded_len)
            {
                free(decoded);
                cJSON_Delete(resp_json);
                return -1;
            }
            if (out_chunk_len)
                *out_chunk_len = decoded_len;
            free(decoded);
        }
    }

    if (out_total_size)
        *out_total_size = total_size;

    cJSON_Delete(resp_json);

    return 0;
}

static int do_download_chunk_lwip(struct device_file_ctx *ctx, size_t offset,
                                  size_t *out_total_size, size_t *out_chunk_len, int fd)
{
    ip_addr_t addr;
    if (!ip4addr_aton(ctx->agent_ip, &addr))
        return -1;

    struct netconn *c = netconn_new(NETCONN_TCP);
    if (!c)
        return -1;

    err_t err = netconn_connect(c, &addr, ctx->agent_port);
    if (err != ERR_OK)
    {
        netconn_delete(c);
        return -1;
    }

    cJSON *req_json = cJSON_CreateObject();
    cJSON_AddStringToObject(req_json, "path", ctx->remote_path);
    cJSON_AddNumberToObject(req_json, "offset", (double)offset);
    cJSON_AddNumberToObject(req_json, "length", (double)DEVICE_FILE_CHUNK_SIZE);
    char *req_body = cJSON_PrintUnformatted(req_json);
    cJSON_Delete(req_json);

    char hdr[512];
    int hdr_len = snprintf(hdr, sizeof(hdr),
                           "POST /file/download HTTP/1.1\r\n"
                           "Host: %s:%u\r\n"
                           "Content-Type: application/json\r\n"
                           "Content-Length: %zu\r\n"
                           "Connection: close\r\n"
                           "\r\n",
                           ctx->agent_ip, ctx->agent_port, strlen(req_body));

    netconn_write_partly(c, hdr, (size_t)hdr_len, NETCONN_COPY, NULL);
    netconn_write_partly(c, req_body, strlen(req_body), NETCONN_COPY, NULL);
    free(req_body);

    netconn_set_recvtimeout(c, ctx->timeout_sec * 1000);

    GString *resp_buf = g_string_new(NULL);
    err_t recv_err = recv_until_close(c, resp_buf);
    netconn_close(c);
    netconn_delete(c);

    if (recv_err == ERR_TIMEOUT)
    {
        g_string_free(resp_buf, TRUE);
        return -1;
    }

    cJSON *resp_json = NULL;
    GError *error = NULL;
    if (parse_json_response(resp_buf, &resp_json, &error) != 0)
    {
        g_string_free(resp_buf, TRUE);
        if (error)
            g_error_free(error);
        return -1;
    }
    g_string_free(resp_buf, TRUE);

    cJSON *size_item = cJSON_GetObjectItem(resp_json, "size");
    cJSON *data_item = cJSON_GetObjectItem(resp_json, "data");
    cJSON *length_item = cJSON_GetObjectItem(resp_json, "length");

    size_t total_size = size_item ? (size_t)size_item->valuedouble : 0;
    size_t chunk_len = length_item ? (size_t)length_item->valuedouble : 0;

    if (data_item && cJSON_IsString(data_item) && chunk_len > 0)
    {
        size_t decoded_len = 0;
        char *decoded = (char *)g_base64_decode(data_item->valuestring, &decoded_len);
        if (decoded)
        {
            if (write(fd, decoded, decoded_len) != (ssize_t)decoded_len)
            {
                free(decoded);
                cJSON_Delete(resp_json);
                return -1;
            }
            if (out_chunk_len)
                *out_chunk_len = decoded_len;
            free(decoded);
        }
    }

    if (out_total_size)
        *out_total_size = total_size;

    cJSON_Delete(resp_json);

    return 0;
}

static bool do_device_download(struct device_file_ctx *ctx,
                                   int (*do_chunk)(struct device_file_ctx *, size_t, size_t *, size_t *, int))
{
    int fd = open(ctx->local_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
    {
        device_file_send_error(ctx, "device_download: cannot open local file '%s': %s",
                               ctx->local_path, strerror(errno));
        return FALSE;
    }

    size_t total_size = 0;
    size_t received = 0;

    while (1)
    {
        size_t chunk_total = 0;
        size_t chunk_len = 0;
        int ret = do_chunk(ctx, received, &chunk_total, &chunk_len, fd);
        if (ret != 0)
        {
            device_file_send_error(ctx, "device_download: chunk at offset %zu failed", received);
            close(fd);
            unlink(ctx->local_path);
            return FALSE;
        }

        if (received == 0)
            total_size = chunk_total;

        received += chunk_len;

        if (total_size == 0 || received >= total_size)
            break;
    }

    close(fd);

    GString *result = g_string_new(NULL);
    g_string_printf(result, "Downloaded %zu bytes to %s", received, ctx->local_path);
    ctx->callback(ctx->tb_ctx, result, NULL);
    return TRUE;
}

static void device_file_thread_socket(struct device_file_ctx *ctx)
{
    if (ctx->is_upload)
        do_device_upload(ctx, do_upload_chunk_socket);
    else
        do_device_download(ctx, do_download_chunk_socket);

    device_file_ctx_free(ctx);
}

static void device_file_thread_lwip(struct device_file_ctx *ctx)
{
    if (ctx->is_upload)
        do_device_upload(ctx, do_upload_chunk_lwip);
    else
        do_device_download(ctx, do_download_chunk_lwip);

    device_file_ctx_free(ctx);
}

static void device_file_thread(void* data, void* user_data)
{
    struct device_file_ctx *ctx = data;
    (void)user_data;

    struct edc_config *conf = ctx->tb_ctx->tb->conf;

    if (conf->device_transport && conf->device_transport[0])
    {
        if (g_ascii_strcasecmp(conf->device_transport, "tty") == 0)
            device_file_thread_lwip(ctx);
        else
            device_file_thread_socket(ctx);
    }
    else if (conf->device_pty && conf->device_pty[0])
    {
        device_file_thread_lwip(ctx);
    }
    else
    {
        device_file_thread_socket(ctx);
    }
}

static void edc_tool_device_upload_exec_async(struct edc_toolbox_call_ctx *tb_ctx, struct edc_tool *tool, cJSON *args,
                                              void (*callback)(struct edc_toolbox_call_ctx *, GString *, GError *))
{
    (void)tool;

    cJSON *local = cJSON_GetObjectItem(args, "local_path");
    cJSON *remote = cJSON_GetObjectItem(args, "remote_path");
    if (!local || !cJSON_IsString(local) || !remote || !cJSON_IsString(remote))
    {
        GError *error = g_error_new(BC_TOOL_ERROR, BC_TOOL_ARG_INVALID,
                                    "device_upload: missing 'local_path' or 'remote_path' parameter");
        callback(tb_ctx, NULL, error);
        return;
    }

    struct edc_config *conf = tb_ctx->tb->conf;
    if (!conf->device_ip)
    {
        GError *error = g_error_new(BC_TOOL_ERROR, BC_TOOL_ARG_INVALID,
                                    "device_upload: device_ip not configured");
        callback(tb_ctx, NULL, error);
        return;
    }

    int file_mode = 0644;
    cJSON *mode_item = cJSON_GetObjectItem(args, "mode");
    if (mode_item && cJSON_IsNumber(mode_item) && mode_item->valueint > 0)
        file_mode = mode_item->valueint;

    int timeout_sec = 60;
    cJSON *timeout_json = cJSON_GetObjectItem(args, "timeout");
    if (timeout_json && cJSON_IsNumber(timeout_json) && timeout_json->valueint > 0)
        timeout_sec = timeout_json->valueint;

    if (!g_file_test(cJSON_GetStringValue(local), G_FILE_TEST_IS_REGULAR))
    {
        GError *error = g_error_new(BC_TOOL_ERROR, BC_TOOL_EXEC_ERROR,
                                    "device_upload: local file not found: '%s'", cJSON_GetStringValue(local));
        callback(tb_ctx, NULL, error);
        return;
    }

    struct device_file_ctx *ctx = g_new0(struct device_file_ctx, 1);
    ctx->tb_ctx = tb_ctx;
    ctx->callback = callback;
    ctx->agent_ip = strdup(conf->device_ip);
    ctx->agent_port = (uint16_t)(conf->device_port > 0 ? conf->device_port : 5000);
    ctx->remote_path = strdup(cJSON_GetStringValue(remote));
    ctx->local_path = strdup(cJSON_GetStringValue(local));
    ctx->file_mode = file_mode;
    ctx->is_upload = TRUE;
    ctx->timeout_sec = timeout_sec;

    g_thread_new("device_upload", (GThreadFunc)device_file_thread, ctx);
}

static void edc_tool_device_download_exec_async(struct edc_toolbox_call_ctx *tb_ctx, struct edc_tool *tool, cJSON *args,
                                                void (*callback)(struct edc_toolbox_call_ctx *, GString *, GError *))
{
    (void)tool;

    cJSON *remote = cJSON_GetObjectItem(args, "remote_path");
    cJSON *local = cJSON_GetObjectItem(args, "local_path");
    if (!remote || !cJSON_IsString(remote) || !local || !cJSON_IsString(local))
    {
        GError *error = g_error_new(BC_TOOL_ERROR, BC_TOOL_ARG_INVALID,
                                    "device_download: missing 'remote_path' or 'local_path' parameter");
        callback(tb_ctx, NULL, error);
        return;
    }

    struct edc_config *conf = tb_ctx->tb->conf;
    if (!conf->device_ip)
    {
        GError *error = g_error_new(BC_TOOL_ERROR, BC_TOOL_ARG_INVALID,
                                    "device_download: device_ip not configured");
        callback(tb_ctx, NULL, error);
        return;
    }

    int timeout_sec = 60;
    cJSON *timeout_json = cJSON_GetObjectItem(args, "timeout");
    if (timeout_json && cJSON_IsNumber(timeout_json) && timeout_json->valueint > 0)
        timeout_sec = timeout_json->valueint;

    struct device_file_ctx *ctx = g_new0(struct device_file_ctx, 1);
    ctx->tb_ctx = tb_ctx;
    ctx->callback = callback;
    ctx->agent_ip = strdup(conf->device_ip);
    ctx->agent_port = (uint16_t)(conf->device_port > 0 ? conf->device_port : 5000);
    ctx->remote_path = strdup(cJSON_GetStringValue(remote));
    ctx->local_path = strdup(cJSON_GetStringValue(local));
    ctx->is_upload = FALSE;
    ctx->timeout_sec = timeout_sec;

    g_thread_new("device_download", (GThreadFunc)device_file_thread, ctx);
}

static struct edc_tool *edc_tool_create_device_upload(struct edc_toolbox *tb)
{
    GSList *params = NULL;
    params = g_slist_append(params,
        edc_tool_param_new("local_path", edc_param_str_type("Local file path on host to upload", NULL), TRUE));
    params = g_slist_append(params,
        edc_tool_param_new("remote_path", edc_param_str_type("Remote file path on device destination", NULL), TRUE));
    params = g_slist_append(params,
        edc_tool_param_new("mode", edc_param_int_type("File permission mode (default 0644)", 0, 07777), FALSE));
    params = g_slist_append(params,
        edc_tool_param_new("timeout", edc_param_int_type("Timeout per chunk in seconds (default 60)", 1, 3600), FALSE));

    struct edc_tool *tool = edc_tool_new("device_upload",
        "Upload a file from the host to a remote device agent (supports chunked transfer for large files)",
        params, edc_tool_device_upload_exec_async);
    edc_toolbox_register(tb, tool);
    return tool;
}

static struct edc_tool *edc_tool_create_device_download(struct edc_toolbox *tb)
{
    GSList *params = NULL;
    params = g_slist_append(params,
        edc_tool_param_new("remote_path", edc_param_str_type("Remote file path on device to download", NULL), TRUE));
    params = g_slist_append(params,
        edc_tool_param_new("local_path", edc_param_str_type("Local destination file path on host", NULL), TRUE));
    params = g_slist_append(params,
        edc_tool_param_new("timeout", edc_param_int_type("Timeout per chunk in seconds (default 60)", 1, 3600), FALSE));

    struct edc_tool *tool = edc_tool_new("device_download",
        "Download a file from a remote device agent to the host (supports chunked transfer for large files)",
        params, edc_tool_device_download_exec_async);
    edc_toolbox_register(tb, tool);
    return tool;
}

void edc_tools_device_file_register(struct edc_toolbox *tb)
{
    edc_tool_create_device_upload(tb);
    edc_tool_create_device_download(tb);
}
