#include "edc_compat.h"
#include "edc_platform.h"
#include "edc_error.h"
#include "edc_output_buf.h"
#include "edc_sandbox.h"
#include "edc_sandbox_helper.h"
#include "edc_scheduler.h"
#include "edc_shell_env.h"
#include "edc_toolbox.h"
#include "edc_tools.h"
#include "log.h"
#include "sds.h"
#include <edc_param_types.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>

#define BC_MASTER_KEY_ENV "EDGECLAW_MASTER_KEY"
#define CMD_TIMEOUT_SEC 30

/* Defaults for the two output limits (a config value > 0 overrides either).
 * CAPTURE: bytes KEPT per stream; past it the reader keeps draining and counting but stores
 *   nothing, so memory is flat whatever the command emits.
 * HARD: bytes a single stream may EMIT before the command is killed -- the runaway test. It
 *   is far above a legitimately loud build on purpose: being verbose must not get a command
 *   killed, only never stopping must. Without it a user command (which has no timeout, see
 *   edc_tools_shell_abort / the docs) could run forever. */
#define EDC_OUTPUT_CAPTURE_MAX ((size_t)1 << 20) /* 1 MiB */
#define EDC_OUTPUT_HARD_MAX ((size_t)256 << 20)  /* 256 MiB */

struct edc_tool_shell_ctx
{
    struct edc_tool *tool;
    sds command;
    struct edc_output_buf out; /* stdout: always counted, kept only up to the capture cap */
    struct edc_output_buf err; /* stderr: same */
    bool over_limit;           /* the runaway cap was hit and the child was killed */
    bool timed_out;            /* the timeout fired and the child was killed */
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

static void alloc_buffer(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf)
{
    (void)handle;
    buf->base = malloc(suggested_size);
    buf->len = suggested_size;
}

static void shell_ctx_destroy(struct edc_tool_shell_ctx *ctx)
{
    edc_output_buf_free(&ctx->out);
    edc_output_buf_free(&ctx->err);
    if (ctx->command)
        sdsfree(ctx->command);
    free(ctx);
}

/* Kill the child's whole process group. On POSIX the child is spawned with
 * UV_PROCESS_DETACHED, so setsid() made it the leader of its own group (pgid == pid):
 * signalling the GROUP is what also takes down grandchildren -- for `sh -c 'a && sudo b'`
 * killing only `/bin/sh` would leave the grandchild running and holding stdout/stderr open.
 * uv_process_kill() is kept as a fallback in case the group signal did not land (e.g. the
 * group is already gone). Windows has no process groups here (and the child is not
 * detached: see the spawn flags below), so only the child itself is terminated. */
static void shell_ctx_kill_group(struct edc_tool_shell_ctx *ctx)
{
    if (!ctx || ctx->child_exited)
        return;
    /* POSIX: the child is spawned detached (setsid), so it leads its own group and the
     * GROUP signal is what also takes down grandchildren -- for `sh -c 'a && sudo b'`,
     * killing only the shell leaves the grandchild running and holding the pipes open.
     * edc_platform_kill_process_group() is the no-op form where there are no process
     * groups, so the child itself is terminated here either way. */
    edc_platform_kill_process_group(ctx->process.pid);
    uv_process_kill(&ctx->process, SIGKILL);
}

/* Why the command ended is recorded as a flag, not as text appended to the output: the
 * notices are assembled at the HEAD of the result (see shell_ctx_complete), because that is
 * the part both readers keep. */

static void shell_ctx_close_handles(struct edc_tool_shell_ctx *ctx)
{
    if (--ctx->pending_handles == 0)
        shell_ctx_destroy(ctx);
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

/* The two limits, each overridable by config (a value > 0 wins; 0 = the compile-time
 * default above). Kept as small readers so the policy has one home. */
static size_t edc_shell_capture_max(const struct edc_config *conf)
{
    return (conf && conf->shell_capture_max > 0) ? (size_t)conf->shell_capture_max : EDC_OUTPUT_CAPTURE_MAX;
}

static size_t edc_shell_hard_max(const struct edc_config *conf)
{
    return (conf && conf->shell_output_kill_max > 0) ? (size_t)conf->shell_output_kill_max : EDC_OUTPUT_HARD_MAX;
}

/* The runaway test, after every read: a command that never stops has to end by itself,
 * because a user-typed command has no timeout. The capture cap alone would keep memory flat
 * but leave the task running until the app exits -- which is exactly what must not happen.
 * Killing is therefore the second half of the answer; `!yes` is the case this exists for. */
static void shell_ctx_check_runaway(struct edc_tool_shell_ctx *ctx)
{
    if (!ctx || ctx->over_limit || ctx->child_exited)
        return;

    size_t hard = edc_shell_hard_max(ctx->tb_ctx ? ctx->tb_ctx->tb->conf : NULL);
    if (!edc_output_buf_over(&ctx->out, hard) && !edc_output_buf_over(&ctx->err, hard))
        return;

    ctx->over_limit = true;
    shell_ctx_kill_group(ctx);
}

static void on_stdout_read(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf)
{
    struct edc_tool_shell_ctx *ctx = stream->data;
    if (nread > 0)
        edc_output_buf_append(&ctx->out, buf->base, (size_t)nread);
    if (nread != UV_EOF && nread < 0)
        log_error("edc_tools_shell: stdout read error: %s", uv_strerror((int)nread));
    free(buf->base);
    if (nread > 0)
        shell_ctx_check_runaway(ctx);
    if (nread == UV_EOF || nread < 0)
    {
        ctx->stdout_complete = true;
        uv_read_stop(stream);
        shell_ctx_try_complete(ctx);
    }
}

static void on_stderr_read(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf)
{
    struct edc_tool_shell_ctx *ctx = stream->data;
    if (nread > 0)
        edc_output_buf_append(&ctx->err, buf->base, (size_t)nread);
    if (nread != UV_EOF && nread < 0)
        log_error("edc_tools_shell: stderr read error: %s", uv_strerror((int)nread));
    free(buf->base);
    if (nread > 0)
        shell_ctx_check_runaway(ctx);
    if (nread == UV_EOF || nread < 0)
    {
        ctx->stderr_complete = true;
        uv_read_stop(stream);
        shell_ctx_try_complete(ctx);
    }
}

static void on_shell_exit(uv_process_t *process, int64_t exit_status, int term_signal)
{
    struct edc_tool_shell_ctx *ctx = process->data;
    ctx->child_exited = true;
    ctx->exit_status = term_signal ? -term_signal : (int)exit_status;
    shell_ctx_try_complete(ctx);
}

static void shell_ctx_complete(struct edc_tool_shell_ctx *ctx)
{
    if (ctx->completed)
        return;
    ctx->completed = true;

    uv_timer_stop(&ctx->timeout_timer);

    sds out = ctx->out.data ? ctx->out.data : sdsempty();
    sds err = ctx->err.data ? ctx->err.data : sdsempty();
    sdstrim(out, " \t\r\n");
    sdstrim(err, " \t\r\n");

    /* The notices go FIRST, and that placement is the whole point: the head is the only part
     * both readers keep -- the collapsed bubble draws just the first lines, and the model's
     * copy is cut at tool_max_output_size from the head too. Appended at the tail they would
     * be exactly the part nobody ever sees (measured: a 256 MiB `yes` reported "killed" only
     * inside the dropped tail). */
    sds result = sdsempty();
    char note[160];
    if (ctx->out.truncated)
    {
        edc_output_buf_truncation_note(&ctx->out, note, sizeof(note));
        result = sdscatprintf(result, "%s\n", note);
    }
    if (ctx->err.truncated)
    {
        edc_output_buf_truncation_note(&ctx->err, note, sizeof(note));
        result = sdscatprintf(result, "%s (stderr)\n", note);
    }
    if (ctx->over_limit)
    {
        char limit[32];
        edc_output_buf_size_str(edc_shell_hard_max(ctx->tb_ctx ? ctx->tb_ctx->tb->conf : NULL), limit, sizeof(limit));
        result = sdscatprintf(result, "[output exceeded %s — command killed]\n", limit);
    }
    if (ctx->timed_out)
        result = sdscat(result, "[command timed out]\n");

    if (sdslen(out) > 0)
        result = sdscatprintf(result, "%s\n", out);
    /* The command's error output follows its normal output with no label: a reader
     * (person or model) gets what the command produced, nothing prepended. The two
     * streams are already collected separately, so the interleaving was lost either
     * way; the label only told you where the switch happened. */
    if (sdslen(err) > 0)
        result = sdscatprintf(result, "%s\n", err);

    edc_toolbox_emit_tool_message_plain(ctx->tb_ctx, ctx->command, result);

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
        shell_ctx_kill_group(ctx);
        ctx->child_exited = true;
        ctx->exit_status = -1;
        ctx->timed_out = true;
    }
    ctx->stdout_complete = true;
    ctx->stderr_complete = true;
    shell_ctx_try_complete(ctx);
}

/* ── Spawn ─────────────────────────────────────────────────── */

/* |timeout_sec| == 0 means NO timeout: the command runs until it exits (only a
 * cancellation can end it -- see edc_tool_shell_abort). */
static void edc_shell_spawn(struct edc_toolbox_call_ctx *tb_ctx, struct edc_tool *tool, const char *cmd_s,
                            const char *cwd, unsigned int timeout_sec,
                            void (*callback)(struct edc_toolbox_call_ctx *, sds, EdcError *))
{
    uv_loop_t *loop = tb_ctx->tb->scheduler->loop;

    struct edc_tool_shell_ctx *ctx = calloc(1, sizeof(*ctx));
    ctx->tb_ctx = tb_ctx;
    {
        /* The capture cap is per call, not per read: past it the reader keeps draining and
         * counting but stores nothing (see edc_output_buf.h). */
        size_t cap = edc_shell_capture_max(tb_ctx->tb->conf);
        edc_output_buf_init(&ctx->out, cap);
        edc_output_buf_init(&ctx->err, cap);
    }
    ctx->command = sdsnew(cmd_s);
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
     *
     * stdin is deliberately left at UV_IGNORE: libuv then opens /dev/null and
     * dup2()s it onto fd 0 (src/unix/process.c, "Redirect stdin, stdout and
     * stderr to /dev/null even if UV_IGNORE is set"), so anything that reads
     * stdin gets an immediate EOF. Do NOT switch this to UV_INHERIT_STDIO:
     * the app has no way to type into the child, so sharing our stdin would
     * only let the command steal TUI keystrokes.
     */
    uv_stdio_container_t stdio[3];
    stdio[0].flags = UV_IGNORE;
    stdio[1].flags = UV_CREATE_PIPE | UV_WRITABLE_PIPE;
    stdio[1].data.stream = (uv_stream_t *)&ctx->stdout_pipe;
    stdio[2].flags = UV_CREATE_PIPE | UV_WRITABLE_PIPE;
    stdio[2].data.stream = (uv_stream_t *)&ctx->stderr_pipe;

    uv_process_options_t options = {0};
    options.exit_cb = on_shell_exit;
    /*
     * POSIX: run the child in its own session (setsid, via libuv
     * UV_PROCESS_DETACHED) so that it has NO controlling terminal. stdin is
     * already /dev/null, but interactive tools ignore stdin and prompt on
     * /dev/tty instead (sudo, ssh, git without GIT_TERMINAL_PROMPT=0, apt, ...).
     * While the child inherited our terminal those prompts were written straight
     * over the TUI and then blocked on a read nobody could answer, so the tool
     * call hung until its timeout and the screen was left garbled. Detached,
     * open("/dev/tty") fails and the command aborts immediately with a message
     * ("sudo: no tty present and no askpass program specified") that reaches
     * the model on stderr, which can then retry without an interactive step.
     *
     * Windows: DETACHED_PROCESS has the same name but a different effect --
     * libuv maps UV_PROCESS_DETACHED to DETACHED_PROCESS, and a console program
     * started that way (powershell.exe here) exits without writing to the
     * redirected stdout/stderr pipes: every call returned an empty result.
     * UV_PROCESS_WINDOWS_HIDE gives what we actually want -- no console window,
     * no interactive prompt surfaces -- while keeping the redirected pipes
     * connected. It also leaves the child in libuv's job object, so it does not
     * outlive the app.
     */
    /* Which flags is the same platform fact, one line up: detach on POSIX (setsid), hide
     * on Windows (no console, pipes connected). */
    const struct edc_platform_caps *caps = edc_platform_caps();
    options.flags |= (caps->spawn_mode == EDC_SPAWN_HIDE) ? UV_PROCESS_WINDOWS_HIDE : UV_PROCESS_DETACHED;

    const char *spawn_file = NULL;
    char **spawn_args = NULL;
    char *plain_args[EDC_SHELL_ARGV_HEAD_MAX + 2] = {0};
    sds sandbox_helper_file = NULL;
    char **sandbox_helper_args = NULL;
    char **spawn_env = NULL;
    /* The shell is a platform FACT (see edc_platform.h), not a decision made here: the
     * table holds the file to spawn and the argument prefix ("sh" "-c", or
     * "powershell.exe" "-NonInteractive" "-Command"), and the command line is appended.
     * Keeping it in the table is what stops the three places that care about the shell --
     * this one, the kill above and the tests -- from each knowing their own answer. */
    /* Windows: the encoding prologue goes IN FRONT of the command line (the "<- why" lives
     * with the field in edc_platform.h). It is glued to the command rather than passed
     * separately because the shell takes ONE command line, and `A;B` means "run A, then B" --
     * the prologue already ends with ';', so no separator is introduced here. POSIX has no
     * prologue, so on POSIX this is a NULL test and nothing else. */
    sds full_cmd = NULL;
    const char *cmd_line = cmd_s;
    if (caps->shell_prologue)
        cmd_line = full_cmd = sdscat(sdsnew(caps->shell_prologue), cmd_s);

    int argv_n = 0;
    for (const char *const *a = caps->shell_argv_head;
         a && *a && argv_n < (int)G_N_ELEMENTS(plain_args) - 2; a++)
        plain_args[argv_n++] = (char *)*a;
    plain_args[argv_n++] = (char *)cmd_line;
    plain_args[argv_n] = NULL;
    spawn_file = caps->shell_file;
    spawn_args = plain_args;

    /* Where there IS a kernel sandbox, run the command through the embedded helper: it
     * applies Landlock/seccomp in its own fresh process and then execs the shell, keeping
     * the pid, so the timeout kill and the exit handling below are unaffected. It cannot
     * be applied here (that would sandbox the app) nor between fork and exec (not
     * async-signal-safe in this multithreaded process). The table above is the fallback. */
    if (caps->kernel_sandbox && tb_ctx->tb->conf && tb_ctx->tb->conf->enable_sandbox &&
        edc_sandbox_helper_prepare(tb_ctx->tb->conf, cmd_s, &sandbox_helper_file, &sandbox_helper_args))
    {
        spawn_file = sandbox_helper_file;
        spawn_args = sandbox_helper_args;
    }
    options.file = spawn_file;
    options.args = spawn_args;
    /* Minimal whitelist, never NULL: NULL makes uv_spawn inherit the app's
     * entire environment (see edc_shell_env.h for why that leaked secrets). */
    spawn_env = edc_shell_env_build();
    options.env = spawn_env;
    options.cwd = cwd;
    options.stdio = stdio;
    options.stdio_count = 3;

    ctx->process.data = ctx;
    int r = uv_spawn(loop, &ctx->process, &options);

    /* libuv has consumed argv/envp by now. */
    if (sandbox_helper_file)
        sdsfree(sandbox_helper_file);
    free(sandbox_helper_args);
    edc_shell_env_free(spawn_env);
    sdsfree(full_cmd); /* libuv has built the command line by now */

    if (r != 0)
    {
        EdcError *error = edc_error_new(EDC_ERR_EXEC, "uv_spawn failed: %s", uv_strerror(r));
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
    uv_read_start((uv_stream_t *)&ctx->stdout_pipe, alloc_buffer, on_stdout_read);
    uv_read_start((uv_stream_t *)&ctx->stderr_pipe, alloc_buffer, on_stderr_read);

    /* 0 = no timeout at all (the timer stays unstarted; it is still closed when the
     * context completes, which uv_close accepts for a never-started handle). */
    if (timeout_sec > 0)
        uv_timer_start(&ctx->timeout_timer, on_timeout, (uint64_t)timeout_sec * 1000, 0);

    uv_once(&active_mutex_once, active_mutex_init);
    uv_mutex_lock(&active_mutex);
    LL_PREPEND(active_contexts, ctx);
    uv_mutex_unlock(&active_mutex);
}

static void edc_tool_exec_shell_async(struct edc_toolbox_call_ctx *tb_ctx, struct edc_tool *tool, cJSON *args,
                                      void (*callback)(struct edc_toolbox_call_ctx *, sds, EdcError *))
{
    EdcError *error = NULL;
    cJSON *cmd = cJSON_GetObjectItemCaseSensitive(args, "command");
    if (!cmd || !cJSON_IsString(cmd))
    {
        char *msg = cJSON_PrintUnformatted(args);
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

    /* A user-typed command (`!cmd`) is a TOOL-ONLY task, and it runs with NO timeout: the
     * user owns this machine and bounds the command themselves when they want one
     * (`!timeout 60 make -j8`). An LLM-issued call keeps the default cap -- a model that
     * picked a hanging command must not hang the task forever. Either way a cancellation
     * kills the child (the tool's abort hook, edc_tool_shell_abort). */
    const bool user_command = tb_ctx->task && tb_ctx->task->tool_only;

    const char *working_dir = NULL;
    cJSON *wd = cJSON_GetObjectItemCaseSensitive(args, "working_dir");
    if (wd && cJSON_IsString(wd))
        working_dir = cJSON_GetStringValue(wd);

    struct edc_config *conf = tb_ctx->tb->conf;
    sds allocated_cwd = NULL;
    /* Default cwd: the session's bound working directory, falling back to the
     * launch directory (sandbox root) when the session is unbound. */
    const char *cwd = edc_scheduler_task_workdir(tb_ctx->task);
    if (!cwd)
        cwd = edc_sandbox_get_root(conf);
    if (working_dir)
    {
        allocated_cwd = edc_sandbox_verify_path_at(conf, cwd, working_dir, false, &error);
        if (!allocated_cwd)
        {
            callback(tb_ctx, NULL, error);
            return;
        }
        cwd = allocated_cwd;
    }

    edc_shell_spawn(tb_ctx, tool, cmd_s, cwd, user_command ? 0 : timeout_sec, callback);
    sdsfree(allocated_cwd);
}

/* The tool's `abort` hook (see edc_tool_abort_fn): kill THIS call's child.
 *
 * The call context the actor hands back is exactly how this module finds its own per-call
 * state -- the previous version took a request_id and scanned the registry, which meant the
 * port implementation had to know this module's internals. Nothing here closes handles: the
 * child's death makes the read callbacks hit EOF, and the ordinary completion path
 * (shell_ctx_complete) closes and frees the context. The whole process group is signalled,
 * the same rule the timeout uses, so a `sh -c 'a && b'` grandchild cannot survive and hold
 * the pipes open. */
static void edc_tool_shell_abort(struct edc_toolbox_call_ctx *call)
{
    if (!call)
        return;

    uv_once(&active_mutex_once, active_mutex_init);
    uv_mutex_lock(&active_mutex);
    struct edc_tool_shell_ctx *ctx;
    LL_FOREACH(active_contexts, ctx)
    {
        if (ctx->tb_ctx != call)
            continue;
        shell_ctx_kill_group(ctx);
    }
    uv_mutex_unlock(&active_mutex);
}

void edc_tool_shell_abort_all(void)
{
    uv_once(&active_mutex_once, active_mutex_init);
    uv_mutex_lock(&active_mutex);
    struct edc_tool_shell_ctx *ctx, *tmp;
    LL_FOREACH_SAFE(active_contexts, ctx, tmp)
    {
        LL_DELETE(active_contexts, ctx);
        if (!ctx->child_exited)
        {
            /* SIGKILL everywhere: libuv ignores the signal NUMBER on Windows (it calls
             * TerminateProcess), and this is a teardown -- there is nobody left to ask politely. */
            uv_process_kill(&ctx->process, SIGKILL);
        }
        uv_timer_stop(&ctx->timeout_timer);

        /* Only close handles that are still open. Teardown order in
         * edc_app_free() runs edc_actor_runner_free() (which stops the actor
         * loop and uv_walk()s every handle on it shut with a NULL callback)
         * *before* edc_toolbox_free() lands here, so by now these handles are
         * usually already closed. Calling uv_close() on a closing/closed handle
         * trips libuv's assert(!uv__is_closing(handle)) and aborts the whole
         * process (SIGABRT during teardown).
         *
         * count only the handles we close here: those are the only ones whose
         * close callback will decrement ctx->pending_handles, so the context is
         * released exactly when the last of *our* callbacks fires. */
        int to_close = 0;
        if (!uv_is_closing((uv_handle_t *)&ctx->stdout_pipe))
        {
            uv_close((uv_handle_t *)&ctx->stdout_pipe, on_stdout_close);
            to_close++;
        }
        if (!uv_is_closing((uv_handle_t *)&ctx->stderr_pipe))
        {
            uv_close((uv_handle_t *)&ctx->stderr_pipe, on_stderr_close);
            to_close++;
        }
        if (!uv_is_closing((uv_handle_t *)&ctx->timeout_timer))
        {
            uv_close((uv_handle_t *)&ctx->timeout_timer, on_timer_close);
            to_close++;
        }
        if (!uv_is_closing((uv_handle_t *)&ctx->process))
        {
            uv_close((uv_handle_t *)&ctx->process, on_process_close);
            to_close++;
        }

        if (to_close == 0)
        {
            /* Every handle was already closed elsewhere (the runner walk closed
             * them with a NULL callback), so no callback will run to release the
             * context: do it here. */
            shell_ctx_destroy(ctx);
        }
        else
        {
            ctx->pending_handles = to_close;
        }
    }
    uv_mutex_unlock(&active_mutex);
}

struct edc_tool *edc_tool_create_exec_shell(struct edc_toolbox *tb)
{
    const char *name = "exec_shell";
    const char *desc = "Execute shell command and return output";
    struct edc_tool_param *params = NULL;
    LL_APPEND(params, edc_tool_param_new("command", edc_param_str_type("The shell command to execute", NULL), true));
    LL_APPEND(params,
              edc_tool_param_new("timeout", edc_param_int_type("Timeout in seconds (default 30)", 1, 3600), false));
    LL_APPEND(params,
              edc_tool_param_new("working_dir",
                                 edc_param_str_type("Working directory (relative to sandbox root)", NULL), false));
    struct edc_tool *tool = edc_tool_new(name, desc, params, edc_tool_exec_shell_async);
    /* A running command is the one thing here that must be killed when its task is
     * cancelled -- which matters most for `!cmd`, whose commands have no timeout. */
    edc_tool_set_abort(tool, edc_tool_shell_abort);
    edc_toolbox_register(tb, tool);
    return tool;
}

void edc_tools_shell_register(struct edc_toolbox *tb)
{
    edc_tool_create_exec_shell(tb);
}
