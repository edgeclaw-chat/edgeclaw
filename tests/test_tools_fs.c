/* tests/test_tools_fs.c -- the four file tools (read_file / write_file / list_dir /
 * edit_file) on a real event loop and a real thread pool.
 *
 * Why a file of its own: nothing tested these. `bazel coverage --config=cov` showed
 * src/toolbox/edc_tools_fs.c at 22% with every one of the four tools' I/O halves at zero
 * -- the sandbox, the pool hand-off, the line window and the whole edit_file selection
 * family (occurrence / line_hint / replace_all / expected_replacements) never ran under
 * `bazel test`. What can break here is not "did a file get written" but:
 *
 *   1. the window contract read_file states in its own result (which lines, and how to
 *      fetch the rest) -- the model pages with `offset` on the strength of that first line;
 *   2. edit_file's disambiguation: replacing the WRONG occurrence is silent data loss, so
 *      every "more than one match" path must refuse or pick the one the caller named;
 *   3. the create/overwrite convention of an empty old_text (create-if-missing, but never
 *      clobber a non-empty file);
 *   4. bytes the caller did not ask about stay put (a UTF-8 BOM, the rest of the file).
 *
 * Harness: a uv loop with the scheduler actor and the real toolbox, pumped by THIS thread
 * (`uv_run(..., UV_RUN_NOWAIT)`) instead of by a runner thread, so the tool's after_cb runs
 * inline and the assertions need no locking. The tools hand their work to uv's thread pool
 * and come back through it, so this is the real async path, not a synchronous shortcut.
 *
 * Every path is ABSOLUTE and under the disposable workspace from test_ws.h: with the
 * sandbox off a relative path would be resolved against the test process's CWD, which is
 * not the workspace and not ours to write in. */

#include "edc_actor.h"
#include "test_ws.h"
#include "edc_compat.h"
#include "edc_config.h"
#include "edc_platform.h"
#include "edc_scheduler.h"
#include "edc_toolbox.h"
#include "log.h"

#include <cJSON.h>
#include "sds.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <uv.h>

/* Stub for _run_loop pulled in by edc_commands (transitively linked). */
volatile sig_atomic_t _run_loop = 1;

static int test_failed = 0;
static int test_passed = 0;

#define TEST(name)                                                                                                     \
    do                                                                                                                 \
    {                                                                                                                  \
        log_info("  TEST: %s", name);                                                                                  \
    } while (0)
#define CHECK(cond, ...)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            log_error("    FAIL: " __VA_ARGS__);                                                                       \
            test_failed++;                                                                                             \
        }                                                                                                              \
        else                                                                                                           \
            test_passed++;                                                                                             \
    } while (0)

/* ══════════════════════════════════════════════════════════════
 *  Harness
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

static sds g_result; /* owned by the callback */
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

static void harness_up(struct harness *h)
{
    memset(h, 0, sizeof(*h));
    h->cfg = g_new0(struct edc_config, 1);
    h->cfg->workspace = sdsnew(edc_test_workspace());
    h->cfg->enable_sandbox = false;

    h->sched = edc_scheduler_new(h->cfg);
    h->sched_actor = edc_scheduler_get_actor(h->sched);

    h->runner = edc_actor_runner_new();
    edc_actor_runner_add(h->runner, h->sched_actor);

    h->tb = edc_toolbox_new(h->cfg, h->sched_actor, NULL);
}

static void harness_down(struct harness *h)
{
    /* Drain the TOOL_EMIT messages the tools queued at the scheduler actor: they point at
     * the call context, which is freed last. */
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

/* One call through the real tool. |args| is consumed here; the tool copies what it needs
 * before returning. */
static bool run_tool(struct harness *h, const char *tool, cJSON *args)
{
    g_done = false;
    if (g_result)
        sdsfree(g_result);
    g_result = NULL;
    if (g_error)
        edc_error_free(g_error);
    g_error = NULL;

    if (h->call)
    {
        edc_toolbox_call_ctx_free(h->call);
        h->call = NULL;
    }
    h->call = edc_toolbox_call_ctx_new(h->tb, edc_channel_adapter, NULL, NULL, NULL);
    edc_toolbox_exec(h->call, tool, args, result_cb);
    cJSON_Delete(args);

    int waited = 0;
    while (!g_done && waited < 20000)
    {
        uv_run(&h->runner->loop, UV_RUN_NOWAIT);
        uv_sleep(1);
        waited++;
    }
    return g_done;
}

static const char *result_text(void)
{
    return g_result ? g_result : "";
}

static const char *error_text(void)
{
    return (g_error && g_error->message) ? g_error->message : "";
}

/* Path inside the disposable workspace. EVERY call returns a fresh, stable string, freed by
 * ws_paths_free() at the end of main: a fixed set of rotating buffers looks tempting and is
 * a trap -- a test that takes three paths (`dir`, `sub`, `dir/file`) silently overwrites the
 * first one with the third, and then asserts about a directory that is now a file. */
#define WS_PATH_MAX 256
static sds g_paths[WS_PATH_MAX];
static int g_path_n;

static const char *ws_path(const char *rel)
{
    if (g_path_n >= WS_PATH_MAX)
    {
        fprintf(stderr, "ws_path: more than %d paths\n", WS_PATH_MAX);
        abort();
    }
    g_paths[g_path_n] = sdscatprintf(sdsempty(), "%s/%s", edc_test_workspace(), rel);
    return g_paths[g_path_n++];
}

static void ws_paths_free(void)
{
    for (int i = 0; i < g_path_n; i++)
        sdsfree(g_paths[i]);
    g_path_n = 0;
}

/* mkdir -p for the fixture's own directory, so a test names "r/whole.txt" and does not have
 * to spell the intermediate steps. Deliberately NOT the tool under test: write_file's own
 * parent creation is asserted separately. */
static void mkdirs_for(const char *path)
{
    char buf[1024];
    snprintf(buf, sizeof(buf), "%s", path);
    for (char *p = buf + 1; *p; p++)
    {
        if (*p != '/')
            continue;
        *p = '\0';
        mkdir(buf, 0755);
        *p = '/';
    }
}

static bool write_bytes(const char *path, const char *data, size_t len)
{
    mkdirs_for(path);
    FILE *f = fopen(path, "wb");
    if (!f)
        return false;
    size_t n = fwrite(data, 1, len, f);
    fclose(f);
    return n == len;
}

/* A literal fixture: the length comes from the literal, so a test cannot silently write 27
 * of 30 bytes and then assert against 30 (which is exactly how the first draft of the
 * line_hint case failed -- for the fixture, not for the tool). */
#define write_lit(path, lit) write_bytes((path), (lit), sizeof(lit) - 1)

/* Read a whole file. NULL when it cannot be opened; |*len| gets the size. */
static sds read_bytes(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    sds out = sdsempty();
    char chunk[4096];
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0)
        out = sdscatlen(out, chunk, n);
    fclose(f);
    if (len)
        *len = sdslen(out);
    return out;
}

/* Did the tool come back with a result (no error) and does it contain |needle|? */
static bool result_has(const char *needle)
{
    return g_error == NULL && strstr(result_text(), needle) != NULL;
}

static bool error_has(const char *needle)
{
    return g_error != NULL && strstr(error_text(), needle) != NULL;
}

static cJSON *args_path(const char *path)
{
    cJSON *args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "path", path);
    return args;
}

/* ══════════════════════════════════════════════════════════════
 *  write_file
 *  ══════════════════════════════════════════════════════════════ */

static void test_write_creates_parents_and_exact_bytes(void)
{
    TEST("write_file: creates the parent directories and writes the bytes verbatim");
    struct harness h;
    harness_up(&h);

    const char *path = ws_path("w/one/two/deep.txt");
    cJSON *args = args_path(path);
    cJSON_AddStringToObject(args, "content", "alpha\nbeta\n");
    CHECK(run_tool(&h, "write_file", args), "the callback fired");
    CHECK(error_has("") == false, "no error (got: %s)", error_text());
    CHECK(result_has("Successfully wrote to file"), "the result names the write: %.80s", result_text());
    CHECK(result_has(path), "and the path it wrote: %.80s", result_text());

    size_t len = 0;
    sds back = read_bytes(path, &len);
    CHECK(back != NULL, "the file exists on disk");
    CHECK(back && strcmp(back, "alpha\nbeta\n") == 0, "the bytes are exactly what was sent: %.40s",
          back ? back : "(null)");
    if (back)
        sdsfree(back);

    harness_down(&h);
}

/* ══════════════════════════════════════════════════════════════
 *  read_file
 *  ══════════════════════════════════════════════════════════════ */

static void test_read_whole_file_has_no_marker(void)
{
    TEST("read_file: a file that fits comes back whole, with no window marker");
    struct harness h;
    harness_up(&h);

    const char *path = ws_path("r/whole.txt");
    CHECK(write_lit(path, "one\ntwo\nthree\n"), "the fixture was written");

    CHECK(run_tool(&h, "read_file", args_path(path)), "the callback fired");
    /* A trailing newline must not open an empty extra line: "one\ntwo\nthree\n" is 3 lines,
     * so the whole file IS the whole file and there is nothing to say about a window. */
    CHECK(strcmp(result_text(), "one\ntwo\nthree\n") == 0, "the text is the file, byte for byte: %.60s",
          result_text());
    CHECK(strstr(result_text(), "[lines ") == NULL, "and no window header");

    harness_down(&h);
}

static void test_read_window_marker_and_continuation(void)
{
    TEST("read_file: offset/limit returns the window, says which lines, and how to continue");
    struct harness h;
    harness_up(&h);

    const char *path = ws_path("r/window.txt");
    CHECK(write_lit(path, "a\nb\nc\nd\ne\n"), "the fixture was written");

    cJSON *args = args_path(path);
    cJSON_AddNumberToObject(args, "offset", 2);
    cJSON_AddNumberToObject(args, "limit", 2);
    CHECK(run_tool(&h, "read_file", args), "the callback fired");
    CHECK(result_has("[lines 2-3 of 5]"), "the head states the window: %.60s", result_text());
    CHECK(result_has("b\nc\n"), "the body is lines 2..3: %.60s", result_text());
    CHECK(result_has("[more lines follow: pass offset=4 to continue]"),
          "and it says how to fetch the rest: %.60s", result_text());

    /* The last window has nothing after it, so it must NOT tell the model to keep paging. */
    args = args_path(path);
    cJSON_AddNumberToObject(args, "offset", 4);
    cJSON_AddNumberToObject(args, "limit", 2);
    CHECK(run_tool(&h, "read_file", args), "the callback fired");
    CHECK(result_has("[lines 4-5 of 5]"), "the head states the window: %.60s", result_text());
    CHECK(strstr(result_text(), "more lines follow") == NULL, "no continuation note at the end of the file");

    /* Past the end is an answer, not an error: the model asked a question. */
    args = args_path(path);
    cJSON_AddNumberToObject(args, "offset", 99);
    CHECK(run_tool(&h, "read_file", args), "the callback fired");
    CHECK(result_has("[no line 99: the file has 5 line(s)]"), "an out-of-range offset says so: %.60s",
          result_text());
    CHECK(g_error == NULL, "out of range is not an error");

    harness_down(&h);
}

static void test_read_line_and_byte_budgets(void)
{
    TEST("read_file bounds itself: 2000 lines, then 64 KiB, and says which stopped it");
    struct harness h;
    harness_up(&h);

    /* 3000 short lines: the LINE budget (2000) is what ends this window. */
    {
        const char *path = ws_path("r/many.txt");
        sds body = sdsempty();
        for (int i = 0; i < 3000; i++)
            body = sdscatprintf(body, "l%d\n", i);
        CHECK(write_bytes(path, body, sdslen(body)), "the fixture was written");
        sdsfree(body);

        CHECK(run_tool(&h, "read_file", args_path(path)), "the callback fired");
        CHECK(result_has("[lines 1-2000 of 3000]"), "2000 lines is the ceiling: %.60s", result_text());
        CHECK(result_has("pass offset=2001"), "and it names the next page: %.200s", result_text());
        CHECK(strstr(result_text(), "cut at 64 KiB") == NULL, "this window ended on the LINE budget");
    }

    /* 100 lines of ~1 KiB: the BYTE budget ends this one, still on a line boundary. */
    {
        const char *path = ws_path("r/wide.txt");
        sds body = sdsempty();
        char line[1001];
        memset(line, 'x', 1000);
        line[1000] = '\n';
        for (int i = 0; i < 100; i++)
            body = sdscatlen(body, line, 1001);
        CHECK(write_bytes(path, body, sdslen(body)), "the fixture was written");
        sdsfree(body);

        CHECK(run_tool(&h, "read_file", args_path(path)), "the callback fired");
        CHECK(result_has("[lines 1-"), "a window header is there: %.60s", result_text());
        CHECK(result_has(" of 100]"), "all 100 lines exist: %.40s", result_text());
        CHECK(strstr(result_text(), "more lines follow") != NULL, "and more follow");
        /* The point of the byte budget: the answer stays near 64 KiB, not 100 KiB. */
        CHECK(sdslen(g_result) < 70 * 1024, "the result respects the 64 KiB budget: %zu bytes",
              (size_t)sdslen(g_result));
    }

    /* One line longer than the whole budget: cut, and SAY it was cut (a half line the model
     * cannot tell from a whole one is worse than an honest truncation). */
    {
        const char *path = ws_path("r/longline.txt");
        sds body = sdsnewlen("", 0);
        for (int i = 0; i < 70000; i++)
            body = sdscatlen(body, "A", 1);
        CHECK(write_bytes(path, body, sdslen(body)), "the fixture was written");
        sdsfree(body);

        CHECK(run_tool(&h, "read_file", args_path(path)), "the callback fired");
        CHECK(result_has("cut at 64 KiB"), "the header says the line was cut: %.80s", result_text());
        CHECK(result_has("[that line is longer than 64 KiB and was cut]"),
              "and so does the trailer: %.80s", result_text());
    }

    harness_down(&h);
}

static void test_read_refuses_an_oversized_file(void)
{
    TEST("read_file: over the 8 MiB single-read limit it refuses AND names a next step");
    struct harness h;
    harness_up(&h);

    /* 8 MiB + 1: the ceiling is on ONE read, so the answer is "page it with exec_shell",
     * not a 9 MiB allocation. */
    const char *path = ws_path("r/huge.bin");
    {
        sds body = sdsnewlen("", 0);
        for (int i = 0; i < (8 * 1024 * 1024 + 1); i++)
            body = sdscatlen(body, "z", 1);
        CHECK(write_bytes(path, body, sdslen(body)), "the fixture was written");
        sdsfree(body);
    }

    CHECK(run_tool(&h, "read_file", args_path(path)), "the callback fired");
    CHECK(g_error != NULL, "it is an error");
    CHECK(error_has("over the"), "the error states the size: %.120s", error_text());
    CHECK(error_has("exec_shell"), "and hands the model a way forward: %.160s", error_text());
    CHECK(g_result == NULL, "no partial content is handed back");

    /* The same ceiling, from edit_file's side: it must refuse rather than edit half a file
     * (old_text has to be matched against the WHOLE file). */
    cJSON *args = args_path(path);
    cJSON_AddStringToObject(args, "old_text", "z");
    cJSON_AddStringToObject(args, "new_text", "y");
    CHECK(run_tool(&h, "edit_file", args), "the callback fired");
    CHECK(error_has("limit for an edit"), "edit_file refuses an oversize file: %.160s", error_text());
    CHECK(error_has("sed"), "and suggests exec_shell/sed: %.200s", error_text());

    harness_down(&h);
}

static void test_read_missing_file_is_an_error(void)
{
    TEST("read_file: a missing file is an error, not an empty result");
    struct harness h;
    harness_up(&h);

    CHECK(run_tool(&h, "read_file", args_path(ws_path("r/absent.txt"))), "the callback fired");
    CHECK(g_error != NULL, "it is an error");
    CHECK(g_result == NULL, "no empty string posing as content");

    harness_down(&h);
}

/* ══════════════════════════════════════════════════════════════
 *  list_dir
 *  ══════════════════════════════════════════════════════════════ */

static void test_list_dir_one_level(void)
{
    TEST("list_dir: one level, directories and files told apart");
    struct harness h;
    harness_up(&h);

    const char *dir = ws_path("l");
    const char *sub = ws_path("l/sub");
    CHECK(write_lit(ws_path("l/file.txt"), "x"), "the file fixture was written");
    CHECK(mkdir(dir, 0755) == 0 || 1, "the dir fixture exists");
    CHECK(mkdir(sub, 0755) == 0, "the sub-dir fixture exists");

    CHECK(run_tool(&h, "list_dir", args_path(dir)), "the callback fired");
    CHECK(result_has("[FILE] file.txt"), "a file is marked as one: %.80s", result_text());
    CHECK(result_has("[DIR]  sub"), "a directory is marked as one: %.80s", result_text());

    /* One level only: the file inside the sub-directory must not show up. */
    CHECK(strstr(result_text(), "sub/") == NULL, "it does not recurse");

    CHECK(run_tool(&h, "list_dir", args_path(ws_path("l/absent"))), "the callback fired");
    CHECK(g_error != NULL, "a missing directory is an error");

    harness_down(&h);
}

/* ══════════════════════════════════════════════════════════════
 *  edit_file
 *  ══════════════════════════════════════════════════════════════ */

static bool file_is(const char *path, const char *expected)
{
    size_t len = 0;
    sds back = read_bytes(path, &len);
    bool same = back && strcmp(back, expected) == 0;
    if (back)
        sdsfree(back);
    return same;
}

/* Build an edit_file call. Any of the four selectors may be NULL to mean "not given". */
static cJSON *edit_args(const char *path, const char *old_text, const char *new_text, const char *replace_all,
                        const char *occurrence, const char *line_hint, const char *expected_replacements)
{
    cJSON *args = args_path(path);
    cJSON_AddStringToObject(args, "old_text", old_text);
    cJSON_AddStringToObject(args, "new_text", new_text);
    if (replace_all)
        cJSON_AddBoolToObject(args, "replace_all", true);
    if (occurrence)
        cJSON_AddNumberToObject(args, "occurrence", atoi(occurrence));
    if (line_hint)
        cJSON_AddNumberToObject(args, "line_hint", atoi(line_hint));
    if (expected_replacements)
        cJSON_AddNumberToObject(args, "expected_replacements", atoi(expected_replacements));
    return args;
}

static void test_edit_single_match(void)
{
    TEST("edit_file: one unique match is replaced, and nothing else moves");
    struct harness h;
    harness_up(&h);

    const char *path = ws_path("e/single.txt");
    CHECK(write_lit(path, "alpha\nbeta\ngamma\n"), "the fixture was written");
    CHECK(run_tool(&h, "edit_file", edit_args(path, "beta", "BETA", NULL, NULL, NULL, NULL)), "the callback fired");
    CHECK(result_has("Successfully edited"), "the result says so: %.80s", result_text());
    CHECK(file_is(path, "alpha\nBETA\ngamma\n"), "the file changed exactly where asked");

    /* Not found: an error that names the file, and the file is untouched. */
    CHECK(run_tool(&h, "edit_file", edit_args(path, "nosuchtext", "x", NULL, NULL, NULL, NULL)), "the callback fired");
    CHECK(error_has("old_text not found"), "a miss is reported as one: %.80s", error_text());
    CHECK(file_is(path, "alpha\nBETA\ngamma\n"), "and the file is byte-identical afterwards");

    harness_down(&h);
}

static void test_edit_refuses_an_ambiguous_match(void)
{
    TEST("edit_file: repeats with no selector REFUSE, and list the candidates");
    struct harness h;
    harness_up(&h);

    const char *path = ws_path("e/repeat.txt");
    CHECK(write_lit(path, "foo one\nfoo two\nfoo three\n"), "the fixture was written");

    CHECK(run_tool(&h, "edit_file", edit_args(path, "foo", "bar", NULL, NULL, NULL, NULL)), "the callback fired");
    CHECK(error_has("old_text appears 3 times"), "the count is stated: %.80s", error_text());
    CHECK(error_has("occurrence"), "and what to do about it: %.160s", error_text());
    CHECK(error_has("[1] line 1"), "the candidates carry line numbers: %.200s", error_text());
    CHECK(error_has("[3] line 3"), "all of them, not just one: %.200s", error_text());
    CHECK(file_is(path, "foo one\nfoo two\nfoo three\n"), "nothing was written");

    harness_down(&h);
}

static void test_edit_selection_by_occurrence(void)
{
    TEST("edit_file: occurrence picks exactly the one the caller named");
    struct harness h;
    harness_up(&h);

    const char *path = ws_path("e/occ.txt");
    CHECK(write_lit(path, "foo one\nfoo two\nfoo three\n"), "the fixture was written");

    CHECK(run_tool(&h, "edit_file", edit_args(path, "foo", "bar", NULL, "2", NULL, NULL)), "the callback fired");
    CHECK(g_error == NULL, "no error (got: %s)", error_text());
    CHECK(file_is(path, "foo one\nbar two\nfoo three\n"), "only the SECOND occurrence changed");

    /* Out of range: refuse, and say how many there are. */
    CHECK(run_tool(&h, "edit_file", edit_args(path, "foo", "baz", NULL, "9", NULL, NULL)), "the callback fired");
    CHECK(error_has("occurrence 9 out of range"), "the range is stated: %.80s", error_text());
    CHECK(error_has("appears 2 times"), "against the real count: %.80s", error_text());
    CHECK(file_is(path, "foo one\nbar two\nfoo three\n"), "nothing was written");

    harness_down(&h);
}

static void test_edit_selection_by_line_hint(void)
{
    TEST("edit_file: line_hint takes the NEAREST match, and refuses a tie");
    struct harness h;
    harness_up(&h);

    /* Matches on lines 1 and 5. A hint of 4 is unambiguously near the second; a hint of 3
     * is exactly between them. */
    const char *path = ws_path("e/hint.txt");
    CHECK(write_lit(path, "foo a\nline2\nline3\nline4\nfoo b\n"), "the fixture was written");

    CHECK(run_tool(&h, "edit_file", edit_args(path, "foo", "BAR", NULL, NULL, "4", NULL)), "the callback fired");
    CHECK(g_error == NULL, "no error (got: %s)", error_text());
    CHECK(file_is(path, "foo a\nline2\nline3\nline4\nBAR b\n"), "the match near the hint changed, the far one did not");

    /* A hint exactly between the two (lines 1 and 5, hint 3) is a coin flip: refusing is the
     * only honest answer, since guessing silently edits the wrong line. */
    const char *tie = ws_path("e/tie.txt");
    CHECK(write_lit(tie, "foo a\nline2\nline3\nline4\nfoo b\n"), "the tie fixture was written");
    CHECK(run_tool(&h, "edit_file", edit_args(tie, "foo", "Z", NULL, NULL, "3", NULL)), "the callback fired");
    CHECK(error_has("is ambiguous"), "a tie is refused: %.120s", error_text());
    CHECK(error_has("line 1"), "with the candidates listed: %.200s", error_text());
    CHECK(error_has("line 5"), "both of them: %.200s", error_text());
    CHECK(file_is(tie, "foo a\nline2\nline3\nline4\nfoo b\n"), "and nothing was written");

    /* The other side of the same rule: a hint of 1 is nearest the first match. */
    const char *near = ws_path("e/near.txt");
    CHECK(write_lit(near, "foo a\nline2\nline3\nline4\nfoo b\n"), "the near fixture was written");
    CHECK(run_tool(&h, "edit_file", edit_args(near, "foo", "BAR", NULL, NULL, "1", NULL)), "the callback fired");
    CHECK(file_is(near, "BAR a\nline2\nline3\nline4\nfoo b\n"), "the FIRST match changed this time");

    harness_down(&h);
}

static void test_edit_preview_shows_context_and_truncates(void)
{
    TEST("edit_file: the candidate preview escapes whitespace and stops at five");
    struct harness h;
    harness_up(&h);

    /* A tab in the context must not be printed raw: the preview is one line per candidate,
     * and a raw tab would misalign it (a newline would break it into two).
     *
     * Seven matches, so the preview says how many it did NOT show -- a list that silently
     * stops at five reads as "there are five". */
    const char *path = ws_path("e/preview.txt");
    CHECK(write_lit(path, "one\tfoo\ntwo\tfoo\nthree\tfoo\nfour\tfoo\nfive\tfoo\nsix\tfoo\nseven\tfoo\n"),
          "the fixture was written");

    CHECK(run_tool(&h, "edit_file", edit_args(path, "foo", "bar", NULL, NULL, NULL, NULL)), "the callback fired");
    CHECK(error_has("old_text appears 7 times"), "all seven are counted: %.80s", error_text());
    CHECK(error_has("\\t"), "a tab in the context is escaped, not printed raw: %.200s", error_text());
    CHECK(error_has("... and 2 more matches"), "and the preview says what it left out: %.260s", error_text());
    CHECK(strstr(error_text(), "line 6:") == NULL, "the sixth candidate is not listed");
    CHECK(file_is(path, "one\tfoo\ntwo\tfoo\nthree\tfoo\nfour\tfoo\nfive\tfoo\nsix\tfoo\nseven\tfoo\n"),
          "nothing was written");

    harness_down(&h);
}

static void test_edit_replace_all_and_expected_count(void)
{
    TEST("edit_file: replace_all + expected_replacements is a checked bulk edit");
    struct harness h;
    harness_up(&h);

    const char *path = ws_path("e/all.txt");
    CHECK(write_lit(path, "x x x\n"), "the fixture was written");

    /* The guard: the caller says how many replacements it believes it is asking for. */
    CHECK(run_tool(&h, "edit_file", edit_args(path, "x", "y", "1", NULL, NULL, "5")), "the callback fired");
    CHECK(error_has("expected 5 replacements but would make 3"), "a wrong expectation is refused: %.120s",
          error_text());
    CHECK(file_is(path, "x x x\n"), "and nothing was written");

    CHECK(run_tool(&h, "edit_file", edit_args(path, "x", "y", "1", NULL, NULL, "3")), "the callback fired");
    CHECK(g_error == NULL, "no error (got: %s)", error_text());
    CHECK(file_is(path, "y y y\n"), "all three were replaced");

    /* The three selectors are alternatives, not a combination. */
    CHECK(write_lit(path, "x x\n"), "the fixture was reset");
    CHECK(run_tool(&h, "edit_file", edit_args(path, "x", "y", "1", "1", NULL, NULL)), "the callback fired");
    CHECK(error_has("mutually exclusive"), "replace_all + occurrence is refused: %.120s", error_text());
    CHECK(file_is(path, "x x\n"), "and nothing was written");

    harness_down(&h);
}

static void test_edit_deleting_text_removes_the_line(void)
{
    TEST("edit_file: deleting a line's text takes its newline with it -- no blank line left behind");
    struct harness h;
    harness_up(&h);

    /* old_text without the trailing newline: the newline that follows is dropped too, which
     * is what "delete this line" means. Leaving it would grow a blank line on every pass. */
    const char *path = ws_path("e/del.txt");
    CHECK(write_lit(path, "a\nb\nc\n"), "the fixture was written");
    CHECK(run_tool(&h, "edit_file", edit_args(path, "b", "", NULL, NULL, NULL, NULL)), "the callback fired");
    CHECK(g_error == NULL, "no error (got: %s)", error_text());
    CHECK(file_is(path, "a\nc\n"), "the line is gone, not blanked");

    /* old_text WITH the newline: nothing extra to drop. */
    CHECK(write_lit(path, "a\nc\n"), "the fixture was reset");
    CHECK(run_tool(&h, "edit_file", edit_args(path, "c\n", "", NULL, NULL, NULL, NULL)), "the callback fired");
    CHECK(file_is(path, "a\n"), "the line and its newline are gone");

    harness_down(&h);
}

static void test_edit_create_convention(void)
{
    TEST("edit_file: an empty old_text creates a missing file, and never clobbers a non-empty one");
    struct harness h;
    harness_up(&h);

    /* 1. missing file + empty old_text = create (parent directories included). */
    const char *fresh = ws_path("e/new/dir/made.txt");
    CHECK(run_tool(&h, "edit_file", edit_args(fresh, "", "hello\n", NULL, NULL, NULL, NULL)), "the callback fired");
    CHECK(result_has("Successfully created"), "the result says CREATED, not edited: %.80s", result_text());
    CHECK(file_is(fresh, "hello\n"), "the file holds exactly new_text");

    /* 2. a non-empty file must not be silently overwritten that way. */
    CHECK(run_tool(&h, "edit_file", edit_args(fresh, "", "clobber\n", NULL, NULL, NULL, NULL)), "the callback fired");
    CHECK(error_has("already exists and is not empty"), "the clobber is refused: %.120s", error_text());
    CHECK(file_is(fresh, "hello\n"), "and the file still holds what it did");

    /* 3. an EMPTY (or whitespace-only) file is fair game: that is what "create" means when
     *    the path exists but holds nothing. */
    const char *empty = ws_path("e/blank.txt");
    CHECK(write_lit(empty, "  \n\n"), "the empty fixture was written");
    CHECK(run_tool(&h, "edit_file", edit_args(empty, "", "fresh\n", NULL, NULL, NULL, NULL)), "the callback fired");
    CHECK(result_has("Successfully edited"), "this one is an EDIT: %.80s", result_text());
    CHECK(file_is(empty, "fresh\n"), "the whitespace-only file was replaced");

    /* 4. a missing file with a NON-empty old_text is a plain not-found error. */
    CHECK(run_tool(&h, "edit_file", edit_args(ws_path("e/absent.txt"), "x", "y", NULL, NULL, NULL, NULL)),
          "the callback fired");
    CHECK(error_has("File not found"), "the error names the cause: %.120s", error_text());

    harness_down(&h);
}

static void test_edit_argument_validation(void)
{
    TEST("edit_file: missing and out-of-range arguments are refused before any I/O");
    struct harness h;
    harness_up(&h);

    /* The toolbox validates against the tool's own schema BEFORE exec: a missing required
     * argument and a value below the declared minimum (1) both stop there. That is the
     * contract a model actually meets, so it is what this pins -- edit_file's own
     * "missing required parameter" / "must be non-negative" guards sit behind it and are
     * unreachable through this door (kept for a direct caller, not exercised here). */
    {
        cJSON *args = args_path(ws_path("e/args.txt"));
        cJSON_AddStringToObject(args, "old_text", "x");
        CHECK(run_tool(&h, "edit_file", args), "the callback fired");
        CHECK(error_has("required parameter 'new_text' is missing"), "a missing new_text is caught: %.120s",
              error_text());
    }
    {
        cJSON *args = edit_args(ws_path("e/args.txt"), "x", "y", NULL, "-1", NULL, NULL);
        CHECK(run_tool(&h, "edit_file", args), "the callback fired");
        CHECK(error_has("parameter 'occurrence' must be of type"), "a negative occurrence is refused: %.120s",
              error_text());
    }
    {
        cJSON *args = edit_args(ws_path("e/args.txt"), "x", "y", NULL, NULL, NULL, "-2");
        CHECK(run_tool(&h, "edit_file", args), "the callback fired");
        CHECK(error_has("parameter 'expected_replacements' must be of type"),
              "a negative expected_replacements is refused: %.120s", error_text());
    }

    harness_down(&h);
}

static void test_edit_preserves_untouched_bytes(void)
{
    TEST("edit_file: bytes the caller did not ask about survive -- including a UTF-8 BOM");
    struct harness h;
    harness_up(&h);

    /* A BOM is the interesting case: PowerShell writes one by default, and it is INVISIBLE,
     * so a tool that drops it changes the file in a way nobody can see. What must happen
     * depends on the platform (edc_platform_bom_is_metadata): on Windows it is metadata and
     * the tool strips/restores it, so a line-1 edit still matches; elsewhere it is content,
     * and the tool must not touch it. */
    const char *path = ws_path("e/bom.txt");
    CHECK(write_bytes(path, "\xEF\xBB\xBF""hello world\n", 15), "the BOM fixture was written");

    if (edc_platform_bom_is_metadata())
    {
        /* The model copies old_text out of read_file, which showed it BOM-less: the match
         * must still work on line 1, and the BOM must come back afterwards. */
        CHECK(run_tool(&h, "edit_file", edit_args(path, "hello", "goodbye", NULL, NULL, NULL, NULL)),
              "the callback fired");
        CHECK(g_error == NULL, "the line-1 match works without the BOM in old_text (got: %s)", error_text());
        CHECK(file_is(path, "\xEF\xBB\xBF""goodbye world\n"), "and the BOM is put back");
    }
    else
    {
        /* POSIX: the BOM is three ordinary bytes of content. An edit elsewhere in the file
         * must leave them exactly as they were. */
        CHECK(run_tool(&h, "edit_file", edit_args(path, "world", "there", NULL, NULL, NULL, NULL)),
              "the callback fired");
        CHECK(g_error == NULL, "no error (got: %s)", error_text());
        CHECK(file_is(path, "\xEF\xBB\xBF""hello there\n"), "the BOM bytes are untouched");
    }

    harness_down(&h);
}

int main(void)
{
    edc_test_workspace(); /* the disposable workspace, before anything writes into it */
    log_info("=== the four file tools on a real loop ===");
    log_info("");

    test_write_creates_parents_and_exact_bytes();
    test_read_whole_file_has_no_marker();
    test_read_window_marker_and_continuation();
    test_read_line_and_byte_budgets();
    test_read_refuses_an_oversized_file();
    test_read_missing_file_is_an_error();
    test_list_dir_one_level();
    test_edit_single_match();
    test_edit_refuses_an_ambiguous_match();
    test_edit_selection_by_occurrence();
    test_edit_selection_by_line_hint();
    test_edit_preview_shows_context_and_truncates();
    test_edit_replace_all_and_expected_count();
    test_edit_deleting_text_removes_the_line();
    test_edit_create_convention();
    test_edit_argument_validation();
    test_edit_preserves_untouched_bytes();

    log_info("");
    log_info("=== 测试结果: %d passed, %d failed ===", test_passed, test_failed);
    ws_paths_free();
    return test_failed > 0 ? 1 : 0;
}
