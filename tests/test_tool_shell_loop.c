/* tests/test_tool_shell_loop.c -- the real exec_shell tool on a real event loop.
 *
 * Why a file of its own: every other test that reaches a shell command stubs the toolbox
 * (`test_edc_scheduler.c` stubs dispatch_tools, `test_task_resume.c` stubs
 * redispatch_tools), so the actual spawn / pipe / timeout / kill machinery never ran under
 * `bazel test`. This drives it for real: a uv loop, the real toolbox, the real tool.
 *
 * What it pins (see docs/tool-output-limits.zh.md):
 *   1. a command that emits more than the capture cap is TRUNCATED, not killed, and the
 *      result says how much was kept out of how much there was;
 *   2. a command that never stops (`yes`) is KILLED by the runaway cap -- the call
 *      returning at all is the proof, since `yes` never exits by itself;
 *   3. the same holds when the volume arrives on stderr (the second buffer);
 *   4. an ordinary command is untouched by any of it.
 *
 * The limits are read from the config, so a test can pin small ones instead of having to
 * emit a real 256 MiB. */

#include "edc_actor.h"
#include "test_ws.h"
#include "edc_compat.h"
#include "edc_config.h"
#include "edc_scheduler.h"
#include "edc_toolbox.h"
#include "edc_tools.h"
#include "edc_utf8.h"
#include "log.h"

#include <cJSON.h>
#include "sds.h"
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <uv.h>

/* Stub for _run_loop pulled in by edc_commands (transitively linked). */
volatile sig_atomic_t _run_loop = 1;

static int test_failed = 0;
static int test_passed = 0;

#define TEST(name) do { log_info("  TEST: %s", name); } while (0)
#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        log_error("    FAIL: " __VA_ARGS__); \
        test_failed++; \
    } else { \
        test_passed++; \
    } \
} while (0)

/* ══════════════════════════════════════════════════════════════
 *  Harness: scheduler actor + toolbox, pumped by this thread
 *  ══════════════════════════════════════════════════════════════ */

struct harness
{
    struct edc_config *cfg;
    struct edc_scheduler *sched;
    edc_actor_t *sched_actor;
    edc_actor_runner_t *runner;
    struct edc_toolbox *tb;
    struct edc_toolbox_call_ctx *call;
};

static sds g_result; /* owned by the callback (tools hand the result over) */
static EdcError *g_error;
static bool g_done;

static void result_cb(struct edc_toolbox_call_ctx *ctx, sds result, EdcError *error)
{
    (void)ctx;
    if (g_result)
        sdsfree(g_result);
    if (g_error)
        edc_error_free(g_error);
    g_result = result; /* ownership moves here, exactly as it does to the actor */
    g_error = error;
    g_done = true;
}

static void harness_up(struct harness *h, int capture_max, int kill_max)
{
    memset(h, 0, sizeof(*h));
    h->cfg = g_new0(struct edc_config, 1);
    h->cfg->workspace = sdsnew(edc_test_workspace());
    h->cfg->enable_sandbox = false;
    /* The root a real config derives from rw_paths[0] (edc_config.c). Without it,
     * edc_sandbox_get_root() answers NULL, a relative working_dir has nothing to be
     * relative to, and the spawn silently inherits the process cwd -- so the harness has to
     * carry the field production always carries, or `working_dir` cases test nothing. */
    h->cfg->sandbox_root_real = sdsnew(edc_test_workspace());
    h->cfg->shell_capture_max = capture_max;
    h->cfg->shell_output_kill_max = kill_max;

    h->sched = edc_scheduler_new(h->cfg);
    h->sched_actor = edc_scheduler_get_actor(h->sched);

    /* The runner is what gives the actors (and therefore tb->scheduler->loop, which the
     * shell tool spawns on) a loop. The loop is pumped by this thread, not by the runner's
     * own thread, so every handler runs inline and the assertions need no locking. */
    h->runner = edc_actor_runner_new();
    edc_actor_runner_add(h->runner, h->sched_actor);

    h->tb = edc_toolbox_new(h->cfg, h->sched_actor, NULL);
}

static void harness_down(struct harness *h)
{
    /* Drain first: the tool queued a TOOL_EMIT to the scheduler actor, and that message
     * points at the call context (freed last). */
    for (int i = 0; i < 50; i++)
        uv_run(&h->runner->loop, UV_RUN_NOWAIT);

    if (g_result)
        sdsfree(g_result);
    g_result = NULL;
    if (g_error)
        edc_error_free(g_error);
    g_error = NULL;
    g_done = false;

    if (h->call)
        edc_toolbox_call_ctx_free(h->call);
    edc_toolbox_free(h->tb);
    edc_actor_runner_free(h->runner);
    edc_scheduler_free(h->sched);
    edc_config_free(h->cfg);
}

/* Start one command through the real tool. Returns at once: the callback is what ends it. */
static void start_shell(struct harness *h, cJSON *args)
{
    g_done = false;
    h->call = edc_toolbox_call_ctx_new(h->tb, edc_channel_adapter, NULL, NULL, NULL);
    edc_toolbox_exec(h->call, "exec_shell", args, result_cb);
}

/* Pump the loop until the callback fires, or |budget_ms| of wall time goes by. The budget is
 * wall time on purpose: a case that only passes because it waited long enough is a case that
 * cannot tell "killed promptly" from "ran to completion". */
static bool wait_done(struct harness *h, int budget_ms)
{
    int waited = 0;
    while (!g_done && waited < budget_ms)
    {
        uv_run(&h->runner->loop, UV_RUN_NOWAIT);
        uv_sleep(2);
        waited += 2;
    }
    return g_done;
}

static bool run_shell_args(struct harness *h, const char *cmd, int timeout_sec, const char *working_dir)
{
    cJSON *args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "command", cmd);
    if (timeout_sec > 0)
        cJSON_AddNumberToObject(args, "timeout", timeout_sec);
    if (working_dir)
        cJSON_AddStringToObject(args, "working_dir", working_dir);

    start_shell(h, args);
    cJSON_Delete(args); /* the tool copies what it needs before returning (§4.4) */
    return wait_done(h, 30000);
}

/* Run one command through the real tool and wait for its callback. */
static bool run_shell(struct harness *h, const char *cmd, int timeout_sec)
{
    return run_shell_args(h, cmd, timeout_sec, NULL);
}

static const char *result_text(void)
{
    return g_result ? g_result : "";
}

/* ══════════════════════════════════════════════════════════════
 *  Cases
 *  ══════════════════════════════════════════════════════════════ */

static void test_ordinary_command_is_untouched(void)
{
    TEST("an ordinary command: full output, no truncation, no kill");
    struct harness h;
    harness_up(&h, 64 * 1024, 64 * 1024 * 1024);

    CHECK(run_shell(&h, "echo hello-world", 10), "the callback fired");
    CHECK(strstr(result_text(), "hello-world") != NULL, "the output is there: %.60s", result_text());
    CHECK(strstr(result_text(), "truncated") == NULL, "no truncation note");
    CHECK(strstr(result_text(), "killed") == NULL, "no kill note");

    harness_down(&h);
}

/* How the command ENDED is the fact nothing else records. Before this, a failing command
 * produced output indistinguishable from a successful one, and the model reported success on
 * top of a failure -- which is why the note exists and why it is asserted here rather than
 * left to a comment. */
static void test_nonzero_exit_is_reported(void)
{
    TEST("a failing command reports [exit N] instead of looking successful");
    struct harness h;
    harness_up(&h, 64 * 1024, 64 * 1024 * 1024);

    CHECK(run_shell(&h, "echo partial-output; exit 3", 10), "the callback fired");
    CHECK(strstr(result_text(), "[exit 3]") != NULL,
          "the exit status is in the result: %.80s", result_text());
    CHECK(strstr(result_text(), "partial-output") != NULL,
          "and the output it did produce is kept");

    harness_down(&h);
}

static void test_zero_exit_is_silent(void)
{
    TEST("a successful command does NOT get an [exit 0] line");
    struct harness h;
    harness_up(&h, 64 * 1024, 64 * 1024 * 1024);

    CHECK(run_shell(&h, "true", 10), "the callback fired");
    /* `[exit 0]` on every call is noise a reader learns to skip, and skipping it is how the
     * non-zero one gets missed. Only the interesting case is announced. */
    CHECK(strstr(result_text(), "[exit") == NULL,
          "no exit line when it succeeded: %.60s", result_text());

    harness_down(&h);
}

static void test_signal_death_is_reported(void)
{
    TEST("a command killed by a signal says which one");
    struct harness h;
    harness_up(&h, 64 * 1024, 64 * 1024 * 1024);

    /* `kill -9 $$` kills the shell itself: the process exits by signal, not with a code, so
     * an exit-status-only implementation would report nothing at all here. */
    CHECK(run_shell(&h, "kill -9 $$", 10), "the callback fired");
    CHECK(strstr(result_text(), "[signal 9]") != NULL,
          "the signal is in the result: %.80s", result_text());

    harness_down(&h);
}

/* The note has to survive truncation, which is why it sits in the HEAD with the other
 * notices rather than being appended after the output. A 400000-line `seq` blows past the
 * capture cap, so anything placed at the tail would be dropped and this case would pass
 * while a real user saw nothing. */
static void test_exit_survives_truncation(void)
{
    TEST("the exit note is in the head, so truncation cannot drop it");
    struct harness h;
    harness_up(&h, 64 * 1024, 64 * 1024 * 1024);

    CHECK(run_shell(&h, "seq 1 400000; exit 7", 30), "the callback fired");
    CHECK(strstr(result_text(), "[exit 7]") != NULL,
          "the exit note is still present after truncation");
    /* The head is where it must be: the truncation note and the exit note both precede the
     * body. */
    const char *exit_at = strstr(result_text(), "[exit 7]");
    const char *body_at = strstr(result_text(), "1\n2\n");
    CHECK(exit_at && body_at && exit_at < body_at,
          "the exit note comes before the output body");

    harness_down(&h);
}

static void test_volume_is_truncated_but_not_killed(void)
{
    TEST("a loud but finite command: truncated, and it still runs to the end");
    struct harness h;
    /* keep 4 KiB, allow a lot: the runaway limit must NOT fire here. */
    harness_up(&h, 4 * 1024, 64 * 1024 * 1024);

    /* ~2.2 MiB of output. */
    CHECK(run_shell(&h, "seq 1 400000", 30), "the callback fired");
    CHECK(strstr(result_text(), "[output truncated: kept the first 4.0 KiB of") != NULL,
          "the note states kept-of-total: %.120s", result_text());
    CHECK(strncmp(result_text(), "[output truncated:", 18) == 0,
          "and it is at the HEAD of the result -- the tail is what both readers drop: %.80s",
          result_text());
    CHECK(strstr(result_text(), "killed") == NULL, "a verbose command is not killed");
    CHECK(strstr(result_text(), "1\n2\n3\n") != NULL, "the head of the output survives");
    CHECK(g_error == NULL, "no error");

    harness_down(&h);
}

static void test_runaway_is_killed(void)
{
    TEST("`yes` (stdout): the runaway cap kills it -- returning at all is the proof");
    struct harness h;
    harness_up(&h, 4 * 1024, 64 * 1024); /* kill after 64 KiB */

    CHECK(run_shell(&h, "yes", 30), "the call came back (a `yes` that never ends would not)");
    CHECK(strstr(result_text(), "command killed") != NULL, "the result says why: %.120s", result_text());
    CHECK(strstr(result_text(), "[output exceeded 64.0 KiB") != NULL, "and quotes the limit");
    CHECK(strstr(result_text(), "command killed") < result_text() + 200,
          "the notice is in the head, where a reader cut at 10000 chars still sees it");

    harness_down(&h);
}

static void test_runaway_on_stderr_is_killed_too(void)
{
    TEST("`yes 1>&2` (stderr): the second buffer is bounded the same way");
    struct harness h;
    harness_up(&h, 4 * 1024, 64 * 1024);

    CHECK(run_shell(&h, "yes 1>&2", 30), "the call came back");
    CHECK(strstr(result_text(), "command killed") != NULL, "killed on stderr volume too");

    harness_down(&h);
}

/* The encoding invariant, end to end: whatever the command is, the result of a tool call is
 * valid UTF-8. On Windows that is not free -- PowerShell encodes its own text with the console
 * code page, and a program that follows the CRT locale encodes independently of it -- so this
 * is the case that proves the prologue and the boundary conversion TOGETHER, and that tells
 * them apart: a run where the prologue did the work reports NO conversion.
 *
 * The GBK bytes are written straight to the pipe rather than by shelling out to a
 * locale-dependent program, so the case does not need python (or anything else) installed. */
static void test_cjk_output_is_utf8(void)
{
    TEST("Chinese output: a tool result is valid UTF-8, however the command encoded it");
    struct harness h;
    harness_up(&h, 64 * 1024, 64 * 1024 * 1024);

#if defined(_WIN32)
    /* PowerShell's own text: what the prologue is for. */
    CHECK(run_shell(&h, "Write-Output '\xE4\xB8\xAD\xE6\x96\x87PS'", 10), "the callback fired");
    CHECK(edc_utf8_is_valid(result_text(), strlen(result_text())), "valid UTF-8: %.80s", result_text());
    CHECK(strstr(result_text(), "\xE4\xB8\xAD\xE6\x96\x87PS") != NULL, "and the text is intact: %.80s",
          result_text());
    /* No conversion here means the SHELL produced UTF-8 -- i.e. the prologue worked. Without
     * this the two mechanisms mask each other: a broken prologue would still pass, because the
     * boundary conversion would quietly fix the mojibake afterwards. */
    CHECK(strstr(result_text(), "converted") == NULL, "the prologue did the work: nothing had to be converted");

    /* A program that encodes with the CRT locale: raw GBK bytes on the pipe. */
    CHECK(run_shell(&h, "[Console]::OpenStandardOutput().Write([byte[]](0xBC,0xD7,0x0A),0,3)", 10),
          "the callback fired");
    CHECK(edc_utf8_is_valid(result_text(), strlen(result_text())), "valid UTF-8: %.80s", result_text());
    CHECK(strstr(result_text(), "\xE7\x94\xB2") != NULL, "the GBK line became the right character: %.80s",
          result_text());
    CHECK(strstr(result_text(), "line(s) converted") != NULL, "and the conversion is announced: %.80s",
          result_text());

    /* Both encodings in ONE invocation -- exactly what a whole-buffer decision would corrupt. */
    CHECK(run_shell(&h,
                    "[Console]::OpenStandardOutput().Write([byte[]](0xBC,0xD7,0x0A),0,3);"
                    "Write-Output '\xE4\xB8\xAD\xE6\x96\x87PS'",
                    10),
          "the callback fired");
    CHECK(edc_utf8_is_valid(result_text(), strlen(result_text())), "valid UTF-8: %.80s", result_text());
    CHECK(strstr(result_text(), "\xE7\x94\xB2") != NULL, "the GBK line survives: %.80s", result_text());
    CHECK(strstr(result_text(), "\xE4\xB8\xAD\xE6\x96\x87PS") != NULL, "and so does the UTF-8 line: %.80s",
          result_text());
#else
    /* POSIX: no conversion AND no validation -- the bytes come back exactly as written.
     * The tool appends its OWN single trailing newline (it sdstrim()s the command's, then
     * formats "%s\n"), so the expected text is the raw bytes plus that one newline. */
    CHECK(run_shell(&h, "printf '\xE4\xB8\xAD\xE6\x96\x87SH\\n'", 10), "the callback fired");
    CHECK(strcmp(result_text(), "\xE4\xB8\xAD\xE6\x96\x87SH\n") == 0,
          "the bytes come back untouched: %.80s", result_text());
#endif

    harness_down(&h);
}

/* A model that picks a hanging command must not hang the task: the timeout is what ends it.
 *
 * Two facts are asserted, and the second is the one that matters. The result says
 * "[command timed out]" -- but the notice alone would also be produced by an implementation
 * that WAITED the full five seconds and then claimed a timeout. So the case also measures:
 * the call has to come back well before the child would have exited by itself. That is the
 * difference between "killed it" and "lied about killing it". */
static void test_timeout_kills_the_child(void)
{
    TEST("a hanging command is killed by its timeout, promptly, and says so");
    struct harness h;
    harness_up(&h, 64 * 1024, 64 * 1024 * 1024);

    const uint64_t started = uv_hrtime();
    CHECK(run_shell(&h, "sleep 5", 1), "the call came back (a `sleep 5` that was not killed would not)");
    const uint64_t elapsed_ms = (uv_hrtime() - started) / 1000000;

    CHECK(elapsed_ms < 4000, "and well before the command would have finished: %llu ms",
          (unsigned long long)elapsed_ms);
    CHECK(strstr(result_text(), "[command timed out]") != NULL, "the result says why: %.80s", result_text());
    /* The synthetic -1 the timeout leaves behind is not a signal, so reporting `[signal 1]`
     * here would be a fiction -- the two notes are mutually exclusive by construction. */
    CHECK(strstr(result_text(), "[signal") == NULL, "and does not invent a signal number: %.80s",
          result_text());
    CHECK(g_error == NULL, "a timeout is reported in the result text, not as a tool error");

    harness_down(&h);
}

/* A timeout of 0 means "no timeout at all": a user-typed `!cmd` runs until it exits, because
 * the person at the keyboard owns the machine and bounds the command themselves. The timer is
 * never started, and the context still has to close it -- `uv_close` on a never-started handle
 * is legal, and this case is what keeps that true. */
static void test_no_timeout_lets_the_command_finish(void)
{
    TEST("timeout 0 (a user command) runs the command to its end, however long");
    struct harness h;
    harness_up(&h, 64 * 1024, 64 * 1024 * 1024);

    /* 2 s by itself, with the LLM default (30 s) the alternative; asking for 0 must neither
     * cut it short nor be treated as "expired immediately". */
    CHECK(run_shell(&h, "sleep 2; echo finished", 0), "the callback fired");
    CHECK(strstr(result_text(), "finished") != NULL, "the command reached its last line: %.80s", result_text());
    CHECK(strstr(result_text(), "timed out") == NULL, "nothing timed out: %.80s", result_text());

    harness_down(&h);
}

/* Cancellation: a task stopped mid-command hands the call back to the tool that owns it.
 *
 * The path is the actor's, and it is one line -- `ctx->tool->abort(ctx)` for every in-flight
 * call of that request (edc_toolbox_actor.c). Driving that line is what this case does, rather
 * than inventing a second way to cancel: what is being pinned is that the hook finds the
 * child, kills the GROUP (not just `sh`), and lets the ordinary completion path release
 * everything. A `sleep 60` is the command because only killing it can end the call. */
static void test_abort_hook_kills_the_running_child(void)
{
    TEST("cancelling a call kills its child and completes through the normal path");
    struct harness h;
    harness_up(&h, 64 * 1024, 64 * 1024 * 1024);

    cJSON *args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "command", "sleep 60"); /* no timeout: only the abort can end it */
    start_shell(&h, args);
    cJSON_Delete(args);

    /* Let the spawn land, so the child really exists when the abort arrives (aborting before
     * uv_spawn would pass while proving nothing). */
    CHECK(!wait_done(&h, 300), "it is still running after 300 ms, as intended");
    CHECK(h.call && h.call->tool && h.call->tool->abort != NULL, "the tool exposes an abort hook");

    h.call->tool->abort(h.call); /* exactly what the actor's fan-out does */

    CHECK(wait_done(&h, 5000), "the call completed once its child was killed");
    CHECK(g_error == NULL, "a cancellation is not an error of the tool's making");
    CHECK(strstr(result_text(), "timed out") == NULL, "and it is not reported as a timeout");

    harness_down(&h);
}

/* A grandchild must not survive the kill, and the reason is not tidiness: `sh -c 'a && b'`
 * leaves `b` holding the stdout/stderr pipes, so killing only the shell would leave the read
 * side open and the call would never complete. This is the case that proves the GROUP is
 * signalled. */
static void test_abort_reaches_the_grandchild(void)
{
    TEST("the kill takes down the grandchildren, not just the shell");
    struct harness h;
    harness_up(&h, 64 * 1024, 64 * 1024 * 1024);

    char marker[512];
    snprintf(marker, sizeof(marker), "%s/grandchild-marker", edc_test_workspace());

    /* The grandchild writes its marker after 2 s; the abort happens well before that. If only
     * `sh` were killed, the grandchild would live on and create the file. */
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "sh -c 'sleep 2; touch %s' && echo never", marker);

    cJSON *args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "command", cmd);
    start_shell(&h, args);
    cJSON_Delete(args);

    CHECK(!wait_done(&h, 300), "still running after 300 ms");
    h.call->tool->abort(h.call);
    CHECK(wait_done(&h, 5000), "the call completed");

    /* Wait past the point where the grandchild would have written, then look. */
    uv_sleep(2500);
    CHECK(access(marker, F_OK) != 0, "the grandchild was killed too: %s must not exist", marker);

    harness_down(&h);
}

/* `working_dir` is relative to the session's directory and goes through the sandbox, so a
 * relative path is what the model actually gets, and the cwd the command sees is absolute. */
static void test_working_dir_is_honoured(void)
{
    TEST("working_dir changes the directory the command runs in");
    struct harness h;
    harness_up(&h, 64 * 1024, 64 * 1024 * 1024);

    const char *ws = edc_test_workspace();
    char sub[600];
    snprintf(sub, sizeof(sub), "%s/subdir", ws);
    if (mkdir(sub, 0755) != 0)
        fprintf(stderr, "    (note: %s already exists)\n", sub);

    CHECK(run_shell_args(&h, "pwd", 10, "subdir"), "the callback fired");
    CHECK(g_error == NULL, "a readable directory inside the session is accepted");
    CHECK(strstr(result_text(), sub) != NULL, "the command ran there: %.120s", result_text());

    harness_down(&h);
}

/* A directory that does not exist is a spawn failure, and the message has to say so: the
 * reader is a model that asked for the wrong place, and "uv_spawn failed: no such file or
 * directory" is the sentence that lets it correct itself. This also covers the branch where
 * the spawn never started -- the handles are closed and the callback is called with an error,
 * with no read ever registered. */
static void test_working_dir_that_does_not_exist_fails(void)
{
    TEST("a working_dir that does not exist is a described spawn error");
    struct harness h;
    harness_up(&h, 64 * 1024, 64 * 1024 * 1024);

    CHECK(run_shell_args(&h, "pwd", 10, "no-such-directory-here"), "the callback fired");
    CHECK(g_error != NULL, "it is an error, not a silent run elsewhere");
    CHECK(g_result == NULL, "and nothing ran");
    CHECK(g_error && strstr(g_error->message, "uv_spawn failed") != NULL,
          "the message names the failure: %s", g_error ? g_error->message : "(none)");

    harness_down(&h);
}

/* With the sandbox OFF there is no refusal by design ("no enforcement at all", edc_sandbox.c),
 * so a `..` walk-out is expected to be honoured rather than denied. Asserted because the
 * opposite assumption is the natural one to make when reading the sandbox code, and a future
 * change that started refusing paths with the switch off would break the documented contract
 * (`--no-sandbox` is the escape hatch users are told about). */
static void test_sandbox_off_does_not_refuse_paths(void)
{
    TEST("with the sandbox off, a working_dir is honoured, not refused");
    struct harness h;
    harness_up(&h, 64 * 1024, 64 * 1024 * 1024);

    CHECK(run_shell_args(&h, "pwd", 10, "../"), "the callback fired");
    CHECK(g_error == NULL, "the sandbox switch is off, so nothing refuses it: %s",
          g_error ? g_error->message : "(no error)");

    harness_down(&h);
}

/* A malformed call is an error with the arguments echoed back, because the reader of that
 * message is a model that has to fix its own call. */
static void test_missing_command_is_a_described_error(void)
{
    TEST("a call with no `command` fails with the arguments in the message");
    struct harness h;
    harness_up(&h, 64 * 1024, 64 * 1024 * 1024);

    cJSON *args = cJSON_CreateObject();
    cJSON_AddNumberToObject(args, "timeout", 10); /* a number where the tool reads a command */
    start_shell(&h, args);
    cJSON_Delete(args);

    CHECK(wait_done(&h, 3000), "the callback fired without spawning anything");
    CHECK(g_error != NULL, "it is an error");
    CHECK(g_result == NULL, "and there is no output");
    CHECK(g_error && strstr(g_error->message, "command") != NULL,
          "the message names the missing argument: %s", g_error ? g_error->message : "(none)");

    harness_down(&h);
}

/* The fan-out is per REQUEST, so an abort that arrives for a different request must leave
 * this child alone. Asserted because the loop's match is the only thing separating "cancel
 * this call" from "cancel every shell in the process" -- and a wrong answer is invisible
 * until someone cancels one of two parallel commands. */
static void test_abort_of_another_request_leaves_this_one_running(void)
{
    TEST("cancelling a different request does not kill this call's child");
    struct harness h;
    harness_up(&h, 64 * 1024, 64 * 1024 * 1024);

    cJSON *args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "command", "sleep 60");
    start_shell(&h, args);
    cJSON_Delete(args);
    CHECK(!wait_done(&h, 300), "the command is running");

    /* A second call context -- the shape the actor uses for a DIFFERENT in-flight request. */
    struct edc_toolbox_call_ctx *other =
        edc_toolbox_call_ctx_new(h.tb, edc_channel_adapter, (void *)0x1234, NULL, NULL);
    h.call->tool->abort(other);
    CHECK(!wait_done(&h, 500), "the unrelated abort did not end this call");
    edc_toolbox_call_ctx_free(other);

    /* A NULL call is a no-op too: the actor's fan-out can hand one over. */
    h.call->tool->abort(NULL);
    CHECK(!wait_done(&h, 300), "and neither did a NULL one");

    /* Now the matching one, which must. */
    h.call->tool->abort(h.call);
    CHECK(wait_done(&h, 5000), "the call's own abort did end it");

    harness_down(&h);
}

/* Quitting while a command runs is an ordinary thing to do (Ctrl-C during `!sleep 100`), and
 * `edc_tool_shell_abort_all()` is the one path that handles it: edc_toolbox_free() calls it
 * after the runner has stopped, so nothing else will ever complete those calls.
 *
 * That code carries a long comment about calling uv_close() on an already-closing handle
 * tripping libuv's assert and aborting the process during teardown, which means the ordering
 * here is delicate. This case is the only thing that would notice if it rotted: it frees the
 * toolbox with a child in flight and then pumps the loop, so every close callback runs. */
static void test_teardown_with_a_running_child(void)
{
    TEST("tearing down with a child in flight kills it and releases its handles");
    struct harness h;
    harness_up(&h, 64 * 1024, 64 * 1024 * 1024);

    cJSON *args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "command", "sleep 60");
    start_shell(&h, args);
    cJSON_Delete(args);
    CHECK(!wait_done(&h, 300), "a child is running (this is the state to be torn down in)");

    /* Exactly what edc_app_free does, minus the app: the toolbox is freed while the call is
     * in flight, and it is the tool's abort_all hook that has to clean up. */
    edc_toolbox_free(h.tb);
    h.tb = NULL;

    /* Pump so the close callbacks fire; the process not aborting inside uv_close is half the
     * assertion, and reaching the end of this function is the other half. */
    for (int i = 0; i < 200; i++)
        uv_run(&h.runner->loop, UV_RUN_NOWAIT);
    CHECK(1, "the teardown completed without tripping libuv's close assert");
    CHECK(!g_done, "no completion was reported: nobody was left to receive it");

    harness_down(&h);
}

int main(void)
{
    edc_test_workspace(); /* open the disposable store before anything writes */
    log_info("=== exec_shell on a real loop: output limits ===");
    log_info("");

    test_ordinary_command_is_untouched();
    test_nonzero_exit_is_reported();
    test_zero_exit_is_silent();
    test_signal_death_is_reported();
    test_exit_survives_truncation();
    test_cjk_output_is_utf8();
    test_volume_is_truncated_but_not_killed();
    test_runaway_is_killed();
    test_runaway_on_stderr_is_killed_too();
    test_timeout_kills_the_child();
    test_no_timeout_lets_the_command_finish();
    test_abort_hook_kills_the_running_child();
    test_abort_reaches_the_grandchild();
    test_abort_of_another_request_leaves_this_one_running();
    test_teardown_with_a_running_child();
    test_working_dir_is_honoured();
    test_working_dir_that_does_not_exist_fails();
    test_sandbox_off_does_not_refuse_paths();
    test_missing_command_is_a_described_error();

    log_info("");
    log_info("=== 测试结果: %d passed, %d failed ===", test_passed, test_failed);
    return test_failed > 0 ? 1 : 0;
}
