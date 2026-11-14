#include "edc_upgrade_run.h"

#include "edc_compat.h"
#include "edc_https_client.h"
#include "edc_upgrade.h"
#include "log.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

typedef struct edc_upgrade_run edc_upgrade_run_t;

/* How much of the installer's output is kept for the failure message. Enough for a shell
 * error with context, far below anything that could matter for memory. */
#define OUTPUT_KEEP (4u << 10)

/* How many handles have to close before the struct can be freed: two pipes, the process, the
 * async, and the timeout timer. Counted rather than assumed, because a close that is skipped
 * (a handle that was never initialised) must not hold the struct alive forever. */
enum
{
    H_STDOUT = 0,
    H_STDERR,
    H_PROCESS,
    H_ASYNC,
    H_TIMER,
    H_COUNT
};

struct edc_upgrade_run
{
    uv_loop_t *loop;

    /* Stage 1, on the worker thread. */
    uv_thread_t worker;
    bool worker_started;
    sds cacert;
    sds tag;
    sds script_url;
    sds install_dir;
    int timeout_ms;
    sds script_path; /* written by the worker, read by the loop */
    sds fetch_error; /* set by the worker when the fetch failed */
    uv_async_t fetched;

    /* Stage 2, on the loop. */
    uv_process_t child;
    uv_pipe_t out_pipe;
    uv_pipe_t err_pipe;
    uv_timer_t deadline;
    sds output; /* the tail of stdout+stderr, for the failure message */
    /* A chunk boundary can land between ESC and the byte that says what kind of sequence it
     * is; the stripper has to remember that across reads. Per run, not a static: two upgrades
     * cannot run at once today, but a static would make that assumption invisible. */
    bool output_esc_pending;
    bool spawned;
    bool timed_out;

    /* Reporting waits for BOTH the child's exit and the end of its output. libuv does not
     * order them: the exit callback can fire while a pipe still has unread bytes, and
     * reporting there dropped stderr from the detail -- which is the half a user needs (it
     * is where an installer writes its errors). */
    bool exit_received;
    int exit_status;
    int term_signal;
    int pipes_open;
    bool reported;

    edc_upgrade_done_fn on_done;
    void *ud;

    /* Teardown accounting. |inited[H_COUNT]| records which handles were initialised, so
     * stop() does not close a struct member that libuv never knew about.
     *
     * |refs| is shared ownership between the caller and the handles: one for the caller, one
     * per initialised handle. Whichever lets go last frees. Without it two owners raced --
     * the last close callback freed the struct while the caller still had a pointer, which
     * ASan reported as a heap-use-after-free inside edc_upgrade_run_stop. */
    bool inited[H_COUNT];
    bool stopped; /* the caller has given up its reference */
    int refs;
};

/* ── stage 1: fetch the installer ───────────────────────────────────────── */

struct fetch_state
{
    uv_mutex_t m;
    uv_cond_t c;
    int done;
    int status;
    int err;
    sds body;
    size_t max;
    bool over;
};

static void fetch_on_event(struct edc_https_request *req, int ev, void *data)
{
    (void)data;
    struct fetch_state *st = edc_https_request_get_priv(req);
    if (!st)
        return;
    switch (ev)
    {
    case EDC_HTTPS_EVENT_HS: {
        const char *path = edc_https_request_get_path(req);
        const char *host = edc_https_request_get_host(req);
        sds hdr = sdscatprintf(sdsempty(),
                               "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: edgeclaw-upgrade\r\n"
                               "Connection: close\r\n\r\n",
                               path && path[0] ? path : "/", host ? host : "");
        edc_https_request_write(req, (const unsigned char *)hdr, (size_t)sdslen(hdr));
        sdsfree(hdr);
        break;
    }
    case EDC_HTTPS_EVENT_HEADER:
        st->status = edc_https_request_get_status(req);
        break;
    case EDC_HTTPS_EVENT_MSG: {
        uv_buf_t *buf = data;
        if (buf && buf->base)
        {
            if (!st->over && st->body && sdslen(st->body) + buf->len > st->max)
                st->over = true;
            if (!st->over && st->body)
                st->body = sdscatlen(st->body, buf->base, buf->len);
            free(buf->base);
        }
        break;
    }
    case EDC_HTTPS_EVENT_ERROR:
        st->err = 1;
        break;
    case EDC_HTTPS_EVENT_CLOSE:
        uv_mutex_lock(&st->m);
        st->done = 1;
        uv_cond_signal(&st->c);
        uv_mutex_unlock(&st->m);
        break;
    default:
        break;
    }
}

/* One GET, synchronously, with a deadline. Returns an owned string or NULL.
 *
 * The installer is a few KB; a 256 KiB cap is generous and keeps a hostile or broken response
 * from being accumulated without end. */
static sds fetch_text(const char *cacert, const char *url, int timeout_ms)
{
    struct edc_https_client *client = edc_https_client_new(cacert);
    if (!client)
        return NULL;

    struct fetch_state st;
    memset(&st, 0, sizeof(st));
    uv_mutex_init(&st.m);
    uv_cond_init(&st.c);
    st.body = sdsempty();
    st.max = 256u << 10;

    sds result = NULL;
    if (edc_https_client_connect(client, url, fetch_on_event, &st) == 0)
    {
        uv_mutex_lock(&st.m);
        while (!st.done)
        {
            if (uv_cond_timedwait(&st.c, &st.m, (uint64_t)(timeout_ms > 0 ? timeout_ms : 1) * 1000000ULL) != 0)
                break;
        }
        const int done = st.done, status = st.status, err = st.err;
        const bool over = st.over;
        uv_mutex_unlock(&st.m);

        if (done && !err && !over && status == 200 && sdslen(st.body) > 0)
            result = sdsdup(st.body);
    }

    sdsfree(st.body);
    uv_cond_destroy(&st.c);
    uv_mutex_destroy(&st.m);
    edc_https_client_free(client);
    return result;
}

/* Write |body| to a 0700 temp file and return its path, or NULL. */
static sds write_script(const sds body)
{
    const char *tmpdir = getenv("TMPDIR");
    if (!tmpdir || !tmpdir[0])
        tmpdir = "/tmp";

#ifdef _WIN32
    /* No mkstemp, and no shell to run the script anyway; the Windows path is refused before
     * reaching here (see edc_upgrade_argv). */
    (void)body;
    return NULL;
#else
    char tmpl[512];
    snprintf(tmpl, sizeof(tmpl), "%s/edgeclaw-install-XXXXXX", tmpdir);
    const int fd = mkstemp(tmpl);
    if (fd < 0)
        return NULL;

    size_t off = 0;
    while (off < sdslen(body))
    {
        const ssize_t n = write(fd, body + off, sdslen(body) - off);
        if (n <= 0)
        {
            close(fd);
            unlink(tmpl);
            return NULL;
        }
        off += (size_t)n;
    }
    /* 0700 already from mkstemp; make the intent explicit and make sure an inherited umask
     * did not widen it. */
    if (fchmod(fd, 0700) != 0)
        log_warn("upgrade: cannot set permissions on %s: %s", tmpl, strerror(errno));
    close(fd);
    return sdsnew(tmpl);
#endif
}

/* ── stage 2: run it ────────────────────────────────────────────────────── */

static void run_unref(edc_upgrade_run_t *run)
{
    if (!run || --run->refs > 0)
        return;
    sdsfree(run->cacert);
    sdsfree(run->tag);
    sdsfree(run->script_url);
    sdsfree(run->script_path);
    sdsfree(run->fetch_error);
    sdsfree(run->output);
    sdsfree(run->install_dir);
    free(run);
}

static void run_on_close(uv_handle_t *handle)
{
    run_unref(handle->data);
}

/* Close one handle and take a reference for its close callback. |refs| is bumped BEFORE
 * uv_close, because the callback may run on the next loop iteration -- after this function
 * has returned and the caller's reference may already be gone. */
static void run_close(edc_upgrade_run_t *run, int which)
{
    if (!run->inited[which])
        return;
    run->inited[which] = false;
    run->refs++;

    switch (which)
    {
    case H_STDOUT:
        uv_close((uv_handle_t *)&run->out_pipe, run_on_close);
        break;
    case H_STDERR:
        uv_close((uv_handle_t *)&run->err_pipe, run_on_close);
        break;
    case H_PROCESS:
        uv_close((uv_handle_t *)&run->child, run_on_close);
        break;
    case H_ASYNC:
        uv_close((uv_handle_t *)&run->fetched, run_on_close);
        break;
    case H_TIMER:
        uv_timer_stop(&run->deadline);
        uv_close((uv_handle_t *)&run->deadline, run_on_close);
        break;
    default:
        run->refs--;
        break;
    }
}

/* Drop ANSI escape sequences and other control bytes from installer output.
 *
 * The installer is a shell script run under `sh`, and everything it calls -- curl, tar, apt,
 * git -- writes colour when it thinks a terminal is attached. This output ends up in the
 * dialog, where ftxui renders it as plain text: an ESC is not interpreted, it is drawn, so
 * "\x1b[1mError\x1b[0m" reached the reader as "[1mError[0m" and the sequence's bytes shifted
 * every colour and column after it. That is what "one part is white, one part is grey" was.
 *
 * Stripped at the source rather than in the dialog: the escape is not information, and the
 * dialog is not the only thing that may one day show this text (a log, a report). Handles CSI
 * ("ESC [ ... final"), OSC ("ESC ] ... BEL/ST") and two-byte escapes; anything else below 0x20
 * except tab and newline is dropped, and a lone trailing ESC is held back rather than emitted
 * half-parsed.
 */
static sds run_strip_escapes(const char *data, size_t len, bool *pending_esc)
{
    sds clean = sdsempty();
    size_t i = 0;
    bool in_esc = pending_esc ? *pending_esc : false;

    while (i < len)
    {
        const unsigned char c = (unsigned char)data[i];

        if (in_esc)
        {
            if (c == '[')
            {
                /* CSI: parameters then a final byte in 0x40..0x7E. */
                i++;
                while (i < len)
                {
                    const unsigned char d = (unsigned char)data[i];
                    if (d >= 0x40 && d <= 0x7E)
                    {
                        i++;
                        in_esc = false;
                        break;
                    }
                    i++;
                }
                if (in_esc && i >= len)
                    break; /* sequence continues in the next chunk */
                continue;
            }
            if (c == ']')
            {
                /* OSC: ends with BEL or ESC \. */
                i++;
                while (i < len)
                {
                    const unsigned char d = (unsigned char)data[i];
                    if (d == 0x07)
                    {
                        i++;
                        in_esc = false;
                        break;
                    }
                    if (d == 0x1B && i + 1 < len && data[i + 1] == '\\')
                    {
                        i += 2;
                        in_esc = false;
                        break;
                    }
                    i++;
                }
                if (in_esc && i >= len)
                    break;
                continue;
            }
            /* Any other two-byte escape: drop ESC and the byte after it. */
            i++;
            in_esc = false;
            continue;
        }

        if (c == 0x1B)
        {
            if (i + 1 >= len)
            {
                /* A chunk boundary landed between ESC and its introducer. Hold it. */
                in_esc = true;
                i++;
                break;
            }
            in_esc = true;
            i++;
            continue;
        }

        if (c == '\t' || c == '\n' || c == '\r' || c >= 0x20)
        {
            /* '\r' is kept: the installer writes progress with it, and carriage returns make
             * a mess of the wrapped dialog but deleting them joins words. The caller's
             * line-splitting turns each into a break. */
            clean = sdscatlen(clean, &data[i], 1);
        }
        /* Any other control byte (< 0x20) is dropped. */
        i++;
    }

    if (pending_esc)
        *pending_esc = in_esc;
    return clean;
}

/* Append to the output tail, keeping only the last OUTPUT_KEEP bytes.
 *
 * Stripped on the way in, not on the way out: the tail window is a fixed byte budget, and an
 * escape sequence split by that boundary would leave half of one at the front of the text --
 * exactly the kind of fragment that used to reach the reader. */
static void run_keep_output(edc_upgrade_run_t *run, const char *data, size_t len)
{
    if (!run->output)
        return;

    sds clean = run_strip_escapes(data, len, &run->output_esc_pending);
    run->output = sdscatsds(run->output, clean);
    sdsfree(clean);

    if (sdslen(run->output) > OUTPUT_KEEP)
    {
        const size_t cut = sdslen(run->output) - OUTPUT_KEEP;
        memmove(run->output, run->output + cut, OUTPUT_KEEP);
        sdssetlen(run->output, OUTPUT_KEEP);
    }
}

/* Report once, and only when there is something complete to report. */
static void run_ready_to_report(edc_upgrade_run_t *run)
{
    if (run->reported || !run->exit_received)
        return;
    /* Normally wait for both pipes: their bytes are the installer's own explanation of what
     * went wrong. But a deadline kill can leave a grandchild (sh -c 'sleep 60') holding the
     * write end, so the pipe never reaches EOF -- waiting for it then means never reporting at
     * all. After a timeout the exit status is the whole story, so it is reported without the
     * output. */
    if (run->pipes_open > 0 && !run->timed_out)
        return;
    run->reported = true;

    const bool ok = !run->timed_out && run->term_signal == 0 && run->exit_status == 0;
    sds detail = sdsempty();
    if (run->timed_out)
        detail = sdscat(detail, "the installer did not finish in time");
    else if (run->term_signal != 0)
        detail = sdscatprintf(detail, "the installer was killed (signal %d)", run->term_signal);
    else if (run->exit_status != 0)
        detail = sdscatprintf(detail, "the installer exited with status %d", run->exit_status);
    if (run->output && sdslen(run->output) > 0)
    {
        if (sdslen(detail) > 0)
            detail = sdscat(detail, "\n");
        detail = sdscat(detail, run->output);
    }

    if (run->on_done)
        run->on_done(ok, detail ? detail : "", run->ud);
    sdsfree(detail);
}

static void run_on_stdout(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf)
{
    edc_upgrade_run_t *run = stream->data;
    if (buf && buf->base)
    {
        if (nread > 0 && run)
            run_keep_output(run, buf->base, (size_t)nread);
        free(buf->base);
    }
    if (nread < 0 && run)
    {
        /* EOF (or a read error): this pipe is done. Both have to be before the report, or
         * stderr loses the race with the exit callback. */
        run_close(run, stream == (uv_stream_t *)&run->out_pipe ? H_STDOUT : H_STDERR);
        if (run->pipes_open > 0)
            run->pipes_open--;
        run_ready_to_report(run);
    }
}

static void run_alloc(uv_handle_t *handle, size_t suggested, uv_buf_t *buf)
{
    (void)handle;
    buf->base = malloc(suggested);
    buf->len = buf->base ? suggested : 0;
}

static void run_on_deadline(uv_timer_t *timer)
{
    edc_upgrade_run_t *run = timer->data;
    if (!run || !run->spawned || run->timed_out)
        return;
    /* A hung installer must not hang the interface forever. Kill it; the exit callback below
     * still runs and reports the timeout. */
    run->timed_out = true;
    log_warn("upgrade: the installer did not finish in time; killing it");
    uv_process_kill(&run->child, SIGKILL);
}

static void run_on_exit(uv_process_t *process, int64_t exit_status, int term_signal)
{
    edc_upgrade_run_t *run = process->data;
    if (!run)
        return;

    uv_timer_stop(&run->deadline);
    run->spawned = false;

    /* Record, do not report: the pipes may still hold unread bytes, and reporting here is
     * what dropped stderr from the detail. run_ready_to_report() decides. */
    run->exit_received = true;
    run->exit_status = (int)exit_status;
    run->term_signal = term_signal;

    run_close(run, H_TIMER);
    run_close(run, H_PROCESS);
    run_ready_to_report(run);
}

static void run_spawn(edc_upgrade_run_t *run)
{
    if (!run->script_path)
        return;

    sds *argv = edc_upgrade_argv(EDC_UPGRADE_METHOD_INSTALLER, run->script_path, run->tag, run->install_dir);
    if (!argv)
    {
        if (run->on_done)
            run->on_done(false, edc_upgrade_unavailable_reason(EDC_UPGRADE_METHOD_INSTALLER), run->ud);
        run_close(run, H_TIMER);
        return;
    }

    static char *const no_env[] = {NULL};
    uv_process_options_t options;
    memset(&options, 0, sizeof(options));
    options.exit_cb = run_on_exit;
    options.file = argv[0];
    options.args = argv;

    /* No environment: the installer needs PATH at most, and inheriting ours would hand it the
     * app's working state for no reason. uv_spawn with a NULL env inherits everything, so an
     * empty vector is the way to say "nothing". */
    options.env = no_env;
#ifndef _WIN32
    options.flags |= UV_PROCESS_DETACHED;
#endif

    uv_stdio_container_t stdio[3];
    stdio[0].flags = UV_IGNORE;
    /* WRITABLE_PIPE, not READABLE: the flag describes the CHILD's end. The child writes to
     * its stdout, so the parent holds the read end -- which is the only side uv_read_start
     * accepts. With READABLE_PIPE the parent got a write end and every uv_read_start returned
     * UV_EINVAL, so the installer's output was never captured at all and the failure detail
     * was always empty. */
    stdio[1].flags = UV_CREATE_PIPE | UV_WRITABLE_PIPE;
    stdio[1].data.stream = (uv_stream_t *)&run->out_pipe;
    stdio[2].flags = UV_CREATE_PIPE | UV_WRITABLE_PIPE;
    stdio[2].data.stream = (uv_stream_t *)&run->err_pipe;
    options.stdio = stdio;
    options.stdio_count = 3;

    if (uv_pipe_init(run->loop, &run->out_pipe, 0) != 0 || uv_pipe_init(run->loop, &run->err_pipe, 0) != 0)
    {
        edc_upgrade_argv_free(argv);
        if (run->on_done)
            run->on_done(false, "cannot create pipes for the installer", run->ud);
        return;
    }
    run->inited[H_STDOUT] = true;
    run->inited[H_STDERR] = true;
    run->pipes_open = 2;
    run->out_pipe.data = run;
    run->err_pipe.data = run;

    const int r = uv_spawn(run->loop, &run->child, &options);
    run->child.data = run;
    edc_upgrade_argv_free(argv);

    if (r != 0)
    {
        /* uv_spawn initialises the process handle before it can fail, and the exec-failure
         * path returns without unlinking it -- see the note in edc_tools_shell.c. Closing it
         * only when libuv got that far is what keeps the handle queue from pointing into a
         * freed struct. */
        if (run->child.loop != NULL)
            run->inited[H_PROCESS] = true;
        if (run->on_done)
            run->on_done(false, uv_strerror(r), run->ud);
        run_close(run, H_STDOUT);
        run_close(run, H_STDERR);
        run_close(run, H_PROCESS);
        return;
    }

    run->inited[H_PROCESS] = true;
    run->spawned = true;
    uv_read_start((uv_stream_t *)&run->out_pipe, run_alloc, run_on_stdout);
    uv_read_start((uv_stream_t *)&run->err_pipe, run_alloc, run_on_stdout);
    uv_timer_start(&run->deadline, run_on_deadline, (uint64_t)(run->timeout_ms > 0 ? run->timeout_ms : 120000), 0);
}

/* The worker finished stage 1. */
static void run_on_fetched(uv_async_t *handle)
{
    edc_upgrade_run_t *run = handle->data;
    if (!run)
        return;

    if (!run->script_path)
    {
        if (run->on_done)
            run->on_done(false, run->fetch_error ? run->fetch_error : "cannot fetch the installer", run->ud);
        run_close(run, H_TIMER);
        return;
    }
    run_spawn(run);
}

static void run_worker(void *arg)
{
    edc_upgrade_run_t *run = arg;

    sds body = fetch_text(run->cacert, run->script_url, 15000);
    if (!body)
        run->fetch_error = sdscatprintf(sdsempty(), "cannot fetch %s", run->script_url);
    else
    {
        run->script_path = write_script(body);
        if (!run->script_path)
            run->fetch_error = sdsnew("cannot write the installer to a temporary file");
        sdsfree(body);
    }
    uv_async_send(&run->fetched);
}

edc_upgrade_run_t *edc_upgrade_run_start(uv_loop_t *loop, const char *cacert_path, const char *script_url,
                                         const char *tag, const char *install_dir, int timeout_ms,
                                         edc_upgrade_done_fn on_done, void *ud)
{
    if (!loop || !script_url || !script_url[0] || !tag || !tag[0] || !install_dir || !install_dir[0] || !on_done)
        return NULL;

    edc_upgrade_run_t *run = calloc(1, sizeof(*run));
    if (!run)
        return NULL;

    run->loop = loop;
    run->cacert = cacert_path ? sdsnew(cacert_path) : NULL;
    run->tag = sdsnew(tag);
    run->script_url = sdsnew(script_url);
    run->install_dir = sdsnew(install_dir);
    run->output = sdsempty();
    run->timeout_ms = timeout_ms > 0 ? timeout_ms : 120000;
    run->on_done = on_done;
    run->ud = ud;
    run->refs = 1; /* the caller's reference; stop() gives it up */

    if (!run->tag || !run->script_url || !run->install_dir || !run->output)
    {
        edc_upgrade_run_stop(run);
        return NULL;
    }

    if (uv_async_init(loop, &run->fetched, run_on_fetched) != 0)
    {
        edc_upgrade_run_stop(run);
        return NULL;
    }
    run->inited[H_ASYNC] = true;
    run->fetched.data = run;

    if (uv_timer_init(loop, &run->deadline) != 0)
    {
        edc_upgrade_run_stop(run);
        return NULL;
    }
    run->inited[H_TIMER] = true;
    run->deadline.data = run;

    if (uv_thread_create(&run->worker, run_worker, run) != 0)
    {
        edc_upgrade_run_stop(run);
        return NULL;
    }
    run->worker_started = true;
    return run;
}

void edc_upgrade_run_stop(edc_upgrade_run_t *run)
{
    if (!run)
        return;
    /* Idempotent: the caller may give up its reference once, and a second call must not
     * decrement it again. */
    if (run->stopped)
        return;
    run->stopped = true;

    if (run->worker_started)
    {
        uv_thread_join(&run->worker);
        run->worker_started = false;
    }

    if (run->spawned)
    {
        uv_process_kill(&run->child, SIGKILL);
        run->spawned = false;
    }

    for (int i = 0; i < H_COUNT; i++)
        run_close(run, i);

    /* Release the caller's reference. The handles hold one each until their close callbacks
     * run on the loop; the last of those frees (see run_unref). */
    run_unref(run);
}
