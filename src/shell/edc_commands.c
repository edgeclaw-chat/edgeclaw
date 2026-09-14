#include "edc_commands.h"
#include "edc_actor.h"
#include "edc_agent.h"
#include "edc_channel_actor.h"
#include "edc_cmd_parse.h"
#include "edc_compat.h"
#include "edc_scheduler.h"
#include "edc_session.h"
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
    struct edc_channel_message *reply =
        edc_channel_message_new(channel_type, EDC_CHANNEL_MESSAGE_SYS_TYPE, sdsnew(text));
    edc_actor_send(agent->channel_actor,
                   edc_actor_msg_new_sub_type(BC_ACTOR_MSG_CHANNEL_SEND, reply->type, actor, NULL, reply,
                                              (edc_actor_data_free_fn)edc_channel_message_free));
}

/* ── /session stop ─────────────────────────────────────────── */

/* 取消当前 session 的全部非终结 turns 并立即落盘。原顶层 /stop 命令, 现收归
 * /session 命名空间 (无参数)。移除某个 session 请用 /session remove <name>。
 * 不再 reload session: LLM 上下文来自 group->history, session->messages 只是
 * 持久化用的 legacy 副本, stop 不需要重建它 (重建反而会释放 group history
 * 引用的对象)。edc_agent_stop_all_turns 内部会落盘。 */
static bool edc_commands_handle_session_stop(struct edc_agent *agent, edc_actor_t *actor, edc_channel_type channel_type)
{
    edc_agent_stop_all_turns(agent);

    edc_commands_reply(agent, actor, channel_type, "**Session** — stop signal sent to all running turns.");
    return true;
}

/* ── /help ─────────────────────────────────────────────────── */

static bool edc_commands_handle_help(struct edc_agent *agent, edc_actor_t *actor, edc_channel_type channel_type)
{
    const char *text = "## EdgeClaw Commands\n"
                       "\n"
                       "- /help\n"
                       "- /session new [name] [dir]  ·  /session switch <name>\n"
                       "- /session list  ·  /session info  ·  /session status [id]\n"
                       "- /session save  ·  /session stop\n"
                       "- /session remove <name>  ·  /session rename <old> <new>\n"
                       "\n"
                       "- /task new <desc>   [--bg] [--suspended]\n"
                       "- /task fork <parent> <desc> [--suspended]\n"
                       "- /task list  ·  /task status [id]  ·  /task output <id>\n"
                       "- /task groups  ·  /task fg <id>  ·  /task bg [id]\n"
                       "- /task stop [id]  ·  /task resume [id]   (default: active task of the foreground group)\n"
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
        edc_commands_reply(agent, actor, channel_type, "**Session** — no active session.");
        return true;
    }

    edc_session_save_state(active, agent->scheduler);
    edc_commands_reply(agent, actor, channel_type, "**Session saved** — history and state persisted.");
    log_info("receive /session save command");
    return true;
}

/* ── /session commands ─────────────────────────────────────── */

/* Split "/session new" arguments into an optional name and an optional working
 * directory: "<name> [dir]". Leading/trailing whitespace is trimmed; the
 * directory may itself contain spaces (everything after the first token).
 * Either output may be NULL. Callers sdsfree both. */
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

/* 切活跃 session 的公共尾巴 (P3 ①b): 告诉前端"现在显示哪个 session"。
 *
 *  ①b 之后快照是**拉取**的 —— 前端不再从推送里得知自己该看哪棵树, 所以这件事
 *  得显式说一声 (这是"active_index 变了"这个事实的通报, 权威仍在 session_manager)。
 *  两道讲究:
 *    - **只在真的换了时才发**: 没换就什么都不做, 免得前端白拉一棵没变的树;
 *    - **位置在 reply 之后**: 今天那条 "**Switched**" / "**Session created**" 文本
 *      落在**旧** session 的转录里 (前端先收到回复、再被推送切台) —— 顺序反过来
 *      会把它写进新 session 的转录, 那是用户可见的差异。 */
static void cmd_notify_if_active_session_changed(struct edc_agent *agent, struct edc_session *before)
{
    struct edc_session *now = edc_session_manager_get_active(agent->session_manager);
    if (now == before)
        return;
    edc_agent_notify_active_session(agent, now ? now->session_id : NULL);
}

static bool edc_commands_handle_session_new(struct edc_agent *agent, edc_actor_t *actor, edc_channel_type channel_type,
                                            sds arg)
{
    /* edc_session_manager_create 会把 active 直接切到新 session, 所以 "旧
     * session" 必须在 create 之前取到, 否则拿到的是它自己。 */
    struct edc_session *old = edc_session_manager_get_active(agent->session_manager);
    sds name = NULL, dir = NULL;
    edc_commands_split_name_dir(arg, &name, &dir);

    /* A given working directory is resolved up front and must name an existing
     * directory. Relative paths are anchored at the process launch directory
     * (the process cwd), not at the active session's workdir, so the anchor is
     * stable regardless of which session is current. If it does not resolve,
     * fail the whole command: never silently create the session bound to the
     * default directory instead. */
    if (dir && sdslen(dir) > 0)
    {
        EdcError *werr = NULL;
        sds resolved = edc_session_resolve_workdir(NULL, dir, &werr);
        if (!resolved)
        {
            sds reply =
                sdscatprintf(sdsempty(), "**Session** — failed: %s", werr ? werr->message : "invalid directory");
            edc_commands_reply(agent, actor, channel_type, reply);
            sdsfree(reply);
            edc_error_free(werr);
            sdsfree(name);
            sdsfree(dir);
            return true;
        }
        sdsfree(dir);
        dir = resolved;
    }

    struct edc_session *sess = edc_session_manager_create_ex(agent->session_manager, name, dir);
    sdsfree(name);
    if (!sess)
    {
        sds reply = sdscatprintf(sdsempty(), "**Session** — failed to create session%s%s", dir ? " bound to " : "",
                                 dir ? dir : "");
        edc_commands_reply(agent, actor, channel_type, reply);
        sdsfree(reply);
        sdsfree(dir);
        return true;
    }
    const char *bound = edc_session_get_workdir(sess);
    sdsfree(dir);
    /* 切到新 session: 旧 session **继续在后台执行**, 不取消任何任务 (session
     * 像终端 tab —— 切走不停进程)。
     * P3 ③: 这里不再有"把 binding 换成新 session"这一步 —— 切 session 只是
     * `active_index` 变了 (session_manager 的唯一权威), scheduler 里根本没有
     * 那份副本了。落盘也不再依赖"切之前先存": 任务完成时按属主存、退出时存
     * 所有已加载的 (save_loaded_states)。 */
    (void)old;
    /* 全新 session: 内存状态即权威, 标记已加载, 避免日后切回时被（此时可能
     * 已存在的）session.json 反序列化覆盖。 */
    sess->state_loaded = true;

    sds reply = sdscatprintf(sdsempty(), "**Session created** — `[%s]` %s  ·  workdir: `%s`", sess->session_id,
                             edc_session_get_name(sess), bound ? bound : "default");
    edc_commands_reply(agent, actor, channel_type, reply);
    sdsfree(reply);
    cmd_notify_if_active_session_changed(agent, old);
    return true;
}

static bool edc_commands_handle_session_switch(struct edc_agent *agent, edc_actor_t *actor,
                                               edc_channel_type channel_type, sds arg)
{
    if (!arg || sdslen(arg) == 0)
    {
        edc_commands_reply(agent, actor, channel_type, "Usage: `/session switch <name_or_id>`");
        return true;
    }
    struct edc_session *new_sess = edc_session_manager_find(agent->session_manager, arg);
    if (!new_sess)
    {
        sds reply = sdscatprintf(sdsempty(), "Session not found: `%s`", arg);
        edc_commands_reply(agent, actor, channel_type, reply);
        sdsfree(reply);
        return true;
    }
    struct edc_session *old = edc_session_manager_get_active(agent->session_manager);
    if (old == new_sess)
    {
        sds reply =
            sdscatprintf(sdsempty(), "Already on `[%s]` %s", new_sess->session_id, edc_session_get_name(new_sess));
        edc_commands_reply(agent, actor, channel_type, reply);
        sdsfree(reply);
        return true;
    }
    /* 切走时旧 session **继续在后台执行** (session 像终端 tab, 切走切 tab 不停
     * 进程), 因此不取消任何任务 —— 前台组随 session 保留 (session->foreground_group),
     * 切回来时 set_session 会恢复。这里显式落一次盘: 旧 session 之后在后台继续
     * 推进, 先保证已产生的内容持久 (前台组已随 session 落盘, 不再丢失)。 */
    if (old)
    {
        edc_session_save_state(old, agent->scheduler);
    }

    if (!edc_session_manager_switch(agent->session_manager, arg))
    {
        sds reply = sdscatprintf(sdsempty(), "Session not found: `%s`", arg);
        edc_commands_reply(agent, actor, channel_type, reply);
        sdsfree(reply);
        return true;
    }
    /* P3 ③: active_index 已经由 edc_session_manager_switch 改好, 没有别的状态
     * 要改 —— 切 session 只是 UI 换视图 (前端收 reason=active 事件后自己换)。 */
    /* 只在本 run 首次激活该 session 时反序列化磁盘状态; 已加载过的 session
     * (可能正在后台执行) 直接沿用内存对象, 避免被磁盘副本覆盖 —— 不再 reload
     * (重建会释放 group->history 引用的对象)。 */
    if (!new_sess->state_loaded)
        edc_agent_load_session(agent, new_sess);
    sds reply =
        sdscatprintf(sdsempty(), "**Switched** — `[%s]` %s", new_sess->session_id, edc_session_get_name(new_sess));
    edc_commands_reply(agent, actor, channel_type, reply);
    sdsfree(reply);
    cmd_notify_if_active_session_changed(agent, old);
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
        edc_commands_reply(agent, actor, channel_type, "Usage: `/session remove <name_or_id>`");
        return true;
    }
    struct edc_session *target = edc_session_manager_find(agent->session_manager, arg);
    if (!target)
    {
        sds reply = sdscatprintf(sdsempty(), "Session not found: `%s`", arg);
        edc_commands_reply(agent, actor, channel_type, reply);
        sdsfree(reply);
        return true;
    }
    /* 最后一个 session 不能 remove。先于取消任务拦住, 否则一次失败的 remove 会把
     * 当前 session 的任务白白打成 CANCELLED。 */
    if (agent->session_manager->session_count <= 1)
    {
        edc_commands_reply(agent, actor, channel_type, "**Session** — cannot remove the last session.");
        return true;
    }

    /* remove 也会调整 active_index (删掉的是当前之前的那个时会顺移), 所以先把
     * "移除前显示的是谁"记下来, 末尾按事实决定要不要通知前端换台。 */
    struct edc_session *active_before = edc_session_manager_get_active(agent->session_manager);
    sds stopped_id = sdsnew(target->session_id);
    sds stopped_name = sdsnew(edc_session_get_name(target));

    /* /session remove = 用户明确表达移除意愿 (相当于关掉这个 tab): 全取消该
     * session 的所有非终态任务 (含 WAITING_INPUT), 而非只掐在途执行。随后
     * edc_session_manager_stop 内部的 save 会把内存里的 CANCELLED 落盘。目标
     * 可能不是当前 active session, 故用 session 版信号显式指定目标。 */
    edc_scheduler_send_signal_to_session(agent->scheduler, target, TASK_SIGTERM, false);

    if (!edc_session_manager_stop(agent->session_manager, arg))
    {
        sdsfree(stopped_id);
        sdsfree(stopped_name);
        edc_commands_reply(agent, actor, channel_type, "**Session** — cannot remove the last session.");
        return true;
    }
    struct edc_session *new_active = edc_session_manager_get_active(agent->session_manager);
    if (new_active && !new_active->state_loaded)
        edc_agent_load_session(agent, new_active);
    sds reply = sdscatprintf(sdsempty(), "**Session removed** — `[%s]` %s", stopped_id, stopped_name);
    edc_commands_reply(agent, actor, channel_type, reply);
    sdsfree(reply);
    cmd_notify_if_active_session_changed(agent, active_before);
    sdsfree(stopped_id);
    sdsfree(stopped_name);
    return true;
}

static bool edc_commands_handle_session_rename(struct edc_agent *agent, edc_actor_t *actor,
                                               edc_channel_type channel_type, sds arg)
{
    if (!arg || sdslen(arg) == 0)
    {
        edc_commands_reply(agent, actor, channel_type, "Usage: `/session rename <old_name> <new_name>`");
        return true;
    }
    int count = 0;
    sds *tokens = sdssplitlen(arg, sdslen(arg), " ", 1, &count);
    if (count < 2)
    {
        sdsfreesplitres(tokens, count);
        edc_commands_reply(agent, actor, channel_type, "Usage: `/session rename <old_name> <new_name>`");
        return true;
    }
    bool ok = edc_session_manager_rename(agent->session_manager, tokens[0], tokens[1]);
    if (!ok)
    {
        sds reply = sdscatprintf(sdsempty(), "Session not found: `%s`", tokens[0]);
        edc_commands_reply(agent, actor, channel_type, reply);
        sdsfree(reply);
        sdsfreesplitres(tokens, count);
        return true;
    }
    sds reply = sdscatprintf(sdsempty(), "**Session renamed** — `%s` → `%s`", tokens[0], tokens[1]);
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
        edc_commands_reply(agent, actor, channel_type, "**Session** — no active session.");
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
    edc_commands_reply(agent, actor, channel_type, info);
    sdsfree(info);
    return true;
}

/* /session status [name_or_id]: live runtime status of a session — group/task
 * counts, the per-state task breakdown, and (for the current session) the
 * foreground group and active task. Without an argument it reports the current
 * session. Static metadata (id/name/created/…) stays in /session info; the
 * per-task detail stays in /task list. */
static bool edc_commands_handle_session_status(struct edc_agent *agent, edc_actor_t *actor,
                                               edc_channel_type channel_type, sds arg)
{
    struct edc_session *sess = NULL;
    if (arg && sdslen(arg) > 0)
    {
        sess = edc_session_manager_find(agent->session_manager, arg);
        if (!sess)
        {
            sds reply = sdscatprintf(sdsempty(), "Session not found: `%s`", arg);
            edc_commands_reply(agent, actor, channel_type, reply);
            sdsfree(reply);
            return true;
        }
    }
    else
    {
        sess = edc_session_manager_get_active(agent->session_manager);
        if (!sess)
        {
            edc_commands_reply(agent, actor, channel_type, "**Session** — no current session.");
            return true;
        }
    }

    /* "是不是当前这台" = 是不是 active session (P3 ③: 唯一权威在 session_manager)。 */
    bool is_current = (sess == edc_session_manager_get_active(agent->session_manager));
    struct edc_task_group *fg = is_current ? edc_scheduler_get_foreground(sess) : NULL;

    sds out = edc_session_status_text(sess, is_current, fg ? fg->group_id : NULL);
    edc_commands_reply(agent, actor, channel_type, out);
    sdsfree(out);
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
    if (edc_commands_is_target(msg->content, "/session status"))
        return edc_commands_handle_session_status(agent, actor, msg->type, NULL);
    if (edc_commands_arg(msg->content, "/session status", &rest))
    {
        bool ret = edc_commands_handle_session_status(agent, actor, msg->type, rest);
        sdsfree(rest);
        return ret;
    }
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
        edc_commands_reply(
            agent, actor, msg->type,
            "`/session stop` takes no arguments. To remove a session use `/session remove <name_or_id>`.");
        return true;
    }
    sds reply =
        sdscatprintf(sdsempty(), "**Unknown `/session` subcommand.** Try: `new`, `switch`, `list`, `info`, `status`, "
                                 "`save`, `stop`, `remove`, `rename`.");
    edc_commands_reply(agent, actor, msg->type, reply);
    sdsfree(reply);
    return true;
}

/* ── /task commands ─────────────────────────────────────────― */

/* 命令层派发任务时用的 request id。**这一文件里所有派发点共用同一个计数器** ——
 * 各自 `static int64_t counter = 0` 的话都从 1 开始, 两个命令会发出**相同**的 id,
 * 而 scheduler 的 task_hash 正是以 request id 为键。
 * (注: edc_shell.c 的 AST 派发点另有自己的计数器, 尚未合并 —— 既有偏离, 不在本批。) */
static int64_t g_cmd_req_seq = 0;

/* ── 这条输入属于哪个 session (P3) ──────────────────────────
 *
 * 命令建出来的任务/组必须落在**输入自己声明**的 session 上, 而不是"当前绑定"
 * 的那份副本 (scheduler 里那份正在被拆掉)。标签为空 = 调用方没指定 (crontab,
 * 老渠道) → agent 给活跃 session, 与老实现一致; 标签指向不存在的 session 时
 * 返回 NULL, 调用方**拒绝**这条命令 (不猜)。 */
static struct edc_session *cmd_input_session(struct edc_agent *agent, struct edc_channel_message *msg)
{
    return edc_agent_resolve_session(agent, msg ? msg->session_id : NULL);
}

static void edc_commands_handle_task_new(struct edc_agent *agent, edc_actor_t *actor, edc_channel_type channel_type,
                                         struct edc_session *session, sds arg)
{
    if (!session)
        return;
    struct edc_cmd_task_opts opts;
    edc_cmd_parse_task_opts(arg, &opts);

    /* Resolve `-d` dependencies up front. Task ids are globally unique, so a
     * dependency may live in another session. An unknown id fails the command
     * (nothing is created) rather than being silently dropped. */
    struct edc_task **deps = NULL;
    if (opts.dep_count > 0)
    {
        deps = calloc((size_t)opts.dep_count, sizeof(*deps));
        for (int i = 0; i < opts.dep_count; i++)
        {
            deps[i] = edc_agent_resolve_task(agent, opts.dep_ids[i], NULL);
            if (!deps[i])
            {
                sds reply = sdscatprintf(sdsempty(), "Task not found: `%s`", opts.dep_ids[i]);
                edc_commands_reply(agent, actor, channel_type, reply);
                sdsfree(reply);
                free(deps);
                edc_cmd_task_opts_free(&opts);
                return;
            }
        }
    }

    struct edc_task *task = edc_scheduler_create_task(
        agent->scheduler, session, (opts.desc && sdslen(opts.desc) > 0) ? opts.desc : "New task", channel_type);
    if (!task)
        return;
    if (opts.prompt_name)
        edc_task_set_prompt(task, opts.prompt_name, NULL);

    /* Wire the (already resolved) dependencies onto the new task. Done before
     * the group branching below so it is not skipped by the `goto done` path. */
    for (int i = 0; i < opts.dep_count; i++)
        edc_task_add_dependency(task, deps[i], TASK_DEP_JOIN);
    free(deps);
    deps = NULL;

    if (opts.group_id)
    {
        struct edc_task_group *group = edc_session_find_group(session, opts.group_id);
        if (!group)
        {
            sds reply = sdscatprintf(sdsempty(), "Group `%s` not found — creating a new group.", opts.group_id);
            edc_commands_reply(agent, actor, channel_type, reply);
            sdsfree(reply);
            group = edc_scheduler_create_group(agent->scheduler, session, task);
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
        struct edc_task_group *group = edc_scheduler_create_group(agent->scheduler, session, task);

        /* 挂起的任务不算 active task, 也不抢前台 —— 与 --bg 同样只建组。 */
        if (opts.bg || opts.suspended)
        {
            // sds reply =
            //     sdscatprintf(sdsempty(), "[task] created %s (group %s) in background", task->task_id,
            //     group->group_id);
            // edc_commands_reply(agent, actor, channel_type, reply);
            // sdsfree(reply);
        }
        else
        {
            edc_scheduler_set_foreground(session, group);
            edc_session_set_active_task(session, task);
            // sds reply = sdscatprintf(sdsempty(), "[task] created %s (group %s)", task->task_id, group->group_id);
            // edc_commands_reply(agent, actor, channel_type, reply);
            // sdsfree(reply);
        }
    }

done:
    /* desc 就是 input, 任务**创建即运行** (2026-09-14 决定)。此前这里只建不派发,
     * 任务停在 WAITING_INPUT 且**永远等不到输入** —— 没有任何代码会给它,
     * 那是个死胡同。派发与 shell 的创建路径 (edc_shell.c) 走同一条:
     * 建 turn -> 注册 -> 置 LLM_PROCESSING -> send_llm。
     *
     * 放在 `done:` **之后**: 指定已存在 group 的那条路是 `goto done` 过来的,
     * 放前面会被跳过, 于是 `--group` 创建的任务又不跑了。 */
    if (task && opts.desc && sdslen(opts.desc) > 0)
    {
        /* desc 就是 input: 先把 turn 装上 —— 普通任务与 --suspended 任务在**这一点
         * 上完全一样** (初始相位相同), 区别只在下面那一步"立即执行"。 */
        struct edc_turn *turn = edc_turn_new();
        turn->task_id = strdup(task->task_id);
        edc_turn_add_user_message(turn, opts.desc);
        task->active_turn = turn;

        if (opts.suspended)
        {
            /* --suspended: 到此为止。不注册请求、不进 LLM_PROCESSING —— 停在
             * SUSPENDED, 等 /task resume。有依赖的照样等依赖 (resume 之后走的
             * 就是普通路径)。 */
            bool ok = edc_task_mark_suspended(task);
            log_info("/task new: task=%s created SUSPENDED -> %s (awaiting /task resume)", task->task_id,
                     ok ? "ok" : "FAILED");
        }
        else if (edc_task_awaits_deps(task))
        {
            log_info("/task new spawn: task %s blocked in WAITING_DEPS, turn queued", task->task_id);
            edc_cmd_task_opts_free(&opts);
            return;
        }
        else
        {
        void *req_id = (void *)(intptr_t)(++g_cmd_req_seq);
        edc_scheduler_register_task(agent->scheduler, task, req_id);
        task->current_request_id = req_id;

        bool ok = edc_task_await_exec(task);
        log_info("/task new spawn: task=%s set_state LLM_PROCESSING -> %s, send_llm=%s", task->task_id,
                 ok ? "ok" : "FAILED",
                 (agent->scheduler && agent->scheduler->send_llm) ? "ready" : "NULL");
        if (ok && agent->scheduler && agent->scheduler->send_llm)
            agent->scheduler->send_llm(agent->scheduler, task, req_id);
        }
    }

    edc_cmd_task_opts_free(&opts);
}

static void edc_commands_handle_task_list(struct edc_agent *agent, edc_actor_t *actor, edc_channel_type channel_type)
{
    sds out = sdsnew("## Tasks\n\n");
    struct edc_session *sess = edc_session_manager_get_active(agent->session_manager);
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
            bool is_active = edc_session_manager_get_active(agent->session_manager) &&
                             edc_session_manager_get_active(agent->session_manager)->active_task == t;
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
        struct edc_task_group *fg = edc_scheduler_get_foreground(sess);
        struct edc_task *active = sess ? sess->active_task : NULL;
        if (active && (!fg || active->group == fg))
            t = active;
        if (!t)
        {
            edc_commands_reply(agent, actor, channel_type,
                               fg ? "**Task** — no active task to stop in the foreground group."
                                  : "**Task** — no foreground group.");
            return;
        }
    }
    else
    {
        /* 先校验任务存在再发信号: 盲发时对不存在的 id 也会回 "stop signal sent",
         * 与实际效果不符。查找范围 = 所有已加载 session (task id 全局唯一) ——
         * 允许停掉别的 session 里的任务。 */
        t = edc_agent_resolve_task(agent, arg, NULL);
        if (!t)
        {
            sds reply = sdscatprintf(sdsempty(), "Task not found: `%s`", arg);
            edc_commands_reply(agent, actor, channel_type, reply);
            sdsfree(reply);
            return;
        }
    }
    edc_scheduler_send_signal_to_task_ptr(agent->scheduler, t, TASK_SIGTERM);
    sds reply = sdscatprintf(sdsempty(), "**Task `%s`** — stop signal sent.", t->task_id);
    if (t->group && t->group->session && t->group->session != sess)
        reply = sdscatprintf(reply, " (session `[%s]`)", t->group->session->session_id);
    edc_commands_reply(agent, actor, channel_type, reply);
    sdsfree(reply);
}

/* /task resume [id]: 对可恢复任务重新判定并驱动 (依赖/工具/turn)。接受两类:
 *   - SUSPENDED: 重启后被停住的任务, 按持久化状态重新判定该回到哪一步;
 *   - ERROR:    执行中失败的任务, 续跑失败的那一轮 (复用残留 active_turn /
 *               工具账本, 不整轮重来)。
 * 不带 id 时默认当前前台组的 active 任务, 与 /task stop 一致。 */
static void edc_commands_handle_task_resume(struct edc_agent *agent, edc_actor_t *actor, edc_channel_type channel_type,
                                            sds arg)
{
    struct edc_session *sess = edc_session_manager_get_active(agent->session_manager);
    struct edc_task *t = NULL;

    if (!arg || sdslen(arg) == 0)
    {
        struct edc_task_group *fg = edc_scheduler_get_foreground(sess);
        struct edc_task *active = sess ? sess->active_task : NULL;
        if (active && (!fg || active->group == fg))
            t = active;
        if (!t)
        {
            edc_commands_reply(agent, actor, channel_type,
                               fg ? "**Task** — no active task to resume in the foreground group."
                                  : "**Task** — no foreground group.");
            return;
        }
    }
    else
    {
        t = edc_agent_resolve_task(agent, arg, NULL);
        if (!t)
        {
            sds reply = sdscatprintf(sdsempty(), "Task not found: `%s`", arg);
            edc_commands_reply(agent, actor, channel_type, reply);
            sdsfree(reply);
            return;
        }
    }

    /* SUSPENDED = 重启后被停住的任务 (resume 重新判定并驱动);
     * ERROR = 执行中失败的任务 (resume 续跑失败的那一轮, 复用残留的
     * active_turn / 工具账本)。其余状态都不可 resume。 */
    if (!edc_task_is_resumable(t))
    {
        sds reply =
            sdscatprintf(sdsempty(), "**Task `%s`** — %s, not resumable.", t->task_id, edc_task_state_str(t));
        edc_commands_reply(agent, actor, channel_type, reply);
        sdsfree(reply);
        return;
    }

    /* Resume gate: try to restore any cross-session dependency right now (this
     * may lazy-load its owner session). If it still cannot be resolved, hold the
     * task — resuming would run it with an incomplete dependency view. */
    edc_agent_reconcile_pending_deps(agent);
    const char *missing = edc_scheduler_first_unresolved_dep(agent->scheduler, t);
    if (missing)
    {
        sds reply = sdscatprintf(sdsempty(),
                                 "**Task `%s` is held** — it depends on `%s`, which could not be restored "
                                 "(no loaded session provides it). It will not run until that dependency is available.",
                                 t->task_id, missing);
        edc_commands_reply(agent, actor, channel_type, reply);
        sdsfree(reply);
        return;
    }

    edc_scheduler_resume_task(agent->scheduler, t);
    sds reply =
        sdscatprintf(sdsempty(), "**Task `%s`** — resume signal sent (%s).", t->task_id, edc_task_state_str(t));
    if (t->group && t->group->session && t->group->session != sess)
        reply = sdscatprintf(reply, " (session `[%s]`)", t->group->session->session_id);
    edc_commands_reply(agent, actor, channel_type, reply);
    sdsfree(reply);
}

/* A task in a non-active session can be *referenced* globally (dependency,
 * status, output, stop/resume), but commands that change the current session's
 * foreground view (fg/bg/fork) must not silently operate on it. Returns true
 * when |t| belongs to the active session; otherwise replies with an error and
 * returns false. */
static bool edc_commands_task_in_active_session(struct edc_agent *agent, edc_actor_t *actor,
                                                edc_channel_type channel_type, struct edc_task *t)
{
    struct edc_session *active = edc_session_manager_get_active(agent->session_manager);
    struct edc_session *owner = (t->group && t->group->session) ? t->group->session : active;
    if (owner == active)
        return true;
    sds reply = sdscatprintf(sdsempty(),
                             "Task `%s` belongs to session `[%s]` — switch to it first (`/session switch %s`).",
                             t->task_id, owner->session_id, owner->session_id);
    edc_commands_reply(agent, actor, channel_type, reply);
    sdsfree(reply);
    return false;
}

static void edc_commands_handle_task_fg(struct edc_agent *agent, edc_actor_t *actor, edc_channel_type channel_type,
                                        sds arg)
{
    if (!arg)
    {
        edc_commands_reply(agent, actor, channel_type, "Usage: `/task fg <id>`");
        return;
    }
    struct edc_task *t = edc_session_manager_find_task_any(agent->session_manager, arg, NULL);
    if (!t)
    {
        edc_commands_reply(agent, actor, channel_type, "Task not found.");
        return;
    }
    if (!edc_commands_task_in_active_session(agent, actor, channel_type, t))
        return;
    struct edc_task_group *g =
        edc_session_find_group(edc_session_manager_get_active(agent->session_manager), t->group_id);
    if (!g)
    {
        edc_commands_reply(agent, actor, channel_type, "Group not found.");
        return;
    }
    edc_scheduler_set_foreground(edc_session_manager_get_active(agent->session_manager), g);
    edc_session_set_active_task(edc_session_manager_get_active(agent->session_manager), t);
    sds reply = sdscatprintf(sdsempty(), "**Foreground** — group `%s` · active task `%s`.", g->group_id, t->task_id);
    edc_commands_reply(agent, actor, channel_type, reply);
    sdsfree(reply);
}

static void edc_commands_handle_task_bg(struct edc_agent *agent, edc_actor_t *actor, edc_channel_type channel_type,
                                        sds arg)
{
    if (arg)
    {
        struct edc_task *t = edc_session_manager_find_task_any(agent->session_manager, arg, NULL);
        if (!t)
        {
            edc_commands_reply(agent, actor, channel_type, "Task not found.");
            return;
        }
        if (!edc_commands_task_in_active_session(agent, actor, channel_type, t))
            return;
        struct edc_task_group *g =
            edc_session_find_group(edc_session_manager_get_active(agent->session_manager), t->group_id);
        if (!g)
        {
            edc_commands_reply(agent, actor, channel_type, "Group not found.");
            return;
        }
        if (edc_scheduler_get_foreground(edc_session_manager_get_active(agent->session_manager)) == g)
        {
            edc_scheduler_set_foreground(edc_session_manager_get_active(agent->session_manager), NULL);
            edc_session_set_active_task(edc_session_manager_get_active(agent->session_manager), NULL);
            sds reply = sdscatprintf(sdsempty(), "**Background** — group `%s`.", g->group_id);
            edc_commands_reply(agent, actor, channel_type, reply);
            sdsfree(reply);
        }
        else
        {
            sds reply = sdscatprintf(sdsempty(), "Group `%s` is already in the background.", g->group_id);
            edc_commands_reply(agent, actor, channel_type, reply);
            sdsfree(reply);
        }
    }
    else
    {
        edc_scheduler_set_foreground(edc_session_manager_get_active(agent->session_manager), NULL);
        edc_session_set_active_task(edc_session_manager_get_active(agent->session_manager), NULL);
        edc_commands_reply(agent, actor, channel_type, "No foreground group.");
    }
}

static void edc_commands_handle_task_fork(struct edc_agent *agent, edc_actor_t *actor, edc_channel_type channel_type,
                                          struct edc_session *session, sds arg)
{
    if (!session)
        return;
    /* /task fork <parent_id> <desc> [--prompt <name>] [--suspended] */
    if (!arg)
    {
        edc_commands_reply(agent, actor, channel_type,
                           "Usage: `/task fork <parent_id> <desc> [--prompt <name>] [--suspended]`");
        return;
    }

    int count = 0;
    sds *tokens = sdssplitlen(arg, sdslen(arg), " ", 1, &count);
    if (count < 2)
    {
        edc_commands_reply(agent, actor, channel_type,
                           "Usage: `/task fork <parent_id> <desc> [--prompt <name>] [--suspended]`");
        sdsfreesplitres(tokens, count);
        return;
    }

    /* `session` 是形参: **这条输入所属的** session (P3 起由消息的标签解析, 不再是
     * "问活跃 session 再假设它就是这个输入的家")。 */
    struct edc_task *parent = edc_session_manager_find_task_any(agent->session_manager, tokens[0], NULL);
    if (!parent)
    {
        sds reply = sdscatprintf(sdsempty(), "Parent task not found: `%s`", tokens[0]);
        edc_commands_reply(agent, actor, channel_type, reply);
        sdsfree(reply);
        sdsfreesplitres(tokens, count);
        return;
    }
    if (!edc_commands_task_in_active_session(agent, actor, channel_type, parent))
    {
        sdsfreesplitres(tokens, count);
        return;
    }

    /* find --prompt / --suspended in remaining args; the rest is the input */
    sds prompt_name = NULL;
    bool suspended = false;
    sds desc = sdsempty();
    for (int i = 1; i < count; i++)
    {
        if (strcmp(tokens[i], "--prompt") == 0 && i + 1 < count)
        {
            prompt_name = sdsnew(tokens[i + 1]);
            i++; /* skip prompt value */
        }
        else if (strcmp(tokens[i], "--suspended") == 0)
        {
            suspended = true;
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
    /* fork 点用"提交序水位"表达: 父组中已提交成员的最大 completion_seq。 */
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
    char *fork_tid = edc_scheduler_gen_task_id(agent->scheduler);
    struct edc_task *child = edc_task_fork(parent, fork_tid, desc, fork_point_seq, channel_type);
    free(fork_tid);
    if (prompt_name)
        edc_task_set_prompt(child, prompt_name, NULL);

    /* 新语义: 子任务另起独立 group (成为 leader), 继承父 group 的前文快照,
     * 而非塞进父 group。快照作为子任务的一条合成初始 turn 存在,
     * 随子任务一起持久化/折叠, 父 group 不受影响。 */
    struct edc_task_group *child_group = edc_scheduler_create_group(agent->scheduler, session, child);

    if (parent->group)
    {
        /* 继承父 group 的 memory 摘要 */
        if (parent->group->memory)
            child_group->memory = sdsdup(parent->group->memory);

        /* system prompt 先占 history[0], 避免快照第一条被 cb_send_llm 误当 system */
        cJSON *sys = cJSON_GetArrayItem(parent->group->history, 0);
        if (sys)
            cJSON_AddItemToArray(child_group->history, cJSON_Duplicate(sys, 1));

        /* 合成 turn: 深拷贝父 group 未折叠消息 [1+已折叠数 .. fork_point) */
        size_t snap_begin = 1 + edc_task_group_folded_msg_count(parent->group);
        struct edc_turn *snap = NULL;

        if (parent->turns_folded)
        {
            /* 规则 2: parent 的 turns 已被 L1 折叠, 捞回 parent 自己的 turns 全文,
             * 保证 fork 点本身的上下文完整。其余已折叠前文仍只靠 memory 摘要。 */
            snap = edc_task_snapshot_turns(parent, session, child->task_id);
        }
        else
        {
            /* 规则 1: 未折叠区间 [1+已折叠数, fork_point) 作为快照 */
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

    /* 前台切到新 group, 并把子任务设为 active task。 */
    edc_scheduler_set_foreground(session, child_group);
    edc_session_set_active_task(session, child);

    /* desc 就是 input —— 与 /task new 同一条规则: 任务创建时就带着它的 turn。
     *
     * 旧实现把子任务停在 WAITING_INPUT, 等用户下一条普通输入被 attach 进来; 那是
     * "任务是个长期会话、停在原地等输入" 的旧模型, 已随 attach 路径一起退休。
     * 要"先建好、稍后再跑"就用 --suspended: turn 照样挂上, 但不派发, 由
     * /task resume 驱动 —— 那才是新模型里休眠态的表达方式。
     * 见 docs/retire-waiting-input.md §4 第 6 行。 */
    {
        struct edc_turn *turn = edc_turn_new();
        turn->task_id = strdup(child->task_id);
        edc_turn_add_user_message(turn, desc);
        child->active_turn = turn;
    }

    if (suspended)
    {
        bool ok = edc_task_mark_suspended(child);
        log_info("/task fork: task=%s created SUSPENDED -> %s (awaiting /task resume)", child->task_id,
                 ok ? "ok" : "FAILED");
    }
    else
    {
        void *req_id = (void *)(intptr_t)(++g_cmd_req_seq);
        edc_scheduler_register_task(agent->scheduler, child, req_id);
        child->current_request_id = req_id;
        bool ok = edc_task_await_exec(child);
        log_info("/task fork spawn: task=%s set_state LLM_PROCESSING -> %s, send_llm=%s", child->task_id,
                 ok ? "ok" : "FAILED", (agent->scheduler && agent->scheduler->send_llm) ? "ready" : "NULL");
        if (ok && agent->scheduler && agent->scheduler->send_llm)
            agent->scheduler->send_llm(agent->scheduler, child, req_id);
    }

    sds reply = sdscatprintf(sdsempty(), "**Fork created** — `%s` from `%s` · group `%s` · fork point %zu",
                             child->task_id, parent->task_id, child->group_id, fork_point);
    if (prompt_name)
        reply = sdscatprintf(reply, " · prompt `%s`", prompt_name);
    if (suspended)
        reply = sdscatprintf(reply, " · parked (send `/task resume %s` to run)", child->task_id);
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
    struct edc_session *sess = edc_session_manager_get_active(agent->session_manager);
    DL_FOREACH(sess ? sess->groups : NULL, g)
    {
        out = sdscatprintf(out, "- %s%s  **%s**  %d members  _%s_\n",
                           edc_scheduler_get_foreground(sess) == g ? "`[fg]` " : "",
                           g->is_orphaned ? "`[orphan]` " : "", g->group_id, g->member_count,
                           edc_task_group_status_label(g));
    }
    edc_commands_reply(agent, actor, channel_type, out);
    sdsfree(out);
}

static void edc_commands_handle_task_status(struct edc_agent *agent, edc_actor_t *actor, edc_channel_type channel_type,
                                            sds arg)
{
    struct edc_task *t;
    struct edc_session *owner = NULL;
    if (arg)
    {
        /* Global lookup: a task in another session can be inspected too. */
        t = edc_agent_resolve_task(agent, arg, &owner);
        if (!t)
        {
            edc_commands_reply(agent, actor, channel_type, "Task not found.");
            return;
        }
    }
    else
    {
        owner = edc_session_manager_get_active(agent->session_manager);
        t = owner ? edc_session_get_active_task(owner) : NULL;
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
                           t->task_id, edc_task_state_str(t), t->group_id, t->description);

    /* 非当前 session 的任务额外标出属主 session, 否则用户看不出这条从哪来。 */
    {
        struct edc_session *active = edc_session_manager_get_active(agent->session_manager);
        if (owner && owner != active)
            out = sdscatprintf(out, "- **session**: `[%s]`\n", owner->session_id);
    }

    /* 未解析的跨 session 依赖: 任务被"卡住", resume 会被 gate 拒绝。 */
    {
        const char *missing = edc_scheduler_first_unresolved_dep(agent->scheduler, t);
        if (missing)
            out = sdscatprintf(out, "- **held**: waiting on `%s` (unresolved cross-session dependency)\n", missing);
    }

    /* 展示 task 实际收到的用户输入（而不只是 description）。多行输入把换行
     * 后的内容缩进两格，作为 markdown 列表项的续行，避免撑破列表。
     * P4: 消息归属 turn, user 输入直接从 turn 队列取 (不再需要 session 消息数组)。 */
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
    /* token 消耗: 该 task 生命周期内所有 LLM 响应 usage 的累计值。为 0
     * (尚未发生任何 LLM 调用, 或 provider 未返回 usage) 时不打印, 免得
     * 给刚创建、还在 WAITING_DEPS 的 task 挂一行 "0 total" 的噪音。 */
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
    edc_commands_reply(agent, actor, channel_type, out);
    sdsfree(out);
}

static void edc_commands_handle_task_output(struct edc_agent *agent, edc_actor_t *actor, edc_channel_type channel_type,
                                            sds arg)
{
    if (!arg)
    {
        edc_commands_reply(agent, actor, channel_type, "Usage: `/task output <id>`");
        return;
    }
    struct edc_task *t = edc_agent_resolve_task(agent, arg, NULL);
    if (!t)
    {
        edc_commands_reply(agent, actor, channel_type, "Task not found.");
        return;
    }
    if (!t->output)
    {
        edc_commands_reply(agent, actor, channel_type, "_No output._");
        return;
    }
    edc_commands_reply(agent, actor, channel_type, t->output);
}

static void edc_commands_handle_task_join(struct edc_agent *agent, edc_actor_t *actor, edc_channel_type channel_type,
                                          sds arg)
{
    if (!arg)
    {
        edc_commands_reply(agent, actor, channel_type, "Usage: `/task join <id>`");
        return;
    }
    /* Global lookup: the joined task may live in another session (task ids are
     * unique). The edge is pointer-based, so the dependency works either way. */
    struct edc_task *t = edc_agent_resolve_task(agent, arg, NULL);
    if (!t)
    {
        edc_commands_reply(agent, actor, channel_type, "Task not found.");
        return;
    }
    if (edc_task_is_completed(t))
    {
        /* join = "把这条已完成任务的输出注入到一个**已有的、可接收的任务**里"。
         *
         * 以前没有可用的 active task 时, 这里会新建一个没有 turn 的 "Joined task"
         * 停在 READY, 等用户下一条普通输入被 attach 进来, 把 output 一起带进 LLM。
         * 那是旧模型 (任务停在原地等输入) 的残余, attach 路径已退休, 而新模型里
         * 任务创建时就带着它的 input —— 不存在"先建一个没有 input 的空任务"。
         * 所以这里**拒绝**: 让用户先起一个任务 (/task new) 或先 /task resume 一个,
         * 再 join。源任务原样不动, 命令可以原样重试。
         * 见 docs/retire-waiting-input.md §4 第 10 行。 */
        struct edc_session *active_session = edc_session_manager_get_active(agent->session_manager);
        struct edc_task *active = active_session ? edc_session_get_active_task(active_session) : NULL;
        /* "停着" = READY (IDLE 镜像) 或等依赖 —— 两者都能接收注入的输出 */
        if (!active || !edc_task_is_parked(active))
        {
            sds reply = sdscatprintf(sdsempty(),
                                     "No task to join into — start one first (`/task new <desc>`), then "
                                     "`/task join %s` again.",
                                     t->task_id);
            edc_commands_reply(agent, actor, channel_type, reply);
            sdsfree(reply);
            return;
        }

        /* auto-chain: inject output into active task */
        edc_task_add_dependency(active, t, TASK_DEP_JOIN);

        /* 只回显原始输出，不加 "[task xxx output]" / "injected/reaped" 等元信息噪声 */
        sds reply = t->output ? sdsnew(t->output) : sdsnew("_No output._");
        edc_commands_reply(agent, actor, channel_type, reply);
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
        edc_commands_reply(agent, actor, channel_type, reply);
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
    edc_commands_reply(agent, actor, channel_type, reply);
    sdsfree(reply);
}

static void edc_commands_handle_task_chain(struct edc_agent *agent, edc_actor_t *actor, edc_channel_type channel_type,
                                           sds arg)
{
    /* /task chain <src> <dst> */
    if (!arg)
    {
        edc_commands_reply(agent, actor, channel_type, "Usage: `/task chain <src> <dst>`");
        return;
    }

    int count = 0;
    sds *tokens = sdssplitlen(arg, sdslen(arg), " ", 1, &count);
    if (count < 2)
    {
        edc_commands_reply(agent, actor, channel_type, "Usage: `/task chain <src> <dst>`");
        sdsfreesplitres(tokens, count);
        sdsfree(arg);
        return;
    }

    struct edc_session *src_owner = NULL;
    struct edc_session *dst_owner = NULL;
    struct edc_task *src = edc_agent_resolve_task(agent, tokens[0], &src_owner);
    struct edc_task *dst = edc_agent_resolve_task(agent, tokens[1], &dst_owner);
    sdsfreesplitres(tokens, count);

    if (!src)
    {
        edc_commands_reply(agent, actor, channel_type, "Source task not found.");
        return;
    }
    if (!dst)
    {
        edc_commands_reply(agent, actor, channel_type, "Target task not found.");
        return;
    }

    if (edc_task_add_dependency(dst, src, TASK_DEP_JOIN))
    {
        struct edc_session *active = edc_session_manager_get_active(agent->session_manager);
        sds reply = sdscatprintf(sdsempty(), "**Chained** — `%s` → `%s` · dep %zu/%zu", src->task_id, dst->task_id,
                                 dst->deps_completed, dst->deps_total);
        if (src_owner && src_owner != active)
            reply = sdscatprintf(reply, " · source in `[%s]`", src_owner->session_id);
        if (dst_owner && dst_owner != active)
            reply = sdscatprintf(reply, " · target in `[%s]`", dst_owner->session_id);
        edc_commands_reply(agent, actor, channel_type, reply);
        sdsfree(reply);
    }
    else
    {
        edc_commands_reply(agent, actor, channel_type, "Failed to add dependency.");
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
        edc_commands_handle_task_new(agent, actor, message->type, cmd_input_session(agent, message), arg);
    }
    else if (strncmp(content_trim, "/task fork ", 11) == 0)
    {
        arg = sdsnew(content_trim + 11);
        edc_commands_handle_task_fork(agent, actor, message->type, cmd_input_session(agent, message), arg);
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
        sds reply = sdscatprintf(sdsempty(), "Unknown `/task` subcommand. Type `/help` for available commands.");
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

        sds reply = sdscatprintf(sdsempty(), "**Unknown command** — `%s`\n\nType `/help` for available commands.",
                                 message->content);
        edc_commands_reply(agent, actor, message->type, reply);
        sdsfree(reply);
        return true;
    }

    /* non-/ input: delegate to shell module */
    return edc_shell_execute(agent, actor, message);
}
