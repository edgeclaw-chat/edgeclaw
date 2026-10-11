#include "edc_tools_device_file.h"
#include "edc_config.h"
#include "edc_toolbox.h"
#include "edc_tools.h"
#include "log.h"
#include <edc_param_types.h>

#ifndef _WIN32
#include "lwip/api.h"
#include "lwip/err.h"
#include "lwip/ip_addr.h"
#endif
#include "edc_compat.h"
#include "edc_error.h"
#include "edc_file_utils.h"
#include "edc_string_utils.h"
#include "sds.h"
#include <cJSON.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <uv.h>

#define DEVICE_FILE_CHUNK_SIZE (256u * 1024u)

/* Windows ANSI CRT APIs (stat/open) interpret paths using the local code page,
 * which mangles non-ASCII (e.g. Chinese) local file names. Use wide-char
 * equivalents via edc_utf8_to_wide() from edc_compat.h. On POSIX these
 * resolve directly to stat()/open(). */
#ifdef _WIN32
static int edc_win_stat_utf8(const char *path, struct stat *st)
{
    wchar_t *wpath = edc_utf8_to_wide(path);
    if (!wpath)
    {
        errno = EINVAL;
        return -1;
    }
    struct _stat64 s64;
    int ret = _wstat64(wpath, &s64);
    free(wpath);
    if (ret == 0 && st)
    {
        memset(st, 0, sizeof(*st));
        st->st_size = (off_t)s64.st_size;
        st->st_mode = (mode_t)s64.st_mode;
        st->st_mtime = (time_t)s64.st_mtime;
    }
    return ret;
}

static int edc_win_open_utf8(const char *path, int flags, int mode)
{
    wchar_t *wpath = edc_utf8_to_wide(path);
    if (!wpath)
    {
        errno = EINVAL;
        return -1;
    }
    int fd = _wopen(wpath, flags, mode);
    free(wpath);
    return fd;
}
#else
#define edc_win_stat_utf8 stat
#define edc_win_open_utf8 open
#ifndef O_BINARY
#define O_BINARY 0
#endif
#endif

struct device_file_ctx
{
    struct edc_toolbox_call_ctx *tb_ctx;
    void (*callback)(struct edc_toolbox_call_ctx *, sds, EdcError *);
    uv_async_t result_async; /* async handle to marshal result back to main loop */
    sds result;              /* result from worker thread (consumed by main-loop callback) */
    EdcError *error;         /* error from worker thread (consumed by main-loop callback) */
    sds agent_ip;
    uint16_t agent_port;

    sds remote_path;
    sds local_path;
    sds file_data;
    size_t file_data_len;
    int file_mode;

    bool is_upload;
    int timeout_sec;

    EdcError *last_error;
};

static void device_file_ctx_free(struct device_file_ctx *ctx)
{
    sdsfree(ctx->agent_ip);
    sdsfree(ctx->remote_path);
    sdsfree(ctx->local_path);
    sdsfree(ctx->file_data);
    if (ctx->last_error)
        edc_error_free(ctx->last_error);
    free(ctx);
}

/*
 * device_file_async_close_cb - Callback invoked when the uv_async_t handle
 * has been fully closed. Frees the device_file_ctx.
 */
static void device_file_async_close_cb(uv_handle_t *handle)
{
    struct device_file_ctx *ctx = handle->data;
    device_file_ctx_free(ctx);
}

/*
 * device_file_on_async_result - Callback invoked on the MAIN event loop
 * thread when the worker thread signals completion via uv_async_send().
 * This is the ONLY place the original callback is invoked.
 */
static void device_file_on_async_result(uv_async_t *handle)
{
    struct device_file_ctx *ctx = handle->data;

    /* Surface the result in the originating channel before handing it back. */
    const char *content = ctx->result ? ctx->result : (ctx->error && ctx->error->message ? ctx->error->message : NULL);
    sds header = sdscatprintf(sdsempty(), "device_%s %s", ctx->is_upload ? "upload" : "download",
                              ctx->local_path ? ctx->local_path : "");
    edc_toolbox_emit_tool_message_plain(ctx->tb_ctx, header, content);
    sdsfree(header);

    /* Invoke the original callback on the main loop thread */
    ctx->callback(ctx->tb_ctx, ctx->result, ctx->error);

    /* Close the async handle; ctx is freed in the close callback */
    uv_close((uv_handle_t *)handle, device_file_async_close_cb);
}

/*
 * device_file_deliver_result - Called from the worker thread to safely
 * marshal the result back to the main loop via uv_async_send().
 * uv_async_send() is thread-safe.
 */
static void device_file_deliver_result(struct device_file_ctx *ctx, sds result, EdcError *error)
{
    ctx->result = result;
    ctx->error = error;
    uv_async_send(&ctx->result_async);
}

#ifndef _WIN32
static err_t recv_until_close(struct netconn *c, sds *buf)
{
    struct netbuf *nb;
    u8_t *p;
    u16_t chunk;
    err_t err;

    while ((err = netconn_recv(c, &nb)) == ERR_OK)
    {
        netbuf_data(nb, (void **)&p, &chunk);
        *buf = sdscatlen(*buf, (char *)p, chunk);
        netbuf_delete(nb);
    }
    return err;
}
#endif

static void device_file_send_error(struct device_file_ctx *ctx, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    sds msg = edc_str_vprintf(fmt, ap);
    va_end(ap);

    EdcError *error = edc_error_new(EDC_ERR_EXEC, "%s", msg);
    device_file_deliver_result(ctx, NULL, error);
    sdsfree(msg);
}

static int parse_json_response(sds resp_buf, cJSON **out_json, EdcError **error)
{
    char *body = strstr(resp_buf, "\r\n\r\n");
    if (!body)
    {
        *error = edc_error_new(EDC_ERR_EXEC, "device_file: invalid HTTP response");
        log_error("DEBUG parse_json_response: invalid HTTP response, resp_buf len=%zu", sdslen(resp_buf));
        return -1;
    }
    body += 4;

    int http_status = 0;
    if (strncasecmp(resp_buf, "HTTP/", 5) == 0)
    {
        const char *sp = strchr(resp_buf, ' ');
        if (sp)
            http_status = atoi(sp + 1);
    }

    log_error("DEBUG parse_json_response: http_status=%d, body=%.100s", http_status, body);

    /* Try JSON parse first (device agent returns JSON error even on non-200) */
    *out_json = cJSON_Parse(body);
    if (*out_json)
    {
        if (http_status == 200)
        {
            cJSON *error_item = cJSON_GetObjectItem(*out_json, "error");
            if (error_item && cJSON_IsString(error_item))
            {
                *error = edc_error_new(EDC_ERR_EXEC, "device_file: %s", error_item->valuestring);
                log_error("DEBUG parse_json_response: created error, message='%s', ptr=%p", (*error)->message,
                          (void *)(*error));
                cJSON_Delete(*out_json);
                *out_json = NULL;
                return -1;
            }
            return 0;
        }

        cJSON *error_item = cJSON_GetObjectItem(*out_json, "error");
        if (error_item && cJSON_IsString(error_item))
            *error = edc_error_new(EDC_ERR_EXEC, "device_file: HTTP %d: %s", http_status, error_item->valuestring);
        else
            *error = edc_error_new(EDC_ERR_EXEC, "device_file: HTTP %d", http_status);
        cJSON_Delete(*out_json);
        *out_json = NULL;
        return -1;
    }

    /* Non-JSON body: include text in error message */
    char *trimmed = body;
    while (*trimmed == ' ' || *trimmed == '\r' || *trimmed == '\n')
        trimmed++;
    if (http_status != 200)
    {
        if (*trimmed)
            *error = edc_error_new(EDC_ERR_EXEC, "device_file: HTTP %d: %.200s", http_status, trimmed);
        else
            *error = edc_error_new(EDC_ERR_EXEC, "device_file: HTTP %d", http_status);
    }
    else
    {
        if (*trimmed)
            *error = edc_error_new(EDC_ERR_EXEC, "device_file: invalid JSON response: %.200s", trimmed);
        else
            *error = edc_error_new(EDC_ERR_EXEC, "device_file: invalid JSON response (empty body)");
    }
    return -1;
}

struct device_file_uv_state
{
    uv_loop_t loop;
    uv_tcp_t tcp;
    uv_connect_t connect_req;
    uv_write_t write_req;
    uv_timer_t timeout_timer;

    sds resp_buf;
    sds header;
    sds body_sds;

    int timeout_sec;
    bool timed_out;
    EdcError *error;
};

static void device_file_uv_close_cb(uv_handle_t *handle)
{
    (void)handle;
}

static void device_file_uv_alloc(uv_handle_t *handle, size_t suggested, uv_buf_t *buf)
{
    (void)handle;
    size_t len = suggested > 0 ? suggested : 4096;
    buf->base = malloc(len);
    buf->len = buf->base ? len : 0;
}

static void device_file_uv_on_timeout(uv_timer_t *timer)
{
    struct device_file_uv_state *state = timer->data;
    if (state->error || state->timed_out)
        return;
    state->timed_out = true;
    if (!uv_is_closing((uv_handle_t *)&state->tcp))
        uv_close((uv_handle_t *)&state->tcp, device_file_uv_close_cb);
}

static void device_file_uv_on_read(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf)
{
    struct device_file_uv_state *state = stream->data;

    if (state->error || state->timed_out)
    {
        free(buf->base);
        return;
    }

    if (nread > 0)
    {
        state->resp_buf = sdscatlen(state->resp_buf, buf->base, nread);
        free(buf->base);
        return;
    }

    free(buf->base);
    uv_timer_stop(&state->timeout_timer);
    uv_close((uv_handle_t *)stream, device_file_uv_close_cb);

    if (nread < 0 && nread != UV_EOF)
    {
        state->error = edc_error_new(EDC_ERR_EXEC, "device_file: recv failed: %s", uv_strerror((int)nread));
    }

    uv_stop(&state->loop);
}

static void device_file_uv_on_write(uv_write_t *req, int status)
{
    struct device_file_uv_state *state = req->data;

    if (state->error || state->timed_out)
        return;

    if (status < 0)
    {
        state->error = edc_error_new(EDC_ERR_EXEC, "device_file: write failed: %s", uv_strerror(status));
        uv_timer_stop(&state->timeout_timer);
        uv_close((uv_handle_t *)&state->tcp, device_file_uv_close_cb);
        uv_stop(&state->loop);
        return;
    }

    int r = uv_read_start((uv_stream_t *)&state->tcp, device_file_uv_alloc, device_file_uv_on_read);
    if (r < 0)
    {
        state->error = edc_error_new(EDC_ERR_EXEC, "device_file: read start failed: %s", uv_strerror(r));
        uv_timer_stop(&state->timeout_timer);
        uv_close((uv_handle_t *)&state->tcp, device_file_uv_close_cb);
        uv_stop(&state->loop);
    }
}

static void device_file_uv_on_connect(uv_connect_t *req, int status)
{
    struct device_file_uv_state *state = req->data;

    if (state->error || state->timed_out)
        return;

    if (status < 0)
    {
        state->error = edc_error_new(EDC_ERR_EXEC, "device_file: connect failed: %s", uv_strerror(status));
        uv_timer_stop(&state->timeout_timer);
        uv_close((uv_handle_t *)&state->tcp, device_file_uv_close_cb);
        uv_stop(&state->loop);
        return;
    }

    uv_timer_stop(&state->timeout_timer);
    uv_timer_start(&state->timeout_timer, device_file_uv_on_timeout, (uint64_t)state->timeout_sec * 1000, 0);

    uv_buf_t bufs[2];
    bufs[0].base = state->header;
    bufs[0].len = sdslen(state->header);
    bufs[1].base = state->body_sds;
    bufs[1].len = sdslen(state->body_sds);

    int r = uv_write(&state->write_req, (uv_stream_t *)&state->tcp, bufs, 2, device_file_uv_on_write);
    if (r < 0)
    {
        state->error = edc_error_new(EDC_ERR_EXEC, "device_file: write failed: %s", uv_strerror(r));
        uv_timer_stop(&state->timeout_timer);
        uv_close((uv_handle_t *)&state->tcp, device_file_uv_close_cb);
        uv_stop(&state->loop);
    }
}

static void device_file_uv_close_walk(uv_handle_t *handle, void *arg)
{
    (void)arg;
    if (!uv_is_closing(handle))
        uv_close(handle, device_file_uv_close_cb);
}

static sds device_file_uv_send_recv(const char *agent_ip, uint16_t agent_port, const char *path, cJSON *req_json,
                                    int timeout_sec, EdcError **out_error)
{
    char *req_body = cJSON_PrintUnformatted(req_json);

    sds header = sdscatprintf(sdsempty(),
                              "POST %s HTTP/1.1\r\n"
                              "Host: %s:%u\r\n"
                              "Content-Type: application/json\r\n"
                              "Content-Length: %zu\r\n"
                              "Connection: close\r\n"
                              "\r\n",
                              path, agent_ip, agent_port, strlen(req_body));

    struct device_file_uv_state *state = calloc(1, sizeof(*state));
    if (!state)
    {
        sdsfree(header);
        cJSON_free(req_body);
        *out_error = edc_error_new(EDC_ERR_EXEC, "device_file: failed to allocate state");
        return NULL;
    }

    state->resp_buf = sdsempty();
    state->header = header;
    state->body_sds = sdsnewlen(req_body, strlen(req_body));
    state->timeout_sec = timeout_sec;
    cJSON_free(req_body);

    state->tcp.data = state;
    state->connect_req.data = state;
    state->write_req.data = state;
    state->timeout_timer.data = state;

    uv_loop_init(&state->loop);
    uv_tcp_init(&state->loop, &state->tcp);
    uv_timer_init(&state->loop, &state->timeout_timer);

    struct sockaddr_in addr;
    uv_ip4_addr(agent_ip, agent_port, &addr);

    uv_timer_start(&state->timeout_timer, device_file_uv_on_timeout, 5000, 0);

    int r = uv_tcp_connect(&state->connect_req, &state->tcp, (const struct sockaddr *)&addr, device_file_uv_on_connect);
    if (r < 0)
    {
        *out_error = edc_error_new(EDC_ERR_EXEC, "device_file: connect failed: %s", uv_strerror(r));
        uv_timer_stop(&state->timeout_timer);
        uv_close((uv_handle_t *)&state->tcp, device_file_uv_close_cb);
        uv_close((uv_handle_t *)&state->timeout_timer, device_file_uv_close_cb);
        uv_run(&state->loop, UV_RUN_DEFAULT);
        uv_loop_close(&state->loop);
        sdsfree(state->header);
        sdsfree(state->body_sds);
        sdsfree(state->resp_buf);
        free(state);
        return NULL;
    }

    uv_run(&state->loop, UV_RUN_DEFAULT);

    uv_walk(&state->loop, device_file_uv_close_walk, NULL);
    uv_run(&state->loop, UV_RUN_DEFAULT);

    sds resp = state->resp_buf;
    EdcError *err = state->error;

    if (state->timed_out)
    {
        sdsfree(resp);
        resp = NULL;
        *out_error = edc_error_new(EDC_ERR_EXEC, "device_file: request timed out after %ds", timeout_sec);
    }
    else if (err)
    {
        sdsfree(resp);
        resp = NULL;
        *out_error = err;
    }

    sdsfree(state->header);
    sdsfree(state->body_sds);
    uv_loop_close(&state->loop);
    free(state);

    return resp;
}

static int do_upload_chunk_socket(struct device_file_ctx *ctx, const char *b64_chunk, size_t raw_chunk_len,
                                  size_t offset)
{
    cJSON *req_json = cJSON_CreateObject();
    cJSON_AddStringToObject(req_json, "path", ctx->remote_path);
    cJSON_AddStringToObject(req_json, "data", b64_chunk);
    if (ctx->file_mode > 0)
        cJSON_AddNumberToObject(req_json, "mode", ctx->file_mode);
    if (offset > 0)
        cJSON_AddNumberToObject(req_json, "offset", (double)offset);

    EdcError *error = NULL;
    sds resp_buf =
        device_file_uv_send_recv(ctx->agent_ip, ctx->agent_port, "/file/upload", req_json, ctx->timeout_sec, &error);
    cJSON_Delete(req_json);

    if (!resp_buf)
    {
        if (error)
        {
            log_error("device_upload: %s", error->message);
            if (ctx->last_error)
                edc_error_free(ctx->last_error);
            ctx->last_error = error;
        }
        return -1;
    }

    cJSON *resp_json = NULL;
    int ret = parse_json_response(resp_buf, &resp_json, &error);
    sdsfree(resp_buf);
    if (ret != 0)
    {
        log_error("device_upload: parse_json_response failed");
        if (error)
        {
            log_error("device_upload: %s", error->message);
            if (ctx->last_error)
                edc_error_free(ctx->last_error);
            ctx->last_error = error;
        }
        return -1;
    }

    cJSON_Delete(resp_json);
    return 0;
}

#ifndef _WIN32
static int do_upload_chunk_lwip(struct device_file_ctx *ctx, const char *b64_chunk, size_t raw_chunk_len, size_t offset)
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

    sds hdr = sdscatprintf(sdsempty(),
                           "POST /file/upload HTTP/1.1\r\n"
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

    if (recv_err == ERR_TIMEOUT)
    {
        sdsfree(resp_buf);
        return -1;
    }

    cJSON *resp_json = NULL;
    EdcError *error = NULL;
    int ret = parse_json_response(resp_buf, &resp_json, &error);
    sdsfree(resp_buf);
    if (ret != 0)
    {
        if (error)
        {
            if (ctx->last_error)
                edc_error_free(ctx->last_error);
            ctx->last_error = error;
        }
        return -1;
    }

    cJSON_Delete(resp_json);
    return 0;
}
#endif

static bool do_device_upload(struct device_file_ctx *ctx,
                             int (*do_chunk)(struct device_file_ctx *, const char *, size_t, size_t))
{
    struct stat st;
    if (edc_win_stat_utf8(ctx->local_path, &st) != 0 || !S_ISREG(st.st_mode))
    {
        device_file_send_error(ctx, "device_upload: cannot stat local file '%s'", ctx->local_path);
        return false;
    }

    int fd = edc_win_open_utf8(ctx->local_path, O_RDONLY | O_BINARY, 0);
    if (fd < 0)
    {
        device_file_send_error(ctx, "device_upload: cannot open local file '%s': %s", ctx->local_path, strerror(errno));
        return false;
    }

    size_t raw_len = (size_t)st.st_size;
    size_t chunk_buf_size = DEVICE_FILE_CHUNK_SIZE;
    char *chunk_buf = malloc(chunk_buf_size);
    size_t offset = 0;
    int chunk_idx = 0;
    sds final_result = sdsempty();

    /* Handle empty file: send one empty chunk to create the file on device */
    if (raw_len == 0)
    {
        int ret = do_chunk(ctx, "", 0, 0);
        if (ret != 0)
        {
            if (ctx->last_error)
            {
                device_file_send_error(ctx, "device_upload: %s", ctx->last_error->message);
            }
            else
            {
                device_file_send_error(ctx, "device_upload: failed to create empty file '%s'", ctx->remote_path);
            }
            free(chunk_buf);
            close(fd);
            return false;
        }
        final_result = sdscatprintf(final_result, "Uploaded 0 bytes in 1 chunk(s) to %s", ctx->remote_path);
        free(chunk_buf);
        close(fd);
        device_file_deliver_result(ctx, final_result, NULL);
        return true;
    }

    while (offset < raw_len)
    {
        size_t this_len = raw_len - offset;
        if (this_len > chunk_buf_size)
            this_len = chunk_buf_size;

        ssize_t n = read(fd, chunk_buf, this_len);
        if (n <= 0 || (size_t)n != this_len)
        {
            device_file_send_error(ctx, "device_upload: read failed at offset %zu: %s (n=%zd, expected=%zu)", offset,
                                   strerror(errno), n, this_len);
            free(chunk_buf);
            sdsfree(final_result);
            close(fd);
            return false;
        }

        sds b64 = edc_base64_encode((const unsigned char *)chunk_buf, this_len);
        if (!b64)
        {
            device_file_send_error(ctx, "device_upload: base64 encode failed at offset %zu", offset);
            free(chunk_buf);
            sdsfree(final_result);
            close(fd);
            return false;
        }

        log_debug("device_upload: chunk %d offset=%zu len=%zu b64_len=%zu", chunk_idx, offset, this_len, strlen(b64));

        int ret = do_chunk(ctx, b64, this_len, offset);
        sdsfree(b64);

        if (ret != 0)
        {
            if (ctx->last_error)
            {
                device_file_send_error(ctx, "device_upload: %s", ctx->last_error->message);
            }
            else
            {
                device_file_send_error(ctx, "device_upload: chunk %d failed at offset %zu", chunk_idx, offset);
            }
            free(chunk_buf);
            sdsfree(final_result);
            close(fd);
            return false;
        }

        offset += this_len;
        chunk_idx++;
    }

    free(chunk_buf);
    close(fd);

    final_result =
        sdscatprintf(final_result, "Uploaded %zu bytes in %d chunk(s) to %s", raw_len, chunk_idx, ctx->remote_path);

    device_file_deliver_result(ctx, final_result, NULL);
    return true;
}

static int do_download_chunk_socket(struct device_file_ctx *ctx, size_t offset, size_t *out_total_size,
                                    size_t *out_chunk_len, int fd)
{
    cJSON *req_json = cJSON_CreateObject();
    cJSON_AddStringToObject(req_json, "path", ctx->remote_path);
    cJSON_AddNumberToObject(req_json, "offset", (double)offset);
    cJSON_AddNumberToObject(req_json, "length", (double)DEVICE_FILE_CHUNK_SIZE);

    EdcError *error = NULL;
    sds resp_buf =
        device_file_uv_send_recv(ctx->agent_ip, ctx->agent_port, "/file/download", req_json, ctx->timeout_sec, &error);
    cJSON_Delete(req_json);

    if (!resp_buf)
    {
        log_error("device_download: send_recv failed");
        if (error)
        {
            log_error("device_download: %s", error->message);
            if (ctx->last_error)
                edc_error_free(ctx->last_error);
            ctx->last_error = error;
        }
        return -1;
    }

    cJSON *resp_json = NULL;
    if (parse_json_response(resp_buf, &resp_json, &error) != 0)
    {
        log_error("device_download: parse error: %s", error ? error->message : "unknown");
        sdsfree(resp_buf);
        if (error)
        {
            if (ctx->last_error)
                edc_error_free(ctx->last_error);
            ctx->last_error = error;
        }
        return -1;
    }
    sdsfree(resp_buf);

    cJSON *size_item = cJSON_GetObjectItem(resp_json, "size");
    cJSON *data_item = cJSON_GetObjectItem(resp_json, "data");
    cJSON *length_item = cJSON_GetObjectItem(resp_json, "length");

    size_t total_size = size_item ? (size_t)size_item->valuedouble : 0;
    size_t chunk_len = length_item ? (size_t)length_item->valuedouble : 0;

    if (data_item && cJSON_IsString(data_item) && chunk_len > 0)
    {
        size_t decoded_len = 0;
        sds decoded = edc_base64_decode(data_item->valuestring, &decoded_len);
        if (decoded)
        {
            if (write(fd, decoded, decoded_len) != (ssize_t)decoded_len)
            {
                sdsfree(decoded);
                cJSON_Delete(resp_json);
                return -1;
            }
            if (out_chunk_len)
                *out_chunk_len = decoded_len;
            sdsfree(decoded);
        }
    }

    if (out_total_size)
        *out_total_size = total_size;

    cJSON_Delete(resp_json);

    return 0;
}

#ifndef _WIN32
static int do_download_chunk_lwip(struct device_file_ctx *ctx, size_t offset, size_t *out_total_size,
                                  size_t *out_chunk_len, int fd)
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

    sds hdr = sdscatprintf(sdsempty(),
                           "POST /file/download HTTP/1.1\r\n"
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

    if (recv_err == ERR_TIMEOUT)
    {
        sdsfree(resp_buf);
        return -1;
    }

    cJSON *resp_json = NULL;
    EdcError *error = NULL;
    if (parse_json_response(resp_buf, &resp_json, &error) != 0)
    {
        log_error("device_download: parse error: %s", error ? error->message : "unknown");
        sdsfree(resp_buf);
        if (error)
        {
            if (ctx->last_error)
                edc_error_free(ctx->last_error);
            ctx->last_error = error;
        }
        return -1;
    }
    sdsfree(resp_buf);

    cJSON *size_item = cJSON_GetObjectItem(resp_json, "size");
    cJSON *data_item = cJSON_GetObjectItem(resp_json, "data");
    cJSON *length_item = cJSON_GetObjectItem(resp_json, "length");

    size_t total_size = size_item ? (size_t)size_item->valuedouble : 0;
    size_t chunk_len = length_item ? (size_t)length_item->valuedouble : 0;

    if (data_item && cJSON_IsString(data_item) && chunk_len > 0)
    {
        size_t decoded_len = 0;
        sds decoded = edc_base64_decode(data_item->valuestring, &decoded_len);
        if (decoded)
        {
            if (write(fd, decoded, decoded_len) != (ssize_t)decoded_len)
            {
                sdsfree(decoded);
                cJSON_Delete(resp_json);
                return -1;
            }
            if (out_chunk_len)
                *out_chunk_len = decoded_len;
            sdsfree(decoded);
        }
    }

    if (out_total_size)
        *out_total_size = total_size;

    cJSON_Delete(resp_json);

    return 0;
}
#endif

static bool do_device_download(struct device_file_ctx *ctx,
                               int (*do_chunk)(struct device_file_ctx *, size_t, size_t *, size_t *, int))
{
    int fd = edc_win_open_utf8(ctx->local_path, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, 0644);
    if (fd < 0)
    {
        device_file_send_error(ctx, "device_download: cannot open local file '%s': %s", ctx->local_path,
                               strerror(errno));
        return false;
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
            if (ctx->last_error)
            {
                device_file_send_error(ctx, "device_download: %s", ctx->last_error->message);
            }
            else
            {
                device_file_send_error(ctx, "device_download: chunk at offset %zu failed", received);
            }
            close(fd);
            unlink(ctx->local_path);
            return false;
        }

        if (received == 0)
            total_size = chunk_total;

        received += chunk_len;

        if (total_size == 0 || received >= total_size)
            break;
    }

    close(fd);

    sds result = sdsempty();
    result = sdscatprintf(result, "Downloaded %zu bytes to %s", received, ctx->local_path);
    device_file_deliver_result(ctx, result, NULL);
    return true;
}

static void device_file_thread_socket(struct device_file_ctx *ctx)
{
    if (ctx->is_upload)
        do_device_upload(ctx, do_upload_chunk_socket);
    else
        do_device_download(ctx, do_download_chunk_socket);
    /* ctx will be freed by async close callback */
}

#ifndef _WIN32
static void device_file_thread_lwip(struct device_file_ctx *ctx)
{
    if (ctx->is_upload)
        do_device_upload(ctx, do_upload_chunk_lwip);
    else
        do_device_download(ctx, do_download_chunk_lwip);
    /* ctx will be freed by async close callback */
}
#endif

static void device_file_thread(void *data)
{
    struct device_file_ctx *ctx = data;

    struct edc_config *conf = ctx->tb_ctx->tb->conf;

    if (conf->device_transport && conf->device_transport[0])
    {
        if (strcasecmp(conf->device_transport, "tty") == 0)
        {
#ifdef _WIN32
            device_file_thread_socket(ctx);
#else
            device_file_thread_lwip(ctx);
#endif
        }
        else
        {
            device_file_thread_socket(ctx);
        }
    }
    else if (conf->device_pty && conf->device_pty[0])
    {
#ifdef _WIN32
        device_file_thread_socket(ctx);
#else
        device_file_thread_lwip(ctx);
#endif
    }
    else
    {
        device_file_thread_socket(ctx);
    }
}

static void edc_tool_device_upload_exec_async(struct edc_toolbox_call_ctx *tb_ctx, struct edc_tool *tool, cJSON *args,
                                              void (*callback)(struct edc_toolbox_call_ctx *, sds, EdcError *))
{
    (void)tool;

    cJSON *local = cJSON_GetObjectItem(args, "local_path");
    cJSON *remote = cJSON_GetObjectItem(args, "remote_path");
    if (!local || !cJSON_IsString(local) || !remote || !cJSON_IsString(remote))
    {
        EdcError *error =
            edc_error_new(EDC_ERR_INVAL, "device_upload: missing 'local_path' or 'remote_path' parameter");
        callback(tb_ctx, NULL, error);
        return;
    }

    struct edc_config *conf = tb_ctx->tb->conf;
    if (!conf->device_ip)
    {
        EdcError *error = edc_error_new(EDC_ERR_INVAL, "device_upload: device_ip not configured");
        callback(tb_ctx, NULL, error);
        return;
    }

    int file_mode = 0644;
    cJSON *mode_item = cJSON_GetObjectItem(args, "mode");
    if (mode_item && cJSON_IsString(mode_item) && mode_item->valuestring[0] != '\0')
    {
        char *endptr = NULL;
        long val = strtol(mode_item->valuestring, &endptr, 8);
        if (endptr != mode_item->valuestring && *endptr == '\0' && val >= 0 && val <= 07777)
            file_mode = (int)val;
    }

    int timeout_sec = 60;
    cJSON *timeout_json = cJSON_GetObjectItem(args, "timeout");
    if (timeout_json && cJSON_IsNumber(timeout_json) && timeout_json->valueint > 0)
        timeout_sec = timeout_json->valueint;

    if (!edc_file_is_regular(cJSON_GetStringValue(local)))
    {
        EdcError *error =
            edc_error_new(EDC_ERR_EXEC, "device_upload: local file not found: '%s'", cJSON_GetStringValue(local));
        callback(tb_ctx, NULL, error);
        return;
    }

    struct device_file_ctx *ctx = g_new0(struct device_file_ctx, 1);
    ctx->tb_ctx = tb_ctx;
    ctx->callback = callback;
    ctx->agent_ip = sdsnew(conf->device_ip);
    ctx->agent_port = (uint16_t)(conf->device_port > 0 ? conf->device_port : 5000);
    ctx->remote_path = sdsnew(cJSON_GetStringValue(remote));
    ctx->local_path = sdsnew(cJSON_GetStringValue(local));
    ctx->file_mode = file_mode;
    ctx->is_upload = true;
    ctx->timeout_sec = timeout_sec;

    uv_loop_t *main_loop = tb_ctx->tb->scheduler->loop;
    int ar = uv_async_init(main_loop, &ctx->result_async, device_file_on_async_result);
    if (ar < 0)
    {
        EdcError *error = edc_error_new(EDC_ERR_EXEC, "device_upload: uv_async_init failed: %s", uv_strerror(ar));
        callback(tb_ctx, NULL, error);
        device_file_ctx_free(ctx);
        return;
    }
    ctx->result_async.data = ctx;

    uv_thread_t tid;
    int tr = uv_thread_create(&tid, device_file_thread, ctx);
    if (tr != 0)
    {
        log_error("device_upload: uv_thread_create failed: %s", uv_strerror(tr));
        EdcError *error = edc_error_new(EDC_ERR_EXEC, "device_upload: failed to create thread: %s", uv_strerror(tr));
        callback(tb_ctx, NULL, error);
        device_file_ctx_free(ctx);
        return;
    }
}

static void edc_tool_device_download_exec_async(struct edc_toolbox_call_ctx *tb_ctx, struct edc_tool *tool, cJSON *args,
                                                void (*callback)(struct edc_toolbox_call_ctx *, sds, EdcError *))
{
    (void)tool;

    cJSON *remote = cJSON_GetObjectItem(args, "remote_path");
    cJSON *local = cJSON_GetObjectItem(args, "local_path");
    if (!remote || !cJSON_IsString(remote) || !local || !cJSON_IsString(local))
    {
        EdcError *error =
            edc_error_new(EDC_ERR_INVAL, "device_download: missing 'remote_path' or 'local_path' parameter");
        callback(tb_ctx, NULL, error);
        return;
    }

    struct edc_config *conf = tb_ctx->tb->conf;
    if (!conf->device_ip)
    {
        EdcError *error = edc_error_new(EDC_ERR_INVAL, "device_download: device_ip not configured");
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
    ctx->agent_ip = sdsnew(conf->device_ip);
    ctx->agent_port = (uint16_t)(conf->device_port > 0 ? conf->device_port : 5000);
    ctx->remote_path = sdsnew(cJSON_GetStringValue(remote));
    ctx->local_path = sdsnew(cJSON_GetStringValue(local));
    ctx->is_upload = false;
    ctx->timeout_sec = timeout_sec;

    uv_loop_t *main_loop = tb_ctx->tb->scheduler->loop;
    int ar = uv_async_init(main_loop, &ctx->result_async, device_file_on_async_result);
    if (ar < 0)
    {
        EdcError *error = edc_error_new(EDC_ERR_EXEC, "device_download: uv_async_init failed: %s", uv_strerror(ar));
        callback(tb_ctx, NULL, error);
        device_file_ctx_free(ctx);
        return;
    }
    ctx->result_async.data = ctx;

    uv_thread_t tid;
    int tr = uv_thread_create(&tid, device_file_thread, ctx);
    if (tr != 0)
    {
        log_error("device_upload: uv_thread_create failed: %s", uv_strerror(tr));
        EdcError *error = edc_error_new(EDC_ERR_EXEC, "device_upload: failed to create thread: %s", uv_strerror(tr));
        callback(tb_ctx, NULL, error);
        device_file_ctx_free(ctx);
        return;
    }
}

static struct edc_tool *edc_tool_create_device_upload(struct edc_toolbox *tb)
{
    struct edc_tool_param *params = NULL;
    LL_APPEND(params,
              edc_tool_param_new("local_path", edc_param_str_type("Local file path on host to upload", NULL), true));
    LL_APPEND(params, edc_tool_param_new("remote_path",
                                         edc_param_str_type("Remote file path on device destination", NULL), true));
    LL_APPEND(params, edc_tool_param_new(
                          "mode",
                          edc_param_str_type(
                              "File permission mode in octal, e.g. \"644\" for rw-r--r-- (default \"644\")", NULL),
                          false));
    LL_APPEND(params, edc_tool_param_new(
                          "timeout", edc_param_int_type("Timeout per chunk in seconds (default 60)", 1, 3600), false));

    struct edc_tool *tool =
        edc_tool_new("device_upload",
                     "Upload a file from the host to a remote device agent (supports chunked transfer for large files)",
                     params, edc_tool_device_upload_exec_async);
    edc_toolbox_register(tb, tool);
    return tool;
}

static struct edc_tool *edc_tool_create_device_download(struct edc_toolbox *tb)
{
    struct edc_tool_param *params = NULL;
    LL_APPEND(params, edc_tool_param_new("remote_path",
                                         edc_param_str_type("Remote file path on device to download", NULL), true));
    LL_APPEND(params,
              edc_tool_param_new("local_path", edc_param_str_type("Local destination file path on host", NULL), true));
    LL_APPEND(params, edc_tool_param_new(
                          "timeout", edc_param_int_type("Timeout per chunk in seconds (default 60)", 1, 3600), false));

    struct edc_tool *tool = edc_tool_new(
        "device_download",
        "Download a file from a remote device agent to the host (supports chunked transfer for large files)", params,
        edc_tool_device_download_exec_async);
    edc_toolbox_register(tb, tool);
    return tool;
}

void edc_tools_device_file_register(struct edc_toolbox *tb)
{
    edc_tool_create_device_upload(tb);
    edc_tool_create_device_download(tb);
}
