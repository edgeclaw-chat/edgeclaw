#include <cJSON.h>
#include "edc_commands.h"
#include "edc_actor.h"
#include "edc_session_store.h"
#include "edc_channel_actor.h"
#include "edc_cmd_parse.h"
#include "edc_compat.h"
#include "edc_scheduler.h"
#include "edc_session.h"
#include "edc_session_memory.h"
#include "edc_session_manager.h"
#include "edc_shell.h"
#include "edc_state_serializer.h"
#include "edc_task.h"
#include "edc_turn.h"
#include "edc_task_group.h"
#include "sds.h"
#include "utlist_fix.h"
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

static bool edc_commands_arg(const char *content, const char *prefix, sds *out)
{
    sds cmd = sdsnew(content);
    cmd = sdstrim(cmd, " \t\r\n");
    size_t plen = strlen(prefix);
    if (strncmp(cmd, prefix, plen) != 0)
    {
        sdsfree(cmd);
        return false;
    }
    const char *arg = cmd + plen;
    while (*arg == ' ')
        arg++;
    if (*arg == '\0')
    {
        sdsfree(cmd);
        *out = NULL;
        return false;
    }
    *out = sdsnew(arg);
    sdsfree(cmd);
    return true;
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

/* ── /session stop ─────────────────────────────────────────── */

/* Cancel every non-terminal turn in the current session and persist. Was the
 * top-level /stop; now /session with no arguments. */
static bool edc_commands_handle_session_stop( struct edc_session_manager *sm, struct edc_scheduler *sched,
                                             struct edc_reply_to to)
{
    edc_session_store_stop_all_turns(sm, sched);

    edc_commands_reply(to, "**Session** — stop signal sent to all running turns.");
    return true;
}

/* ── /help ─────────────────────────────────────────────────── */

static bool edc_commands_handle_help(struct edc_reply_to to)
{
    const char *text = "## EdgeClaw Commands\n"
                       "\n"
                       "- /help\n"
                       "- /session new [name] [dir]  ·  /session switch <name>\n"
                       "- /session list  ·  /session info  ·  /session status [id]\n"
                       "- /session save  ·  /session stop\n"
                       "- /session memo [show [id] | search <q> | compact [focus]]\n"
                       "- /session remove <name>  ·  /session rename <old> <new>\n"
                       "\n"
                       "- /task new <desc>   [--bg] [--new-group]\n"
                       "- /task fork <parent> <desc> [--prompt <name>]\n"
                       "- /task list  ·  /task status [id]  ·  /task output <id>\n"
                       "- /task history <id>   (its messages; `--json` = raw payload)\n"
                       "- /group history <id>  (the whole group; one block per member)\n"
                       "- /group fold [id] [focus]  (fold now, ignoring the trigger)\n"
                       "- /task groups  ·  /task fg <id>  ·  /task bg [id]\n"
                       "- /task stop [id]  ·  /task resume [id|--all]   (default: active task of the foreground group)\n"
                       "- /task chain <src> <dst>  ·  /task join <id>\n"
                       "\n"
                       "- /whoami\n"
                       "- /model [name]\n"
                       "- /connect   (configure the LLM provider)\n"
                       "- /sandbox [on|off]   (enable/disable the filesystem sandbox)\n"
                       "- /sandbox list   ·  /sandbox add-ro|rm-ro <dir>  ·  /sandbox add-rw|rm-rw <dir>\n"
                       "- /sandbox add-deny|rm-deny <dir>   (mask a dir: wins over ro/rw; e.g. ~/.ssh)\n"
                       "\n"
                       "## Shell Syntax\n"
                       "\n"
                       "- `@desc` — new group + task\n"
                       "- `desc` — task in current group\n"
                       "- `a | b` — pipe chain\n"
                       "- `cmd &` — background\n"
                       "- `> file` `>> file` — redirect output\n"
                       "- `< file` — redirect input\n"
                       "- `a ; b` — sequential execution\n"
                       "- `$LAST` `$1` `$$` — variable expansion\n";

    edc_commands_reply(to, text);
    return true;
}

/* ── /session save ─────────────────────────────────────────── */

/* Persist the current session's history + state. Was the top-level /save; now under /session. */
static bool edc_commands_handle_session_save( struct edc_session_manager *sm, struct edc_scheduler *sched,
                                             struct edc_reply_to to)
{
    struct edc_session *active = edc_session_manager_get_active(sm);
    if (!active)
    {
        edc_commands_reply(to, "**Session** — no active session.");
        return true;
    }

    edc_session_save_state(active, sched);
    edc_commands_reply(to, "**Session saved** — history and state persisted.");
    log_info("receive /session save command");
    return true;
}

/* ── /session memo ─────────────────────────────────────────── */

/* Viewing and manual curation of L2 session memory. Scope = the ACTIVE session (the
 * command layer always speaks to the current session; the kernel-side tools are the ones
 * scoped "by task owner"). */
static bool edc_commands_handle_session_memo(struct edc_session_manager *sm, struct edc_scheduler *sched,
                                             struct edc_reply_to to, const char *args)
{
    struct edc_session *active = edc_session_manager_get_active(sm);
    if (!active || !active->memo)
    {
        edc_commands_reply(to, "**Session memory** — no active session.");
        return true;
    }

    sds rest = args ? sdsnew(args) : sdsnew("");
    sdstrim(rest, " ");
    /* Subcommand = the first token; the rest is verbatim (focus / a query may contain spaces) */
    char *sp = strchr(rest, ' ');
    sds sub = sp ? sdsnewlen(rest, (size_t)(sp - rest)) : sdsdup(rest);
    const char *tail = sp ? sp + 1 : "";
    while (*tail == ' ')
        tail++;

    if (sdslen(sub) == 0 || strcmp(sub, "show") == 0)
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
    else if (strcmp(sub, "compact") == 0)
    {
        struct edc_task *t = edc_session_memory_start_manual(sched, active, (*tail ? tail : NULL));
        if (t)
            edc_commands_reply(to, "**Session memory** — curation started (it writes entries through its memory "
                                   "tools). `/session memo show` when it is done.");
        else if (edc_scheduler_has_pending_system_task(active))
            edc_commands_reply(to, "**Session memory** — a system task is already running in this session; "
                                   "wait for it to finish.");
        else
            edc_commands_reply(to, "**Session memory** — could not start curation.");
    }
    else
    {
        edc_commands_reply(to,
                           "Usage: `/session memo [show [id] | search <keyword> | compact [focus]]`\n"
                           "`show` lists the entries, `show <id>` prints one entry, `search` matches title/"
                           "summary/detail, `compact` curates now (else it only runs when a group is folded).");
    }

    sdsfree(sub);
    sdsfree(rest);
    return true;
}

/* ── /session commands ─────────────────────────────────────── */

/* Split "/session new" args into an optional name and an optional working
 * directory ("<name> [dir]"); the directory may contain spaces. Either may be
 * NULL; callers free both. */
static void edc_commands_split_name_dir(const char *rest, sds *out_name, sds *out_dir)
{
    *out_name = NULL;
    *out_dir = NULL;
    if (!rest)
        return;

    const char *p = rest;
    while (*p == ' ' || *p == '\t')
        p++;
    if (*p == '\0')
        return;

    const char *name_start = p;
    while (*p && *p != ' ' && *p != '\t')
        p++;
    *out_name = sdsnewlen(name_start, (size_t)(p - name_start));

    while (*p == ' ' || *p == '\t')
        p++;
    if (*p == '\0')
        return;

    const char *dir_start = p;
    const char *dir_end = p + strlen(p);
    while (dir_end > dir_start && (dir_end[-1] == ' ' || dir_end[-1] == '\t'))
        dir_end--;
    if (dir_end > dir_start)
        *out_dir = sdsnewlen(dir_start, (size_t)(dir_end - dir_start));
}

/* The shared tail of switching the active session: tell the frontend which
 * session is now displayed. Since ①b snapshots are pulled, this must be said
 * explicitly. Only sent when the session really changed, after the reply, so the
 * "Switched" text lands in the old session's transcript. */
static void cmd_notify_if_active_session_changed( struct edc_session_manager *sm, struct edc_actor *channel_actor,
                                                 struct edc_session *before)
{
    struct edc_session *now = edc_session_manager_get_active(sm);
    if (now == before)
        return;
    edc_channel_notify_active_session(channel_actor, now ? now->session_id : NULL);
}

static bool edc_commands_handle_session_new(struct edc_session_manager *sm, struct edc_reply_to to, sds arg)
{
    /* create() switches active to the new session, so take the old one first. */
    struct edc_session *old = edc_session_manager_get_active(sm);
    sds name = NULL, dir = NULL;
    edc_commands_split_name_dir(arg, &name, &dir);

    /* A given working directory is resolved up front against the process cwd and
     * must exist; otherwise the whole command fails. */
    if (dir && sdslen(dir) > 0)
    {
        EdcError *werr = NULL;
        sds resolved = edc_session_resolve_workdir(NULL, dir, &werr);
        if (!resolved)
        {
            sds reply =
                sdscatprintf(sdsempty(), "**Session** — failed: %s", werr ? werr->message : "invalid directory");
            edc_commands_reply(to, reply);
            sdsfree(reply);
            edc_error_free(werr);
            sdsfree(name);
            sdsfree(dir);
            return true;
        }
        sdsfree(dir);
        dir = resolved;
    }

    struct edc_session *sess = edc_session_manager_create_ex(sm, name, dir);
    sdsfree(name);
    if (!sess)
    {
        sds reply = sdscatprintf(sdsempty(), "**Session** — failed to create session%s%s", dir ? " bound to " : "",
                                 dir ? dir : "");
        edc_commands_reply(to, reply);
        sdsfree(reply);
        sdsfree(dir);
        return true;
    }
    const char *bound = edc_session_get_workdir(sess);
    sdsfree(dir);
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
    return true;
}

static bool edc_commands_handle_session_switch( struct edc_session_manager *sm, struct edc_scheduler *sched,
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
        edc_session_save_state(old, sched);
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
    return true;
}

static bool edc_commands_handle_session_list(struct edc_session_manager *sm, struct edc_reply_to to)
{
    sds list = edc_session_manager_list(sm);
    edc_commands_reply(to, list);
    sdsfree(list);
    return true;
}

static bool edc_commands_handle_session_remove( struct edc_session_manager *sm, struct edc_scheduler *sched,
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
    sdsfree(stopped_id);
    sdsfree(stopped_name);
    return true;
}

static bool edc_commands_handle_session_rename(struct edc_session_manager *sm, struct edc_reply_to to, sds arg)
{
    if (!arg || sdslen(arg) == 0)
    {
        edc_commands_reply(to, "Usage: `/session rename <old_name> <new_name>`");
        return true;
    }
    int count = 0;
    sds *tokens = sdssplitlen(arg, sdslen(arg), " ", 1, &count);
    if (count < 2)
    {
        sdsfreesplitres(tokens, count);
        edc_commands_reply(to, "Usage: `/session rename <old_name> <new_name>`");
        return true;
    }
    bool ok = edc_session_manager_rename(sm, tokens[0], tokens[1]);
    if (!ok)
    {
        sds reply = sdscatprintf(sdsempty(), "Session not found: `%s`", tokens[0]);
        edc_commands_reply(to, reply);
        sdsfree(reply);
        sdsfreesplitres(tokens, count);
        return true;
    }
    sds reply = sdscatprintf(sdsempty(), "**Session renamed** — `%s` → `%s`", tokens[0], tokens[1]);
    edc_commands_reply(to, reply);
    sdsfree(reply);
    sdsfreesplitres(tokens, count);
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
                     sess->session_id, edc_session_get_name(sess), sess->created_at ? sess->created_at : "unknown",
                     sess->last_active ? sess->last_active : "unknown", edc_session_get_message_count(sess),
                     sess->history_dir ? sess->history_dir : "",
                     edc_session_get_workdir(sess) ? edc_session_get_workdir(sess) : "(default)");
    edc_commands_reply(to, info);
    sdsfree(info);
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

static bool edc_commands_handle_session( struct edc_session_manager *sm, struct edc_scheduler *sched,
                                        struct edc_reply_to to, struct edc_channel_message *msg)
{
    sds rest = NULL;
    /* The name is optional: match exactly first, or a bare "/session new" falls through to unknown subcommand. */
    if (edc_commands_is_target(msg->content, "/session new"))
        return edc_commands_handle_session_new(sm, to, NULL);
    if (edc_commands_arg(msg->content, "/session new", &rest))
    {
        bool ret = edc_commands_handle_session_new(sm, to, rest);
        sdsfree(rest);
        return ret;
    }
    if (edc_commands_arg(msg->content, "/session switch", &rest))
    {
        bool ret = edc_commands_handle_session_switch(sm, sched, to, rest);
        sdsfree(rest);
        return ret;
    }
    if (edc_commands_is_target(msg->content, "/session list"))
        return edc_commands_handle_session_list(sm, to);
    if (edc_commands_is_target(msg->content, "/session info"))
        return edc_commands_handle_session_info(sm, to);
    if (edc_commands_is_target(msg->content, "/session status"))
        return edc_commands_handle_session_status(sm, to, NULL);
    if (edc_commands_arg(msg->content, "/session status", &rest))
    {
        bool ret = edc_commands_handle_session_status(sm, to, rest);
        sdsfree(rest);
        return ret;
    }
    if (edc_commands_is_target(msg->content, "/session save"))
        return edc_commands_handle_session_save(sm, sched, to);
    if (edc_commands_is_target(msg->content, "/session memo"))
        return edc_commands_handle_session_memo(sm, sched, to, NULL);
    if (edc_commands_arg(msg->content, "/session memo", &rest))
    {
        bool ret = edc_commands_handle_session_memo(sm, sched, to, rest);
        sdsfree(rest);
        return ret;
    }
    if (edc_commands_is_target(msg->content, "/session stop"))
        return edc_commands_handle_session_stop(sm, sched, to);
    if (edc_commands_arg(msg->content, "/session remove", &rest))
    {
        bool ret = edc_commands_handle_session_remove(sm, sched, to, rest);
        sdsfree(rest);
        return ret;
    }
    if (edc_commands_is_target(msg->content, "/session remove"))
        return edc_commands_handle_session_remove(sm, sched, to, NULL);
    if (edc_commands_arg(msg->content, "/session rename", &rest))
    {
        bool ret = edc_commands_handle_session_rename(sm, to, rest);
        sdsfree(rest);
        return ret;
    }
    if (edc_commands_is_target(msg->content, "/session rename"))
        return edc_commands_handle_session_rename(sm, to, NULL);
    /* /session stop takes no argument -- removing a session moved to /session remove. */
    if (edc_commands_is_prefix(msg->content, "/session stop"))
    {
        edc_commands_reply(
              to,
            "`/session stop` takes no arguments. To remove a session use `/session remove <name_or_id>`.");
        return true;
    }
    sds reply =
        sdscatprintf(sdsempty(), "**Unknown `/session` subcommand.** Try: `new`, `switch`, `list`, `info`, `status`, "
                                 "`save`, `stop`, `remove`, `rename`.");
    edc_commands_reply(to, reply);
    sdsfree(reply);
    return true;
}

/* ── /task commands ─────────────────────────────────────────― */

/* The session this input belongs to, as declared by the input itself; an empty
 * label means the active session, an unknown one rejects the command. */
static struct edc_session *cmd_input_session(struct edc_session_manager *sm, struct edc_channel_message *msg)
{
    return edc_session_store_resolve_session(sm, msg ? msg->session_id : NULL);
}

static void edc_commands_handle_task_new( struct edc_session_manager *sm, struct edc_scheduler *sched,
                                         struct edc_reply_to to, struct edc_session *session, sds arg)
{
    if (!session)
        return;
    /* Same rule as the `@` form: options are only recognised after a quoted
     * description, so the shape that would silently swallow them into the description
     * text is rejected rather than quietly creating a task nobody meant. */
    if (edc_cmd_desc_needs_quotes(arg))
    {
        edc_commands_reply(to, "Syntax error — `/task new` with options needs its description quoted: "
                               "write `/task new \"<description>\" --bg`, or drop the options.");
        return;
    }
    struct edc_cmd_task_opts opts;
    edc_cmd_parse_task_opts(arg, &opts);

    /* A task needs something to run. With no text there is nothing to attach, and a READY
     * task without a turn is never drivable: it just sits in the group forever (and would
     * be picked as the foreground group's "current task" by /task stop, /task status and
     * `$$`). Same rule as the shell path. */
    if (!opts.input || sdslen(opts.input) == 0)
    {
        edc_commands_reply(to, "A task needs an input — write `/task new \"<desc>\"` "
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
    if (opts.prompt_name)
        edc_task_set_prompt(task, opts.prompt_name, NULL);

    if (opts.desc && sdslen(opts.desc) > 0 && edc_task_awaits_deps(task))
        log_info("/task new: task %s waits for its dependencies, turn queued", task->task_id);

    edc_cmd_task_opts_free(&opts);
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
            bool is_active = edc_session_manager_get_active(sm) &&
                             edc_session_manager_get_active(sm)->active_task == t;
            out = sdscatprintf(out, "  - %s `%s` **%s**", t == g->leader ? "`[L]`" : "`[M]`", t->task_id,
                               edc_task_state_str(t));
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

static void edc_commands_handle_task_stop( struct edc_session_manager *sm, struct edc_scheduler *sched,
                                          struct edc_reply_to to, sds arg)
{
    struct edc_session *sess = edc_session_manager_get_active(sm);
    struct edc_task *t = NULL;

    if (!arg || sdslen(arg) == 0)
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
        t = edc_session_store_resolve_task(sm, sched, arg, NULL);
        if (!t)
        {
            sds reply = sdscatprintf(sdsempty(), "Task not found: `%s`", arg);
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

/* /task resume [id]: re-decide and drive a resumable task. Accepts INTERRUPTED
 * (the loop stopped mid-flight), SUSPENDED (the user parked it),
 * ERROR (continue the failed round, reusing the active turn and tool ledger), and
 * CANCELLED (cancel only stopped it; the step is re-derived from the messages).
 * Without an id it uses the foreground group's active task. */
static void edc_commands_handle_task_resume( struct edc_session_manager *sm, struct edc_scheduler *sched,
                                            struct edc_reply_to to, sds arg)
{
    struct edc_session *sess = edc_session_manager_get_active(sm);
    struct edc_task *t = NULL;

    if (arg && strcmp(arg, "--all") == 0)
    {
        /* Batch revive, for after a restart (which stops every unfinished task): re-decide
         * each stopped task of this session. The order does not matter -- a task whose
         * dependency is stopped too parks in dep-wait and is released when that one
         * produces, and the message-end scan drives it then. */
        edc_session_store_reconcile_pending_deps(sm, sched);
        int resumed = 0;
        struct edc_task_group *g;
        DL_FOREACH(sess ? sess->groups : NULL, g)
        {
            struct edc_task *m;
            LL_FOREACH2(g->members, m, group_next)
            {
                if (!edc_task_is_resumable(m))
                    continue;
                edc_scheduler_resume_task(sched, m);
                resumed++;
            }
        }
        sds reply = sdscatprintf(sdsempty(),
                                 "**Resumed** — %d task(s) re-decided. Ones whose dependency is still stopped "
                                 "wait for it.",
                                 resumed);
        edc_commands_reply(to, reply);
        sdsfree(reply);
        return;
    }

    if (!arg || sdslen(arg) == 0)
    {
        /* No id: the last member of the foreground group that is not COMPLETED. A
         * stopped task (INTERRUPTED / SUSPENDED / ERROR / CANCELLED) is what one wants
         * to revive here,
         * so this is wider than "the one still in play" (see the header). */
        t = edc_scheduler_resumable_task(sess);
        if (!t)
        {
            edc_commands_reply(to, edc_scheduler_get_foreground(sess)
                                     ? "**Task** — nothing to resume in the foreground group."
                                     : "**Task** — no foreground group.");
            return;
        }
    }
    else
    {
        t = edc_session_store_resolve_task(sm, sched, arg, NULL);
        if (!t)
        {
            sds reply = sdscatprintf(sdsempty(), "Task not found: `%s`", arg);
            edc_commands_reply(to, reply);
            sdsfree(reply);
            return;
        }
    }

    /* The resumable kinds: INTERRUPTED, SUSPENDED, ERROR, CANCELLED (only COMPLETED
     * and ALIVE are not). */
    if (!edc_task_is_resumable(t))
    {
        sds reply =
            sdscatprintf(sdsempty(), "**Task `%s`** — %s, not resumable.", t->task_id, edc_task_state_str(t));
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
            waiting = sdscatprintf(waiting, "`%s` (%s)", dep->source->task_id,
                                   edc_task_state_str(dep->source));
        }
        sds reply = sdscatprintf(sdsempty(),
                                 "**Task `%s` is waiting** — it still depends on %s. Resume that one (this task "
                                 "runs once its output arrives), or remove it.",
                                 t->task_id, sdslen(waiting) ? waiting : "an unfinished dependency");
        if (t->group && t->group->session && t->group->session != sess)
            reply = sdscatprintf(reply, " (session `[%s]`)", t->group->session->session_id);
        edc_commands_reply(to, reply);
        sdsfree(reply);
        sdsfree(waiting);
        return;
    }

    sds reply =
        sdscatprintf(sdsempty(), "**Task `%s`** — resume signal sent (%s).", t->task_id, edc_task_state_str(t));
    if (t->group && t->group->session && t->group->session != sess)
        reply = sdscatprintf(reply, " (session `[%s]`)", t->group->session->session_id);
    edc_commands_reply(to, reply);
    sdsfree(reply);
}

/* A task in a non-active session can be referenced globally (dependency, status,
 * stop/resume), but fg/bg/fork must not silently touch it. Returns true when |t|
 * is in the active session, otherwise replies with an error. */
static bool edc_commands_task_in_active_session( struct edc_session_manager *sm, struct edc_reply_to to,
                                                struct edc_task *t)
{
    struct edc_session *active = edc_session_manager_get_active(sm);
    struct edc_session *owner = (t->group && t->group->session) ? t->group->session : active;
    if (owner == active)
        return true;
    sds reply = sdscatprintf(sdsempty(),
                             "Task `%s` belongs to session `[%s]` — switch to it first (`/session switch %s`).",
                             t->task_id, owner->session_id, owner->session_id);
    edc_commands_reply(to, reply);
    sdsfree(reply);
    return false;
}

static void edc_commands_handle_task_fg(struct edc_session_manager *sm, struct edc_reply_to to, sds arg)
{
    if (!arg)
    {
        edc_commands_reply(to, "Usage: `/task fg <id>`");
        return;
    }
    struct edc_task *t = edc_session_manager_find_task_any(sm, arg, NULL);
    if (!t)
    {
        edc_commands_reply(to, "Task not found.");
        return;
    }
    if (!edc_commands_task_in_active_session(sm, to, t))
        return;
    struct edc_task_group *g =
        edc_session_find_group(edc_session_manager_get_active(sm), t->group_id);
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

static void edc_commands_handle_task_bg(struct edc_session_manager *sm, struct edc_reply_to to, sds arg)
{
    if (arg)
    {
        struct edc_task *t = edc_session_manager_find_task_any(sm, arg, NULL);
        if (!t)
        {
            edc_commands_reply(to, "Task not found.");
            return;
        }
        if (!edc_commands_task_in_active_session(sm, to, t))
            return;
        struct edc_task_group *g =
            edc_session_find_group(edc_session_manager_get_active(sm), t->group_id);
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

static void edc_commands_handle_task_fork( struct edc_session_manager *sm, struct edc_scheduler *sched,
                                          struct edc_reply_to to, struct edc_session *session, sds arg)
{
    if (!session)
        return;
    /* /task fork <parent_id> <desc> [--prompt <name>] */
    if (!arg)
    {
        edc_commands_reply(to,
                           "Usage: `/task fork <parent_id> <desc> [--prompt <name>]`");
        return;
    }

    int count = 0;
    sds *tokens = sdssplitlen(arg, sdslen(arg), " ", 1, &count);
    if (count < 2)
    {
        edc_commands_reply(to,
                           "Usage: `/task fork <parent_id> <desc> [--prompt <name>]`");
        sdsfreesplitres(tokens, count);
        return;
    }

    /* `session` is a parameter: the session THIS INPUT belongs to (resolved from the
     * message's tag -- no more "ask the active session and assume it is this input's
     * home"). */
    struct edc_task *parent = edc_session_manager_find_task_any(sm, tokens[0], NULL);
    if (!parent)
    {
        sds reply = sdscatprintf(sdsempty(), "Parent task not found: `%s`", tokens[0]);
        edc_commands_reply(to, reply);
        sdsfree(reply);
        sdsfreesplitres(tokens, count);
        return;
    }
    if (!edc_commands_task_in_active_session(sm, to, parent))
    {
        sdsfreesplitres(tokens, count);
        return;
    }

    /* find --prompt in remaining args; the rest is the input */
    sds prompt_name = NULL;
    sds desc = sdsempty();
    for (int i = 1; i < count; i++)
    {
        if (strcmp(tokens[i], "--prompt") == 0 && i + 1 < count)
        {
            prompt_name = sdsnew(tokens[i + 1]);
            i++; /* skip prompt value */
        }
        else
        {
            if (sdslen(desc) > 0)
                desc = sdscat(desc, " ");
            desc = sdscat(desc, tokens[i]);
        }
    }
    sdsfreesplitres(tokens, count);

    size_t fork_point = parent->group ? (size_t)cJSON_GetArraySize(parent->group->history) : 0;
    /* The fork point is expressed as a commit-order watermark: the max completion_seq among the parent group's committed members. */
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
    if (prompt_name)
        edc_task_set_prompt(child, prompt_name, NULL);

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
        if (prompt_name)
            sdsfree(prompt_name);
        return;
    }

    /* The child group inherits the parent's transcript snapshot as a synthetic
     * initial turn. */
    struct edc_task_group *child_group = child->group;

    if (parent->group)
    {
        /* inherit the parent group's memory summary */
        if (parent->group->memory)
            child_group->memory = sdsdup(parent->group->memory);

        /* The system prompt takes history[0] first, so the snapshot's first message is not mistaken for the system prompt by cb_send_llm */
        cJSON *sys = cJSON_GetArrayItem(parent->group->history, 0);
        if (sys)
            cJSON_AddItemToArray(child_group->history, cJSON_Duplicate(sys, 1));

        /* Synthesize a turn: deep-copy the parent group's unfolded messages
         * [1 .. fork_point) -- folded messages are not in history (pruned at landing). */
        size_t snap_begin = 1;
        struct edc_turn *snap = NULL;

        if (edc_task_has_folded_turns(parent))
        {
            /* Rule 2: the parent's turns were folded by L1, so recover the parent's own
             * full turns to keep the context at the fork point intact. Earlier folded
             * history still relies only on the memory summary. */
            snap = edc_task_snapshot_turns(parent, session, child->task_id);
        }
        else
        {
            /* Rule 1: the unfolded span [1+folded_count, fork_point) is the snapshot */
            snap = edc_turn_new();
            snap->task_id = strdup(child->task_id);
            if (snap_begin > fork_point)
                snap_begin = fork_point;
            for (size_t i = snap_begin; i < fork_point; i++)
            {
                cJSON *src = cJSON_GetArrayItem(parent->group->history, i);
                if (!src)
                    break;
                cJSON *msg = cJSON_Duplicate(src, 1);
                edc_queue_push(&snap->messages, msg); /* the turn owns it */
            }
        }

        if (snap && edc_queue_length(&snap->messages) > 0)
        {
            /* Reference the snapshot messages into the child group history (no double ownership) */
            for (size_t mj = snap->messages.head; mj < snap->messages.tail; mj++)
                cJSON_AddItemReferenceToArray(child_group->history, snap->messages.data[mj]);
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
    if (prompt_name)
        reply = sdscatprintf(reply, " · prompt `%s`", prompt_name);
    edc_commands_reply(to, reply);
    sdsfree(reply);
    sdsfree(desc);
    if (prompt_name)
        sdsfree(prompt_name);
}

static void edc_commands_handle_task_groups(struct edc_session_manager *sm, struct edc_reply_to to)
{
    sds out = sdsnew("## Groups\n\n");
    struct edc_task_group *g;
    struct edc_session *sess = edc_session_manager_get_active(sm);
    DL_FOREACH(sess ? sess->groups : NULL, g)
    {
        out = sdscatprintf(out, "- %s%s  **%s**  %d members  _%s_\n",
                           edc_scheduler_get_foreground(sess) == g ? "`[fg]` " : "",
                           g->is_orphaned ? "`[orphan]` " : "", g->group_id, g->member_count,
                           edc_task_group_status_label(g));
    }
    edc_commands_reply(to, out);
    sdsfree(out);
}

static void edc_commands_handle_task_status( struct edc_session_manager *sm, struct edc_scheduler *sched,
                                            struct edc_reply_to to, sds arg)
{
    struct edc_task *t;
    struct edc_session *owner = NULL;
    if (arg)
    {
        /* Global lookup: a task in another session can be inspected too. */
        t = edc_session_store_resolve_task(sm, sched, arg, &owner);
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
    if (t->prompt_name)
    {
        out = sdscatprintf(out, "- **prompt**: %s\n", t->prompt_name);
    }
    if (t->forked_from)
    {
        out = sdscatprintf(out, "- **forked from**: `%s`\n", t->forked_from->task_id);
    }
    edc_commands_reply(to, out);
    sdsfree(out);
}

static void edc_commands_handle_task_output( struct edc_session_manager *sm, struct edc_scheduler *sched,
                                            struct edc_reply_to to, sds arg)
{
    if (!arg)
    {
        edc_commands_reply(to, "Usage: `/task output <id>`");
        return;
    }
    struct edc_task *t = edc_session_store_resolve_task(sm, sched, arg, NULL);
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
static void edc_commands_handle_history( struct edc_session_manager *sm, struct edc_scheduler *sched,
                                       struct edc_reply_to to, sds arg, bool group)
{
    if (!arg)
    {
        edc_commands_reply(to, group ? "Usage: `/group history <id>`" : "Usage: `/task history <id>`  (append `--json` for the raw payload)");
        return;
    }

    const bool want_json = strncmp(arg, "--json ", 7) == 0;
    const char *task_id = want_json ? arg + 7 : arg;

    /* The resolver also returns the owning session: a task id is globally unique, so this command can find another session's task. */
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
    const char *target_id = group ? (want_json ? arg + 7 : arg) : t->task_id;
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
static void edc_commands_handle_group_fold( struct edc_session_manager *sm, struct edc_scheduler *sched,
                                           struct edc_reply_to to, sds arg)
{
    struct edc_session *session = NULL;
    struct edc_task_group *group = NULL;
    const char *focus = NULL;

    if (arg && *arg)
    {
        /* The first token = group id; the rest = focus (may contain spaces, kept verbatim). */
        const char *sp = strchr(arg, ' ');
        size_t id_len = sp ? (size_t)(sp - arg) : sdslen(arg);
        sds id = sdsnewlen(arg, id_len);
        struct edc_session *owner = NULL;
        struct edc_task *t = edc_session_store_resolve_task(sm, sched, id, &owner);
        if (!t || !owner || !(group = edc_session_find_group(owner, t->group_id)))
        {
            sds reply = sdscatprintf(sdsempty(),
                                     "Group `%s` not found — `/group fold [id] [focus]` (no id = the foreground group).",
                                     id);
            edc_commands_reply(to, reply);
            sdsfree(reply);
            sdsfree(id);
            return;
        }
        session = owner;
        sdsfree(id);
        if (sp)
        {
            focus = sp + 1;
            while (*focus == ' ')
                focus++;
            if (!*focus)
                focus = NULL;
        }
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

/* `/group history <id>`: a group's message history -- one block per member (the block
 * starts with its input, then its own messages, never interleaved across tasks). The
 * group id is the group head's id. */
static bool edc_commands_handle_group( struct edc_session_manager *sm, struct edc_scheduler *sched,
                                      struct edc_reply_to to, struct edc_channel_message *message)
{
    sds arg = NULL;
    if (edc_commands_arg(message->content, "/group history", &arg))
    {
        edc_commands_handle_history(sm, sched, to, arg, true);
        sdsfree(arg);
        return true;
    }
    sds fold_arg = NULL;
    if (edc_commands_arg(message->content, "/group fold", &fold_arg))
    {
        edc_commands_handle_group_fold(sm, sched, to, fold_arg);
        sdsfree(fold_arg);
        return true;
    }
    /* Bare `/group fold` (no id, no focus) -- edc_commands_arg is true only with
     * arguments, so this branch handles it separately: fold the current foreground group. */
    if (edc_commands_is_command(message->content, "/group fold"))
    {
        edc_commands_handle_group_fold(sm, sched, to, NULL);
        return true;
    }
    if (edc_commands_is_command(message->content, "/group") ||
        edc_commands_is_command(message->content, "/group history"))
    {
        edc_commands_reply(to, "Usage: `/group history <id>`   (the id is the group header's)\n\n"
                               "Usage: `/group fold [id] [focus]`   (fold now, ignoring the trigger; "
                               "no id = the foreground group)");
        return true;
    }
    edc_commands_reply(to, "**Unknown subcommand** — `/group` knows `history <id>` and `fold [id] [focus]`.");
    return true;
}

static void edc_commands_handle_task_join( struct edc_session_manager *sm, struct edc_scheduler *sched,
                                          struct edc_reply_to to, sds arg)
{
    if (!arg)
    {
        edc_commands_reply(to, "Usage: `/task join <id>`");
        return;
    }
    /* Global lookup: the joined task may live in another session (task ids are
     * unique). The edge is pointer-based, so the dependency works either way. */
    struct edc_task *t = edc_session_store_resolve_task(sm, sched, arg, NULL);
    if (!t)
    {
        edc_commands_reply(to, "Task not found.");
        return;
    }
    if (edc_task_is_completed(t))
    {
        /* join = inject a completed task's output into an existing receiving task.
         * There is no "create an empty task to attach into later", so with no
         * candidate the command is rejected and can be retried. */
        struct edc_session *active_session = edc_session_manager_get_active(sm);
        struct edc_task *front = active_session ? edc_scheduler_current_task(active_session) : NULL;
        /* "parked" = READY (IDLE's mirror) or waiting on deps -- both can receive injected output */
        if (!front || !edc_task_is_parked(front))
        {
            sds reply = sdscatprintf(sdsempty(),
                                     "No task to join into — the foreground group has no parked task. "
                                     "Start one (`/task new <desc>`), then `/task join %s` again.",
                                     t->task_id);
            edc_commands_reply(to, reply);
            sdsfree(reply);
            return;
        }

        /* auto-chain: inject output into the task in front */
        edc_task_add_dependency(front, t, TASK_DEP_JOIN);

        /* Echo only the raw output, no "[task xxx output]" / "injected/reaped" metadata noise */
        sds reply = t->output ? sdsnew(t->output) : sdsnew("_No output._");
        edc_commands_reply(to, reply);
        sdsfree(reply);

        /* reap source */
        if (t->group)
            edc_task_group_remove_task(t->group, t);
        edc_task_free(t);
        return;
    }

    if (edc_task_is_cancelled(t) || edc_task_has_failed(t))
    {
        sds reply = sdscatprintf(sdsempty(), "Task `%s` is %s — reaped.", t->task_id, edc_task_state_str(t));
        edc_commands_reply(to, reply);
        sdsfree(reply);
        if (t->group)
            edc_task_group_remove_task(t->group, t);
        edc_task_free(t);
        return;
    }

    sds reply = sdscatprintf(sdsempty(),
                             "Task `%s` is currently **%s**.\n\n"
                             "- `/task chain %s <current>` — a current task waits for it\n"
                             "- `/task join %s` — check back later, once it is COMPLETED",
                             t->task_id, edc_task_state_str(t), t->task_id, t->task_id);
    edc_commands_reply(to, reply);
    sdsfree(reply);
}

static void edc_commands_handle_task_chain( struct edc_session_manager *sm, struct edc_scheduler *sched,
                                           struct edc_reply_to to, sds arg)
{
    /* /task chain <src> <dst> */
    if (!arg)
    {
        edc_commands_reply(to, "Usage: `/task chain <src> <dst>`");
        return;
    }

    int count = 0;
    sds *tokens = sdssplitlen(arg, sdslen(arg), " ", 1, &count);
    if (count < 2)
    {
        edc_commands_reply(to, "Usage: `/task chain <src> <dst>`");
        sdsfreesplitres(tokens, count);
        sdsfree(arg);
        return;
    }

    struct edc_session *src_owner = NULL;
    struct edc_session *dst_owner = NULL;
    struct edc_task *src = edc_session_store_resolve_task(sm, sched, tokens[0], &src_owner);
    struct edc_task *dst = edc_session_store_resolve_task(sm, sched, tokens[1], &dst_owner);
    sdsfreesplitres(tokens, count);

    if (!src)
    {
        edc_commands_reply(to, "Source task not found.");
        return;
    }
    if (!dst)
    {
        edc_commands_reply(to, "Target task not found.");
        return;
    }

    if (edc_task_add_dependency(dst, src, TASK_DEP_JOIN))
    {
        struct edc_session *active = edc_session_manager_get_active(sm);
        sds reply = sdscatprintf(sdsempty(), "**Chained** — `%s` → `%s` · dep %zu/%zu", src->task_id, dst->task_id,
                                 dst->deps_completed, dst->deps_total);
        if (src_owner && src_owner != active)
            reply = sdscatprintf(reply, " · source in `[%s]`", src_owner->session_id);
        if (dst_owner && dst_owner != active)
            reply = sdscatprintf(reply, " · target in `[%s]`", dst_owner->session_id);
        edc_commands_reply(to, reply);
        sdsfree(reply);
    }
    else
    {
        edc_commands_reply(to, "Failed to add dependency.");
    }
}

static bool edc_commands_handle_task( struct edc_session_manager *sm, struct edc_scheduler *sched,
                                     struct edc_reply_to to, struct edc_channel_message *message)
{

    const char *content = message->content;
    sds content_trim = sdstrim(sdsnew(content), " \t\r\n");

    sds arg = NULL;

    if (strncmp(content_trim, "/task new ", 10) == 0)
    {
        arg = sdsnew(content_trim + 10);
        edc_commands_handle_task_new(sm, sched, to, cmd_input_session(sm, message), arg);
    }
    else if (strncmp(content_trim, "/task fork ", 11) == 0)
    {
        arg = sdsnew(content_trim + 11);
        edc_commands_handle_task_fork(sm, sched, to, cmd_input_session(sm, message), arg);
    }
    else if (strcmp(content_trim, "/task list") == 0)
    {
        edc_commands_handle_task_list(sm, to);
    }
    else if (strcmp(content_trim, "/task stop") == 0)
    {
        edc_commands_handle_task_stop(sm, sched, to, NULL);
    }
    else if (strncmp(content_trim, "/task stop ", 11) == 0)
    {
        arg = sdsnew(content_trim + 11);
        edc_commands_handle_task_stop(sm, sched, to, arg);
    }
    else if (strcmp(content_trim, "/task resume") == 0)
    {
        edc_commands_handle_task_resume(sm, sched, to, NULL);
    }
    else if (strncmp(content_trim, "/task resume ", 13) == 0)
    {
        arg = sdsnew(content_trim + 13);
        edc_commands_handle_task_resume(sm, sched, to, arg);
    }
    else if (strcmp(content_trim, "/task status") == 0)
    {
        edc_commands_handle_task_status(sm, sched, to, NULL);
    }
    else if (strncmp(content_trim, "/task status ", 13) == 0)
    {
        arg = sdsnew(content_trim + 13);
        edc_commands_handle_task_status(sm, sched, to, arg);
    }
    else if (strncmp(content_trim, "/task output ", 13) == 0)
    {
        arg = sdsnew(content_trim + 13);
        edc_commands_handle_task_output(sm, sched, to, arg);
    }
    else if (strcmp(content_trim, "/task history") == 0)
    {
        edc_commands_handle_history(sm, sched, to, NULL, false);
    }
    else if (strncmp(content_trim, "/task history ", 14) == 0)
    {
        arg = sdsnew(content_trim + 14);
        edc_commands_handle_history(sm, sched, to, arg, false);
    }
    else if (strncmp(content_trim, "/task fg ", 9) == 0)
    {
        arg = sdsnew(content_trim + 9);
        edc_commands_handle_task_fg(sm, to, arg);
    }
    else if (strncmp(content_trim, "/task bg ", 9) == 0)
    {
        arg = sdsnew(content_trim + 9);
        edc_commands_handle_task_bg(sm, to, arg);
    }
    else if (strcmp(content_trim, "/task bg") == 0)
    {
        edc_commands_handle_task_bg(sm, to, NULL);
    }
    else if (strcmp(content_trim, "/task groups") == 0)
    {
        edc_commands_handle_task_groups(sm, to);
    }
    else if (strncmp(content_trim, "/task chain ", 12) == 0)
    {
        arg = sdsnew(content_trim + 12);
        edc_commands_handle_task_chain(sm, sched, to, arg);
    }
    else if (strncmp(content_trim, "/task join ", 11) == 0)
    {
        arg = sdsnew(content_trim + 11);
        edc_commands_handle_task_join(sm, sched, to, arg);
    }
    else
    {
        /* Unknown /task subcommand — report error instead of falling through */
        sds reply = sdscatprintf(sdsempty(), "Unknown `/task` subcommand. Type `/help` for available commands.");
        edc_commands_reply(to, reply);
        sdsfree(reply);
        sdsfree(content_trim);
        return true;
    }

    sdsfree(content_trim);
    if (arg)
        sdsfree(arg);
    return true;
}

/* ── Entry ─────────────────────────────────────────────────── */

bool edc_commands_handler( struct edc_config *cfg, struct edc_session_manager *sm, struct edc_scheduler *sched,
                          struct edc_reply_to to, struct edc_channel_message *message)
{
    /* The validity gate checks only the message itself, not message->type -- historically
     * edc_channel_cli==0 made CLI messages look "untyped" and drop them wholesale (type
     * is a routing key, not a validity flag). */
    if (!message || !message->content)
        return false;

    /* / commands: shell builtins first, then shortcuts, then existing handlers */
    if (message->content[0] == '/')
    {
        if (edc_shell_handle_builtin(cfg, sm, to, message))
            return true;

        /* The top-level shortcuts (/tasks, /bg, /fg) were removed -- all go through /task subcommands. */

        if (edc_commands_is_command(message->content, "/task"))
            return edc_commands_handle_task(sm, sched, to, message);

        if (edc_commands_is_command(message->content, "/group"))
            return edc_commands_handle_group(sm, sched, to, message);

        if (edc_commands_is_command(message->content, "/session"))
            return edc_commands_handle_session(sm, sched, to, message);

        if (edc_commands_is_target(message->content, "/help"))
            return edc_commands_handle_help(to);

        sds reply = sdscatprintf(sdsempty(), "**Unknown command** — `%s`\n\nType `/help` for available commands.",
                                 message->content);
        edc_commands_reply(to, reply);
        sdsfree(reply);
        return true;
    }

    /* non-/ input: delegate to shell module */
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
