#include "edc_commands.h"
#include "edc_crontab_jobs.h"
#include "edc_datetime_utils.h"
#include "edc_interp.h"
#include "edc_lex.h"
#include "edc_actor.h"
#include "edc_channel_actor.h"
#include "edc_cmd_parse.h"
#include "edc_compat.h"
#include "edc_context.h"
#include "edc_models.h"
#include "edc_parse.h"
#include "edc_file_utils.h"
#include "edc_path_guard.h"
#include "edc_fold_files.h"
#include "edc_group_fold.h"
#include "edc_local_shell.h"
#include "edc_prompts.h"
#include "edc_resources.h"
#include "edc_skills.h"
#include "edc_string_utils.h"
#include "edc_scheduler.h"
#include "edc_session.h"
#include "edc_session_manager.h"
#include "edc_session_memory.h"
#include "edc_session_store.h"
#include "edc_state_store.h"
#include "edc_shell.h"
#include "edc_state_serializer.h"
#include "edc_task.h"
#include "edc_task_group.h"
#include "edc_turn.h"
#include "sds.h"
#include "utlist_fix.h"
#include <cJSON.h>
#include <log.h>
#include <string.h>

static bool edc_commands_is_target(const char *content, const char *target)
{
    sds cmd = sdsnew(content);
    cmd = sdstrim(cmd, " \t\r\n");
    bool is = strcmp(cmd, target) == 0;
    sdsfree(cmd);
    return is;
}

static bool edc_commands_is_prefix(const char *content, const char *prefix)
{
    sds cmd = sdsnew(content);
    cmd = sdstrim(cmd, " \t\r\n");
    bool is = strncmp(cmd, prefix, strlen(prefix)) == 0;
    sdsfree(cmd);
    return is;
}

/* Strict command prefix: content equals the prefix, or starts with prefix + space.
 * A loose prefix would swallow "/tasks" into "/task". */
static bool edc_commands_is_command(const char *content, const char *prefix)
{
    sds cmd = sdstrim(sdsnew(content), " \t\r\n");
    bool is = strcmp(cmd, prefix) == 0;
    if (!is)
    {
        size_t plen = strlen(prefix);
        is = strncmp(cmd, prefix, plen) == 0 && cmd[plen] == ' ';
    }
    sdsfree(cmd);
    return is;
}


void edc_commands_reply(struct edc_reply_to to, const char *text)
{
    struct edc_channel_message *reply = edc_channel_message_new(to.type, EDC_CHANNEL_MESSAGE_SYS_TYPE, sdsnew(text));
    edc_actor_send(to.channel_actor,
                   edc_actor_msg_new_sub_type(BC_ACTOR_MSG_CHANNEL_SEND, reply->type, to.actor, NULL, reply,
                                              (edc_actor_data_free_fn)edc_channel_message_free));
}

void edc_commands_send_history(struct edc_reply_to to, const char *payload)
{
    struct edc_channel_message *reply =
        edc_channel_message_new(to.type, EDC_CHANNEL_MESSAGE_TASK_HISTORY_TYPE, sdsnew(payload));
    edc_actor_send(to.channel_actor,
                   edc_actor_msg_new_sub_type(BC_ACTOR_MSG_CHANNEL_SEND, reply->type, to.actor, NULL, reply,
                                              (edc_actor_data_free_fn)edc_channel_message_free));
}



/* ── Task-creation receipt ─────────────────────── */

/* The one line a user sees after a task is born: which task, which group, and -- only when
 * it is worth saying -- when it runs. Everything it needs lives on the task itself (the
 * group it landed in and its own dependency edges), so no scheduler state has to be
 * threaded through; that is what makes it a pure function of the task, testable without an
 * actor.
 *
 * It speaks ONLY about what the UI cannot already show: that the task runs in the
 * background, or what it waits for. A plain foreground creation is silent -- the new
 * sidebar row and the echoed input already say it, and a receipt per task is just noise
 * (in a `a | b | c` chain, three lines of it).
 *
 * Callers: the three user-facing creation paths -- plain input / `@` / pipes in
 * edc_shell_ast.c (which aggregates the whole input into ONE receipt), `/task new` here,
 * and `!cmd` in edc_local_shell.c. System tasks (fold / curation / extraction) are created
 * on another path and are never announced. */
sds edc_commands_task_receipt_text(const struct edc_task *task)
{
    if (!task || !task->task_id)
        return sdsempty();
    /* System work is not something the user "派"ed; stay silent about it. */
    if (task->group && edc_task_group_is_system(task->group))
        return sdsempty();

    /* A background task runs alongside: "background" is the whole story. */
    const char *gid = (task->group && task->group->group_id) ? task->group->group_id : "?";
    const char *fresh = (task->group && task->group->member_count == 1) ? " (new)" : "";
    if (task->is_background)
        return sdscatprintf(sdsempty(), "→ task #%s · group #%s%s · background", task->task_id, gid, fresh);

    /* JOIN deps inject the source's output; every other kind only orders (the `|` / `;` / `&&` /
 * `||` predecessor, the wait-behind-the-front edge, or a maintenance barrier). A just-created
 * task has at most these, never a mix worth more than a short list. */
    sds joins = sdsempty();
    const struct edc_task_dep *seq = NULL;
    for (const struct edc_task_dep *d = task->deps; d; d = d->next)
    {
        if (!d->source || !d->source->task_id)
            continue;
        if (d->type == TASK_DEP_JOIN)
            joins = sdscatprintf(joins, "%s#%s", sdslen(joins) ? ", " : "", d->source->task_id);
        else if (!seq && !edc_task_is_memory_sink(d->source))
            /* "queued behind" is about a predecessor the user can see. A fold / curation task
             * is the runtime's own housekeeping: naming it here would only confuse. */
            seq = d;
    }

    sds out = sdsempty();
    if (sdslen(joins) > 0)
        out = sdscatprintf(out, "→ task #%s · group #%s%s · waits for %s (outputs injected)", task->task_id, gid, fresh,
                           joins);
    else if (seq)
        out = sdscatprintf(out, "→ task #%s · group #%s%s · queued behind #%s", task->task_id, gid, fresh,
                           seq->source->task_id);
    /* else: a plain foreground creation -- the UI already shows it, so say nothing. */
    sdsfree(joins);
    return out;
}

sds edc_commands_tasks_receipt_text(struct edc_task *const *tasks, size_t count)
{
    if (!tasks || count == 0)
        return sdsempty();
    if (count == 1)
        return edc_commands_task_receipt_text(tasks[0]);

    /* Several tasks from one input (a `|` / `;` chain): one line naming them all, so a
     * three-segment pipeline is one receipt, not three. Silent when none of them is worth
     * mentioning. */
    bool any_notable = false;
    bool all_background = true;
    bool same_group = true;
    const char *group_id = NULL;
    sds ids = sdsempty();
    for (size_t i = 0; i < count; i++)
    {
        const struct edc_task *t = tasks[i];
        sds one = edc_commands_task_receipt_text(t);
        if (sdslen(one) > 0)
            any_notable = true;
        sdsfree(one);

        if (!t || !t->task_id)
            continue;
        ids = sdscatprintf(ids, "%s#%s", sdslen(ids) ? ", " : "", t->task_id);
        if (!t->is_background)
            all_background = false;

        const char *gid = (t->group && t->group->group_id) ? t->group->group_id : NULL;
        if (!gid)
            same_group = false;
        else if (!group_id)
            group_id = gid;
        else if (strcmp(group_id, gid) != 0)
            same_group = false;
    }

    if (!any_notable)
    {
        sdsfree(ids);
        return sdsempty();
    }

    sds out = sdscatprintf(sdsempty(), "→ %zu tasks %s", count, ids);
    if (same_group && group_id)
        out = sdscatprintf(out, " · group #%s", group_id);
    if (all_background)
        out = sdscat(out, " · background");
    sdsfree(ids);
    return out;
}

void edc_commands_announce_tasks(struct edc_reply_to to, struct edc_task *const *tasks, size_t count)
{
    sds text = edc_commands_tasks_receipt_text(tasks, count);
    if (sdslen(text) > 0)
        edc_commands_reply(to, text);
    sdsfree(text);
}

void edc_commands_announce_task(struct edc_reply_to to, const struct edc_task *task)
{
    sds text = edc_commands_task_receipt_text(task);
    if (sdslen(text) > 0)
        edc_commands_reply(to, text);
    sdsfree(text);
}

/* ── /session stop ─────────────────────────────────────────── */

/* Cancel every non-terminal turn in the current session and persist. Was the
 * top-level /stop; now /session with no arguments. */
static bool edc_commands_handle_session_stop(struct edc_session_manager *sm, struct edc_scheduler *sched,
                                             struct edc_reply_to to)
{
    edc_session_store_stop_all_turns(sm, sched);

    edc_commands_reply(to, "**Session** — stop signal sent to all running turns.");
    return true;
}

/* ── /help ─────────────────────────────────────────────────── */

/* The command surface, described ONCE. `/help` renders it, `/help <command>` looks a row
 * (or several) up, and this table is the thing to keep in sync with the dispatcher -- the
 * help text used to be a hand-written blob and drifted from the code (missing `curate`,
 * missing `-g/-d`, ...). Keep rows in dispatch order; a run of rows sharing |section| is
 * printed under one heading. */
struct edc_cmd_doc
{
    const char *section; /* heading */
    const char *cmd;     /* the spelling, as the user writes it */
    const char *brief;   /* one line */
    const char *detail;  /* longer text for `/help <command>`; NULL = the brief suffices */
};

static const struct edc_cmd_doc k_cmd_docs[] = {
    {"Input", "`desc`", "a message: a task in the current group, nothing parsed",
     "A line that does not begin with `\"` / `'` / `+` / `!` is a NORMAL MESSAGE: it becomes one task's "
     "input, verbatim, and nothing inside it is parsed -- no operators, no `{{…}}` references. This is "
     "ordinary conversation, so it needs no quoting at all. (`现在新建任务A|B` is a message: one task whose "
     "text contains the `|`.)"},
    {"Input", "`\"desc\"`", "SHELL syntax: a string literal (one task, but the line IS parsed)",
     "A shell line is recognised by its FIRST character. Every string in shell syntax is quoted, so such a "
     "line always begins with `\"` or `'` -- or with its sigil, `+` / `!`. The quoting rule is Python's: "
     "`\"…\"` interpolates `{{…}}`, `'…'` is raw, `\"\"\"…\"\"\"` / `'''…'''` span lines (the shape pasted "
     "text wants), escapes as in Python. Inside a shell line EVERY string is quoted: an unquoted operand is "
     "refused, never guessed (`\"a\" | b` is ambiguous, `\"a\" | \"b\"` is a pipe, `\"a|b\"` is one task). "
     "Everything AFTER the closing quote must be an option (`-d <id>`, `-g <group>`, `2>` …; there is no "
     "`--bg` in shell syntax -- backgrounding is the trailing `&`): "
     "anything else is refused rather than silently dropped, so `\"hello\" and more` is an error, not a "
     "task called `hello`."},
    {"Input", "`+\"desc\"`", "a new group + task",
     "`+` is a SHELL sigil, so the line is a shell line and its description is a quoted string -- like "
     "every string in shell syntax. The description goes into a NEW group; a plain message joins the "
     "current one. Quoting is also how you write a description that itself starts with `+` or `@` "
     "(`\"+1 issue\"` is text). Options ride after the closing quote: `+\"desc\" -d <id>`.\n\n"
     "`+` does not combine with the command syntax: `+/task new …` / `+!cmd` are not a thing (a line is "
     "one or the other). To put a task in a group of its own, either write `+\"desc\"`, or "
     "`/task new \"<desc>\" --new-group`.\n\n"
     "(`@` used to open a new group; it is the file-reference sigil now and no longer does.)"},
    {"Input", "`!<cmd>` · `!!`",
     "local shell: run it / rerun the last one (no model call)",
     "The text after `!` is a command line for `sh`, so it is NOT quoted here -- quoting is for the strings "
     "of our own syntax. To send the text `!cmd` as an ordinary task description instead, quote it: "
     "`\"!cmd\"`."},
    {"Input", "`\"a\" \\| \"b\"` · `\"a\" ; \"b\"` · `\"a\" &`",
     "pipe (injects the source output) · sequential (wait only) · background",
     "Both sides are quoted strings, because EVERY string in shell syntax is: `\"alpha\" | \"beta\"`. "
     "Unquoted there is no way to tell a `|` the user meant as text from one they meant as a pipe "
     "(`现在新建任务A|B` is either one task or two), so an unquoted operand is refused. Quote the `|` "
     "instead and it is text: `\"alpha|beta\"`. `|` chains tasks and injects the source's output into the "
     "next one's context; `;` only makes it wait; `&` runs it in the background (takes no foreground)."},
    {"Input", "`> file` `>> file` `2> file` `< file`", "redirect output / append / stderr / input",
     "Both the description and the target are quoted strings: `\"say hi\" > \"out.md\"`. The target's "
     "quotes are delimiters, not part of the path -- which is also what makes a filename with a space in "
     "it expressible (`> \"my file.txt\"`). `if x > 5` is therefore prose, not a redirect, and is refused "
     "rather than silently writing a file called `5`."},
    {"Input", "`{{LAST}}` `{{1}}` `{{SELF}}`", "task references (last output / a task's output / current task id)", NULL},

    {"General", "/help [<command>]", "show this list, or the detail of one command",
     "`/help` alone lists every command; `/help <command>` (a prefix, or a subcommand name "
     "like `memo`) prints that command's detail."},
    {"General", "/whoami", "show the current session, model, foreground group and active task", NULL},
    {"General", "/model", "list the current provider's chat models; `/model <id>` switches",
     "No argument lists the current provider's chat models (the current one marked). `/model <id>` switches "
     "to that model and persists it, applying the catalog's context window -- the id is the one the list "
     "shows (`/model` is the place to copy it from). `/model search <q>` searches every provider.\n\n"
     "`<id>` deliberately has NO row of its own: a row whose subcommand position holds a placeholder has "
     "no word to grow into, and WordCompletion() gives up for ALL candidates when any of them lacks a token "
     "at that index -- so that one row would cost `/model search` its completion. One row per completable "
     "subcommand word."},
    {"General", "/model search <q>", "search chat models across all providers", NULL},
    {"General", "/connect", "guided setup: choose a provider, API key, and model", NULL},
    {"General", "/prompt", "list the templates in workspace/prompts/", NULL},
    {"General", "/prompt <name> [args…]",
     "run a template: its placeholders are filled in and the body becomes ONE task, as written",
     "The body is a DOCUMENT, not a command line: the template's `$1` … `$N` / `$ARGUMENTS` placeholders are "
     "substituted (opencode's spelling -- `{{…}}` is NOT a placeholder), and what comes out is one task's "
     "input, verbatim. `| ; & >` in a body do nothing, and a `{{…}}` in it is plain text too. A body is "
     "therefore independent of the `/shell` switch -- its meaning must not depend on the reader's setting.\n\n"
     "`--new-group` runs it in a group of its own (a fresh conversation); `--group <id>` (also "
     "spelled `--group=<id>`) runs it in "
     "that existing group. Either may stand before or after the template name, because a template "
     "argument may not start with `--` (a single dash is fine). The task waits for whatever is still "
     "running in that group, whether or not the group is the one in front -- unless you add `--bg`: "
     "then it runs alongside instead (no queueing, and it does not take the front)."},
    {"General", "/skill", "list the skills (yours, plus any found in .claude / .agents / .pi / .opencode)", NULL},
    {"General", "/skill <name> [args…]",
     "run a skill directly: its SKILL.md body is loaded into this task (no tool round)",
     "Same rule as a prompt body: the skill text is a DOCUMENT, one task's input, verbatim -- never parsed "
     "as a command line. `--new-group` / `--group <id>` / `--bg` place it, exactly as they do for "
     "`/prompt`."},
    {"General", "/reload", "re-read the resource files and rebuild the cached system prompt", NULL},
    {"General", "/sandbox [on|off]", "enable/disable the filesystem sandbox", NULL},
    {"General", "/sandbox list", "show the allow/deny path lists", NULL},
    /* `/sandbox`'s six verbs are six rows, not one row with `add-rw|rm-rw` inside its
     * argument shape: the completion palette inserts the literal words, so a packed row
     * made Tab hand back `add-ro|rm-ro` as if that were the verb. One row per verb. */
    {"General", "/sandbox add-ro <dir>", "make a path read-only (it leaves the rw/deny lists)", NULL},
    {"General", "/sandbox rm-ro <dir>", "drop a read-only path", NULL},
    {"General", "/sandbox add-rw <dir>", "make a path read-write (it leaves the ro/deny lists)", NULL},
    {"General", "/sandbox rm-rw <dir>", "drop a read-write path", NULL},
    {"General", "/sandbox add-deny <dir>", "mask a dir (wins over ro/rw; e.g. ~/.ssh)", NULL},
    {"General", "/sandbox rm-deny <dir>", "unmask a dir", NULL},

    {"Scheduling", "/jobs", "list the scheduled jobs (cron / every / one-shot) with when they run next", NULL},
    {"Scheduling", "/jobs run <id>", "fire a scheduled job now; its schedule is left alone",
     "The job's instruction goes through the input pipeline exactly as a timer firing does, so "
     "the task it creates is the one the schedule would have created. The job stays scheduled."},
    {"Scheduling", "/jobs rm <id>", "remove a scheduled job", NULL},

    {"Session", "/session new [name] <dir>",
     "start a new session bound to <dir> (the name is optional, the directory is not)",
     "Command arguments are split like a shell splits them, so a value containing spaces has to "
     "be quoted: `/session new dev 'D:\my repo'`."},
    {"Session", "/session switch <name>", "switch to another session", NULL},
    {"Session", "/session list", "list all sessions", NULL},
    {"Session", "/session info", "show the current session's details (name, id, directory)", NULL},
    {"Session", "/session status [id]", "show a session's runtime state (no id = the active session)",
     "`/session status` is runtime state; `/session info` is static metadata."},
    {"Session", "/session save", "persist the current session's history and state", NULL},
    {"Session", "/session stop", "cancel all active tasks in the current session", NULL},
    {"Session", "/session remove <name>", "remove a session (the last one cannot be removed)", NULL},
    {"Session", "/session rename <old> <new>", "rename a session", NULL},
    /* `/session memo`'s sub-modes are rows of their own (not one row with the alternatives
     * inside its argument shape): the completion palette matches whole-row prefixes, so a
     * shape like "[show [id] | search <q> | ...]" would make `show`/`search`/`curate`/
     * `compact` impossible to complete. The docs list them separately too. */
    {"Session", "/session memo show [id]", "show the session memory (all entries, or one entry's detail)",
     "A bare `/session memo` is the same as `show`."},
    {"Session", "/session memo search <q>", "grep the session memory entries", NULL},
    {"Session", "/session memo curate [focus]", "pull durable knowledge out of the group memories now",
     "Curation reads the group memories and writes entries through its memory tools."},
    {"Session", "/session memo compact", "merge / condense the session memory entries now",
     "Compaction only rewrites the session entries (merge / condense / drop / rebuild the index)."},

    {"Task", "/task new <desc>   [--bg] [--new-group] [-g <group>] [-d <task> | --with <task>]", "create a task",
     "`--bg` runs it in the background (no foreground) · `--new-group` gives it a fresh "
     "conversation · `-g <group>` puts it in an existing group · `-d` / `--with <task>` makes it "
     "wait for that task AND injects its output into this one's context. `+/task new <desc>` says the "
     "same thing as `--new-group`.\n\n"
     "The description does NOT have to be quoted: an option is a whole word, so `/task new 写脚本 -d 1` "
     "is a task called `写脚本` that waits for task 1, while `/task new '写脚本 -d 1'` is a task called "
     "`写脚本 -d 1`. Quoting is what decides, exactly as it does everywhere else."},
    {"Task", "/task fork <parent> <desc>",
     "fork: the child leads a group of its own and inherits the parent's context", NULL},
    {"Task", "/task list", "list all groups and tasks", NULL},
    {"Task", "/task status [id]", "show a task's state, output, deps and turns", NULL},
    {"Task", "/task output <id>", "show only the task's output text", NULL},
    {"Task", "/task history <id>", "show the task's messages (`--json` = raw payload)", NULL},
    {"Task", "/task fg <id>", "foreground + set as active (only the active task outputs to terminal)", NULL},
    {"Task", "/task bg [id]", "move the foreground group (or that task) to the background", NULL},
    {"Task", "/task dep <id> [--drop [<source>]]",
     "show a task's dependency edges; `--drop` voids one (or all) so a task waiting on a source that can never "
     "produce again can run", NULL},
    {"Task", "/task stop [id]", "cancel a task (default: the foreground group's active task)", NULL},
    {"Task", "/task resume <id>",
     "resume an INTERRUPTED / ERROR / CANCELLED task; the id is required (there is no batch form)",
     NULL},

    {"Group", "/group list", "list all groups", NULL},
    {"Group", "/group info [id]", "a group's context / fold numbers (default: the foreground group)", NULL},
    {"Group", "/group files [id]", "the files the group's folded work read / modified", NULL},
    {"Group", "/group history <id>", "the whole group's history, one block per member", NULL},
    {"Group", "/group fold [id] [focus]", "fold the group now, ignoring the trigger", NULL},
};

#define EDC_CMD_DOC_COUNT (sizeof(k_cmd_docs) / sizeof(k_cmd_docs[0]))

/* The first tokens, one row each: a bare `/` in the completion palette lists these ~9
 * families instead of all ~45 rows. Kept beside the table, so a new family is one edit. */
/* The bare-`/` list. The THREE CONTAINERS lead: they are what a user reaches for, and the
 * popup only shows 8 rows -- with the leaves first the three families fell off the bottom.
 * (Table order is the display order; `/help`'s own list comes from k_cmd_docs.) */
static const struct edc_cmd_doc k_cmd_families[] = {
    {"Task", "/task", "create, inspect and control tasks", NULL},
    {"Session", "/session", "new / switch / list / save / remove sessions, and the session memory", NULL},
    {"Group", "/group", "list groups and their context / fold state", NULL},
    {"General", "/help", "show this list, or one command's detail", NULL},
    {"General", "/whoami", "show the current session, model and foreground task", NULL},
    {"General", "/model", "list / switch / search models", NULL},
    {"General", "/connect", "guided setup: provider, API key, model", NULL},
    {"General", "/prompt", "list and run prompt templates", NULL},
    {"General", "/skill", "list and run skills", NULL},
    {"General", "/reload", "re-read resources, rebuild the system prompt", NULL},
    {"General", "/sandbox", "the filesystem sandbox (on/off, ro/rw/deny paths)", NULL},
    {"General", "/shell", "the shell-like input syntax: sigils + operators (on/off)",
     "The switch owns the sigils `+<desc>` (open a new group) and `!<cmd>` (run a local shell command), and "
     "the operators `|` `;` `&` `>` `>>` `2>`. ON by default: they are this product's own idiom, and "
     "a line that begins with no quote and no sigil is still one plain task description, verbatim. "
     "With the switch off, EVERY line that is not a `/` command is one task description, verbatim. "
     "`/shell on|off` is the only way to flip it (persisted; no CLI flag).\n\n"
     "It governs what you TYPE. A `/prompt` or `/skill` body is a document, not input to this layer, so the "
     "switch never changes what a template does. And `@` is not part of it either: `@file` is a file "
     "reference (the opencode / Claude Code convention, not a Unix shell one), so it works with the switch "
     "off too."},
};
#define EDC_CMD_FAMILY_COUNT (sizeof(k_cmd_families) / sizeof(k_cmd_families[0]))

/* Read-only window onto the tables above: the surface as DATA, for the TUI's completion
 * palette (which must not parse the rendered markdown). Borrowed strings, static
 * lifetime. */
size_t edc_commands_doc_count(void)
{
    return EDC_CMD_DOC_COUNT;
}
const char *edc_commands_doc_cmd(size_t index)
{
    return index < EDC_CMD_DOC_COUNT ? k_cmd_docs[index].cmd : NULL;
}
const char *edc_commands_doc_brief(size_t index)
{
    return index < EDC_CMD_DOC_COUNT ? k_cmd_docs[index].brief : NULL;
}
size_t edc_commands_family_count(void)
{
    return EDC_CMD_FAMILY_COUNT;
}
const char *edc_commands_family_name(size_t index)
{
    return index < EDC_CMD_FAMILY_COUNT ? k_cmd_families[index].cmd : NULL;
}
const char *edc_commands_family_brief(size_t index)
{
    return index < EDC_CMD_FAMILY_COUNT ? k_cmd_families[index].brief : NULL;
}

/* Does a row match a `/help <arg>` query? A prefix of the command, or anywhere in it (so
 * `/help memo` finds `/session memo …`). */
static bool edc_cmd_doc_matches(const struct edc_cmd_doc *d, const char *q)
{
    return strncmp(d->cmd, q, strlen(q)) == 0 || strstr(d->cmd, q) != NULL;
}

sds edc_commands_help_text(const char *arg)
{
    if (!arg || !*arg)
    {
        sds out = sdsnew("## EdgeClaw Commands\n");
        const char *section = NULL;
        for (size_t i = 0; i < EDC_CMD_DOC_COUNT; i++)
        {
            const struct edc_cmd_doc *d = &k_cmd_docs[i];
            if (!section || strcmp(section, d->section) != 0)
            {
                section = d->section;
                out = sdscatprintf(out, "\n### %s\n", section);
            }
            out = sdscatprintf(out, "- %s — %s\n", d->cmd, d->brief);
        }
        return out;
    }

    sds out = sdscatprintf(sdsempty(), "## Help — `%s`\n", arg);
    bool any = false;
    for (size_t i = 0; i < EDC_CMD_DOC_COUNT; i++)
    {
        const struct edc_cmd_doc *d = &k_cmd_docs[i];
        if (!edc_cmd_doc_matches(d, arg))
            continue;
        any = true;
        out = sdscatprintf(out, "\n**%s**\n%s\n", d->cmd, d->brief);
        if (d->detail)
            out = sdscatprintf(out, "\n%s\n", d->detail);
    }
    if (!any)
        out = sdscatprintf(out, "\nNo command matches `%s`. Type `/help` for the full list.\n", arg);
    return out;
}

static bool edc_commands_handle_help(struct edc_reply_to to, const char *arg)
{
    sds text = edc_commands_help_text(arg);
    edc_commands_reply(to, text);
    sdsfree(text);
    return true;
}

/* ── /session save ─────────────────────────────────────────── */

/* Persist the current session's history + state. Was the top-level /save; now under /session. */
static bool edc_commands_handle_session_save(struct edc_session_manager *sm, struct edc_scheduler *sched,
                                             struct edc_reply_to to)
{
    struct edc_session *active = edc_session_manager_get_active(sm);
    if (!active)
    {
        edc_commands_reply(to, "**Session** — no active session.");
        return true;
    }

    edc_session_save_state_tagged(active, sched, "cmd:save");
    edc_commands_reply(to, "**Session saved** — history and state persisted.");
    log_info("receive /session save command");
    return true;
}

/* ── /session memo ─────────────────────────────────────────── */

/* Viewing and manual curation of L2 session memory. Scope = the ACTIVE session (the
 * command layer always speaks to the current session; the kernel-side tools are the ones
 * scoped "by task owner"). */
/* `/session memo show|search|curate|compact` -- the words are already the parser's. */
static bool edc_commands_handle_session_memo(struct edc_session_manager *sm, struct edc_scheduler *sched,
                                            struct edc_reply_to to, const char *sub, const char *tail)
{
    struct edc_session *active = edc_session_manager_get_active(sm);
    if (!active || !active->memo)
    {
        edc_commands_reply(to, "**Session memory** — no active session.");
        return true;
    }

    if (!*sub || strcmp(sub, "show") == 0)
    {
        sds text = edc_session_memory_show_text(active->memo, tail);
        edc_commands_reply(to, text);
        sdsfree(text);
    }
    else if (strcmp(sub, "search") == 0)
    {
        sds text = edc_session_memory_search_text(active->memo, tail);
        edc_commands_reply(to, text);
        sdsfree(text);
    }
    else if (strcmp(sub, "curate") == 0)
    {
        /* CURATION pulls durable knowledge OUT of the group memories into the index. Its material
         * is every group's memory plus the existing entries (see build_curate_prompt) -- that is
         * the job, so group content belongs in that prompt. It is NOT a compaction. */
        struct edc_task *t = edc_session_memory_start_manual(sched, active, (*tail ? tail : NULL));
        if (t)
            edc_commands_reply(to, "**Session memory** — curation started (it reads the group memories and "
                                   "writes entries through its memory tools). `/session memo show` when it "
                                   "is done.");
        else if (edc_scheduler_has_pending_system_task(active))
            edc_commands_reply(to, "**Session memory** — a system task is already running in this session; "
                                   "wait for it to finish.");
        else
            edc_commands_reply(to, "**Session memory** — could not start curation.");
    }
    else if (strcmp(sub, "compact") == 0)
    {
        /* COMPACTION shrinks the session memory and nothing else: merge / condense / drop / rebuild
         * the index. Its material is the entries themselves -- no group memory is involved (see
         * build_compact_prompt). Normally started by the entry cap; this runs one now. */
        struct edc_task *t = edc_session_memory_start_compact(sched, active);
        if (t)
            edc_commands_reply(to, "**Session memory** — compaction started (it only rewrites the session "
                                   "entries: merge / condense / drop / rebuild the index).");
        else if (edc_scheduler_has_pending_system_task(active))
            edc_commands_reply(to, "**Session memory** — a system task is already running in this session; "
                                   "wait for it to finish.");
        else
            edc_commands_reply(to, "**Session memory** — could not start compaction.");
    }
    else
    {
        edc_commands_reply(to, "Usage: `/session memo [show [id] | search <keyword> | curate [focus] | compact]`\n"
                               "`show` lists the entries, `show <id>` prints one entry, `search` matches title/summary/"
                               "detail, `curate` merges the group memories INTO the entries now (else it only runs "
                               "when a group is folded), `compact` shrinks the session entries themselves.");
    }

    return true;
}

/* ── /session commands ─────────────────────────────────────── */

/* The shared tail of switching the active session: tell the frontend which
 * session is now displayed. Since ①b snapshots are pulled, this must be said
 * explicitly. Only sent when the session really changed, after the reply, so the
 * "Switched" text lands in the old session's transcript. */
static void cmd_notify_if_active_session_changed(struct edc_session_manager *sm, struct edc_actor *channel_actor,
                                                 struct edc_session *before)
{
    struct edc_session *now = edc_session_manager_get_active(sm);
    if (now == before)
        return;
    edc_channel_notify_active_session(channel_actor, now ? now->session_id : NULL);
}

static bool edc_commands_handle_session_new(struct edc_session_manager *sm, struct edc_scheduler *sched,
                                            struct edc_reply_to to, const char *name, const char *dir)
{
    /* create() switches active to the new session, so take the old one first. */
    struct edc_session *old = edc_session_manager_get_active(sm);

    /* The directory is resolved up front against the process cwd. If it does not
     * resolve to an existing directory the whole command fails and NO session is
     * created -- never a silently unbound one. */
    EdcError *werr = NULL;
    sds resolved = edc_session_resolve_workdir(NULL, dir, &werr);
    if (!resolved)
    {
        sds reply = sdscatprintf(sdsempty(), "**Session** — failed: %s", werr ? werr->message : "invalid directory");
        edc_commands_reply(to, reply);
        sdsfree(reply);
        edc_error_free(werr);
        return true;
    }

    struct edc_session *sess = edc_session_manager_create_ex(sm, name, resolved);
    if (!sess)
    {
        sds reply = sdscatprintf(sdsempty(), "**Session** — failed to create a session bound to `%s`", resolved);
        edc_commands_reply(to, reply);
        sdsfree(reply);
        sdsfree(resolved);
        return true;
    }
    const char *bound = edc_session_get_workdir(sess);
    sdsfree(resolved);
    /* The old session keeps running in the background (a session is like a
     * terminal tab). Switching only changes active_index; persistence happens per
     * task and at exit. */
    (void)old;
    /* A fresh session: memory is authoritative, marked loaded so a later switch cannot overwrite it from disk. */
    sess->state_loaded = true;

    sds reply = sdscatprintf(sdsempty(), "**Session created** — `[%s]` %s  ·  workdir: `%s`", sess->session_id,
                             edc_session_get_name(sess), bound ? bound : "default");
    edc_commands_reply(to, reply);
    sdsfree(reply);
    cmd_notify_if_active_session_changed(sm, to.channel_actor, old);
    /* A switch is the only moment the resident set can grow, so it is also the moment to
     * give memory back: the sessions nobody has touched in a while are pure cache. */
    edc_session_manager_evict_idle(sm, sched, 0);
    return true;
}

static bool edc_commands_handle_session_switch(struct edc_session_manager *sm, struct edc_scheduler *sched,
                                               struct edc_reply_to to, sds arg)
{
    if (!arg || sdslen(arg) == 0)
    {
        edc_commands_reply(to, "Usage: `/session switch <name_or_id>`");
        return true;
    }
    struct edc_session *new_sess = edc_session_manager_find(sm, arg);
    if (!new_sess)
    {
        sds reply = sdscatprintf(sdsempty(), "Session not found: `%s`", arg);
        edc_commands_reply(to, reply);
        sdsfree(reply);
        return true;
    }
    struct edc_session *old = edc_session_manager_get_active(sm);
    if (old == new_sess)
    {
        sds reply =
            sdscatprintf(sdsempty(), "Already on `[%s]` %s", new_sess->session_id, edc_session_get_name(new_sess));
        edc_commands_reply(to, reply);
        sdsfree(reply);
        return true;
    }
    /* The old session keeps running in the background, so no task is cancelled;
     * persist once now, since it will keep progressing. */
    if (old)
    {
        edc_session_save_state_tagged(old, sched, "cmd:switch");
    }

    if (!edc_session_manager_switch(sm, arg))
    {
        sds reply = sdscatprintf(sdsempty(), "Session not found: `%s`", arg);
        edc_commands_reply(to, reply);
        sdsfree(reply);
        return true;
    }
    /* P3 ③: active_index is already updated by the switch; nothing else to change. */
    /* Deserialize disk state only on first activation this run; an already-loaded
     * session keeps its in-memory object. */
    if (!new_sess->state_loaded)
        edc_session_store_load_session(sm, sched, new_sess);
    sds reply =
        sdscatprintf(sdsempty(), "**Switched** — `[%s]` %s", new_sess->session_id, edc_session_get_name(new_sess));
    edc_commands_reply(to, reply);
    sdsfree(reply);
    cmd_notify_if_active_session_changed(sm, to.channel_actor, old);
    edc_session_manager_evict_idle(sm, sched, 0);
    return true;
}

static bool edc_commands_handle_session_list(struct edc_session_manager *sm, struct edc_reply_to to)
{
    sds list = edc_session_manager_list(sm);
    edc_commands_reply(to, list);
    sdsfree(list);
    return true;
}

static bool edc_commands_handle_session_remove(struct edc_session_manager *sm, struct edc_scheduler *sched,
                                               struct edc_reply_to to, sds arg)
{
    if (!arg || sdslen(arg) == 0)
    {
        edc_commands_reply(to, "Usage: `/session remove <name_or_id>`");
        return true;
    }
    struct edc_session *target = edc_session_manager_find(sm, arg);
    if (!target)
    {
        sds reply = sdscatprintf(sdsempty(), "Session not found: `%s`", arg);
        edc_commands_reply(to, reply);
        sdsfree(reply);
        return true;
    }
    /* The last session cannot be removed: check before cancelling, or a failed remove strands CANCELLED tasks. */
    if (sm->session_count <= 1)
    {
        edc_commands_reply(to, "**Session** — cannot remove the last session.");
        return true;
    }

    /* remove() may adjust active_index; note who was displayed first, then notify the frontend if it changed. */
    struct edc_session *active_before = edc_session_manager_get_active(sm);
    sds stopped_id = sdsnew(target->session_id);
    sds stopped_name = sdsnew(edc_session_get_name(target));

    /* /session remove = the user wants this tab gone: cancel every non-terminal
     * task in it (targeted explicitly, since it may not be the active session). */
    edc_scheduler_send_signal_to_session(sched, target, TASK_SIGTERM, false);

    if (!edc_session_manager_stop(sm, arg))
    {
        sdsfree(stopped_id);
        sdsfree(stopped_name);
        edc_commands_reply(to, "**Session** — cannot remove the last session.");
        return true;
    }
    struct edc_session *new_active = edc_session_manager_get_active(sm);
    if (new_active && !new_active->state_loaded)
        edc_session_store_load_session(sm, sched, new_active);
    sds reply = sdscatprintf(sdsempty(), "**Session removed** — `[%s]` %s", stopped_id, stopped_name);
    edc_commands_reply(to, reply);
    sdsfree(reply);
    cmd_notify_if_active_session_changed(sm, to.channel_actor, active_before);
    edc_session_manager_evict_idle(sm, sched, 0);
    sdsfree(stopped_id);
    sdsfree(stopped_name);
    return true;
}

static bool edc_commands_handle_session_rename(struct edc_session_manager *sm, struct edc_reply_to to,
                                               const char *old_name, const char *new_name)
{
    if (!edc_session_manager_rename(sm, old_name, new_name))
    {
        sds reply = sdscatprintf(sdsempty(), "Session not found: `%s`", old_name);
        edc_commands_reply(to, reply);
        sdsfree(reply);
        return true;
    }
    sds reply = sdscatprintf(sdsempty(), "**Session renamed** — `%s` → `%s`", old_name, new_name);
    edc_commands_reply(to, reply);
    sdsfree(reply);
    return true;
}

static bool edc_commands_handle_session_info(struct edc_session_manager *sm, struct edc_reply_to to)
{
    struct edc_session *sess = edc_session_manager_get_active(sm);
    if (!sess)
    {
        edc_commands_reply(to, "**Session** — no active session.");
        return true;
    }
    /* The timestamps are integers; this is the presentation edge, so format them here. */
    sds created = sess->created_at ? edc_datetime_format((time_t)sess->created_at) : NULL;
    sds active = sess->last_active ? edc_datetime_format((time_t)sess->last_active) : NULL;
    sds info =
        sdscatprintf(sdsempty(),
                     "## Current Session\n\n"
                     "- **ID**: %s\n"
                     "- **Name**: %s\n"
                     "- **Created**: %s\n"
                     "- **Last Active**: %s\n"
                     "- **Messages**: %zu\n"
                     "- **Directory**: %s\n"
                     "- **Working Directory**: %s\n",
                     sess->session_id, edc_session_get_name(sess), created ? created : "unknown",
                     active ? active : "unknown", edc_session_get_message_count(sess),
                     sess->history_dir ? sess->history_dir : "",
                     edc_session_get_workdir(sess) ? edc_session_get_workdir(sess) : "(default)");
    edc_commands_reply(to, info);
    sdsfree(info);
    sdsfree(created);
    sdsfree(active);
    return true;
}

/* /session status [name_or_id]: live runtime status (group/task counts, per-state
 * breakdown, foreground/active for the current session). Static metadata is in
 * /session info; per-task detail in /task list. */
static bool edc_commands_handle_session_status(struct edc_session_manager *sm, struct edc_reply_to to, sds arg)
{
    struct edc_session *sess = NULL;
    if (arg && sdslen(arg) > 0)
    {
        sess = edc_session_manager_find(sm, arg);
        if (!sess)
        {
            sds reply = sdscatprintf(sdsempty(), "Session not found: `%s`", arg);
            edc_commands_reply(to, reply);
            sdsfree(reply);
            return true;
        }
    }
    else
    {
        sess = edc_session_manager_get_active(sm);
        if (!sess)
        {
            edc_commands_reply(to, "**Session** — no current session.");
            return true;
        }
    }

    /* "Is this the current one" = is it the active session (the sole authority is the session_manager). */
    bool is_current = (sess == edc_session_manager_get_active(sm));
    struct edc_task_group *fg = is_current ? edc_scheduler_get_foreground(sess) : NULL;

    sds out = edc_session_status_text(sess, is_current, fg ? fg->group_id : NULL);
    edc_commands_reply(to, out);
    sdsfree(out);
    return true;
}

/* ── 子命令表 ───────────────────────────────────────────────────
 *
 * 一个命令的子命令 = 一组数据：动词 + 入口。派发从"一串 strncmp"变成"拆一次动词 + 查一次表"，
 * 于是动词表可以被测试读到（见 test_commands_help 的防漂移断言），也不会再出现
 * `strncmp(content, "/task new ", 10)` 那种手写长度的写法。 */
/* ── Sub-command arguments ────────────────────────────────────────
 *
 * A sub-command receives the words AFTER its verb: the parser produced them (see
 * edc_ast_cmd), so nobody re-splits text here. Every table is on this shape now --
 * `/session`, `/group`, `/task`, `/jobs` -- and the raw-tail form (edc_sub_fn) with its
 * splitter is gone. `/session` was migrated first and the rest followed one table at a
 * time; that order is why two shapes coexisted for a while. */
struct edc_sub_argv
{
    sds *argv;
    int argc;
};

/* Exactly |want| words? Replies |usage| and returns false when it is not. */
static bool sub_argv_exactly(struct edc_reply_to to, const struct edc_sub_argv *a, int want, const char *usage)
{
    if (a->argc == want)
        return true;
    edc_commands_reply(to, usage);
    return false;
}

/* At most |max| words? Replies |usage| and returns false when there are more. For the
 * commands whose operand is optional (`/group info [id]`, `/task status [id]`) -- the
 * bound is what matters, not an exact count. */
static bool sub_argv_exactly_or_none(struct edc_reply_to to, const struct edc_sub_argv *a, int max, const char *usage)
{
    if (a->argc <= max)
        return true;
    edc_commands_reply(to, usage);
    return false;
}

/* The words |from| .. end joined with single spaces: a free-text tail (a query, a focus). */
static sds sub_argv_tail(const struct edc_sub_argv *a, int from)
{
    sds out = sdsempty();
    for (int i = from; i < a->argc; i++)
    {
        if (i > from)
            out = sdscat(out, " ");
        out = sdscatsds(out, a->argv[i]);
    }
    return out;
}

/* ── /session 的子命令 ──────────────────────────────────────── */
static void sub_session_new(struct edc_session_manager *sm, struct edc_scheduler *sched, struct edc_reply_to to,
                            struct edc_channel_message *message, const struct edc_sub_argv *args)
{
    (void)message;
    /* ONE word can only be the directory, two are (name, dir), more is a usage error. */
    if (args->argc == 1)
    {
        (void)edc_commands_handle_session_new(sm, sched, to, NULL, args->argv[0]);
        return;
    }
    if (args->argc == 2)
    {
        (void)edc_commands_handle_session_new(sm, sched, to, args->argv[0], args->argv[1]);
        return;
    }
    edc_commands_reply(to, "Usage: `/session new [name] <dir>`\n"
                           "A session is bound to a working directory: `<dir>` is required and must already "
                           "exist. A directory that contains spaces has to be quoted: "
                           "`/session new dev 'D:\\my repo'`.");
}

static void sub_session_switch(struct edc_session_manager *sm, struct edc_scheduler *sched, struct edc_reply_to to,
                               struct edc_channel_message *message, const struct edc_sub_argv *args)
{
    (void)message;
    if (!sub_argv_exactly(to, args, 1, "Usage: `/session switch <name_or_id>` (quote a name that contains spaces)"))
        return;
    (void)edc_commands_handle_session_switch(sm, sched, to, args->argv[0]);
}

static void sub_session_list(struct edc_session_manager *sm, struct edc_scheduler *sched, struct edc_reply_to to,
                             struct edc_channel_message *message, const struct edc_sub_argv *args)
{
    (void)sched;
    (void)message;
    (void)args;
    (void)edc_commands_handle_session_list(sm, to);
}

static void sub_session_info(struct edc_session_manager *sm, struct edc_scheduler *sched, struct edc_reply_to to,
                             struct edc_channel_message *message, const struct edc_sub_argv *args)
{
    (void)sched;
    (void)message;
    (void)args;
    (void)edc_commands_handle_session_info(sm, to);
}

static void sub_session_status(struct edc_session_manager *sm, struct edc_scheduler *sched, struct edc_reply_to to,
                               struct edc_channel_message *message, const struct edc_sub_argv *args)
{
    (void)sched;
    (void)message;
    if (args->argc > 1)
    {
        edc_commands_reply(to, "Usage: `/session status [id]` (quote a name that contains spaces)");
        return;
    }
    (void)edc_commands_handle_session_status(sm, to, args->argc == 1 ? args->argv[0] : NULL);
}

static void sub_session_save(struct edc_session_manager *sm, struct edc_scheduler *sched, struct edc_reply_to to,
                             struct edc_channel_message *message, const struct edc_sub_argv *args)
{
    (void)message;
    (void)args;
    (void)edc_commands_handle_session_save(sm, sched, to);
}

static void sub_session_stop(struct edc_session_manager *sm, struct edc_scheduler *sched, struct edc_reply_to to,
                             struct edc_channel_message *message, const struct edc_sub_argv *args)
{
    (void)message;
    if (args->argc != 0)
    {
        /* `/session stop` takes no argument -- removing a session moved to `remove`. */
        edc_commands_reply(to, "`/session stop` takes no arguments. To remove a session use "
                               "`/session remove <name_or_id>`.");
        return;
    }
    (void)edc_commands_handle_session_stop(sm, sched, to);
}

static void sub_session_remove(struct edc_session_manager *sm, struct edc_scheduler *sched, struct edc_reply_to to,
                               struct edc_channel_message *message, const struct edc_sub_argv *args)
{
    (void)message;
    if (!sub_argv_exactly(to, args, 1, "Usage: `/session remove <name_or_id>` (quote a name that contains spaces)"))
        return;
    (void)edc_commands_handle_session_remove(sm, sched, to, args->argv[0]);
}

static void sub_session_rename(struct edc_session_manager *sm, struct edc_scheduler *sched, struct edc_reply_to to,
                               struct edc_channel_message *message, const struct edc_sub_argv *args)
{
    (void)sched;
    (void)message;
    if (!sub_argv_exactly(to, args, 2,
                          "Usage: `/session rename <old_name> <new_name>` (quote a name that contains spaces)"))
        return;
    (void)edc_commands_handle_session_rename(sm, to, args->argv[0], args->argv[1]);
}

static void sub_session_memo(struct edc_session_manager *sm, struct edc_scheduler *sched, struct edc_reply_to to,
                             struct edc_channel_message *message, const struct edc_sub_argv *args)
{
    (void)message;
    sds tail = sub_argv_tail(args, 1);
    (void)edc_commands_handle_session_memo(sm, sched, to, args->argc > 0 ? args->argv[0] : "", tail);
    sdsfree(tail);
}

/* `/session`'s sub-commands. This is the first table whose handlers take the parsed words
 * (`struct edc_sub_argv`) instead of the raw tail. */
typedef void (*edc_sub_argv_fn)(struct edc_session_manager *sm, struct edc_scheduler *sched, struct edc_reply_to to,
                                struct edc_channel_message *message, const struct edc_sub_argv *args);

struct edc_sub_argv_entry
{
    const char *verb;
    edc_sub_argv_fn fn;
};

static const struct edc_sub_argv_entry k_session_subs[] = {
    {"new", sub_session_new},       {"switch", sub_session_switch}, {"list", sub_session_list},
    {"info", sub_session_info},     {"status", sub_session_status}, {"save", sub_session_save},
    {"stop", sub_session_stop},     {"remove", sub_session_remove}, {"rename", sub_session_rename},
    {"memo", sub_session_memo},
};

static bool edc_commands_handle_session(struct edc_config *cfg, struct edc_session_manager *sm,
                                        struct edc_scheduler *sched, struct edc_reply_to to,
                                        struct edc_channel_message *msg, const struct edc_ast_cmd *cmd)
{
    (void)cfg;
    (void)msg;

    /* No verb at all: the usage line, same shape as bare `/task` and `/group`. */
    if (!cmd || cmd->argc == 0)
    {
        edc_commands_reply(to, "Usage: `/session` knows `new [name] <dir>`, `switch <name>`, `list`, `info`, "
                               "`status [id]`, `save`, `stop`, `remove <name>`, `rename <old> <new>`, `memo`. "
                               "Type `/help /session`.");
        return true;
    }

    const struct edc_sub_argv args = {cmd->argv + 1, cmd->argc - 1};
    for (size_t i = 0; i < sizeof(k_session_subs) / sizeof(k_session_subs[0]); i++)
    {
        if (strcmp(k_session_subs[i].verb, cmd->argv[0]) != 0)
            continue;
        k_session_subs[i].fn(sm, sched, to, msg, &args);
        return true;
    }

    edc_commands_reply(to, "**Unknown `/session` subcommand.** Try: `new`, `switch`, `list`, `info`, `status`, "
                           "`save`, `stop`, `remove`, `rename`.");
    return true;
}

/* ── /task commands ─────────────────────────────────────────― */

/* The session this input belongs to, as declared by the input itself; an empty
 * label means the active session, an unknown one rejects the command. */
static struct edc_session *cmd_input_session(struct edc_session_manager *sm, struct edc_channel_message *msg)
{
    return edc_session_store_resolve_session(sm, msg ? msg->session_id : NULL);
}

/* `/task new`'s OWN getopt -- the command layer's half of the input pipeline.
 *
 * The lexer already split the line, so an option is a whole WORD here: `-d 1` is two words,
 * `'-d 1'` is one. That single fact is the whole difference between
 *
 *     /task new 写脚本 -d 1     -> desc "写脚本", waits for task 1
 *     /task new '写脚本 -d 1'   -> desc "写脚本 -d 1", waits for nothing
 *
 * and it is decided by the user's quoting, not by a rule inside this handler. The old code
 * required the description to be quoted precisely because the raw text could not say where
 * the description stopped; now the splitter can, so the restriction is gone.
 *
 * The vocabulary (`--bg`, `--new-group`, `-d|--with`, `-g|--group`) is this command's
 * business, exactly as `-l` is `ls`'s: the splitter must not know it. Separated form only --
 * `-dt1` stays a word, because the lexer never makes it an option. */
static bool cmd_task_opts_from_argv(struct edc_reply_to to, const struct edc_sub_argv *args,
                                    struct edc_cmd_task_opts *opts)
{
    memset(opts, 0, sizeof(*opts));
    sds text = sdsempty(); /* the description: every non-option word, in order */
    bool any = false;

    for (int i = 0; i < args->argc; i++)
    {
        const char *w = args->argv[i];

        if (strcmp(w, "--bg") == 0)
        {
            opts->bg = true;
            continue;
        }
        if (strcmp(w, "--new-group") == 0)
        {
            opts->new_group = true;
            continue;
        }

        const bool is_dep = (strcmp(w, "-d") == 0 || strcmp(w, "--with") == 0);
        const bool is_group = (strcmp(w, "-g") == 0 || strcmp(w, "--group") == 0);
        if (is_dep || is_group)
        {
            /* The value is the NEXT word -- quoting put it there. A missing one is a usage
             * error: taking an empty value would silently drop the flag. */
            if (i + 1 >= args->argc)
            {
                sds reply = sdscatprintf(sdsempty(), "**`%s` needs a value** — `/task new <desc> %s <id>`.", w, w);
                edc_commands_reply(to, reply);
                sdsfree(reply);
                sdsfree(text);
                edc_cmd_task_opts_free(opts);
                return false;
            }
            const char *v = args->argv[++i];
            if (is_dep)
            {
                opts->dep_ids = realloc(opts->dep_ids, sizeof(sds) * (size_t)(opts->dep_count + 1));
                opts->dep_ids[opts->dep_count++] = sdsnew(v);
            }
            else
            {
                sdsfree(opts->group_id);
                opts->group_id = sdsnew(v);
            }
            continue;
        }

        if (w[0] == '-' && w[1] != '\0')
        {
            /* Option-shaped and not ours: refuse it. Folding it into the description is how
             * a typo would silently become part of the task's name. */
            sds reply = sdscatprintf(sdsempty(),
                                     "**Not an option** — `/task new` knows `--bg`, `--new-group`, "
                                     "`-d|--with <id>` and `-g|--group <id>`, not `%s`.\n\n"
                                     "If it belongs to the description, quote the description: "
                                     "`/task new '<text with %s>'`.",
                                     w, w);
            edc_commands_reply(to, reply);
            sdsfree(reply);
            sdsfree(text);
            edc_cmd_task_opts_free(opts);
            return false;
        }

        if (any)
            text = sdscat(text, " ");
        text = sdscat(text, w);
        any = true;
    }

    opts->input = text;
    opts->desc = edc_str_utf8_truncate(opts->input, EDC_TASK_DESC_MAX, true);
    return true;
}

static void edc_commands_handle_task_new(struct edc_session_manager *sm, struct edc_scheduler *sched,
                                         struct edc_reply_to to, struct edc_session *session,
                                         const struct edc_sub_argv *args)
{
    if (!session)
        return;
    struct edc_cmd_task_opts opts;
    if (!cmd_task_opts_from_argv(to, args, &opts))
        return;

    /* A task needs something to run. With no text there is nothing to attach, and a READY
     * task without a turn is never drivable: it just sits in the group forever (and would
     * be picked as the foreground group's "current task" by /task stop, /task status and
     * `{{SELF}}`). Same rule as the shell path. */
    if (!opts.input || sdslen(opts.input) == 0)
    {
        edc_commands_reply(to, "A task needs an input — write `/task new <desc>` "
                               "(flags alone do not create a task).");
        edc_cmd_task_opts_free(&opts);
        return;
    }

    /* Resolve -d dependencies up front (ids are globally unique, so a dep may live
     * in another session); an unknown id fails the command. */
    struct edc_task **deps = NULL;
    if (opts.dep_count > 0)
    {
        deps = calloc((size_t)opts.dep_count, sizeof(*deps));
        for (int i = 0; i < opts.dep_count; i++)
        {
            deps[i] = edc_session_store_resolve_task(sm, sched, opts.dep_ids[i], NULL);
            if (!deps[i])
            {
                sds reply = sdscatprintf(sdsempty(), "Task not found: `%s`", opts.dep_ids[i]);
                edc_commands_reply(to, reply);
                sdsfree(reply);
                free(deps);
                edc_cmd_task_opts_free(&opts);
                return;
            }
        }
    }

    /* Which group, who takes the foreground, the edges and the turn: one spec, one
     * call (edc_scheduler_task_create). This handler only turns the command's syntax
     * into that spec -- it no longer knows how a task is placed. */
    struct edc_task_spec spec;
    memset(&spec, 0, sizeof(spec));
    /* desc names the task (truncated for display); input is the whole prompt -- they are
     * not the same string once the prompt is longer than EDC_TASK_DESC_MAX. Non-empty:
     * the guard above rejected an empty input. */
    spec.desc = opts.desc;
    spec.channel_type = to.type;
    spec.input = opts.input;
    spec.deps = deps;
    spec.dep_count = (size_t)opts.dep_count;

    if (opts.group_id)
    {
        /* The name may not exist yet; the scheduler makes one led by this task. */
        if (!edc_session_find_group(session, opts.group_id))
        {
            sds reply = sdscatprintf(sdsempty(), "Group `%s` not found — creating a new group.", opts.group_id);
            edc_commands_reply(to, reply);
            sdsfree(reply);
        }
        spec.place = EDC_TASK_PLACE_GROUP;
        spec.group_id = opts.group_id;
    }
    else
    {
        /* No group named: join the group that is in front (creating one when the session
         * has none). It is the same group, i.e. the same conversation and the same queue --
         * a new group is a new conversation, so it takes an explicit --new-group. */
        spec.place = opts.new_group ? EDC_TASK_PLACE_NEW_GROUP : EDC_TASK_PLACE_FOREGROUND;
        spec.take_foreground = !opts.bg;
        spec.background = opts.bg;
    }

    struct edc_task *task = edc_scheduler_task_create(sched, session, &spec);
    free(deps);
    deps = NULL;
    if (!task)
    {
        edc_cmd_task_opts_free(&opts);
        return;
    }

    edc_commands_announce_task(to, task);

    if (opts.desc && sdslen(opts.desc) > 0 && edc_task_awaits_deps(task))
        log_info("/task new: task %s waits for its dependencies, turn queued", task->task_id);

    edc_cmd_task_opts_free(&opts);
}

/* Why a task ended, in words -- for /task list and /task status. NULL when it has not
 * ended (paused/running) or ended by succeeding, which the state already says. A BLOCKED
 * one names the dependency and what became of it, because "ERROR" alone cannot tell that
 * apart from the task failing by itself. */
static sds edc_commands_end_note(const struct edc_task *t)
{
    if (!t)
        return NULL;
    switch (t->end)
    {
    case EDC_END_INTERRUPTED:
        return sdsnew("interrupted: the process went down");
    case EDC_END_STOPPED:
        return sdsnew("stopped by the user");
    case EDC_END_FAILED:
        if (t->fail != EDC_FAIL_NONE)
            return sdscatprintf(sdsempty(), "failed on its own (%s)", edc_task_fail_name(t->fail));
        return sdsnew("failed on its own");
    case EDC_END_SKIPPED:
        return sdsnew("skipped: `||` saw its source succeed");
    case EDC_END_BLOCKED:
        if (t->blocked_end == EDC_END_NONE)
            return sdscatprintf(sdsempty(), "blocked by `%s` (it produced nothing)",
                                (t->blocked_by && *t->blocked_by) ? t->blocked_by : "a dependency");
        return sdscatprintf(sdsempty(), "blocked by `%s`, which was %s",
                            (t->blocked_by && *t->blocked_by) ? t->blocked_by : "a dependency",
                            edc_task_end_name(t->blocked_end));
    case EDC_END_NONE:
        break;
    }
    return NULL;
}

static void edc_commands_handle_task_list(struct edc_session_manager *sm, struct edc_reply_to to)
{
    sds out = sdsnew("## Tasks\n\n");
    struct edc_session *sess = edc_session_manager_get_active(sm);
    struct edc_task_group *g;
    bool any = false;
    DL_FOREACH(sess ? sess->groups : NULL, g)
    {
        any = true;
        bool fg = edc_scheduler_get_foreground(sess) == g;
        out =
            sdscatprintf(out, "- `%s` · %d task%s%s%s\n", g->group_id, g->member_count, g->member_count == 1 ? "" : "s",
                         fg ? " · **foreground**" : "", g->is_orphaned ? " · _orphaned_" : "");

        struct edc_task *t;
        LL_FOREACH2(g->members, t, group_next)
        {
            bool is_active = edc_session_manager_get_active(sm) && edc_session_manager_get_active(sm)->active_task == t;
            /* The state is an inline code span, not bold: the renderer colours a span that
             * carries a task-state name with that state's colour (markdown.cpp), so the list is
             * scannable by colour without any new markup. */
            out = sdscatprintf(out, "  - %s `%s` `%s`", t == g->leader ? "`[L]`" : "`[M]`", t->task_id,
                               edc_task_state_str(t));
            /* Why it ended, when the state alone does not say it (see EDC_END_*). */
            {
                sds note = edc_commands_end_note(t);
                if (note)
                {
                    out = sdscatprintf(out, " _%s_", note);
                    sdsfree(note);
                }
            }
            if (t->description && *t->description)
                out = sdscatprintf(out, " — %s", t->description);
            if (is_active)
                out = sdscat(out, "  ·  **active**");
            out = sdscat(out, "\n");
        }
    }
    if (!any)
        out = sdscat(out, "_No tasks._\n");
    edc_commands_reply(to, out);
    sdsfree(out);
}

static void edc_commands_handle_task_stop(struct edc_session_manager *sm, struct edc_scheduler *sched,
                                          struct edc_reply_to to, const char *id)
{
    struct edc_session *sess = edc_session_manager_get_active(sm);
    struct edc_task *t = NULL;

    if (!id || !*id)
    {
        /* No id: the one in front. Deliberately not the active task -- that one is the
         * screen focus and may sit on a task that already finished, on which stopping
         * would do nothing. */
        t = edc_scheduler_current_task(sess);
        if (!t)
        {
            edc_commands_reply(to, edc_scheduler_get_foreground(sess)
                                       ? "**Task** — nothing unfinished in the foreground group."
                                       : "**Task** — no foreground group.");
            return;
        }
    }
    else
    {
        /* Check the task exists first: otherwise a blind signal reports success for
         * an unknown id. Search spans all loaded sessions. */
        t = edc_session_store_resolve_task(sm, sched, id, NULL);
        if (!t)
        {
            sds reply = sdscatprintf(sdsempty(), "Task not found: `%s`", id);
            edc_commands_reply(to, reply);
            sdsfree(reply);
            return;
        }
    }
    edc_scheduler_send_signal_to_task_ptr(sched, t, TASK_SIGTERM);
    sds reply = sdscatprintf(sdsempty(), "**Task `%s`** — stop signal sent.", t->task_id);
    if (t->group && t->group->session && t->group->session != sess)
        reply = sdscatprintf(reply, " (session `[%s]`)", t->group->session->session_id);
    edc_commands_reply(to, reply);
    sdsfree(reply);
}

/* /task resume <id>: re-decide and drive a resumable task. Accepts INTERRUPTED
 * (the loop stopped mid-flight), ERROR (continue the failed round, reusing the active
 * turn and tool ledger), and CANCELLED (cancel only stopped it; the step is re-derived
 * from the messages). A task that `||` skipped is refused. The id is required. */
sds edc_commands_resume_text(const struct edc_task *t, edc_task_end was, edc_task_fail was_fail)
{
    if (!t)
        return sdsempty();

    const char *what = was == EDC_END_INTERRUPTED ? "was interrupted"
                      : was == EDC_END_FAILED    ? "had failed"
                      : was == EDC_END_BLOCKED   ? "was blocked by a dependency"
                                                 : "was stopped";
    sds out = sdscatprintf(sdsempty(), "**Task `%s`** — resumed (%s, %s).", t->task_id ? t->task_id : "?", what,
                           edc_task_state_str(t));

    /* A failed task is being retried: say what failed last time, or "resumed" looks like a
     * fresh start (an auth or context-overflow error will just fail again). First line, and
     * capped -- the full text is available with /task output. */
    if (was == EDC_END_FAILED && t->output)
    {
        const char *s = t->output;
        size_t n = 0;
        while (s[n] && s[n] != '\n')
            n++;
        bool cut = false;
        if (n > 100)
        {
            n = 100;
            cut = true;
        }
        while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80)
            n--; /* never end inside a UTF-8 sequence */
        if (n > 0)
            out = sdscatprintf(out, " Last error: %.*s%s", (int)n, s, cut ? "…" : "");
    }

    /* "Retried the same doomed request" is the case this class exists for: say it, so the
     * user fixes the cause instead of watching the same failure twice. */
    if (was == EDC_END_FAILED)
    {
        if (was_fail == EDC_FAIL_AUTH)
            out = sdscat(out, " This looks like an auth failure — fix the credentials (`/connect`) first; "
                              "a blind retry fails the same way.");
        else if (was_fail == EDC_FAIL_CONTEXT_OVERFLOW)
            out = sdscat(out, " The context did not fit — the same request will not fit either; let the "
                              "group fold, or start a smaller task.");
    }
    return out;
}

static void edc_commands_handle_task_resume(struct edc_session_manager *sm, struct edc_scheduler *sched,
                                            struct edc_reply_to to, const char *id)
{
    struct edc_session *sess = edc_session_manager_get_active(sm);
    struct edc_task *t = NULL;

    /* Resume is per task, and there is NO batch form. A stopped task is a decision
     * point, so reviving them wholesale is not a shortcut -- it also revives what the
     * user deliberately stopped, tasks that failed for good, and the `||` branches that
     * were SKIPPED because their source succeeded (their edge is satisfied, so a bulk
     * resume would actually run them). An option-shaped argument is therefore a usage
     * error and never a task id. */
    if (id && id[0] == '-')
    {
        sds reply = sdscatprintf(sdsempty(),
                                 "**Task resume** — unknown option `%s`. Usage: `/task resume [id]`, one task at "
                                 "a time.",
                                 id);
        edc_commands_reply(to, reply);
        sdsfree(reply);
        return;
    }

    if (!id || !*id)
    {
        /* The id is REQUIRED. There is no "the obvious one": reviving a task is a decision
         * about a specific task, and a derivation here would happily pick a running member
         * of the foreground group and then refuse it. `/task list` names them. */
        edc_commands_reply(to, "**Task resume** — an id is required: `/task resume <id>`. `/task list` shows the "
                               "tasks and their states.");
        return;
    }
    else
    {
        t = edc_session_store_resolve_task(sm, sched, id, NULL);
        if (!t)
        {
            sds reply = sdscatprintf(sdsempty(), "Task not found: `%s`", id);
            edc_commands_reply(to, reply);
            sdsfree(reply);
            return;
        }
    }

    /* `||` decided this branch is not taken (checked BEFORE the generic gate so the
     * specific answer wins): running it now would execute exactly the thing the shell
     * said should not run, with the input it was created with. Sending the command again
     * is the unambiguous way to ask. */
    if (t->end == EDC_END_SKIPPED)
    {
        sds reply = sdscatprintf(sdsempty(),
                                 "**Task `%s`** — skipped: `||` saw its source succeed, so this branch was not "
                                 "taken. Send the command again if you want it to run.",
                                 t->task_id);
        edc_commands_reply(to, reply);
        sdsfree(reply);
        return;
    }

    /* The resumable kinds: INTERRUPTED, ERROR, CANCELLED (only COMPLETED
     * and ALIVE are not). */
    if (!edc_task_is_resumable(t))
    {
        sds reply = sdscatprintf(sdsempty(), "**Task `%s`** — %s, not resumable.", t->task_id, edc_task_state_str(t));
        edc_commands_reply(to, reply);
        sdsfree(reply);
        return;
    }

    /* Resume gate: try to restore any cross-session dependency now (may lazy-load
     * its owner); if it cannot be resolved, hold the task. */
    edc_session_store_reconcile_pending_deps(sm, sched);
    const char *missing = edc_scheduler_first_unresolved_dep(sched, t);
    if (missing)
    {
        sds reply = sdscatprintf(sdsempty(),
                                 "**Task `%s` is held** — it depends on `%s`, which could not be restored "
                                 "(no loaded session provides it). It will not run until that dependency is available.",
                                 t->task_id, missing);
        edc_commands_reply(to, reply);
        sdsfree(reply);
        return;
    }

    /* What it was, said before resume clears the reason (any move back to ALIVE wipes it). */
    const edc_task_end was_end = t->end;
    const edc_task_fail was_fail = t->fail;

    edc_scheduler_resume_task(sched, t);

    /* Still waiting on deps? (the other end produced nothing: the source was cancelled /
     * failed, or has not finished.) Say whom it waits for -- after cancel stopped
     * detaching edges this is a NORMAL state and must not look like "clicked, no
     * response". Ways out: resume that source (once it produces, the edge auto-satisfies
     * and this task is released), or delete that source. */
    if (edc_task_awaits_deps(t))
    {
        sds waiting = sdsempty();
        struct edc_task_dep *dep;
        LL_FOREACH(t->deps, dep)
        {
            if (dep->completed || dep->cancelled || !dep->source)
                continue;
            if (sdslen(waiting))
                waiting = sdscat(waiting, ", ");
            waiting = sdscatprintf(waiting, "`%s` (%s)", dep->source->task_id, edc_task_state_str(dep->source));
        }
        /* No "its output arrives": a SEQUENCE / BARRIER edge injects nothing, it only
         * needs the dependency to produce (succeed). */
        sds reply = sdscatprintf(sdsempty(),
                                 "**Task `%s` is waiting** — it still depends on %s. Resume one of them "
                                 "(`/task resume <id>`); this task runs once its dependency produces.",
                                 t->task_id, sdslen(waiting) ? waiting : "an unfinished dependency");
        if (t->group && t->group->session && t->group->session != sess)
            reply = sdscatprintf(reply, " (session `[%s]`)", t->group->session->session_id);
        edc_commands_reply(to, reply);
        sdsfree(reply);
        sdsfree(waiting);
        return;
    }

    /* What it was, now that it is being driven again (the move to ALIVE cleared `end`). */
    sds reply = edc_commands_resume_text(t, was_end, was_fail);
    if (t->group && t->group->session && t->group->session != sess)
        reply = sdscatprintf(reply, " (session `[%s]`)", t->group->session->session_id);
    edc_commands_reply(to, reply);
    sdsfree(reply);
}

/* A task in a non-active session can be referenced globally (dependency, status,
 * stop/resume), but fg/bg/fork must not silently touch it. Returns true when |t|
 * is in the active session, otherwise replies with an error. */
static bool edc_commands_task_in_active_session(struct edc_session_manager *sm, struct edc_reply_to to,
                                                struct edc_task *t)
{
    struct edc_session *active = edc_session_manager_get_active(sm);
    struct edc_session *owner = (t->group && t->group->session) ? t->group->session : active;
    if (owner == active)
        return true;
    sds reply =
        sdscatprintf(sdsempty(), "Task `%s` belongs to session `[%s]` — switch to it first (`/session switch %s`).",
                     t->task_id, owner->session_id, owner->session_id);
    edc_commands_reply(to, reply);
    sdsfree(reply);
    return false;
}

static void edc_commands_handle_task_fg(struct edc_session_manager *sm, struct edc_reply_to to, const char *id)
{
    if (!id || !*id)
    {
        edc_commands_reply(to, "Usage: `/task fg <id>`");
        return;
    }
    struct edc_task *t = edc_session_manager_find_task_any(sm, id, NULL);
    if (!t)
    {
        edc_commands_reply(to, "Task not found.");
        return;
    }
    if (!edc_commands_task_in_active_session(sm, to, t))
        return;
    struct edc_task_group *g = edc_session_find_group(edc_session_manager_get_active(sm), t->group_id);
    if (!g)
    {
        edc_commands_reply(to, "Group not found.");
        return;
    }
    if (edc_task_group_is_system(g))
    {
        /* The system group holds fold / curation work; putting it in front would let it
         * take the screen and become what an id-less command acts on. */
        edc_commands_reply(to, "**Foreground** — that task lives in the system group (fold / curation); "
                               "the foreground is for user groups.");
        return;
    }
    edc_scheduler_set_foreground(edc_session_manager_get_active(sm), g);
    edc_session_set_active_task(edc_session_manager_get_active(sm), t);
    sds reply = sdscatprintf(sdsempty(), "**Foreground** — group `%s` · active task `%s`.", g->group_id, t->task_id);
    edc_commands_reply(to, reply);
    sdsfree(reply);
}

static void edc_commands_handle_task_bg(struct edc_session_manager *sm, struct edc_reply_to to, const char *id)
{
    if (id && *id)
    {
        struct edc_task *t = edc_session_manager_find_task_any(sm, id, NULL);
        if (!t)
        {
            edc_commands_reply(to, "Task not found.");
            return;
        }
        if (!edc_commands_task_in_active_session(sm, to, t))
            return;
        struct edc_task_group *g = edc_session_find_group(edc_session_manager_get_active(sm), t->group_id);
        if (!g)
        {
            edc_commands_reply(to, "Group not found.");
            return;
        }
        if (edc_scheduler_get_foreground(edc_session_manager_get_active(sm)) == g)
        {
            edc_scheduler_set_foreground(edc_session_manager_get_active(sm), NULL);
            edc_session_set_active_task(edc_session_manager_get_active(sm), NULL);
            sds reply = sdscatprintf(sdsempty(), "**Background** — group `%s`.", g->group_id);
            edc_commands_reply(to, reply);
            sdsfree(reply);
        }
        else
        {
            sds reply = sdscatprintf(sdsempty(), "Group `%s` is already in the background.", g->group_id);
            edc_commands_reply(to, reply);
            sdsfree(reply);
        }
    }
    else
    {
        edc_scheduler_set_foreground(edc_session_manager_get_active(sm), NULL);
        edc_session_set_active_task(edc_session_manager_get_active(sm), NULL);
        edc_commands_reply(to, "No foreground group.");
    }
}

static void edc_commands_handle_task_fork(struct edc_session_manager *sm, struct edc_scheduler *sched,
                                          struct edc_reply_to to, struct edc_session *session,
                                          const struct edc_sub_argv *args)
{
    if (!session)
        return;
    /* /task fork <parent_id> <desc>: the id is the first word, the description is the rest.
     * Both were split already (quoting decided the words). */
    if (args->argc < 2)
    {
        edc_commands_reply(to, "Usage: `/task fork <parent_id> <desc>`");
        return;
    }
    /* Borrowed from the caller's AST -- never freed here. */
    sds *tokens = args->argv;
    const int count = args->argc;

    /* `session` is a parameter: the session THIS INPUT belongs to (resolved from the
     * message's tag -- no more "ask the active session and assume it is this input's
     * home"). */
    struct edc_task *parent = edc_session_manager_find_task_any(sm, tokens[0], NULL);
    if (!parent)
    {
        sds reply = sdscatprintf(sdsempty(), "Parent task not found: `%s`", tokens[0]);
        edc_commands_reply(to, reply);
        sdsfree(reply);
        return;
    }
    if (!edc_commands_task_in_active_session(sm, to, parent))
        return;

    /* the rest of the args are the input */
    sds desc = sub_argv_tail(args, 1);

    size_t fork_point = parent->group ? (size_t)edc_task_group_history_count(parent->group) : 0;
    /* The fork point is expressed as a commit-order watermark: the max completion_seq among the parent group's
     * committed members. */
    uint32_t fork_point_seq = 0;
    if (parent->group)
    {
        struct edc_task *m;
        LL_FOREACH2(parent->group->members, m, group_next)
        {
            if (m->completion_seq > fork_point_seq)
                fork_point_seq = m->completion_seq;
        }
    }
    char *fork_tid = edc_scheduler_gen_task_id(sched);
    struct edc_task *child = edc_task_fork(parent, fork_tid, desc, fork_point_seq, to.type);
    free(fork_tid);

    /* The child starts its own group (as leader) and takes the foreground; desc is the
     * input, as in /task new. Which group and who owns the foreground is the
     * scheduler's answer (edc_scheduler_task_adopt), not this handler's. */
    struct edc_task_spec spec;
    memset(&spec, 0, sizeof(spec));
    spec.place = EDC_TASK_PLACE_NEW_GROUP;
    spec.take_foreground = true;
    spec.input = desc;
    if (!child || !edc_scheduler_task_adopt(sched, session, child, &spec))
    {
        edc_commands_reply(to, "Fork failed — the child task could not be placed.");
        if (child)
            edc_task_free(child);
        sdsfree(desc);
        return;
    }

    /* The child group inherits the parent's transcript snapshot as a synthetic
     * initial turn. */
    struct edc_task_group *child_group = child->group;

    if (parent->group)
    {
        /* inherit the parent group's memory summary (body + tracked-file lists: the child
         * continues the same work, so it starts from the same knowledge) */
        if (parent->group->memory)
            child_group->memory = sdsdup(parent->group->memory);
        if (parent->group->fold_files)
            child_group->fold_files = edc_fold_files_dup(parent->group->fold_files);

        /* The system prompt takes history[0] first, so the snapshot's first message is not mistaken for the system
         * prompt by cb_send_llm */
        /* The slot API hands out an owned copy, so it is moved straight into the child. */
        cJSON *sys = edc_task_group_history_at(parent->group, 0);
        if (sys)
            edc_task_group_history_add_owned(child_group, sys);

        /* Synthesize a turn: deep-copy the parent group's unfolded messages
         * [1 .. fork_point) -- folded messages are not in history (pruned at landing). */
        size_t snap_begin = 1;
        struct edc_turn *snap = NULL;

        if (edc_task_has_folded_turns(parent))
        {
            /* Rule 2: the parent's turns were folded by L1, so recover the parent's own
             * full turns to keep the context at the fork point intact. Earlier folded
             * history still relies only on the memory summary. */
            snap = edc_task_snapshot_turns(parent, child->session_id, child->group_id, child->task_id,
                                           child->active_turn ? 1 : 0);
        }
        else
        {
            /* Rule 1: the unfolded span [1+folded_count, fork_point) is the snapshot.
             * appending each message writes it to the store under the child's turn 0. */
            /* The child's input turn (attached by adopt) already holds ordinal 0; a second
             * turn on the same ordinal would share its rows. */
            snap = edc_turn_new_for(child->session_id, child->group_id, child->task_id,
                                    child->active_turn ? 1 : 0);
            if (snap_begin > fork_point)
                snap_begin = fork_point;
            for (size_t i = snap_begin; i < fork_point; i++)
            {
                cJSON *msg = edc_task_group_history_at(parent->group, (int)i);
                if (!msg)
                    break;
                /* A copy MUST get a fresh identity: messages.id is the primary key, so
                 * reusing it would replace the parent's row. */
                cJSON_DeleteItemFromObject(msg, "id");
                edc_turn_add_message(snap, msg);
            }
        }

        if (snap && snap->msg_count > 0)
        {
            /* The child group's history is the derived view of the turns; rebuild it from
             * the snapshot the same way every other path does. */
            edc_task_group_history_add_turn(child_group, snap);
            edc_task_add_turn(child, snap);
        }
        else if (snap)
        {
            edc_turn_free(snap);
        }
    }

    /* The foreground is already on the child and the input turn is already attached
     * (adopt). Running it is the message-end scheduler's job (the only drive entry). */
    log_info("/task fork: task=%s turn attached (deps_total=%zu)", child->task_id, child->deps_total);

    sds reply = sdscatprintf(sdsempty(), "**Fork created** — `%s` from `%s` · group `%s` · fork point %zu",
                             child->task_id, parent->task_id, child->group_id, fork_point);
    edc_commands_reply(to, reply);
    sdsfree(reply);
    sdsfree(desc);
}

static void edc_commands_handle_group_list(struct edc_session_manager *sm, struct edc_reply_to to)
{
    sds out = sdsnew("## Groups\n\n");
    struct edc_task_group *g;
    struct edc_session *sess = edc_session_manager_get_active(sm);
    DL_FOREACH(sess ? sess->groups : NULL, g)
    {
        out = sdscatprintf(
            out, "- %s%s  **%s**  %d members  _%s_\n", edc_scheduler_get_foreground(sess) == g ? "`[fg]` " : "",
            g->is_orphaned ? "`[orphan]` " : "", g->group_id, g->member_count, edc_task_group_status_label(g));
    }
    edc_commands_reply(to, out);
    sdsfree(out);
}

static void edc_commands_handle_task_status(struct edc_session_manager *sm, struct edc_scheduler *sched,
                                            struct edc_reply_to to, const char *id)
{
    struct edc_task *t;
    struct edc_session *owner = NULL;
    if (id && *id)
    {
        /* Global lookup: a task in another session can be inspected too. */
        t = edc_session_store_resolve_task(sm, sched, id, &owner);
        if (!t)
        {
            edc_commands_reply(to, "Task not found.");
            return;
        }
    }
    else
    {
        owner = edc_session_manager_get_active(sm);
        t = owner ? edc_scheduler_current_task(owner) : NULL;
        if (!t)
        {
            edc_commands_reply(to, "No task in front — the foreground group is empty or all its tasks have finished.");
            return;
        }
    }

    sds out = sdscatprintf(sdsempty(),
                           "## Task `%s`\n\n"
                           "- **state**: %s\n"
                           "- **group**: %s\n"
                           "- **desc**: %s\n",
                           t->task_id, edc_task_state_str(t), t->group_id, t->description);

    /* The state says which bucket it is in; this says why it got there. Without it a
     * CANCELLED task looks the same whether the user stopped it, a `||` branch skipped it,
     * or a dependency took it down. */
    {
        sds note = edc_commands_end_note(t);
        if (note)
        {
            out = sdscatprintf(out, "- **ended**: %s\n", note);
            sdsfree(note);
        }
    }

    /* For a task of a non-current session, also mark its owning session, or the user cannot tell where it came from. */
    {
        struct edc_session *active = edc_session_manager_get_active(sm);
        if (owner && owner != active)
            out = sdscatprintf(out, "- **session**: `[%s]`\n", owner->session_id);
    }

    /* Unresolved cross-session dependency: the task is "stuck" and resume is refused by the gate. */
    {
        const char *missing = edc_scheduler_first_unresolved_dep(sched, t);
        if (missing)
            out = sdscatprintf(out, "- **held**: waiting on `%s` (unresolved cross-session dependency)\n", missing);
    }

    /* The user input the task actually received (not just the description); the
     * messages belong to the turn. */
    sds user_input = edc_task_user_input(t, NULL);
    if (user_input)
    {
        sds indented = sdsempty();
        for (const char *p = user_input; *p; p++)
        {
            if (*p == '\n')
                indented = sdscat(indented, "\n  ");
            else
                indented = sdscatlen(indented, p, 1);
        }
        out = sdscatprintf(out, "- **input**: %s\n", indented);
        sdsfree(indented);
    }
    sdsfree(user_input);

    out = sdscatprintf(out,
                       "- **deps**: %zu/%zu completed\n"
                       "- **messages**: %zu\n",
                       t->deps_completed, t->deps_total, edc_task_message_count(t));
    /* Token usage over the task's lifetime; omitted while zero. */
    if (t->tokens_total > 0)
    {
        out = sdscatprintf(out, "- **tokens**: %lld total (%lld prompt · %lld completion · %lld cached)\n",
                           t->tokens_total, t->tokens_prompt, t->tokens_completion, t->tokens_cached);
    }
    if (t->output)
    {
        out = sdscatprintf(out, "- **output**: %s\n", t->output);
    }
    if (t->forked_from)
    {
        out = sdscatprintf(out, "- **forked from**: `%s`\n", t->forked_from->task_id);
    }
    edc_commands_reply(to, out);
    sdsfree(out);
}

static void edc_commands_handle_task_output(struct edc_session_manager *sm, struct edc_scheduler *sched,
                                            struct edc_reply_to to, const char *id)
{
    if (!id || !*id)
    {
        edc_commands_reply(to, "Usage: `/task output <id>`");
        return;
    }
    struct edc_task *t = edc_session_store_resolve_task(sm, sched, id, NULL);
    if (!t)
    {
        edc_commands_reply(to, "Task not found.");
        return;
    }
    if (!t->output)
    {
        edc_commands_reply(to, "_No output._");
        return;
    }
    edc_commands_reply(to, t->output);
}

/* /task history <id>: the task's message history (its construction input and each
 * turn's messages), from task->turns. Works across sessions. --json prints the raw
 * payload. */
/* `/group history [--json] <id>` and `/task history [--json] <id>`.
 *
 * The operands arrive already split (the lexer owns word boundaries), so `--json` is a word
 * here rather than a prefix of the raw text. It may stand anywhere among the words: the
 * splitter cannot know which words are options of which command -- that knowledge is exactly
 * what used to require a fifth tokenizer in this file. */
static void edc_commands_handle_history(struct edc_session_manager *sm, struct edc_scheduler *sched,
                                        struct edc_reply_to to, const char *task_id, bool want_json, bool group)
{
    if (!task_id || !*task_id)
    {
        edc_commands_reply(to, group ? "Usage: `/group history <id>`"
                                     : "Usage: `/task history <id>`  (append `--json` for the raw payload)");
        return;
    }

    /* The resolver also returns the owning session: a task id is globally unique, so this command can find another
     * session's task. */
    struct edc_session *owner = NULL;
    struct edc_task *t = edc_session_store_resolve_task(sm, sched, task_id, &owner);
    if (!t || !owner)
    {
        edc_commands_reply(to, "Task not found.");
        return;
    }

    /* A group's id IS its leader's task id (edc_task_group_new), so the same resolver
     * knows groups too; the group payload is chunked by member (each block is that
     * member's own messages). */
    const char *target_id = group ? task_id : t->task_id;
    char *payload = edc_scheduler_task_history_json(sched, owner, group ? target_id : NULL, group ? NULL : target_id);
    if (!payload)
    {
        edc_commands_reply(to, "_No history._");
        return;
    }
    /* Structured reply: the frontend splits it into bubbles. With `--json` it reverts to
     * text (debug/test), which also contrasts "two renderings of one payload". */
    if (!want_json)
    {
        edc_commands_send_history(to, payload);
        free(payload);
        return;
    }
    edc_commands_reply(to, payload);
    free(payload);
    return;

    cJSON *root = cJSON_Parse(payload);
    free(payload);
    if (!root)
    {
        edc_commands_reply(to, "Failed to read the history.");
        return;
    }

    const cJSON *messages = cJSON_GetObjectItem(root, "messages");
    const int count = cJSON_GetArraySize(messages);
    const char *input = cJSON_GetStringValue(cJSON_GetObjectItem(root, "input"));

    sds out = sdscatprintf(sdsempty(), "**task %s** · %d message%s\n", t->task_id, count, count == 1 ? "" : "s");
    if (input && *input)
        out = sdscatprintf(out, "_input:_ %s\n", input);

    int i = 0;
    const cJSON *m = NULL;
    cJSON_ArrayForEach(m, messages)
    {
        i++;
        const char *role = cJSON_GetStringValue(cJSON_GetObjectItem(m, "role"));
        const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(m, "name"));
        const char *body = cJSON_GetStringValue(cJSON_GetObjectItem(m, "content"));
        sds who = sdsnew(role ? role : "?");
        if (name && *name)
            who = sdscatprintf(who, "(%s)", name);

        const int full = body ? (int)strlen(body) : 0;
        const int cap = full > 400 ? 400 : full;
        out = sdscatprintf(out, "%d. **%s**: %.*s%s\n", i, who, cap, body ? body : "", full > cap ? " …" : "");
        sdsfree(who);
    }
    cJSON_Delete(root);
    edc_commands_reply(to, out);
    sdsfree(out);
}

/* `/group fold [id] [focus]`: fold once now -- BYPASSING the trigger predicate
 * (window/count), but the barrier is unchanged (in-flight members are not interrupted,
 * one per session). No id = the current foreground group; an id is resolved as given
 * (group id = the group head's task id, possibly cross-session). The remaining text goes
 * verbatim as focus into the summary prompt's "Additional focus:". */
static void edc_commands_handle_group_fold(struct edc_session_manager *sm, struct edc_scheduler *sched,
                                           struct edc_reply_to to, const char *id, const char *focus)
{
    struct edc_session *session = NULL;
    struct edc_task_group *group = NULL;

    if (id && *id)
    {
        /* `id` is the first word; `focus` is every word after it, joined with single
         * spaces by the caller (quoting decided where the words broke). */
        struct edc_session *owner = NULL;
        struct edc_task *t = edc_session_store_resolve_task(sm, sched, id, &owner);
        if (!t || !owner || !(group = edc_session_find_group(owner, t->group_id)))
        {
            sds reply = sdscatprintf(
                sdsempty(), "Group `%s` not found — `/group fold [id] [focus]` (no id = the foreground group).", id);
            edc_commands_reply(to, reply);
            sdsfree(reply);
            return;
        }
        session = owner;
    }
    else
    {
        session = edc_session_manager_get_active(sm);
        group = session ? edc_scheduler_get_foreground(session) : NULL;
        if (!group)
        {
            edc_commands_reply(to, "No foreground group to fold — pass a group id: `/group fold <id>`.");
            return;
        }
    }
    if (focus && !*focus)
        focus = NULL;

    struct edc_task *fold = edc_scheduler_fold_group_now(sched, session, group, focus);
    if (!fold)
    {
        sds reply = sdscatprintf(sdsempty(),
                                 "Nothing to fold in group `%s` — it has no unfolded messages, or a fold is "
                                 "already in flight for that session.",
                                 group->group_id);
        edc_commands_reply(to, reply);
        sdsfree(reply);
        return;
    }

    sds reply = sdscatprintf(sdsempty(), "Folding group `%s` into its Group Memory%s%s.", group->group_id,
                             focus ? " with focus: " : "", focus ? focus : "");
    edc_commands_reply(to, reply);
    sdsfree(reply);
}

/* Number of entries in a newline-separated list (the shape
 * edc_group_fold_tracked_files() hands back). */
static size_t edc_commands_count_entries(const sds list)
{
    if (!list || sdslen(list) == 0)
        return 0;
    int n = 0;
    char **lines = sdssplitlen(list, (ssize_t)sdslen(list), "\n", 1, &n);
    size_t count = 0;
    for (int i = 0; lines && i < n; i++)
    {
        if (lines[i] && lines[i][0])
            count++;
    }
    if (lines)
        sdsfreesplitres(lines, n);
    return count;
}

/* Append one "### Title (n)" section with its entries as a bullet list. `count` is the
 * entry count the caller already computed (0 = omit the section entirely). */
static sds edc_commands_append_file_section(sds out, const char *title, const sds list, size_t count)
{
    if (count == 0 || !list)
        return out;
    out = sdscatprintf(out, "### %s (%zu)\n", title, count);
    int n = 0;
    char **lines = sdssplitlen(list, (ssize_t)sdslen(list), "\n", 1, &n);
    for (int i = 0; lines && i < n; i++)
    {
        if (lines[i] && lines[i][0])
            out = sdscatprintf(out, "- `%s`\n", lines[i]);
    }
    if (lines)
        sdsfreesplitres(lines, n);
    return sdscat(out, "\n");
}

/* `/group files [id]`: the files the group's FOLDED work read / modified.
 *
 * What this shows is a snapshot taken at fold landing, not a live view: the lists cover
 * the turns that became memory, they do not advance afterwards, and `exec_shell` calls
 * (`sed -i`, `git checkout`, `cat`) were never tracked. Saying so in the output matters
 * more than the list itself -- a reader who takes it for `git status` will be wrong. */
sds edc_commands_group_files_text(struct edc_task_group *group)
{
    if (!group)
        return sdsnew("**Group** — no such group (the id is the one on its header).");

    sds reads = NULL;
    sds writes = NULL;
    edc_group_fold_tracked_files(group, &reads, &writes);
    const size_t nr = edc_commands_count_entries(reads);
    const size_t nw = edc_commands_count_entries(writes);

    sds out = sdscatprintf(sdsempty(), "## Tracked files in `%s`\n\n",
                           group->group_id ? group->group_id : "?");

    if (nr + nw == 0)
    {
        if (!group->memory || sdslen(group->memory) == 0)
            out = sdscat(out, "This group has never been folded, so nothing is tracked yet.\n");
        else
            out = sdscat(out, "No files recorded. Either this group was folded before the lists existed, or the "
                              "folded corpus touched no `read_file` / `list_dir` / `write_file` / `edit_file`.\n");
    }
    else
    {
        out = edc_commands_append_file_section(out, "Modified", writes, nw);
        out = edc_commands_append_file_section(out, "Read", reads, nr);
    }
    sdsfree(reads);
    sdsfree(writes);

    /* The caveat always ships with the list: a reader who takes it for `git status`
     * will be wrong, and the wrongness is not visible from the list itself. */
    out = sdscat(out, "_A snapshot taken when a fold landed, not a live view: later edits are not "
                      "reflected, and `exec_shell` calls (`sed -i`, `cat`, `git checkout`) are never tracked._\n");
    return out;
}

/* `/group history <id>`: a group's message history -- one block per member (the block
 * starts with its input, then its own messages, never interleaved across tasks). The
 * group id is the group head's id. */
sds edc_commands_group_info_text(struct edc_session *session, struct edc_task_group *group,
                                 const struct edc_config *conf)
{
    if (!group)
        return sdsnew("**Group** — no such group (the id is the one on its header).");

    bool estimated = false;
    long long ctx = edc_context_group_ctx_tokens(group, &estimated);
    int window = conf ? conf->context_window : 0;
    long long line = (long long)window - EDC_FOLD_RESERVE_TOKENS;

    struct edc_task *leader = group->leader;
    const char *place = (session && edc_scheduler_get_foreground(session) == group) ? "foreground" : "background";

    sds out = sdscatprintf(sdsempty(), "## Group `%s` (%s%s)\n\n", group->group_id, place,
                           group->is_orphaned ? ", orphaned" : "");
    out = sdscatprintf(out, "- **%d members** · **%zu foldable** · **%zu unfolded messages**\n", group->member_count,
                       edc_task_group_count_foldable(group), edc_task_group_unfolded_msg_count(group));
    out = sdscatprintf(out, "- **context** `%lld`%s\n", ctx,
                       estimated ? " (the tail after the last reply is estimated)" : " (provider-reported)");
    out = sdscatprintf(out, "- **window** `%d` · **fold line** `%lld` (= window − %d reserve)\n", window, line,
                       EDC_FOLD_RESERVE_TOKENS);
    if (window > 0)
        out = sdscatprintf(out, "- **verdict**: %s, %lld tokens %s the line%s\n",
                           edc_group_fold_due(group, conf) ? "due" : "not due", ctx > line ? ctx - line : line - ctx,
                           ctx > line ? "above" : "below",
                           edc_group_fold_due(group, conf) ? " — the next message folds it" : "");
    out = sdscatprintf(out, "- **fold epoch** %u · **memory** %s · **failure streak** %d/%d\n", group->fold_epoch,
                       (group->memory && sdslen(group->memory) > 0) ? "present" : "none (never folded)",
                       group->fold_failures, EDC_FOLD_MAX_FAILURES);
    {
        /* The tracked-file counts belong next to `memory`: they are part of it, and this
         * line is where someone looks to find out whether there is anything to see. */
        sds reads = NULL;
        sds writes = NULL;
        edc_group_fold_tracked_files(group, &reads, &writes);
        const size_t nr = edc_commands_count_entries(reads);
        const size_t nw = edc_commands_count_entries(writes);
        if (nr + nw > 0)
            out = sdscatprintf(out, "- **tracked files** %zu read · %zu modified (`/group files %s`)\n", nr, nw,
                               group->group_id ? group->group_id : "<id>");
        else if (group->memory && sdslen(group->memory) > 0)
            out = sdscatprintf(out, "- **tracked files** none recorded — this group was folded before the lists "
                                    "existed, or its corpus touched no readable/writable file\n");
        sdsfree(reads);
        sdsfree(writes);
    }
    out = sdscatprintf(out, "- **fold in flight** %s (one per session) · **barrier on this group** %s\n",
                       (session && edc_scheduler_has_pending_system_task(session)) ? "yes" : "no",
                       edc_group_fold_in_flight(group) ? "yes" : "no");
    if (leader)
        out = sdscatprintf(out, "- leader `%s`%s\n", leader->task_id,
                           (leader->description && *leader->description) ? leader->description : "");
    out = sdscatprintf(out, "\n_`/group fold` folds now, ignoring the line._\n");
    return out;
}

static void edc_commands_handle_group_info(struct edc_session_manager *sm, struct edc_scheduler *sched,
                                           struct edc_reply_to to, const char *id)
{
    struct edc_session *sess = edc_session_manager_get_active(sm);
    bool named = (id && *id);
    struct edc_task_group *g =
        named ? (sess ? edc_session_find_group(sess, id) : NULL) : (sess ? edc_scheduler_get_foreground(sess) : NULL);
    if (!g)
    {
        edc_commands_reply(to, named ? "**Group** — not found (the id is the one on its header)."
                                     : "**Group** — no foreground group; pass an id: `/group info <id>`.");
        return;
    }
    sds out = edc_commands_group_info_text(sess, g, sched ? sched->config : NULL);
    edc_commands_reply(to, out);
    sdsfree(out);
}

/* ── /group 的子命令 ─────────────────────────────────────────── */
/* ── /group 的子命令 ─────────────────────────────────────────── */
/* `/group fold [id] [focus]`: the id is the first word and the focus is the rest, joined
 * back with single spaces -- quoting already decided where the words broke, so
 * `/group fold 1 "make it short"` and `/group fold 1 make it short` are the same request
 * and a quoted phrase stays one word. */
static void sub_group_fold(struct edc_session_manager *sm, struct edc_scheduler *sched, struct edc_reply_to to,
                           struct edc_channel_message *message, const struct edc_sub_argv *args)
{
    (void)message;
    sds focus = sub_argv_tail(args, 1);
    edc_commands_handle_group_fold(sm, sched, to, args->argc > 0 ? args->argv[0] : NULL, focus);
    sdsfree(focus);
}

/* `[--json] <id>`: shared by `/group history` and `/task history`, because both take the
 * same two operands and the flag may stand anywhere among the words. */
static bool sub_history_operands(struct edc_reply_to to, const struct edc_sub_argv *args, const char *usage,
                                 const char **id, bool *want_json)
{
    *id = NULL;
    *want_json = false;
    for (int i = 0; i < args->argc; i++)
    {
        if (strcmp(args->argv[i], "--json") == 0)
            *want_json = true;
        else if (!*id)
            *id = args->argv[i];
        else
        {
            edc_commands_reply(to, usage);
            return false;
        }
    }
    if (!*id)
    {
        edc_commands_reply(to, usage);
        return false;
    }
    return true;
}

static void sub_group_list(struct edc_session_manager *sm, struct edc_scheduler *sched, struct edc_reply_to to,
                           struct edc_channel_message *message, const struct edc_sub_argv *args)
{
    (void)sched;
    (void)message;
    if (!sub_argv_exactly(to, args, 0, "Usage: `/group list`"))
        return;
    edc_commands_handle_group_list(sm, to);
}
static void sub_group_info(struct edc_session_manager *sm, struct edc_scheduler *sched, struct edc_reply_to to,
                           struct edc_channel_message *message, const struct edc_sub_argv *args)
{
    (void)message;
    if (!sub_argv_exactly_or_none(to, args, 1, "Usage: `/group info [id]`"))
        return;
    edc_commands_handle_group_info(sm, sched, to, args->argc > 0 ? args->argv[0] : NULL); /* NULL = foreground */
}
/* `/group files [id]`: the tracked-file lists, via the same lookup as `/group info`. */
static void sub_group_files(struct edc_session_manager *sm, struct edc_scheduler *sched, struct edc_reply_to to,
                            struct edc_channel_message *message, const struct edc_sub_argv *args)
{
    (void)sched;
    (void)message;
    if (!sub_argv_exactly_or_none(to, args, 1, "Usage: `/group files [id]`"))
        return;
    struct edc_session *sess = edc_session_manager_get_active(sm);
    const char *id = args->argc > 0 ? args->argv[0] : NULL;
    const bool named = (id && *id);
    struct edc_task_group *g =
        named ? (sess ? edc_session_find_group(sess, id) : NULL) : (sess ? edc_scheduler_get_foreground(sess) : NULL);
    if (!g)
    {
        edc_commands_reply(to, named ? "**Group** — not found (the id is the one on its header)."
                                     : "**Group** — no foreground group; pass an id: `/group files <id>`.");
        return;
    }
    sds out = edc_commands_group_files_text(g);
    edc_commands_reply(to, out);
    sdsfree(out);
}
static void sub_group_history(struct edc_session_manager *sm, struct edc_scheduler *sched, struct edc_reply_to to,
                              struct edc_channel_message *message, const struct edc_sub_argv *args)
{
    (void)message;
    const char *id = NULL;
    bool want_json = false;
    if (!sub_history_operands(to, args, "Usage: `/group history <id>` (append `--json` for the raw payload)", &id,
                              &want_json))
        return;
    edc_commands_handle_history(sm, sched, to, id, want_json, true);
}

static const struct edc_sub_argv_entry k_group_subs[] = {
    {"list", sub_group_list},
    {"info", sub_group_info},
    {"files", sub_group_files},
    {"history", sub_group_history},
    {"fold", sub_group_fold},
};

static bool edc_commands_handle_group(struct edc_config *cfg, struct edc_session_manager *sm,
                                      struct edc_scheduler *sched, struct edc_reply_to to,
                                      struct edc_channel_message *message, const struct edc_ast_cmd *cmd)
{
    (void)cfg;
    /* The words after `/group` are already split: `cmd->argv[0]` is the verb and the rest
     * are its operands. Nothing here looks at the raw line. */
    const struct edc_sub_argv args = {cmd->argv + 1, cmd->argc - 1};
    if (cmd->argc > 0)
    {
        for (size_t i = 0; i < sizeof(k_group_subs) / sizeof(k_group_subs[0]); i++)
        {
            if (strcmp(k_group_subs[i].verb, cmd->argv[0]) != 0)
                continue;
            k_group_subs[i].fn(sm, sched, to, message, &args);
            return true;
        }
        sds reply = sdscatprintf(sdsempty(),
                                 "**Unknown subcommand** — `/group` knows `list`, `info [id]`, `files [id]`, "
                                 "`history <id>` and `fold [id] [focus]`, not `%s`.",
                                 cmd->argv[0]);
        edc_commands_reply(to, reply);
        sdsfree(reply);
        return true;
    }
    edc_commands_reply(to, "Usage: `/group list`   (all groups of the session)\n\n"
                           "Usage: `/group info [id]`   (context / fold numbers; no id = the foreground group)\n\n"
                           "Usage: `/group files [id]`   (the files the folded work read / modified)\n\n"
                           "Usage: `/group history <id>`   (the id is the group header's)\n\n"
                           "Usage: `/group fold [id] [focus]`   (fold now, ignoring the trigger; "
                           "no id = the foreground group)");
    return true;
}

/* ── /task 的子命令 ─────────────────────────────────────────── */
/* /task dep <id> [--drop [<source>]]: show a task's dependency edges, or drop one.
 *
 * `--drop` is the door out of an edge that can never be satisfied: a cross-session
 * dependency whose owner session is gone keeps the task held forever (`/task resume`
 * answers "is held"), and a stopped source nobody will ever resume does the same.
 * Dropping is the user's explicit "stop waiting for it": the edge is voided (persisted,
 * like any other release), and the task runs as soon as that was the last thing it
 * waited for. */
static const char *edc_commands_dep_kind(edc_task_dep_type type)
{
    switch (type)
    {
    case TASK_DEP_JOIN:
        return "JOIN (`|`)";
    case TASK_DEP_SEQUENCE:
        return "SEQUENCE (`&&`)";
    case TASK_DEP_BARRIER:
        return "ORDER (`;`)";
    case TASK_DEP_UNLESS:
        return "INVERSE (`||`)";
    }
    return "DEP";
}

/* The outstanding edges of |t|, as one comma-separated list of ids (caller frees). */
static sds edc_commands_outstanding_deps(const struct edc_task *t)
{
    sds out = sdsempty();
    for (const struct edc_task_dep *dep = t->deps; dep; dep = dep->next)
    {
        if (dep->completed || dep->cancelled || !dep->source || !dep->source->task_id)
            continue;
        if (sdslen(out))
            out = sdscat(out, ", ");
        out = sdscatprintf(out, "`%s` (%s)", dep->source->task_id, edc_task_state_str(dep->source));
    }
    return out;
}

static void edc_commands_handle_task_dep(struct edc_session_manager *sm, struct edc_scheduler *sched,
                                         struct edc_reply_to to, const struct edc_sub_argv *args)
{
    /* "<id> [--drop [<source>]]" -- three words at most, already split. They are borrowed
     * from the AST, so nothing here is freed. */
    if (args->argc == 0)
    {
        edc_commands_reply(to, "**Task dep** — `/task dep <id>` lists the edges, `/task dep <id> --drop "
                               "[<source-id>]` stops waiting for one (or for all of them).");
        return;
    }
    if (args->argc > 3)
    {
        edc_commands_reply(to, "Usage: `/task dep <id> [--drop [<source-id>]]`");
        return;
    }
    const char *id = args->argv[0];
    const char *flag = args->argc > 1 ? args->argv[1] : NULL;
    const char *src = args->argc > 2 ? args->argv[2] : NULL;

    struct edc_task *t = id ? edc_session_store_resolve_task(sm, sched, id, NULL) : NULL;
    if (!t)
    {
        sds reply = sdscatprintf(sdsempty(), "Task not found: `%s`", id ? id : "");
        edc_commands_reply(to, reply);
        sdsfree(reply);
        return;
    }

    if (flag && strcmp(flag, "--drop") != 0)
    {
        sds reply = sdscatprintf(sdsempty(),
                                 "**Task dep** — unknown option `%s`. Use `/task dep %s --drop [<source-id>]`.", flag,
                                 t->task_id ? t->task_id : "");
        edc_commands_reply(to, reply);
        sdsfree(reply);
        return;
    }

    if (!flag)
    {
        /* Listing: the edges, their kind, and -- for the ones still outstanding -- what
         * the source is doing. Unresolved cross-session holds come last: they are the ones
         * `--drop` exists for. */
        size_t total = 0;
        for (const struct edc_task_dep *dep = t->deps; dep; dep = dep->next)
            total++;
        sds out = sdscatprintf(sdsempty(), "**Task `%s`** — %d dependency edge%s\n", t->task_id ? t->task_id : "?",
                               (int)total, total == 1 ? "" : "s");
        for (const struct edc_task_dep *dep = t->deps; dep; dep = dep->next)
        {
            const char *state = dep->cancelled ? "dropped" : dep->completed ? "done" : "outstanding";
            out = sdscatprintf(out, "- `%s` %s — %s",
                               (dep->source && dep->source->task_id) ? dep->source->task_id : "?",
                               edc_commands_dep_kind(dep->type), state);
            if (!dep->cancelled && !dep->completed && dep->source)
                out = sdscatprintf(out, " (%s)", edc_task_state_str(dep->source));
            out = sdscat(out, "\n");
        }
        {
            const struct edc_pending_dep *pd = NULL;
            while ((pd = edc_scheduler_next_pending_dep(sched, t->task_id, pd)) != NULL)
                out = sdscatprintf(out,
                                   "- `%s` — held: its owner session is not loaded "
                                   "(`/task dep %s --drop %s`)\n",
                                   pd->source_task_id ? pd->source_task_id : "?", t->task_id,
                                   pd->source_task_id ? pd->source_task_id : "?");
        }
        if (total == 0 && !edc_scheduler_first_unresolved_dep(sched, t))
            out = sdscat(out, "_No dependencies._\n");
        edc_commands_reply(to, out);
        sdsfree(out);
        return;
    }

    /* --drop: live edges first (that includes the "source is stopped and nobody will
     * resume it" case), then the unresolved cross-session constraint, which has no edge
     * row at all. */
    int dropped = 0;
    for (struct edc_task_dep *dep = t->deps; dep; dep = dep->next)
    {
        if (dep->completed || dep->cancelled || !dep->source)
            continue;
        if (src && (!dep->source->task_id || strcmp(dep->source->task_id, src) != 0))
            continue;
        if (edc_task_drop_dep(t, dep->source))
            dropped++;
    }
    dropped += edc_scheduler_drop_pending_dep(sched, t->task_id, src);

    if (dropped == 0)
    {
        sds reply = sdscatprintf(sdsempty(), "**Task `%s`** — nothing to drop%s%s%s.", t->task_id ? t->task_id : "",
                                 src ? " for `" : "", src ? src : "", src ? "`" : "");
        edc_commands_reply(to, reply);
        sdsfree(reply);
        return;
    }

    /* Re-decide and drive. A task that already ENDED because of this dependency
     * (end == BLOCKED) is the case this command exists for: with the edge gone there is
     * nothing left to wait for, so it gets what the automatic release gives it -- back to
     * dep-wait, then READY. resume_task is the one entry that respects the group queue and
     * any dependency that is still outstanding. */
    if (!edc_task_is_finished(t) || t->end == EDC_END_BLOCKED)
        edc_scheduler_resume_task(sched, t);

    sds reply = sdscatprintf(sdsempty(), "**Task `%s`** — dropped %d dependency edge%s", t->task_id ? t->task_id : "",
                             dropped, dropped == 1 ? "" : "s");
    /* Say what actually happened, in the task's own words: "ready" for a task that is
     * still BLOCKED would be a lie. */
    if (edc_task_is_running(t))
        reply = sdscat(reply, "; it is running now");
    else if (edc_task_awaits_deps(t))
    {
        sds still = edc_commands_outstanding_deps(t);
        if (sdslen(still))
            reply = sdscatprintf(reply, "; it still waits for %s", still);
        else
            reply = sdscat(reply, "; it is ready and the scan will run it");
        sdsfree(still);
    }
    else if (edc_task_is_ready(t))
        reply = sdscat(reply, "; it is ready and the scan will run it");
    else
        reply = sdscatprintf(reply, "; it is still %s (`/task resume %s` decides what next)",
                             edc_task_state_str(t), t->task_id ? t->task_id : "?");
    reply = sdscat(reply, ".");
    edc_commands_reply(to, reply);
    sdsfree(reply);
}

static void sub_task_dep(struct edc_session_manager *sm, struct edc_scheduler *sched, struct edc_reply_to to,
                         struct edc_channel_message *message, const struct edc_sub_argv *args)
{
    (void)message;
    edc_commands_handle_task_dep(sm, sched, to, args);
}
static void sub_task_new(struct edc_session_manager *sm, struct edc_scheduler *sched, struct edc_reply_to to,
                         struct edc_channel_message *message, const struct edc_sub_argv *args)
{
    edc_commands_handle_task_new(sm, sched, to, cmd_input_session(sm, message), args);
}
static void sub_task_fork(struct edc_session_manager *sm, struct edc_scheduler *sched, struct edc_reply_to to,
                          struct edc_channel_message *message, const struct edc_sub_argv *args)
{
    edc_commands_handle_task_fork(sm, sched, to, cmd_input_session(sm, message), args);
}
static void sub_task_list(struct edc_session_manager *sm, struct edc_scheduler *sched, struct edc_reply_to to,
                          struct edc_channel_message *message, const struct edc_sub_argv *args)
{
    (void)sched;
    (void)message;
    if (!sub_argv_exactly(to, args, 0, "Usage: `/task list`"))
        return;
    edc_commands_handle_task_list(sm, to);
}
static void sub_task_stop(struct edc_session_manager *sm, struct edc_scheduler *sched, struct edc_reply_to to,
                          struct edc_channel_message *message, const struct edc_sub_argv *args)
{
    (void)message;
    if (!sub_argv_exactly_or_none(to, args, 1, "Usage: `/task stop [id]`"))
        return;
    edc_commands_handle_task_stop(sm, sched, to, args->argc > 0 ? args->argv[0] : NULL); /* NULL = the one in front */
}
static void sub_task_resume(struct edc_session_manager *sm, struct edc_scheduler *sched, struct edc_reply_to to,
                            struct edc_channel_message *message, const struct edc_sub_argv *args)
{
    (void)message;
    if (!sub_argv_exactly(to, args, 1, "Usage: `/task resume <id>`, one task at a time"))
        return;
    edc_commands_handle_task_resume(sm, sched, to, args->argv[0]);
}
static void sub_task_status(struct edc_session_manager *sm, struct edc_scheduler *sched, struct edc_reply_to to,
                            struct edc_channel_message *message, const struct edc_sub_argv *args)
{
    (void)message;
    if (!sub_argv_exactly_or_none(to, args, 1, "Usage: `/task status [id]`"))
        return;
    edc_commands_handle_task_status(sm, sched, to, args->argc > 0 ? args->argv[0] : NULL);
}
static void sub_task_output(struct edc_session_manager *sm, struct edc_scheduler *sched, struct edc_reply_to to,
                            struct edc_channel_message *message, const struct edc_sub_argv *args)
{
    (void)message;
    if (!sub_argv_exactly(to, args, 1, "Usage: `/task output <id>`"))
        return;
    edc_commands_handle_task_output(sm, sched, to, args->argv[0]);
}
static void sub_task_history(struct edc_session_manager *sm, struct edc_scheduler *sched, struct edc_reply_to to,
                             struct edc_channel_message *message, const struct edc_sub_argv *args)
{
    (void)message;
    const char *id = NULL;
    bool want_json = false;
    if (!sub_history_operands(to, args, "Usage: `/task history <id>` (append `--json` for the raw payload)", &id,
                              &want_json))
        return;
    edc_commands_handle_history(sm, sched, to, id, want_json, false);
}
static void sub_task_fg(struct edc_session_manager *sm, struct edc_scheduler *sched, struct edc_reply_to to,
                        struct edc_channel_message *message, const struct edc_sub_argv *args)
{
    (void)sched;
    (void)message;
    if (!sub_argv_exactly(to, args, 1, "Usage: `/task fg <id>`"))
        return;
    edc_commands_handle_task_fg(sm, to, args->argv[0]);
}
static void sub_task_bg(struct edc_session_manager *sm, struct edc_scheduler *sched, struct edc_reply_to to,
                        struct edc_channel_message *message, const struct edc_sub_argv *args)
{
    (void)sched;
    (void)message;
    if (!sub_argv_exactly_or_none(to, args, 1, "Usage: `/task bg [id]`"))
        return;
    edc_commands_handle_task_bg(sm, to, args->argc > 0 ? args->argv[0] : NULL); /* NULL = the foreground group */
}

static const struct edc_sub_argv_entry k_task_subs[] = {
    {"new", sub_task_new},       {"fork", sub_task_fork},   {"list", sub_task_list},
    {"dep", sub_task_dep},
    {"stop", sub_task_stop},     {"resume", sub_task_resume}, {"status", sub_task_status},
    {"output", sub_task_output}, {"history", sub_task_history}, {"fg", sub_task_fg},
    {"bg", sub_task_bg},
};

static bool edc_commands_handle_task(struct edc_config *cfg, struct edc_session_manager *sm,
                                     struct edc_scheduler *sched, struct edc_reply_to to,
                                     struct edc_channel_message *message, const struct edc_ast_cmd *cmd)
{
    (void)cfg;
    /* The words after `/task`: the first is the verb, the rest are its operands. */
    const struct edc_sub_argv args = {cmd->argv + 1, cmd->argc - 1};
    if (cmd->argc == 0)
    {
        /* Bare `/task`: usage, not "unknown" (same shape as /group and /session). */
        edc_commands_reply(to, "Usage: `/task` knows `new <desc>`, `fork <parent> <desc>`, `list`, `status [id]`, "
                               "`output <id>`, `history <id>`, `fg <id>`, `bg [id]`, `stop [id]`, "
                               "`resume <id>`, `dep <id> [--drop [<source>]]`. Type `/help /task`.");
        return true;
    }
    for (size_t i = 0; i < sizeof(k_task_subs) / sizeof(k_task_subs[0]); i++)
    {
        if (strcmp(k_task_subs[i].verb, cmd->argv[0]) != 0)
            continue;
        k_task_subs[i].fn(sm, sched, to, message, &args);
        return true;
    }
    edc_commands_reply(to, "Unknown `/task` subcommand. Type `/help /task` for the subcommands.");
    return true;
}

/* ── Entry ─────────────────────────────────────────────────── */

/* ── /prompt ─────────────────────────────────────────────────
 *
 * The whole user-extensible surface is this ONE built-in: a template is reached as its
 * ARGUMENT, never as a name in the command namespace (docs/prompt-library.md §4,
 * invariant 1). Two shapes:
 *
 *     /prompt                    list
 *     /prompt <name> [args…]     load, substitute, then run the result as INPUT
 *
 * There is no reserved sub-command, so a template named `list` keeps working. */

/* The directory the SESSION is bound to -- the "project" whose resources we read. Empty
 * when there is no session; the resource lookup then falls back to the process cwd. */
static sds cmd_session_workdir(struct edc_session_manager *sm, const struct edc_channel_message *msg)
{
    struct edc_session *s = edc_session_store_resolve_session(sm, msg ? msg->session_id : NULL);
    const char *w = s ? edc_session_get_workdir(s) : NULL;
    return (w && *w) ? sdsnew(w) : sdsempty();
}

static void edc_commands_prompt_list(struct edc_config *cfg, const char *workdir, struct edc_reply_to to)
{
    /* Every resource root (ours first), so a template the user brought over from pi /
     * Claude Code / opencode shows up here too. */
    UT_array *roots = edc_resource_dirs(cfg, workdir);
    UT_array *prompts = edc_prompts_list_roots(roots, cfg);

    sds out;
    if (utarray_len(prompts) == 0)
    {
        sds dir = edc_path_join(cfg->workspace, "prompts", NULL);
        out = sdscatprintf(sdsempty(),
                           "**No prompts** — put a `.md` file in `%s` (or in a `.claude` / `.pi` / "
                           "`.opencode` prompts or command directory).",
                           dir);
        sdsfree(dir);
    }
    else
    {
        out = sdsnew("## Prompts\n\n");
        for (size_t i = 0; i < utarray_len(prompts); i++)
        {
            struct edc_prompt **slot = (struct edc_prompt **)utarray_eltptr(prompts, (unsigned)i);
            struct edc_prompt *p = *slot;
            sds hint = p->argument_hint ? sdscatprintf(sdsempty(), " %s", p->argument_hint) : sdsempty();
            out = sdscatprintf(out, "- `/prompt %s%s`%s%s\n", p->name, hint,
                               (p->description && sdslen(p->description)) ? " — " : "",
                               (p->description && sdslen(p->description)) ? p->description : "");
            sdsfree(hint);
            /* Name the root when it is not ours: "where did this come from" matters once
             * foreign directories are read. */
            if (p->source && cfg->workspace && strcmp(p->source, cfg->workspace) != 0)
                out = sdscatprintf(out, "  _(from `%s`)_\n", p->source);
            /* The same-named templates this one shadowed: reported so editing them does not
             * look like it had no effect. */
            if (p->shadowed)
            {
                for (int j = 0; p->shadowed[j]; j++)
                    out = sdscatprintf(out, "  _(shadowed: `%s`)_\n", p->shadowed[j]);
            }
        }
        out = sdscatprintf(out, "\nUsage: `/prompt <name> [args…]`\n");
    }
    edc_commands_reply(to, out);
    sdsfree(out);

    for (size_t i = 0; i < utarray_len(prompts); i++)
    {
        struct edc_prompt **slot = (struct edc_prompt **)utarray_eltptr(prompts, (unsigned)i);
        edc_prompt_free(*slot);
    }
    utarray_free(prompts);
    edc_resource_dirs_free(roots);
}

/* `/help [<command>]` 的表入口：取参这一步从派发链搬进来。 */
static bool edc_commands_handle_help_cmd(struct edc_config *cfg, struct edc_session_manager *sm,
                                         struct edc_scheduler *sched, struct edc_reply_to to,
                                         struct edc_channel_message *message, const struct edc_ast_cmd *cmd)
{
    (void)cfg;
    (void)sm;
    (void)sched;
    (void)message;
    /* `/help [command]`: one word after the verb, already split. More than one is a usage
     * error rather than silently helping about the first. */
    if (cmd->argc > 1)
    {
        edc_commands_reply(to, "Usage: `/help [command]`");
        return true;
    }
    return edc_commands_handle_help(to, cmd->argc == 1 ? cmd->argv[0] : NULL);
}

/* ── --new-group / --group <id> on a document command ────────────────
 *
 * /prompt and /skill hand their arguments to the template VERBATIM, so where the
 * placement options may stand has to be unambiguous. The rule (decided 2026-10-18):
 *
 *   - a `--…` WORD is an option WHEREVER it stands, before or after the template name;
 *   - a `--…` word that was QUOTED WHOLE (`"--strict"`) is text, so template arguments
 *     that start with `--` are still expressible -- quoting says "this is a literal word",
 *     which is exactly what it says everywhere else;
 *   - an unknown bare `--…` is an ERROR, never a template name or argument -- a typo must
 *     not silently end up inside the prompt text.
 *
 * Both spellings mean the same thing, so either muscle memory works:
 *
 *     /prompt --new-group review src/main.c
 *     /prompt review --new-group src/main.c
 */
struct cmd_place_flags
{
    bool new_group; /* --new-group: put the task in a group of its own */
    bool bg;        /* --bg: run it in the background, alongside (no queueing, no front) */
    sds group_id;   /* --group <id>: put it in that existing group */
};

/* ── `--new-group` / `--group <id>` / `--bg`: /prompt and /skill ─────────
 *
 * The same getopt rule the rest of the command layer uses -- an option is a WORD, and the
 * lexer already decided the words. One thing argv alone cannot say is whether a word was
 * QUOTED, so the AST carries `argv_quoted` alongside:
 *
 *     --strict      bare: option-shaped and not ours -> refused (a typo must not become
 *                   a template argument)
 *     "--strict"    one closed quoted region: exactly the text the user asked to pass on,
 *                   so it is an OPERAND, not an option
 *
 * Quoting therefore keeps its only job -- it says where a word begins and ends -- and one
 * extra bit says a word was quoted whole, which is what "this is text, not syntax" means.
 * |out_vec| borrows from |cmd| (caller frees the array, never the strings). */
static bool cmd_place_from_argv(struct edc_reply_to to, const char *cmd_name, const struct edc_ast_cmd *cmd,
                                struct cmd_place_flags *out, sds **out_vec, int *out_count)
{
    *out_vec = NULL;
    *out_count = 0;

    for (int i = 0; i < cmd->argc; i++)
    {
        const char *w = cmd->argv[i];
        const bool quoted = cmd->argv_quoted && cmd->argv_quoted[i];

        /* A quoted word is text, whatever it looks like. */
        if (!quoted && strcmp(w, "--bg") == 0)
        {
            out->bg = true;
            continue;
        }
        if (!quoted && strcmp(w, "--new-group") == 0)
        {
            out->new_group = true;
            continue;
        }
        if (!quoted && (strcmp(w, "--group") == 0 || strncmp(w, "--group=", 8) == 0))
        {
            const char *value = NULL;
            if (w[7] == '=')
                value = w + 8;
            else if (i + 1 < cmd->argc)
                value = cmd->argv[++i];
            if (!value || !*value)
            {
                sdsfree(out->group_id);
                out->group_id = NULL;
                /* Reported by the caller, which owns the wording for its own command. */
                return false;
            }
            sdsfree(out->group_id);
            out->group_id = sdsnew(value);
            continue;
        }
        if (!quoted && w[0] == '-' && w[1] == '-')
            return false; /* unknown option: the caller names it */

        *out_vec = realloc(*out_vec, sizeof(sds) * (size_t)(*out_count + 1));
        (*out_vec)[(*out_count)++] = (sds)w; /* borrowed from the AST */
    }
    return true;
}

/* Where does this task go? |*out| == NULL means "open a group of its own" (the contract
 * of edc_shell_spawn_task). false = the request cannot be honoured; |*err| is the reply. */
static bool cmd_resolve_place(struct edc_session *session, const struct cmd_place_flags *flags,
                              struct edc_task_group **out, sds *err)
{
    *out = NULL;

    if (flags->new_group && flags->group_id)
    {
        *err = sdsnew("`--new-group` and `--group <id>` are alternatives — pick one.");
        return false;
    }

    if (flags->new_group)
        return true; /* a group of its own */

    if (flags->group_id)
    {
        struct edc_task_group *group = session ? edc_session_find_group(session, flags->group_id) : NULL;
        if (!group)
        {
            *err = sdscatprintf(sdsempty(), "**No group** named `%s`.\n\nRun `/group list` to see them.",
                                flags->group_id);
            return false;
        }
        if (edc_task_group_is_system(group))
        {
            *err = sdsnew("`--group` cannot name the system group — it does not take new work.");
            return false;
        }
        /* A group whose leader left (or one that was born without a leader) has no
         * `leader`: adding a member to it would leave "leader == NULL with members",
         * which every other path assumes cannot happen -- the group is serialized with
         * the old leader's id and comes back from disk with a DIFFERENT leader object.
         * Refuse instead of half-reviving it. */
        if (group->is_orphaned || !group->leader)
        {
            *err = sdsnew("`--group` cannot name a conversation whose leader is gone — "
                          "start a fresh one with `--new-group`.");
            return false;
        }
        *out = group;
        return true;
    }

    struct edc_task_group *group = session ? session->foreground_group : NULL;
    if (group && (group->is_orphaned || group->member_count == 0 || edc_task_group_is_system(group)))
        group = NULL;
    *out = group;
    return true;
}

static bool edc_commands_handle_prompt(struct edc_config *cfg, struct edc_session_manager *sm,
                                       struct edc_scheduler *sched, struct edc_reply_to to,
                                       struct edc_channel_message *message, const struct edc_ast_cmd *cmd)
{
    /* The words after `/prompt`: options are this command's (the place flags), and the
     * operands are `[name] [template args…]`. All of it arrives already split. */
    struct cmd_place_flags flags = {0};
    sds *args = NULL;
    int argc = 0;
    if (!cmd_place_from_argv(to, "/prompt", cmd, &flags, &args, &argc))
    {
        sds reply = sdscatprintf(sdsempty(),
                                 "**Unknown option** — `/prompt` knows `--new-group`, `--group <id>` and `--bg`.\n\n"
                                 "Quote it (`\"--x\"`) if a template should receive it as an argument.");
        edc_commands_reply(to, reply);
        sdsfree(reply);
        if (flags.group_id)
            sdsfree(flags.group_id);
        free(args);
        return true;
    }

    if (argc == 0)
    {
        free(args);
        if (flags.group_id)
            sdsfree(flags.group_id);
        sds wd = cmd_session_workdir(sm, message);
        edc_commands_prompt_list(cfg, wd, to);
        sdsfree(wd);
        return true;
    }

    /* Search every resource root, ours first: a template the user brought over works
     * without being moved (and is translated if its dialect is `$`). */
    sds workdir = cmd_session_workdir(sm, message);
    UT_array *roots = edc_resource_dirs(cfg, workdir);
    sdsfree(workdir);
    struct edc_prompt *prompt = edc_prompt_load_roots(roots, args[0], cfg);
    edc_resource_dirs_free(roots);
    if (!prompt)
    {
        sds reply = sdscatprintf(sdsempty(), "**No prompt** named `%s`.\n\nRun `/prompt` to list them.",
                                 args[0]);
        edc_commands_reply(to, reply);
        sdsfree(reply);
        free(args); /* borrowed words: the AST owns the strings */
        if (flags.group_id)
            sdsfree(flags.group_id);
        return true;
    }

    sds expanded = edc_prompt_expand(prompt, args + 1, argc - 1);
    edc_prompt_free(prompt);
    free(args);

    if (sdslen(expanded) == 0)
    {
        edc_commands_reply(to, "**Prompt expanded to nothing** — nothing to run.");
        sdsfree(expanded);
        if (flags.group_id)
            sdsfree(flags.group_id);
        return true;
    }

    /* The body is a DOCUMENT, not a command line: it becomes ONE task's input, verbatim.
     * The shell never parses it -- the same treatment a skill body gets, and the same
     * shape as a `'''` block (one literal atom). So a body gets no `| ; & >`, and no
     * `{{…}}` task references; the template layer's own placeholders ($1..$N / $ARGUMENTS)
     * were substituted above, which is a different layer -- and `{{…}}` is NOT one of
     * them (that alias is gone; it is the input layer's task reference, so in a body it
     * is plain text).
     *
     * Its meaning must not depend on the READER's shell-like syntax setting: a template is
     * authored content, and an author who could not reproduce someone else's report would
     * be chasing a ghost. (This is also why a body containing `/prompt` stays harmless:
     * nothing in it is ever dispatched as a command.) */
    struct edc_session *session = cmd_input_session(sm, message);
    struct edc_task_group *group = NULL;
    sds place_err = NULL;
    if (!cmd_resolve_place(session, &flags, &group, &place_err))
    {
        edc_commands_reply(to, place_err);
        sdsfree(place_err);
        sdsfree(expanded); /* the body would leak */
        if (flags.group_id)
            sdsfree(flags.group_id);
        return true;
    }

    struct edc_cmd_task_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.input = expanded; /* ownership moves to |opts| */
    /* LABELLED by what the user typed (`/prompt review ars`), not by the body. */
    opts.bg = flags.bg;
    opts.desc = edc_str_utf8_truncate(message->content, EDC_TASK_DESC_MAX, true);

    struct edc_task_group *out_group = NULL;
    sds spawn_err = NULL;
    struct edc_task *task = edc_shell_spawn_task(sm, sched, to.type, session, &opts, NULL, group, "task", &out_group,
                                                NULL, false, NULL, &spawn_err);
    if (spawn_err)
    {
        edc_commands_reply(to, spawn_err);
        sdsfree(spawn_err);
    }
    if (task)
        edc_commands_announce_task(to, task);

    edc_cmd_task_opts_free(&opts); /* frees |input| too */
    if (flags.group_id)
        sdsfree(flags.group_id);
    return true;
}

/* ── /skill ──────────────────────────────────────────────────
 *
 * A skill is a directory of instructions (`<root>/skills/<name>/SKILL.md`) the model may
 * pick up on its own -- it sees name + description in the system prompt, then reads the
 * file. `/skill` is the EXPLICIT way in: `/skill <name> [args…]` loads the body and runs
 * it as this task's input, the same path `/prompt` takes (so a pipe or a redirect in the
 * body still works). pi / Claude Code spell this `/skill:name`; we keep our single-verb
 * style, `/prompt <name>` / `/skill <name>`.
 *
 * A skill whose front matter says `disable-model-invocation: true` is NOT in the system
 * prompt (the model must not reach for it), but it is still runnable from here. */

static void edc_commands_skill_list(struct edc_config *cfg, const char *workdir, struct edc_reply_to to)
{
    UT_array *roots = edc_resource_dirs(cfg, workdir);
    UT_array *skills = edc_skills_load_roots(roots, cfg);

    sds out;
    if (utarray_len(skills) == 0)
    {
        sds dir = edc_path_join(cfg->workspace, "skills", NULL);
        out = sdscatprintf(sdsempty(),
                           "**No skills** — a skill is `<dir>/<name>/SKILL.md`; ours live in `%s` (a `.claude` / "
                           "`.agents` / `.pi` / `.opencode` skills directory is read too).",
                           dir);
        sdsfree(dir);
    }
    else
    {
        out = sdsnew("## Skills\n\n");
        for (size_t i = 0; i < utarray_len(skills); i++)
        {
            struct edc_skill **slot = (struct edc_skill **)utarray_eltptr(skills, (unsigned)i);
            struct edc_skill *sk = slot ? *slot : NULL;
            if (!sk)
                continue;
            struct edc_skill_meta_entry *e = NULL;
            HASH_FIND_STR(sk->meta, "description", e);
            out = sdscatprintf(out, "- `/skill %s`%s%s\n", sk->name, (e && e->value && sdslen(e->value)) ? " — " : "",
                               (e && e->value && sdslen(e->value)) ? e->value : "");
            if (!sk->model_invocable)
                out = sdscat(out, "  _(user-only: `disable-model-invocation`)_\n");
            if (sk->source && cfg->workspace && strcmp(sk->source, cfg->workspace) != 0)
                out = sdscatprintf(out, "  _(from `%s`)_\n", sk->source);
            /* The copies that lost the name to this one: a user editing them would
             * otherwise never learn the change has no effect (pi's collision diagnostic). */
            if (sk->shadowed)
            {
                for (int j = 0; sk->shadowed[j]; j++)
                    out = sdscatprintf(out, "  _(shadowed: `%s`)_\n", sk->shadowed[j]);
            }
        }
        out = sdscat(out, "\nUsage: `/skill <name> [args…]`\n");
    }
    edc_commands_reply(to, out);
    sdsfree(out);

    for (size_t i = 0; i < utarray_len(skills); i++)
    {
        struct edc_skill **slot = (struct edc_skill **)utarray_eltptr(skills, (unsigned)i);
        if (slot)
            edc_skill_free(*slot);
    }
    utarray_free(skills);
    edc_resource_dirs_free(roots);
}

static bool edc_commands_handle_skill(struct edc_config *cfg, struct edc_session_manager *sm,
                                      struct edc_scheduler *sched, struct edc_reply_to to,
                                      struct edc_channel_message *message, const struct edc_ast_cmd *cmd)
{
    /* Same shape as `/prompt`: this command's place options, then `[name] [args…]`. */
    struct cmd_place_flags flags = {0};
    sds *args = NULL;
    int argc = 0;
    if (!cmd_place_from_argv(to, "/skill", cmd, &flags, &args, &argc))
    {
        sds reply = sdscatprintf(sdsempty(),
                                 "**Unknown option** — `/skill` knows `--new-group`, `--group <id>` and `--bg`.\n\n"
                                 "Quote it (`\"--x\"`) if the skill should receive it as an argument.");
        edc_commands_reply(to, reply);
        sdsfree(reply);
        if (flags.group_id)
            sdsfree(flags.group_id);
        free(args);
        return true;
    }

    if (argc == 0)
    {
        free(args);
        if (flags.group_id)
            sdsfree(flags.group_id);
        sds wd = cmd_session_workdir(sm, message);
        edc_commands_skill_list(cfg, wd, to);
        sdsfree(wd);
        return true;
    }

    /* Find it by name, roots in order; take it OUT of the array so the shared free below
     * cannot touch it. */
    sds workdir = cmd_session_workdir(sm, message);
    UT_array *roots = edc_resource_dirs(cfg, workdir);
    sdsfree(workdir);
    UT_array *skills = edc_skills_load_roots(roots, cfg);
    struct edc_skill *found = NULL;
    for (size_t i = 0; i < utarray_len(skills); i++)
    {
        struct edc_skill **slot = (struct edc_skill **)utarray_eltptr(skills, (unsigned)i);
        if (slot && *slot && strcmp((*slot)->name, args[0]) == 0)
        {
            found = *slot;
            *slot = NULL;
            break;
        }
    }
    for (size_t i = 0; i < utarray_len(skills); i++)
    {
        struct edc_skill **slot = (struct edc_skill **)utarray_eltptr(skills, (unsigned)i);
        if (slot)
            edc_skill_free(*slot);
    }
    utarray_free(skills);
    edc_resource_dirs_free(roots);

    if (!found)
    {
        sds reply = sdscatprintf(sdsempty(), "**No skill** named `%s`.\n\nRun `/skill` to list them.", args[0]);
        edc_commands_reply(to, reply);
        sdsfree(reply);
        free(args); /* borrowed words: the AST owns the strings */
        if (flags.group_id)
            sdsfree(flags.group_id);
        return true;
    }

    /* pi's shape: the body goes in wrapped in a <skill> block that names the file and the
     * directory its relative paths hang off -- otherwise "run scripts/foo.sh" leaves the
     * model guessing where that is. The user's arguments then land either in the body's
     * placeholders or, when it has none, appended after the closing tag (the rule in
     * edc_prompt_expand). |found| is the skill: it owns the strings and is freed below. */
    sds base_dir = edc_path_dirname(found->location ? found->location : "");
    sds wrapped = sdscatprintf(sdsempty(),
                               "<skill name=\"%s\" location=\"%s\">\nReferences are relative to %s.\n\n%s\n</skill>",
                               found->name ? found->name : "", found->location ? found->location : "",
                               base_dir ? base_dir : "", found->content ? found->content : "");
    sdsfree(base_dir);

    struct edc_prompt shell_body;
    memset(&shell_body, 0, sizeof(shell_body));
    shell_body.body = wrapped;
    sds input = edc_prompt_expand(&shell_body, args + 1, argc - 1);
    sdsfree(wrapped);
    edc_skill_free(found);
    free(args);

    if (sdslen(input) == 0)
    {
        edc_commands_reply(to, "**Skill has no body** — nothing to run.");
        sdsfree(input);
        if (flags.group_id)
            sdsfree(flags.group_id);
        return true;
    }

    /* Spawned DIRECTLY rather than through edc_shell_execute: a skill body is a DOCUMENT,
     * and the shell layer tokenizes its input -- `<` / `>` are operators there (`<` is
 * rejected outright now, since `@file` replaced it, but `>` still redirects),
     * so pi's `<skill …>` wrapper would be eaten by the tokenizer (and any `<tag>` in a
     * body silently mangled). */
    struct edc_session *session = cmd_input_session(sm, message);
    struct edc_task_group *group = NULL;
    sds place_err = NULL;
    if (!cmd_resolve_place(session, &flags, &group, &place_err))
    {
        edc_commands_reply(to, place_err);
        sdsfree(place_err);
        sdsfree(input); /* the body would leak */
        if (flags.group_id)
            sdsfree(flags.group_id);
        return true;
    }

    struct edc_cmd_task_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.input = input; /* ownership moves to |opts| */
    /* LABELLED by what the user typed (`/skill pdf extract pages`), not by the body. */
    opts.bg = flags.bg;
    opts.desc = edc_str_utf8_truncate(message->content, EDC_TASK_DESC_MAX, true);

    struct edc_task_group *out_group = NULL;
    sds spawn_err = NULL;
    struct edc_task *task = edc_shell_spawn_task(sm, sched, to.type, session, &opts, NULL, group, "task", &out_group,
                                                 NULL, false, NULL, &spawn_err);
    if (spawn_err)
    {
        edc_commands_reply(to, spawn_err);
        sdsfree(spawn_err);
    }
    /* A skill body is a DOCUMENT, exactly like a prompt body: ONE task's input, verbatim,
     * never parsed as a command line. So a `{{LAST}}` / `{{<id>}}` written in it stays
     * text -- the input layer's `{{…}}` collection belongs to lines the shell parses, not
     * to authored documents (see edc_commands_handle_prompt for the whole reasoning). */
    if (task)
        edc_commands_announce_task(to, task); /* the receipt: id / group / when it runs */

    edc_cmd_task_opts_free(&opts); /* frees |input| too */
    if (flags.group_id)
        sdsfree(flags.group_id);
    return true;
}

/* ── /reload ─────────────────────────────────────────────────
 *
 * The system prompt is built ONCE per group and then CACHED in `group->history[0]`
 * (every request references that node -- that constancy is what keeps the provider's
 * cache prefix stable). So a resource the user adds or edits on disk -- a skill, an
 * AGENTS.md, anything that ends up in the prompt -- would otherwise never take effect.
 *
 * `/reload` rebuilds it from disk and writes the new content back over history[0] for
 * every group of the session. The node itself is kept (its "content" is replaced in
 * place) so a request that is holding a reference to it cannot dangle.
 *
 * Prompts and skills are read from disk on every use already, so they need nothing; the
 * lazily-loaded model catalog IS dropped here so a regenerated one is re-read. */
static bool edc_commands_handle_reload(struct edc_config *cfg, struct edc_session_manager *sm,
                                       struct edc_scheduler *sched, struct edc_reply_to to,
                                       struct edc_channel_message *message, const struct edc_ast_cmd *cmd)
{
    (void)cmd;
    struct edc_session *session = cmd_input_session(sm, message);
    if (!session)
    {
        edc_commands_reply(to, "No session — nothing to reload.");
        return true;
    }

    const char *workdir = edc_session_get_workdir(session);
    size_t groups = 0;
    size_t changed = 0;

    struct edc_task_group *g;
    DL_FOREACH(session->groups, g)
    {
        /* A system group carries no system prompt (see edc_task_group.c), and a group
         * that never built one has nothing to replace. */
        if (g->kind == EDC_GROUP_KIND_SYSTEM || edc_task_group_history_count(g) == 0)
            continue;

        /* Slot 0 is the system prompt: an in-memory message, so this is the borrowed node
         * itself. /reload rewrites its content IN PLACE -- requests hold a reference to
         * this node, and replacing it would leave them pointing at the old prompt. */
        cJSON *slot = edc_task_group_history_owned_at(g, 0);
        cJSON *content = slot ? cJSON_GetObjectItem(slot, "content") : NULL;
        if (!slot || !content || !cJSON_IsString(content))
            continue;

        sds prompt = edc_context_build_system_prompt(cfg, workdir);
        if (!prompt)
            continue;

        if (strcmp(content->valuestring, prompt) != 0)
        {
            /* In place: the node stays, only its content changes. */
            cJSON_ReplaceItemInObject(slot, "content", cJSON_CreateString(prompt));
            changed++;
        }
        groups++;
        sdsfree(prompt);
    }

    /* Forget the lazily-loaded catalog so the next use re-reads it (EDGECLAW_MODELS_CATALOG). */
    edc_models_reset();

    sds reply = sdscatprintf(sdsempty(),
                             "**Reloaded** — system prompt rebuilt from disk for %zu group%s (%zu changed). "
                             "The model catalog was dropped (re-read on next use). Prompts and skills are "
                             "already read on every use. `config.json` is NOT re-read — restart for that.",
                             groups, groups == 1 ? "" : "s", changed);
    edc_commands_reply(to, reply);
    sdsfree(reply);
    return true;
}

/* ── 命令表：表驱动派发 ────────────────────────────────────────
 *
 * 一条命令 = 一行数据：名字、匹配方式、入口。派发 = 查一次表，于是：
 *   - 没有"插入顺序决定谁先匹配"这回事；
 *   - 匹配方式（整行相等 / 前缀 + 空格）写在行里，不用在 is_command 与 is_target 之间挑；
 *   - 表可以被测试直接读到（见 test_commands_help 的防漂移断言）。
 *
 * 子命令也是查表，且**每张表**都吃解析后的词（`struct edc_sub_argv`）—— 没有第二套签名了。 */
typedef bool (*edc_cmd_fn)(struct edc_config *cfg, struct edc_session_manager *sm,
                           struct edc_scheduler *sched, struct edc_reply_to to,
                           struct edc_channel_message *message, const struct edc_ast_cmd *cmd);


/* ── /jobs — what is scheduled ───────────────────────────────
 *
 * The schedules are DATA (<workspace>/jobs.json), owned by the crontab tool; the ticking
 * is a background service. Until now only the model could touch them: a person could not
 * see what was going to run, remove a stale entry, or run one now. This family is that
 * window. It reads and writes the same file through the guarded file helpers, and `run`
 * injects the job's text into the input pipeline exactly the way the ticker does -- so
 * "run now" and "the timer fired" produce the same task.
 */
#define EDC_JOBS_MAX_BYTES (4 * 1024 * 1024)

static sds jobs_file_path(struct edc_config *cfg)
{
    return edc_path_join(cfg && cfg->workspace ? cfg->workspace : "", "jobs.json", NULL);
}

/* NULL with |*error| set when the file exists but cannot be used. A MISSING file is not
 * an error: it means "nothing scheduled yet". */
static cJSON *jobs_load(struct edc_config *cfg, EdcError **error)
{
    sds path = jobs_file_path(cfg);
    struct edc_path_rules rules = {
        .root = cfg ? cfg->workspace : NULL,
        .refuse_symlink = true,
        .deny_paths = (cfg && cfg->enable_sandbox) ? cfg->sandbox_deny_paths : NULL,
    };
    sds content = edc_path_guard_read(&rules, path, EDC_JOBS_MAX_BYTES, error);
    bool missing = !content && !edc_file_is_regular(path);
    sdsfree(path);
    if (!content)
    {
        if (missing)
        {
            if (error)
                *error = NULL;
            return cJSON_CreateObject(); /* nothing scheduled */
        }
        return NULL;
    }
    cJSON *db = cJSON_Parse(content);
    sdsfree(content);
    if (!db)
    {
        edc_error_set(error, EDC_ERR_INVAL, "jobs.json is not valid JSON");
        return NULL;
    }
    if (!cJSON_GetObjectItem(db, "jobs"))
        cJSON_AddArrayToObject(db, "jobs");
    return db;
}

static bool jobs_save(struct edc_config *cfg, cJSON *db, sds *err)
{
    sds path = jobs_file_path(cfg);
    char *text = cJSON_Print(db);
    EdcError *error = NULL;
    const bool ok = text && edc_file_write_safe(path, text, (ssize_t)strlen(text), cfg->workspace, &error);
    if (!ok && err)
        *err = sdsnew(error && error->message ? error->message : "cannot write jobs.json");
    if (error)
        edc_error_free(error);
    if (text)
        cJSON_free(text);
    sdsfree(path);
    return ok;
}

/* Host-local wall clock (the schedules without an IANA tz are host-local). */
static bool jobs_localtime(time_t t, struct tm *out)
{
    return edc_localtime_r(&t, out) != NULL;
}

/* "2026-10-19 09:00 (in 2h 13m)" -- the number a person needs; the raw unix timestamp
 * alone was unreadable and did not show which zone it was in. */
static sds jobs_describe_next(const cJSON *schedule)
{
    const cJSON *ts = schedule ? cJSON_GetObjectItem(schedule, "next_run_at_unix_ts") : NULL;
    if (!ts || !cJSON_IsNumber(ts))
        return sdsnew("not scheduled");

    const time_t at = (time_t)ts->valuedouble;
    struct tm tm;
    char buf[32];
    if (!jobs_localtime(at, &tm) || strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm) == 0)
        snprintf(buf, sizeof(buf), "%ld", (long)at);

    const char *tz = cJSON_GetStringValue(cJSON_GetObjectItem(schedule, "tz"));
    sds when = sdsnew(buf);
    const long delta = (long)(at - time(NULL));
    if (delta <= 0)
        return sdscatprintf(when, " (due now)%s%s", (tz && *tz) ? " " : "", (tz && *tz) ? tz : "");

    const long d = delta / 86400, h = (delta % 86400) / 3600, m = (delta % 3600) / 60;
    sds rel = sdsempty();
    if (d > 0)
        rel = sdscatprintf(rel, "%ldd %ldh", d, h);
    else if (h > 0)
        rel = sdscatprintf(rel, "%ldh %ldm", h, m);
    else
        rel = sdscatprintf(rel, "%ldm", m);
    when = sdscatprintf(when, " (in %s)%s%s", rel, (tz && *tz) ? " " : "", (tz && *tz) ? tz : "");
    sdsfree(rel);
    return when;
}

static sds jobs_describe_schedule(const cJSON *schedule)
{
    const char *kind = schedule ? cJSON_GetStringValue(cJSON_GetObjectItem(schedule, "kind")) : NULL;
    if (!kind)
        return sdsnew("?");

    if (strcmp(kind, "cron") == 0)
    {
        const char *expr = cJSON_GetStringValue(cJSON_GetObjectItem(schedule, "cron_expr"));
        return sdscatprintf(sdsempty(), "cron `%s`", expr ? expr : "?");
    }
    if (strcmp(kind, "every") == 0)
    {
        const cJSON *every = cJSON_GetObjectItem(schedule, "every_seconds");
        const long secs = (every && cJSON_IsNumber(every)) ? (long)every->valuedouble : 0;
        if (secs > 0 && secs % 3600 == 0)
            return sdscatprintf(sdsempty(), "every %ldh", secs / 3600);
        if (secs > 0 && secs % 60 == 0)
            return sdscatprintf(sdsempty(), "every %ldm", secs / 60);
        return sdscatprintf(sdsempty(), "every %lds", secs);
    }
    return sdsnew("one-shot"); /* "at" */
}

/* The job whose id starts with |prefix|. |*matches| counts how many did: 0 = none,
 * >1 = ambiguous (ids are uuids, so a short prefix is enough in practice but a wrong one
 * must not silently pick a job). */
static cJSON *jobs_by_prefix(cJSON *jobs, const char *prefix, int *matches)
{
    *matches = 0;
    cJSON *found = NULL;
    if (!jobs || !prefix || !*prefix)
        return NULL;
    const int n = cJSON_GetArraySize(jobs);
    for (int i = 0; i < n; i++)
    {
        cJSON *job = cJSON_GetArrayItem(jobs, i);
        const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(job, "job_id"));
        if (id && strncmp(id, prefix, strlen(prefix)) == 0)
        {
            (*matches)++;
            found = job;
        }
    }
    return *matches == 1 ? found : NULL;
}

static sds jobs_short_id(const cJSON *job)
{
    const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(job, "job_id"));
    return sdsnewlen(id ? id : "", (id && strlen(id) > 8) ? 8 : (id ? strlen(id) : 0));
}

static void sub_jobs_list(struct edc_session_manager *sm, struct edc_scheduler *sched, struct edc_reply_to to,
                          struct edc_channel_message *message, const struct edc_sub_argv *args)
{
    (void)sm;
    (void)message;
    if (!sub_argv_exactly(to, args, 0, "Usage: `/jobs list`"))
        return;
    struct edc_config *cfg = sched ? sched->config : NULL;

    EdcError *error = NULL;
    cJSON *db = jobs_load(cfg, &error);
    if (!db)
    {
        sds reply = sdscatprintf(sdsempty(), "**Cannot read the scheduled jobs** — %s",
                                 error && error->message ? error->message : "unknown error");
        edc_commands_reply(to, reply);
        sdsfree(reply);
        if (error)
            edc_error_free(error);
        return;
    }

    cJSON *jobs = cJSON_GetObjectItem(db, "jobs");
    const int n = cJSON_GetArraySize(jobs);
    if (n == 0)
    {
        edc_commands_reply(to, "**No scheduled jobs.**\n\nAsk the assistant to set one up; it uses the `crontab` tool.");
        cJSON_Delete(db);
        return;
    }

    sds reply = sdscatprintf(sdsempty(), "**Scheduled jobs** (%d)\n\n", n);
    for (int i = 0; i < n; i++)
    {
        cJSON *job = cJSON_GetArrayItem(jobs, i);
        const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(job, "name"));
        cJSON *schedule = cJSON_GetObjectItem(job, "schedule");
        sds id = jobs_short_id(job);
        sds kind = jobs_describe_schedule(schedule);
        sds next = jobs_describe_next(schedule);
        reply = sdscatprintf(reply, "`%s`  **%s**  · %s\n      next: %s\n", id, name ? name : "(unnamed)", kind, next);
        sdsfree(id);
        sdsfree(kind);
        sdsfree(next);
    }
    reply = sdscat(reply, "\n`/jobs run <id>` fires one now · `/jobs rm <id>` removes it");
    edc_commands_reply(to, reply);
    sdsfree(reply);
    cJSON_Delete(db);
}

static void sub_jobs_run(struct edc_session_manager *sm, struct edc_scheduler *sched, struct edc_reply_to to,
                         struct edc_channel_message *message, const struct edc_sub_argv *args)
{
    (void)sm;
    struct edc_config *cfg = sched ? sched->config : NULL;
    if (!sub_argv_exactly(to, args, 1, "Usage: `/jobs run <id>`   (the id is the one `/jobs` prints)"))
        return;
    const char *arg = args->argv[0];

    EdcError *error = NULL;
    cJSON *db = jobs_load(cfg, &error);
    if (!db)
    {
        sds reply = sdscatprintf(sdsempty(), "**Cannot read the scheduled jobs** — %s",
                                 error && error->message ? error->message : "unknown error");
        edc_commands_reply(to, reply);
        sdsfree(reply);
        if (error)
            edc_error_free(error);
        return;
    }

    int matches = 0;
    cJSON *job = jobs_by_prefix(cJSON_GetObjectItem(db, "jobs"), arg, &matches);
    if (!job)
    {
        sds reply = matches == 0 ? sdscatprintf(sdsempty(), "**No job** whose id starts with `%s`.", arg)
                                 : sdscatprintf(sdsempty(), "**Ambiguous id** `%s` — %d jobs match; type more of it.",
                                                arg, matches);
        edc_commands_reply(to, reply);
        sdsfree(reply);
        cJSON_Delete(db);
        return;
    }

    const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(job, "name"));
    const char *instruction = cJSON_GetStringValue(cJSON_GetObjectItem(job, "message"));
    sds text = edc_crontab_task_message(name, instruction);

    /* Exactly what the ticker does: the text enters the input pipeline as a user message.
     * The caller's session is used, so "run now" lands where the person is looking. */
    struct edc_channel_message *cm =
        edc_channel_message_new(edc_channel_adapter, EDC_CHANNEL_MESSAGE_USER_TYPE, text);
    if (message && message->session_id)
        cm->session_id = sdsnew(message->session_id);
    edc_actor_send(edc_scheduler_get_actor(sched),
                   edc_actor_msg_new(BC_ACTOR_MSG_CHANNEL_RECV, NULL, NULL, edc_channel_input_new(cm),
                                     (edc_actor_data_free_fn)edc_channel_input_free));

    sds reply = sdscatprintf(sdsempty(), "**Job `%s` fired** — its instruction went in as a message; "
                                         "the schedule itself was not changed.",
                             name ? name : arg);
    edc_commands_reply(to, reply);
    sdsfree(reply);
    cJSON_Delete(db);
}

static void sub_jobs_rm(struct edc_session_manager *sm, struct edc_scheduler *sched, struct edc_reply_to to,
                        struct edc_channel_message *message, const struct edc_sub_argv *args)
{
    (void)sm;
    (void)message;
    struct edc_config *cfg = sched ? sched->config : NULL;
    if (!sub_argv_exactly(to, args, 1, "Usage: `/jobs rm <id>`   (the id is the one `/jobs` prints)"))
        return;
    const char *arg = args->argv[0];

    EdcError *error = NULL;
    cJSON *db = jobs_load(cfg, &error);
    if (!db)
    {
        sds reply = sdscatprintf(sdsempty(), "**Cannot read the scheduled jobs** — %s",
                                 error && error->message ? error->message : "unknown error");
        edc_commands_reply(to, reply);
        sdsfree(reply);
        if (error)
            edc_error_free(error);
        return;
    }

    cJSON *jobs = cJSON_GetObjectItem(db, "jobs");
    int matches = 0;
    cJSON *job = jobs_by_prefix(jobs, arg, &matches);
    if (!job)
    {
        sds reply = matches == 0 ? sdscatprintf(sdsempty(), "**No job** whose id starts with `%s`.", arg)
                                 : sdscatprintf(sdsempty(), "**Ambiguous id** `%s` — %d jobs match; type more of it.",
                                                arg, matches);
        edc_commands_reply(to, reply);
        sdsfree(reply);
        cJSON_Delete(db);
        return;
    }

    const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(job, "name"));
    sds removed_name = sdsnew(name ? name : arg);

    /* The pointer came out of this array, so its index is what DeleteItemFromArray wants
     * -- deleting position 0 would remove somebody else's job. */
    int index = -1;
    const int count = cJSON_GetArraySize(jobs);
    for (int i = 0; i < count; i++)
    {
        if (cJSON_GetArrayItem(jobs, i) == job)
        {
            index = i;
            break;
        }
    }
    if (index < 0)
    {
        edc_commands_reply(to, "**Could not remove the job** — it vanished while reading the file.");
        sdsfree(removed_name);
        cJSON_Delete(db);
        return;
    }
    cJSON_DeleteItemFromArray(jobs, index);

    sds err = NULL;
    if (!jobs_save(cfg, db, &err))
    {
        sds reply = sdscatprintf(sdsempty(), "**Could not remove the job** — %s", err ? err : "write failed");
        edc_commands_reply(to, reply);
        sdsfree(reply);
        if (err)
            sdsfree(err);
        cJSON_Delete(db);
        sdsfree(removed_name);
        return;
    }

    sds reply = sdscatprintf(sdsempty(), "**Removed** job `%s` (`%s`).", removed_name, arg);
    edc_commands_reply(to, reply);
    sdsfree(reply);
    sdsfree(removed_name);
    cJSON_Delete(db);
}

static const struct edc_sub_argv_entry k_jobs_subs[] = {
    {"list", sub_jobs_list},
    {"run", sub_jobs_run},
    {"rm", sub_jobs_rm},
};

static bool edc_commands_handle_jobs(struct edc_config *cfg, struct edc_session_manager *sm,
                                     struct edc_scheduler *sched, struct edc_reply_to to,
                                     struct edc_channel_message *message, const struct edc_ast_cmd *cmd)
{
    (void)cfg;
    const struct edc_sub_argv args = {cmd->argv + 1, cmd->argc - 1};
    if (cmd->argc > 0)
    {
        for (size_t i = 0; i < sizeof(k_jobs_subs) / sizeof(k_jobs_subs[0]); i++)
        {
            if (strcmp(k_jobs_subs[i].verb, cmd->argv[0]) != 0)
                continue;
            k_jobs_subs[i].fn(sm, sched, to, message, &args);
            return true;
        }
        sds reply = sdscatprintf(sdsempty(),
                                 "**Unknown subcommand** — `/jobs` knows `list`, `run <id>` and `rm <id>`, not `%s`.",
                                 cmd->argv[0]);
        edc_commands_reply(to, reply);
        sdsfree(reply);
        return true;
    }

    /* Bare `/jobs` lists: it is the question people actually have. */
    const struct edc_sub_argv none = {NULL, 0};
    sub_jobs_list(sm, sched, to, message, &none);
    return true;
}

struct edc_cmd_entry
{
    const char *name; /* 不含斜杠 */
    bool exact;       /* true = 整行必须等于 `/name`；false = 允许 `/name …` */
    edc_cmd_fn fn;
};

static const struct edc_cmd_entry k_commands[] = {
    {"prompt", false, edc_commands_handle_prompt},
    {"skill", false, edc_commands_handle_skill},
    {"reload", true, edc_commands_handle_reload},
    {"task", false, edc_commands_handle_task},
    {"group", false, edc_commands_handle_group},
    {"jobs", false, edc_commands_handle_jobs},
    {"session", false, edc_commands_handle_session},
    {"help", false, edc_commands_handle_help_cmd},
};

#define EDC_CMD_COUNT (sizeof(k_commands) / sizeof(k_commands[0]))

/* One lexer + one parser for a `/command` line: the AST's cmd node carries the name
 * and the already-unquoted argv, so nothing below re-splits the raw text. The lexer
 * runs with the shell switch OFF -- inside a command line `|`, `&`, `--bg` … are
 * ordinary arguments, which is what a command line always meant.
 *
 * Returns false when it already replied: an unterminated quote (the parser's message
 * names the column) or a line that does not start with a command name at all. */
static bool edc_commands_parse_line(struct edc_reply_to to, const char *content, edc_ast *ast)
{
    memset(ast, 0, sizeof(*ast));

    edc_lex_out toks;
    if (!edc_lex(content, EDC_LEX_SHELL_OFF, &toks))
        return false; /* OOM: leave it to the unknown-command reply */

    bool bad_quote = false;
    for (int i = 0; i < toks.count; i++)
        if (toks.toks[i].kind == TK_BAD_QUOTED)
            bad_quote = true;

    sds err = NULL;
    const bool ok = edc_parse(&toks, ast, &err);
    edc_lex_out_free(&toks);
    if (ok)
        return true;

    if (bad_quote)
    {
        sds reply = sdscatprintf(sdsempty(), "**Syntax error** — %s", err ? err : "unterminated quote");
        edc_commands_reply(to, reply);
        sdsfree(reply);
    }
    else
    {
        /* `/…` that the grammar cannot start a command from (`/@x`): name what the user
         * typed rather than answering a bare "syntax error". */
        const char *end = content;
        while (*end && *end != ' ' && *end != '\t')
            end++;
        sds reply = sdscatprintf(sdsempty(), "**Unknown command** — `%.*s`\n\nType `/help` for available commands.",
                                 (int)(end - content), content);
        edc_commands_reply(to, reply);
        sdsfree(reply);
    }
    sdsfree(err);
    return false;
}

/* 按名字派发：名字与参数都由文法给出（edc_ast_cmd），这里不再切原文。
 * false = 这张表里没有它（或它不接受参数而参数不为空）。 */
static bool edc_commands_dispatch(struct edc_config *cfg, struct edc_session_manager *sm,
                                  struct edc_scheduler *sched, struct edc_reply_to to,
                                  const struct edc_ast_cmd *cmd, struct edc_channel_message *message)
{
    if (!cmd || !cmd->name)
        return false;

    const bool alone = (cmd->argc == 0);
    for (size_t i = 0; i < EDC_CMD_COUNT; i++)
    {
        const struct edc_cmd_entry *e = &k_commands[i];
        if (strcmp(e->name, cmd->name) != 0)
            continue;
        if (e->exact && !alone)
            return false; /* `/reload x` 不是 `/reload` */
        return e->fn(cfg, sm, sched, to, message, cmd);
    }
    return false;
}

int edc_commands_table_names(const char **out, int cap)
{
    int n = 0;
    for (size_t i = 0; i < EDC_CMD_COUNT && n < cap; i++)
        out[n++] = k_commands[i].name;
    return n;
}

edc_in_kind edc_commands_input_kind(const struct edc_config *cfg, const char *text, sds *err)
{
    if (err)
        *err = NULL;
    if (!text || !*text)
        return EDC_IN_MESSAGE;

    /* The two dispatch prefixes. Neither is the parser's vocabulary -- `!` never enters
     * the shell layer at all (one token, one exec_shell task) and `/` belongs to the
     * command layer -- so the switch, which only selects the LEXER's vocabulary, has no
     * say here. */
    if (text[0] == '!')
        return EDC_IN_LOCALSHELL;
    if (text[0] == '/')
        return EDC_IN_COMMAND;

    /* Everything else is the shell layer's call, decided by the same two calls
     * edc_shell_execute makes, in the same order: lex in the switch's mode, then classify
     * the tokens (a pure function -- see edc_classify). The lexer has a catch-all rule,
     * so a false here means "not our input": treat it as a message rather than inventing
     * a fifth kind. */
    edc_lex_out toks;
    if (!edc_lex(text, (cfg && cfg->shell_syntax) ? EDC_LEX_SHELL_ON : EDC_LEX_SHELL_OFF, &toks))
        return EDC_IN_MESSAGE;
    const edc_in_kind kind = edc_classify(&toks, err);
    edc_lex_out_free(&toks);
    return kind;
}

bool edc_commands_handler(struct edc_config *cfg, struct edc_session_manager *sm, struct edc_scheduler *sched,
                          struct edc_reply_to to, struct edc_channel_message *message)
{
    /* The validity gate checks only the message itself, not message->type -- historically
     * edc_channel_cli==0 made CLI messages look "untyped" and drop them wholesale (type
     * is a routing key, not a validity flag). */
    if (!message || !message->content)
        return false;

    /* Where this line goes is decided ONCE, by the predicate the UI also reads
     * (edc_commands_input_kind). The comment on that function is the whole rule; the
     * short version: `!` and `/` are dispatch prefixes (never gated), everything else is
     * the shell layer's call.
     *
     * `+desc` is SHELL syntax and is handled in the shell layer (edc_shell_execute), which
     * the fall-through below reaches. It deliberately does NOT combine with the command
     * syntax: `+/task new …` / `+!cmd` used to run the command in a new group, and that is
     * gone -- the two syntaxes are separate, and a line is one or the other. */
    const edc_in_kind kind = edc_commands_input_kind(cfg, message->content, NULL);

    /* `!cmd`: a user-typed local shell command. It becomes a task of its own that issues
     * one `exec_shell` call and settles on its result -- no model involved, and the
     * output lands in the group's history like any other task's. */
    if (kind == EDC_IN_LOCALSHELL && edc_local_shell_handle(sm, sched, to, message))
        return true;

    /* / commands: ONE lex + ONE parse, then the AST decides -- the builtins and the
     * table-driven handlers both read the name and argv from it, so no layer below
     * splits the raw text again. */
    if (kind == EDC_IN_COMMAND)
    {
        edc_ast ast;
        if (!edc_commands_parse_line(to, message->content, &ast))
            return true;

        const struct edc_ast_cmd *cmd = ast.cmd;
        const bool handled =
            cmd && cmd->name &&
            (edc_shell_handle_builtin(cfg, sm, to, cmd) ||
             edc_commands_dispatch(cfg, sm, sched, to, cmd, message));
        if (!handled)
        {
            sds reply =
                sdscatprintf(sdsempty(), "**Unknown command** — `%s`\n\nType `/help` for available commands.",
                             cmd && cmd->name ? cmd->name : message->content);
            edc_commands_reply(to, reply);
            sdsfree(reply);
        }
        edc_ast_free(&ast);
        return true;
    }

    /* non-/ input: the shell layer decides. It owns the shell-like syntax switch (checked
     * at ITS entry, so every door into the parser is covered -- see edc_shell_execute),
     * and it owns the `@` file-reference layer, which is deliberately outside that
     * switch. */
    return edc_shell_execute(sm, sched, to, message);
}

/* Port: the command layer provides handle_command. It reads config and the session
 * store straight off the scheduler, and replies to the sender of the inbound
 * message. `input->origin` is the opaque original channel message. */
static bool edc_commands_cb_handle_command(struct edc_scheduler *sched, struct edc_actor *actor,
                                           const struct edc_sched_input *input)
{
    struct edc_channel_message *msg = input ? input->origin : NULL;
    if (!msg)
        return false;

    /* The inbound message's sender is the channel actor -- reply straight back to it. */
    struct edc_reply_to to = {.channel_actor = actor, .actor = actor, .type = msg->type};
    return edc_commands_handler(sched->config, sched->sessions, sched, to, msg);
}

void edc_commands_bind_scheduler(struct edc_scheduler *sched)
{
    if (!sched)
        return;

    sched->handle_command = edc_commands_cb_handle_command;
}
