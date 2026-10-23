/* Group / foreground semantics of one shell command line.
 *
 * Pins what `&`, `--bg` and the `+` prefix mean for the *group* dimension:
 *   - a plain command opens a group (when the session has none) and takes the
 *     foreground;
 *   - `cmd &` joins the **current** group and stays off the foreground -- it is not
 *     a "+" command, it does not open a group of its own;
 *   - `+cmd "..."` opens a **new** group (and takes the foreground unless background);
 *   - and a task that declared itself background must not touch the foreground concept
 *     at all -- not even when the session has no foreground group, which is the case
 *     that used to promote it (the AST used to set the foreground pointer there).
 *
 * Plus the syntax rule: a `+` command that carries options must quote its
 * description, otherwise nothing is built at all; and a leading `@` (the sigil `+`
 * replaced) is rejected outright rather than silently reread as a current-group task.
 *
 * And the serialization rule: joining the group that is in front means queueing behind
 * whatever is still running there (one in front at a time), while a background task in
 * the same group runs alongside; the active pointer -- what the frontend shows live --
 * follows whoever is running.
 *
 * harness copied from test_commands_task_new: drive the public edc_commands_handler
 * entry, then call edc_scheduler_schedule ourselves (the message end is the tick).
 * The spy hands us the task that was actually dispatched, so nothing is matched by
 * text. No channel actor ⇒ replies go nowhere.
 */
#include "edc_channel_actor.h"
#include "edc_commands.h"
#include "edc_compat.h"
#include "edc_config.h"
#include "edc_scheduler.h"
#include "edc_session.h"
#include "edc_session_manager.h"
#include "edc_task.h"
#include "edc_task_group.h"
#include "utlist_fix.h"
#include <cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* The first user message of a turn -- what the model actually receives. */
static const char *first_user_content(struct edc_turn *turn)
{
    if (!turn || turn->msg_count == 0)
        return NULL;
    cJSON *msg = edc_turn_msg_at(turn, 0);
    return cJSON_GetStringValue(cJSON_GetObjectItem(msg, "content"));
}

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

/* Returns the task this command line dispatched (each drive creates exactly one). */
static struct edc_task *drive_command(struct edc_config *cfg, struct edc_session_manager *sm,
                                      struct edc_scheduler *sched, const char *content)
{
    struct edc_channel_message *cm =
        edc_channel_message_new(edc_channel_adapter, EDC_CHANNEL_MESSAGE_USER_TYPE, sdsnew(content));
    edc_commands_handler(cfg, sm, sched, (struct edc_reply_to){.type = cm->type}, cm);
    g_llm_calls = 0;
    g_llm_task = NULL;
    /* The command layer only attaches the input; running it is the message-end scan. */
    edc_scheduler_schedule(sched);
    return g_llm_task;
}

/* Number of groups the session holds -- to assert that a rejected command line built
 * nothing at all (not even the group it would have opened). */
static int group_count(struct edc_session *sess)
{
    int n = 0;
    struct edc_task_group *g;
    DL_FOREACH(sess->groups, g)
    n++;
    return n;
}

/* Total tasks the session holds, however they queue. A rejected line must not add one --
 * checking only the dispatcher would confuse "rejected" with "waiting behind a peer". */
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

/* A task that is waiting keeps out of send_llm, so lookup by description is the only
 * way to reach it. */
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

int main(void)
{
    const char *ws = "/tmp/test_shell_job_control_ws";
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "rm -rf %s && mkdir -p %s", ws, ws);
    if (system(cmd) != 0)
        return 1;

    struct edc_config *cfg = calloc(1, sizeof(*cfg));
    cfg->workspace = sdsnew(ws);
    /* This file is ABOUT the shell-like syntax, so it declares the switch on. The default
     * is off; what off does is pinned in test_shell_switch.c. */
    cfg->shell_syntax = true;
    struct edc_session_manager *sm = edc_session_manager_new(cfg);
    struct edc_session *sess = edc_session_manager_get_active(sm);
    CHECK(sess != NULL, "manager has an active session");
    if (!sess)
        return 1;

    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, sess);
    sched->send_llm = mock_send_llm;

    /* ── 普通命令: 开组 + 占前台 ─────────────────────────────────── */

    TEST("a plain command opens a group and takes the foreground");
    struct edc_task *seed = drive_command(cfg, sm, sched, "\"seed\"");
    CHECK(seed != NULL, "the task ran");
    struct edc_task_group *g0 = seed ? seed->group : NULL;
    CHECK(g0 != NULL && g0 == edc_scheduler_get_foreground(sess), "its group is the foreground");
    CHECK(edc_session_get_active_task(sess) == seed, "and it is the active task");

    /* ── cmd & : 进当前组, 不占前台 ─────────────────────────────── */

    TEST("`cmd &` joins the current group and leaves the foreground alone");
    struct edc_task *bg = drive_command(cfg, sm, sched, "\"helper\" &");
    CHECK(bg != NULL, "the background task ran anyway (bg is not 'do not run')");
    CHECK(bg && bg->group == g0, "it joined the group already in front");
    CHECK(g0 && g0->member_count == 2, "member count grew");
    CHECK(edc_scheduler_get_foreground(sess) == g0, "the foreground did not move");
    CHECK(edc_session_get_active_task(sess) == seed, "nor did the active task");

    /* ── 没有前台组时的 cmd & ───────────────────────────────────── */

    TEST("`cmd &` with no foreground group does not make one");
    /* 与 `/task bg`(无参数) 同一效果: 清掉前台指针。 */
    edc_scheduler_set_foreground(sess, NULL);
    edc_session_set_active_task(sess, NULL);
    struct edc_task *lonely = drive_command(cfg, sm, sched, "\"lonely\" &");
    CHECK(lonely != NULL, "the task ran");
    CHECK(lonely && lonely->group && lonely->group != g0, "nowhere to join -> a group of its own");
    CHECK(edc_scheduler_get_foreground(sess) == NULL, "still no foreground group");
    CHECK(edc_session_get_active_task(sess) == NULL, "still no active task");

    /* ── + + 后台 + 没有前台组: 不得被推上前台 ───────────────────── */

    TEST("`+cmd \"...\" &` with no foreground group must not be promoted");
    struct edc_task *at = drive_command(cfg, sm, sched, "+\"new group work\" &");
    CHECK(at != NULL, "the task ran");
    CHECK(at && at->group && lonely && at->group != lonely->group, "the + opened its own group");
    CHECK(edc_scheduler_get_foreground(sess) == NULL, "a background task did not touch the foreground");
    CHECK(edc_session_get_active_task(sess) == NULL, "nor the active task");

    /* 新规则: shell 行里没有 `--bg` —— 后台只有一种拼法: 链尾 `&`(拍板 ⑩)。
     * `--bg` 保留给命令层(`/task new "…" --bg`)。 */
    TEST("`+\"...\" --bg` is refused in shell syntax; `&` is how a shell line says it");
    const int tasks_before_bg = task_count(sess);
    const int groups_before_bg = group_count(sess);
    struct edc_task *at_bg = drive_command(cfg, sm, sched, "+\"quoted bg\" --bg");
    CHECK(at_bg == NULL, "nothing was dispatched -- `--bg` is not our shell syntax");
    CHECK(task_count(sess) == tasks_before_bg, "and no task was created");
    CHECK(group_count(sess) == groups_before_bg, "nor a group opened");

    struct edc_task *at_amp = drive_command(cfg, sm, sched, "+\"quoted bg\" &");
    CHECK(at_amp != NULL, "the `&` form runs");
    CHECK(at_amp && at_amp->is_background, "and it is background");
    CHECK(at && at_amp && at_amp->group && at->group && at_amp->group != at->group, "its own group again");
    CHECK(edc_scheduler_get_foreground(sess) == NULL, "still no foreground group");
    CHECK(edc_session_get_active_task(sess) == NULL, "still no active task");

    /* `<` 是"把文件读进这条任务"，这件事归 `@file`（那个 pass 里读，过沙箱与 workdir）。
     * 不能静默变成别的意思：解析阶段就拒掉，并指向替代写法。 */
    TEST("input redirect `<` is refused (it is `@file`'s job now)");
    const int tasks_before_in = task_count(sess);
    const int groups_before_in = group_count(sess);
    struct edc_task *at_in = drive_command(cfg, sm, sched, "+\"fix bugs\" < \"context.txt\"");
    CHECK(at_in == NULL, "nothing was dispatched");
    CHECK(task_count(sess) == tasks_before_in, "and no task was created");
    CHECK(group_count(sess) == groups_before_in, "nor a group opened");

    /* ── desc 没带引号却带选项: 语法错误, 什么都不建 ────────────────── */

    TEST("an unquoted word after `+` is a MESSAGE (拍板①), not a group");
    int groups_before = group_count(sess);
    const int tasks_before_plusword = task_count(sess);
    drive_command(cfg, sm, sched, "+grp \"new group\"");
    CHECK(task_count(sess) == tasks_before_plusword + 1, "the line still became one task");
    CHECK(find_task_by_desc(sess, "+grp \"new group\"") != NULL,
          "as text, with the `+` kept -- it is not a sigil when a quote does not follow");
    /* (没有前台组时，一条普通任务本来就会自己开一个组 -- 那不是这里要钉的事。) */

    /* ── + 不带 --bg: 这才是占前台的那条路 ──────────────────────── */

    TEST("`+cmd \"...\"` without --bg is the one that takes the foreground");
    struct edc_task *at_fg = drive_command(cfg, sm, sched, "+\"grp plain work\"");
    CHECK(at_fg != NULL, "the task ran");
    CHECK(at_fg && at_fg->group && at_fg->group == edc_scheduler_get_foreground(sess),
          "its new group became the foreground");
    CHECK(edc_session_get_active_task(sess) == at_fg, "and it became the active task");

    /* ── 前台串行: 同组的下一条排在前一条后面 ──────────────────────── */

    TEST("a second foreground task in the same group queues behind the first");
    /* 此刻前台组就是 at_fg 的组, 而 at_fg 正在跑。 */
    struct edc_task *queued = drive_command(cfg, sm, sched, "\"queued work\"");
    CHECK(queued == NULL, "nothing was dispatched -- it waits for the one in front");
    struct edc_task *waiting = find_task_by_desc(sess, "queued work");
    CHECK(waiting != NULL, "but the task was created");
    CHECK(waiting && edc_task_awaits_deps(waiting), "it is waiting on a dependency");
    CHECK(waiting && waiting->deps_total == 1, "one order edge");
    /* 组内顺序 = BARRIER（源死了放行），不是 SEQUENCE（源死了级联）。 */
    CHECK(waiting && waiting->deps && waiting->deps->type == TASK_DEP_BARRIER, "and it is a BARRIER");
    CHECK(at_fg && waiting && waiting->group == at_fg->group, "and it joined the same group");
    CHECK(at_fg && edc_scheduler_get_foreground(sess) == at_fg->group, "the group in front did not move");
    CHECK(edc_session_get_active_task(sess) == at_fg, "nor did the front land on a waiting task");

    TEST("a background task in that same group is not chained");
    struct edc_task *aside = drive_command(cfg, sm, sched, "\"aside\" &");
    CHECK(aside != NULL && aside->group == at_fg->group, "it joined the same group");
    CHECK(aside && aside->deps_total == 0, "but it waits for nobody");
    CHECK(g_llm_task == aside, "so it ran alongside the one in front");
    CHECK(edc_session_get_active_task(sess) == at_fg, "and it did not take the front");

    TEST("when the one in front finishes, the queued task runs and takes the front");
    edc_task_mark_completed(at_fg);
    edc_task_notify_dependents(at_fg); /* what the scheduler does when a task completes */
    g_llm_calls = 0;
    g_llm_task = NULL;
    edc_scheduler_schedule(sched);
    CHECK(g_llm_task == waiting, "the queued task was released and driven");
    CHECK(waiting && waiting->current_request_id != NULL, "it is running now");
    CHECK(edc_session_get_active_task(sess) == waiting, "and the front followed whoever is running");

    /* ── 无 id 的 /task stop: 打"场上的那个", 不是屏幕焦点 ───────── */

    TEST("`/task stop` with no id targets the one in front, not the screen focus");
    CHECK(edc_scheduler_current_task(sess) == aside, "the derivation picks the last unfinished member");
    CHECK(edc_session_get_active_task(sess) == waiting, "while the focus sits on another task");
    drive_command(cfg, sm, sched, "/task stop");
    CHECK(aside && edc_task_is_cancelled(aside), "the task in front was stopped");
    CHECK(waiting && !edc_task_is_cancelled(waiting), "the focused task was left alone");

    /* ── 后台任务不上屏: 就算场上没人跑, 也不给它焦点 ─────────────── */

    TEST("a driven background task does not take the front, even when nothing runs");
    edc_task_mark_completed(waiting); /* 焦点停在一个已结束的任务上, 场上没人跑 */
    g_llm_calls = 0;
    g_llm_task = NULL;
    struct edc_task *bg2 = drive_command(cfg, sm, sched, "\"bgwork\" &");
    CHECK(bg2 != NULL, "后台任务跑起来了");
    CHECK(bg2 && bg2->is_background, "它被记成后台任务");
    CHECK(bg2 && bg2->group == at_fg->group, "还是进当前前台组");
    CHECK(edc_session_get_active_task(sess) == waiting, "焦点没有跑到它身上 (后台不上屏)");

    /* ── 旧 `@` 前缀: 拒绝, 什么都不建 ──────────────────────────── */

    TEST("a leading `@` is a MESSAGE now (Ⓕ) -- `@` is the file-reference sigil, not a group one");
    const int tasks_before_at = task_count(sess);
    drive_command(cfg, sm, sched, "@grpwork probe");
    CHECK(task_count(sess) == tasks_before_at + 1, "the line became one task");
    CHECK(find_task_by_desc(sess, "@grpwork probe") != NULL,
          "carrying the text verbatim (the migration hint waits for the message-body pass)");

    /* ── 转义: 描述可以以字面 `+` / `@` 开头, 落在当前组 ────────── */

    TEST("a quoted string starting with `+` is ordinary text (current group)");
    /* The `\+` escape is gone -- quoting is how you say "literal": the sigil is only read
     * when the `+` is the first character OUTSIDE a quoted string. */
    const int groups_before_plus = group_count(sess);
    const int tasks_before_plus = task_count(sess);
    drive_command(cfg, sm, sched, "\"+1 issue is still open\"");
    struct edc_task *plus = find_task_by_desc(sess, "+1 issue is still open");
    CHECK(plus != NULL, "a task carried the literal `+` description");
    CHECK(task_count(sess) == tasks_before_plus + 1, "exactly one task was created");
    CHECK(plus && plus->group == edc_scheduler_get_foreground(sess), "it joined the current group");
    CHECK(group_count(sess) == groups_before_plus, "and did not open a group of its own");

    TEST("...and a quoted string starting with `@` is ordinary text too");
    const int groups_before_at_lit = group_count(sess);
    drive_command(cfg, sm, sched, "\"@alice please take a look\"");
    struct edc_task *at_lit = find_task_by_desc(sess, "@alice please take a look");
    CHECK(at_lit != NULL, "a task carried the literal `@` description (the rejection did not fire)");
    CHECK(at_lit && at_lit->group == edc_scheduler_get_foreground(sess), "it joined the current group");
    CHECK(group_count(sess) == groups_before_at_lit, "and did not open a group of its own");

    /* ── 判别: 引号开头 = shell 行; 否则是普通输入 ─────────────────── */

    TEST("an unquoted line is a normal message -- the `|` in it is text");
    /* `现在新建任务A|B` needs neither quotes nor an error: unquoted, it is a message, i.e.
     * ONE task whose text contains the `|`. The pipe has to be asked for explicitly. */
    const int before_ambiguous = task_count(sess);
    drive_command(cfg, sm, sched, "alpha|beta");
    CHECK(task_count(sess) == before_ambiguous + 1, "one task");
    CHECK(find_task_by_desc(sess, "alpha|beta") != NULL, "carrying the whole text, `|` and all");
    CHECK(find_task_by_desc(sess, "alpha") == NULL, "and no first half was split off");

    TEST("...and a plain sentence is a message too, with no quotes anywhere");
    const int before_plain = task_count(sess);
    drive_command(cfg, sm, sched, "analyze src/ for bugs");
    CHECK(task_count(sess) == before_plain + 1, "one task");
    CHECK(find_task_by_desc(sess, "analyze src/ for bugs") != NULL, "verbatim, as written");

    TEST("a quoted first character makes the same line SHELL syntax: a pipe");
    const int before_pipe = task_count(sess);
    drive_command(cfg, sm, sched, "\"alpha\" | \"beta\"");
    CHECK(task_count(sess) == before_pipe + 2, "two tasks -- the quoted operands are a chain");
    CHECK(find_task_by_desc(sess, "alpha") != NULL, "the left operand is the first task's text");
    CHECK(find_task_by_desc(sess, "beta") != NULL, "and the right one the second's");

    TEST("...and quoting the `|` makes it text");
    const int before_literal = task_count(sess);
    drive_command(cfg, sm, sched, "\"alpha|beta\"");
    CHECK(task_count(sess) == before_literal + 1, "one task");
    CHECK(find_task_by_desc(sess, "alpha|beta") != NULL, "whose text contains the `|`");

    TEST("inside a SHELL line every string is still quoted (so a mixed one is refused)");
    const int before_mixed = task_count(sess);
    drive_command(cfg, sm, sched, "\"alpha\" | beta");
    CHECK(task_count(sess) == before_mixed, "the unquoted operand made it ambiguous -- nothing was created");

    TEST("`if x > 5` is prose (a message), not a redirect to a file called 5");
    const int before_gt = task_count(sess);
    drive_command(cfg, sm, sched, "if x > 5");
    CHECK(task_count(sess) == before_gt + 1, "one task");
    CHECK(find_task_by_desc(sess, "if x > 5") != NULL, "with the text as written, nothing redirected");

    TEST("the redirect TARGET is a string too -- quoted, and unquoted on the way out");
    const int before_redir = task_count(sess);
    drive_command(cfg, sm, sched, "\"say hi\" > \"out.md\"");
    struct edc_task *redir = find_task_by_desc(sess, "say hi");
    CHECK(redir != NULL, "the quoted description became the task's text");
    CHECK(redir && redir->redirect_stdout && strcmp(redir->redirect_stdout, "out.md") == 0,
          "and the quoted filename reached it unquoted (the quotes are delimiters, not part of the path)");

    /* ── 选项区只接选项: 其它 token 报错, 不再静默丢弃 ─────────────── */

    TEST("text after a quoted string is not an option -- refused, not dropped");
    /* `"hello" and some more` used to keep `hello` and lose the rest with no word. */
    const int before_tail = task_count(sess);
    drive_command(cfg, sm, sched, "\"hello\" and then some more");
    CHECK(task_count(sess) == before_tail, "nothing was created");
    CHECK(find_task_by_desc(sess, "hello") == NULL, "and no truncated task either");

    TEST("...but a real shell option is still an option");
    const int before_opt = task_count(sess);
    {
        char optline[128];
        snprintf(optline, sizeof(optline), "\"opt work\" -d %s", at_fg ? at_fg->task_id : "1");
        drive_command(cfg, sm, sched, optline);
    }
    struct edc_task *opt = find_task_by_desc(sess, "opt work");
    CHECK(opt != NULL, "the task was created");
    CHECK(opt && opt->deps_total >= 1, "and `-d` made it wait for that task");
    CHECK(task_count(sess) == before_opt + 1, "exactly one task");

    TEST("...and the `2` of a stderr redirect is a redirect prefix, not a stray token");
    const int before_err = task_count(sess);
    drive_command(cfg, sm, sched, "\"redirect work\" 2> \"err.log\"");
    struct edc_task *err = find_task_by_desc(sess, "redirect work");
    CHECK(err != NULL, "the task was created");
    CHECK(err && err->redirect_stderr && strcmp(err->redirect_stderr, "err.log") == 0,
          "with the stderr redirect intact");

    /* ── 指南里的例子: 至少把没被别处覆盖的几条钉住 ────────────────── */

    TEST("guide: `\"a\" ; \"b\"` is two tasks, sequential");
    const int before_seq = task_count(sess);
    drive_command(cfg, sm, sched, "\"first step\" ; \"second step\"");
    CHECK(task_count(sess) == before_seq + 2, "two tasks");
    struct edc_task *first_step = find_task_by_desc(sess, "first step");
    CHECK(first_step != NULL, "the first");
    struct edc_task *second = find_task_by_desc(sess, "second step");
    CHECK(second != NULL, "and the second");
    CHECK(second && second->deps_total >= 1, "which waits for it");
    {
        /* `;` 是顺序, 不是 `&&`: 边必须是 BARRIER(源怎么结束都放行), 否则前一条失败会把
         * 后一条一起取消 —— 那不是 shell 的 `;`。 */
        struct edc_task_dep *seq_edge = NULL;
        for (struct edc_task_dep *d = second ? second->deps : NULL; d; d = d->next)
            if (first_step && d->source == first_step)
            {
                seq_edge = d;
                break;
            }
        CHECK(seq_edge != NULL, "one edge to the first task");
        CHECK(seq_edge && seq_edge->type == TASK_DEP_BARRIER, "and it is an ORDER edge (`;`), not `&&`");
    }

    TEST("`a ; b`: the predecessor failing runs b anyway (shell's `;`, not `&&`)");
    g_llm_calls = 0;
    g_llm_task = NULL;
    drive_command(cfg, sm, sched, "+\"doomed\" ; \"runs anyway\"");
    struct edc_task *doomed = find_task_by_desc(sess, "doomed");
    struct edc_task *runs = find_task_by_desc(sess, "runs anyway");
    CHECK(doomed && runs && doomed->group == runs->group, "both landed in the fresh group");
    CHECK(runs && runs->deps && runs->deps->type == TASK_DEP_BARRIER, "b waits on an ORDER edge");
    if (doomed)
        edc_task_mark_failed(doomed);
    CHECK(runs && !edc_task_has_failed(runs), "b was NOT taken down with a");
    CHECK(runs && edc_task_is_ready(runs), "so b is free to run");
    g_llm_calls = 0;
    g_llm_task = NULL;
    edc_scheduler_schedule(sched);
    CHECK(g_llm_task == runs, "and the scan drove it");

    TEST("`a && b`: the edge is a SEQUENCE -- b runs only if a succeeded");
    g_llm_calls = 0;
    g_llm_task = NULL;
    drive_command(cfg, sm, sched, "+\"gate\" && \"gated\"");
    struct edc_task *gate = find_task_by_desc(sess, "gate");
    struct edc_task *gated = find_task_by_desc(sess, "gated");
    CHECK(gate && gated && gate->group == gated->group, "both landed in the fresh group");
    CHECK(gated && gated->deps_total == 1, "one edge");
    CHECK(gated && gated->deps && gated->deps->type == TASK_DEP_SEQUENCE, "and it is the STRICT kind (`&&`)");

    TEST("`a && b`: a failing CANCELS b, and the edge is kept");
    if (gate)
        edc_task_mark_failed(gate);
    CHECK(gated && edc_task_has_failed(gated), "b was taken down with a");
    CHECK(gated && gated->deps_total == 1 && gated->deps && !gated->deps->cancelled,
          "the edge is KEPT: resuming b puts it back behind a");
    CHECK(gated && edc_task_is_resumable(gated), "and b stays resumable");

    TEST("`a || b`: the edge is an UNLESS -- b runs only if a did NOT succeed");
    g_llm_calls = 0;
    g_llm_task = NULL;
    drive_command(cfg, sm, sched, "+\"try\" || \"fallback\"");
    struct edc_task *try_t = find_task_by_desc(sess, "try");
    struct edc_task *fallback = find_task_by_desc(sess, "fallback");
    CHECK(try_t && fallback && try_t->group == fallback->group, "both landed in the fresh group");
    CHECK(fallback && fallback->deps_total == 1, "one edge");
    CHECK(fallback && fallback->deps && fallback->deps->type == TASK_DEP_UNLESS, "and it is UNLESS (`||`)");

    TEST("`a || b`: a SUCCEEDING skips b (cancelled, not run)");
    if (try_t)
    {
        edc_task_await_exec(try_t);
        edc_task_mark_completed(try_t);
        edc_task_notify_dependents(try_t);
    }
    CHECK(fallback && !edc_task_is_ready(fallback), "b is not runnable");
    CHECK(fallback && edc_task_is_cancelled(fallback), "b was skipped (cancelled)");
    CHECK(fallback && !edc_task_is_resumable(fallback),
          "a skipped branch is NOT resumable: `||` decided it does not run");
    g_llm_calls = 0;
    g_llm_task = NULL;
    edc_scheduler_schedule(sched);
    CHECK(g_llm_task != fallback, "and the scan did not drive it");

    TEST("`a || b`: a FAILING runs b (the whole point of `||`)");
    g_llm_calls = 0;
    g_llm_task = NULL;
    drive_command(cfg, sm, sched, "+\"boom\" || \"rescue\"");
    struct edc_task *boom = find_task_by_desc(sess, "boom");
    struct edc_task *rescue = find_task_by_desc(sess, "rescue");
    CHECK(boom && rescue && rescue->deps && rescue->deps->type == TASK_DEP_UNLESS, "the UNLESS edge is there");
    if (boom)
    {
        edc_task_await_exec(boom);
        edc_task_mark_failed(boom);
        edc_task_notify_dependents(boom);
    }
    CHECK(rescue && edc_task_is_ready(rescue), "b was released by a's failure");
    g_llm_calls = 0;
    g_llm_task = NULL;
    edc_scheduler_schedule(sched);
    CHECK(g_llm_task == rescue, "and the scan drove it");

    TEST("`a && b`: a succeeding releases b");
    g_llm_calls = 0;
    g_llm_task = NULL;
    drive_command(cfg, sm, sched, "+\"ok\" && \"then\"");
    struct edc_task *ok = find_task_by_desc(sess, "ok");
    struct edc_task *then = find_task_by_desc(sess, "then");
    CHECK(ok && then && then->deps && then->deps->type == TASK_DEP_SEQUENCE, "the strict edge is there");
    if (ok)
    {
        edc_task_await_exec(ok);
        edc_task_mark_completed(ok);
        edc_task_notify_dependents(ok); /* what the scheduler does when a task completes */
    }
    CHECK(then && edc_task_is_ready(then), "b was released by a's success");
    g_llm_calls = 0;
    g_llm_task = NULL;
    edc_scheduler_schedule(sched);
    CHECK(g_llm_task == then, "and the scan drove it");

    TEST("guide: `\"task\" < \"path\"` is now REFUSED -- `<` is `@file`'s job");
    {
        /* Input redirect is gone: the parser refuses it (and says to use @file), so no
         * task is built. This pins the failure closed -- a line that used to append a
         * file must not silently become something else. */
        char path[256];
        snprintf(path, sizeof(path), "%s/context.txt", ws);
        FILE *f = fopen(path, "w");
        if (f)
        {
            fputs("FILE-CONTENT-MARKER\n", f);
            fclose(f);
        }
        char line[512];
        snprintf(line, sizeof(line), "\"read the context\" < \"%s\"", path);
        const int before_in = task_count(sess);
        drive_command(cfg, sm, sched, line);
        CHECK(find_task_by_desc(sess, "read the context") == NULL, "no task was created");
        CHECK(task_count(sess) == before_in, "and the count did not move");
    }

    TEST("`2>>` is stderr + append; `2>` is stderr alone (the old `2>>` bug)");
    {
        drive_command(cfg, sm, sched, "\"stderr app\" 2>> \"e.log\"");
        struct edc_task *t = find_task_by_desc(sess, "stderr app");
        CHECK(t != NULL, "the task was created");
        CHECK(t && t->redirect_stderr && strcmp(t->redirect_stderr, "e.log") == 0, "pointed at e.log");
        CHECK(t && t->redirect_stderr_append, "and APPENDING (2>>)");

        drive_command(cfg, sm, sched, "\"stderr trunc\" 2> \"t.log\"");
        struct edc_task *u = find_task_by_desc(sess, "stderr trunc");
        CHECK(u && u->redirect_stderr && strcmp(u->redirect_stderr, "t.log") == 0, "`2>` also points");
        CHECK(u && !u->redirect_stderr_append, "but truncates");
    }

    TEST("a malformed chain that STARTS quoted creates nothing at all (no partial run)");
    {
        /* 只列"以引号开头"的：以 `|`/`;`/`&`/`>` 开头的是消息（§2 的提交集里没有它们），
         * 那就和今天一样是一条任务，不是收紧。 */
        const char *bad[] = {"\"a\" |", "\"a\" ;", "\"a\" 2>", "+\"a\" & &"};
        for (int i = 0; i < 4; i++)
        {
            const int b4 = task_count(sess);
            drive_command(cfg, sm, sched, bad[i]);
            CHECK(task_count(sess) == b4, bad[i]);
        }
    }

    TEST("`{{…}}` in a shell stage collects a dependency (interpreter, in stage order)");
    {
        const int b4v = task_count(sess);
        drive_command(cfg, sm, sched, "\"ref {{1}}\"");
        struct edc_task *t = find_task_by_desc(sess, "ref {{1}}");
        CHECK(t != NULL, "the task was created");
        CHECK(t && t->deps_total >= 1, "with a dependency taken from the reference");
        CHECK(t && t->has_deferred_vars, "and registered for the deferred expansion");
        CHECK(task_count(sess) == b4v + 1, "exactly one task");
    }

    TEST("an unterminated quote at the START of a shell line is an error, not a message");
    {
        const int b4q = task_count(sess);
        drive_command(cfg, sm, sched, "+\"ABC");
        CHECK(task_count(sess) == b4q, "`+\"ABC` created nothing (it committed to a shell line)");
        drive_command(cfg, sm, sched, "\"ABC");
        CHECK(task_count(sess) == b4q, "`\"ABC` created nothing either");
        drive_command(cfg, sm, sched, "'ABC");
        CHECK(task_count(sess) == b4q, "and neither did the single-quoted form");
        /* 句中才出现的未闭合引号：行首已经提交到 shell 行，所以同样是错误。 */
        drive_command(cfg, sm, sched, "\"a\" \"b");
        CHECK(task_count(sess) == b4q, "nor a shell line whose second literal never closes");
    }

    TEST("...but an apostrophe inside a plain message is just text");
    {
        const int b4a = task_count(sess);
        drive_command(cfg, sm, sched, "don't panic");
        CHECK(task_count(sess) == b4a + 1, "one task");
        CHECK(find_task_by_desc(sess, "don't panic") != NULL, "with the apostrophe kept");
    }

    printf("\n  %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
