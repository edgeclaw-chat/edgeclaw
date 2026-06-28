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
    GString *stderr_buf;
    GString *stdout_buf;
    gboolean stderr_complete;
    gboolean stdout_complete;
    gboolean child_exited;
    gint status;
    GPid child_pid;
    GIOChannel *stdout_ch;
    GIOChannel *stderr_ch;
    struct edc_toolbox_call_ctx *tb_ctx;
    void (*callback)(struct edc_toolbox_call_ctx *, GString *result, GError *error);
    guint timeout_id;
};

static GSList *active_contexts = NULL;
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
    GString *result = ctx->stdout_buf;
    gint status = ctx->status;

    if (ctx->stderr_buf->len)
    {
        g_string_append_printf(result, "STDERR:\n%s", ctx->stderr_buf->str);
    }

    if (result->len)
    {
        g_string_append_printf(result, "\nDone (exit code %d)", status);
    }
    else
    {
        g_string_append_printf(result, "Done (exit code %d)", status);
    }

    if (status)
    {
        GError *error = g_error_new(BC_TOOL_ERROR, BC_TOOL_EXEC_ERROR, "%s", result->str);
        ctx->callback(ctx->tb_ctx, NULL, error);
        g_string_free(result, TRUE);
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
    g_string_free(ctx->stderr_buf, TRUE);

    g_mutex_lock(&active_mutex);
    active_contexts = g_slist_remove(active_contexts, ctx);
    g_mutex_unlock(&active_mutex);

    g_free(ctx);
}

static gboolean on_pipe_readable(GIOChannel *source, GIOCondition condition, gpointer data)
{
    if (condition & G_IO_ERR)
    {
        log_error("edc_tools_shell: io channel error.");
        return FALSE;
    }

    if (condition & G_IO_HUP)
    {
        return FALSE;
    }

    GString *buf = data;

    gchar line[1024];
    gsize bytes_read = 0;
    GIOStatus status = g_io_channel_read_chars(source, line, sizeof(line), &bytes_read, NULL);

    if (status == G_IO_STATUS_ERROR)
    {
        return FALSE;
    }

    if (status == G_IO_STATUS_AGAIN)
    {
        return TRUE;
    }

    if (bytes_read > 0)
    {
        g_string_append_len(buf, line, bytes_read);
    }

    if (status == G_IO_STATUS_EOF)
    {
        return FALSE;
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
        gint status;
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
static gboolean on_stderr_pipe_readable(GIOChannel *source, GIOCondition cond, void *data)
{
    struct edc_tool_shell_ctx *ctx = data;
    gboolean ret = on_pipe_readable(source, cond, ctx->stderr_buf);
    if (!ret)
    {
        ctx->stderr_complete = TRUE;
    }
    edc_tool_shell_try_complete(ctx);
    return ret;
}

static gboolean on_stdout_pipe_readable(GIOChannel *source, GIOCondition cond, void *data)
{
    struct edc_tool_shell_ctx *ctx = data;
    gboolean ret = on_pipe_readable(source, cond, ctx->stdout_buf);
    if (!ret)
    {
        ctx->stdout_complete = TRUE;
    }
    edc_tool_shell_try_complete(ctx);
    return ret;
}

static void edc_tool_shell_watch_io_channel(GIOChannel *channel, GIOFunc fn, gpointer user_data)
{
    GSource *source = g_io_create_watch(channel, G_IO_IN | G_IO_HUP | G_IO_ERR);
    g_source_set_callback(source, (GSourceFunc)fn, user_data, NULL);
    g_source_attach(source, g_main_context_get_thread_default());
    g_source_unref(source);
}

static void edc_tool_shell_child_setup(gpointer user_data)
{
    struct edc_config *conf = user_data;
    edc_sandbox_apply(conf);
}

static gboolean shell_timeout_cb(gpointer data)
{
    struct edc_tool_shell_ctx *ctx = data;

    if (!ctx->child_exited)
    {
        kill(ctx->child_pid, SIGKILL);
        g_string_append(ctx->stderr_buf, "\n[command timed out]");
    }

    ctx->timeout_id = 0;
    return G_SOURCE_REMOVE;
}

static void edc_tool_exec_shell_async(struct edc_toolbox_call_ctx *tb_ctx, struct edc_tool *tool, cJSON *args,
                                     void (*callback)(struct edc_toolbox_call_ctx *, GString *, GError *))
{
    GError *error = NULL;
    cJSON *cmd = cJSON_GetObjectItemCaseSensitive(args, "command");
    if (!cmd || !cJSON_IsString(cmd))
    {
        gchar *msg = cJSON_PrintUnformatted(args);
        log_error("bc tool shell error, cmd error %s", msg);
        error = g_error_new(BC_TOOL_ERROR, BC_TOOL_ARG_INVALID, "ERROR: edc_tools_shell exec command failed: %s", msg);
        g_free(msg);
        callback(tb_ctx, NULL, error);
        return;
    }

    const gchar *cmd_s = cJSON_GetStringValue(cmd);

    guint timeout_sec = CMD_TIMEOUT_SEC;
    cJSON *timeout_json = cJSON_GetObjectItemCaseSensitive(args, "timeout");
    if (timeout_json && cJSON_IsNumber(timeout_json) && timeout_json->valueint > 0)
    {
        timeout_sec = (guint)timeout_json->valueint;
    }

    log_debug("shell tool exec: %s (timeout %us)", cmd_s, timeout_sec);

    gchar *argv[] = {"sh", "-c", (gchar *)cmd_s, NULL};

    gchar **envp = g_get_environ();
    gint env_count = g_strv_length(envp);
    gchar **filtered_envp = g_new0(gchar *, env_count + 1);
    gint j = 0;
    gsize master_key_prefix_len = strlen(BC_MASTER_KEY_ENV);
    for (gint i = 0; i < env_count; i++)
    {
        if (strncmp(envp[i], BC_MASTER_KEY_ENV, master_key_prefix_len) != 0 || envp[i][master_key_prefix_len] != '=')
        {
            filtered_envp[j++] = g_strdup(envp[i]);
        }
    }
    g_strfreev(envp);

    GPid child_pid = 0;
    gint stdin_fd = 0, stdout_fd = 0, stderr_fd = 0;
    struct edc_config *conf = tb_ctx->tb->conf;
    const gchar *working_dir = edc_sandbox_get_root(conf);
    gboolean success =
        g_spawn_async_with_pipes(working_dir, argv, filtered_envp, G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD,
                                 edc_tool_shell_child_setup, conf, &child_pid, &stdin_fd, &stdout_fd, &stderr_fd, &error);

    g_strfreev(filtered_envp);

    if (!success)
    {
        callback(tb_ctx, NULL, error);
        return;
    }

    close(stdin_fd);

    struct edc_tool_shell_ctx *ctx = g_new0(struct edc_tool_shell_ctx, 1);
    ctx->tb_ctx = tb_ctx;
    ctx->stderr_buf = g_string_new(NULL);
    ctx->stdout_buf = g_string_new(NULL);
    ctx->stderr_complete = FALSE;
    ctx->stdout_complete = FALSE;
    ctx->child_exited = FALSE;
    ctx->child_pid = child_pid;
    ctx->callback = callback;

    ctx->stdout_ch = g_io_channel_unix_new(stdout_fd);
    ctx->stderr_ch = g_io_channel_unix_new(stderr_fd);

    g_io_channel_set_flags(ctx->stdout_ch, G_IO_FLAG_NONBLOCK, NULL);
    g_io_channel_set_flags(ctx->stderr_ch, G_IO_FLAG_NONBLOCK, NULL);

    ctx->timeout_id = g_timeout_add_seconds(timeout_sec, shell_timeout_cb, ctx);

    edc_tool_shell_watch_io_channel(ctx->stderr_ch, on_stderr_pipe_readable, ctx);
    edc_tool_shell_watch_io_channel(ctx->stdout_ch, on_stdout_pipe_readable, ctx);

    g_mutex_lock(&active_mutex);
    active_contexts = g_slist_prepend(active_contexts, ctx);
    g_mutex_unlock(&active_mutex);
}

void edc_tool_shell_abort_all(void)
{
    g_mutex_lock(&active_mutex);
    GSList *p = active_contexts;
    while (p)
    {
        struct edc_tool_shell_ctx *ctx = p->data;
        if (ctx->timeout_id > 0)
        {
            g_source_remove(ctx->timeout_id);
            ctx->timeout_id = 0;
        }
        if (!ctx->child_exited)
        {
            kill(ctx->child_pid, SIGKILL);
            gint status;
            waitpid(ctx->child_pid, &status, 0);
            g_spawn_close_pid(ctx->child_pid);
            ctx->child_exited = TRUE;
            ctx->status = status;
        }
        edc_tool_shell_cleanup_channel(ctx->stdout_ch);
        edc_tool_shell_cleanup_channel(ctx->stderr_ch);
        ctx->stdout_ch = NULL;
        ctx->stderr_ch = NULL;
        p = g_slist_next(p);
    }
    g_mutex_unlock(&active_mutex);
}

struct edc_tool *edc_tool_create_exec_shell(struct edc_toolbox *tb)
{
    const gchar *name = "exec_shell";
    const gchar *desc = "Execute `sh -c '<command>'` then return output";

    GSList *params = g_slist_append(
        NULL, edc_tool_param_new("command", edc_param_str_type("The shell command to execute", NULL), TRUE));
    params = g_slist_append(params,
        edc_tool_param_new("timeout", edc_param_int_type("Timeout in seconds (default 30)", 1, 3600), FALSE));

    struct edc_tool *tool = edc_tool_new(name, desc, params, edc_tool_exec_shell_async);
    edc_toolbox_register(tb, tool);
    return tool;
}

void edc_tools_shell_register(struct edc_toolbox *tb)
{
    edc_tool_create_exec_shell(tb);
}
