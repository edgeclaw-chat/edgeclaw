/* /prompt: the template is reached as the ARGUMENT of one built-in, and what it expands
 * to becomes ONE task's input, verbatim -- a body is a DOCUMENT, not a command line, so
 * the shell never parses it (no `| ; & >`, and no `{{…}}` task references; the template
 * layer's own placeholders are substituted first, which is a different layer). This drives
 * the public edc_commands_handler entry with a spy send_llm -- handle_prompt is static --
 * and asserts on the tasks that come out.
 *
 * Harness copied from test_commands_task_new. */
#include "edc_channel_actor.h"
#include "test_ws.h"
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

static int count_groups(struct edc_session *sess)
{
    int n = 0;
    struct edc_task_group *g;
    DL_FOREACH(sess->groups, g)
    n++;
    return n;
}

/* The task created last (the description is truncated to 30 chars, so it is a poor key). */
static struct edc_task *last_member(struct edc_task_group *group)
{
    struct edc_task *last = NULL, *t;
    if (!group)
        return NULL;
    LL_FOREACH2(group->members, t, group_next)
    last = t;
    return last;
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
 * 顺序边（BARRIER）排到后面，于是"正文自己造了依赖"和"在队列里等"就分不开了（deps_total 都是 1）。 */
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
    if (!turn || turn->msg_count == 0)
        return NULL;
    cJSON *msg = edc_turn_msg_at(turn, 0);
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
    const char *ws = edc_test_workspace();
    char cmd[512];
    /* Create the prompts/ directory only: the workspace itself must survive, the store is
     * open on it. */
    snprintf(cmd, sizeof(cmd), "mkdir -p %s/prompts", ws);
    if (system(cmd) != 0)
        return 1;

    write_prompt(ws, "review",
                 "---\n"
                 "description: Review staged changes\n"
                 "argument-hint: \"[focus]\"\n"
                 "---\n"
                 "Review the staged changes. Focus on $1.\n");
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
        CHECK(input && strstr(input, "$1") == NULL, "and no placeholder survived");
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

    /* ── --new-group / --group <id>: where the template's task lands ── */

    TEST("/prompt --new-group opens a group of its own");
    {
        complete_all_tasks(sess);
        struct edc_task_group *prev_fg = sess->foreground_group;
        const int groups_before = count_groups(sess);
        drive_command(cfg, sm, sched, "/prompt --new-group review focus");

        CHECK(count_groups(sess) == groups_before + 1, "a group was opened");
        struct edc_task *t = last_member(sess->foreground_group);
        CHECK(t != NULL, "a task was created");
        CHECK(t && t->group && t->group != prev_fg, "it did not join the group that was in front");
        CHECK(t && t->group && t->group->leader == t, "it leads the new group");
        CHECK(edc_scheduler_get_foreground(sess) == (t ? t->group : NULL), "and the new group took the front");
    }

    TEST("--new-group also works AFTER the template name");
    {
        complete_all_tasks(sess);
        const int groups_before = count_groups(sess);
        drive_command(cfg, sm, sched, "/prompt review --new-group focus");

        CHECK(count_groups(sess) == groups_before + 1, "one more group");
        struct edc_task *t = last_member(sess->foreground_group);
        CHECK(t && t->group && t->group->leader == t, "the task leads it");
    }

    TEST("--group <id> puts the task in a group that is NOT in front");
    {
        complete_all_tasks(sess);
        /* Two fresh groups: the second takes the front, so the first can only be reached
         * by NAMING it -- aiming at the foreground group would pass even if --group were
         * ignored altogether. */
        drive_command(cfg, sm, sched, "/prompt --new-group review first");
        struct edc_task_group *older = sess->foreground_group; /* the group it just opened */
        drive_command(cfg, sm, sched, "/prompt --new-group review second");
        CHECK(older != NULL && sess->foreground_group != older, "the older group is not in front");

        char line[256];
        snprintf(line, sizeof(line), "/prompt review --group %s focus", older ? older->group_id : "none");
        const int groups_before = count_groups(sess);
        const int tasks_before = count_tasks(sess);
        drive_command(cfg, sm, sched, line);

        CHECK(count_tasks(sess) == tasks_before + 1, "exactly one task was created");
        CHECK(count_groups(sess) == groups_before, "and no new group was opened");
        CHECK(older && last_member(older) && last_member(older)->group == older, "it joined the named group");

        TEST("--group accepts a QUOTED id");
        snprintf(line, sizeof(line), "/prompt review --group \"%s\" focus", older ? older->group_id : "none");
        const int tasks_before_q = count_tasks(sess);
        drive_command(cfg, sm, sched, line);
        CHECK(count_tasks(sess) == tasks_before_q + 1, "the quotes are stripped, so the group is found");
        CHECK(older && last_member(older) && last_member(older)->group == older, "and the task joined it");
    }

    TEST("--group refuses a conversation whose leader is gone");
    {
        complete_all_tasks(sess);
        drive_command(cfg, sm, sched, "/prompt --new-group review doomed");
        struct edc_task_group *doomed = sess->foreground_group; /* the group it just opened */
        CHECK(doomed != NULL, "a group to orphan");
        if (doomed)
        {
            const int members_before = doomed->member_count;
            edc_task_group_mark_orphaned(doomed); /* what a departed leader leaves behind */

            char line[256];
            snprintf(line, sizeof(line), "/prompt review --group %s focus", doomed->group_id);
            const int tasks_before = count_tasks(sess);
            drive_command(cfg, sm, sched, line);

            CHECK(count_tasks(sess) == tasks_before, "nothing was created");
            CHECK(doomed->member_count == members_before, "the group did not grow");
            CHECK(doomed->leader == NULL, "and no leader was silently invented for it");
        }
    }

    TEST("--group with an unknown id builds nothing");
    {
        const int tasks_before = count_tasks(sess);
        drive_command(cfg, sm, sched, "/prompt review --group grp-nope focus");
        CHECK(count_tasks(sess) == tasks_before, "no task was created");
    }

    TEST("--new-group and --group together are refused, not silently resolved");
    {
        const int tasks_before = count_tasks(sess);
        drive_command(cfg, sm, sched, "/prompt --new-group --group grp-1 review focus");
        CHECK(count_tasks(sess) == tasks_before, "no task was created");
    }

    TEST("an option where --group's id belongs is an error");
    {
        const int tasks_before = count_tasks(sess);
        drive_command(cfg, sm, sched, "/prompt --group --new-group review focus");
        CHECK(count_tasks(sess) == tasks_before, "no task was created");
    }

    TEST("--group=<id> (the equals form) names the group too");
    {
        complete_all_tasks(sess);
        drive_command(cfg, sm, sched, "/prompt --new-group review eq-target");
        struct edc_task_group *target = sess->foreground_group;
        drive_command(cfg, sm, sched, "/prompt --new-group review eq-other");
        CHECK(target != NULL && sess->foreground_group != target, "the target is not the group in front");

        char line[256];
        snprintf(line, sizeof(line), "/prompt review --group=%s focus", target ? target->group_id : "none");
        const int tasks_before = count_tasks(sess);
        drive_command(cfg, sm, sched, line);
        CHECK(count_tasks(sess) == tasks_before + 1, "a task was created");
        CHECK(target && last_member(target) && last_member(target)->group == target, "and it joined the named group");
    }

    TEST("--group refuses the system group (it takes no new work)");
    {
        complete_all_tasks(sess);
        struct edc_task *sys = edc_scheduler_spawn_system_task(sched, sess, "fold probe");
        CHECK(sys != NULL && sys->group != NULL, "a system group exists");
        if (sys && sys->group)
        {
            char line[256];
            snprintf(line, sizeof(line), "/prompt review --group %s focus", sys->group->group_id);
            const int tasks_before = count_tasks(sess);
            drive_command(cfg, sm, sched, line);
            CHECK(count_tasks(sess) == tasks_before, "nothing was created");
            CHECK(sys->group->member_count == 1, "and the system group did not grow");
        }
    }

    TEST("a QUOTED \"--strict\" is an argument, not an option");
    {
        complete_all_tasks(sess);
        drive_command(cfg, sm, sched, "/prompt review \"--strict\"");
        struct edc_task *t = last_member(sess->foreground_group);
        const char *in = t ? first_user_content(t->active_turn) : NULL;
        CHECK(in && strstr(in, "--strict") != NULL, "the literal reached the template's $1");
    }

    TEST("--group into a BUSY group queues behind the task still running there");
    {
        complete_all_tasks(sess); /* clean slate: nothing unfinished anywhere */

        /* Group A gets a task that stays running (the spy never answers), then group B
         * takes the front, so A is a group in the background that is still busy. */
        drive_command(cfg, sm, sched, "/prompt --new-group review busy");
        struct edc_task_group *busy = sess->foreground_group;
        drive_command(cfg, sm, sched, "/prompt --new-group review other");
        CHECK(busy != NULL && sess->foreground_group != busy, "the busy group is not the group in front");

        char line[256];
        snprintf(line, sizeof(line), "/prompt review --group %s queued", busy ? busy->group_id : "none");
        const int tasks_before = count_tasks(sess);
        drive_command(cfg, sm, sched, line);

        CHECK(count_tasks(sess) == tasks_before + 1, "the task exists");
        struct edc_task *t = last_member(busy);
        CHECK(t != NULL && t->group == busy, "it joined the busy group");
        CHECK(t && t->deps_total >= 1, "and it waits for the task still running in that group");
        CHECK(t && edc_task_awaits_deps(t), "so it is queued, not running alongside");
    }

    TEST("--bg runs the template alongside a busy group instead of queueing");
    {
        complete_all_tasks(sess);
        drive_command(cfg, sm, sched, "/prompt --new-group review busy-bg");
        struct edc_task_group *busy = sess->foreground_group;
        drive_command(cfg, sm, sched, "/prompt --new-group review other-bg");
        struct edc_task_group *front = sess->foreground_group;

        char line[256];
        snprintf(line, sizeof(line), "/prompt review --group %s --bg alongside", busy ? busy->group_id : "none");
        drive_command(cfg, sm, sched, line);

        struct edc_task *t = last_member(busy);
        CHECK(t != NULL && t->group == busy, "it joined the busy group");
        CHECK(t && t->is_background, "as a background task");
        CHECK(t && t->deps_total == 0, "so it waits for nobody (no queueing)");
        CHECK(sess->foreground_group == front, "and the group in front did not move");
    }

    TEST("--new-group --bg opens a group without taking the front");
    {
        complete_all_tasks(sess);
        struct edc_task_group *front = sess->foreground_group;
        const int groups_before = count_groups(sess);
        drive_command(cfg, sm, sched, "/prompt --new-group --bg review lonely");

        CHECK(count_groups(sess) == groups_before + 1, "a group was opened");
        CHECK(sess->foreground_group == front, "but the front did not move");

        struct edc_task *bg = NULL, *m;
        struct edc_task_group *g;
        DL_FOREACH(sess->groups, g)
        {
            LL_FOREACH2(g->members, m, group_next)
            {
                if (m->is_background)
                    bg = m;
            }
        }
        CHECK(bg != NULL, "the task is background");
    }

    TEST("an unknown --option is an error, never part of the prompt");
    {
        const int tasks_before = count_tasks(sess);
        drive_command(cfg, sm, sched, "/prompt review --strict");
        CHECK(count_tasks(sess) == tasks_before, "no task was created");
    }

    TEST("a single dash is still a template argument");
    {
        complete_all_tasks(sess);
        drive_command(cfg, sm, sched, "/prompt review -strict");
        struct edc_task *t = last_member(sess->foreground_group);
        const char *in = t ? first_user_content(t->active_turn) : NULL;
        CHECK(in && strstr(in, "-strict") != NULL, "`-strict` was substituted into the body");
    }

    /* store still open: nothing to remove */

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

        TEST("/skill --new-group opens a group of its own");
        complete_all_tasks(sess);
        const int skill_groups_before = count_groups(sess);
        drive_command(cfg, sm, sched, "/skill --new-group pdf page 4");
        CHECK(count_groups(sess) == skill_groups_before + 1, "a group was opened");
        struct edc_task *sk = last_member(sess->foreground_group);
        CHECK(sk && sk->group && sk->group->leader == sk, "the skill task leads it");

        TEST("/skill --bg ... is background too");
        complete_all_tasks(sess);
        drive_command(cfg, sm, sched, "/skill pdf --bg page 5");
        struct edc_task *skbg = last_member(sess->foreground_group);
        CHECK(skbg && skbg->is_background, "background");
        CHECK(skbg && skbg->deps_total == 0, "and not queued behind anything");
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
