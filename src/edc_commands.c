#include "sds.h"
#include "edc_commands.h"
#include "edc_actor.h"
#include "edc_agent.h"
#include "edc_compat.h"
#include "edc_channel_actor.h"
#include "edc_memory_actor.h"
#include "edc_scheduler.h"
#include "edc_session.h"
#include "edc_task.h"
#include "edc_task_group.h"
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
    if (strncmp(cmd, prefix, plen) != 0) { sdsfree(cmd); return false; }
    const char *arg = cmd + plen;
    while (*arg == ' ') arg++;
    if (*arg == '\0') { sdsfree(cmd); *out = NULL; return false; }
    *out = sdsnew(arg);
    sdsfree(cmd);
    return true;
}

static void edc_commands_reply(struct edc_agent *agent, edc_actor_t *actor,
                                struct edc_channel *channel, const char *text)
{
    struct edc_channel_message *reply =
        edc_channel_message_new(edc_channel_copy(channel), sdsnew(text));
    edc_actor_send(agent->channel_actor,
                   edc_actor_msg_new_sub_type(BC_ACTOR_MSG_CHANNEL_SEND,
                                              reply->channel->type, actor, NULL, reply,
                                              (edc_actor_data_free_fn)edc_channel_message_free));
}

/* ── /new ──────────────────────────────────────────────────── */

static bool edc_commands_handle_new(struct edc_agent *agent, edc_actor_t *actor,
                                     struct edc_channel *channel)
{
    edc_session_reload(agent->session, false);
    edc_agent_remove_all_turns(agent);

    if (!edc_memory_actor_is_compressing(agent->memory_actor))
    {
        edc_actor_send(agent->memory_actor,
                       edc_actor_msg_new(BC_ACTOR_MSG_MEMORY_COMPRESS,
                                         edc_scheduler_get_actor(agent->scheduler),
                                         NULL, (void *)(intptr_t)1, NULL));
    }

    edc_commands_reply(agent, actor, channel, "[system] start new session now.");
    log_info("receive /new command");
    return true;
}

/* ── /stop ─────────────────────────────────────────────────── */

static bool edc_commands_handle_stop(struct edc_agent *agent, edc_actor_t *actor,
                                      struct edc_channel *channel)
{
    edc_session_reload(agent->session, true);
    edc_agent_remove_all_turns(agent);

    edc_commands_reply(agent, actor, channel, "[system] stop all turns.");
    return true;
}

/* ── /help ─────────────────────────────────────────────────── */

static bool edc_commands_handle_help(struct edc_agent *agent, edc_actor_t *actor,
                                      struct edc_channel *channel)
{
    const char *text =
        "# EdgeClaw Commands\n\n"
        "**Session:** `/new` `/stop` `/help` `/exit`\n"
        "**Task:** `/task new <desc>` `/task list` `/task switch <id>` "
        "`/task stop [id]` `/task status`\n"
        "**Group:** `/task fg <id>` `/task bg` `/task groups`\n"
        "**Chain:** `/task chain <src> <dst>` `/task chains [id]`\n"
        "**Prompt:** `/task prompt <id> [name|--text \"...\"]`\n";

    edc_commands_reply(agent, actor, channel, text);
    return true;
}

/* ── /exit ─────────────────────────────────────────────────── */

static bool edc_commands_handle_exit(struct edc_agent *agent, edc_actor_t *actor,
                                      struct edc_channel *channel)
{
    edc_commands_reply(agent, actor, channel, "[system] shutting down...");
    log_info("received /exit command");
    _run_loop = 0;
    return true;
}

/* ── /task commands ─────────────────────────────────────────― */

static void edc_commands_handle_task_new(struct edc_agent *agent, edc_actor_t *actor,
                                          struct edc_channel *channel, sds arg)
{
    struct edc_task *task = edc_scheduler_create_task(agent->scheduler,
                                                       arg ? arg : "New task", channel);
    struct edc_task_group *group = edc_scheduler_create_group(agent->scheduler, task);
    edc_scheduler_set_foreground(agent->scheduler, group);

    sds reply = sdscatprintf(sdsempty(), "[task] created %s (group %s)",
                             task->task_id, group->group_id);
    edc_commands_reply(agent, actor, channel, reply);
    sdsfree(reply);
}

static void edc_commands_handle_task_list(struct edc_agent *agent, edc_actor_t *actor,
                                           struct edc_channel *channel)
{
    sds out = sdsnew("Tasks:\n");
    struct edc_task_group *g;
    DL_FOREACH(agent->scheduler->groups, g)
    {
        bool fg = agent->scheduler->foreground_group == g;
        out = sdscatprintf(out, "  [%s%s] group %s  (%d members)\n",
                           fg ? "*" : " ",
                           g->is_orphaned ? "!" : " ",
                           g->group_id, g->member_count);

        struct edc_task *t;
        LL_FOREACH2(g->members, t, group_next)
        {
            out = sdscatprintf(out, "    [%s] %-14s  %-16s  %s\n",
                               t == g->leader ? "L" : "M",
                               t->task_id,
                               edc_task_state_name(t->state),
                               t->description);
        }
    }
    edc_commands_reply(agent, actor, channel, out);
    sdsfree(out);
}

static void edc_commands_handle_task_switch(struct edc_agent *agent, edc_actor_t *actor,
                                              struct edc_channel *channel, sds arg)
{
    if (!arg) { edc_commands_reply(agent, actor, channel, "/task switch <id>"); return; }
    struct edc_task *t = edc_session_find_task(agent->session, arg);
    if (!t) { edc_commands_reply(agent, actor, channel, "task not found"); return; }
    edc_session_set_active_task(agent->session, t);
    sds reply = sdscatprintf(sdsempty(), "switched to task %s", t->task_id);
    edc_commands_reply(agent, actor, channel, reply);
    sdsfree(reply);
}

static void edc_commands_handle_task_stop(struct edc_agent *agent, edc_actor_t *actor,
                                            struct edc_channel *channel, sds arg)
{
    if (!arg) { edc_commands_reply(agent, actor, channel, "/task stop <id>"); return; }
    edc_scheduler_send_signal_to_task(agent->scheduler, arg, TASK_SIGTERM);
    edc_commands_reply(agent, actor, channel, "stop signal sent");
}

static void edc_commands_handle_task_fg(struct edc_agent *agent, edc_actor_t *actor,
                                         struct edc_channel *channel, sds arg)
{
    if (!arg) { edc_commands_reply(agent, actor, channel, "/task fg <id>"); return; }
    struct edc_task *t = edc_session_find_task(agent->session, arg);
    if (!t) { edc_commands_reply(agent, actor, channel, "task not found"); return; }
    struct edc_task_group *g = edc_session_find_group(agent->session, t->group_id);
    if (!g) { edc_commands_reply(agent, actor, channel, "group not found"); return; }
    edc_scheduler_set_foreground(agent->scheduler, g);
    sds reply = sdscatprintf(sdsempty(), "foreground group: %s", g->group_id);
    edc_commands_reply(agent, actor, channel, reply);
    sdsfree(reply);
}

static void edc_commands_handle_task_bg(struct edc_agent *agent, edc_actor_t *actor,
                                         struct edc_channel *channel)
{
    edc_scheduler_set_foreground(agent->scheduler, NULL);
    edc_commands_reply(agent, actor, channel, "no foreground group");
}

static void edc_commands_handle_task_groups(struct edc_agent *agent, edc_actor_t *actor,
                                              struct edc_channel *channel)
{
    sds out = sdsnew("Task Groups:\n");
    struct edc_task_group *g;
    DL_FOREACH(agent->scheduler->groups, g)
    {
        out = sdscatprintf(out, "  [%s%s] %s  %d members  %s\n",
                           agent->scheduler->foreground_group == g ? "*" : " ",
                           g->is_orphaned ? "!" : " ",
                           g->group_id, g->member_count,
                           edc_task_group_status_label(g));
    }
    edc_commands_reply(agent, actor, channel, out);
    sdsfree(out);
}

static void edc_commands_handle_task_status(struct edc_agent *agent, edc_actor_t *actor,
                                              struct edc_channel *channel)
{
    struct edc_task *t = edc_session_get_active_task(agent->session);
    if (!t) { edc_commands_reply(agent, actor, channel, "no active task"); return; }

    sds out = sdscatprintf(sdsempty(),
                           "task: %s\n"
                           "state: %s\n"
                           "group: %s\n"
                           "desc: %s\n"
                           "deps: %zu/%zu completed\n",
                           t->task_id, edc_task_state_name(t->state),
                           t->group_id, t->description,
                           t->deps_completed, t->deps_total);
    if (t->output)
    {
        out = sdscatprintf(out, "output: %s\n", t->output);
    }
    if (t->prompt_name)
    {
        out = sdscatprintf(out, "prompt: %s\n", t->prompt_name);
    }
    if (t->forked_from)
    {
        out = sdscatprintf(out, "forked from: %s\n", t->forked_from->task_id);
    }
    edc_commands_reply(agent, actor, channel, out);
    sdsfree(out);
}

static void edc_commands_handle_task_chain(struct edc_agent *agent, edc_actor_t *actor,
                                            struct edc_channel *channel, sds arg)
{
    /* /task chain <src> <dst> */
    if (!arg) { edc_commands_reply(agent, actor, channel, "/task chain <src> <dst>"); return; }

    int count = 0;
    sds *tokens = sdssplitlen(arg, sdslen(arg), " ", 1, &count);
    if (count < 2)
    {
        edc_commands_reply(agent, actor, channel, "usage: /task chain <source_id> <target_id>");
        sdsfreesplitres(tokens, count);
        sdsfree(arg);
        return;
    }

    struct edc_task *src = edc_session_find_task(agent->session, tokens[0]);
    struct edc_task *dst = edc_session_find_task(agent->session, tokens[1]);
    sdsfreesplitres(tokens, count);

    if (!src) { edc_commands_reply(agent, actor, channel, "source task not found"); return; }
    if (!dst) { edc_commands_reply(agent, actor, channel, "target task not found"); return; }

    if (edc_task_add_dependency(dst, src, TASK_DEP_JOIN))
    {
        sds reply = sdscatprintf(sdsempty(), "chain: %s → %s  (dep %zu/%zu)",
                                 src->task_id, dst->task_id,
                                 dst->deps_completed, dst->deps_total);
        edc_commands_reply(agent, actor, channel, reply);
        sdsfree(reply);
    }
    else
    {
        edc_commands_reply(agent, actor, channel, "failed to add dependency");
    }
}

static void edc_commands_handle_task_prompt(struct edc_agent *agent, edc_actor_t *actor,
                                             struct edc_channel *channel, sds arg)
{
    /* /task prompt <id> <name>  or  /task prompt <id> --text "..." */
    if (!arg) { edc_commands_reply(agent, actor, channel, "/task prompt <id> [name|--text \"...\"]"); return; }
    /* simplified: not parsing --text for now */
    edc_commands_reply(agent, actor, channel, "prompt set (stub)");
}

static bool edc_commands_handle_task(struct edc_agent *agent, edc_actor_t *actor,
                                      struct edc_channel_message *message)
{
    const char *content = message->content;
    sds content_trim = sdstrim(sdsnew(content), " \t\r\n");

    sds arg = NULL;

    if (strncmp(content_trim, "/task new ", 10) == 0)
    {
        arg = sdsnew(content_trim + 10);
        edc_commands_handle_task_new(agent, actor, message->channel, arg);
    }
    else if (strcmp(content_trim, "/task list") == 0)
    {
        edc_commands_handle_task_list(agent, actor, message->channel);
    }
    else if (strncmp(content_trim, "/task switch ", 13) == 0)
    {
        arg = sdsnew(content_trim + 13);
        edc_commands_handle_task_switch(agent, actor, message->channel, arg);
    }
    else if (strncmp(content_trim, "/task stop ", 11) == 0)
    {
        arg = sdsnew(content_trim + 11);
        edc_commands_handle_task_stop(agent, actor, message->channel, arg);
    }
    else if (strcmp(content_trim, "/task status") == 0)
    {
        edc_commands_handle_task_status(agent, actor, message->channel);
    }
    else if (strncmp(content_trim, "/task fg ", 9) == 0)
    {
        arg = sdsnew(content_trim + 9);
        edc_commands_handle_task_fg(agent, actor, message->channel, arg);
    }
    else if (strcmp(content_trim, "/task bg") == 0)
    {
        edc_commands_handle_task_bg(agent, actor, message->channel);
    }
    else if (strcmp(content_trim, "/task groups") == 0)
    {
        edc_commands_handle_task_groups(agent, actor, message->channel);
    }
    else if (strncmp(content_trim, "/task chain ", 12) == 0)
    {
        arg = sdsnew(content_trim + 12);
        edc_commands_handle_task_chain(agent, actor, message->channel, arg);
    }
    else if (strncmp(content_trim, "/task prompt ", 13) == 0)
    {
        arg = sdsnew(content_trim + 13);
        edc_commands_handle_task_prompt(agent, actor, message->channel, arg);
    }
    else
    {
        sdsfree(content_trim);
        return false;
    }

    sdsfree(content_trim);
    if (arg) sdsfree(arg);
    return true;
}

/* ── 入口 ──────────────────────────────────────────────────── */

bool edc_commands_handler(struct edc_agent *agent, struct edc_actor *actor,
                          struct edc_channel_message *message)
{
    if (!message || !message->channel || !message->content)
        return false;

    /* /task prefix */
    if (edc_commands_is_prefix(message->content, "/task"))
    {
        return edc_commands_handle_task(agent, actor, message);
    }

    if (edc_commands_is_target(message->content, "/new"))
        return edc_commands_handle_new(agent, actor, message->channel);
    if (edc_commands_is_target(message->content, "/stop"))
        return edc_commands_handle_stop(agent, actor, message->channel);
    if (edc_commands_is_target(message->content, "/help"))
        return edc_commands_handle_help(agent, actor, message->channel);
    if (edc_commands_is_target(message->content, "/exit"))
        return edc_commands_handle_exit(agent, actor, message->channel);

    return false;
}
