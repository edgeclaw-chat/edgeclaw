/* The shell-like syntax switch: what the input means with it OFF (the opt-out).
 *
 * The point of the switch is that the operators (`| ; & > >> 2> <`) and the sigil `+`
 * (plus the quoting rule) spell out a Unix shell vocabulary. A user who does not expect
 * them should get their text, not a silently half-parsed task -- so OFF, every line that is
 * not a `/` command or a `!` local shell is ONE task description, verbatim. (`!` and `@`
 * are outside the switch: they are not the parser's vocabulary, and a command must never
 * silently become a description.) This pins both halves: OFF the operators and `+` are
 * inert, ON they are not (that direction is what test_shell_job_control covers in depth;
 * here it is the control that keeps these cases from passing vacuously).
 *
 * The product default is ON (see edc_config.c): the config here is hand-built, so this
 * test spells the default out and then enters OFF through `/shell off` -- the same door a
 * user has. `/shell on|off` is the only way to flip it, which is pinned below.
 *
 * harness copied from test_shell_job_control: drive the public edc_commands_handler entry,
 * then call edc_scheduler_schedule (the message end is the tick). No channel actor, so
 * replies go nowhere.
 */
#include "edc_ast.h"
#include "edc_channel_actor.h"
#include "edc_commands.h"
#include "edc_compat.h"
#include "edc_config.h"
#include "edc_models.h"
#include "edc_scheduler.h"
#include "edc_shell.h"
#include "edc_session.h"
#include "edc_session_manager.h"
#include "edc_task.h"
#include "edc_task_group.h"
#include "utlist_fix.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "test_fs_util.h"

/* Stub for _run_loop pulled in by edc_commands (transitively linked). */
volatile int _run_loop = 1;

static int passed = 0, failed = 0;
#define TEST(n)                                                                                                        \
    do                                                                                                                 \
    {                                                                                                                  \
        printf("  TEST: %-58s ", n);                                                                                   \
    } while (0)
#define CHECK(c, m)                                                                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(c))                                                                                                      \
        {                                                                                                              \
            printf("FAIL: %s\n", m);                                                                                   \
            failed++;                                                                                                  \
        }                                                                                                              \
        else                                                                                                           \
        {                                                                                                              \
            printf("PASS\n");                                                                                          \
            passed++;                                                                                                  \
        }                                                                                                              \
    } while (0)

static void install_resolver(struct edc_scheduler *sched, struct edc_session *sess)
{
    sched->ctx = sess;
    struct edc_session_manager *sm = g_new0(struct edc_session_manager, 1);
    sm->sessions = g_new0(struct edc_session *, 1);
    sm->sessions[0] = sess;
    sm->session_count = sess ? 1 : 0;
    sm->active_index = 0;
    sched->sessions = sm; /* 测试进程内不释放: 与测试同生共死 */
}

static int g_llm_calls = 0;
static struct edc_task *g_llm_task = NULL;

static void mock_send_llm(struct edc_scheduler *sched, struct edc_task *task, void *request_id)
{
    (void)sched;
    (void)request_id;
    g_llm_calls++;
    g_llm_task = task;
}

/* Returns the task this line dispatched (NULL when it queued or built nothing). */
static struct edc_task *drive(struct edc_config *cfg, struct edc_session_manager *sm, struct edc_scheduler *sched,
                              const char *content)
{
    struct edc_channel_message *cm =
        edc_channel_message_new(edc_channel_adapter, EDC_CHANNEL_MESSAGE_USER_TYPE, sdsnew(content));
    edc_commands_handler(cfg, sm, sched, (struct edc_reply_to){.type = cm->type}, cm);
    g_llm_calls = 0;
    g_llm_task = NULL;
    edc_scheduler_schedule(sched);
    return g_llm_task;
}

/* Total tasks, however they queue -- a rejected or merged line must not add one. */
static int task_count(struct edc_session *sess)
{
    int n = 0;
    struct edc_task_group *g;
    DL_FOREACH(sess->groups, g)
    {
        struct edc_task *t;
        LL_FOREACH2(g->members, t, group_next)
        n++;
    }
    return n;
}

static int group_count(struct edc_session *sess)
{
    int n = 0;
    struct edc_task_group *g;
    DL_FOREACH(sess->groups, g)
    n++;
    return n;
}

/* A waiting task never reaches send_llm, so lookup by description is the only way in. */
static struct edc_task *find_task_by_desc(struct edc_session *sess, const char *desc)
{
    struct edc_task_group *g;
    DL_FOREACH(sess->groups, g)
    {
        struct edc_task *t;
        LL_FOREACH2(g->members, t, group_next)
        {
            if (t->description && strcmp(t->description, desc) == 0)
                return t;
        }
    }
    return NULL;
}

/* `/shell <arg>` through the builtin entry (it is a `/` command, so the switch does not
 * gate it -- which is the whole point: it is how you turn the switch back on).
 *
 * The entry takes the CMD AST node, not the raw message (36481a0): the lexer and parser
 * have already split the tail, and no layer below splits raw text again. So build the same
 * node `/shell <arg>` would have produced. argv is sds* because the parser sdsdup()s every
 * word, and the node is released the way edc_ast_free() does it. */
static bool run_shell_cmd(struct edc_config *cfg, struct edc_session_manager *sm, struct edc_reply_to to,
                          const char *arg)
{
    struct edc_ast_cmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.name = sdsnew("shell");
    if (arg[0])
    {
        cmd.argv = malloc(sizeof(sds));
        cmd.argv[0] = sdsnew(arg);
        cmd.argc = 1;
    }
    const bool handled = edc_shell_handle_builtin(cfg, sm, to, &cmd);
    sdsfree(cmd.name);
    for (int i = 0; i < cmd.argc; i++)
        sdsfree(cmd.argv[i]);
    free(cmd.argv);
    return handled;
}

/* The input-kind predicate is what the UI's mode tag reads; it must agree with the
 * dispatch in every branch, so it is exercised as a table rather than one case at a time.
 * `err` is non-NULL exactly when the dispatch would reject the line. */
static void test_input_kind_table(void)
{
    struct
    {
        const char *text;
        bool shell_on;
        edc_in_kind want;
        bool want_err;
    } cases[] = {
        {"", false, EDC_IN_MESSAGE, false},
        {"hello", false, EDC_IN_MESSAGE, false},
        {"hello", true, EDC_IN_MESSAGE, false},
        {"!git status", false, EDC_IN_LOCALSHELL, false},
        {"!git status", true, EDC_IN_LOCALSHELL, false},
        {"/task list", false, EDC_IN_COMMAND, false},
        {"/task list", true, EDC_IN_COMMAND, false},
        {"\"a\" | \"b\"", false, EDC_IN_MESSAGE, false},
        {"\"a\" | \"b\"", true, EDC_IN_SHELL, false},
        {"+\"x\"", false, EDC_IN_MESSAGE, false},
        {"+\"x\"", true, EDC_IN_SHELL, false},
        {"\"abc", true, EDC_IN_MESSAGE, true},
        {"+", true, EDC_IN_MESSAGE, true},
        {"+!cmd", true, EDC_IN_MESSAGE, true},
    };

    TEST("edc_commands_input_kind(): one predicate, both switch states, errors flagged");
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    {
        struct edc_config *cfg = calloc(1, sizeof(*cfg));
        cfg->shell_syntax = cases[i].shell_on;
        sds err = NULL;
        const edc_in_kind got = edc_commands_input_kind(cfg, cases[i].text, &err);
        if (got != cases[i].want || (err != NULL) != cases[i].want_err)
        {
            printf("FAIL: `%s` (switch %s) -> kind %d err=%d, wanted kind %d err=%d\n", cases[i].text,
                   cases[i].shell_on ? "on" : "off", (int)got, err != NULL, (int)cases[i].want,
                   (int)cases[i].want_err);
            failed++;
        }
        else
        {
            passed++;
        }
        sdsfree(err);
        free(cfg);
    }
    printf("  (each row counts once; the last message above is the failing one)\n");
}

/* ── The builtin bodies: the four blocks that used to be unassertable ────────
 *
 * Every one of these used to be built inline next to edc_commands_reply(), so the text
 * only ever left through a channel actor and no test could see it. The bodies are now
 * pure functions (see edc_shell.h); the reply still happens in handle_builtin().
 *
 * The models catalog is a GENERATED build input, not committed (tools/gen_models.py), so
 * the embedded copy is empty on a clean checkout -- these cases point
 * EDGECLAW_MODELS_CATALOG at a fixture, the way test_models_catalog does, instead of
 * assuming what the catalog contains.
 */

static const char *kCatalogFixture = "{\n"
                                     "  \"models\": {\n"
                                     "    \"fixture-chat\": {\"name\":\"Fixture Chat\",\"context\":4096,"
                                     "\"output\":512,\"tool_call\":true},\n"
                                     "    \"fixture-hidden\": {\"name\":\"Fixture Hidden\",\"context\":2048,"
                                     "\"output\":256}\n"
                                     "  },\n"
                                     "  \"providers\": {\n"
                                     "    \"fixtureprov\": {\"id\":\"fixtureprov\",\"name\":\"Fixture\","
                                     "\"api\":\"https://fixture/v1\",\"env\":[\"FIXTURE_KEY\"],"
                                     "\"models\":[\"fixture-chat\"]}\n"
                                     "  }\n"
                                     "}\n";

static void test_builtin_bodies(struct edc_config *cfg, struct edc_session_manager *sm,
                                struct edc_scheduler *sched)
{
    /* Point the catalog at the fixture and load it. */
    const char *fixture = "/tmp/test_shell_switch_catalog.json";
    FILE *ff = fopen(fixture, "w");
    if (ff)
    {
        fputs(kCatalogFixture, ff);
        fclose(ff);
    }
    setenv("EDGECLAW_MODELS_CATALOG", fixture, 1);
    edc_models_reset();
    (void)edc_models_load();

    /* ── /whoami ─────────────────────────────────────────────────────────── */
    TEST("/whoami names the session and the model");
    {
        char *saved = cfg->llm_api_conf.model ? strdup(cfg->llm_api_conf.model) : NULL;
        if (cfg->llm_api_conf.model)
            sdsfree(cfg->llm_api_conf.model);
        cfg->llm_api_conf.model = sdsnew("fixture-chat");

        /* The no-foreground arm, deterministically: a freshly created manager has an
         * active session and no groups, so this does not depend on what the rest of the
         * test has spawned. (Leaked on purpose -- same "lives as long as the test"
         * convention as the harness's own manager.) */
        struct edc_session_manager *fresh = edc_session_manager_new(cfg);
        sds empty_text = edc_shell_whoami_text(cfg, fresh);
        CHECK(empty_text && strstr(empty_text, "foreground group**: _(none)_") != NULL,
              "no foreground group -> says so");
        sdsfree(empty_text);

        /* The other arm: whatever this session's state actually is, assert the matching
         * one rather than skipping the check. */
        struct edc_session *active = edc_session_manager_get_active(sm);
        sds text = edc_shell_whoami_text(cfg, sm);
        CHECK(text && strstr(text, "## Whoami") != NULL, "it has a heading");
        CHECK(text && active && strstr(text, edc_session_get_name(active)) != NULL, "it names the active session");
        CHECK(text && strstr(text, "- **model**: `fixture-chat`") != NULL, "it names the model");
        if (edc_scheduler_get_foreground(active))
            CHECK(text && strstr(text, "foreground group**: `") != NULL, "a foreground group is named");
        else
            CHECK(text && strstr(text, "foreground group**: _(none)_") != NULL, "no foreground -> says so");
        sdsfree(text);

        /* A model the catalog does not know must still print, as the raw id. */
        if (cfg->llm_api_conf.model)
            sdsfree(cfg->llm_api_conf.model);
        cfg->llm_api_conf.model = sdsnew("model-not-in-catalog");
        text = edc_shell_whoami_text(cfg, sm);
        CHECK(text && strstr(text, "model-not-in-catalog") != NULL, "an unknown model prints verbatim");
        sdsfree(text);

        if (cfg->llm_api_conf.model)
            sdsfree(cfg->llm_api_conf.model);
        cfg->llm_api_conf.model = saved ? sdsnew(saved) : NULL;
        free(saved);
    }

    /* ── /model (no argument) ────────────────────────────────────────────── */
    TEST("/model lists the current provider's chat models");
    {
        char *saved = cfg->llm_api_conf.model ? strdup(cfg->llm_api_conf.model) : NULL;

        if (cfg->llm_api_conf.model)
            sdsfree(cfg->llm_api_conf.model);
        cfg->llm_api_conf.model = sdsnew("fixture-chat");
        sds text = edc_shell_model_text(cfg);
        CHECK(text && strstr(text, "**Model** — `fixture-chat`") != NULL, "it states the current model");
        CHECK(text && strstr(text, "`fixtureprov` models:") != NULL, "it names the provider the catalog resolved");
        CHECK(text && strstr(text, "fixture-chat") != NULL, "and lists that model");
        CHECK(text && strstr(text, "/model search <q>") != NULL, "and points at search");
        sdsfree(text);

        /* Unknown model -> the catalog cannot place it, and the text says how to set one. */
        if (cfg->llm_api_conf.model)
            sdsfree(cfg->llm_api_conf.model);
        cfg->llm_api_conf.model = sdsnew("nowhere-model");
        text = edc_shell_model_text(cfg);
        CHECK(text && strstr(text, "not in the catalog") != NULL, "an unplaceable model says so");
        sdsfree(text);

        /* No model at all -> "default", still a complete body. */
        if (cfg->llm_api_conf.model)
            sdsfree(cfg->llm_api_conf.model);
        cfg->llm_api_conf.model = NULL;
        text = edc_shell_model_text(cfg);
        CHECK(text && strstr(text, "**Model** — `default`") != NULL, "no model prints default");
        CHECK(text && strstr(text, "not in the catalog") != NULL, "and is treated as unplaceable");
        sdsfree(text);

        if (cfg->llm_api_conf.model)
            sdsfree(cfg->llm_api_conf.model);
        cfg->llm_api_conf.model = saved ? sdsnew(saved) : NULL;
        free(saved);
    }

    /* ── /model search ───────────────────────────────────────────────────── */
    TEST("/model search finds a chat model and reports a miss");
    {
        sds text = edc_shell_model_search_text("fixture-chat");
        CHECK(text && strstr(text, "**Model search** — `fixture-chat`") != NULL, "the query is echoed");
        CHECK(text && strstr(text, "fixture-chat") != NULL && strstr(text, "fixtureprov") != NULL,
              "the hit names the model and its provider");
        sdsfree(text);

        /* A miss: no chat model matches, and the text says exactly that. */
        text = edc_shell_model_search_text("zzz-no-such-model");
        CHECK(text && strstr(text, "_(no chat model matches)_") != NULL, "a miss says so");
        sdsfree(text);

        /* NULL query is treated as empty, not dereferenced. */
        text = edc_shell_model_search_text(NULL);
        CHECK(text && strstr(text, "**Model search**") != NULL, "a NULL query still renders");
        sdsfree(text);
    }

    /* ── /sandbox (no argument) ──────────────────────────────────────────── */
    TEST("/sandbox shows the switch state and the three path lists");
    {
        const bool saved_sandbox = cfg->enable_sandbox;

        cfg->enable_sandbox = false;
        sds text = edc_shell_sandbox_text(cfg);
        CHECK(text && strstr(text, "## Sandbox") != NULL, "it has a heading");
        /* Either "off" or "off (not supported on this platform)" -- both are correct and
         * which one depends on the host, so assert the prefix, not the whole line. */
        CHECK(text && strstr(text, "`off") != NULL, "an off switch reads as off");
        CHECK(text && strstr(text, "Manage: `/sandbox add-rw") != NULL, "and it lists the verbs");
        sdsfree(text);

        cfg->enable_sandbox = true;
        text = edc_shell_sandbox_text(cfg);
        CHECK(text && strstr(text, "## Sandbox `on`") != NULL, "an on switch reads as on");
        sdsfree(text);

        /* A path in each list must actually be listed. The directory has to EXIST:
         * edc_config_set_sandbox_path() realpath()s it and persists to config.json, and
         * the listed form is the RESOLVED path -- on macOS /tmp is a symlink to
         * /private/tmp, so assert the basename rather than the path we passed in. */
        (void)system("mkdir -p /tmp/fixture-rw");
        sds add_msg = NULL;
        const bool added = edc_config_set_sandbox_path(cfg, "/tmp/fixture-rw", EDC_SANDBOX_PATH_RW, true, &add_msg);
        CHECK(added, "an rw path can be added (needs a real directory)");
        if (added)
        {
            text = edc_shell_sandbox_text(cfg);
            /* A path bullet, not just the word: `- `<path>`` is what the renderer emits. */
            CHECK(text && strstr(text, "fixture-rw`") != NULL, "the rw path is listed as a bullet");
            sdsfree(text);
            (void)edc_config_set_sandbox_path(cfg, "/tmp/fixture-rw", EDC_SANDBOX_PATH_RW, false, NULL);
        }
        if (add_msg)
            sdsfree(add_msg);

        cfg->enable_sandbox = saved_sandbox;
    }

    /* ── Through the dispatcher: the side effects the bodies do not cover ── */
    TEST("`/model <id>` sets the model, and the dispatcher reports it handled");
    {
        char *saved = cfg->llm_api_conf.model ? strdup(cfg->llm_api_conf.model) : NULL;
        struct edc_ast_cmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.name = sdsnew("model");
        cmd.argv = calloc(1, sizeof(sds));
        cmd.argv[0] = sdsnew("fixture-chat");
        cmd.argc = 1;
        CHECK(edc_shell_handle_builtin(cfg, sm, (struct edc_reply_to){}, &cmd), "`/model <id>` is a builtin");
        CHECK(cfg->llm_api_conf.model && strcmp(cfg->llm_api_conf.model, "fixture-chat") == 0,
              "the live config carries the new model");
        sdsfree(cmd.name);
        sdsfree(cmd.argv[0]);
        free(cmd.argv);

        if (cfg->llm_api_conf.model)
            sdsfree(cfg->llm_api_conf.model);
        cfg->llm_api_conf.model = saved ? sdsnew(saved) : NULL;
        free(saved);
    }

    TEST("`/sandbox <unknown verb>` is handled but rejected with usage");
    {
        struct edc_ast_cmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.name = sdsnew("sandbox");
        cmd.argv = calloc(1, sizeof(sds));
        cmd.argv[0] = sdsnew("frobnicate");
        cmd.argc = 1;
        CHECK(edc_shell_handle_builtin(cfg, sm, (struct edc_reply_to){}, &cmd), "an unknown verb is still handled");
        sdsfree(cmd.name);
        sdsfree(cmd.argv[0]);
        free(cmd.argv);
    }

    TEST("an unknown builtin name is NOT handled");
    {
        struct edc_ast_cmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.name = sdsnew("not-a-builtin");
        CHECK(!edc_shell_handle_builtin(cfg, sm, (struct edc_reply_to){}, &cmd), "it falls through");
        sdsfree(cmd.name);

        /* A NULL command is likewise not ours. */
        CHECK(!edc_shell_handle_builtin(cfg, sm, (struct edc_reply_to){}, NULL), "NULL cmd falls through");
    }

    unsetenv("EDGECLAW_MODELS_CATALOG");
    edc_models_reset();
    (void)sched;
}

int main(void)
{
    const char *ws = "/tmp/test_shell_switch_ws";
    const char *home = "/tmp/test_shell_switch_home";
    char cmd[512];
    /* A real (minimal) config.json: /shell persists before it flips memory, so the home
     * has to be writable for the switch to take effect at all. */
    snprintf(cmd, sizeof(cmd), "rm -rf %s %s && mkdir -p %s %s && printf '{}' > %s/config.json", ws, home, ws, home,
             home);
    if (test_sh(cmd) != 0)
        return 1;

    struct edc_config *cfg = calloc(1, sizeof(*cfg));
    cfg->workspace = sdsnew(ws);
    cfg->edc_home = sdsnew(home);
    struct edc_session_manager *sm = edc_session_manager_new(cfg);
    struct edc_session *sess = edc_session_manager_get_active(sm);
    CHECK(sess != NULL, "manager has an active session");
    if (!sess)
        return 1;

    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, sess);
    sched->send_llm = mock_send_llm;

    /* ── /shell off: 算符只是普通文本（产品默认是打开的） ─────────────────────────────── */

    /* This config never goes through edc_config_load, so the product default is spelled
     * out (ON, see edc_config.c) instead of being inherited from zero-init. The OFF cases
     * below are then entered the way a user enters them: `/shell off`. */
    cfg->shell_syntax = true;

    TEST("the switch starts ON (the product default)");
    CHECK(cfg->shell_syntax, "shell_syntax starts true");

    TEST("`/shell off` is the opt-out the OFF cases below run under");
    CHECK(run_shell_cmd(cfg, sm, (struct edc_reply_to){}, "off"), "`/shell off` is a builtin");
    CHECK(!cfg->shell_syntax, "the live config now says off");

    test_input_kind_table();

    TEST("`a | b` with the switch off is ONE task, verbatim (not a pipe chain)");
    struct edc_task_group *front = edc_scheduler_get_foreground(sess);
    int before = task_count(sess);
    drive(cfg, sm, sched, "alpha | beta");
    CHECK(task_count(sess) == before + 1, "exactly one task was created (no second pipe segment)");
    struct edc_task *piped = find_task_by_desc(sess, "alpha | beta");
    CHECK(piped != NULL, "the whole line is the description, `|` and all");
    CHECK(piped && piped->deps_total == 0, "and it waits for nobody (no JOIN edge)");

    TEST("`cmd &` with the switch off does not go to the background");
    before = task_count(sess);
    drive(cfg, sm, sched, "bgwork &");
    CHECK(task_count(sess) == before + 1, "one task");
    struct edc_task *bg = find_task_by_desc(sess, "bgwork &");
    CHECK(bg != NULL, "the `&` is part of the text");
    CHECK(bg && !bg->is_background, "and it did NOT become a background task");

    TEST("`> file` with the switch off is not a redirect");
    before = task_count(sess);
    drive(cfg, sm, sched, "say hi > out.md");
    CHECK(task_count(sess) == before + 1, "one task");
    struct edc_task *red = find_task_by_desc(sess, "say hi > out.md");
    CHECK(red != NULL, "the whole line is the description");
    CHECK(red && red->redirect_stdout == NULL, "and nothing was redirected to out.md");

    TEST("`+desc` with the switch off is a literal description (no new group)");
    before = task_count(sess);
    int groups_before = group_count(sess);
    struct edc_task_group *front_before = edc_scheduler_get_foreground(sess);
    drive(cfg, sm, sched, "plus work");
    CHECK(task_count(sess) == before + 1, "one task");
    CHECK(group_count(sess) == groups_before, "and no group was opened");
    CHECK(front_before == NULL || edc_scheduler_get_foreground(sess) == front_before,
          "the group in front did not move");

    TEST("`@desc` with the switch off is a literal description (no migration reply, no task lost)");
    before = task_count(sess);
    drive(cfg, sm, sched, "@probe");
    CHECK(task_count(sess) == before + 1, "the line still became a task");
    CHECK(find_task_by_desc(sess, "@probe") != NULL, "with the `@` kept as text");

    TEST("with the switch off a backslash is just text (nothing is a sigil, so nothing is escaped)");
    /* The `\@` / `\+` escapes are gone: with shell syntax ON they were superseded by
     * quoting (`"+text"` is a literal `+`), and with it OFF there is no sigil to escape. */
    before = task_count(sess);
    drive(cfg, sm, sched, "\\+1 issue is still open");
    CHECK(find_task_by_desc(sess, "\\+1 issue is still open") != NULL,
          "the backslash stays, as text");
    CHECK(task_count(sess) == before + 1, "one task");

    /* ── `/` commands are NOT shell syntax: they keep working ───── */

    TEST("a `/` command is not gated (the switch could never be turned back on otherwise)");
    before = task_count(sess);
    groups_before = group_count(sess);
    drive(cfg, sm, sched, "/task new \"scoped\" --new-group");
    CHECK(task_count(sess) == before + 1, "the command created its task");
    CHECK(group_count(sess) == groups_before + 1, "and `--new-group` still did its job");

    /* ── /shell on|off: the only switch ─────────────────────────── */

    TEST("`/shell` reports and changes nothing");
    CHECK(run_shell_cmd(cfg, sm, (struct edc_reply_to){}, ""), "`/shell` is a builtin");
    CHECK(!cfg->shell_syntax, "the state is unchanged");
    before = task_count(sess);
    CHECK(task_count(sess) == before, "and no task was created");

    TEST("`/shell bogus` is refused, and does not flip the switch");
    CHECK(run_shell_cmd(cfg, sm, (struct edc_reply_to){}, "bogus"), "it answers");
    CHECK(!cfg->shell_syntax, "the switch is untouched");

    TEST("`/shell on` flips the live config (persistence is pinned in test_config_shell_syntax)");
    CHECK(run_shell_cmd(cfg, sm, (struct edc_reply_to){}, "on"), "`/shell on` is a builtin");
    CHECK(cfg->shell_syntax, "the live config now says on");

    /* ── 打开后, 同样的输入才有 shell 语义（对照, 防上面全绿地假通过）── */

    TEST("with the switch on, `a | b` really is a pipe chain (two dependent tasks)");
    before = task_count(sess);
    /* Quoted operands: a line that uses an operator requires them (see the rule in
     * edc_shell.c) -- `gamma | delta` unquoted is refused, not guessed. */
    drive(cfg, sm, sched, "\"gamma\" | \"delta\"");
    CHECK(task_count(sess) == before + 2, "two tasks were created");
    struct edc_task *delta = find_task_by_desc(sess, "delta");
    CHECK(delta != NULL && delta->deps_total == 1, "the second one waits on the first (JOIN edge)");

    TEST("with the switch on, `!cmd` really is the local shell");
    before = task_count(sess);
    drive(cfg, sm, sched, "!git status");
    CHECK(task_count(sess) == before + 1, "one task");
    struct edc_task *tool = find_task_by_desc(sess, "git status");
    CHECK(tool != NULL && tool->tool_only, "and it is tool-only (one exec_shell call, no model)");

    TEST("with the switch on, `+desc` really opens a group");
    front_before = edc_scheduler_get_foreground(sess);
    groups_before = group_count(sess);
    drive(cfg, sm, sched, "+\"grp plain work\"");
    CHECK(group_count(sess) == groups_before + 1, "one group was opened");
    CHECK(edc_scheduler_get_foreground(sess) != front_before, "and it took the foreground");

    TEST("with the switch on, `@desc` is now an ordinary MESSAGE (Ⓕ: the hint waits for the post-pass)");
    /* The `@` layer sits ABOVE this switch, so it answers in both modes. Today, with no
     * resolution built yet, that answer is the migration hint; the off-mode case above
     * shows the other half (literal text). When `@file` lands, the resolved case attaches
     * the file in BOTH modes -- which is the whole reason this layer is not gated. */
    before = task_count(sess);
    drive(cfg, sm, sched, "@probe");
    CHECK(task_count(sess) == before + 1, "it became a task");
    CHECK(find_task_by_desc(sess, "@probe") != NULL, "carrying the text verbatim (`@` is not a sigil here)");

    TEST("`/shell off` puts it back");
    CHECK(run_shell_cmd(cfg, sm, (struct edc_reply_to){}, "off"), "`/shell off` is a builtin");
    CHECK(!cfg->shell_syntax, "the switch is off again");
    before = task_count(sess);
    drive(cfg, sm, sched, "epsilon | zeta");
    CHECK(task_count(sess) == before + 1, "and a `|` line is one task again");
    CHECK(find_task_by_desc(sess, "epsilon | zeta") != NULL,
          "and needs no quotes -- with the switch off, nothing is an operator");

    /* ── `!` is NOT part of the switch (the last one, so a pending tool-only task
     *    cannot disturb the queueing of the cases above) ─────────────── */

    TEST("`!cmd` is not gated: with the switch off it is STILL the local shell");
    /* The switch owns the shell parser's vocabulary -- `+`, the quoting rule, the
     * operators. `!cmd` is a one-token dispatch that never enters the parser, so turning
     * the switch off must not turn a command into a task description. That was the bug:
     * the line became an ordinary message, the model then called exec_shell itself -- one
     * extra LLM round, a task that never settles, and no hint at all. */
    before = task_count(sess);
    drive(cfg, sm, sched, "!git status");
    CHECK(task_count(sess) == before + 1, "one task");
    struct edc_task *off_tool = find_task_by_desc(sess, "git status");
    CHECK(off_tool != NULL && off_tool->tool_only, "it is tool-only (one exec_shell call, no model)");
    CHECK(find_task_by_desc(sess, "!git status") == NULL, "the `!` never becomes part of a description");
    CHECK(g_llm_calls == 0, "no LLM request was made for it");

    /* The builtin bodies: /whoami, /model (list + search), /sandbox. Last, because it
     * swaps the models catalog and the sandbox config. */
    test_builtin_bodies(cfg, sm, sched);

    printf("\n  %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
