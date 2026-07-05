#include "sds.h"
#include "edc_commands.h"
#include "edc_actor.h"
#include "edc_agent.h"
#include "edc_compat.h"
#include "edc_channel_actor.h"
#include "edc_cmd_parse.h"
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
        "## EdgeClaw Commands\n"
        "\n"
        "- /new  ·  /stop  ·  /help  ·  /exit\n"
        "- /task new <desc>   [--bg]\n"
        "- /task fork <parent> <desc>\n"
        "- /task list  ·  /task status [id]  ·  /task output <id>\n"
        "- /task groups  ·  /task fg <id>  ·  /task bg\n"
        "- /task switch <id>  ·  /task stop <id>  [--force]\n"
        "- /task suspend <id>  ·  /task resume <id>\n"
        "- /task chain <src> <dst>  ·  /task join <id>\n"
        "- /task prompt <id> <name>\n";

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
    struct edc_cmd_task_opts opts;
    edc_cmd_parse_task_opts(arg, &opts);

    struct edc_task *task = edc_scheduler_create_task(agent->scheduler,
                                                       (opts.desc && sdslen(opts.desc) > 0) ? opts.desc : "New task",
                                                       edc_channel_copy(channel));
    if (opts.prompt_name) edc_task_set_prompt(task, opts.prompt_name, NULL);

    if (opts.group_id)
    {
        struct edc_task_group *group = edc_session_find_group(agent->session, opts.group_id);
        if (!group)
        {
            sds reply = sdscatprintf(sdsempty(), "group %s not found, creating new group", opts.group_id);
            edc_commands_reply(agent, actor, channel, reply);
            sdsfree(reply);
            group = edc_scheduler_create_group(agent->scheduler, task);
        }
        else
        {
            edc_task_group_add_task(group, task);
            sds reply = sdscatprintf(sdsempty(), "[task] created %s in group %s",
                                     task->task_id, group->group_id);
            edc_commands_reply(agent, actor, channel, reply);
            sdsfree(reply);
            goto done;
        }
    }
    else
    {
        struct edc_task_group *group = edc_scheduler_create_group(agent->scheduler, task);

        if (opts.bg)
        {
            sds reply = sdscatprintf(sdsempty(), "[task] created %s (group %s) in background",
                                     task->task_id, group->group_id);
            edc_commands_reply(agent, actor, channel, reply);
            sdsfree(reply);
        }
        else
        {
            edc_scheduler_set_foreground(agent->scheduler, group);
            edc_session_set_active_task(agent->session, task);
            sds reply = sdscatprintf(sdsempty(), "[task] created %s (group %s)",
                                     task->task_id, group->group_id);
            edc_commands_reply(agent, actor, channel, reply);
            sdsfree(reply);
        }
    }

    /* add deps from -d flags */
    for (int i = 0; i < opts.dep_count; i++)
    {
        struct edc_task *dt = edc_session_find_task(agent->session, opts.dep_ids[i]);
        if (dt) edc_task_add_dependency(task, dt, TASK_DEP_JOIN);
    }

done:
    edc_cmd_task_opts_free(&opts);
}

static void edc_commands_handle_task_list(struct edc_agent *agent, edc_actor_t *actor,
                                           struct edc_channel *channel)
{
    sds out = sdsempty();
    struct edc_task_group *g;
    bool first = true;
    DL_FOREACH(agent->scheduler->groups, g)
    {
        if (!first) out = sdscat(out, "\n");
        first = false;

        bool fg = agent->scheduler->foreground_group == g;
        out = sdscatprintf(out, "- %s%s%s (%d)\n",
                           fg ? "[fg] " : "",
                           g->is_orphaned ? "[orphan] " : "",
                           g->group_id, g->member_count);

        struct edc_task *t;
        LL_FOREACH2(g->members, t, group_next)
        {
            bool is_active = agent->session && agent->session->active_task == t;
            out = sdscatprintf(out, "    - %s%s %-12s %s",
                               t == g->leader ? "[L]" : "[M]",
                               is_active ? "*" : " ",
                               t->task_id,
                               edc_task_state_name(t->state));
            if (*t->description)
                out = sdscatprintf(out, "  %s", t->description);
            out = sdscat(out, "\n");
        }
    }
    if (!first) ; /* at least one group */
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
    edc_session_set_active_task(agent->session, t);
    sds reply = sdscatprintf(sdsempty(), "foreground: group %s (active: %s)",
                             g->group_id, t->task_id);
    edc_commands_reply(agent, actor, channel, reply);
    sdsfree(reply);
}

static void edc_commands_handle_task_bg(struct edc_agent *agent, edc_actor_t *actor,
                                         struct edc_channel *channel)
{
    edc_scheduler_set_foreground(agent->scheduler, NULL);
    edc_commands_reply(agent, actor, channel, "no foreground group");
}

static void edc_commands_handle_task_fork(struct edc_agent *agent, edc_actor_t *actor,
                                           struct edc_channel *channel, sds arg)
{
    /* /task fork <parent_id> <desc> [--prompt <name>] */
    if (!arg) { edc_commands_reply(agent, actor, channel, "/task fork <parent_id> <desc> [--prompt <name>]"); return; }

    int count = 0;
    sds *tokens = sdssplitlen(arg, sdslen(arg), " ", 1, &count);
    if (count < 2)
    {
        edc_commands_reply(agent, actor, channel, "usage: /task fork <parent_id> <desc> [--prompt <name>]");
        sdsfreesplitres(tokens, count);
        return;
    }

    struct edc_task *parent = edc_session_find_task(agent->session, tokens[0]);
    if (!parent)
    {
        sds reply = sdscatprintf(sdsempty(), "parent task %s not found", tokens[0]);
        edc_commands_reply(agent, actor, channel, reply);
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
            if (sdslen(desc) > 0) desc = sdscat(desc, " ");
            desc = sdscat(desc, tokens[i]);
        }
    }
    sdsfreesplitres(tokens, count);

    size_t fork_point = parent->group ? (size_t)cJSON_GetArraySize(parent->group->history) : 0;
    struct edc_task *child = edc_task_fork(parent, NULL /* task_id generated by scheduler */,
                                            desc, fork_point, edc_channel_copy(channel));
    if (prompt_name) edc_task_set_prompt(child, prompt_name, NULL);

    if (parent->group)
        edc_task_group_add_task(parent->group, child);

    sds reply = sdscatprintf(sdsempty(), "[fork] created %s from %s (group %s, fork_point=%zu)%s",
                             child->task_id, parent->task_id,
                             child->group_id, fork_point,
                             prompt_name ? " prompt=" : "");
    if (prompt_name) reply = sdscat(reply, prompt_name);
    edc_commands_reply(agent, actor, channel, reply);
    sdsfree(reply);
    sdsfree(desc);
    if (prompt_name) sdsfree(prompt_name);
}

static void edc_commands_handle_task_groups(struct edc_agent *agent, edc_actor_t *actor,
                                              struct edc_channel *channel)
{
    sds out = sdsnew("## Groups\n\n");
    struct edc_task_group *g;
    DL_FOREACH(agent->scheduler->groups, g)
    {
        out = sdscatprintf(out, "- %s%s  **%s**  %d members  _%s_\n",
                           agent->scheduler->foreground_group == g ? "`[fg]` " : "",
                           g->is_orphaned ? "`[orphan]` " : "",
                           g->group_id, g->member_count,
                           edc_task_group_status_label(g));
    }
    edc_commands_reply(agent, actor, channel, out);
    sdsfree(out);
}

static void edc_commands_handle_task_status(struct edc_agent *agent, edc_actor_t *actor,
                                              struct edc_channel *channel, sds arg)
{
    struct edc_task *t;
    if (arg)
    {
        t = edc_session_find_task(agent->session, arg);
        if (!t) { edc_commands_reply(agent, actor, channel, "task not found"); return; }
    }
    else
    {
        t = edc_session_get_active_task(agent->session);
        if (!t) { edc_commands_reply(agent, actor, channel, "no active task"); return; }
    }

    sds out = sdscatprintf(sdsempty(),
                           "## Task `%s`\n\n"
                           "- **state**: %s\n"
                           "- **group**: %s\n"
                           "- **desc**: %s\n"
                           "- **deps**: %zu/%zu completed\n"
                           "- **turns**: %zu\n",
                           t->task_id, edc_task_state_name(t->state),
                           t->group_id, t->description,
                           t->deps_completed, t->deps_total,
                           (size_t)edc_queue_length(&t->turns));
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
    edc_commands_reply(agent, actor, channel, out);
    sdsfree(out);
}

static void edc_commands_handle_task_output(struct edc_agent *agent, edc_actor_t *actor,
                                             struct edc_channel *channel, sds arg)
{
    if (!arg) { edc_commands_reply(agent, actor, channel, "/task output <id>"); return; }
    struct edc_task *t = edc_session_find_task(agent->session, arg);
    if (!t) { edc_commands_reply(agent, actor, channel, "task not found"); return; }
    if (!t->output) { edc_commands_reply(agent, actor, channel, "(no output)"); return; }
    edc_commands_reply(agent, actor, channel, t->output);
}

static void edc_commands_handle_task_join(struct edc_agent *agent, edc_actor_t *actor,
                                           struct edc_channel *channel, sds arg)
{
    if (!arg) { edc_commands_reply(agent, actor, channel, "/task join <id>"); return; }
    struct edc_task *t = edc_session_find_task(agent->session, arg);
    if (!t) { edc_commands_reply(agent, actor, channel, "task not found"); return; }

    if (t->state == TASK_COMPLETED)
    {
        /* find or create a target task to inject output into */
        struct edc_task *active = edc_session_get_active_task(agent->session);
        if (!active
            || (active->state != TASK_WAITING_INPUT && active->state != TASK_WAITING_DEPS))
        {
            /* no usable active task — create one in foreground group */
            struct edc_task_group *fg = agent->scheduler->foreground_group;
            struct edc_channel *ch = NULL;
            if (fg && fg->members) ch = fg->members->channel;
            active = edc_scheduler_create_task(agent->scheduler, "Joined task", ch);
            if (fg)
            {
                edc_task_group_add_task(fg, active);
                edc_session_set_active_task(agent->session, active);
            }
            else
            {
                struct edc_task_group *g = edc_scheduler_create_group(agent->scheduler, active);
                edc_scheduler_set_foreground(agent->scheduler, g);
            }
        }

        /* auto-chain: inject output into active task */
        edc_task_add_dependency(active, t, TASK_DEP_JOIN);

        sds reply = sdscatprintf(sdsempty(), "[task %s output]:\n%s\n\n→ injected into %s\n→ %s reaped",
                                 t->task_id,
                                 t->output ? t->output : "(empty)",
                                 active->task_id,
                                 t->task_id);
        edc_commands_reply(agent, actor, channel, reply);
        sdsfree(reply);

        /* reap source */
        if (t->group) edc_task_group_remove_task(t->group, t);
        edc_task_free(t);
        return;
    }

    if (t->state == TASK_CANCELLED || t->state == TASK_ERROR)
    {
        sds reply = sdscatprintf(sdsempty(), "[task %s] %s, reaped", t->task_id,
                                 edc_task_state_name(t->state));
        edc_commands_reply(agent, actor, channel, reply);
        sdsfree(reply);
        if (t->group) edc_task_group_remove_task(t->group, t);
        edc_task_free(t);
        return;
    }

    sds reply = sdscatprintf(sdsempty(),
                              "task %s is currently %s. Try:\n"
                              "  /task chain %s <current>   — current task waits for it\n"
                              "  /task join %s              — check back later when COMPLETED",
                              t->task_id, edc_task_state_name(t->state),
                              t->task_id, t->task_id);
    edc_commands_reply(agent, actor, channel, reply);
    sdsfree(reply);
}

static void edc_commands_handle_task_suspend(struct edc_agent *agent, edc_actor_t *actor,
                                              struct edc_channel *channel, sds arg)
{
    struct edc_task *t;
    if (arg) t = edc_session_find_task(agent->session, arg);
    else     t = edc_session_get_active_task(agent->session);
    if (!t) { edc_commands_reply(agent, actor, channel, "task not found"); return; }
    if (!edc_task_set_state(t, TASK_SUSPENDED))
    {
        edc_commands_reply(agent, actor, channel, "cannot suspend in current state");
        return;
    }
    sds reply = sdscatprintf(sdsempty(), "task %s suspended", t->task_id);
    edc_commands_reply(agent, actor, channel, reply);
    sdsfree(reply);
}

static void edc_commands_handle_task_resume(struct edc_agent *agent, edc_actor_t *actor,
                                             struct edc_channel *channel, sds arg)
{
    struct edc_task *t;
    if (arg) t = edc_session_find_task(agent->session, arg);
    else     t = edc_session_get_active_task(agent->session);
    if (!t) { edc_commands_reply(agent, actor, channel, "task not found"); return; }
    if (t->state != TASK_SUSPENDED)
    {
        sds reply = sdscatprintf(sdsempty(), "task %s is %s, not SUSPENDED", t->task_id,
                                 edc_task_state_name(t->state));
        edc_commands_reply(agent, actor, channel, reply);
        sdsfree(reply);
        return;
    }
    edc_task_set_state(t, TASK_WAITING_INPUT);
    sds reply = sdscatprintf(sdsempty(), "task %s resumed", t->task_id);
    edc_commands_reply(agent, actor, channel, reply);
    sdsfree(reply);
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
    else if (strncmp(content_trim, "/task fork ", 11) == 0)
    {
        arg = sdsnew(content_trim + 11);
        edc_commands_handle_task_fork(agent, actor, message->channel, arg);
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
        edc_commands_handle_task_status(agent, actor, message->channel, NULL);
    }
    else if (strncmp(content_trim, "/task status ", 13) == 0)
    {
        arg = sdsnew(content_trim + 13);
        edc_commands_handle_task_status(agent, actor, message->channel, arg);
    }
    else if (strncmp(content_trim, "/task output ", 13) == 0)
    {
        arg = sdsnew(content_trim + 13);
        edc_commands_handle_task_output(agent, actor, message->channel, arg);
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
    else if (strncmp(content_trim, "/task join ", 11) == 0)
    {
        arg = sdsnew(content_trim + 11);
        edc_commands_handle_task_join(agent, actor, message->channel, arg);
    }
    else if (strncmp(content_trim, "/task prompt ", 13) == 0)
    {
        arg = sdsnew(content_trim + 13);
        edc_commands_handle_task_prompt(agent, actor, message->channel, arg);
    }
    else
    {
        /* Unknown /task subcommand — report error instead of falling through */
        sds reply = sdscatprintf(sdsempty(),
            "Unknown /task subcommand. Type /help for available commands.");
        edc_commands_reply(agent, actor, message->channel, reply);
        sdsfree(reply);
        sdsfree(content_trim);
        return true;
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

    if (message->content[0] == '@')
    {
        /* @desc "input" [--bg] [-d <id>]...   or   @a | @b | @c  (pipe chain) */
        sds arg = sdsnew(message->content + 1);
        sdstrim(arg, " \t");

        int pipe_count = 0;
        sds *pipe_seg = sdssplitlen(arg, sdslen(arg), "|", 1, &pipe_count);
        struct edc_task *prev_task = NULL;

        for (int pi = 0; pi < pipe_count; pi++)
        {
            sdstrim(pipe_seg[pi], " \t");
            if (sdslen(pipe_seg[pi]) == 0) continue;

            /* strip leading '@' from subsequent pipe segments */
            if (pipe_seg[pi][0] == '@')
            {
                sds trimmed = sdsnew(pipe_seg[pi] + 1);
                sdstrim(trimmed, " \t");
                sdsfree(pipe_seg[pi]);
                pipe_seg[pi] = trimmed;
            }

            struct edc_cmd_task_opts opts;
            edc_cmd_parse_task_opts(pipe_seg[pi], &opts);

            struct edc_task *task = edc_scheduler_create_task(agent->scheduler,
                                                               (opts.desc && sdslen(opts.desc) > 0) ? opts.desc : "Quick task",
                                                               edc_channel_copy(message->channel));
            struct edc_task_group *group = edc_scheduler_create_group(agent->scheduler, task);
            if (!opts.bg)
            {
                edc_scheduler_set_foreground(agent->scheduler, group);
                edc_session_set_active_task(agent->session, task);
            }

            for (int i = 0; i < opts.dep_count; i++)
            {
                struct edc_task *dt = edc_session_find_task(agent->session, opts.dep_ids[i]);
                if (dt) edc_task_add_dependency(task, dt, TASK_DEP_JOIN);
            }

            /* pipe chain: each segment waits for the previous one */
            if (prev_task)
                edc_task_add_dependency(task, prev_task, TASK_DEP_JOIN);

            sds reply = sdscatprintf(sdsempty(), "[@] created %s (group %s)%s%s%s",
                                     task->task_id, group->group_id,
                                     opts.bg ? " [bg]" : "",
                                     opts.dep_count > 0 ? " [chained]" : "",
                                     pi > 0 ? " [pipe]" : "");
            edc_commands_reply(agent, actor, message->channel, reply);
            sdsfree(reply);

            if (opts.input)
            {
                struct edc_turn *turn = edc_turn_new();
                turn->task_id = strdup(task->task_id);
                edc_turn_add_user_message(turn, opts.input);
                task->active_turn = turn;

                /* register with unique request_id (int64 counter, won't collide with pointers) */
                static int64_t counter = 0;
                void *req_id = (void *)(intptr_t)(++counter);
                edc_scheduler_register_task(agent->scheduler, task, req_id);
                task->current_request_id = req_id;

                log_info("@ handler: task=%s state=%s deps_total=%zu input='%s'",
                         task->task_id, edc_task_state_name(task->state),
                         task->deps_total, opts.input);

                /* only send_llm if not blocked by pipe deps */
                if (task->state != TASK_WAITING_DEPS)
                {
                    bool ok = edc_task_set_state(task, TASK_LLM_PROCESSING);
                    log_info("@ handler: set_state LLM_PROCESSING -> %s, send_llm=%s",
                             ok ? "ok" : "FAILED",
                             (agent->scheduler && agent->scheduler->send_llm) ? "ready" : "NULL");
                    if (ok && agent->scheduler && agent->scheduler->send_llm)
                        agent->scheduler->send_llm(agent->scheduler, task, req_id);
                }
                else
                {
                    log_info("@ handler: task %s blocked in WAITING_DEPS, turn queued", task->task_id);
                }
            }

            edc_cmd_task_opts_free(&opts);
            prev_task = task;
        }

        sdsfreesplitres(pipe_seg, pipe_count);
        sdsfree(arg);
        return true;
    }

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

    /* Input starts with '/' but no command matched — treat as invalid command */
    if (message->content[0] == '/')
    {
        sds reply = sdscatprintf(sdsempty(),
            "Unknown command: %s\nType /help for available commands.",
            message->content);
        edc_commands_reply(agent, actor, message->channel, reply);
        sdsfree(reply);
        return true;
    }

    return false;
}
