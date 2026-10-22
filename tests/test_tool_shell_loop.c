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
#include "edc_compat.h"
#include "edc_config.h"
#include "edc_scheduler.h"
#include "edc_toolbox.h"
#include "edc_utf8.h"
#include "log.h"

#include <cJSON.h>
#include "sds.h"
#include <signal.h>
#include <stdio.h>
#include <string.h>
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
    h->cfg->workspace = sdsnew("/tmp/test_tool_shell_loop_ws");
    h->cfg->enable_sandbox = false;
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

/* Run one command through the real tool and wait for its callback. */
static bool run_shell(struct harness *h, const char *cmd, int timeout_sec)
{
    g_done = false;

    cJSON *args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "command", cmd);
    if (timeout_sec > 0)
        cJSON_AddNumberToObject(args, "timeout", timeout_sec);

    h->call = edc_toolbox_call_ctx_new(h->tb, edc_channel_adapter, NULL, NULL, NULL);
    edc_toolbox_exec(h->call, "exec_shell", args, result_cb);
    cJSON_Delete(args); /* the tool copies what it needs before returning (§4.4) */

    int waited = 0;
    while (!g_done && waited < 30000)
    {
        uv_run(&h->runner->loop, UV_RUN_NOWAIT);
        uv_sleep(2);
        waited += 2;
    }
    return g_done;
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

int main(void)
{
    log_info("=== exec_shell on a real loop: output limits ===");
    log_info("");

    test_ordinary_command_is_untouched();
    test_cjk_output_is_utf8();
    test_volume_is_truncated_but_not_killed();
    test_runaway_is_killed();
    test_runaway_on_stderr_is_killed_too();

    log_info("");
    log_info("=== 测试结果: %d passed, %d failed ===", test_passed, test_failed);
    return test_failed > 0 ? 1 : 0;
}
