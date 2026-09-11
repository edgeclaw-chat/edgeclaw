#include "edc_scheduler.h"
#include "edc_channel_actor.h"
#include "edc_cmd_parse.h"
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
/* 任务树快照: 只服务侧边栏 (任务树 + todo + 计数)。对话内容不再随快照下发
 * —— 聊天区由 TUI 自己持久化/回放显示流 (见 edc_tui_window.cpp)。 */
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
    /* 任务归属调度器当前绑定的 session (经 edc_scheduler_set_session 切换),
     * 不再硬编码 "sess-1"。无 session 时 (测试/极端情况) 才回退占位 id。 */
    const char *sid =
        (sched->session && sched->session->session_id) ? sched->session->session_id : "sess-1";
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

/* ── 当前 session ──────────────────────────────────────────── */

struct edc_session *edc_scheduler_get_session(struct edc_scheduler *sched)
{
    return sched ? sched->session : NULL;
}

void edc_scheduler_set_session(struct edc_scheduler *sched, struct edc_session *session)
{
    if (!sched)
        return;
    sched->session = session;
    /* 切换 session 后旧 session 的前台组不再有效; 命令层随后按需 reload/恢复 */
    sched->foreground_group = NULL;
}

bool edc_scheduler_has_active_tasks(struct edc_scheduler *sched)
{
    if (!sched || !sched->session)
        return false;
    struct edc_task_group *g;
    DL_FOREACH(sched->session->groups, g)
    {
        struct edc_task *t;
        LL_FOREACH2(g->members, t, group_next)
        {
            if (t->state != TASK_COMPLETED && t->state != TASK_CANCELLED && t->state != TASK_ERROR &&
                t->state != TASK_SUSPENDED)
                return true;
        }
    }
    return false;
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

    if (task->state == TASK_CANCELLED)
    {
        /* 显式取消必须立即持久化 + 推送快照: CANCELLED 目前只在内存里, 若不
         * 保存, 重启后任务会按 session.json 里的旧状态恢复 (运行中任务会被
         * 降级成 WAITING_INPUT), 取消看起来像没生效。此函数只被用户命令
         * (/task stop) 调用, 不涉及 teardown 的 remove_all_turns。 */
        sched_notify_foreground_state(sched, task);
        if (sched->save_session)
            sched->save_session(sched);
    }
}

/* 对单个 group 发信号: 先反注册要取消的 in-flight 请求 (否则已作废的 LLM/
 * 工具响应回来时仍能命中 task_hash), 再交给 task 层逐成员发送。
 * running_only=true 时只处理 edc_task_is_running() 的成员 (离开 session 的
 * "只取消正在跑的"), 其余成员的状态与注册都原样保留。 */
static void sched_signal_group_members(struct edc_scheduler *sched, struct edc_task_group *group, edc_task_signal sig,
                                       bool running_only)
{
    if (!sched || !group)
        return;
    struct edc_task *task;
    LL_FOREACH2(group->members, task, group_next)
    {
        if (running_only && !edc_task_is_running(task))
            continue;
        if (task->current_request_id)
        {
            edc_scheduler_unregister_task(sched, task->current_request_id);
        }
    }
    if (running_only)
        edc_task_group_send_signal_running(group, sig);
    else
        edc_task_group_send_signal(group, sig);
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
    sched_signal_group_members(sched, group, sig, false);
}

void edc_scheduler_send_signal_to_session(struct edc_scheduler *sched, struct edc_session *session, edc_task_signal sig,
                                          bool running_only)
{
    if (!sched || !session)
        return;
    struct edc_task_group *group, *tmp;
    DL_FOREACH_SAFE(session->groups, group, tmp)
    {
        sched_signal_group_members(sched, group, sig, running_only);
    }
}

void edc_scheduler_send_signal_to_all(struct edc_scheduler *sched, edc_task_signal sig)
{
    edc_scheduler_send_signal_to_session(sched, sched ? sched->session : NULL, sig, false);
}

/* ── resume ────────────────────────────────────────────────── */

/* resume 是低频且不与输入路径并发触发 (统一在 scheduler actor 线程上跑), 用一个
 * 高位起步的进程内计数器生成 request_id, 避免与 shell 输入路径的小整数 counter
 * 发生指针碰撞。 */
static int64_t g_resume_req_seq = (int64_t)1 << 40;

static void *sched_gen_resume_request_id(void)
{
    return (void *)(intptr_t)(++g_resume_req_seq);
}

void edc_scheduler_resume_task(struct edc_scheduler *sched, struct edc_task *task)
{
    if (!sched || !task)
        return;

    /* 1. 依赖未满足 (排除已取消的边) → 回 WAITING_DEPS */
    {
        size_t total = 0, completed = 0;
        struct edc_task_dep *dep;
        LL_FOREACH(task->deps, dep)
        {
            if (dep->cancelled)
                continue;
            total++;
            if (dep->completed)
                completed++;
        }
        task->deps_total = total;
        task->deps_completed = completed;
        if (completed < total)
        {
            sched_set_task_state(sched, task, TASK_WAITING_DEPS);
            return;
        }
    }

    /* 2. 账本里还有未完成的工具调用 */
    if (edc_task_has_pending_tool_calls(task))
    {
        /* 2a. EXECUTING (已派发、结果未回, 即中断时正在执行的): 人工补一条
         *     "执行到一半被中断"的 tool 响应, 并从账本删除。 */
        struct edc_tool_call_record *rec, *tmp;
        LL_FOREACH_SAFE(task->tool_calls, rec, tmp)
        {
            if (rec->status != TOOL_CALL_EXECUTING)
                continue;
            if (task->active_turn && rec->call_id)
            {
                const char *note =
                    "[interrupted: this tool was still running when the process restarted; "
                    "it did not produce a result]";
                edc_turn_add_tool_message(task->active_turn, rec->call_id, rec->name ? rec->name : "", note);
            }
            edc_task_tool_call_remove(task, rec->call_id);
        }

        /* 2b. 仍未执行 (PENDING) 的: 重新派发 */
        if (edc_task_has_pending_tool_calls(task))
        {
            void *req_id = sched_gen_resume_request_id();
            sched_set_task_state(sched, task, TASK_TOOL_CALLING);
            edc_scheduler_register_task(sched, task, req_id);
            task->current_request_id = req_id;
            /* 重发成功即等 toolbox 回来; 各工具的 EXECUTING 由 toolbox 的
             * TOOL_STARTED 上报后按 call_id 置位 —— 不再按链序预标。 */
            if (!sched->redispatch_tools || sched->redispatch_tools(sched, task, req_id) <= 0)
            {
                /* 无重发通道 / 无可派发 → 退回等待输入, 避免卡死 */
                edc_scheduler_unregister_task(sched, req_id);
                task->current_request_id = NULL;
                sched_set_task_state(sched, task, TASK_WAITING_INPUT);
            }
            return;
        }
        /* 2c. 只有 EXECUTING 且已补完响应 → 所有工具都有结果, 落到下面续 LLM */
    }

    /* 3. 有待跑的 turn → LLM_PROCESSING, 重发 LLM */
    if (task->active_turn)
    {
        void *req_id = sched_gen_resume_request_id();
        sched_set_task_state(sched, task, TASK_LLM_PROCESSING);
        edc_scheduler_register_task(sched, task, req_id);
        task->current_request_id = req_id;
        if (sched->send_llm)
        {
            task->llm_rounds++;
            sched->send_llm(sched, task, req_id);
        }
        else
        {
            edc_scheduler_unregister_task(sched, req_id);
            task->current_request_id = NULL;
            sched_set_task_state(sched, task, TASK_WAITING_INPUT);
        }
        return;
    }

    /* 4. 无待跑内容 → WAITING_INPUT */
    sched_set_task_state(sched, task, TASK_WAITING_INPUT);
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

    /* 普通输入也遵循 python-like 引号解析："""…""" / '''…''' / '…' / "…" 的
     * 引号在 attach 到等待任务时也要剥掉，与 @ 新建 task 的 shell 路径一致。
     * (/ 与 @ 输入不走到这里，已在上方交给命令处理器。) */
    sds effective_input = NULL;
    if (is_plain)
    {
        struct edc_cmd_task_opts popts;
        edc_cmd_parse_task_opts(input->content, &popts);
        if (popts.input && sdslen(popts.input) > 0)
            effective_input = sdsdup(popts.input);
        edc_cmd_task_opts_free(&popts);
    }

    struct edc_turn *turn = edc_turn_new();
    turn->task_id = strdup(task->task_id);
    edc_turn_add_user_message(turn, effective_input ? effective_input : input->content);
    if (effective_input)
        sdsfree(effective_input);
    task->active_turn = turn;
    task->llm_rounds = 0; /* 新一轮输入: 重置 max_iter 迭代预算 */
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
        {
            task->llm_rounds++; /* 发出一条 LLM 请求 → 消耗一轮迭代预算 */
            sched->send_llm(sched, task, msg->request_id);
        }
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

    /* Token 账: 把本次响应的 usage 累加到 task。放在分支之前, 让 STOP、
     * TOOL_CALLING 以及 error 三条路径都计入 —— 一次用户输入常触发多轮
     * 请求 (工具链续发), 每轮都要算, 否则会漏掉中间轮。 */
    edc_task_add_usage(task, r->usage.prompt_tokens, r->usage.completion_tokens, r->usage.total_tokens,
                       r->usage.cached_tokens);

    if (r->error)
    {
        edc_scheduler_unregister_task(sched, msg->request_id);
        task->current_request_id = NULL;
        sched_set_task_state(sched, task, TASK_ERROR);
        if (r->content)
            edc_task_set_output(task, sdsnew(r->content));
        sched_flush_redirects(task);
        if (sched->send_reply)
        {
            const char *err = r->error->message ? r->error->message : "LLM request failed";
            bool no_key = sched->config && (!sched->config->llm_api_conf.api_key ||
                                            sdslen(sched->config->llm_api_conf.api_key) == 0);
            if (no_key)
            {
                /* First run: with no API key the request can only fail, so
                 * point the user at the /connect dialog instead of just
                 * reporting the error. */
                sds combined = sdscatprintf(
                    sdsempty(), "%s\n\nNo API key configured — run /connect to set the API URL, model and key.", err);
                sched->send_reply(sched, task, combined, EDC_CHANNEL_MESSAGE_SYS_TYPE, msg->request_id);
                sdsfree(combined);
            }
            else
            {
                sched->send_reply(sched, task, err, EDC_CHANNEL_MESSAGE_SYS_TYPE, msg->request_id);
            }
        }
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
        /* 账本: 先把本轮 tool_calls 全部登记为 PENDING (dispatch 会转移
         * r->tool_calls 里 name/args/id 的所有权, 故必须先复制登记), 派发后
         * 再把链首提升 EXECUTING —— 中断后 resume 据此判定哪些工具需要重新
         * 执行、哪些只需补一条"中断"响应。 */
        if (r->tool_calls)
        {
            for (unsigned i = 0; i < utarray_len(r->tool_calls); i++)
            {
                struct edc_llm_chat_tool_call *tcall =
                    (struct edc_llm_chat_tool_call *)utarray_eltptr(r->tool_calls, i);
                if (tcall && tcall->type == BC_LLM_TOOL_CALL_TYPE_FUNCTION)
                    edc_task_tool_call_add(task, tcall->id, tcall->function.name, tcall->function.arguments);
            }
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
                    /* 带归属写入: 回填历史时每条消息可标注 "task N" */
                    edc_task_group_history_add_ref(task->group, item, task->task_id);
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

        /* session 消息数刚因 turn 提交而增长。sched_set_task_state 里的
         * notify 只在前台 active task 上触发；被更新输入顶替、或在后台/
         * 依赖链中完成的任务其 turn 是"静默"的（send_reply 会丢弃非
         * active task 的输出，不产生任何 channel 消息），若不在状态迁移
         * 之外主动补推一次快照，TUI 侧边栏的 session 统计会一直停留在
         * 旧值。快照带 COMPLETED 状态 + 新 message_count 一并刷新。 */
        if (!sched->session || edc_session_get_active_task(sched->session) != task ||
            task->group != sched->foreground_group)
        {
            sched_handle_task_snapshot(sched);
        }

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
                    {
                        dt->llm_rounds++; /* 排队的依赖输入开始消耗预算 */
                        sched->send_llm(sched, dt, dt->current_request_id);
                    }
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

/* 到达 max_iter 上限: 构造一条 STOP 语义的 LLM 响应 (内容为收尾说明),
 * 复用 sched_handle_llm_resp 的完整终结路径 —— turn (用户消息 + assistant
 * tool_call + 工具结果) 整体入库、任务置 COMPLETED、通知依赖者、触发 L1
 * 折叠与 save_session, 保证被打断的上下文仍是连贯的。 */
static void sched_stop_at_iteration_cap(struct edc_scheduler *sched, struct edc_task *task, struct edc_actor_msg *msg)
{
    int max_iter = (sched->config && sched->config->max_iter > 0) ? sched->config->max_iter : 0;

    sds notice = sdsempty();
    notice = sdscatprintf(
        notice,
        "[stopped: reached max_iter=%d LLM/tool rounds without a final answer — iteration budget exhausted. "
        "Send a follow-up message to continue.]",
        max_iter);

    cJSON *assistant = cJSON_CreateObject();
    cJSON_AddStringToObject(assistant, "role", "assistant");
    cJSON_AddStringToObject(assistant, "content", notice);

    struct edc_llm_actor_resp fake;
    memset(&fake, 0, sizeof(fake));
    fake.reason = BC_LLM_FINISH_REASON_STOP;
    fake.content = notice; /* STOP 分支用 sdsnew 拷贝, 不会持有 */
    fake.msg = assistant;  /* 所有权转入 active_turn (与真实响应一致) */

    struct edc_actor_msg evt;
    memset(&evt, 0, sizeof(evt));
    evt.type = BC_ACTOR_MSG_LLM_RESPONSE;
    evt.from = msg->from;
    evt.request_id = msg->request_id; /* task 此刻仍注册在该 request_id 下 */
    evt.data = &fake;

    sched_handle_llm_resp(sched, &evt);

    sdsfree(notice);
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

    /* 账本: 该工具已完成 → 删除该条记录 (EXECUTING 由 toolbox 的 TOOL_STARTED 上报置位) */
    edc_task_tool_call_remove(task, tr->call_id);

    /* 若 tool chain 完成，发送续 LLM 请求 */
    if (!tr->next)
    {
        /* max_iter 治理: 工具链结束后若已耗尽本轮 LLM 迭代预算, 不再续发
         * LLM —— 模型已拿到最后一轮工具结果仍要继续时, 说明大概率在工具
         * 循环里空转。max_iter <= 0 (如 -1 = 无限/默认, 或 0 兼容旧行为)
         * 表示不限制。 */
        if (sched->config->max_iter > 0 && task->llm_rounds >= sched->config->max_iter)
        {
            sched_stop_at_iteration_cap(sched, task, msg);
            return;
        }

        sched_set_task_state(sched, task, TASK_LLM_PROCESSING);
        if (sched->send_llm)
        {
            task->llm_rounds++; /* 工具链结束后续发 → 再消耗一轮预算 */
            sched->send_llm(sched, task, msg->request_id);
        }
    }
}

/* toolbox 上报"某个工具真正开始执行" (data = call_id): 把账本里对应记录置
 * EXECUTING。这样中断后 resume 能精确区分"未执行"与"执行中", 不再依赖链序。 */
static void sched_handle_tool_started(struct edc_scheduler *sched, struct edc_actor_msg *msg)
{
    struct edc_task *task = edc_scheduler_find_task(sched, msg->request_id);
    if (!task || task->cancelled_request)
        return;
    const char *call_id = (const char *)msg->data;
    if (call_id)
        edc_task_tool_call_mark_executing(task, call_id);
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

    /* todowrite 每次成功更新 (含清空) 都会回显一条 TOOL 消息; 该消息只
     * 携带文本, todo 本身已挂到 task->todolist。多轮工具链内 task 状态不再
     * 变化 (TOOL_CALLING 期间), 不会触发状态通知快照 —— 这里主动补推一次,
     * 让 TUI 的 TodoWidget 立即按新清单刷新 (规则3)。 */
    if (emit->header && strcmp(emit->header, "todowrite") == 0)
        sched_handle_task_snapshot(sched);
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
    /* 统计口径 = 已提交 session 消息 + session 内所有 task 在途 turn 的消息数。
     * 语义: session 总消息数。任何 task (前台 active、被顶替、后台/依赖链中、
     * WAITING_DEPS 排队) 的 turn 在 STOP 时都会整体提交进 session, 其提交前
     * 的消息同样属于 session; 只统计 active task 会漏掉其余在途 turn 的消息,
     * 侧边栏数字仍会在多 task 并发/排队时停滞。逐 task 累加 active_turn 后,
     * 计数随任意 task 消息流增长; 提交时 edc_session_add_turn 把消息移入
     * session、active_turn 随即清空, 不会重复计数。 */
    size_t msg_count = edc_session_get_message_count(sess);
    {
        struct edc_task_group *g;
        DL_FOREACH(sess->groups, g)
        {
            struct edc_task *t;
            LL_FOREACH2(g->members, t, group_next)
            {
                if (t->active_turn)
                    msg_count += edc_queue_length(&t->active_turn->messages);
            }
        }
    }
    cJSON_AddNumberToObject(root, "message_count", (double)msg_count);
    cJSON_AddStringToObject(root, "created_at", sess->created_at ? sess->created_at : "");

    cJSON *groups = cJSON_AddArrayToObject(root, "groups");
    struct edc_task_group *g;
    DL_FOREACH(sched->session->groups, g)
    {
        cJSON *gj = cJSON_CreateObject();
        cJSON_AddStringToObject(gj, "id", g->group_id ? g->group_id : "");
        cJSON_AddNumberToObject(gj, "member_count", g->member_count);
        cJSON_AddBoolToObject(gj, "orphaned", g->is_orphaned);
        cJSON_AddBoolToObject(gj, "fg", sched->foreground_group == g);
        /* 组消息数: group history 已提交消息 (不含 history[0] 的组 system
         * prompt) + 组内各 task 在途 turn 的消息。history 在 task turn STOP
         * 提交前同步引用 (提交后 active_turn 清空), 因此不重复计数; 侧边栏
         * 借此能看到组内任意 task 流式消息的实时增长。 */
        size_t g_msg_count = g->history ? (size_t)cJSON_GetArraySize(g->history) : 0;
        if (g_msg_count > 0)
            g_msg_count -= 1;
        long long g_tokens_total = 0;
        {
            struct edc_task *mt;
            LL_FOREACH2(g->members, mt, group_next)
            {
                if (mt->active_turn)
                    g_msg_count += edc_queue_length(&mt->active_turn->messages);
                g_tokens_total += mt->tokens_total;
            }
        }
        cJSON_AddNumberToObject(gj, "msg_count", (double)g_msg_count);
        cJSON_AddNumberToObject(gj, "tokens_total", (double)g_tokens_total);

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
            /* token 消耗随快照下发, 供侧边栏按 task 展示. 数值用 double 承载
             * (cJSON 无 64 位整型), 计数远小于 2^53, 无精度损失. */
            cJSON_AddNumberToObject(tj, "tokens_prompt", (double)t->tokens_prompt);
            cJSON_AddNumberToObject(tj, "tokens_completion", (double)t->tokens_completion);
            cJSON_AddNumberToObject(tj, "tokens_total", (double)t->tokens_total);
            cJSON_AddNumberToObject(tj, "tokens_cached", (double)t->tokens_cached);
            /* todo 清单是 task 状态的一部分: 快照携带每个持有清单的 task 的
             * todolist (item 数组, 同 todowrite 参数形状)。TUI 借此把
             * fg 组 active task 的清单渲染到右侧 TodoWidget —— 快照是侧边栏
             * (任务树 + todo) 的唯一数据源, 不再需要旁路 payload 通道。 */
            if (t->todolist && cJSON_GetArraySize(t->todolist) > 0)
                cJSON_AddItemToObject(tj, "todolist", cJSON_Duplicate(t->todolist, 1));
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
    case BC_ACTOR_MSG_TOOL_STARTED:
        sched_handle_tool_started(sched, msg);
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
