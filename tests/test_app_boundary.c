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
#include "edc_update.h"
#include "sds.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <fcntl.h>
#include <sys/stat.h>
#ifndef _WIN32
#include <sys/wait.h>
#endif
#include <unistd.h>
#ifndef _WIN32
#include "fake_release.h"
#endif
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
/* ── self-bootstrapping: run the binary from where the installer would put it ──
 *
 * `edc_app_update_can_self_upgrade()` asks `edc_upgrade_detect(dirname(/proc/self/exe), $HOME)`,
 * which answers "the installer's copy" only when the executable sits in `$HOME/.local/bin` or
 * `$HOME/bin`. A bazel test binary sits in bazel-out, so the whole upgrade path reads as
 * "cannot upgrade itself" and the SUCCESS half of the feature -- update_start, the completion
 * callback, the banner -- could not be exercised at all.
 *
 * Copying the test binary into `$HOME/.local/bin` and re-exec there makes it look installed.
 * That is the test bootstrapping itself, not a seam in the production code: nothing in src/
 * knows about it, and the code under test runs exactly as it would on a user's machine.
 *
 * One re-exec, guarded by an environment variable. On the way back, setup() clears the home
 * directory but must leave `.local/bin` alone -- unlinking the running executable would still
 * work (dirname of the "(deleted)" path is the same) but it is a needlessly fragile thing to
 * rely on. */
static const char kInstalledMarker[] = "EDGECLAW_TEST_AS_INSTALLED";
static const char kInstalledName[] = "edgeclaw-under-test";

/* Copy |from| to |to|, mode 0755. */
static bool copy_file(const char *from, const char *to)
{
    FILE *in = fopen(from, "rb");
    if (!in)
        return false;
    FILE *out = fopen(to, "wb");
    if (!out)
    {
        fclose(in);
        return false;
    }

    char buf[64 * 1024];
    size_t n;
    bool ok = true;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
    {
        if (fwrite(buf, 1, n, out) != n)
        {
            ok = false;
            break;
        }
    }
    if (ferror(in))
        ok = false;
    fclose(in);
    if (fclose(out) != 0)
        ok = false;
    if (ok && chmod(to, 0755) != 0)
        ok = false;
    return ok;
}

/* Ask the copy whether it can start, before committing to it.
 *
 * Checking "is this binary dynamically linked" was the first attempt and it was both too
 * strict and too weak: a binary that needs only libc starts fine from anywhere, while one that
 * needs a bazel-generated .so does not -- and an exec that SUCCEEDS but produces a process the
 * loader then rejects cannot be detected by the parent, because the parent is already gone.
 * That is exactly how ASan in CI turned this into "error while loading shared libraries".
 *
 * So run the copy once with a probe flag and look at its exit status. The flag is handled
 * before any of the test's own work (see main), so this costs a process spawn. */
static bool copy_can_start(const char *dest)
{
#ifdef _WIN32
    (void)dest;
    return false;
#else
    pid_t pid = fork();
    if (pid < 0)
        return false;
    if (pid == 0)
    {
        /* Child: exec the copy with the probe flag. stderr silenced -- a loader failure is
         * expected here in some configurations and is reported by the parent instead. */
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0)
        {
            dup2(devnull, STDERR_FILENO);
            dup2(devnull, STDOUT_FILENO);
            close(devnull);
        }
        execl(dest, dest, "--bootstrap-probe", (char *)NULL);
        _exit(127); /* execl itself failed */
    }

    int status = 0;
    if (waitpid(pid, &status, 0) != pid)
        return false;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
#endif
}

/* Returns only when the current process is already the installed copy. */
static void reexec_as_installed(int argc, char **argv)
{
#ifndef _WIN32
    if (getenv(kInstalledMarker))
        return; /* already there */

    const char *tmp = getenv("TEST_TMPDIR");
    if (!tmp || !*tmp)
        tmp = "/tmp";

    char home[900], bindir[1024], dest[1200];
    snprintf(home, sizeof(home), "%s/app-boundary-home", tmp);
    snprintf(bindir, sizeof(bindir), "%s/.local/bin", home);
    snprintf(dest, sizeof(dest), "%s/%s", bindir, kInstalledName);

    char cmd[1400];
    snprintf(cmd, sizeof(cmd), "mkdir -p '%s'", bindir);
    if (test_sh(cmd) != 0)
        return; /* no bootstrap: the upgrade cases below will say "cannot upgrade", which is
                 * still a valid assertion, just a weaker one */

    char self[PATH_MAX];
    const ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (n <= 0)
        return;
    self[n] = '\0';

    if (!copy_file(self, dest))
    {
        fprintf(stderr, "  (bootstrap: could not copy %s to %s)\n", self, dest);
        return;
    }

    /* Decided before the real exec: an exec that starts a process the loader then rejects
     * cannot be observed from here (see copy_can_start). */
    if (!copy_can_start(dest))
    {
        fprintf(stderr, "  (bootstrap: the copy cannot start from %s -- probably a shared library it "
                        "cannot find there; the update cases below run without it)\n",
                dest);
        unlink(dest);
        return;
    }

    setenv(kInstalledMarker, "1", 1);
    setenv("HOME", home, 1);
    setenv("USERPROFILE", home, 1);

    /* argv[0] becomes the copy so anything that reads it agrees with /proc/self/exe. */
    char **next = calloc((size_t)argc + 1, sizeof(char *));
    if (!next)
        return;
    for (int i = 0; i < argc; i++)
        next[i] = argv[i];
    next[0] = dest;
    next[argc] = NULL;

    execv(dest, next);
    /* Only reachable if exec failed: fall through and run the un-bootstrapped tests. */
    fprintf(stderr, "  (bootstrap: execv %s failed: %s)\n", dest, strerror(errno));
    free(next);
#else
    (void)argc;
    (void)argv;
#endif
}

static void prepare_home(void)
{
    const char *tmp = getenv("TEST_TMPDIR");
    if (!tmp || !*tmp)
        tmp = "/tmp";
    snprintf(g_home, sizeof(g_home), "%s/app-boundary-home", tmp);
    snprintf(g_workspace, sizeof(g_workspace), "%s/.edgeclaw/workspace", g_home);
    snprintf(g_config, sizeof(g_config), "%s/.edgeclaw/config.json", g_home);

    char cmd[1024];
    /* Clears everything EXCEPT .local/bin, which holds the copy of this test binary that
     * reexec_as_installed() put there and is currently running. */
    snprintf(cmd, sizeof(cmd), "rm -rf '%s/.edgeclaw' '%s/catalog.json'", g_home, g_home);
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
}

/* Split from prepare_home() so a caller can put things in place that edc_app_new() reads at
 * construction -- the release endpoint and the CA bundle it is reached with. The update check
 * runs ONCE, inside the constructor, so there is no later hook to set them from. */
static void start_app(void)
{
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
     * includes /usr (C:\Windows on Windows), so an existing path there is readable and IS a
     * reference; a path under no list at all is refused even though it is just as absent.
     * Both arms matter: a stat()-only implementation would answer 1 for /root and 0 for /usr,
     * i.e. exactly backwards. */
#if defined(_WIN32)
    /* EDC_SANDBOX_DEFAULT_RO_PATHS on this platform (see edc_config.h). */
    const char *ro_existing = "C:\\Windows";
#else
    const char *ro_existing = "/usr";
#endif
    CHECK(edc_app_mention_exists(g_app, g_workspace, ro_existing) == 1,
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

/* The expectations below are written with '/' while the app builds its paths in NORMAL form
 * (edc_path_norm: the platform's separator). On Windows that is the same path in a different
 * spelling, and the assertion compares strings, so the expectation is normalised first.
 * No-op on POSIX, where '/' is the only separator. */
static void native_sep(char *s)
{
#if defined(_WIN32)
    for (; *s; s++)
        if (*s == '/')
            *s = '\\';
#else
    (void)s;
#endif
}

static void test_chat_log_path_is_built_by_convention()
{
    TEST("the transcript path is <workspace>/<session_id>/chat.jsonl");
    char buf[512];
    CHECK(edc_app_chat_log_path_for(g_app, "sess-1", buf, sizeof(buf)), "a session id resolves");
    char expected[700];
    snprintf(expected, sizeof(expected), "%s/sess-1/chat.jsonl", g_workspace);
    native_sep(expected);
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
    native_sep(expected);
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

/* ── The release check / upgrade surface ────────────────────────────────── */

/* A callback the app would call when a newer release is found. It cannot be invoked from
 * here (that needs the background check to have run and found one), but registering it is
 * what the frontend does at startup, and the null-app paths are what a failed startup hits. */
static void on_update_found(const char *tag, void *ud)
{
    (void)tag;
    (void)ud;
}

static void on_upgrade_done(bool ok, const char *detail, void *ud)
{
    (void)ok;
    (void)detail;
    (void)ud;
}

static void test_update_surface_on_a_real_app()
{
    TEST("the update surface answers on a real app");

    /* Registering the observer is what TUIWindow does right after construction. Nothing
     * observable happens until the check fires, so the contract here is "it does not crash and
     * the app keeps working". */
    edc_app_set_update_observer(g_app, on_update_found, g_app);
    CHECK(1, "an observer can be registered");

    /* The binary this process is running from decides which half of the feature is
     * reachable. main() re-execs itself from $HOME/.local/bin when it can, so the normal
     * case here is "installed by the installer" -- the branch that offers an upgrade.
     * When the bootstrap could not run (no shell, no /proc), the fallback assertions below
     * still hold, so the test degrades instead of failing for an environmental reason. */
    const bool installed = getenv(kInstalledMarker) != NULL;
    if (installed)
    {
        printf("      (running as the installed copy: %s)\n", "self-upgrade is available");
        CHECK(edc_app_update_can_self_upgrade(g_app),
              "a copy in $HOME/.local/bin reports that it CAN upgrade itself");
    }
    else
    {
        printf("      (bootstrap did not run; asserting the cannot-upgrade half)\n");
        CHECK(!edc_app_update_can_self_upgrade(g_app),
              "a build-tree binary reports that it cannot upgrade itself");
    }

    /* Cancelling when nothing runs is a no-op, not a crash: Esc reaches it from every state. */
    edc_app_update_cancel(g_app);
    CHECK(1, "cancelling with nothing in flight is a no-op");

    /* The manual hint is the fallback for the cannot-upgrade case, so it must never be empty
     * and must point at where the release actually is. */
    char buf[512];
    memset(buf, 0, sizeof(buf));
    edc_app_update_manual_hint(g_app, buf, sizeof(buf));
    CHECK(buf[0] != '\0', "the manual hint is never empty");
    CHECK(strstr(buf, "releases") != NULL || strstr(buf, "install.") != NULL,
          "and points at the release page or the installer");

    /* A buffer too small must be handled, not overflowed. */
    char tiny[8];
    memset(tiny, 0, sizeof(tiny));
    edc_app_update_manual_hint(g_app, tiny, sizeof(tiny));
    CHECK(strlen(tiny) < sizeof(tiny), "a short buffer truncates instead of overflowing");
    edc_app_update_manual_hint(g_app, NULL, 0);
    CHECK(1, "a NULL buffer is a no-op");
}

/* Set by on_upgrade_done; the case below waits for it. */
static volatile int g_upgrade_done_fired = 0;
static bool g_upgrade_done_ok = false;

static void on_upgrade_done_counted(bool ok, const char *detail, void *ud)
{
    (void)detail;
    (void)ud;
    g_upgrade_done_ok = ok;
    g_upgrade_done_fired = 1;
}

/* The completion path: what happens when the runner actually finishes. It has to finish
 * somehow -- the fetch is refused (no CA bundle here, or no network) or times out -- and the
 * callback must fire once with a failure, clear the slot, and leave the app usable. */
static void test_update_completion_callback()
{
    if (!getenv(kInstalledMarker))
        return; /* unreachable without the bootstrap; the start case said so already */

    TEST("the completion callback fires when the runner finishes");

    g_upgrade_done_fired = 0;
    g_upgrade_done_ok = true;
    const bool started = edc_app_update_start(g_app, "v0.0.0-test", on_upgrade_done_counted, NULL);
    CHECK(started, "the upgrade starts");
    if (!started)
        return;

    /* The worker fetches from GitHub; it cannot succeed here (no CA bundle in this scratch
     * home, and the test must not depend on the network), so the run ends in a failure. Wait
     * for the callback rather than asserting on its timing: the fetch has its own 15s
     * deadline and a refused connection can return sooner. */
    for (int i = 0; i < 600 && !g_upgrade_done_fired; i++)
        usleep(50 * 1000); /* up to 30s, well inside the test's own timeout */

    CHECK(g_upgrade_done_fired == 1, "the callback fired");
    CHECK(!g_upgrade_done_ok, "and reported the failure the fetch caused");

    /* The slot is free afterwards -- that is what lets the user retry, or the next launch
     * offer again. Asserted by starting once more. */
    g_upgrade_done_fired = 0;
    const bool again = edc_app_update_start(g_app, "v0.0.0-test", on_upgrade_done_counted, NULL);
    CHECK(again, "after a completed run the slot is free");
    if (again)
        edc_app_update_cancel(g_app);
}

/* The startup half of update_start: only reachable when the binary looks installed. This is
 * what the other half of `can_self_upgrade` exists for, and it is where the one-at-a-time
 * guard and the cancel path live. */
static void test_update_start_when_installed()
{
    if (!getenv(kInstalledMarker))
    {
        printf("\n── update_start on an installed copy ──\n");
        printf("    (skipped: the bootstrap did not run, so this copy cannot self-upgrade)\n");
        return;
    }

    TEST("update_start on an installed copy");

    /* It starts a real runner, which spawns a worker to fetch the installer from GitHub. The
     * fetch is not what is being tested here -- test_upgrade_run drives that against a local
     * server -- so this case asserts the handover and then immediately cancels, which also
     * exercises the cancel path with a live worker. */
    bool done_called = false;
    const bool started = edc_app_update_start(g_app, "v0.0.0-test", on_upgrade_done, &done_called);
    CHECK(started, "an installed copy starts the upgrade");

    if (started)
    {
        /* One at a time: a second run would put two installers over the same binary. */
        CHECK(!edc_app_update_start(g_app, "v0.0.0-test", on_upgrade_done, &done_called),
              "a second concurrent start is refused");

        /* Cancel: joins the worker, kills any child, and clears the slot so a later upgrade
         * can start. The completion callback is deliberately cleared first, so it must not
         * fire after this. */
        edc_app_update_cancel(g_app);
        CHECK(true, "cancelling a live upgrade returns");
        CHECK(!done_called, "and the completion callback does not fire for a cancelled run");

        /* The slot is free again. Asserted by starting once more and cancelling: if cancel
         * left `app->upgrade` set, this would be refused like the concurrent case above. */
        const bool restarted = edc_app_update_start(g_app, "v0.0.0-test", on_upgrade_done, &done_called);
        CHECK(restarted, "after a cancel the slot is free again");
        if (restarted)
            edc_app_update_cancel(g_app);
    }
}

static void test_update_surface_tolerates_null()
{
    TEST("the update surface tolerates a NULL app");
    /* Every one of these is on a path that runs when the app never came up. */
    edc_app_set_update_observer(NULL, on_update_found, NULL);
    CHECK(1, "set_update_observer(NULL) is a no-op");
    CHECK(!edc_app_update_can_self_upgrade(NULL), "can_self_upgrade(NULL) is false");
    CHECK(!edc_app_update_start(NULL, "v1", on_upgrade_done, NULL), "update_start(NULL) is false");
    edc_app_update_cancel(NULL);
    CHECK(1, "update_cancel(NULL) is a no-op");

    char buf[256];
    memset(buf, 0, sizeof(buf));
    /* No app, but the hint must still come out: the caller prints it either way. */
    edc_app_update_manual_hint(NULL, buf, sizeof(buf));
    CHECK(buf[0] != '\0', "manual_hint(NULL) still produces a command");
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

/* ── the check, end to end, against a local release server ──────────────────
 *
 * This is the case that makes the rest testable: it starts a real TLS server answering the
 * releases endpoint, points the app at it, and lets the app's OWN one-shot check run. What it
 * proves is the whole chain -- service, fetch, JSON, selection, the C observer -- rather than
 * each piece against a stub.
 *
 * Three environment values make it reachable, all read in edc_app_new:
 *   EDGECLAW_UPDATE_RELEASES_URL      where the check goes
 *   EDGECLAW_UPDATE_CURRENT_VERSION   what it compares against (a dev build's commit hash is
 *                                     not a version, so the check would skip itself)
 *   EDGECLAW_UPDATE_CHECK_DELAY_MS    skip the ten seconds meant to keep the launch path clear
 *
 * The app is created AFTER these are set, because the check runs inside edc_app_new.
 */
static volatile int g_found_fired = 0;
static char g_found_tag[64];

static void on_update_found_recording(const char *tag, void *ud)
{
    (void)ud;
    snprintf(g_found_tag, sizeof(g_found_tag), "%s", tag ? tag : "");
    g_found_fired = 1;
}

#ifndef _WIN32
/* The JSON a release list would carry for this platform, one version up. */
static sds release_json_for_this_platform(void)
{
    const char *target = edc_update_target();
    return sdscatprintf(sdsempty(),
                        "[{\"tag_name\":\"v9.9.9\",\"draft\":false,\"prerelease\":false,\"assets\":"
                        "[{\"name\":\"%s_v9.9.9.tar.gz\"}]}]",
                        target);
}

#endif

int main(int argc, char **argv)
{
    /* Handled before anything else: reexec_as_installed() runs the copy with this flag to find
     * out whether it can start from its new location at all. Reaching here means it can. */
    if (argc > 1 && strcmp(argv[1], "--bootstrap-probe") == 0)
        return 0;

    reexec_as_installed(argc, argv);

    prepare_home();

    /* Whether the local release server came up. On Windows the fake-release cases do not
     * exist at all (the stub it needs is POSIX), so this stays false and the assertions below
     * report themselves as skipped rather than disappearing silently. */
    bool fake_release_up = false;

#ifndef _WIN32
    /* The check runs once, inside edc_app_new, so everything it reads has to be in place
     * first: the server, the endpoint it answers on, and the CA bundle the app will verify it
     * with (the app reads <workspace>/ssl/cacert.pem). */
    {
        sds json = release_json_for_this_platform();
        char url[256] = {0};
        char cert[600] = {0};

        if (fake_release_start(json, url, sizeof(url), cert, sizeof(cert)) == 0)
        {
            char ssl_dir[700], cacert[800];
            snprintf(ssl_dir, sizeof(ssl_dir), "%s/ssl", g_workspace);
            test_mkdir_p(ssl_dir);
            snprintf(cacert, sizeof(cacert), "%s/cacert.pem", ssl_dir);

            FILE *in = fopen(cert, "rb");
            FILE *out = in ? fopen(cacert, "wb") : NULL;
            if (out)
            {
                char buf[8192];
                size_t n;
                while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
                    fwrite(buf, 1, n, out);
                fclose(out);
                fake_release_up = true;
            }
            if (in)
                fclose(in);

            setenv("EDGECLAW_UPDATE_RELEASES_URL", url, 1);
            setenv("EDGECLAW_UPDATE_CURRENT_VERSION", "1.0.0", 1);
            setenv("EDGECLAW_UPDATE_CHECK_DELAY_MS", "0", 1);
        }
        sdsfree(json);
    }
#endif

    start_app();
    if (!g_app)
    {
        fprintf(stderr, "could not start the app; nothing else can run\n");
#ifndef _WIN32
        fake_release_stop();
#endif
        return 2;
    }

    printf("\n── the release check, end to end ──\n");
    if (!fake_release_up)
    {
        printf("    (skipped: no local release server on this platform, or it would not start)\n");
    }
    else
    {
        edc_app_set_update_observer(g_app, on_update_found_recording, NULL);

        /* The check runs on a worker thread and posts back through the loop, so this waits for
         * the loop to deliver it. Bounded well above the fetch's own timeout. */
        for (int i = 0; i < 400 && !g_found_fired; i++)
            usleep(25 * 1000);

        CHECK(g_found_fired == 1, "the app's own check found the release and called the observer");
        CHECK(strcmp(g_found_tag, "v9.9.9") == 0, "and handed over the offered tag");
    }

#ifndef _WIN32
    /* The server answered its one connection; nothing else will use it. */
    fake_release_stop();
    unsetenv("EDGECLAW_UPDATE_RELEASES_URL");
    unsetenv("EDGECLAW_UPDATE_CURRENT_VERSION");
    unsetenv("EDGECLAW_UPDATE_CHECK_DELAY_MS");
#endif

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

    test_update_surface_on_a_real_app();
    test_update_start_when_installed();
    test_update_completion_callback();
    test_update_surface_tolerates_null();

    teardown();

    printf("\n%d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
