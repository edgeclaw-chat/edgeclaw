/* /prompt: the template is reached as the ARGUMENT of one built-in, and what it expands
 * to is USER INPUT (it goes through edc_shell_execute, never back through the command
 * dispatch). This drives the public edc_commands_handler entry with a spy send_llm --
 * handle_prompt is static -- and asserts on the tasks that come out.
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
                 "Review the staged changes. Focus on {{1:-correctness}}.\n");
    /* A body that is a shell pipeline: only the input path can make two tasks out of it. */
    write_prompt(ws, "piped", "alpha | beta\n");
    /* A body that STARTS with a command-looking line: it must stay text. */
    write_prompt(ws, "recursive", "/task new inner\n");
    /* Named `x` on purpose: the strict-vs-loose prefix match on `/prompt` is only
     * OBSERVABLE when a template exists that a loose match would find -- `/promptx`
     * with `edc_commands_is_prefix` reads "x" as the template name and runs it. */
    write_prompt(ws, "x", "sneaky-template-body\n");

    struct edc_config *cfg = calloc(1, sizeof(*cfg));
    cfg->workspace = sdsnew(ws);
    /* The `piped` body and the `+/prompt` / `+/skill` cases are shell syntax; the switch
     * is off by default (see test_shell_switch.c for what off does). */
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
        CHECK(input && strstr(input, "{{1:-") == NULL, "and no placeholder survived");
        CHECK(input && strstr(input, "Review the staged changes") != NULL, "the template body is there");
    }

    /* ── `+/prompt`: 展开体照跑, 但落进它自己的组 ─────────────────── */

    TEST("`+/prompt <name>` runs the expanded body in a group of its own");
    struct edc_task_group *front_before = edc_scheduler_get_foreground(sess);
    const int tasks_before = count_tasks(sess);
    g_llm_calls = 0;
    g_llm_task = NULL;
    drive_command(cfg, sm, sched, "+/prompt review focus");

    CHECK(count_tasks(sess) == tasks_before + 1, "one task was created");
    CHECK(g_llm_task != NULL, "and dispatched (a fresh group has nothing to queue behind)");
    if (g_llm_task)
    {
        const char *input = first_user_content(g_llm_task->active_turn);
        CHECK(input && strstr(input, "Review the staged changes") != NULL,
              "the body still expands and runs -- the `+` is not read as part of the template name");
        CHECK(g_llm_task->group && g_llm_task->group != front_before,
              "but its group is not the one that was in front");
        CHECK(edc_scheduler_get_foreground(sess) == g_llm_task->group, "and it took the foreground");
    }

    /* ── 未知名字: 只回一条错误, 不建任务 ────────────────────────── */

    TEST("/prompt <unknown> builds nothing");
    int before = count_tasks(sess);
    g_llm_calls = 0;
    g_llm_task = NULL;
    drive_command(cfg, sm, sched, "/prompt nosuchprompt");

    CHECK(count_tasks(sess) == before, "no task was created");
    CHECK(g_llm_calls == 0, "and nothing ran");

    /* ── 正文里的管道: 证明它走的是 shell 那条路 ─────────────────── */

    TEST("a body with '|' becomes two dependent tasks");
    before = count_tasks(sess);
    g_llm_calls = 0;
    g_llm_task = NULL;
    drive_command(cfg, sm, sched, "/prompt piped");

    CHECK(count_tasks(sess) == before + 2, "two tasks were created");
    /* The two tasks are LABELLED by the command the user typed and carry the body's halves
     * as their INPUT (edc_channel_message.task_desc). */
    struct edc_task *alpha = NULL;
    struct edc_task *beta = NULL;
    {
        struct edc_task_group *g;
        DL_FOREACH(sess->groups, g)
        {
            struct edc_task *t;
            LL_FOREACH2(g->members, t, group_next)
            {
                const char *in = first_user_content(t->active_turn);
                if (in && strcmp(in, "alpha") == 0)
                    alpha = t;
                if (in && strcmp(in, "beta") == 0)
                    beta = t;
            }
        }
    }
    CHECK(alpha != NULL && beta != NULL, "alpha and beta both exist (as their inputs)");
    CHECK(alpha && alpha->description && strcmp(alpha->description, "/prompt piped") == 0,
          "labelled by what the user typed, not by the body");
    CHECK(beta != NULL && beta->deps_total > 0, "beta is the piped dependent (it has a dependency)");

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
        fputs("---\ndescription: extract from PDFs\n---\nExtract text from the PDF.\n", f);
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
        CHECK(in && strstr(in, "</skill>") != NULL, "the block is closed");
        CHECK(in && strstr(in, "page 3") != NULL, "the user's words reached it (no placeholder: appended)");
        CHECK(in && (strstr(in, "page 3") > strstr(in, "</skill>")), "and they land after the closing tag");
    }

    /* ── `+/skill`: 同一个 body, 但落进它自己的组 ──────────────────── */

    TEST("`+/skill <name>` runs the same body in a group of its own");
    {
        struct edc_task_group *front_before = edc_scheduler_get_foreground(sess);
        g_llm_calls = 0;
        g_llm_task = NULL;
        drive_command(cfg, sm, sched, "+/skill pdf page 7");

        /* A fresh group has nothing to queue behind, so this one does dispatch (unlike the
         * case above) -- and the `+` must not have leaked into the label. */
        struct edc_task *own = find_task_by_desc(sess, "/skill pdf page 7");
        CHECK(own != NULL, "a task was created, still labelled by the command (`+` stripped)");
        CHECK(own && own->group && own->group != front_before, "its group is not the one that was in front");
        CHECK(own && own->group && strcmp(own->group->group_id, own->task_id) == 0,
              "it leads a group of its own");
        CHECK(g_llm_task == own, "and it ran immediately");
        const char *in = own ? first_user_content(own->active_turn) : NULL;
        CHECK(in && strstr(in, "<skill name=\"pdf\"") != NULL, "the skill body is still what runs");
    }

    printf("\n  %d passed, %d failed\n", passed, failed);
    return failed > 0 ? 1 : 0;
}
