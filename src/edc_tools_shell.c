#include "sds.h"
#include "edc_compat.h"
#include "edc_sandbox.h"
#include "edc_error.h"
#include "edc_toolbox.h"
#include "edc_tools.h"
#include "log.h"
#include <edc_param_types.h>
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>

#define BC_MASTER_KEY_ENV "EDGECLAW_MASTER_KEY"
#define CMD_TIMEOUT_SEC 30

struct edc_tool_shell_ctx
{
    struct edc_tool *tool;
    sds stderr_buf;
    sds stdout_buf;
    bool stderr_complete;
    bool stdout_complete;
    bool child_exited;
    bool completed;
    int exit_status;
    uv_process_t process;
    uv_pipe_t stdout_pipe;
    uv_pipe_t stderr_pipe;
    uv_timer_t timeout_timer;
    int pending_handles;
    struct edc_toolbox_call_ctx *tb_ctx;
    void (*callback)(struct edc_toolbox_call_ctx *, sds result, EdcError *error);
    struct edc_tool_shell_ctx *next;
};

static struct edc_tool_shell_ctx *active_contexts = NULL;
static uv_mutex_t active_mutex;
static uv_once_t active_mutex_once = UV_ONCE_INIT;

static void active_mutex_init(void)
{
    uv_mutex_init(&active_mutex);
}

static void shell_ctx_try_complete(struct edc_tool_shell_ctx *ctx);

static void alloc_buffer(uv_handle_t *handle, size_t suggested_size,
                         uv_buf_t *buf)
{
    (void)handle;
    buf->base = malloc(suggested_size);
    buf->len = suggested_size;
}

static void shell_ctx_close_handles(struct edc_tool_shell_ctx *ctx)
{
    if (--ctx->pending_handles == 0)
    {
        if (ctx->stderr_buf) sdsfree(ctx->stderr_buf);
        if (ctx->stdout_buf) sdsfree(ctx->stdout_buf);
        free(ctx);
    }
}

static void on_stdout_close(uv_handle_t *handle)
{
    struct edc_tool_shell_ctx *ctx = handle->data;
    shell_ctx_close_handles(ctx);
}

static void on_stderr_close(uv_handle_t *handle)
{
    struct edc_tool_shell_ctx *ctx = handle->data;
    shell_ctx_close_handles(ctx);
}

static void on_timer_close(uv_handle_t *handle)
{
    struct edc_tool_shell_ctx *ctx = handle->data;
    shell_ctx_close_handles(ctx);
}

static void on_process_close(uv_handle_t *handle)
{
    struct edc_tool_shell_ctx *ctx = handle->data;
    shell_ctx_close_handles(ctx);
}

static void on_stdout_read(uv_stream_t *stream, ssize_t nread,
                           const uv_buf_t *buf)
{
    struct edc_tool_shell_ctx *ctx = stream->data;
    if (nread > 0)
        ctx->stdout_buf = sdscatlen(ctx->stdout_buf, buf->base, (size_t)nread);
    if (nread != UV_EOF && nread < 0)
        log_error("edc_tools_shell: stdout read error: %s",
                  uv_strerror((int)nread));
    free(buf->base);
    if (nread == UV_EOF || nread < 0)
    {
        ctx->stdout_complete = true;
        uv_read_stop(stream);
        shell_ctx_try_complete(ctx);
    }
}

static void on_stderr_read(uv_stream_t *stream, ssize_t nread,
                           const uv_buf_t *buf)
{
    struct edc_tool_shell_ctx *ctx = stream->data;
    if (nread > 0)
        ctx->stderr_buf = sdscatlen(ctx->stderr_buf, buf->base, (size_t)nread);
    if (nread != UV_EOF && nread < 0)
        log_error("edc_tools_shell: stderr read error: %s",
                  uv_strerror((int)nread));
    free(buf->base);
    if (nread == UV_EOF || nread < 0)
    {
        ctx->stderr_complete = true;
        uv_read_stop(stream);
        shell_ctx_try_complete(ctx);
    }
}

static void on_shell_exit(uv_process_t *process, int64_t exit_status,
                          int term_signal)
{
    struct edc_tool_shell_ctx *ctx = process->data;
    ctx->child_exited = true;
    ctx->exit_status = term_signal ? -term_signal : (int)exit_status;
    shell_ctx_try_complete(ctx);
}

static void shell_ctx_complete(struct edc_tool_shell_ctx *ctx)
{
    if (ctx->completed) return;
    ctx->completed = true;

    uv_timer_stop(&ctx->timeout_timer);

    if (ctx->stdout_buf) sdstrim(ctx->stdout_buf, " \t\r\n");
    if (ctx->stderr_buf) sdstrim(ctx->stderr_buf, " \t\r\n");

    sds result = sdsempty();
    if (ctx->stdout_buf && sdslen(ctx->stdout_buf) > 0)
        result = sdscatprintf(result, "%s\n", ctx->stdout_buf);
    if (ctx->stderr_buf && sdslen(ctx->stderr_buf) > 0)
        result = sdscatprintf(result, "stderr:\n%s\n", ctx->stderr_buf);

    ctx->callback(ctx->tb_ctx, result, NULL);

    uv_once(&active_mutex_once, active_mutex_init);
    uv_mutex_lock(&active_mutex);
    LL_DELETE(active_contexts, ctx);
    uv_mutex_unlock(&active_mutex);

    ctx->pending_handles = 4;
    uv_close((uv_handle_t *)&ctx->stdout_pipe, on_stdout_close);
    uv_close((uv_handle_t *)&ctx->stderr_pipe, on_stderr_close);
    uv_close((uv_handle_t *)&ctx->timeout_timer, on_timer_close);
    uv_close((uv_handle_t *)&ctx->process, on_process_close);
}

static void shell_ctx_try_complete(struct edc_tool_shell_ctx *ctx)
{
    if (!ctx->stderr_complete || !ctx->stdout_complete || !ctx->child_exited)
        return;
    shell_ctx_complete(ctx);
}

static void on_timeout(uv_timer_t *timer)
{
    struct edc_tool_shell_ctx *ctx = timer->data;
    if (!ctx->child_exited)
    {
        uv_process_kill(&ctx->process,
#ifdef _WIN32
                         SIGTERM
#else
                         SIGKILL
#endif
                         );
        ctx->child_exited = true;
        ctx->exit_status = -1;
        ctx->stderr_buf = sdscat(ctx->stderr_buf,
                                  "\n[command timed out]");
    }
    ctx->stdout_complete = true;
    ctx->stderr_complete = true;
    shell_ctx_try_complete(ctx);
}

static void edc_shell_spawn(struct edc_toolbox_call_ctx *tb_ctx,
                            struct edc_tool *tool,
                            const char *cmd_s, const char *cwd,
                            unsigned int timeout_sec,
                            void (*callback)(struct edc_toolbox_call_ctx *,
                                             sds, EdcError *))
{
    uv_loop_t *loop = tb_ctx->tb->scheduler->loop;

    struct edc_tool_shell_ctx *ctx = calloc(1, sizeof(*ctx));
    ctx->tb_ctx = tb_ctx;
    ctx->stderr_buf = sdsempty();
    ctx->stdout_buf = sdsempty();
    ctx->callback = callback;
    ctx->tool = tool;

    uv_pipe_init(loop, &ctx->stdout_pipe, 0);
    uv_pipe_init(loop, &ctx->stderr_pipe, 0);
    uv_timer_init(loop, &ctx->timeout_timer);
    ctx->timeout_timer.data = ctx;

    /*
     * NOTE: uv__process_open_stream() in this libuv version has inverted
     * flag semantics for UV_READABLE_PIPE / UV_WRITABLE_PIPE:
     *   UV_READABLE_PIPE → UV_HANDLE_WRITABLE  (parent can write = stdin)
     *   UV_WRITABLE_PIPE → UV_HANDLE_READABLE  (parent can read  = stdout/stderr)
     * Therefore stdout/stderr use UV_WRITABLE_PIPE so that uv_read_start
     * sees UV_HANDLE_READABLE and starts polling POLLIN.
     */
    uv_stdio_container_t stdio[3];
    stdio[0].flags = UV_IGNORE;
    stdio[1].flags = UV_CREATE_PIPE | UV_WRITABLE_PIPE;
    stdio[1].data.stream = (uv_stream_t *)&ctx->stdout_pipe;
    stdio[2].flags = UV_CREATE_PIPE | UV_WRITABLE_PIPE;
    stdio[2].data.stream = (uv_stream_t *)&ctx->stderr_pipe;

    uv_process_options_t options = {0};
    options.exit_cb = on_shell_exit;
#ifdef _WIN32
    options.file = "cmd.exe";
    char *args[] = {"cmd.exe", "/c", (char *)cmd_s, NULL};
#else
    options.file = "/bin/sh";
    char *args[] = {"sh", "-c", (char *)cmd_s, NULL};
#endif
    options.args = args;
    options.env = NULL;
    options.cwd = cwd;
    options.stdio = stdio;
    options.stdio_count = 3;

    ctx->process.data = ctx;
    int r = uv_spawn(loop, &ctx->process, &options);
    if (r != 0)
    {
        EdcError *error = edc_error_new(EDC_ERR_EXEC, "uv_spawn failed: %s",
                                        uv_strerror(r));
        ctx->stdout_pipe.data = ctx;
        ctx->stderr_pipe.data = ctx;
        ctx->pending_handles = 3;
        uv_close((uv_handle_t *)&ctx->stdout_pipe, on_stdout_close);
        uv_close((uv_handle_t *)&ctx->stderr_pipe, on_stderr_close);
        uv_close((uv_handle_t *)&ctx->timeout_timer, on_timer_close);
        callback(tb_ctx, NULL, error);
        return;
    }

    ctx->stdout_pipe.data = ctx;
    ctx->stderr_pipe.data = ctx;
    uv_read_start((uv_stream_t *)&ctx->stdout_pipe, alloc_buffer,
                  on_stdout_read);
    uv_read_start((uv_stream_t *)&ctx->stderr_pipe, alloc_buffer,
                  on_stderr_read);

    uv_timer_start(&ctx->timeout_timer, on_timeout,
                   (uint64_t)timeout_sec * 1000, 0);

    uv_once(&active_mutex_once, active_mutex_init);
    uv_mutex_lock(&active_mutex);
    LL_PREPEND(active_contexts, ctx);
    uv_mutex_unlock(&active_mutex);
}

static void edc_tool_exec_shell_async(struct edc_toolbox_call_ctx *tb_ctx,
                                      struct edc_tool *tool,
                                      cJSON *args,
                                      void (*callback)(struct edc_toolbox_call_ctx *,
                                                       sds, EdcError *))
{
    EdcError *error = NULL;
    cJSON *cmd = cJSON_GetObjectItemCaseSensitive(args, "command");
    if (!cmd || !cJSON_IsString(cmd))
    {
        char *msg = cJSON_PrintUnformatted(args);
        error = edc_error_new(EDC_ERR_INVAL,
                              "ERROR: edc_tools_shell exec command failed: %s",
                              msg);
        cJSON_free(msg);
        callback(tb_ctx, NULL, error);
        return;
    }

    const char *cmd_s = cJSON_GetStringValue(cmd);
    unsigned int timeout_sec = CMD_TIMEOUT_SEC;
    cJSON *timeout_json = cJSON_GetObjectItemCaseSensitive(args, "timeout");
    if (timeout_json && cJSON_IsNumber(timeout_json) &&
        timeout_json->valueint > 0)
        timeout_sec = (unsigned int)timeout_json->valueint;

    const char *working_dir = NULL;
    cJSON *wd = cJSON_GetObjectItemCaseSensitive(args, "working_dir");
    if (wd && cJSON_IsString(wd))
        working_dir = cJSON_GetStringValue(wd);

    struct edc_config *conf = tb_ctx->tb->conf;
    sds allocated_cwd = NULL;
    const char *cwd = edc_sandbox_get_root(conf);
    if (working_dir)
    {
        allocated_cwd = edc_sandbox_verify_path(conf, working_dir, false,
                                                &error);
        if (!allocated_cwd) { callback(tb_ctx, NULL, error); return; }
        cwd = allocated_cwd;
    }

    edc_shell_spawn(tb_ctx, tool, cmd_s, cwd, timeout_sec, callback);
    sdsfree(allocated_cwd);
}

void edc_tool_shell_abort_all(void)
{
    uv_once(&active_mutex_once, active_mutex_init);
    uv_mutex_lock(&active_mutex);
    struct edc_tool_shell_ctx *ctx, *tmp;
    LL_FOREACH_SAFE(active_contexts, ctx, tmp)
    {
        LL_DELETE(active_contexts, ctx);
        if (!ctx->child_exited) {
            uv_process_kill(&ctx->process,
#ifdef _WIN32
                         SIGTERM
#else
                         SIGKILL
#endif
                         );
        }
        uv_timer_stop(&ctx->timeout_timer);
        ctx->pending_handles = 4;
        uv_close((uv_handle_t *)&ctx->stdout_pipe, on_stdout_close);
        uv_close((uv_handle_t *)&ctx->stderr_pipe, on_stderr_close);
        uv_close((uv_handle_t *)&ctx->timeout_timer, on_timer_close);
        uv_close((uv_handle_t *)&ctx->process, on_process_close);
    }
    uv_mutex_unlock(&active_mutex);
}

struct edc_tool *edc_tool_create_exec_shell(struct edc_toolbox *tb)
{
    const char *name = "exec_shell";
    const char *desc = "Execute shell command and return output";
    struct edc_tool_param *params = NULL;
    LL_APPEND(params,
              edc_tool_param_new("command",
                                 edc_param_str_type("The shell command to execute", NULL),
                                 true));
    LL_APPEND(params,
              edc_tool_param_new("timeout",
                                 edc_param_int_type("Timeout in seconds (default 30)", 1, 3600),
                                 false));
    LL_APPEND(params,
              edc_tool_param_new("working_dir",
                                 edc_param_str_type("Working directory (relative to sandbox root)", NULL),
                                 false));
    struct edc_tool *tool = edc_tool_new(name, desc, params, edc_tool_exec_shell_async);
    edc_toolbox_register(tb, tool);
    return tool;
}

void edc_tools_shell_register(struct edc_toolbox *tb)
{
    edc_tool_create_exec_shell(tb);
}
