#include "edc_string.h"
#include "edc_strutil.h"
#include "edc_sandbox.h"
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
    int status;
    GPid child_pid;
    GIOChannel *stdout_ch;
    GIOChannel *stderr_ch;
    struct edc_toolbox_call_ctx *tb_ctx;
    void (*callback)(struct edc_toolbox_call_ctx *, sds result, GError *error);
    unsigned int timeout_id;
    struct edc_tool_shell_ctx *next;
};

static struct edc_tool_shell_ctx *active_contexts = NULL;
static GMutex active_mutex;

static void edc_tool_shell_cleanup_channel(GIOChannel *ch)
{
    if (ch)
    {
        g_io_channel_shutdown(ch, FALSE, NULL);
        g_io_channel_unref(ch);
    }
}

static void edc_tool_shell_complete(struct edc_tool_shell_ctx *ctx)
{
    sds result = ctx->stdout_buf;
    int status = ctx->status;

    if (edc_str_len(ctx->stderr_buf))
    {
        edc_str_printf(result, "STDERR:\n%s", ctx->stderr_buf);
    }

    if (edc_str_len(result))
    {
        edc_str_printf(result, "\nDone (exit code %d)", status);
    }
    else
    {
        edc_str_printf(result, "Done (exit code %d)", status);
    }

    ctx->stdout_buf = result;

    if (status)
    {
        GError *error = g_error_new(BC_TOOL_ERROR, BC_TOOL_EXEC_ERROR, "%s", result);
        ctx->callback(ctx->tb_ctx, NULL, error);
        edc_str_free(result);
    }
    else
    {
        ctx->callback(ctx->tb_ctx, result, NULL);
    }

    if (ctx->timeout_id > 0)
    {
        g_source_remove(ctx->timeout_id);
        ctx->timeout_id = 0;
    }

    edc_tool_shell_cleanup_channel(ctx->stdout_ch);
    edc_tool_shell_cleanup_channel(ctx->stderr_ch);
    edc_str_free(ctx->stderr_buf);

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
        edc_str_catn(*buf_ptr, line, bytes_read);
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
            ctx->status = status;
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
    bool ret = on_pipe_readable(source, cond, &ctx->stdout_buf);
    if (!ret)
    {
        ctx->stdout_complete = TRUE;
    }
    edc_tool_shell_try_complete(ctx);
    return ret;
}

static void edc_tool_shell_watch_io_channel(GIOChannel *channel, GIOFunc fn, void* user_data)
{
    GSource *source = g_io_create_watch(channel, G_IO_IN | G_IO_HUP | G_IO_ERR);
    g_source_set_callback(source, (GSourceFunc)fn, user_data, NULL);
    g_source_attach(source, g_main_context_get_thread_default());
    g_source_unref(source);
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
        edc_str_cat(ctx->stderr_buf, "\n[command timed out]");
    }

    ctx->timeout_id = 0;
    return G_SOURCE_REMOVE;
}

static void edc_tool_exec_shell_async(struct edc_toolbox_call_ctx *tb_ctx, struct edc_tool *tool, cJSON *args,
                                     void (*callback)(struct edc_toolbox_call_ctx *, sds , GError *))
{
    GError *error = NULL;
    cJSON *cmd = cJSON_GetObjectItemCaseSensitive(args, "command");
    if (!cmd || !cJSON_IsString(cmd))
    {
        char *msg = cJSON_PrintUnformatted(args);
        log_error("bc tool shell error, cmd error %s", msg);
        error = g_error_new(BC_TOOL_ERROR, BC_TOOL_ARG_INVALID, "ERROR: edc_tools_shell exec command failed: %s", msg);
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

    log_debug("shell tool exec: %s (timeout %us)", cmd_s, timeout_sec);

    char *argv[] = {"sh", "-c", (char *)cmd_s, NULL};

    char **envp = g_get_environ();
    int env_count = edc_strv_length(envp);
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
    edc_strfreev(envp);

    GPid child_pid = 0;
    int stdin_fd = 0, stdout_fd = 0, stderr_fd = 0;
    struct edc_config *conf = tb_ctx->tb->conf;
    const char *working_dir = edc_sandbox_get_root(conf);
    bool success =
        g_spawn_async_with_pipes(working_dir, argv, filtered_envp, G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD,
                                 edc_tool_shell_child_setup, conf, &child_pid, &stdin_fd, &stdout_fd, &stderr_fd, &error);

    edc_strfreev(filtered_envp);

    if (!success)
    {
        callback(tb_ctx, NULL, error);
        return;
    }

    close(stdin_fd);

    struct edc_tool_shell_ctx *ctx = g_new0(struct edc_tool_shell_ctx, 1);
    ctx->tb_ctx = tb_ctx;
    ctx->stderr_buf = edc_str_new(NULL);
    ctx->stdout_buf = edc_str_new(NULL);
    ctx->stderr_complete = FALSE;
    ctx->stdout_complete = FALSE;
    ctx->child_exited = FALSE;
    ctx->child_pid = child_pid;
    ctx->callback = callback;

    ctx->stdout_ch = g_io_channel_unix_new(stdout_fd);
    ctx->stderr_ch = g_io_channel_unix_new(stderr_fd);

    g_io_channel_set_flags(ctx->stdout_ch, G_IO_FLAG_NONBLOCK, NULL);
    g_io_channel_set_flags(ctx->stderr_ch, G_IO_FLAG_NONBLOCK, NULL);

    ctx->timeout_id = g_timeout_add_seconds(timeout_sec, (GSourceFunc)shell_timeout_cb, ctx);

    edc_tool_shell_watch_io_channel(ctx->stderr_ch, (GIOFunc)on_stderr_pipe_readable, ctx);
    edc_tool_shell_watch_io_channel(ctx->stdout_ch, (GIOFunc)on_stdout_pipe_readable, ctx);

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
        if (ctx->timeout_id > 0)
        {
            g_source_remove(ctx->timeout_id);
            ctx->timeout_id = 0;
        }
        if (!ctx->child_exited)
        {
            kill(-ctx->child_pid, SIGKILL);
            int status;
            waitpid(ctx->child_pid, &status, 0);
            g_spawn_close_pid(ctx->child_pid);
            ctx->child_exited = TRUE;
            ctx->status = status;
        }
        edc_tool_shell_cleanup_channel(ctx->stdout_ch);
        edc_tool_shell_cleanup_channel(ctx->stderr_ch);
        ctx->stdout_ch = NULL;
        ctx->stderr_ch = NULL;
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

    struct edc_tool *tool = edc_tool_new(name, desc, params, edc_tool_exec_shell_async);
    edc_toolbox_register(tb, tool);
    return tool;
}

void edc_tools_shell_register(struct edc_toolbox *tb)
{
    edc_tool_create_exec_shell(tb);
}
