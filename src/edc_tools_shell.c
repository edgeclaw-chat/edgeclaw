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
    GPid child_pid;
    GIOChannel *stdout_ch;
    GIOChannel *stderr_ch;
    GSource *stdout_source;
    GSource *stderr_source;
    GSource *timeout_source;
    struct edc_toolbox_call_ctx *tb_ctx;
    void (*callback)(struct edc_toolbox_call_ctx *, sds result, EdcError *error);
    struct edc_tool_shell_ctx *next;
};

static struct edc_tool_shell_ctx *active_contexts = NULL;
static GMutex active_mutex;

static void edc_tool_shell_complete(struct edc_tool_shell_ctx *ctx)
{
    if (ctx->completed)
        return;
    ctx->completed = TRUE;

    sds result = ctx->stdout_buf;
    int status = ctx->status;

    if (sdslen(ctx->stderr_buf))
    {
        result = sdscatprintf(result, "STDERR:\n%s", ctx->stderr_buf);
    }

    if (sdslen(result))
    {
        result = sdscatprintf(result, "\nDone (exit code %d)", status);
    }
    else
    {
        result = sdscatprintf(result, "Done (exit code %d)", status);
    }

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

    if (ctx->timeout_source)
    {
        g_source_destroy(ctx->timeout_source);
        g_source_unref(ctx->timeout_source);
        ctx->timeout_source = NULL;
    }
    if (ctx->stdout_source)
    {
        g_source_destroy(ctx->stdout_source);
        g_source_unref(ctx->stdout_source);
        ctx->stdout_source = NULL;
    }
    if (ctx->stderr_source)
    {
        g_source_destroy(ctx->stderr_source);
        g_source_unref(ctx->stderr_source);
        ctx->stderr_source = NULL;
    }

    if (ctx->stdout_ch)
    {
        g_io_channel_unref(ctx->stdout_ch);
        ctx->stdout_ch = NULL;
    }
    if (ctx->stderr_ch)
    {
        g_io_channel_unref(ctx->stderr_ch);
        ctx->stderr_ch = NULL;
    }
    sdsfree(ctx->stderr_buf);
    ctx->stderr_buf = NULL;

    g_mutex_lock(&active_mutex);
    LL_DELETE(active_contexts, ctx);
    g_mutex_unlock(&active_mutex);

    free(ctx);
}

static bool on_pipe_readable(GIOChannel *source, GIOCondition condition, void* data)
{
    if (condition & G_IO_ERR)
    {
        log_error("edc_tools_shell: io channel error.");
        return FALSE;
    }

    sds *buf_ptr = data;

    char line[1024];
    size_t bytes_read = 0;
    GIOStatus status = g_io_channel_read_chars(source, line, sizeof(line), &bytes_read, NULL);

    if (status == G_IO_STATUS_ERROR)
    {
        return FALSE;
    }

    if (bytes_read > 0)
    {
        *buf_ptr = sdscatlen(*buf_ptr, line, bytes_read);
    }

    if (status == G_IO_STATUS_EOF || (condition & G_IO_HUP))
    {
        return FALSE;
    }

    if (status == G_IO_STATUS_AGAIN)
    {
        return TRUE;
    }

    return TRUE;
}

static void edc_tool_shell_try_complete(struct edc_tool_shell_ctx *ctx)
{
    if (!ctx->stderr_complete || !ctx->stdout_complete)
        return;

    /* Both pipes closed. Try to reap the child, but don't block. */
    if (!ctx->child_exited)
    {
        int status;
        pid_t wret = waitpid(ctx->child_pid, &status, WNOHANG);

        if (wret > 0)
        {
            ctx->child_exited = TRUE;
            ctx->status = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
            g_spawn_close_pid(ctx->child_pid);
        }
        else if (wret == -1)
        {
            log_error("waitpid failed: %s", strerror(errno));
        }
        else
        {
            /* Child still running (pipes closed but not exited).
             * Don't block the event loop — signal ECHILD as if exited. */
            log_error("child pid %d still running after pipes closed", ctx->child_pid);
            ctx->status = 1;
        }
    }

    edc_tool_shell_complete(ctx);
}
static bool on_stderr_pipe_readable(GIOChannel *source, GIOCondition cond, void *data)
{
    struct edc_tool_shell_ctx *ctx = data;
    if (ctx->completed)
        return FALSE;
    bool ret = on_pipe_readable(source, cond, &ctx->stderr_buf);
    if (!ret)
    {
        ctx->stderr_complete = TRUE;
    }
    edc_tool_shell_try_complete(ctx);
    return ret;
}

static bool on_stdout_pipe_readable(GIOChannel *source, GIOCondition cond, void *data)
{
    struct edc_tool_shell_ctx *ctx = data;
    if (ctx->completed)
        return FALSE;
    bool ret = on_pipe_readable(source, cond, &ctx->stdout_buf);
    if (!ret)
    {
        ctx->stdout_complete = TRUE;
    }
    edc_tool_shell_try_complete(ctx);
    return ret;
}

static GSource *edc_tool_shell_watch_io_channel(GIOChannel *channel, GIOFunc fn, void* user_data)
{
    GSource *source = g_io_create_watch(channel, G_IO_IN | G_IO_HUP | G_IO_ERR);
    g_source_set_callback(source, (GSourceFunc)fn, user_data, NULL);
    g_source_attach(source, g_main_context_get_thread_default());
    return source;
}

static void edc_tool_shell_child_setup(void* user_data)
{
    struct edc_config *conf = user_data;
    setpgid(0, 0);
    edc_sandbox_apply(conf);
}

static bool shell_timeout_cb(void* data)
{
    struct edc_tool_shell_ctx *ctx = data;

    if (!ctx->child_exited)
    {
        kill(-ctx->child_pid, SIGKILL);

        int status;
        pid_t wret = waitpid(ctx->child_pid, &status, WNOHANG);
        if (wret > 0)
        {
            ctx->child_exited = TRUE;
            ctx->status = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
            g_spawn_close_pid(ctx->child_pid);
        }
        else
        {
            ctx->status = 1;
        }

        ctx->stderr_buf = sdscat(ctx->stderr_buf, "\n[command timed out]");
    }

    ctx->stdout_complete = TRUE;
    ctx->stderr_complete = TRUE;
    edc_tool_shell_try_complete(ctx);
    return G_SOURCE_REMOVE;
}

static void edc_tool_exec_shell_async(struct edc_toolbox_call_ctx *tb_ctx, struct edc_tool *tool, cJSON *args,
                                     void (*callback)(struct edc_toolbox_call_ctx *, sds , EdcError *))
{
    EdcError *error = NULL;
    cJSON *cmd = cJSON_GetObjectItemCaseSensitive(args, "command");
    if (!cmd || !cJSON_IsString(cmd))
    {
        char *msg = cJSON_PrintUnformatted(args);
        log_error("bc tool shell error, cmd error %s", msg);
        error = edc_error_new(EDC_ERR_INVAL, "ERROR: edc_tools_shell exec command failed: %s", msg);
        free(msg);
        callback(tb_ctx, NULL, error);
        return;
    }

    const char *cmd_s = cJSON_GetStringValue(cmd);

    unsigned int timeout_sec = CMD_TIMEOUT_SEC;
    cJSON *timeout_json = cJSON_GetObjectItemCaseSensitive(args, "timeout");
    if (timeout_json && cJSON_IsNumber(timeout_json) && timeout_json->valueint > 0)
    {
        timeout_sec = (unsigned int)timeout_json->valueint;
    }

    const char *working_dir = NULL;
    cJSON *working_dir_json = cJSON_GetObjectItemCaseSensitive(args, "working_dir");
    if (working_dir_json && cJSON_IsString(working_dir_json))
    {
        working_dir = cJSON_GetStringValue(working_dir_json);
    }

    log_debug("shell tool exec: %s (timeout %us, working_dir %s)", cmd_s, timeout_sec,
              working_dir ? working_dir : "(default)");

    char *argv[] = {"sh", "-c", (char *)cmd_s, NULL};

    struct edc_config *conf = tb_ctx->tb->conf;

    char *allocated_cwd = NULL;
    const char *cwd = edc_sandbox_get_root(conf);
    if (working_dir)
    {
        allocated_cwd = edc_sandbox_verify_path(conf, working_dir, FALSE, &error);
        if (!allocated_cwd)
        {
            callback(tb_ctx, NULL, error);
            return;
        }
        cwd = allocated_cwd;
    }

    char **envp = g_get_environ();
    int env_count = 0;
    while (envp && envp[env_count]) env_count++;
    char **filtered_envp = g_new0(char *, env_count + 1);
    int j = 0;
    size_t master_key_prefix_len = strlen(BC_MASTER_KEY_ENV);
    for (int i = 0; i < env_count; i++)
    {
        if (strncmp(envp[i], BC_MASTER_KEY_ENV, master_key_prefix_len) != 0 || envp[i][master_key_prefix_len] != '=')
        {
            filtered_envp[j++] = strdup(envp[i]);
        }
    }
    for (int _i = 0; envp[_i]; _i++) free(envp[_i]);
    free(envp);

    GPid child_pid = 0;
    int stdin_fd = 0, stdout_fd = 0, stderr_fd = 0;
    GError *spawn_error = NULL;
    bool success =
        g_spawn_async_with_pipes(cwd, argv, filtered_envp, G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD,
                                 edc_tool_shell_child_setup, conf, &child_pid, &stdin_fd, &stdout_fd, &stderr_fd, &spawn_error);

    free(allocated_cwd);

    for (int _i = 0; filtered_envp[_i]; _i++) free(filtered_envp[_i]);
    free(filtered_envp);

    if (!success)
    {
        error = edc_error_new(EDC_ERR_EXEC, "%s", spawn_error->message);
        g_error_free(spawn_error);
        callback(tb_ctx, NULL, error);
        return;
    }

    close(stdin_fd);

    struct edc_tool_shell_ctx *ctx = g_new0(struct edc_tool_shell_ctx, 1);
    ctx->tb_ctx = tb_ctx;
    ctx->stderr_buf = sdsempty();
    ctx->stdout_buf = sdsempty();
    ctx->stderr_complete = FALSE;
    ctx->stdout_complete = FALSE;
    ctx->child_exited = FALSE;
    ctx->completed = FALSE;
    ctx->stdout_source = NULL;
    ctx->stderr_source = NULL;
    ctx->timeout_source = NULL;
    ctx->child_pid = child_pid;
    ctx->callback = callback;

    ctx->stdout_ch = g_io_channel_unix_new(stdout_fd);
    ctx->stderr_ch = g_io_channel_unix_new(stderr_fd);

    g_io_channel_set_flags(ctx->stdout_ch, G_IO_FLAG_NONBLOCK, NULL);
    g_io_channel_set_flags(ctx->stderr_ch, G_IO_FLAG_NONBLOCK, NULL);

    ctx->timeout_source = g_timeout_source_new_seconds(timeout_sec);
    g_source_set_callback(ctx->timeout_source, (GSourceFunc)shell_timeout_cb, ctx, NULL);
    g_source_attach(ctx->timeout_source, g_main_context_get_thread_default());

    ctx->stderr_source = edc_tool_shell_watch_io_channel(ctx->stderr_ch, (GIOFunc)on_stderr_pipe_readable, ctx);
    ctx->stdout_source = edc_tool_shell_watch_io_channel(ctx->stdout_ch, (GIOFunc)on_stdout_pipe_readable, ctx);

    g_mutex_lock(&active_mutex);
    LL_PREPEND(active_contexts, ctx);
    g_mutex_unlock(&active_mutex);
}

void edc_tool_shell_abort_all(void)
{
    g_mutex_lock(&active_mutex);
    struct edc_tool_shell_ctx *ctx;
    LL_FOREACH(active_contexts, ctx)
    {
        if (ctx->timeout_source)
        {
            g_source_destroy(ctx->timeout_source);
            g_source_unref(ctx->timeout_source);
            ctx->timeout_source = NULL;
        }
        if (ctx->stdout_source)
        {
            g_source_destroy(ctx->stdout_source);
            g_source_unref(ctx->stdout_source);
            ctx->stdout_source = NULL;
        }
        if (ctx->stderr_source)
        {
            g_source_destroy(ctx->stderr_source);
            g_source_unref(ctx->stderr_source);
            ctx->stderr_source = NULL;
        }
        if (!ctx->child_exited)
        {
            kill(-ctx->child_pid, SIGKILL);
            int status;
            waitpid(ctx->child_pid, &status, 0);
            g_spawn_close_pid(ctx->child_pid);
            ctx->child_exited = TRUE;
            ctx->status = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
        }
        if (ctx->stdout_ch)
        {
            g_io_channel_unref(ctx->stdout_ch);
            ctx->stdout_ch = NULL;
        }
        if (ctx->stderr_ch)
        {
            g_io_channel_unref(ctx->stderr_ch);
            ctx->stderr_ch = NULL;
        }
    }
    g_mutex_unlock(&active_mutex);
}

struct edc_tool *edc_tool_create_exec_shell(struct edc_toolbox *tb)
{
    const char *name = "exec_shell";
    const char *desc = "Execute `sh -c '<command>'` then return output";

    struct edc_tool_param *params = NULL;
    LL_APPEND(params, edc_tool_param_new("command", edc_param_str_type("The shell command to execute", NULL), TRUE));
    LL_APPEND(params, edc_tool_param_new("timeout", edc_param_int_type("Timeout in seconds (default 30)", 1, 3600), FALSE));
    LL_APPEND(params, edc_tool_param_new("working_dir", edc_param_str_type("Working directory (relative to sandbox root)", NULL), FALSE));

    struct edc_tool *tool = edc_tool_new(name, desc, params, edc_tool_exec_shell_async);
    edc_toolbox_register(tb, tool);
    return tool;
}

void edc_tools_shell_register(struct edc_toolbox *tb)
{
    edc_tool_create_exec_shell(tb);
}
