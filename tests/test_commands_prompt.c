/* /prompt: the template is reached as the ARGUMENT of one built-in, and what it expands
 * to becomes ONE task's input, verbatim -- a body is a DOCUMENT, not a command line, so
 * the shell never parses it (no `| ; & >`, and no `{{…}}` task references; the template
 * layer's own placeholders are substituted first, which is a different layer). This drives
 * the public edc_commands_handler entry with a spy send_llm -- handle_prompt is static --
 * and asserts on the tasks that come out.
 *
 * Harness copied from test_commands_task_new. */
#include "edc_channel_actor.h"
#include "edc_commands.h"
#include "edc_config.h"
#include "edc_compat.h"
#include "edc_resources.h"
#include "edc_scheduler.h"
#include "edc_session.h"
#include "edc_session_manager.h"
#include "edc_skills.h"
#include "edc_task.h"
#include "edc_task_group.h"
#include "edc_turn.h"
#include "utlist_fix.h"

#include <cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static int g_llm_calls = 0;
static struct edc_task *g_llm_task = NULL;

static void mock_send_llm(struct edc_scheduler *sched, struct edc_task *task, void *request_id)
{
    (void)sched;
    (void)request_id;
    g_llm_calls++;
    g_llm_task = task;
}

static void drive_command(struct edc_config *cfg, struct edc_session_manager *sm, struct edc_scheduler *sched,
                          const char *content)
{
    struct edc_channel_message *cm =
        edc_channel_message_new(edc_channel_adapter, EDC_CHANNEL_MESSAGE_USER_TYPE, sdsnew(content));
    edc_commands_handler(cfg, sm, sched, (struct edc_reply_to){.type = cm->type}, cm);
    edc_scheduler_schedule(sched); /* "this message is done" */
}

static int count_tasks(struct edc_session *sess)
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

/* 把会话里现有任务全部标记完成：否则下一条落在同组的任务会因为"组里有人没跑完"而拿到一条
 * SEQUENCE 边排到后面，于是"正文自己造了依赖"和"在队列里等"就分不开了（deps_total 都是 1）。 */
static void complete_all_tasks(struct edc_session *sess)
{
    struct edc_task_group *g;
    DL_FOREACH(sess->groups, g)
    {
        struct edc_task *t;
        LL_FOREACH2(g->members, t, group_next)
        edc_task_mark_completed(t);
    }
}

/* The first user message of a turn -- what the model actually receives. */
static const char *first_user_content(struct edc_turn *turn)
{
    if (!turn || turn->messages.tail == turn->messages.head)
        return NULL;
    cJSON *msg = turn->messages.data[turn->messages.head];
    return cJSON_GetStringValue(cJSON_GetObjectItem(msg, "content"));
}

static void write_prompt(const char *ws, const char *name, const char *content)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/prompts/%s.md", ws, name);
    FILE *f = fopen(path, "w");
    if (!f)
    {
        printf("  (cannot write %s)\n", path);
        return;
    }
    fputs(content, f);
    fclose(f);
}

int main(void)
{
    const char *ws = "/tmp/test_commands_prompt_ws";
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s && mkdir -p %s/prompts", ws, ws);
    if (system(cmd) != 0)
        return 1;

    write_prompt(ws, "review",
                 "---\n"
                 "description: Review staged changes\n"
                 "argument-hint: \"[focus]\"\n"
                 "---\n"
                 "Review the staged changes. Focus on ${1:-correctness}.\n");
    /* A body full of shell syntax: it must stay ONE task -- a body is a document, never
     * parsed as a command line. */
    write_prompt(ws, "piped", "alpha | beta\n");
    /* A body with an input-layer reference: it must stay TEXT (see the header). */
    write_prompt(ws, "refs", "summarize {{LAST}} please\n");
    /* A body that STARTS with a command-looking line: it must stay text. */
    write_prompt(ws, "recursive", "/task new inner\n");
    /* Named `x` on purpose: the strict-vs-loose prefix match on `/prompt` is only
     * OBSERVABLE when a template exists that a loose match would find -- `/promptx`
     * with `edc_commands_is_prefix` reads "x" as the template name and runs it. */
    write_prompt(ws, "x", "sneaky-template-body\n");

    struct edc_config *cfg = calloc(1, sizeof(*cfg));
    cfg->workspace = sdsnew(ws);
    /* Bodies are documents and do not care about this (see the header). It is on for the
     * two `+` cases below, which are shell syntax. */
    cfg->shell_syntax = true;
    struct edc_session_manager *sm = edc_session_manager_new(cfg);
    struct edc_session *sess = edc_session_manager_get_active(sm);
    CHECK(sess != NULL, "manager has an active session");
    if (!sess)
        return 1;

    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, sess);
    sched->send_llm = mock_send_llm;

    /* ── 展开 + 参数替换 → 变成任务的输入 ─────────────────────────── */

    TEST("/prompt <name> <arg> expands and runs as input");
    g_llm_calls = 0;
    g_llm_task = NULL;
    drive_command(cfg, sm, sched, "/prompt review focus");

    CHECK(g_llm_calls == 1, "a task was created and dispatched");
    CHECK(g_llm_task != NULL, "handed to send_llm");
    if (g_llm_task)
    {
        const char *input = first_user_content(g_llm_task->active_turn);
        CHECK(input != NULL, "the turn carries a user message");
        CHECK(input && strstr(input, "focus") != NULL, "the argument was substituted in");
        CHECK(input && strstr(input, "${1:-") == NULL, "and no placeholder survived");
        CHECK(input && strstr(input, "Review the staged changes") != NULL, "the template body is there");
    }

    /* ── `+` 不与命令语法组合 ───────────────────────────────────── */

    TEST("`+/prompt` is refused: the two syntaxes do not combine");
    /* `+` is shell syntax, `/prompt` is not. This used to run the template in a new group;
     * now it must not quietly become a task whose description is the command either. */
    const int before_combined = count_tasks(sess);
    g_llm_calls = 0;
    g_llm_task = NULL;
    drive_command(cfg, sm, sched, "+/prompt review focus");

    CHECK(count_tasks(sess) == before_combined, "no task was created");
    CHECK(g_llm_calls == 0, "and nothing ran");

    /* ── 未知名字: 只回一条错误, 不建任务 ────────────────────────── */

    TEST("/prompt <unknown> builds nothing");
    int before = count_tasks(sess);
    g_llm_calls = 0;
    g_llm_task = NULL;
    drive_command(cfg, sm, sched, "/prompt nosuchprompt");

    CHECK(count_tasks(sess) == before, "no task was created");
    CHECK(g_llm_calls == 0, "and nothing ran");

    /* ── 正文是一个原子: 算符不生效, 也不收集 {{…}} ──────────────── */

    /* 先把前面那些还没结束的任务标成完成，否则新任务会排在它们后面拿到一条 SEQUENCE 边，
     * deps_total 就不是"正文造出来的"了（见 complete_all_tasks）。 */

    TEST("a body with '|' is ONE task, verbatim (a document, not a command line)");
    complete_all_tasks(sess);
    before = count_tasks(sess);
    g_llm_calls = 0;
    g_llm_task = NULL;
    drive_command(cfg, sm, sched, "/prompt piped");

    CHECK(count_tasks(sess) == before + 1, "exactly one task was created (no pipe chain)");
    struct edc_task *piped = find_task_by_desc(sess, "/prompt piped");
    CHECK(piped != NULL, "labelled by what the user typed, not by the body");
    CHECK(piped && piped->deps_total == 0, "and it waits for nobody (the `|` made no second task)");
    {
        const char *in = piped ? first_user_content(piped->active_turn) : NULL;
        CHECK(in && strcmp(in, "alpha | beta") == 0, "the body reaches the model as written, `|` and all");
    }

    TEST("a body with `{{LAST}}` keeps it as text (no task reference is collected)");
    complete_all_tasks(sess); /* the `piped` task above is still unfinished: clear the queue */
    before = count_tasks(sess);
    g_llm_calls = 0;
    g_llm_task = NULL;
    drive_command(cfg, sm, sched, "/prompt refs");

    CHECK(count_tasks(sess) == before + 1, "one task");
    struct edc_task *refs = find_task_by_desc(sess, "/prompt refs");
    CHECK(refs != NULL, "it was created");
    CHECK(refs && refs->deps_total == 0, "and no dependency was made for the `{{…}}` in the body");
    CHECK(refs && !refs->has_deferred_vars, "nor was a deferred expansion registered");
    {
        const char *in = refs ? first_user_content(refs->active_turn) : NULL;
        CHECK(in && strstr(in, "{{LAST}}") != NULL, "the `{{LAST}}` is still there, as text");
    }

    /* ── 正文以 '/' 开头: 仍然是**文本**, 不是命令 (不变式 2) ──────── */

    TEST("a body starting with '/' is input, not a command");
    g_llm_calls = 0;
    g_llm_task = NULL;
    drive_command(cfg, sm, sched, "/prompt recursive");

    struct edc_task *literal = find_task_by_desc(sess, "/prompt recursive");
    CHECK(literal != NULL, "the task is labelled by the command the user typed");
    CHECK(literal && first_user_content(literal->active_turn) &&
              strcmp(first_user_content(literal->active_turn), "/task new inner") == 0,
          "and its INPUT is the literal body (no recursion into dispatch)");
    CHECK(find_task_by_desc(sess, "inner") == NULL, "and no '/task new' ever ran");

    /* ── 裸 /prompt: 只列出来, 不建任务 ──────────────────────────── */

    TEST("/prompt with no argument creates nothing");
    before = count_tasks(sess);
    g_llm_calls = 0;
    g_llm_task = NULL;
    drive_command(cfg, sm, sched, "/prompt");

    CHECK(count_tasks(sess) == before, "no task was created");
    CHECK(g_llm_calls == 0, "and nothing ran");

    /* ── /promptx 是未知命令, 不是模板查找 ───────────────────────── */

    TEST("/promptx is an unknown command, not a template lookup");
    before = count_tasks(sess);
    g_llm_calls = 0;
    g_llm_task = NULL;
    drive_command(cfg, sm, sched, "/promptx");

    CHECK(count_tasks(sess) == before, "no task was created (a loose prefix match would run the `x` template)");
    CHECK(g_llm_calls == 0, "and nothing ran");

    snprintf(cmd, sizeof(cmd), "rm -rf %s", ws);
    if (system(cmd) != 0)
        printf("  (cleanup failed)\n");

    /* ── /skill: 正文当输入 + 用户输入（无占位符 → 追加）+ desc 用用户原话 ── */

    TEST("/skill <name> <args>: body runs, args appended, desc = the command");
    {
        char cmd[1024];
        snprintf(cmd, sizeof(cmd), "mkdir -p %s/skills/pdf", ws);
        if (system(cmd) != 0)
            return 1;
        char path[1024];
        snprintf(path, sizeof(path), "%s/skills/pdf/SKILL.md", ws);
        FILE *f = fopen(path, "w");
        if (!f)
            return 1;
        fputs("---\ndescription: extract from PDFs\n---\nExtract text from the PDF. Summarize {{LAST}} too.\n", f);
        fclose(f);

        g_llm_calls = 0;
        g_llm_task = NULL;
        drive_command(cfg, sm, sched, "/skill pdf page 3");

        /* Not "did it dispatch": the earlier cases left unfinished tasks in front, so a task
         * created here legitimately queues behind them. What this pins is the task itself. */
        struct edc_task *pdf = find_task_by_desc(sess, "/skill pdf page 3");
        CHECK(pdf != NULL, "a task was created, labelled by the command the user typed");
        const char *in = pdf ? first_user_content(pdf->active_turn) : NULL;
        CHECK(in && strstr(in, "<skill name=\"pdf\"") != NULL, "the body is wrapped in a <skill> block (pi's shape)");
        CHECK(in && strstr(in, "References are relative to") != NULL, "which names the directory its paths hang off");
        CHECK(in && strstr(in, "Extract text from the PDF.") != NULL, "the SKILL.md body is inside it");
        CHECK(in && strstr(in, "{{LAST}}") != NULL,
              "and a `{{…}}` in the body stays TEXT -- a skill body is a document, not shell input");
        /* The point is that nothing was RECOGNIZED; deps_total here would also count the
         * queue edge behind the earlier cases' unfinished tasks (see the note above). */
        CHECK(pdf && !pdf->has_deferred_vars, "no deferred expansion was registered for it");
        CHECK(in && strstr(in, "</skill>") != NULL, "the block is closed");
        CHECK(in && strstr(in, "page 3") != NULL, "the user's words reached it (no placeholder: appended)");
        CHECK(in && (strstr(in, "page 3") > strstr(in, "</skill>")), "and they land after the closing tag");
    }

    /* ── `+` 不与 `!` 组合: 同一条规矩 ───────────────────────────── */

    TEST("`+!cmd` is refused as well");
    {
        const int before_bang = count_tasks(sess);
        g_llm_calls = 0;
        g_llm_task = NULL;
        drive_command(cfg, sm, sched, "+!git status");
        CHECK(count_tasks(sess) == before_bang, "no task was created");
        CHECK(g_llm_calls == 0, "and nothing ran");
    }

    printf("\n  %d passed, %d failed\n", passed, failed);
    return failed > 0 ? 1 : 0;
}
