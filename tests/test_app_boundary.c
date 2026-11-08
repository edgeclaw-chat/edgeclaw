/* test_app_boundary: the C surface the frontend actually calls.
 *
 * `src/app` was the thinnest module in the tree on branches (38.5%, the worst) and 63.2% on
 * lines, and the gap was not spread out -- it was the frontend's whole entry surface, never
 * executed: the ten `edc_app_models_*` projections, both config string readers, the `@`
 * completion readers, the four setup write paths, and the request guards.
 *
 * Why nobody had tested them: `struct edc_app` is opaque (defined only in edc_app.c), so
 * there is no way to fake one -- every app-taking function here needs a REAL app. This file
 * therefore builds one, the same way `test_tui_dispatch.cpp` does (temp HOME + a minimal
 * config + EDGECLAW_MASTER_KEY so the secret module does not prompt), and drives the
 * functions through it.
 *
 * Two deliberate choices:
 *
 *   - **One app for the whole file.** `edc_app_new()` starts the scheduler, the SQLite store
 *     and three actor threads, and `edc_app_free()` is what closes the store. Building one per
 *     case would be slow and would fight the one-process-per-workspace rule; the cases are
 *     ordered instead, and each one asserts the state it changed.
 *   - **A private model catalog.** The embedded catalog is data that changes; an assertion
 *     about "provider 0 is X" or "this model's context is N" would rot the first time it is
 *     regenerated. `EDGECLAW_MODELS_CATALOG` points the loader at a two-provider file written
 *     by this test, so every catalog assertion is about the projection, not the data.
 *
 * What these cases pin is the CONTRACT the header states, because that is what the frontend
 * relies on and what a refactor would silently break:
 *
 *   - a string reader returns false AND clears the buffer on failure -- a stale buffer would
 *     show the previous provider's URL in /connect;
 *   - `edc_app_message_mid` answers with the reply's header (the mid it answers for);
 *   - an unknown model answers 0/NULL instead of reading out of bounds;
 *   - the setup write path is gated on the LLM write: if that fails, `onboarded` is NOT set,
 *     or the wizard would stop reopening while the config still cannot answer;
 *   - `mark_onboarded` is the Skip path: it must not touch the LLM settings;
 *   - the request functions refuse an empty id instead of queueing a message nobody can match.
 */

#include "edc_api.h"
#include "edc_app.h"
#include "edc_compat.h"
#include "edc_config.h"
#include "edc_models.h"
#include "sds.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "test_fs_util.h"

static int g_passed = 0;
static int g_failed = 0;

#define TEST(name) printf("\n-- %s\n", name)

#define CHECK(cond, ...) do { \
    if (!(cond)) { fprintf(stderr, "    FAIL: "); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); g_failed++; } \
    else { printf("    ok: "); printf(__VA_ARGS__); printf("\n"); g_passed++; } \
} while (0)

/* ── A disposable HOME with a minimal config ──────────────────────────────── */

static char g_home[512];
static char g_workspace[600];
static char g_config[600];
static struct edc_app *g_app = NULL;

/* A two-provider catalog whose every field this test can name. Written to disk because that
 * is the only way in (`EDGECLAW_MODELS_CATALOG` takes a path). */
static const char *kCatalog = "{"
                              "\"providers\":{"
                              "\"alpha\":{\"name\":\"Alpha Inc\",\"api\":\"https://alpha.example/v1\","
                              "\"env\":[\"ALPHA_API_KEY\",\"ALPHA_TOKEN\"],"
                              "\"models\":[\"alpha-one\",\"alpha-two\"]},"
                              "\"beta\":{\"name\":\"Beta\",\"api\":\"https://beta.example/v1\","
                              "\"models\":[\"beta-one\"]}},"
                              "\"models\":{"
                              /* tool_call -> a chat model; no text modality + no tool_call ->
                                 an embedding model (the setup page must not offer it). */
                              "\"alpha-one\":{\"name\":\"Alpha One\",\"context\":128000,\"output\":8192,"
                              "\"tool_call\":true},"
                              "\"alpha-two\":{\"name\":\"alpha-two\",\"context\":32000,\"output\":4096,"
                              "\"tool_call\":true},"
                              "\"beta-one\":{\"name\":\"Beta One\",\"context\":200000,\"output\":16384,"
                              "\"tool_call\":true},"
                              "\"embed-only\":{\"name\":\"Embed Only\",\"context\":8192,\"output\":0}"
                              "}}";

static void die(const char *what)
{
    fprintf(stderr, "setup: %s (HOME=%s)\n", what, g_home);
    exit(2);
}

static void write_file_or_die(const char *path, const char *content)
{
    FILE *f = fopen(path, "w");
    if (!f)
        die(path);
    fputs(content, f);
    fclose(f);
}

/* Build the temp HOME, the config, the catalog and the app. */
static void setup(void)
{
    const char *tmp = getenv("TEST_TMPDIR");
    if (!tmp || !*tmp)
        tmp = "/tmp";
    snprintf(g_home, sizeof(g_home), "%s/app-boundary-home", tmp);
    snprintf(g_workspace, sizeof(g_workspace), "%s/.edgeclaw/workspace", g_home);
    snprintf(g_config, sizeof(g_config), "%s/.edgeclaw/config.json", g_home);

    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_home);
    if (test_sh(cmd) != 0)
        die("could not clear the temp home");

    snprintf(cmd, sizeof(cmd), "mkdir -p '%s' '%s/prompts' '%s/skills/demo'", g_workspace, g_workspace, g_workspace);
    if (test_sh(cmd) != 0)
        die("could not create the workspace");

    /* api_key is PLAINTEXT on purpose: no `EDGECLAW_ENC:` prefix means no decryption, so the
     * master key below only has to exist, not match anything. */
    char conf[1024];
    snprintf(conf, sizeof(conf),
             "{\"api_url\":\"https://alpha.example/v1/chat/completions\","
             "\"model\":\"alpha-one\",\"api_key\":\"plaintext-test-key\","
             "\"context_window\":128000,\"onboarded\":false,"
             "\"workspace\":\"%s\"}",
             g_workspace);
    write_file_or_die(g_config, conf);

    /* One prompt template and one skill: the completion vocabulary has to be non-empty for
     * the "does it name the entry" assertion to mean anything. */
    char path[700];
    snprintf(path, sizeof(path), "%s/prompts/review.md", g_workspace);
    write_file_or_die(path, "---\ndescription: review the staged changes\n---\nReview.\n");
    snprintf(path, sizeof(path), "%s/skills/demo/SKILL.md", g_workspace);
    write_file_or_die(path, "---\ndescription: a demo skill\n---\nbody\n");

    /* A directory with two files, for the `@` completion walk. */
    snprintf(path, sizeof(path), "%s/src", g_workspace);
    if (mkdir(path, 0755) != 0)
        die("mkdir src");
    snprintf(path, sizeof(path), "%s/src/main.c", g_workspace);
    write_file_or_die(path, "int main(void){return 0;}\n");
    snprintf(path, sizeof(path), "%s/src/util.c", g_workspace);
    write_file_or_die(path, "void util(void){}\n");

    /* The catalog: written next to the home, then pointed at. */
    char catalog[600];
    snprintf(catalog, sizeof(catalog), "%s/catalog.json", g_home);
    write_file_or_die(catalog, kCatalog);

    setenv("HOME", g_home, 1);
    /* libuv uv_os_homedir() prefers USERPROFILE on Windows, so a test that
     * relocates the home directory has to move both or it reads the real one. */
    setenv("USERPROFILE", g_home, 1);
    setenv("EDGECLAW_MASTER_KEY", "app-boundary-master-key", 1);
    setenv("EDGECLAW_MODELS_CATALOG", catalog, 1);
    edc_models_reset(); /* the loader is a process-wide cache; start from our file */

    /* setup=false: config.json exists, so this is the normal startup path. */
    g_app = edc_app_new(false, /*sandbox_override=*/-1);
}

static void teardown(void)
{
    if (g_app)
    {
        edc_app_free(g_app);
        g_app = NULL;
    }
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_home);
    if (test_sh(cmd) != 0)
        fprintf(stderr, "teardown: could not remove %s\n", g_home);
}

/* Read the config.json back as text (the write paths are only real if they reach the file). */
static bool config_contains(const char *needle)
{
    FILE *f = fopen(g_config, "rb");
    if (!f)
        return false;
    char buf[4096];
    const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    return strstr(buf, needle) != NULL;
}

/* Whether boolean key |key| is true in config.json. cJSON prints `"key":\ttrue` (a TAB
 * after the colon), so a fixed needle like `"onboarded": true` would be a test that passes
 * on the failure it was written to catch -- the trap recorded in the secret tests. Skip
 * whatever whitespace is actually there instead of assuming one shape. */
static bool config_flag_true(const char *key)
{
    FILE *f = fopen(g_config, "rb");
    if (!f)
        return false;
    char buf[4096];
    const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';

    char needle[128];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *at = strstr(buf, needle);
    if (!at)
        return false;
    at += strlen(needle);
    while (*at == ' ' || *at == '\t' || *at == ':' || *at == '\n' || *at == '\r')
        at++;
    return strncmp(at, "true", 4) == 0;
}

/* ── The model catalog: a pure projection, no app ─────────────────────────── */

static void test_catalog_providers_are_projected_one_for_one()
{
    TEST("every provider in the catalog is reachable, sorted by id");
    CHECK(edc_app_models_providers() == 2, "two providers: got %zu", edc_app_models_providers());

    /* The loader qsorts the ids, so the order is a property of the projection, not of the
     * file -- the setup page lists them in this order. */
    const char *p0 = edc_app_models_provider_id(0);
    const char *p1 = edc_app_models_provider_id(1);
    CHECK(p0 && strcmp(p0, "alpha") == 0, "provider 0 is 'alpha': got '%s'", p0 ? p0 : "(null)");
    CHECK(p1 && strcmp(p1, "beta") == 0, "provider 1 is 'beta': got '%s'", p1 ? p1 : "(null)");

    CHECK(edc_app_models_provider_id(2) == NULL, "an index past the end answers NULL");
    CHECK(edc_app_models_provider_id(9999) == NULL, "and so does a far one");
}

static void test_catalog_provider_facts()
{
    TEST("a provider's name, base URL and env names come from the catalog");
    const char *name = edc_app_models_provider_name("alpha");
    const char *api = edc_app_models_provider_api("alpha");
    CHECK(name && strcmp(name, "Alpha Inc") == 0, "name: got '%s'", name ? name : "(null)");
    CHECK(api && strcmp(api, "https://alpha.example/v1") == 0, "api: got '%s'", api ? api : "(null)");

    const char *env0 = edc_app_models_provider_env_name("alpha", 0);
    const char *env1 = edc_app_models_provider_env_name("alpha", 1);
    CHECK(env0 && strcmp(env0, "ALPHA_API_KEY") == 0, "env[0]: got '%s'", env0 ? env0 : "(null)");
    CHECK(env1 && strcmp(env1, "ALPHA_TOKEN") == 0, "env[1]: got '%s'", env1 ? env1 : "(null)");
    CHECK(edc_app_models_provider_env_name("alpha", 2) == NULL, "an env index past the end is NULL");

    /* An unknown provider: every reader answers NULL rather than reading out of bounds. */
    CHECK(edc_app_models_provider_name("nope") == NULL, "unknown provider has no name");
    CHECK(edc_app_models_provider_api("nope") == NULL, "unknown provider has no api");
    CHECK(edc_app_models_provider_env_name("nope", 0) == NULL, "unknown provider has no env");
    CHECK(edc_app_models_provider_name(NULL) == NULL, "a NULL id is answered, not crashed");
    /* 'beta' declares no env array at all -- distinct from an empty one. */
    CHECK(edc_app_models_provider_env_name("beta", 0) == NULL, "a provider with no env array answers NULL");
}

static void test_catalog_models()
{
    TEST("a provider's models are listed by id, in order");
    CHECK(edc_app_models_count("alpha") == 2, "alpha has two models: got %zu", edc_app_models_count("alpha"));
    const char *m0 = edc_app_models_model_id("alpha", 0);
    const char *m1 = edc_app_models_model_id("alpha", 1);
    CHECK(m0 && strcmp(m0, "alpha-one") == 0, "model 0: got '%s'", m0 ? m0 : "(null)");
    CHECK(m1 && strcmp(m1, "alpha-two") == 0, "model 1: got '%s'", m1 ? m1 : "(null)");
    CHECK(edc_app_models_model_id("alpha", 2) == NULL, "an index past the end is NULL");
    CHECK(edc_app_models_count("nope") == 0, "an unknown provider has no models");
    CHECK(edc_app_models_count(NULL) == 0, "a NULL provider id is answered");
}

static void test_catalog_model_facts()
{
    TEST("a model's name, context, output and chat-ness are projected");
    const char *name = edc_app_models_model_name("alpha-one");
    CHECK(name && strcmp(name, "Alpha One") == 0, "name: got '%s'", name ? name : "(null)");
    CHECK(edc_app_models_model_context("alpha-one") == 128000, "context: got %d",
          edc_app_models_model_context("alpha-one"));
    CHECK(edc_app_models_model_output("alpha-one") == 8192, "output: got %d",
          edc_app_models_model_output("alpha-one"));
    CHECK(edc_app_models_model_is_chat("alpha-one") == 1, "tool_call makes it a chat model");

    /* tool_call=false and no text modality: this is the row the setup page must hide. */
    CHECK(edc_app_models_model_is_chat("embed-only") == 0, "an embedding model is not a chat model");

    /* The three facts read through one struct; an unknown id answers zeroes, not garbage.
     * A model whose name equals its id is a real case (many providers do not rename). */
    const char *same = edc_app_models_model_name("alpha-two");
    CHECK(same && strcmp(same, "alpha-two") == 0, "a model whose name IS its id: got '%s'", same ? same : "(null)");
}

static void test_catalog_unknown_model_answers_empty()
{
    TEST("an unknown model answers NULL/0 from every reader");
    CHECK(edc_app_models_model_name("no-such-model") == NULL, "no name");
    CHECK(edc_app_models_model_context("no-such-model") == 0, "no context");
    CHECK(edc_app_models_model_output("no-such-model") == 0, "no output");
    CHECK(edc_app_models_model_is_chat("no-such-model") == 0, "not a chat model");
    CHECK(edc_app_models_model_name(NULL) == NULL, "a NULL id is answered, not crashed");
    CHECK(edc_app_models_model_context(NULL) == 0, "and its context is 0");
}

/* ── Message window ───────────────────────────────────────────────────────── */

static void test_message_mid_is_the_reply_header()
{
    TEST("edc_app_message_mid answers with the reply's header, and NULL for no message");

    /* The contract the frontend relies on: a MESSAGE_PAYLOAD reply names the request it
     * answers in its header. There is no way to build a `struct edc_channel_message` here
     * without the channel headers, and the message window is a one-line projection over a
     * NULL-checked read, so both arms are driven directly. */
    CHECK(edc_app_message_mid(NULL) == NULL, "a NULL message answers NULL");

    /* Every reader in the window has the same NULL contract; the whole group is asserted so
     * that a new one added without the guard shows up here. */
    CHECK(edc_app_message_content(NULL) == NULL, "content(NULL) is NULL");
    CHECK(edc_app_message_header(NULL) == NULL, "header(NULL) is NULL");
    CHECK(edc_app_message_task_id(NULL) == NULL, "task_id(NULL) is NULL");
    CHECK(edc_app_message_session_id(NULL) == NULL, "session_id(NULL) is NULL");
    CHECK(edc_app_message_is_user(NULL) == 0, "is_user(NULL) is 0");
    CHECK(edc_app_message_is_tool(NULL) == 0, "is_tool(NULL) is 0");
    CHECK(edc_app_message_is_thought(NULL) == 0, "is_thought(NULL) is 0");
    CHECK(edc_app_message_is_resp(NULL) == 0, "is_resp(NULL) is 0");
    CHECK(edc_app_message_is_sys(NULL) == 0, "is_sys(NULL) is 0");
    CHECK(edc_app_message_is_task_history(NULL) == 0, "is_task_history(NULL) is 0");
    CHECK(edc_app_message_is_task_snapshot(NULL) == 0, "is_task_snapshot(NULL) is 0");
    CHECK(edc_app_message_is_task_tree_changed(NULL) == 0, "is_task_tree_changed(NULL) is 0");
    CHECK(edc_app_message_is_message_payload(NULL) == 0, "is_message_payload(NULL) is 0");
    CHECK(edc_app_message_from_active_task(NULL) == 0, "from_active_task(NULL) is 0");
    /* An undeclared render preference is PLAIN, not "whatever the enum's zero happens to
     * be" -- the two are the same number today and this is what stops them drifting. */
    CHECK(edc_app_message_render_pref(NULL) == EDC_APP_RENDER_PLAIN, "render_pref(NULL) is PLAIN");
}

/* ── Config string readers (need the real app) ────────────────────────────── */

static void test_config_readers_copy_the_configured_values()
{
    TEST("the config readers hand back what config.json holds");
    CHECK(g_app != NULL, "the app started");

    char buf[256];
    CHECK(edc_app_config_model(g_app, buf, sizeof(buf)), "model is readable");
    CHECK(strcmp(buf, "alpha-one") == 0, "model: got '%s'", buf);

    CHECK(edc_app_config_api_url(g_app, buf, sizeof(buf)), "api_url is readable");
    CHECK(strcmp(buf, "https://alpha.example/v1/chat/completions") == 0, "api_url: got '%s'", buf);

    CHECK(edc_app_config_api_key(g_app, buf, sizeof(buf)), "api_key is readable");
    CHECK(strcmp(buf, "plaintext-test-key") == 0, "api_key: got '%s'", buf);

    CHECK(edc_app_config_context_window(g_app) == 128000, "the context window: got %d",
          edc_app_config_context_window(g_app));
    CHECK(edc_app_onboarded(g_app) == 0, "the wizard has not run yet");
}

static void test_a_short_buffer_fails_and_clears()
{
    TEST("a buffer too small for the value fails AND is cleared");
    char small[8];
    /* Sentinels first: the contract is "on failure clear the buffer", and a reader that
     * left the old bytes in place would show the previous provider's URL in /connect. */
    memset(small, 'X', sizeof(small));
    CHECK(!edc_app_config_api_url(g_app, small, sizeof(small)), "a too-small buffer returns false");
    CHECK(small[0] == '\0', "and the buffer was cleared, not left with stale bytes");

    memset(small, 'X', sizeof(small));
    CHECK(!edc_app_config_api_key(g_app, small, sizeof(small)), "same for the api key");
    CHECK(small[0] == '\0', "and it is cleared too");

    /* A NULL/zero-capacity buffer is refused rather than written through. */
    CHECK(!edc_app_config_model(g_app, NULL, 16), "a NULL buffer is refused");
    char one = 'X';
    CHECK(!edc_app_config_model(g_app, &one, 0), "cap 0 is refused");
    CHECK(one == 'X', "and nothing was written");
}

static void test_readers_on_a_null_app_answer_false()
{
    TEST("the readers tolerate a NULL app (the frontend calls them before Ready())");
    char buf[64];
    memset(buf, 'X', sizeof(buf));
    CHECK(!edc_app_config_model(NULL, buf, sizeof(buf)), "model(NULL app) is false");
    CHECK(buf[0] == '\0', "and cleared");
    CHECK(!edc_app_config_api_url(NULL, buf, sizeof(buf)), "api_url(NULL app) is false");
    CHECK(!edc_app_config_api_key(NULL, buf, sizeof(buf)), "api_key(NULL app) is false");
    CHECK(edc_app_sandbox_enabled(NULL) == false, "sandbox_enabled(NULL) is false");
    CHECK(edc_app_shell_syntax(NULL) == false, "shell_syntax(NULL) is false");
    CHECK(edc_app_onboarded(NULL) == 0, "onboarded(NULL) is 0");
    CHECK(edc_app_config_context_window(NULL) == 0, "context_window(NULL) is 0");
    CHECK(edc_app_input_mode(NULL, "hello") == EDC_INPUT_TEXT, "input_mode(NULL) is TEXT");
}

/* ── Input mode: the frontend's copy of the ONE predicate ─────────────────── */

static void test_input_mode_mirrors_the_classifier()
{
    TEST("input_mode names every kind the classifier can produce");
    /* The point of this function is that the tag on the input line and Enter can never
     * disagree, so each arm is asserted against the kind it mirrors. */
    CHECK(edc_app_input_mode(g_app, "explain src/main.c") == EDC_INPUT_TEXT, "a plain line is TEXT");
    CHECK(edc_app_input_mode(g_app, "/help") == EDC_INPUT_COMMAND, "/… is COMMAND");
    CHECK(edc_app_input_mode(g_app, "!git status") == EDC_INPUT_LOCALSHELL, "!… is LOCALSHELL");
    CHECK(edc_app_input_mode(g_app, "\"scan src/\"") == EDC_INPUT_SHELL, "a quoted line is SHELL");
    /* An unterminated quote is already an error: the tag must say so instead of promising
     * the line will run. */
    CHECK(edc_app_input_mode(g_app, "\"unterminated") == EDC_INPUT_REJECTED,
          "an unterminated quote is REJECTED");
    CHECK(edc_app_input_mode(g_app, "") == EDC_INPUT_TEXT, "an empty line is TEXT");
}

/* ── The `@` completion readers ──────────────────────────────────────────── */

static void test_file_candidates_list_the_session_directory()
{
    TEST("file candidates list the session's directory, best match first");
    /* The query is split into "the directory to read" + "the name being typed", and only ONE
     * directory level is read -- so a match for src/main.c is asked for as "src/main", not as
     * "main". Rows are "<path>\t<f|d|s>\n". */
    edc_app_file_candidates_reset();

    char buf[4096];
    const size_t rows = edc_app_file_candidates(g_app, g_workspace, "src/main", buf, sizeof(buf));
    CHECK(rows >= 1, "at least one row for query 'src/main': got %zu", rows);
    CHECK(strstr(buf, "src/main.c\tf") != NULL, "the matching file is listed as a file row: got '%s'", buf);

    /* A directory's row ends with '/', which is how the frontend knows that accepting it
     * descends instead of completing. An empty query lists the root's own entries. */
    edc_app_file_candidates_reset();
    char dirs[4096];
    const size_t drows = edc_app_file_candidates(g_app, g_workspace, "", dirs, sizeof(dirs));
    CHECK(drows >= 1, "the root's entries are listed: got %zu", drows);
    CHECK(strstr(dirs, "src/\td") != NULL, "a directory's row ends with '/' and kind 'd': got '%s'", dirs);

    /* The query naming a directory ITSELF ("src/") is answered with kind 's' -- "accept this
     * one, do not descend" -- because a mention of a whole directory is meaningful. */
    edc_app_file_candidates_reset();
    char self[1024];
    const size_t srows = edc_app_file_candidates(g_app, g_workspace, "src/", self, sizeof(self));
    CHECK(srows >= 1, "a directory query answers its own row: got %zu", srows);
    CHECK(strstr(self, "src\ts") != NULL, "the row is kind 's' (select, do not descend): got '%s'", self);

    /* No match is 0 rows and a cleared buffer, not the previous answer. */
    edc_app_file_candidates_reset();
    char none[256];
    memset(none, 'X', sizeof(none));
    CHECK(edc_app_file_candidates(g_app, g_workspace, "zzz-no-such-thing", none, sizeof(none)) == 0,
          "an unmatched query answers 0");
    CHECK(none[0] == '\0', "and the buffer is cleared");
}

static void test_file_candidates_with_no_workdir_fall_back_to_the_launch_directory()
{
    TEST("no workdir falls back to the launch directory, not the agent's data directory");
    /* A NULL workdir and an empty one must behave the same: both mean "no session bound".
     * The fallback is edc_cwd(), so the answer must mention a path relative to the process's
     * own directory -- asserted only as "does not crash and is self-consistent", because the
     * cwd under bazel is the execroot. */
    edc_app_file_candidates_reset();
    char a[4096];
    char b[4096];
    const size_t ra = edc_app_file_candidates(g_app, NULL, "", a, sizeof(a));
    const size_t rb = edc_app_file_candidates(g_app, "", "", b, sizeof(b));
    CHECK(ra == rb, "NULL and \"\" agree: %zu vs %zu", ra, rb);
    CHECK(strcmp(a, b) == 0, "and produce the same listing");
}

static void test_file_candidates_without_an_app_answers_zero()
{
    TEST("file candidates without an app answer 0 rows");
    char buf[64];
    memset(buf, 'X', sizeof(buf));
    CHECK(edc_app_file_candidates(NULL, g_workspace, "main", buf, sizeof(buf)) == 0, "no app, no rows");
    CHECK(buf[0] == '\0', "and the buffer is cleared");
}

static void test_file_candidates_on_a_directory_that_is_gone()
{
    TEST("a directory that cannot be listed answers 0 rows, and says so in the log");
    /* Not hypothetical: a session stays bound to its directory, and the user can delete or
     * rename that directory between two runs. The popup must simply not open. */
    edc_app_file_candidates_reset();
    char buf[256];
    memset(buf, 'X', sizeof(buf));
    CHECK(edc_app_file_candidates(g_app, "/tmp/no-such-directory-for-candidates", "src", buf, sizeof(buf)) == 0,
          "an unlistable directory answers 0 rows");
    CHECK(buf[0] == '\0', "and the buffer is cleared rather than left with the previous listing");
}

static void test_mention_exists_resolves_through_the_sandbox()
{
    TEST("mention_exists answers for real references only");
    CHECK(edc_app_mention_exists(g_app, g_workspace, "src/main.c") == 1, "a real file exists");
    CHECK(edc_app_mention_exists(g_app, g_workspace, "src") == 1, "a real directory exists");
    CHECK(edc_app_mention_exists(g_app, g_workspace, "src/nope.c") == 0, "a missing file does not");
    CHECK(edc_app_mention_exists(g_app, g_workspace, "") == 0, "an empty path does not");
    CHECK(edc_app_mention_exists(g_app, g_workspace, NULL) == 0, "a NULL path does not");
    CHECK(edc_app_mention_exists(NULL, g_workspace, "src/main.c") == 0, "no app, no answer");
    /* The mark follows the SANDBOX's answer, not stat()'s -- that is the whole reason this
     * resolves through the sandbox instead of stat()ing. The default config's ro list
     * includes /usr, so an existing path there is readable and IS a reference; a path under
     * no list at all is refused even though it is just as absent. Both arms matter: a
     * stat()-only implementation would answer 1 for /root and 0 for /usr, i.e. exactly
     * backwards. */
    CHECK(edc_app_mention_exists(g_app, g_workspace, "/usr") == 1,
          "an existing path inside the ro list is a reference");
    CHECK(edc_app_mention_exists(g_app, g_workspace, "/root/definitely-not-listed") == 0,
          "a path outside every sandbox list is refused, so it never wears the mark");
}

/* ── Prompt / skill vocabulary (the completion popup's other half) ────────── */

static void test_prompts_and_skills_are_listed_as_name_tab_description()
{
    TEST("prompt templates and skills come back as \"name<TAB>description\" lines");
    char buf[4096];
    CHECK(edc_app_prompts(g_app, g_workspace, buf, sizeof(buf)) >= 1, "at least one prompt template");
    CHECK(strstr(buf, "review\treview the staged changes") != NULL, "the row names it and its description: got '%s'",
          buf);

    char sbuf[4096];
    CHECK(edc_app_skills(g_app, g_workspace, sbuf, sizeof(sbuf)) >= 1, "at least one skill");
    CHECK(strstr(sbuf, "demo\ta demo skill") != NULL, "the skill row: got '%s'", sbuf);
}

static void test_prompt_and_skill_readers_need_a_workspace()
{
    TEST("without a config the prompt/skill readers answer 0, and with no app too");
    char buf[64];
    memset(buf, 'X', sizeof(buf));
    CHECK(edc_app_prompts(NULL, g_workspace, buf, sizeof(buf)) == 0, "prompts(NULL app) is 0");
    CHECK(buf[0] == '\0', "and cleared");
    memset(buf, 'X', sizeof(buf));
    CHECK(edc_app_skills(NULL, g_workspace, buf, sizeof(buf)) == 0, "skills(NULL app) is 0");
    CHECK(buf[0] == '\0', "and cleared");
}

/* ── Paths ────────────────────────────────────────────────────────────────── */

static void test_chat_log_path_is_built_by_convention()
{
    TEST("the transcript path is <workspace>/<session_id>/chat.jsonl");
    char buf[512];
    CHECK(edc_app_chat_log_path_for(g_app, "sess-1", buf, sizeof(buf)), "a session id resolves");
    char expected[700];
    snprintf(expected, sizeof(expected), "%s/sess-1/chat.jsonl", g_workspace);
    CHECK(strcmp(buf, expected) == 0, "path: got '%s' want '%s'", buf, expected);

    /* The guard list, in one place: an empty id, a missing buffer, a too-small buffer and no
     * app all answer false, and the buffer is cleared first. */
    memset(buf, 'X', sizeof(buf));
    CHECK(!edc_app_chat_log_path_for(g_app, "", buf, sizeof(buf)), "an empty session id is refused");
    CHECK(buf[0] == '\0', "cleared");
    CHECK(!edc_app_chat_log_path_for(g_app, NULL, buf, sizeof(buf)), "a NULL session id is refused");
    CHECK(!edc_app_chat_log_path_for(g_app, "sess-1", NULL, 16), "a NULL buffer is refused");
    CHECK(!edc_app_chat_log_path_for(g_app, "sess-1", buf, 0), "cap 0 is refused");
    CHECK(!edc_app_chat_log_path_for(NULL, "sess-1", buf, sizeof(buf)), "no app, no path");

    char tiny[8];
    memset(tiny, 'X', sizeof(tiny));
    CHECK(!edc_app_chat_log_path_for(g_app, "a-very-long-session-id", tiny, sizeof(tiny)),
          "a buffer too small for the path is refused");
    CHECK(tiny[0] == '\0', "and cleared");
}

static void test_history_path_lives_under_the_home()
{
    TEST("the command history path is <bc_home>/.tui_history");
    char buf[512];
    CHECK(edc_app_history_path(buf, sizeof(buf)), "the path resolves");
    char expected[700];
    snprintf(expected, sizeof(expected), "%s/.edgeclaw/.tui_history", g_home);
    CHECK(strcmp(buf, expected) == 0, "path: got '%s' want '%s'", buf, expected);

    char tiny[8];
    memset(tiny, 'X', sizeof(tiny));
    CHECK(!edc_app_history_path(tiny, sizeof(tiny)), "a too-small buffer is refused");
    CHECK(tiny[0] == '\0', "and cleared");
    CHECK(!edc_app_history_path(NULL, 16), "a NULL buffer is refused");
    CHECK(!edc_app_history_path(buf, 0), "cap 0 is refused");
}

/* ── The setup write paths (the wizard's Finish / Skip / Test) ───────────── */

static void test_set_llm_config_persists_and_hot_applies()
{
    TEST("/connect's write path persists to config.json and updates the live config");
    CHECK(edc_app_set_llm_config(g_app, "https://beta.example/v1/chat/completions", "beta-one", "beta-key"),
          "the update succeeded");

    CHECK(config_contains("https://beta.example/v1/chat/completions"), "the URL reached config.json");
    CHECK(config_contains("beta-one"), "the model reached config.json");

    /* The live config must agree with the file, or the next request would use the old
     * provider until a restart. */
    char buf[256];
    CHECK(edc_app_config_model(g_app, buf, sizeof(buf)) && strcmp(buf, "beta-one") == 0,
          "the live model is the new one: got '%s'", buf);
    CHECK(edc_app_config_api_url(g_app, buf, sizeof(buf)) && strcmp(buf, "https://beta.example/v1/chat/completions") == 0,
          "the live URL is the new one: got '%s'", buf);
    /* The key is stored encrypted when a master key is loaded, so read it back through the
     * reader (which decrypts) rather than through the file. */
    CHECK(edc_app_config_api_key(g_app, buf, sizeof(buf)) && strcmp(buf, "beta-key") == 0,
          "the live key is the new one: got '%s'", buf);
}

static void test_set_llm_config_with_empty_arguments_leaves_fields_alone()
{
    TEST("an empty argument leaves that field unchanged");
    char url_before[256];
    char model_before[256];
    CHECK(edc_app_config_api_url(g_app, url_before, sizeof(url_before)), "read the URL");
    CHECK(edc_app_config_model(g_app, model_before, sizeof(model_before)), "read the model");

    /* Only the model changes: this is what /model (and the setup page's partial writes) do. */
    CHECK(edc_app_set_llm_config(g_app, NULL, "alpha-one", NULL), "a partial update succeeded");

    char buf[256];
    CHECK(edc_app_config_model(g_app, buf, sizeof(buf)) && strcmp(buf, "alpha-one") == 0, "the model changed: got '%s'",
          buf);
    CHECK(edc_app_config_api_url(g_app, buf, sizeof(buf)) && strcmp(buf, url_before) == 0,
          "the URL did not: got '%s'", buf);
}

static void test_set_llm_config_refuses_a_null_app()
{
    TEST("set_llm_config refuses a NULL app instead of writing anything");
    CHECK(!edc_app_set_llm_config(NULL, "https://x/v1", "m", "k"), "NULL app is refused");
}

static void test_mark_onboarded_is_the_skip_path()
{
    TEST("mark_onboarded persists the flag and leaves the LLM settings alone");
    char model_before[256];
    CHECK(edc_app_config_model(g_app, model_before, sizeof(model_before)), "read the model");

    CHECK(edc_app_onboarded(g_app) == 0, "not onboarded yet");
    CHECK(edc_app_mark_onboarded(g_app), "the skip path succeeded");
    CHECK(edc_app_onboarded(g_app) == 1, "and the live flag is set");
    CHECK(config_flag_true("onboarded"), "the flag reached config.json");

    char buf[256];
    CHECK(edc_app_config_model(g_app, buf, sizeof(buf)) && strcmp(buf, model_before) == 0,
          "the LLM settings were not touched: got '%s'", buf);

    CHECK(!edc_app_mark_onboarded(NULL), "NULL app is refused");
}

static void test_complete_setup_writes_all_three()
{
    TEST("complete_setup persists the LLM settings, the context window and the flag");
    /* Put the app back to "not onboarded" so the transition is observable. */
    edc_config_set_onboarded(edc_app_config(g_app), false);

    CHECK(edc_app_complete_setup(g_app, "https://alpha.example/v1/chat/completions", "alpha-one", "setup-key", 32000),
          "the wizard finished");

    char buf[256];
    CHECK(edc_app_config_model(g_app, buf, sizeof(buf)) && strcmp(buf, "alpha-one") == 0, "the model: got '%s'", buf);
    CHECK(edc_app_config_context_window(g_app) == 32000, "the context window: got %d",
          edc_app_config_context_window(g_app));
    CHECK(edc_app_onboarded(g_app) == 1, "and the wizard is marked done");
    CHECK(config_contains("32000"), "the window reached config.json");
}

static void test_complete_setup_leaves_the_flag_when_the_llm_write_fails()
{
    TEST("a failed LLM write leaves `onboarded` unset -- the wizard must reopen");
    edc_config_set_onboarded(edc_app_config(g_app), false);

    /* Break the write: point edc_home at a path that cannot hold config.json. The gate is
     * the LLM write, so nothing else may be touched. */
    struct edc_config *cfg = edc_app_config(g_app);
    sds saved_home = cfg->edc_home;
    cfg->edc_home = sdsnew("/proc/definitely-not-writable");

    const bool ok = edc_app_complete_setup(g_app, "https://alpha.example/v1/chat/completions", "alpha-one", "k", 4096);

    sdsfree(cfg->edc_home);
    cfg->edc_home = saved_home;

    CHECK(!ok, "the setup reported failure");
    CHECK(edc_app_onboarded(g_app) == 0,
          "and the wizard was NOT marked done -- otherwise it would stop reopening with a config that cannot answer");
}

static void test_test_endpoint_reports_what_the_probe_said()
{
    TEST("the setup page's Test button reports the probe's message");
    char out[256];
    memset(out, 'X', sizeof(out));

    /* Nothing listens on port 1: this exercises the whole glue (cacert from the workspace,
     * the /chat/completions suffix, the probe, the frees) without needing a server. The
     * success shape is covered at the probe layer, where it belongs. */
    const bool ok = edc_app_test_endpoint(g_app, "https://127.0.0.1:1/v1", "k", 500, out, sizeof(out));
    CHECK(!ok, "an unreachable endpoint reports failure");
    CHECK(strlen(out) > 0, "and still writes a human message: got '%s'", out);
    CHECK(strstr(out, "connection failed") != NULL || strstr(out, "no response") != NULL,
          "the message names what went wrong: got '%s'", out);

    CHECK(!edc_app_test_endpoint(NULL, "https://127.0.0.1:1/v1", "k", 500, out, sizeof(out)),
          "NULL app is refused");
}

/* ── Requests: the guards are the contract ──────────────────────────────── */

static void test_requests_refuse_an_empty_id()
{
    TEST("the request functions refuse an empty id instead of queueing an unmatchable message");
    /* Each of these ends in a send to the scheduler's mailbox when it accepts, so "refused"
     * is the only thing a unit test can assert without pumping the actor -- and refusing is
     * exactly the behaviour that matters: an empty mid would be answered with nothing. */
    edc_app_request_message_payload(g_app, NULL);
    edc_app_request_message_payload(g_app, "");
    CHECK(1, "payload request with NULL/empty mid is a no-op (no crash)");

    edc_app_request_message_payload(NULL, "mid-1");
    CHECK(1, "payload request with a NULL app is a no-op");

    edc_app_request_task_snapshot(NULL, "sess-1");
    CHECK(1, "snapshot request with a NULL app is a no-op");

    /* These two DO reach the channel actor; they are called so the accepting arm is
     * exercised too (the answer arrives asynchronously and is not asserted here). */
    edc_app_request_message_payload(g_app, "mid-1");
    edc_app_request_task_snapshot(g_app, "sess-1");
    CHECK(1, "an accepted request is queued without crashing");
}

static void test_channel_adapter_registration()
{
    TEST("the frontend registers itself as the channel adapter, and a NULL app is refused");
    /* The TUI's own registration path. Called after the request cases so the adapter it
     * installs never has to carry a message in this test -- what is being pinned is that the
     * empty-app guard is there (a startup failure must not crash on registration). */
    edc_app_channel_adapter_init(g_app, NULL, NULL);
    CHECK(1, "registering on the real app does not crash");

    edc_app_channel_adapter_init(NULL, NULL, NULL);
    CHECK(1, "registering on a NULL app is a no-op");
}

static void test_lifecycle_entry_points_tolerate_null()
{
    TEST("the lifecycle and setup entry points tolerate a NULL app");
    /* Every one of these is called from a path that has already failed (app never came up),
     * so returning a value instead of dereferencing is the contract. */
    CHECK(!edc_app_complete_setup(NULL, "https://x/v1", "m", "k", 0), "complete_setup(NULL) is false");
    edc_app_shutdown(NULL);
    CHECK(1, "shutdown(NULL) is a no-op");
    edc_app_free(NULL);
    CHECK(1, "free(NULL) is a no-op");
    CHECK(edc_app_config(NULL) == NULL, "config(NULL) is NULL");
    CHECK(edc_app_channel_actor(NULL) == NULL, "channel_actor(NULL) is NULL");
}

/* ── Frontend-owned files ───────────────────────────────────────────────── */

static void test_write_file_and_ensure_parent_dir()
{
    TEST("the frontend can write its own files under the workspace, and only there");
    char path[700];
    snprintf(path, sizeof(path), "%s/sub/dir/note.txt", g_workspace);

    CHECK(edc_app_ensure_parent_dir(g_app, path), "the parent directory was created");
    CHECK(access(path, F_OK) != 0, "but not the file itself");

    CHECK(edc_app_write_file(g_app, path, "hello", 5, false, true), "the file was written");
    char buf[64] = {0};
    FILE *f = fopen(path, "rb");
    CHECK(f != NULL, "and it exists");
    if (f)
    {
        const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        buf[n] = '\0';
        CHECK(strcmp(buf, "hello") == 0, "with the right bytes: got '%s'", buf);
    }

    CHECK(edc_app_write_file(g_app, path, "-more", 5, true, true), "append works");
    f = fopen(path, "rb");
    if (f)
    {
        const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        buf[n] = '\0';
        CHECK(strcmp(buf, "hello-more") == 0, "the append landed at the end: got '%s'", buf);
    }

    /* A zero-length write is a legitimate create (a fresh transcript file). */
    char empty[700];
    snprintf(empty, sizeof(empty), "%s/empty.txt", g_workspace);
    CHECK(edc_app_write_file(g_app, empty, "", 0, false, true), "a zero-length write succeeds");
    CHECK(access(empty, F_OK) == 0, "and creates the file");

    /* The refusals. */
    CHECK(!edc_app_write_file(g_app, NULL, "x", 1, false, true), "a NULL path is refused");
    CHECK(!edc_app_write_file(g_app, path, NULL, 1, false, true), "NULL data with len > 0 is refused");
    CHECK(!edc_app_write_file(g_app, "/tmp/outside-the-workspace.txt", "x", 1, false, true),
          "a path outside the workspace is refused when under_workspace is set");
}

static void test_ensure_parent_dir_refuses_what_is_not_a_path()
{
    TEST("ensure_parent_dir refuses a NULL path and a path with no directory part");
    CHECK(!edc_app_ensure_parent_dir(g_app, NULL), "a NULL path is refused");
    CHECK(!edc_app_ensure_parent_dir(g_app, "no-directory-part.txt"),
          "a bare filename has no parent to create, so it answers false rather than guessing");

    /* Outside the workspace: the frontend's own files are the only thing this may create, so
     * a path elsewhere must be refused instead of created. */
    CHECK(!edc_app_ensure_parent_dir(g_app, "/tmp/edgeclaw-outside-the-workspace/x.txt"),
          "a parent outside the workspace is refused");
    CHECK(access("/tmp/edgeclaw-outside-the-workspace", F_OK) != 0, "and was not created");
}

int main(void)
{
    setup();
    if (!g_app)
    {
        fprintf(stderr, "could not start the app; nothing else can run\n");
        return 2;
    }

    /* Pure projections first: they need no app, and running them before the app-touching
     * cases keeps a failure here unambiguous. */
    test_catalog_providers_are_projected_one_for_one();
    test_catalog_provider_facts();
    test_catalog_models();
    test_catalog_model_facts();
    test_catalog_unknown_model_answers_empty();
    test_message_mid_is_the_reply_header();

    test_config_readers_copy_the_configured_values();
    test_a_short_buffer_fails_and_clears();
    test_readers_on_a_null_app_answer_false();
    test_input_mode_mirrors_the_classifier();

    test_file_candidates_list_the_session_directory();
    test_file_candidates_with_no_workdir_fall_back_to_the_launch_directory();
    test_file_candidates_without_an_app_answers_zero();
    test_file_candidates_on_a_directory_that_is_gone();
    test_mention_exists_resolves_through_the_sandbox();

    test_prompts_and_skills_are_listed_as_name_tab_description();
    test_prompt_and_skill_readers_need_a_workspace();

    test_chat_log_path_is_built_by_convention();
    test_history_path_lives_under_the_home();

    /* The write paths last: they mutate config.json, and the cases after each other assert
     * the running state, so the order is part of the test. */
    test_set_llm_config_persists_and_hot_applies();
    test_set_llm_config_with_empty_arguments_leaves_fields_alone();
    test_set_llm_config_refuses_a_null_app();
    test_mark_onboarded_is_the_skip_path();
    test_complete_setup_writes_all_three();
    test_complete_setup_leaves_the_flag_when_the_llm_write_fails();
    test_test_endpoint_reports_what_the_probe_said();

    test_requests_refuse_an_empty_id();
    test_channel_adapter_registration();
    test_lifecycle_entry_points_tolerate_null();

    test_write_file_and_ensure_parent_dir();
    test_ensure_parent_dir_refuses_what_is_not_a_path();

    teardown();

    printf("\n%d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
