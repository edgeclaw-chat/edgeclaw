#include "edc_scheduler.h"
#include "edc_channel_actor.h"
#include "edc_compat.h"
#include "edc_config.h"
#include "edc_context.h"
#include "edc_llm_actor.h"
#include "edc_session.h"
#include "edc_toolbox_actor.h"
#include "edc_turn.h"
#include "uthash.h"
#include "utlist_fix.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── uthash wrapper: request_id → edc_task ──────────────────── */

struct edc_sched_task_entry
{
    void *request_id;
    struct edc_task *task;
    UT_hash_handle hh;
};

/* ── 前向声明 ──────────────────────────────────────────────── */

static void edc_scheduler_actor_handler(edc_actor_t *actor, edc_actor_msg_t *msg);
static void sched_handle_task_snapshot(struct edc_scheduler *sched);
static void sched_notify_foreground_state(struct edc_scheduler *sched, struct edc_task *task);
static void sched_set_task_state(struct edc_scheduler *sched, struct edc_task *task, edc_task_state state);

/* ── 生命周期 ──────────────────────────────────────────────── */

struct edc_scheduler *edc_scheduler_new(struct edc_session *session, struct edc_config *config)
{
    struct edc_scheduler *sched = g_new0(struct edc_scheduler, 1);
    sched->session = session;
    sched->config = config;
    sched->task_hash = NULL;
    sched->next_task_id = 1;

    sched->self = edc_actor_new("edc_scheduler", edc_scheduler_actor_handler, sched);

    return sched;
}

void edc_scheduler_free(struct edc_scheduler *sched)
{
    if (!sched)
        return;

    struct edc_sched_task_entry *entry, *tmp;
    HASH_ITER(hh, sched->task_hash, entry, tmp)
    {
        HASH_DEL(sched->task_hash, entry);
        free(entry);
    }

    struct edc_task_group *group, *gtmp;
    if (sched->session)
    {
        DL_FOREACH_SAFE(sched->session->groups, group, gtmp)
        {
            DL_DELETE(sched->session->groups, group);
            edc_task_group_free(group);
        }
    }

    if (sched->self)
        edc_actor_free(sched->self);
    free(sched);
}

edc_actor_t *edc_scheduler_get_actor(struct edc_scheduler *sched)
{
    return sched ? sched->self : NULL;
}

/* ── Task/Group 管理 ───────────────────────────────────────── */

char *edc_scheduler_gen_task_id(struct edc_scheduler *sched)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "%lld", (long long)sched->next_task_id++);
    return strdup(buf);
}

struct edc_task *edc_scheduler_create_task(struct edc_scheduler *sched, const char *desc, edc_channel_type channel_type)
{
    if (!sched)
        return NULL;

    char *tid = edc_scheduler_gen_task_id(sched);
    const char *sid = "sess-1"; /* Phase 4: from session */
    const char *gid = sched->foreground_group ? sched->foreground_group->group_id : tid;

    struct edc_task *task = edc_task_new(tid, sid, gid, desc, channel_type);
    task->state = TASK_WAITING_INPUT;
    free(tid);

    return task;
}

struct edc_task_group *edc_scheduler_create_group(struct edc_scheduler *sched, struct edc_task *leader)
{
    if (!sched || !leader)
        return NULL;
    struct edc_task_group *group = edc_task_group_new(leader);
    edc_scheduler_add_group(sched, group);
    return group;
}

void edc_scheduler_add_group(struct edc_scheduler *sched, struct edc_task_group *group)
{
    if (!sched || !group)
        return;
    if (sched->session)
        edc_session_add_group(sched->session, group);
}

/* ── 查找 ──────────────────────────────────────────────────── */

struct edc_task *edc_scheduler_find_task(struct edc_scheduler *sched, void *request_id)
{
    if (!sched || !request_id)
        return NULL;
    struct edc_sched_task_entry *entry;
    HASH_FIND_PTR(sched->task_hash, &request_id, entry);
    return entry ? entry->task : NULL;
}

struct edc_task *edc_scheduler_find_task_by_id(struct edc_scheduler *sched, const char *task_id)
{
    if (!sched || !task_id || !sched->session)
        return NULL;
    struct edc_task_group *group;
    DL_FOREACH(sched->session->groups, group)
    {
        struct edc_task *task = edc_task_group_find_task(group, task_id);
        if (task)
            return task;
    }
    return NULL;
}

/* ── 前台/后台 ──────────────────────────────────────────────── */

void edc_scheduler_set_foreground(struct edc_scheduler *sched, struct edc_task_group *group)
{
    if (!sched)
        return;
    sched->foreground_group = group;
}

struct edc_task_group *edc_scheduler_get_foreground(struct edc_scheduler *sched)
{
    return sched ? sched->foreground_group : NULL;
}

/* ── 注册/注销 ──────────────────────────────────────────────── */

void edc_scheduler_register_task(struct edc_scheduler *sched, struct edc_task *task, void *request_id)
{
    struct edc_sched_task_entry *e = g_new0(struct edc_sched_task_entry, 1);
    e->request_id = request_id;
    e->task = task;
    HASH_ADD_PTR(sched->task_hash, request_id, e);
}

void edc_scheduler_unregister_task(struct edc_scheduler *sched, void *request_id)
{
    struct edc_sched_task_entry *e;
    HASH_FIND_PTR(sched->task_hash, &request_id, e);
    if (e)
    {
        HASH_DEL(sched->task_hash, e);
        free(e);
    }
}

/* ── 信号 ──────────────────────────────────────────────────── */

void edc_scheduler_send_signal_to_task(struct edc_scheduler *sched, const char *task_id, edc_task_signal sig)
{
    struct edc_task *task = edc_scheduler_find_task_by_id(sched, task_id);
    if (!task)
        return;
    if (task->current_request_id)
    {
        edc_scheduler_unregister_task(sched, task->current_request_id);
    }
    edc_task_send_signal(task, sig);
}

void edc_scheduler_send_signal_to_group(struct edc_scheduler *sched, const char *group_id, edc_task_signal sig)
{
    if (!sched || !sched->session)
        return;
    struct edc_task_group *group = NULL;
    DL_FOREACH(sched->session->groups, group)
    {
        if (strcmp(group->group_id, group_id) == 0)
            break;
    }
    if (!group)
        return;

    struct edc_task *task;
    LL_FOREACH2(group->members, task, group_next)
    {
        if (task->current_request_id)
        {
            edc_scheduler_unregister_task(sched, task->current_request_id);
        }
    }
    edc_task_group_send_signal(group, sig);
}

void edc_scheduler_send_signal_to_all(struct edc_scheduler *sched, edc_task_signal sig)
{
    if (!sched || !sched->session)
        return;
    struct edc_task_group *group, *tmp;
    DL_FOREACH_SAFE(sched->session->groups, group, tmp)
    {
        edc_scheduler_send_signal_to_group(sched, group->group_id, sig);
    }
}

/* ── 前台任务状态通知 ─────────────────────────────────────── */

/* 前台 group 的 active task 状态变化后立即推送一次 task 树快照。很多这类
 * 迁移不产生任何 channel 消息(依赖完成后自动触发、取消信号、LLM/工具链
 * 内部切换等), 而 TUI 的 "处理中" 动画与侧边栏完全由快照驱动; 若不主动
 * 推送, 空闲状态的 TUI 没有任何帧在跑轮询, 只能等下一条消息/按键才碰巧
 * 发现状态变了。快照在 scheduler 线程同步生成, 与 TUI 拉取共用同一路径。 */
static void sched_notify_foreground_state(struct edc_scheduler *sched, struct edc_task *task)
{
    if (!sched || !task || !sched->session || !sched->foreground_group)
        return;
    if (edc_session_get_active_task(sched->session) != task)
        return;
    if (task->group != sched->foreground_group)
        return;
    sched_handle_task_snapshot(sched);
}

/* 统一的状态迁移入口: 设置状态后, 若被改变的是前台 active task, 主动推送
 * 快照, 使 TUI 的底部动画跟上真实进程状态, 而不是等轮询或消息到达。 */
static void sched_set_task_state(struct edc_scheduler *sched, struct edc_task *task, edc_task_state state)
{
    edc_task_set_state(task, state);
    sched_notify_foreground_state(sched, task);
}

/* ── 内部：消息路由 ────────────────────────────────────────── */

static void sched_handle_input(struct edc_scheduler *sched, struct edc_actor_msg *msg)
{
    struct edc_channel_message *input = msg->data;
    if (!input)
        return;

    struct edc_task_group *fg = sched->foreground_group;
    struct edc_task *task = NULL;
    bool task_is_waiting_deps = false;

    /* 斜杠命令 / @组寻址 交给命令处理器 (shell); 普通输入先尝试 attach 到
     * 前台 group 中等待输入的任务 (fork leader、/task new 无输入等)。 */
    const bool is_plain = input->content && input->content[0] != '/' && input->content[0] != '@';

    /* 优先：active_task 在前台 group 且为 WAITING_INPUT/WAITING_DEPS */
    if (is_plain && sched->session && fg)
    {
        struct edc_task *at = edc_session_get_active_task(sched->session);
        if (at && (at->state == TASK_WAITING_INPUT || at->state == TASK_WAITING_DEPS))
        {
            struct edc_task *m;
            LL_FOREACH2(fg->members, m, group_next)
            {
                if (m == at)
                {
                    task = at;
                    task_is_waiting_deps = (at->state == TASK_WAITING_DEPS);
                    break;
                }
            }
        }
    }

    /* 其次：扫描前台 group 中第一个 WAITING_INPUT (不含 WAITING_DEPS) */
    if (is_plain && !task && fg && !fg->is_orphaned && fg->member_count > 0)
    {
        struct edc_task *m;
        LL_FOREACH2(fg->members, m, group_next)
        {
            if (m->state == TASK_WAITING_INPUT)
            {
                task = m;
                edc_session_set_active_task(sched->session, task);
                break;
            }
        }
    }

    /* 斜杠/@ 或没有可 attach 的等待任务 → 命令处理器 (shell 会新建 task) */
    if (!task)
    {
        if (sched->handle_command && sched->handle_command(sched, msg->from, input))
            return;

        /* 无命令处理器 (或未处理) → 兜底新建 task */
        task = edc_scheduler_create_task(sched, input->content, input->type);
        edc_session_set_active_task(sched->session, task);

        if (!fg || fg->is_orphaned || fg->member_count == 0)
        {
            fg = edc_scheduler_create_group(sched, task);
            edc_scheduler_set_foreground(sched, fg);
        }
        else
        {
            edc_task_group_add_task(fg, task);
        }
    }

    struct edc_turn *turn = edc_turn_new();
    turn->task_id = strdup(task->task_id);
    edc_turn_add_user_message(turn, input->content);
    task->active_turn = turn;
    task->current_request_id = msg->request_id;
    edc_scheduler_register_task(sched, task, msg->request_id);

    if (task_is_waiting_deps)
    {
        /* 排队等待: turn 已创建, deps 完成后自动触发 */
        if (sched->send_reply)
            sched->send_reply(sched, task, "input queued — waiting for dependencies to complete...", EDC_CHANNEL_MESSAGE_SYS_TYPE, msg->request_id);
    }
    else
    {
        sched_set_task_state(sched, task, TASK_LLM_PROCESSING);
        if (sched->send_llm)
            sched->send_llm(sched, task, msg->request_id);
    }
}

static void sched_flush_redirects(struct edc_task *task)
{
    if (task->redirect_stdout)
    {
        FILE *f = fopen(task->redirect_stdout, task->redirect_append ? "a" : "w");
        if (f)
        {
            if (task->output)
                fputs(task->output, f);
            fclose(f);
        }
    }
    if (task->redirect_stderr)
    {
        FILE *f = fopen(task->redirect_stderr, "w");
        if (f)
        {
            if (task->output)
                fputs(task->output, f);
            fclose(f);
        }
    }
}

static void sched_handle_llm_resp(struct edc_scheduler *sched, struct edc_actor_msg *msg)
{
    struct edc_task *task = edc_scheduler_find_task(sched, msg->request_id);
    if (!task || task->cancelled_request)
        return;

    struct edc_llm_actor_resp *r = msg->data;
    if (!r)
        return;

    if (r->error)
    {
        edc_scheduler_unregister_task(sched, msg->request_id);
        task->current_request_id = NULL;
        sched_set_task_state(sched, task, TASK_ERROR);
        if (r->content)
            edc_task_set_output(task, sdsnew(r->content));
        sched_flush_redirects(task);
        if (sched->send_reply)
            sched->send_reply(sched, task, r->error->message, EDC_CHANNEL_MESSAGE_SYS_TYPE, msg->request_id);
        return;
    }

    if (r->reason == BC_LLM_FINISH_REASON_TOOL_CALL)
    {
        sched_set_task_state(sched, task, TASK_TOOL_CALLING);
        if (task->active_turn)
        {
            edc_turn_add_assistant_message(task->active_turn, r->msg);
            r->msg = NULL;
        }
        /* keep task registered — tool responses use same request_id */
        if (sched->dispatch_tools)
            sched->dispatch_tools(sched, task, r, msg->request_id);
        return;
    }

    if (r->reason == BC_LLM_FINISH_REASON_STOP)
    {
        edc_scheduler_unregister_task(sched, msg->request_id);
        task->current_request_id = NULL;
        if (task->active_turn)
        {
            edc_turn_add_assistant_message(task->active_turn, r->msg);
            r->msg = NULL;

            /* sync turn messages to group history (context sharing) */
            if (task->group && task->active_turn)
            {
                for (size_t qi = task->active_turn->messages.head; qi < task->active_turn->messages.tail; qi++)
                {
                    cJSON *item = task->active_turn->messages.data[qi];
                    cJSON_AddItemReferenceToArray(task->group->history, item);
                }
            }

            /* sync to session (persistence) */
            edc_session_add_turn(sched->session, task->active_turn);
        }
        edc_task_commit_active_turn(task);
        if (r->content)
            edc_task_set_output(task, sdsnew(r->content));
        sched_flush_redirects(task);
        task->completion_seq = sched->session ? sched->session->next_completion_seq++ : 0;
        sched_set_task_state(sched, task, TASK_COMPLETED);
        edc_task_notify_dependents(task);

        /* auto-trigger dependents with queued input */
        {
            struct edc_task_dep *dep;
            LL_FOREACH(task->dependents, dep)
            {
                struct edc_task *dt = dep->target;
                if (dt->active_turn && dt->state == TASK_WAITING_INPUT)
                {
                    sched_set_task_state(sched, dt, TASK_LLM_PROCESSING);
                    if (sched->send_llm)
                        sched->send_llm(sched, dt, dt->current_request_id);
                }
            }
        }

        /* send_reply internally checks active_task */
        if (sched->send_reply)
            sched->send_reply(sched, task, r->content, EDC_CHANNEL_MESSAGE_RESP_TYPE, msg->request_id);

        /* auto-reap disabled: tasks stay COMPLETED for cross-group join */
        /* to reclaim a task: use /task join <id> */
        (void)task;

        /* 触发 L1 折叠检查 (task 完成后可能达到 group fold 阈值) */
        if (sched->trigger_memory)
            sched->trigger_memory(sched);

        if (sched->save_session)
            sched->save_session(sched);
        return;
    }

    edc_scheduler_unregister_task(sched, msg->request_id);
    task->current_request_id = NULL;
    sched_set_task_state(sched, task, TASK_ERROR);
    if (sched->send_reply)
        sched->send_reply(sched, task, "Internal error: unknown finish reason", EDC_CHANNEL_MESSAGE_SYS_TYPE, msg->request_id);
}

static void sched_handle_tool_resp(struct edc_scheduler *sched, struct edc_actor_msg *msg)
{
    struct edc_task *task = edc_scheduler_find_task(sched, msg->request_id);
    if (!task || task->cancelled_request)
        return;

    struct edc_toolbox_actor_req *tr = msg->data;
    if (!tr)
        return;

    /* 添加 tool 消息到 active turn */
    if (task->active_turn && tr->result)
    {
        edc_turn_add_tool_message(task->active_turn, tr->call_id, tr->name, tr->result);
    }

    /* 若 tool chain 完成，发送续 LLM 请求 */
    if (!tr->next)
    {
        sched_set_task_state(sched, task, TASK_LLM_PROCESSING);
        if (sched->send_llm)
            sched->send_llm(sched, task, msg->request_id);
    }
}

static void sched_handle_memory_done(struct edc_scheduler *sched, struct edc_actor_msg *msg)
{
    (void)msg;
    if (sched->session)
        edc_session_reload(sched->session, true);
}

/* toolbox 请求把工具结果回显到终端 (TOOL 消息)。与 thought/RESP 一致，
 * 只在调度线程上检查 session->active_task：后台任务的工具输出一律静默。 */
static void sched_handle_tool_emit(struct edc_scheduler *sched, struct edc_actor_msg *msg)
{
    struct edc_toolbox_emit_msg *emit = msg->data;
    if (!emit || !emit->content)
        return;

    struct edc_task *task = edc_scheduler_find_task(sched, msg->request_id);
    if (!task || task->cancelled_request)
        return;

    /* only active task outputs to terminal */
    if (!sched->session || edc_session_get_active_task(sched->session) != task)
        return;

    if (sched->send_tool_message)
        sched->send_tool_message(sched, task, emit->header, emit->content, emit->render_pref, msg->request_id);
}

/* ── Actor 入口 ────────────────────────────────────────────── */

/* 序列化当前 session 的 task 树 (group → members)，供 TUI 侧边栏渲染。
 * 用 cJSON 生成 JSON 文本，在 scheduler 线程上执行，天然无并发。 */
static void sched_handle_task_snapshot(struct edc_scheduler *sched)
{
    if (!sched || !sched->session)
        return;

    cJSON *root = cJSON_CreateObject();
    struct edc_session *sess = sched->session;
    cJSON_AddStringToObject(root, "session_id", sess->session_id ? sess->session_id : "");
    cJSON_AddStringToObject(root, "session_name", edc_session_get_name(sess));

    cJSON *groups = cJSON_AddArrayToObject(root, "groups");
    struct edc_task_group *g;
    DL_FOREACH(sched->session->groups, g)
    {
        cJSON *gj = cJSON_CreateObject();
        cJSON_AddStringToObject(gj, "id", g->group_id ? g->group_id : "");
        cJSON_AddNumberToObject(gj, "member_count", g->member_count);
        cJSON_AddBoolToObject(gj, "orphaned", g->is_orphaned);
        cJSON_AddBoolToObject(gj, "fg", sched->foreground_group == g);

        cJSON *tasks = cJSON_AddArrayToObject(gj, "tasks");
        struct edc_task *t;
        LL_FOREACH2(g->members, t, group_next)
        {
            cJSON *tj = cJSON_CreateObject();
            cJSON_AddStringToObject(tj, "id", t->task_id ? t->task_id : "");
            cJSON_AddStringToObject(tj, "state", edc_task_state_name(t->state));
            cJSON_AddBoolToObject(tj, "leader", t->is_group_leader);
            cJSON_AddBoolToObject(tj, "active", sess->active_task == t);
            cJSON_AddStringToObject(tj, "desc", t->description ? t->description : "");
            cJSON_AddItemToArray(tasks, tj);
        }

        cJSON_AddItemToArray(groups, gj);
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json)
        return;

    sds out = sdsnew(json);
    cJSON_free(json);

    if (sched->snapshot_ready)
        sched->snapshot_ready(sched, out);
    else
        sdsfree(out);
}

void edc_scheduler_actor_handler(edc_actor_t *actor, edc_actor_msg_t *msg)
{
    struct edc_scheduler *sched = actor->state;
    if (!sched || !msg)
        return;

    switch (msg->type)
    {
    case BC_ACTOR_MSG_CHANNEL_RECV:
        sched_handle_input(sched, msg);
        break;
    case BC_ACTOR_MSG_LLM_RESPONSE:
        sched_handle_llm_resp(sched, msg);
        break;
    case BC_ACTOR_MSG_TOOL_RESPONSE:
        sched_handle_tool_resp(sched, msg);
        break;
    case BC_ACTOR_MSG_TOOL_EMIT:
        sched_handle_tool_emit(sched, msg);
        break;
    case BC_ACTOR_MSG_MEMORY_DONE:
        sched_handle_memory_done(sched, msg);
        break;
    case BC_ACTOR_MSG_GROUP_FOLD_DONE:
        /* L1 折叠完成, 持久化 state (turns_folded/memory/fold_cursor) */
        if (sched->save_session)
            sched->save_session(sched);
        break;
    case BC_ACTOR_MSG_TASK_SNAPSHOT:
        sched_handle_task_snapshot(sched);
        break;
    default:
        break;
    }

    edc_actor_msg_free(msg);
}
