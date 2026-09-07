#include "edc_commands.h"
#include "edc_actor.h"
#include "edc_agent.h"
#include "edc_channel_actor.h"
#include "edc_cmd_parse.h"
#include "edc_compat.h"
#include "edc_memory_actor.h"
#include "edc_scheduler.h"
#include "edc_session.h"
#include "edc_session_manager.h"
#include "edc_shell.h"
#include "edc_state_serializer.h"
#include "edc_task.h"
#include "edc_task_group.h"
#include "sds.h"
#include "utlist_fix.h"
#include <log.h>
#include <signal.h>
#include <string.h>

extern volatile sig_atomic_t _run_loop;

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

void edc_commands_reply(struct edc_agent *agent, edc_actor_t *actor, edc_channel_type channel_type, const char *text)
{
    struct edc_channel_message *reply = edc_channel_message_new(channel_type, EDC_CHANNEL_MESSAGE_SYS_TYPE, sdsnew(text));
    edc_actor_send(agent->channel_actor,
                   edc_actor_msg_new_sub_type(BC_ACTOR_MSG_CHANNEL_SEND, reply->type, actor, NULL, reply,
                                              (edc_actor_data_free_fn)edc_channel_message_free));
}

/* ── /new ──────────────────────────────────────────────────── */

static bool edc_commands_handle_new(struct edc_agent *agent, edc_actor_t *actor, edc_channel_type channel_type)
{
    edc_session_reload(edc_session_manager_get_active(agent->session_manager), false);
    edc_agent_remove_all_turns(agent);

    if (!edc_memory_actor_is_compressing(agent->memory_actor))
    {
        edc_actor_send(agent->memory_actor,
                       edc_actor_msg_new(BC_ACTOR_MSG_MEMORY_COMPRESS, edc_scheduler_get_actor(agent->scheduler), NULL,
                                         (void *)(intptr_t)1, NULL));
    }

    edc_commands_reply(agent, actor, channel_type, "[system] start new session now.");
    log_info("receive /new command");
    return true;
}

/* ── /stop ─────────────────────────────────────────────────── */

static bool edc_commands_handle_stop(struct edc_agent *agent, edc_actor_t *actor, edc_channel_type channel_type)
{
    edc_session_reload(edc_session_manager_get_active(agent->session_manager), true);
    edc_agent_remove_all_turns(agent);

    edc_commands_reply(agent, actor, channel_type, "[system] stop all turns.");
    return true;
}

/* ── /help ─────────────────────────────────────────────────── */

static bool edc_commands_handle_help(struct edc_agent *agent, edc_actor_t *actor, edc_channel_type channel_type)
{
    const char *text = "## EdgeClaw Commands\n"
                       "\n"
                       "- /new  ·  /stop  ·  /help  ·  /exit\n"
                       "- /session new [name]  ·  /session switch <name>\n"
                       "- /session list  ·  /session stop <name>\n"
                       "- /session rename <old> <new>  ·  /session info\n"
                       "\n"
                       "- /task new <desc>   [--bg]\n"
                       "- /task fork <parent> <desc>\n"
                       "- /task list  ·  /task status [id]  ·  /task output <id>\n"
                       "- /task groups  ·  /task fg <id>  ·  /task bg [id]\n"
                       "- /task switch <id>  ·  /task stop <id>  [--force]\n"
                       "- /task suspend <id>  ·  /task resume <id>\n"
                       "- /task chain <src> <dst>  ·  /task join <id>\n"
                       "- /task prompt <id> <name>\n"
                       "\n"
                       "- /jobs  ·  /tasks  ·  /cancel <id>  ·  /whoami\n"
                       "- /bg [id]  ·  /fg <id>  ·  /model [name]  ·  /history\n"
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

    edc_commands_reply(agent, actor, channel_type, text);
    return true;
}

/* ── /save ─────────────────────────────────────────────────── */

static bool edc_commands_handle_save(struct edc_agent *agent, edc_actor_t *actor, edc_channel_type channel_type)
{
    struct edc_session *active = edc_session_manager_get_active(agent->session_manager);
    if (!active)
    {
        edc_commands_reply(agent, actor, channel_type, "[save] no active session");
        return true;
    }

    edc_session_save_history(active);
    edc_session_save_state(active, agent->scheduler);
    edc_commands_reply(agent, actor, channel_type, "[save] session saved.");
    log_info("receive /save command");
    return true;
}

/* ── /exit ─────────────────────────────────────────────────── */

static bool edc_commands_handle_exit(struct edc_agent *agent, edc_actor_t *actor, edc_channel_type channel_type)
{
    edc_commands_reply(agent, actor, channel_type, "[system] shutting down...");
    log_info("received /exit command");
    _run_loop = 0;
    return true;
}

/* ── /session commands ─────────────────────────────────────── */

static bool edc_commands_handle_session_new(struct edc_agent *agent, edc_actor_t *actor, edc_channel_type channel_type,
                                            sds arg)
{
    const char *name = (arg && sdslen(arg) > 0) ? arg : NULL;
    struct edc_session *sess = edc_session_manager_create(agent->session_manager, name);
    if (!sess)
    {
        edc_commands_reply(agent, actor, channel_type, "[session] failed to create session");
        return true;
    }
    {
        struct edc_session *old = edc_session_manager_get_active(agent->session_manager);
        if (old && old != sess)
            edc_session_save_state(old, agent->scheduler);
    }
    agent->scheduler->session = sess;
    agent->scheduler->foreground_group = NULL;
    edc_session_reload(sess, true);
    sds reply = sdscatprintf(sdsempty(), "[session] created and switched to [%s] %s", sess->session_id,
                             edc_session_get_name(sess));
    edc_commands_reply(agent, actor, channel_type, reply);
    sdsfree(reply);
    return true;
}

static bool edc_commands_handle_session_switch(struct edc_agent *agent, edc_actor_t *actor, edc_channel_type channel_type,
                                               sds arg)
{
    if (!arg || sdslen(arg) == 0)
    {
        edc_commands_reply(agent, actor, channel_type, "[session] usage: /session switch <name_or_id>");
        return true;
    }
    struct edc_session *old = edc_session_manager_get_active(agent->session_manager);
    if (old)
    {
        edc_session_save_history(old);
        edc_session_save_state(old, agent->scheduler);
    }
    if (!edc_session_manager_switch(agent->session_manager, arg))
    {
        sds reply = sdscatprintf(sdsempty(), "[session] session not found: %s", arg);
        edc_commands_reply(agent, actor, channel_type, reply);
        sdsfree(reply);
        return true;
    }
    struct edc_session *new_sess = edc_session_manager_get_active(agent->session_manager);
    agent->scheduler->session = new_sess;
    agent->scheduler->foreground_group = NULL;
    edc_session_load_state(new_sess, agent->scheduler);
    edc_session_reload(new_sess, true);
    edc_agent_remove_all_turns(agent);
    sds reply =
        sdscatprintf(sdsempty(), "[session] switched to [%s] %s", new_sess->session_id, edc_session_get_name(new_sess));
    edc_commands_reply(agent, actor, channel_type, reply);
    sdsfree(reply);
    return true;
}

static bool edc_commands_handle_session_list(struct edc_agent *agent, edc_actor_t *actor, edc_channel_type channel_type)
{
    sds list = edc_session_manager_list(agent->session_manager);
    edc_commands_reply(agent, actor, channel_type, list);
    sdsfree(list);
    return true;
}

static bool edc_commands_handle_session_stop(struct edc_agent *agent, edc_actor_t *actor, edc_channel_type channel_type,
                                             sds arg)
{
    if (!arg || sdslen(arg) == 0)
    {
        edc_commands_reply(agent, actor, channel_type, "[session] usage: /session stop <name_or_id>");
        return true;
    }
    struct edc_session *target = edc_session_manager_find(agent->session_manager, arg);
    if (!target)
    {
        sds reply = sdscatprintf(sdsempty(), "[session] session not found: %s", arg);
        edc_commands_reply(agent, actor, channel_type, reply);
        sdsfree(reply);
        return true;
    }
    sds stopped_id = sdsnew(target->session_id);
    sds stopped_name = sdsnew(edc_session_get_name(target));
    if (!edc_session_manager_stop(agent->session_manager, arg))
    {
        sdsfree(stopped_id);
        sdsfree(stopped_name);
        edc_commands_reply(agent, actor, channel_type, "[session] cannot stop the last session");
        return true;
    }
    struct edc_session *new_active = edc_session_manager_get_active(agent->session_manager);
    agent->scheduler->session = new_active;
    edc_session_reload(new_active, true);
    edc_agent_remove_all_turns(agent);
    sds reply = sdscatprintf(sdsempty(), "[session] stopped [%s] %s", stopped_id, stopped_name);
    edc_commands_reply(agent, actor, channel_type, reply);
    sdsfree(reply);
    sdsfree(stopped_id);
    sdsfree(stopped_name);
    return true;
}

static bool edc_commands_handle_session_rename(struct edc_agent *agent, edc_actor_t *actor, edc_channel_type channel_type,
                                               sds arg)
{
    if (!arg || sdslen(arg) == 0)
    {
        edc_commands_reply(agent, actor, channel_type, "[session] usage: /session rename <old_name> <new_name>");
        return true;
    }
    int count = 0;
    sds *tokens = sdssplitlen(arg, sdslen(arg), " ", 1, &count);
    if (count < 2)
    {
        sdsfreesplitres(tokens, count);
        edc_commands_reply(agent, actor, channel_type, "[session] usage: /session rename <old_name> <new_name>");
        return true;
    }
    bool ok = edc_session_manager_rename(agent->session_manager, tokens[0], tokens[1]);
    if (!ok)
    {
        sds reply = sdscatprintf(sdsempty(), "[session] session not found: %s", tokens[0]);
        edc_commands_reply(agent, actor, channel_type, reply);
        sdsfree(reply);
        sdsfreesplitres(tokens, count);
        return true;
    }
    sds reply = sdscatprintf(sdsempty(), "[session] renamed %s -> %s", tokens[0], tokens[1]);
    edc_commands_reply(agent, actor, channel_type, reply);
    sdsfree(reply);
    sdsfreesplitres(tokens, count);
    return true;
}

static bool edc_commands_handle_session_info(struct edc_agent *agent, edc_actor_t *actor, edc_channel_type channel_type)
{
    struct edc_session *sess = edc_session_manager_get_active(agent->session_manager);
    if (!sess)
    {
        edc_commands_reply(agent, actor, channel_type, "[session] no active session");
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
                     "- **Directory**: %s\n",
                     sess->session_id, edc_session_get_name(sess), sess->created_at ? sess->created_at : "unknown",
                     sess->last_active ? sess->last_active : "unknown", edc_session_get_message_count(sess),
                     sess->history_dir ? sess->history_dir : "");
    edc_commands_reply(agent, actor, channel_type, info);
    sdsfree(info);
    return true;
}

static bool edc_commands_handle_session(struct edc_agent *agent, edc_actor_t *actor, struct edc_channel_message *msg)
{
    sds rest;
    if (edc_commands_arg(msg->content, "/session new", &rest))
    {
        bool ret = edc_commands_handle_session_new(agent, actor, msg->type, rest);
        sdsfree(rest);
        return ret;
    }
    if (edc_commands_arg(msg->content, "/session switch", &rest))
    {
        bool ret = edc_commands_handle_session_switch(agent, actor, msg->type, rest);
        sdsfree(rest);
        return ret;
    }
    if (edc_commands_is_target(msg->content, "/session list"))
        return edc_commands_handle_session_list(agent, actor, msg->type);
    if (edc_commands_arg(msg->content, "/session stop", &rest))
    {
        bool ret = edc_commands_handle_session_stop(agent, actor, msg->type, rest);
        sdsfree(rest);
        return ret;
    }
    if (edc_commands_arg(msg->content, "/session rename", &rest))
    {
        bool ret = edc_commands_handle_session_rename(agent, actor, msg->type, rest);
        sdsfree(rest);
        return ret;
    }
    if (edc_commands_is_target(msg->content, "/session info"))
        return edc_commands_handle_session_info(agent, actor, msg->type);
    sds reply = sdscatprintf(sdsempty(), "[session] unknown subcommand. Try: new, switch, list, stop, rename, info");
    edc_commands_reply(agent, actor, msg->type, reply);
    sdsfree(reply);
    return true;
}

/* ── /task commands ─────────────────────────────────────────― */

static void edc_commands_handle_task_new(struct edc_agent *agent, edc_actor_t *actor, edc_channel_type channel_type,
                                         sds arg)
{
    struct edc_cmd_task_opts opts;
    edc_cmd_parse_task_opts(arg, &opts);

    struct edc_task *task = edc_scheduler_create_task(
        agent->scheduler, (opts.desc && sdslen(opts.desc) > 0) ? opts.desc : "New task", channel_type);
    if (opts.prompt_name)
        edc_task_set_prompt(task, opts.prompt_name, NULL);

    if (opts.group_id)
    {
        struct edc_task_group *group =
            edc_session_find_group(edc_session_manager_get_active(agent->session_manager), opts.group_id);
        if (!group)
        {
            sds reply = sdscatprintf(sdsempty(), "group %s not found, creating new group", opts.group_id);
            edc_commands_reply(agent, actor, channel_type, reply);
            sdsfree(reply);
            group = edc_scheduler_create_group(agent->scheduler, task);
        }
        else
        {
            edc_task_group_add_task(group, task);
            // sds reply = sdscatprintf(sdsempty(), "[task] created %s in group %s", task->task_id, group->group_id);
            // edc_commands_reply(agent, actor, channel_type, reply);
            // sdsfree(reply);
            goto done;
        }
    }
    else
    {
        struct edc_task_group *group = edc_scheduler_create_group(agent->scheduler, task);

        if (opts.bg)
        {
            // sds reply =
            //     sdscatprintf(sdsempty(), "[task] created %s (group %s) in background", task->task_id, group->group_id);
            // edc_commands_reply(agent, actor, channel_type, reply);
            // sdsfree(reply);
        }
        else
        {
            edc_scheduler_set_foreground(agent->scheduler, group);
            edc_session_set_active_task(edc_session_manager_get_active(agent->session_manager), task);
            // sds reply = sdscatprintf(sdsempty(), "[task] created %s (group %s)", task->task_id, group->group_id);
            // edc_commands_reply(agent, actor, channel_type, reply);
            // sdsfree(reply);
        }
    }

    /* add deps from -d flags */
    for (int i = 0; i < opts.dep_count; i++)
    {
        struct edc_task *dt =
            edc_session_find_task(edc_session_manager_get_active(agent->session_manager), opts.dep_ids[i]);
        if (dt)
            edc_task_add_dependency(task, dt, TASK_DEP_JOIN);
    }

done:
    edc_cmd_task_opts_free(&opts);
}

static void edc_commands_handle_task_list(struct edc_agent *agent, edc_actor_t *actor, edc_channel_type channel_type)
{
    sds out = sdsempty();
    struct edc_task_group *g;
    bool first = true;
    DL_FOREACH(agent->scheduler->session->groups, g)
    {
        if (!first)
            out = sdscat(out, "\n");
        first = false;

        bool fg = agent->scheduler->foreground_group == g;
        out = sdscatprintf(out, "- %s%s%s (%d)\n", fg ? "[fg] " : "", g->is_orphaned ? "[orphan] " : "", g->group_id,
                           g->member_count);

        struct edc_task *t;
        LL_FOREACH2(g->members, t, group_next)
        {
            bool is_active = edc_session_manager_get_active(agent->session_manager) &&
                             edc_session_manager_get_active(agent->session_manager)->active_task == t;
            out = sdscatprintf(out, "    - %s%s %-12s %s", t == g->leader ? "[L]" : "[M]", is_active ? "*" : " ",
                               t->task_id, edc_task_state_name(t->state));
            if (*t->description)
                out = sdscatprintf(out, "  %s", t->description);
            out = sdscat(out, "\n");
        }
    }
    if (!first)
        ; /* at least one group */
    edc_commands_reply(agent, actor, channel_type, out);
    sdsfree(out);
}

static void edc_commands_handle_task_switch(struct edc_agent *agent, edc_actor_t *actor, edc_channel_type channel_type,
                                            sds arg)
{
    if (!arg)
    {
        edc_commands_reply(agent, actor, channel_type, "/task switch <id>");
        return;
    }
    struct edc_task *t = edc_session_find_task(edc_session_manager_get_active(agent->session_manager), arg);
    if (!t)
    {
        edc_commands_reply(agent, actor, channel_type, "task not found");
        return;
    }
    edc_session_set_active_task(edc_session_manager_get_active(agent->session_manager), t);
    sds reply = sdscatprintf(sdsempty(), "switched to task %s", t->task_id);
    edc_commands_reply(agent, actor, channel_type, reply);
    sdsfree(reply);
}

static void edc_commands_handle_task_stop(struct edc_agent *agent, edc_actor_t *actor, edc_channel_type channel_type,
                                          sds arg)
{
    if (!arg)
    {
        edc_commands_reply(agent, actor, channel_type, "/task stop <id>");
        return;
    }
    edc_scheduler_send_signal_to_task(agent->scheduler, arg, TASK_SIGTERM);
    edc_commands_reply(agent, actor, channel_type, "stop signal sent");
}

static void edc_commands_handle_task_fg(struct edc_agent *agent, edc_actor_t *actor, edc_channel_type channel_type,
                                        sds arg)
{
    if (!arg)
    {
        edc_commands_reply(agent, actor, channel_type, "/task fg <id>");
        return;
    }
    struct edc_task *t = edc_session_find_task(edc_session_manager_get_active(agent->session_manager), arg);
    if (!t)
    {
        edc_commands_reply(agent, actor, channel_type, "task not found");
        return;
    }
    struct edc_task_group *g =
        edc_session_find_group(edc_session_manager_get_active(agent->session_manager), t->group_id);
    if (!g)
    {
        edc_commands_reply(agent, actor, channel_type, "group not found");
        return;
    }
    edc_scheduler_set_foreground(agent->scheduler, g);
    edc_session_set_active_task(edc_session_manager_get_active(agent->session_manager), t);
    sds reply = sdscatprintf(sdsempty(), "foreground: group %s (active: %s)", g->group_id, t->task_id);
    edc_commands_reply(agent, actor, channel_type, reply);
    sdsfree(reply);
}

static void edc_commands_handle_task_bg(struct edc_agent *agent, edc_actor_t *actor, edc_channel_type channel_type,
                                        sds arg)
{
    if (arg)
    {
        struct edc_task *t = edc_session_find_task(edc_session_manager_get_active(agent->session_manager), arg);
        if (!t)
        {
            edc_commands_reply(agent, actor, channel_type, "task not found");
            return;
        }
        struct edc_task_group *g =
            edc_session_find_group(edc_session_manager_get_active(agent->session_manager), t->group_id);
        if (!g)
        {
            edc_commands_reply(agent, actor, channel_type, "group not found");
            return;
        }
        if (agent->scheduler->foreground_group == g)
        {
            edc_scheduler_set_foreground(agent->scheduler, NULL);
            edc_session_set_active_task(edc_session_manager_get_active(agent->session_manager), NULL);
            sds reply = sdscatprintf(sdsempty(), "background: group %s", g->group_id);
            edc_commands_reply(agent, actor, channel_type, reply);
            sdsfree(reply);
        }
        else
        {
            sds reply = sdscatprintf(sdsempty(), "group %s is already in background", g->group_id);
            edc_commands_reply(agent, actor, channel_type, reply);
            sdsfree(reply);
        }
    }
    else
    {
        edc_scheduler_set_foreground(agent->scheduler, NULL);
        edc_session_set_active_task(edc_session_manager_get_active(agent->session_manager), NULL);
        edc_commands_reply(agent, actor, channel_type, "no foreground group");
    }
}

static void edc_commands_handle_task_fork(struct edc_agent *agent, edc_actor_t *actor, edc_channel_type channel_type,
                                          sds arg)
{
    /* /task fork <parent_id> <desc> [--prompt <name>] */
    if (!arg)
    {
        edc_commands_reply(agent, actor, channel_type, "/task fork <parent_id> <desc> [--prompt <name>]");
        return;
    }

    int count = 0;
    sds *tokens = sdssplitlen(arg, sdslen(arg), " ", 1, &count);
    if (count < 2)
    {
        edc_commands_reply(agent, actor, channel_type, "usage: /task fork <parent_id> <desc> [--prompt <name>]");
        sdsfreesplitres(tokens, count);
        return;
    }

    struct edc_task *parent = edc_session_find_task(edc_session_manager_get_active(agent->session_manager), tokens[0]);
    if (!parent)
    {
        sds reply = sdscatprintf(sdsempty(), "parent task %s not found", tokens[0]);
        edc_commands_reply(agent, actor, channel_type, reply);
        sdsfree(reply);
        sdsfreesplitres(tokens, count);
        return;
    }

    /* find --prompt in remaining args */
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
    char *fork_tid = edc_scheduler_gen_task_id(agent->scheduler);
    struct edc_task *child = edc_task_fork(parent, fork_tid, desc, fork_point, channel_type);
    free(fork_tid);
    if (prompt_name)
        edc_task_set_prompt(child, prompt_name, NULL);

    if (parent->group)
        edc_task_group_add_task(parent->group, child);

    sds reply = sdscatprintf(sdsempty(), "[fork] created %s from %s (group %s, fork_point=%zu)%s", child->task_id,
                             parent->task_id, child->group_id, fork_point, prompt_name ? " prompt=" : "");
    if (prompt_name)
        reply = sdscat(reply, prompt_name);
    edc_commands_reply(agent, actor, channel_type, reply);
    sdsfree(reply);
    sdsfree(desc);
    if (prompt_name)
        sdsfree(prompt_name);
}

static void edc_commands_handle_task_groups(struct edc_agent *agent, edc_actor_t *actor, edc_channel_type channel_type)
{
    sds out = sdsnew("## Groups\n\n");
    struct edc_task_group *g;
    DL_FOREACH(agent->scheduler->session->groups, g)
    {
        out = sdscatprintf(
            out, "- %s%s  **%s**  %d members  _%s_\n", agent->scheduler->foreground_group == g ? "`[fg]` " : "",
            g->is_orphaned ? "`[orphan]` " : "", g->group_id, g->member_count, edc_task_group_status_label(g));
    }
    edc_commands_reply(agent, actor, channel_type, out);
    sdsfree(out);
}

static void edc_commands_handle_task_status(struct edc_agent *agent, edc_actor_t *actor, edc_channel_type channel_type,
                                            sds arg)
{
    struct edc_task *t;
    if (arg)
    {
        t = edc_session_find_task(edc_session_manager_get_active(agent->session_manager), arg);
        if (!t)
        {
            edc_commands_reply(agent, actor, channel_type, "task not found");
            return;
        }
    }
    else
    {
        t = edc_session_get_active_task(edc_session_manager_get_active(agent->session_manager));
        if (!t)
        {
            edc_commands_reply(agent, actor, channel_type, "no active task");
            return;
        }
    }

    sds out = sdscatprintf(sdsempty(),
                           "## Task `%s`\n\n"
                           "- **state**: %s\n"
                           "- **group**: %s\n"
                           "- **desc**: %s\n"
                           "- **deps**: %zu/%zu completed\n"
                           "- **turns**: %zu\n",
                           t->task_id, edc_task_state_name(t->state), t->group_id, t->description, t->deps_completed,
                           t->deps_total, (size_t)edc_queue_length(&t->turns));
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
    edc_commands_reply(agent, actor, channel_type, out);
    sdsfree(out);
}

static void edc_commands_handle_task_output(struct edc_agent *agent, edc_actor_t *actor, edc_channel_type channel_type,
                                            sds arg)
{
    if (!arg)
    {
        edc_commands_reply(agent, actor, channel_type, "/task output <id>");
        return;
    }
    struct edc_task *t = edc_session_find_task(edc_session_manager_get_active(agent->session_manager), arg);
    if (!t)
    {
        edc_commands_reply(agent, actor, channel_type, "task not found");
        return;
    }
    if (!t->output)
    {
        edc_commands_reply(agent, actor, channel_type, "(no output)");
        return;
    }
    edc_commands_reply(agent, actor, channel_type, t->output);
}

static void edc_commands_handle_task_join(struct edc_agent *agent, edc_actor_t *actor, edc_channel_type channel_type,
                                          sds arg)
{
    if (!arg)
    {
        edc_commands_reply(agent, actor, channel_type, "/task join <id>");
        return;
    }
    struct edc_task *t = edc_session_find_task(edc_session_manager_get_active(agent->session_manager), arg);
    if (!t)
    {
        edc_commands_reply(agent, actor, channel_type, "task not found");
        return;
    }

    if (t->state == TASK_COMPLETED)
    {
        /* find or create a target task to inject output into */
        struct edc_task *active = edc_session_get_active_task(edc_session_manager_get_active(agent->session_manager));
        if (!active || (active->state != TASK_WAITING_INPUT && active->state != TASK_WAITING_DEPS))
        {
            /* no usable active task — create one in foreground group */
            struct edc_task_group *fg = agent->scheduler->foreground_group;
            edc_channel_type channel_type = edc_channel_cli;
            if (fg && fg->members)
                channel_type = fg->members->channel_type;
            active = edc_scheduler_create_task(agent->scheduler, "Joined task", channel_type);
            if (fg)
            {
                edc_task_group_add_task(fg, active);
                edc_session_set_active_task(edc_session_manager_get_active(agent->session_manager), active);
            }
            else
            {
                struct edc_task_group *g = edc_scheduler_create_group(agent->scheduler, active);
                edc_scheduler_set_foreground(agent->scheduler, g);
            }
        }

        /* auto-chain: inject output into active task */
        edc_task_add_dependency(active, t, TASK_DEP_JOIN);

        /* 只回显原始输出，不加 "[task xxx output]" / "injected/reaped" 等元信息噪声 */
        sds reply = t->output ? sdsnew(t->output) : sdsnew("(no output)");
        edc_commands_reply(agent, actor, channel_type, reply);
        sdsfree(reply);

        /* reap source */
        if (t->group)
            edc_task_group_remove_task(t->group, t);
        edc_task_free(t);
        return;
    }

    if (t->state == TASK_CANCELLED || t->state == TASK_ERROR)
    {
        sds reply = sdscatprintf(sdsempty(), "task %s %s, reaped", t->task_id, edc_task_state_name(t->state));
        edc_commands_reply(agent, actor, channel_type, reply);
        sdsfree(reply);
        if (t->group)
            edc_task_group_remove_task(t->group, t);
        edc_task_free(t);
        return;
    }

    sds reply = sdscatprintf(sdsempty(),
                             "task %s is currently %s. Try:\n"
                             "  /task chain %s <current>   — current task waits for it\n"
                             "  /task join %s              — check back later when COMPLETED",
                             t->task_id, edc_task_state_name(t->state), t->task_id, t->task_id);
    edc_commands_reply(agent, actor, channel_type, reply);
    sdsfree(reply);
}

static void edc_commands_handle_task_suspend(struct edc_agent *agent, edc_actor_t *actor, edc_channel_type channel_type,
                                             sds arg)
{
    struct edc_task *t;
    if (arg)
        t = edc_session_find_task(edc_session_manager_get_active(agent->session_manager), arg);
    else
        t = edc_session_get_active_task(edc_session_manager_get_active(agent->session_manager));
    if (!t)
    {
        edc_commands_reply(agent, actor, channel_type, "task not found");
        return;
    }
    if (!edc_task_set_state(t, TASK_SUSPENDED))
    {
        edc_commands_reply(agent, actor, channel_type, "cannot suspend in current state");
        return;
    }
    sds reply = sdscatprintf(sdsempty(), "task %s suspended", t->task_id);
    edc_commands_reply(agent, actor, channel_type, reply);
    sdsfree(reply);
}

static void edc_commands_handle_task_resume(struct edc_agent *agent, edc_actor_t *actor, edc_channel_type channel_type,
                                            sds arg)
{
    struct edc_task *t;
    if (arg)
        t = edc_session_find_task(edc_session_manager_get_active(agent->session_manager), arg);
    else
        t = edc_session_get_active_task(edc_session_manager_get_active(agent->session_manager));
    if (!t)
    {
        edc_commands_reply(agent, actor, channel_type, "task not found");
        return;
    }
    if (t->state != TASK_SUSPENDED)
    {
        sds reply = sdscatprintf(sdsempty(), "task %s is %s, not SUSPENDED", t->task_id, edc_task_state_name(t->state));
        edc_commands_reply(agent, actor, channel_type, reply);
        sdsfree(reply);
        return;
    }
    edc_task_set_state(t, TASK_WAITING_INPUT);
    sds reply = sdscatprintf(sdsempty(), "task %s resumed", t->task_id);
    edc_commands_reply(agent, actor, channel_type, reply);
    sdsfree(reply);
}

static void edc_commands_handle_task_chain(struct edc_agent *agent, edc_actor_t *actor, edc_channel_type channel_type,
                                           sds arg)
{
    /* /task chain <src> <dst> */
    if (!arg)
    {
        edc_commands_reply(agent, actor, channel_type, "/task chain <src> <dst>");
        return;
    }

    int count = 0;
    sds *tokens = sdssplitlen(arg, sdslen(arg), " ", 1, &count);
    if (count < 2)
    {
        edc_commands_reply(agent, actor, channel_type, "usage: /task chain <source_id> <target_id>");
        sdsfreesplitres(tokens, count);
        sdsfree(arg);
        return;
    }

    struct edc_task *src = edc_session_find_task(edc_session_manager_get_active(agent->session_manager), tokens[0]);
    struct edc_task *dst = edc_session_find_task(edc_session_manager_get_active(agent->session_manager), tokens[1]);
    sdsfreesplitres(tokens, count);

    if (!src)
    {
        edc_commands_reply(agent, actor, channel_type, "source task not found");
        return;
    }
    if (!dst)
    {
        edc_commands_reply(agent, actor, channel_type, "target task not found");
        return;
    }

    if (edc_task_add_dependency(dst, src, TASK_DEP_JOIN))
    {
        sds reply = sdscatprintf(sdsempty(), "chain: %s → %s  (dep %zu/%zu)", src->task_id, dst->task_id,
                                 dst->deps_completed, dst->deps_total);
        edc_commands_reply(agent, actor, channel_type, reply);
        sdsfree(reply);
    }
    else
    {
        edc_commands_reply(agent, actor, channel_type, "failed to add dependency");
    }
}

static void edc_commands_handle_task_prompt(struct edc_agent *agent, edc_actor_t *actor, edc_channel_type channel_type,
                                            sds arg)
{
    /* /task prompt <id> <name>  or  /task prompt <id> --text "..." */
    if (!arg)
    {
        edc_commands_reply(agent, actor, channel_type, "/task prompt <id> [name|--text \"...\"]");
        return;
    }
    /* simplified: not parsing --text for now */
    edc_commands_reply(agent, actor, channel_type, "prompt set (stub)");
}

static bool edc_commands_handle_task(struct edc_agent *agent, edc_actor_t *actor, struct edc_channel_message *message)
{
    const char *content = message->content;
    sds content_trim = sdstrim(sdsnew(content), " \t\r\n");

    sds arg = NULL;

    if (strncmp(content_trim, "/task new ", 10) == 0)
    {
        arg = sdsnew(content_trim + 10);
        edc_commands_handle_task_new(agent, actor, message->type, arg);
    }
    else if (strncmp(content_trim, "/task fork ", 11) == 0)
    {
        arg = sdsnew(content_trim + 11);
        edc_commands_handle_task_fork(agent, actor, message->type, arg);
    }
    else if (strcmp(content_trim, "/task list") == 0)
    {
        edc_commands_handle_task_list(agent, actor, message->type);
    }
    else if (strncmp(content_trim, "/task switch ", 13) == 0)
    {
        arg = sdsnew(content_trim + 13);
        edc_commands_handle_task_switch(agent, actor, message->type, arg);
    }
    else if (strncmp(content_trim, "/task stop ", 11) == 0)
    {
        arg = sdsnew(content_trim + 11);
        edc_commands_handle_task_stop(agent, actor, message->type, arg);
    }
    else if (strcmp(content_trim, "/task status") == 0)
    {
        edc_commands_handle_task_status(agent, actor, message->type, NULL);
    }
    else if (strncmp(content_trim, "/task status ", 13) == 0)
    {
        arg = sdsnew(content_trim + 13);
        edc_commands_handle_task_status(agent, actor, message->type, arg);
    }
    else if (strncmp(content_trim, "/task output ", 13) == 0)
    {
        arg = sdsnew(content_trim + 13);
        edc_commands_handle_task_output(agent, actor, message->type, arg);
    }
    else if (strncmp(content_trim, "/task fg ", 9) == 0)
    {
        arg = sdsnew(content_trim + 9);
        edc_commands_handle_task_fg(agent, actor, message->type, arg);
    }
    else if (strncmp(content_trim, "/task bg ", 9) == 0)
    {
        arg = sdsnew(content_trim + 9);
        edc_commands_handle_task_bg(agent, actor, message->type, arg);
    }
    else if (strcmp(content_trim, "/task bg") == 0)
    {
        edc_commands_handle_task_bg(agent, actor, message->type, NULL);
    }
    else if (strcmp(content_trim, "/task groups") == 0)
    {
        edc_commands_handle_task_groups(agent, actor, message->type);
    }
    else if (strncmp(content_trim, "/task chain ", 12) == 0)
    {
        arg = sdsnew(content_trim + 12);
        edc_commands_handle_task_chain(agent, actor, message->type, arg);
    }
    else if (strncmp(content_trim, "/task join ", 11) == 0)
    {
        arg = sdsnew(content_trim + 11);
        edc_commands_handle_task_join(agent, actor, message->type, arg);
    }
    else if (strncmp(content_trim, "/task prompt ", 13) == 0)
    {
        arg = sdsnew(content_trim + 13);
        edc_commands_handle_task_prompt(agent, actor, message->type, arg);
    }
    else
    {
        /* Unknown /task subcommand — report error instead of falling through */
        sds reply = sdscatprintf(sdsempty(), "Unknown /task subcommand. Type /help for available commands.");
        edc_commands_reply(agent, actor, message->type, reply);
        sdsfree(reply);
        sdsfree(content_trim);
        return true;
    }

    sdsfree(content_trim);
    if (arg)
        sdsfree(arg);
    return true;
}

/* ── 入口 ──────────────────────────────────────────────────── */

bool edc_commands_handler(struct edc_agent *agent, struct edc_actor *actor, struct edc_channel_message *message)
{
    if (!message || !message->type || !message->content)
        return false;

    /* / commands: shell builtins first, then shortcuts, then existing handlers */
    if (message->content[0] == '/')
    {
        if (edc_shell_handle_builtin(agent, actor, message))
            return true;

        {
            sds trimmed = sdstrim(sdsnew(message->content), " \t\r\n");

            if (strcmp(trimmed, "/tasks") == 0)
            {
                edc_commands_handle_task_list(agent, actor, message->type);
                sdsfree(trimmed);
                return true;
            }
            if (strcmp(trimmed, "/bg") == 0)
            {
                edc_commands_handle_task_bg(agent, actor, message->type, NULL);
                sdsfree(trimmed);
                return true;
            }
            if (strncmp(trimmed, "/bg ", 4) == 0)
            {
                sds arg = sdsnew(trimmed + 4);
                sdstrim(arg, " \t");
                edc_commands_handle_task_bg(agent, actor, message->type, arg);
                sdsfree(trimmed);
                return true;
            }
            if (strncmp(trimmed, "/fg ", 4) == 0)
            {
                sds arg = sdsnew(trimmed + 4);
                sdstrim(arg, " \t");
                edc_commands_handle_task_fg(agent, actor, message->type, arg);
                sdsfree(trimmed);
                return true;
            }
            sdsfree(trimmed);
        }

        if (edc_commands_is_prefix(message->content, "/task"))
            return edc_commands_handle_task(agent, actor, message);

        if (edc_commands_is_prefix(message->content, "/session"))
            return edc_commands_handle_session(agent, actor, message);

        if (edc_commands_is_target(message->content, "/new"))
            return edc_commands_handle_new(agent, actor, message->type);
        if (edc_commands_is_target(message->content, "/stop"))
            return edc_commands_handle_stop(agent, actor, message->type);
        if (edc_commands_is_target(message->content, "/help"))
            return edc_commands_handle_help(agent, actor, message->type);
        if (edc_commands_is_target(message->content, "/exit"))
            return edc_commands_handle_exit(agent, actor, message->type);
        if (edc_commands_is_target(message->content, "/save"))
            return edc_commands_handle_save(agent, actor, message->type);

        sds reply =
            sdscatprintf(sdsempty(), "Unknown command: %s\nType /help for available commands.", message->content);
        edc_commands_reply(agent, actor, message->type, reply);
        sdsfree(reply);
        return true;
    }

    /* non-/ input: delegate to shell module */
    return edc_shell_execute(agent, actor, message);
}
