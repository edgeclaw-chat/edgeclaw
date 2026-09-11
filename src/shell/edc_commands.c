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

/* 严格命令前缀: content 去空白后等于 prefix, 或以 prefix + 空格 开头。
 * 专用于命令路由 —— 宽松前缀会把 "/tasks", "/sessions" 之类误吞成
 * "/task"/"/session" 的子命令, 报出误导性的 "Unknown subcommand"。 */
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

void edc_commands_reply(struct edc_agent *agent, edc_actor_t *actor, edc_channel_type channel_type, const char *text)
{
    struct edc_channel_message *reply = edc_channel_message_new(channel_type, EDC_CHANNEL_MESSAGE_SYS_TYPE, sdsnew(text));
    edc_actor_send(agent->channel_actor,
                   edc_actor_msg_new_sub_type(BC_ACTOR_MSG_CHANNEL_SEND, reply->type, actor, NULL, reply,
                                              (edc_actor_data_free_fn)edc_channel_message_free));
}

/* ── /session stop ─────────────────────────────────────────── */

/* 取消当前 session 的全部非终结 turns 并立即落盘。原顶层 /stop 命令, 现收归
 * /session 命名空间 (无参数)。移除某个 session 请用 /session remove <name>。 */
static bool edc_commands_handle_session_stop(struct edc_agent *agent, edc_actor_t *actor, edc_channel_type channel_type)
{
    edc_session_reload(edc_session_manager_get_active(agent->session_manager), true);
    edc_agent_stop_all_turns(agent);

    edc_commands_reply(agent, actor, channel_type, "[session] stop all turns.");
    return true;
}

/* ── /help ─────────────────────────────────────────────────── */

static bool edc_commands_handle_help(struct edc_agent *agent, edc_actor_t *actor, edc_channel_type channel_type)
{
    const char *text = "## EdgeClaw Commands\n"
                       "\n"
                       "- /help\n"
                       "- /session new [name]  ·  /session switch <name>\n"
                       "- /session list  ·  /session info\n"
                       "- /session save  ·  /session stop\n"
                       "- /session remove <name>  ·  /session rename <old> <new>\n"
                       "\n"
                       "- /task new <desc>   [--bg]\n"
                       "- /task fork <parent> <desc>\n"
                       "- /task list  ·  /task status [id]  ·  /task output <id>\n"
                       "- /task groups  ·  /task fg <id>  ·  /task bg [id]\n"
                       "- /task stop [id]  ·  /task resume [id]   (default: active task of the foreground group)\n"
                       "- /task chain <src> <dst>  ·  /task join <id>\n"
                       "\n"
                       "- /whoami\n"
                       "- /model [name]\n"
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

/* ── /session save ─────────────────────────────────────────── */

/* 持久化当前 session 的 history + state。原顶层 /save 命令, 现收归 /session。 */
static bool edc_commands_handle_session_save(struct edc_agent *agent, edc_actor_t *actor, edc_channel_type channel_type)
{
    struct edc_session *active = edc_session_manager_get_active(agent->session_manager);
    if (!active)
    {
        edc_commands_reply(agent, actor, channel_type, "[session] no active session");
        return true;
    }

    edc_session_save_history(active);
    edc_session_save_state(active, agent->scheduler);
    edc_commands_reply(agent, actor, channel_type, "[session] saved.");
    log_info("receive /session save command");
    return true;
}

/* ── /session commands ─────────────────────────────────────── */

static bool edc_commands_handle_session_new(struct edc_agent *agent, edc_actor_t *actor, edc_channel_type channel_type,
                                            sds arg)
{
    /* edc_session_manager_create 会把 active 直接切到新 session, 所以 "旧
     * session" 必须在 create 之前取到, 否则拿到的是它自己。 */
    struct edc_session *old = edc_session_manager_get_active(agent->session_manager);
    const char *name = (arg && sdslen(arg) > 0) ? arg : NULL;
    struct edc_session *sess = edc_session_manager_create(agent->session_manager, name);
    if (!sess)
    {
        edc_commands_reply(agent, actor, channel_type, "[session] failed to create session");
        return true;
    }
    /* 离开即取消: 旧 session 的**运行中**任务 (LLM/工具调用) 打成 CANCELLED 并
     * 立即落盘; WAITING_INPUT / WAITING_DEPS / SUSPENDED 保持原状。此处
     * sched->session 仍指向 old (set_session 尚未调用), 所以落盘要走显式的
     * save_state(old, sched) —— 不能用 save_session 回调, 它保存的是已经被
     * create 切过去的 "当前" session。 */
    if (old && old != sess)
    {
        edc_scheduler_send_signal_to_session(agent->scheduler, old, TASK_SIGTERM, true);
        edc_session_save_history(old);
        edc_session_save_state(old, agent->scheduler);
    }
    edc_scheduler_set_session(agent->scheduler, sess);
    edc_session_reload(sess, true);

    /* 新建 session 时把历史压进长期记忆 (MEMORY.md / HISTORY.md, force=true)。
     * 这是原 /new 命令的职责 —— 该命令已删除, 由 /session new 承接, 否则全局
     * 记忆压缩将再无运行时触发点。 */
    if (!edc_memory_actor_is_compressing(agent->memory_actor))
    {
        edc_actor_send(agent->memory_actor,
                       edc_actor_msg_new(BC_ACTOR_MSG_MEMORY_COMPRESS, edc_scheduler_get_actor(agent->scheduler), NULL,
                                         (void *)(intptr_t)1, NULL));
    }

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
    struct edc_session *new_sess = edc_session_manager_find(agent->session_manager, arg);
    if (!new_sess)
    {
        sds reply = sdscatprintf(sdsempty(), "[session] session not found: %s", arg);
        edc_commands_reply(agent, actor, channel_type, reply);
        sdsfree(reply);
        return true;
    }
    struct edc_session *old = edc_session_manager_get_active(agent->session_manager);
    if (old == new_sess)
    {
        sds reply = sdscatprintf(sdsempty(), "[session] already on [%s] %s", new_sess->session_id,
                                 edc_session_get_name(new_sess));
        edc_commands_reply(agent, actor, channel_type, reply);
        sdsfree(reply);
        return true;
    }
    /* 离开即取消: 切走前把旧 session 的**运行中**任务 (LLM_PROCESSING /
     * TOOL_CALLING) 打成 CANCELLED 并立即落盘; WAITING_INPUT 等原样保留。
     * 必须在 set_session 之前做 —— 之后 sched->session 已指向新 session,
     * 信号会打到新 session 的任务上。 */
    if (old)
        edc_agent_stop_running_turns(agent);

    if (!edc_session_manager_switch(agent->session_manager, arg))
    {
        sds reply = sdscatprintf(sdsempty(), "[session] session not found: %s", arg);
        edc_commands_reply(agent, actor, channel_type, reply);
        sdsfree(reply);
        return true;
    }
    edc_scheduler_set_session(agent->scheduler, new_sess);
    edc_session_load_state(new_sess, agent->scheduler);
    edc_session_reload(new_sess, true);
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

static bool edc_commands_handle_session_remove(struct edc_agent *agent, edc_actor_t *actor,
                                               edc_channel_type channel_type, sds arg)
{
    if (!arg || sdslen(arg) == 0)
    {
        edc_commands_reply(agent, actor, channel_type, "[session] usage: /session remove <name_or_id>");
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
    /* 最后一个 session 不能 remove。先于取消任务拦住, 否则一次失败的 remove 会把
     * 当前 session 的任务白白打成 CANCELLED。 */
    if (agent->session_manager->session_count <= 1)
    {
        edc_commands_reply(agent, actor, channel_type, "[session] cannot remove the last session");
        return true;
    }

    sds stopped_id = sdsnew(target->session_id);
    sds stopped_name = sdsnew(edc_session_get_name(target));

    /* 离开即取消: 移除前把该 session 的**运行中**任务打成 CANCELLED
     * (WAITING_INPUT 等原样保留)。随后 edc_session_manager_stop 内部的 save
     * 会把内存里的 CANCELLED 落盘。目标可能不是当前 active session, 故用
     * session 版信号显式指定目标, 不依赖 sched->session。 */
    edc_scheduler_send_signal_to_session(agent->scheduler, target, TASK_SIGTERM, true);

    if (!edc_session_manager_stop(agent->session_manager, arg))
    {
        sdsfree(stopped_id);
        sdsfree(stopped_name);
        edc_commands_reply(agent, actor, channel_type, "[session] cannot remove the last session");
        return true;
    }
    struct edc_session *new_active = edc_session_manager_get_active(agent->session_manager);
    edc_scheduler_set_session(agent->scheduler, new_active);
    edc_session_reload(new_active, true);
    sds reply = sdscatprintf(sdsempty(), "[session] removed [%s] %s", stopped_id, stopped_name);
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
    sds rest = NULL;
    /* /session new 的 name 可选 —— 无参也要能建匿名 session。edc_commands_arg
     * 在无参时返回 false, 所以必须先做一次精确匹配, 否则裸 /session new 会掉到
     * 末尾的 unknown subcommand。 */
    if (edc_commands_is_target(msg->content, "/session new"))
        return edc_commands_handle_session_new(agent, actor, msg->type, NULL);
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
    if (edc_commands_is_target(msg->content, "/session info"))
        return edc_commands_handle_session_info(agent, actor, msg->type);
    if (edc_commands_is_target(msg->content, "/session save"))
        return edc_commands_handle_session_save(agent, actor, msg->type);
    if (edc_commands_is_target(msg->content, "/session stop"))
        return edc_commands_handle_session_stop(agent, actor, msg->type);
    if (edc_commands_arg(msg->content, "/session remove", &rest))
    {
        bool ret = edc_commands_handle_session_remove(agent, actor, msg->type, rest);
        sdsfree(rest);
        return ret;
    }
    if (edc_commands_is_target(msg->content, "/session remove"))
        return edc_commands_handle_session_remove(agent, actor, msg->type, NULL);
    if (edc_commands_arg(msg->content, "/session rename", &rest))
    {
        bool ret = edc_commands_handle_session_rename(agent, actor, msg->type, rest);
        sdsfree(rest);
        return ret;
    }
    if (edc_commands_is_target(msg->content, "/session rename"))
        return edc_commands_handle_session_rename(agent, actor, msg->type, NULL);
    /* /session stop 不接受参数 —— 移除 session 已改用 /session remove。 */
    if (edc_commands_is_prefix(msg->content, "/session stop"))
    {
        edc_commands_reply(agent, actor, msg->type,
                           "[session] /session stop takes no arguments; to remove a session use /session remove <name_or_id>");
        return true;
    }
    sds reply = sdscatprintf(sdsempty(),
                             "[session] unknown subcommand. Try: new, switch, list, info, save, stop, remove, rename");
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
    DL_FOREACH(edc_scheduler_get_session(agent->scheduler)->groups, g)
    {
        if (!first)
            out = sdscat(out, "\n");
        first = false;

        bool fg = edc_scheduler_get_foreground(agent->scheduler) == g;
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

static void edc_commands_handle_task_stop(struct edc_agent *agent, edc_actor_t *actor, edc_channel_type channel_type,
                                          sds arg)
{
    struct edc_session *sess = edc_session_manager_get_active(agent->session_manager);
    struct edc_task *t = NULL;

    if (!arg || sdslen(arg) == 0)
    {
        /* 不带 id: 默认停当前前台进程组里的 active 任务 (组内"正在说话"的那个)。
         * active 概念上应属于前台组, 但代码并不强制 —— 这里显式校验成员关系,
         * 免得 session 切换残留的 active 指到别的组去。 */
        struct edc_task_group *fg = edc_scheduler_get_foreground(agent->scheduler);
        struct edc_task *active = sess ? sess->active_task : NULL;
        if (active && (!fg || active->group == fg))
            t = active;
        if (!t)
        {
            edc_commands_reply(agent, actor, channel_type,
                               fg ? "no active task to stop in the foreground group" : "no foreground group");
            return;
        }
    }
    else
    {
        /* 先校验任务存在再发信号: 盲发时对不存在的 id 也会回 "stop signal sent",
         * 与实际效果不符。旧 /cancel <id> 提供的正是这层校验 (该命令已删除,
         * 校验归并到 /task stop)。查找范围 = 当前 session 的任意 group。 */
        t = edc_session_find_task(sess, arg);
        if (!t)
        {
            sds reply = sdscatprintf(sdsempty(), "task not found: %s", arg);
            edc_commands_reply(agent, actor, channel_type, reply);
            sdsfree(reply);
            return;
        }
    }
    edc_scheduler_send_signal_to_task(agent->scheduler, t->task_id, TASK_SIGTERM);
    edc_commands_reply(agent, actor, channel_type, "stop signal sent");
}

/* /task resume [id]: 对可恢复任务重新判定并驱动 (依赖/工具/turn)。接受两类:
 *   - SUSPENDED: 重启后被停住的任务, 按持久化状态重新判定该回到哪一步;
 *   - ERROR:    执行中失败的任务, 续跑失败的那一轮 (复用残留 active_turn /
 *               工具账本, 不整轮重来)。
 * 不带 id 时默认当前前台组的 active 任务, 与 /task stop 一致。 */
static void edc_commands_handle_task_resume(struct edc_agent *agent, edc_actor_t *actor,
                                            edc_channel_type channel_type, sds arg)
{
    struct edc_session *sess = edc_session_manager_get_active(agent->session_manager);
    struct edc_task *t = NULL;

    if (!arg || sdslen(arg) == 0)
    {
        struct edc_task_group *fg = edc_scheduler_get_foreground(agent->scheduler);
        struct edc_task *active = sess ? sess->active_task : NULL;
        if (active && (!fg || active->group == fg))
            t = active;
        if (!t)
        {
            edc_commands_reply(agent, actor, channel_type,
                               fg ? "no active task to resume in the foreground group" : "no foreground group");
            return;
        }
    }
    else
    {
        t = edc_session_find_task(sess, arg);
        if (!t)
        {
            sds reply = sdscatprintf(sdsempty(), "task not found: %s", arg);
            edc_commands_reply(agent, actor, channel_type, reply);
            sdsfree(reply);
            return;
        }
    }

    /* SUSPENDED = 重启后被停住的任务 (resume 重新判定并驱动);
     * ERROR = 执行中失败的任务 (resume 续跑失败的那一轮, 复用残留的
     * active_turn / 工具账本)。其余状态都不可 resume。 */
    if (t->state != TASK_SUSPENDED && t->state != TASK_ERROR)
    {
        sds reply = sdscatprintf(sdsempty(), "task %s is %s, not resumable", t->task_id,
                                 edc_task_state_name(t->state));
        edc_commands_reply(agent, actor, channel_type, reply);
        sdsfree(reply);
        return;
    }

    edc_scheduler_resume_task(agent->scheduler, t);
    sds reply = sdscatprintf(sdsempty(), "resume signal sent to task %s (%s)", t->task_id,
                             edc_task_state_name(t->state));
    edc_commands_reply(agent, actor, channel_type, reply);
    sdsfree(reply);
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
        if (edc_scheduler_get_foreground(agent->scheduler) == g)
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

    struct edc_session *session = edc_session_manager_get_active(agent->session_manager);
    struct edc_task *parent = edc_session_find_task(session, tokens[0]);
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

    /* 新语义: 子任务另起独立 group (成为 leader), 继承父 group 的前文快照,
     * 而非塞进父 group。快照作为子任务的一条合成初始 turn 存在,
     * 随子任务一起持久化/折叠, 父 group 不受影响。 */
    struct edc_task_group *child_group = edc_scheduler_create_group(agent->scheduler, child);

    if (parent->group)
    {
        /* 继承父 group 的 memory 摘要 */
        if (parent->group->memory)
            child_group->memory = sdsdup(parent->group->memory);

        /* system prompt 先占 history[0], 避免快照第一条被 cb_send_llm 误当 system */
        cJSON *sys = cJSON_GetArrayItem(parent->group->history, 0);
        if (sys)
            cJSON_AddItemToArray(child_group->history, cJSON_Duplicate(sys, 1));

        /* 合成 turn: 深拷贝父 group 未折叠消息 [1+fold_cursor .. fork_point) */
        size_t snap_begin = 1 + parent->group->fold_cursor;
        struct edc_turn *snap = NULL;

        if (parent->turns_folded)
        {
            /* 规则 2: parent 的 turns 已被 L1 折叠, 捞回 parent 自己的 turns 全文,
             * 保证 fork 点本身的上下文完整。其余已折叠前文仍只靠 memory 摘要。 */
            snap = edc_task_snapshot_turns(parent, session, child->task_id);
        }
        else
        {
            /* 规则 1: 未折叠区间 [1+fold_cursor, fork_point) 作为快照 */
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
                edc_queue_push(&snap->messages, msg); /* turn 拥有它 */
            }
        }

        if (snap && edc_queue_length(&snap->messages) > 0)
        {
            /* 快照消息引用到 child group history (不重复拥有) */
            for (size_t mj = snap->messages.head; mj < snap->messages.tail; mj++)
                cJSON_AddItemReferenceToArray(child_group->history, snap->messages.data[mj]);
            edc_task_add_turn(child, snap);
        }
        else if (snap)
        {
            edc_turn_free(snap);
        }
    }

    /* 前台切到新 group, 并立即把子任务设为 active task。这样后续普通输入
     * 会被路由 attach 到这个 WAITING_INPUT 的 leader, 而不是再新建一个 task。 */
    edc_scheduler_set_foreground(agent->scheduler, child_group);
    edc_session_set_active_task(edc_session_manager_get_active(agent->session_manager), child);

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
    DL_FOREACH(edc_scheduler_get_session(agent->scheduler)->groups, g)
    {
        out = sdscatprintf(
            out, "- %s%s  **%s**  %d members  _%s_\n", edc_scheduler_get_foreground(agent->scheduler) == g ? "`[fg]` " : "",
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
                           "- **desc**: %s\n",
                           t->task_id, edc_task_state_name(t->state), t->group_id, t->description);

    /* 展示 task 实际收到的用户输入（而不只是 description）。多行输入把换行
     * 后的内容缩进两格，作为 markdown 列表项的续行，避免撑破列表。 */
    sds user_input = edc_task_user_input(t);
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
            struct edc_task_group *fg = edc_scheduler_get_foreground(agent->scheduler);
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
    else if (strcmp(content_trim, "/task stop") == 0)
    {
        edc_commands_handle_task_stop(agent, actor, message->type, NULL);
    }
    else if (strncmp(content_trim, "/task stop ", 11) == 0)
    {
        arg = sdsnew(content_trim + 11);
        edc_commands_handle_task_stop(agent, actor, message->type, arg);
    }
    else if (strcmp(content_trim, "/task resume") == 0)
    {
        edc_commands_handle_task_resume(agent, actor, message->type, NULL);
    }
    else if (strncmp(content_trim, "/task resume ", 13) == 0)
    {
        arg = sdsnew(content_trim + 13);
        edc_commands_handle_task_resume(agent, actor, message->type, arg);
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
    /* 有效性门只检查消息本身; 不检查 message->type —— 历史上 edc_channel_cli==0
     * 曾让 CLI 消息被误判为"无类型"而整体丢弃 (类型是路由键, 不是有效性标志)。 */
    if (!message || !message->content)
        return false;

    /* / commands: shell builtins first, then shortcuts, then existing handlers */
    if (message->content[0] == '/')
    {
        if (edc_shell_handle_builtin(agent, actor, message))
            return true;

        /* 顶层快捷别名 (/tasks, /bg, /fg) 已移除 —— 统一走 /task 子命令。 */

        if (edc_commands_is_command(message->content, "/task"))
            return edc_commands_handle_task(agent, actor, message);

        if (edc_commands_is_command(message->content, "/session"))
            return edc_commands_handle_session(agent, actor, message);

        if (edc_commands_is_target(message->content, "/help"))
            return edc_commands_handle_help(agent, actor, message->type);

        sds reply =
            sdscatprintf(sdsempty(), "Unknown command: %s\nType /help for available commands.", message->content);
        edc_commands_reply(agent, actor, message->type, reply);
        sdsfree(reply);
        return true;
    }

    /* non-/ input: delegate to shell module */
    return edc_shell_execute(agent, actor, message);
}
