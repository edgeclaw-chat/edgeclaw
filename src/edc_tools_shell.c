#include "sds.h"
#include "edc_sandbox.h"
#include "edc_error.h"
#include "edc_toolbox.h"
#include "edc_tools.h"
#include "log.h"
#include <edc_param_types.h>
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

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
    int status;
    pid_t child_pid;
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

static void shell_ctx_try_complete(struct edc_tool_shell_ctx *ctx);

static void on_stdout_close(uv_handle_t *handle)
{
    struct edc_tool_shell_ctx *ctx = container_of(handle, struct edc_tool_shell_ctx, stdout_pipe);
    if (--ctx->pending_handles == 0)
    {
        if (ctx->stderr_buf)
            sdsfree(ctx->stderr_buf);
        free(ctx);
    }
}

static void on_stderr_close(uv_handle_t *handle)
{
    struct edc_tool_shell_ctx *ctx = container_of(handle, struct edc_tool_shell_ctx, stderr_pipe);
    if (--ctx->pending_handles == 0)
    {
        if (ctx->stderr_buf)
            sdsfree(ctx->stderr_buf);
        free(ctx);
    }
}

static void on_timer_close(uv_handle_t *handle)
{
    struct edc_tool_shell_ctx *ctx = container_of(handle, struct edc_tool_shell_ctx, timeout_timer);
    if (--ctx->pending_handles == 0)
    {
        if (ctx->stderr_buf)
            sdsfree(ctx->stderr_buf);
        free(ctx);
    }
}

static void shell_ctx_close_handles(struct edc_tool_shell_ctx *ctx)
{
    int count = 0;

    if (!uv_is_closing((uv_handle_t *)&ctx->stdout_pipe))
    {
        uv_read_stop((uv_stream_t *)&ctx->stdout_pipe);
        uv_close((uv_handle_t *)&ctx->stdout_pipe, on_stdout_close);
        count++;
    }
    if (!uv_is_closing((uv_handle_t *)&ctx->stderr_pipe))
    {
        uv_read_stop((uv_stream_t *)&ctx->stderr_pipe);
        uv_close((uv_handle_t *)&ctx->stderr_pipe, on_stderr_close);
        count++;
    }
    if (!uv_is_closing((uv_handle_t *)&ctx->timeout_timer))
    {
        uv_timer_stop(&ctx->timeout_timer);
        uv_close((uv_handle_t *)&ctx->timeout_timer, on_timer_close);
        count++;
    }

    ctx->pending_handles = count;
}

static void shell_ctx_complete(struct edc_tool_shell_ctx *ctx)
{
    if (ctx->completed)
        return;
    ctx->completed = true;

    sds result = ctx->stdout_buf;
    int status = ctx->status;

    if (sdslen(ctx->stderr_buf))
        result = sdscatprintf(result, "STDERR:\n%s", ctx->stderr_buf);

    if (sdslen(result))
        result = sdscatprintf(result, "\nDone (exit code %d)", status);
    else
        result = sdscatprintf(result, "Done (exit code %d)", status);

    ctx->stdout_buf = result;

    if (status)
    {
        EdcError *error = edc_error_new(EDC_ERR_EXEC, "%s", result);
        ctx->callback(ctx->tb_ctx, NULL, error);
        sdsfree(result);
    }
    else
    {
        ctx->callback(ctx->tb_ctx, result, NULL);
    }

    uv_mutex_lock(&active_mutex);
    LL_DELETE(active_contexts, ctx);
    uv_mutex_unlock(&active_mutex);

    shell_ctx_close_handles(ctx);
}

static void alloc_buffer(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf)
{
    (void)handle;
    buf->base = malloc(suggested_size ? suggested_size : 4096);
    buf->len = suggested_size ? suggested_size : 4096;
}

static void on_stdout_read(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf)
{
    struct edc_tool_shell_ctx *ctx = container_of(stream, struct edc_tool_shell_ctx, stdout_pipe);

    if (nread > 0)
        ctx->stdout_buf = sdscatlen(ctx->stdout_buf, buf->base, (size_t)nread);

    free(buf->base);

    if (nread < 0)
    {
        if (nread != UV_EOF)
            log_error("edc_tools_shell: stdout read error: %s", uv_strerror((int)nread));
        ctx->stdout_complete = true;
        uv_read_stop(stream);
        shell_ctx_try_complete(ctx);
    }
}

static void on_stderr_read(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf)
{
    struct edc_tool_shell_ctx *ctx = container_of(stream, struct edc_tool_shell_ctx, stderr_pipe);

    if (nread > 0)
        ctx->stderr_buf = sdscatlen(ctx->stderr_buf, buf->base, (size_t)nread);

    free(buf->base);

    if (nread < 0)
    {
        if (nread != UV_EOF)
            log_error("edc_tools_shell: stderr read error: %s", uv_strerror((int)nread));
        ctx->stderr_complete = true;
        uv_read_stop(stream);
        shell_ctx_try_complete(ctx);
    }
}

static void shell_ctx_try_complete(struct edc_tool_shell_ctx *ctx)
{
    if (!ctx->stderr_complete || !ctx->stdout_complete)
        return;

    if (!ctx->child_exited)
    {
        int status;
        pid_t wret = waitpid(ctx->child_pid, &status, WNOHANG);

        if (wret > 0)
        {
            ctx->child_exited = true;
            ctx->status = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
        }
        else if (wret == -1)
        {
            log_error("waitpid failed: %s", strerror(errno));
        }
        else
        {
            log_error("child pid %d still running after pipes closed", ctx->child_pid);
            ctx->status = 1;
        }
    }

    shell_ctx_complete(ctx);
}

static void on_timeout(uv_timer_t *timer)
{
    struct edc_tool_shell_ctx *ctx = container_of(timer, struct edc_tool_shell_ctx, timeout_timer);

    if (!ctx->child_exited)
    {
        kill(-ctx->child_pid, SIGKILL);

        int status;
        pid_t wret = waitpid(ctx->child_pid, &status, WNOHANG);
        if (wret > 0)
        {
            ctx->child_exited = true;
            ctx->status = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
        }
        else
        {
            ctx->status = 1;
        }

        ctx->stderr_buf = sdscat(ctx->stderr_buf, "\n[command timed out]");
    }

    ctx->stdout_complete = true;
    ctx->stderr_complete = true;
    shell_ctx_try_complete(ctx);
}

static void edc_tool_exec_shell_async(struct edc_toolbox_call_ctx *tb_ctx, struct edc_tool *tool, cJSON *args,
                                      void (*callback)(struct edc_toolbox_call_ctx *, sds, EdcError *))
{
    EdcError *error = NULL;
    cJSON *cmd = cJSON_GetObjectItemCaseSensitive(args, "command");
    if (!cmd || !cJSON_IsString(cmd))
    {
        char *msg = cJSON_PrintUnformatted(args);
        log_error("bc tool shell error, cmd error %s", msg);
        error = edc_error_new(EDC_ERR_INVAL, "ERROR: edc_tools_shell exec command failed: %s", msg);
        cJSON_free(msg);
        callback(tb_ctx, NULL, error);
        return;
    }

    const char *cmd_s = cJSON_GetStringValue(cmd);

    unsigned int timeout_sec = CMD_TIMEOUT_SEC;
    cJSON *timeout_json = cJSON_GetObjectItemCaseSensitive(args, "timeout");
    if (timeout_json && cJSON_IsNumber(timeout_json) && timeout_json->valueint > 0)
        timeout_sec = (unsigned int)timeout_json->valueint;

    const char *working_dir = NULL;
    cJSON *working_dir_json = cJSON_GetObjectItemCaseSensitive(args, "working_dir");
    if (working_dir_json && cJSON_IsString(working_dir_json))
        working_dir = cJSON_GetStringValue(working_dir_json);

    log_debug("shell tool exec: %s (timeout %us, working_dir %s)", cmd_s, timeout_sec,
              working_dir ? working_dir : "(default)");

    char *argv[] = {"sh", "-c", (char *)cmd_s, NULL};
    struct edc_config *conf = tb_ctx->tb->conf;

    sds allocated_cwd = NULL;
    const char *cwd = edc_sandbox_get_root(conf);
    if (working_dir)
    {
        allocated_cwd = edc_sandbox_verify_path(conf, working_dir, false, &error);
        if (!allocated_cwd)
        {
            callback(tb_ctx, NULL, error);
            return;
        }
        cwd = allocated_cwd;
    }

    extern char **environ;
    int env_count = 0;
    while (environ && environ[env_count]) env_count++;
    sds *filtered_envp = calloc((size_t)env_count + 1, sizeof(sds));
    int j = 0;
    size_t master_key_prefix_len = strlen(BC_MASTER_KEY_ENV);
    for (int i = 0; i < env_count; i++)
    {
        if (strncmp(environ[i], BC_MASTER_KEY_ENV, master_key_prefix_len) != 0 ||
            environ[i][master_key_prefix_len] != '=')
            filtered_envp[j++] = sdsnew(environ[i]);
    }

    int _stdin[2], _stdout[2], _stderr[2];
    if (pipe(_stdin) == -1 || pipe(_stdout) == -1 || pipe(_stderr) == -1)
    {
        error = edc_error_new(EDC_ERR_EXEC, "pipe failed: %s", strerror(errno));
        sdsfree(allocated_cwd);
        for (int _i = 0; filtered_envp[_i]; _i++) sdsfree(filtered_envp[_i]);
        free(filtered_envp);
        callback(tb_ctx, NULL, error);
        return;
    }

    pid_t child_pid = fork();
    if (child_pid < 0)
    {
        error = edc_error_new(EDC_ERR_EXEC, "fork failed: %s", strerror(errno));
        close(_stdin[0]); close(_stdin[1]);
        close(_stdout[0]); close(_stdout[1]);
        close(_stderr[0]); close(_stderr[1]);
        sdsfree(allocated_cwd);
        for (int _i = 0; filtered_envp[_i]; _i++) sdsfree(filtered_envp[_i]);
        free(filtered_envp);
        callback(tb_ctx, NULL, error);
        return;
    }

    if (child_pid == 0)
    {
        /* Child: sandbox setup, dup stdio, exec */
        setpgid(0, 0);
        edc_sandbox_apply(conf);

        dup2(_stdin[0], STDIN_FILENO);
        dup2(_stdout[1], STDOUT_FILENO);
        dup2(_stderr[1], STDERR_FILENO);

        close(_stdin[0]); close(_stdin[1]);
        close(_stdout[0]); close(_stdout[1]);
        close(_stderr[0]); close(_stderr[1]);

        for (int _i = 0; filtered_envp[_i]; _i++)
            sdsfree(filtered_envp[_i]);
        free(filtered_envp);
        sdsfree(allocated_cwd);

        if (chdir(cwd) != 0)
            _exit(EXIT_FAILURE);

        execvp(argv[0], argv);
        _exit(EXIT_FAILURE);
    }

    /* Parent */
    close(_stdin[0]);
    close(_stdout[1]);
    close(_stderr[1]);

    sdsfree(allocated_cwd);
    for (int _i = 0; filtered_envp[_i]; _i++) sdsfree(filtered_envp[_i]);
    free(filtered_envp);

    int stdin_fd = _stdin[1];
    int stdout_fd = _stdout[0];
    int stderr_fd = _stderr[0];

    close(stdin_fd);

    uv_loop_t *loop = tb_ctx->tb->scheduler->loop;

    struct edc_tool_shell_ctx *ctx = calloc(1, sizeof(struct edc_tool_shell_ctx));
    ctx->tb_ctx = tb_ctx;
    ctx->stderr_buf = sdsempty();
    ctx->stdout_buf = sdsempty();
    ctx->child_pid = child_pid;
    ctx->callback = callback;
    ctx->tool = tool;

    uv_pipe_init(loop, &ctx->stdout_pipe, 0);
    uv_pipe_open(&ctx->stdout_pipe, stdout_fd);
    uv_read_start((uv_stream_t *)&ctx->stdout_pipe, alloc_buffer, on_stdout_read);

    uv_pipe_init(loop, &ctx->stderr_pipe, 0);
    uv_pipe_open(&ctx->stderr_pipe, stderr_fd);
    uv_read_start((uv_stream_t *)&ctx->stderr_pipe, alloc_buffer, on_stderr_read);

    uv_timer_init(loop, &ctx->timeout_timer);
    uv_timer_start(&ctx->timeout_timer, on_timeout, (uint64_t)timeout_sec * 1000, 0);

    uv_mutex_lock(&active_mutex);
    LL_PREPEND(active_contexts, ctx);
    uv_mutex_unlock(&active_mutex);
}

void edc_tool_shell_abort_all(void)
{
    uv_mutex_lock(&active_mutex);
    struct edc_tool_shell_ctx *ctx;
    LL_FOREACH(active_contexts, ctx)
    {
        if (!ctx->child_exited)
        {
            kill(-ctx->child_pid, SIGKILL);
            int status;
            waitpid(ctx->child_pid, &status, 0);
            ctx->child_exited = true;
            ctx->status = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
        }
        shell_ctx_close_handles(ctx);
    }
    uv_mutex_unlock(&active_mutex);
}

struct edc_tool *edc_tool_create_exec_shell(struct edc_toolbox *tb)
{
    const char *name = "exec_shell";
    const char *desc = "Execute `sh -c '<command>'` then return output";

    struct edc_tool_param *params = NULL;
    LL_APPEND(params,
              edc_tool_param_new("command", edc_param_str_type("The shell command to execute", NULL), true));
    LL_APPEND(params,
              edc_tool_param_new("timeout", edc_param_int_type("Timeout in seconds (default 30)", 1, 3600), false));
    LL_APPEND(params,
              edc_tool_param_new("working_dir",
                                 edc_param_str_type("Working directory (relative to sandbox root)", NULL), false));

    struct edc_tool *tool = edc_tool_new(name, desc, params, edc_tool_exec_shell_async);
    edc_toolbox_register(tb, tool);
    return tool;
}

void edc_tools_shell_register(struct edc_toolbox *tb)
{
    edc_tool_create_exec_shell(tb);
}
