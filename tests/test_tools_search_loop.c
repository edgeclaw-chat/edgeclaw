/* tests/test_tools_search_loop.c -- grep / find on a REAL event loop.
 *
 * The blocking entry point is covered by test_toolset.c; this file drives the tool the way
 * the app does, because that is where the two halves meet: the sandbox check and the
 * argument copy on the runner thread, the walk on libuv's pool, and the result back on the
 * runner. A crash here means the split is wrong, not the matcher.
 *
 * (This file exists because the tool did crash when first driven for real: the pool thread
 * touched state it must not touch. Keeping the real-loop path under test is what makes that
 * class of bug visible.) */

#include "edc_actor.h"
#include "test_ws.h"
#include "edc_compat.h"
#include "edc_config.h"
#include "edc_scheduler.h"
#include "edc_toolbox.h"
#include "edc_tools_search.h"
#include "log.h"

#include <cJSON.h>
#include "sds.h"
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <uv.h>
#include "test_fs_util.h"

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

struct harness
{
    struct edc_config *cfg;
    struct edc_scheduler *sched;
    edc_actor_t *sched_actor;
    edc_actor_runner_t *runner;
    struct edc_toolbox *tb;
    struct edc_toolbox_call_ctx *call;
};

static sds g_result;
static EdcError *g_error;
static bool g_done;

static void result_cb(struct edc_toolbox_call_ctx *ctx, sds result, EdcError *error)
{
    (void)ctx;
    if (g_result)
        sdsfree(g_result);
    if (g_error)
        edc_error_free(g_error);
    g_result = result;
    g_error = error;
    g_done = true;
}

static void harness_up(struct harness *h, const char *root)
{
    memset(h, 0, sizeof(*h));
    h->cfg = g_new0(struct edc_config, 1);
    h->cfg->workspace = sdsnew(edc_test_workspace());
    h->cfg->enable_sandbox = false;
    /* Even with the sandbox off, the ROOT is what a relative "." resolves against --
     * without it the path comes back as given and the walk would use the process cwd. */
    h->cfg->sandbox_root_real = sdsnew(root);

    h->sched = edc_scheduler_new(h->cfg);
    h->sched_actor = edc_scheduler_get_actor(h->sched);
    h->runner = edc_actor_runner_new();
    edc_actor_runner_add(h->runner, h->sched_actor);
    h->tb = edc_toolbox_new(h->cfg, h->sched_actor, NULL);
}

static void harness_down(struct harness *h)
{
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

/* Run one tool call through the real toolbox and wait for the callback. */
static bool run_tool(struct harness *h, const char *tool, cJSON *args)
{
    g_done = false;
    if (h->call)
    {
        edc_toolbox_call_ctx_free(h->call); /* one per call, or valgrind counts them */
        h->call = NULL;
    }
    h->call = edc_toolbox_call_ctx_new(h->tb, edc_channel_adapter, NULL, NULL, NULL);
    edc_toolbox_exec(h->call, tool, args, result_cb);

    int waited = 0;
    while (!g_done && waited < 30000)
    {
        uv_run(&h->runner->loop, UV_RUN_NOWAIT);
        uv_sleep(2);
        waited += 2;
    }
    return g_done;
}

static void write_file(const char *dir, const char *name, const char *body)
{
    sds path = sdscatprintf(sdsempty(), "%s/%s", dir, name);
    FILE *f = fopen(path, "wb");
    if (f)
    {
        fwrite(body, 1, strlen(body), f);
        fclose(f);
    }
    sdsfree(path);
}

static void test_grep_through_the_real_tool(void)
{
    TEST("grep on a real loop: the pool does the walk, the runner gets the result");

    char root[1024];


    test_tmp_template(root, sizeof(root), "edc_search_loop");
    if (!mkdtemp(root))
    {
        CHECK(false, "mkdtemp failed");
        return;
    }
    write_file(root, "a.c", "int a;\nTODO: alpha\n");
    write_file(root, "b.h", "TODO: header\n");

    struct harness h;
    harness_up(&h, root);

    cJSON *args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "pattern", "TODO");
    cJSON_AddStringToObject(args, "path", root);
    CHECK(run_tool(&h, "grep", args), "the callback fired (no crash, no hang)");
    cJSON_Delete(args);

    const char *text = g_result ? g_result : "";
    CHECK(strstr(text, "a.c:2:TODO: alpha") != NULL, "hit reported: %.120s", text);
    CHECK(strstr(text, "b.h:1:") != NULL, "the second file is there");
    CHECK(strstr(text, root) == NULL, "paths are relative to the search root");

    harness_down(&h);

    sds p1 = sdscatprintf(sdsempty(), "%s/a.c", root);
    sds p2 = sdscatprintf(sdsempty(), "%s/b.h", root);
    remove(p1);
    remove(p2);
    sdsfree(p1);
    sdsfree(p2);
    rmdir(root);
}

static void test_find_through_the_real_tool(void)
{
    TEST("find on a real loop: lists files, runs on the pool");

    char root[1024];


    test_tmp_template(root, sizeof(root), "edc_find_loop");
    if (!mkdtemp(root))
    {
        CHECK(false, "mkdtemp failed");
        return;
    }
    write_file(root, "one.c", "x\n");
    write_file(root, "two.h", "y\n");

    struct harness h;
    harness_up(&h, root);

    cJSON *args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "path", root);
    cJSON_AddStringToObject(args, "glob", "*.c");
    CHECK(run_tool(&h, "find", args), "the callback fired");
    cJSON_Delete(args);

    const char *text = g_result ? g_result : "";
    CHECK(strstr(text, "one.c") != NULL, "the .c file is listed: %.120s", text);
    CHECK(strstr(text, "two.h") == NULL, "the glob filtered the .h out");

    harness_down(&h);

    sds p1 = sdscatprintf(sdsempty(), "%s/one.c", root);
    sds p2 = sdscatprintf(sdsempty(), "%s/two.h", root);
    remove(p1);
    remove(p2);
    sdsfree(p1);
    sdsfree(p2);
    rmdir(root);
}

static void test_grep_bad_input_does_not_crash(void)
{
    TEST("grep edge cases: invalid regex, missing directory, missing pattern");

    char root[1024];


    test_tmp_template(root, sizeof(root), "edc_search_bad");
    if (!mkdtemp(root))
    {
        CHECK(false, "mkdtemp failed");
        return;
    }
    struct harness h;
    harness_up(&h, root);

    /* Invalid regex: reported as an error, not a crash. */
    cJSON *args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "pattern", "([unclosed");
    cJSON_AddStringToObject(args, "path", root);
    CHECK(run_tool(&h, "grep", args), "invalid regex still calls back");
    CHECK(g_error != NULL, "and reports an error");
    cJSON_Delete(args);

    /* A path that is not a directory (the old shell version failed at spawn here). */
    sds file = sdscatprintf(sdsempty(), "%s/notadir.txt", root);
    write_file(root, "notadir.txt", "x\n");
    args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "pattern", "x");
    cJSON_AddStringToObject(args, "path", file);
    CHECK(run_tool(&h, "grep", args), "a file path still calls back");
    CHECK(g_error != NULL, "and says it is not a directory");
    cJSON_Delete(args);
    remove(file);
    sdsfree(file);

    /* No pattern at all. */
    args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "path", root);
    CHECK(run_tool(&h, "grep", args), "missing pattern still calls back");
    CHECK(g_error != NULL, "and reports an error");
    cJSON_Delete(args);

    harness_down(&h);
    rmdir(root);
}

/* ── THE ROOT'S SPELLING MUST NOT LEAK INTO THE RESULT ────────────────────────────────────
 *
 * edc_search_run_blocking()/run_search() strip the search root as a PREFIX from the paths the
 * walk produces, and the walk builds those with edc_path_join(), which normalises the
 * separator. The root used to be stored in the CALLER's spelling, so wherever the two
 * disagreed the strip failed and every hit kept its ABSOLUTE path -- the "paths are relative
 * to the search root" contract broke, and the model was handed the search root for free.
 *
 * On POSIX there is one separator and the two spellings cannot disagree, so no test here could
 * ever see it. On Windows they can: the test therefore feeds the SAME directory back with the
 * separator deliberately swapped, which is exactly the shape that used to fail. The earlier
 * cases pass through TEST_TMPDIR and are already native-spelled -- they caught this bug by
 * accident, when their root happened to be "/tmp/...", and stopped covering it once that
 * changed. This case states it on purpose. */
static void test_non_native_root_spelling_keeps_paths_relative(void)
{
    TEST("a root spelled with the other separator still yields root-relative paths");

    char root[1024];
    test_tmp_template(root, sizeof(root), "edc_search_spelling");
    if (!mkdtemp(root))
    {
        CHECK(false, "mkdtemp failed");
        return;
    }
    {
        char p[1200];
        snprintf(p, sizeof(p), "%s/a.c", root);
        FILE *f = fopen(p, "w");
        if (f)
        {
            fputs("int a;\nTODO: alpha\n", f);
            fclose(f);
        }
    }

    /* The same directory, spelled the other way. On POSIX this is a no-op (a backslash is an
     * ordinary filename character there, and the platform has one separator), so the case
     * still asserts the contract without pretending the platform has a second spelling. */
    char swapped[1200];
    snprintf(swapped, sizeof(swapped), "%s", root);
#ifdef _WIN32
    for (char *q = swapped; *q; q++)
        if (*q == '\\')
            *q = '/';
#endif

    /* 1. the synchronous entry point the tools share */
    sds r = edc_search_run_blocking(swapped, true, "TODO", NULL, false, false, 100);
    CHECK(r != NULL, "the search returned a result");
    if (r)
    {
        CHECK(strstr(r, "a.c:2:TODO: alpha") != NULL, "the hit is reported: %.160s", r);
        CHECK(strstr(r, root) == NULL,
              "THE PIN: no hit may carry the absolute root, got %.200s", r);
        sdsfree(r);
    }

    /* 2. and through the tool, which is the path that reaches the model */
    struct harness h;
    harness_up(&h, root);
    cJSON *args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "pattern", "TODO");
    cJSON_AddStringToObject(args, "path", swapped);
    CHECK(run_tool(&h, "grep", args), "the tool call came back");
    cJSON_Delete(args);
    {
        const char *text = g_result ? g_result : "";
        CHECK(strstr(text, "a.c:2:TODO: alpha") != NULL, "the tool reports the hit: %.160s", text);
        CHECK(strstr(text, root) == NULL,
              "THE PIN: the tool's result must not carry the absolute root, got %.200s", text);
    }
    harness_down(&h);

    test_rm_rf(root);
}

int main(void)
{
    edc_test_workspace(); /* open the disposable store before anything writes */
    log_info("test_tools_search_loop: grep/find through the real toolbox");
    test_grep_through_the_real_tool();
    test_find_through_the_real_tool();
    test_grep_bad_input_does_not_crash();
    test_non_native_root_spelling_keeps_paths_relative();
    log_info("=== %d passed, %d failed ===", test_passed, test_failed);
    printf("%d passed, %d failed\n", test_passed, test_failed);
    return test_failed == 0 ? 0 : 1;
}
