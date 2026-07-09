#include "edc_scheduler.h"
#include "edc_compat.h"
#include "edc_session.h"
#include "edc_config.h"
#include "edc_context.h"
#include "edc_channel_actor.h"
#include "edc_llm_actor.h"
#include "edc_toolbox_actor.h"
#include "edc_turn.h"
#include "uthash.h"
#include "utlist_fix.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ── uthash wrapper: request_id → edc_task ──────────────────── */

struct edc_sched_task_entry {
    void *request_id;
    struct edc_task *task;
    UT_hash_handle hh;
};

/* ── 前向声明 ──────────────────────────────────────────────── */

static void edc_scheduler_actor_handler(edc_actor_t *actor, edc_actor_msg_t *msg);

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
    if (!sched) return;

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

    if (sched->self) edc_actor_free(sched->self);
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

struct edc_task *edc_scheduler_create_task(struct edc_scheduler *sched, const char *desc,
                                            struct edc_channel *channel)
{
    if (!sched) return NULL;

    char *tid = edc_scheduler_gen_task_id(sched);
    const char *sid = "sess-1"; /* Phase 4: from session */
    const char *gid = sched->foreground_group
                          ? sched->foreground_group->group_id
                          : tid;

    struct edc_task *task = edc_task_new(tid, sid, gid, desc, channel);
    task->state = TASK_WAITING_INPUT;
    free(tid);

    return task;
}

struct edc_task_group *edc_scheduler_create_group(struct edc_scheduler *sched,
                                                    struct edc_task *leader)
{
    if (!sched || !leader) return NULL;
    struct edc_task_group *group = edc_task_group_new(leader);
    edc_scheduler_add_group(sched, group);
    return group;
}

void edc_scheduler_add_group(struct edc_scheduler *sched, struct edc_task_group *group)
{
    if (!sched || !group) return;
    if (sched->session) edc_session_add_group(sched->session, group);
}

/* ── 查找 ──────────────────────────────────────────────────── */

struct edc_task *edc_scheduler_find_task(struct edc_scheduler *sched, void *request_id)
{
    if (!sched || !request_id) return NULL;
    struct edc_sched_task_entry *entry;
    HASH_FIND_PTR(sched->task_hash, &request_id, entry);
    return entry ? entry->task : NULL;
}

struct edc_task *edc_scheduler_find_task_by_id(struct edc_scheduler *sched, const char *task_id)
{
    if (!sched || !task_id || !sched->session) return NULL;
    struct edc_task_group *group;
    DL_FOREACH(sched->session->groups, group)
    {
        struct edc_task *task = edc_task_group_find_task(group, task_id);
        if (task) return task;
    }
    return NULL;
}

/* ── 前台/后台 ──────────────────────────────────────────────── */

void edc_scheduler_set_foreground(struct edc_scheduler *sched, struct edc_task_group *group)
{
    if (!sched) return;
    sched->foreground_group = group;
}

struct edc_task_group *edc_scheduler_get_foreground(struct edc_scheduler *sched)
{
    return sched ? sched->foreground_group : NULL;
}

/* ── 注册/注销 ──────────────────────────────────────────────── */

void edc_scheduler_register_task(struct edc_scheduler *sched, struct edc_task *task,
                                   void *request_id)
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
    if (e) { HASH_DEL(sched->task_hash, e); free(e); }
}

/* ── 信号 ──────────────────────────────────────────────────── */

void edc_scheduler_send_signal_to_task(struct edc_scheduler *sched, const char *task_id,
                                        edc_task_signal sig)
{
    struct edc_task *task = edc_scheduler_find_task_by_id(sched, task_id);
    if (!task) return;
    if (task->current_request_id)
    {
        edc_scheduler_unregister_task(sched, task->current_request_id);
    }
    edc_task_send_signal(task, sig);
}

void edc_scheduler_send_signal_to_group(struct edc_scheduler *sched, const char *group_id,
                                         edc_task_signal sig)
{
    if (!sched || !sched->session) return;
    struct edc_task_group *group = NULL;
    DL_FOREACH(sched->session->groups, group)
    {
        if (strcmp(group->group_id, group_id) == 0) break;
    }
    if (!group) return;

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
    if (!sched || !sched->session) return;
    struct edc_task_group *group, *tmp;
    DL_FOREACH_SAFE(sched->session->groups, group, tmp)
    {
        edc_scheduler_send_signal_to_group(sched, group->group_id, sig);
    }
}

/* ── 内部：消息路由 ────────────────────────────────────────── */

static void sched_handle_input(struct edc_scheduler *sched, struct edc_actor_msg *msg)
{
    struct edc_channel_message *input = msg->data;
    if (!input) return;

    if (sched->handle_command && sched->handle_command(sched, msg->from, input))
        return;

    struct edc_task_group *fg = sched->foreground_group;
    struct edc_task *task = NULL;
    bool task_is_waiting_deps = false;

    /* 优先：session->active_task 在前台 group 且为 WAITING_INPUT/WAITING_DEPS */
    if (sched->session && fg)
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
    if (!task && fg && !fg->is_orphaned && fg->member_count > 0)
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

    /* 没有可 attach 的 task → 创建新 task */
    if (!task)
    {
        task = edc_scheduler_create_task(sched, input->content, input->channel);
        input->channel = NULL;
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
            sched->send_reply(sched, task, "input queued — waiting for dependencies to complete...", msg->request_id);
    }
    else
    {
        edc_task_set_state(task, TASK_LLM_PROCESSING);
        if (sched->send_llm) sched->send_llm(sched, task, msg->request_id);
    }
}

static void sched_flush_redirects(struct edc_task *task)
{
    if (task->redirect_stdout)
    {
        FILE *f = fopen(task->redirect_stdout, task->redirect_append ? "a" : "w");
        if (f)
        {
            if (task->output) fputs(task->output, f);
            fclose(f);
        }
    }
    if (task->redirect_stderr)
    {
        FILE *f = fopen(task->redirect_stderr, "w");
        if (f)
        {
            if (task->output) fputs(task->output, f);
            fclose(f);
        }
    }
}

static void sched_handle_llm_resp(struct edc_scheduler *sched, struct edc_actor_msg *msg)
{
    struct edc_task *task = edc_scheduler_find_task(sched, msg->request_id);
    if (!task || task->cancelled_request) return;

    struct edc_llm_actor_resp *r = msg->data;
    if (!r) return;

    if (r->error)
    {
        edc_scheduler_unregister_task(sched, msg->request_id);
        task->current_request_id = NULL;
        edc_task_set_state(task, TASK_ERROR);
        if (r->content) edc_task_set_output(task, sdsnew(r->content));
        sched_flush_redirects(task);
        if (sched->send_reply) sched->send_reply(sched, task, r->error->message, msg->request_id);
        return;
    }

    if (r->reason == BC_LLM_FINISH_REASON_TOOL_CALL)
    {
        edc_task_set_state(task, TASK_TOOL_CALLING);
        if (task->active_turn)
        {
            edc_turn_add_assistant_message(task->active_turn, r->msg);
            r->msg = NULL;
        }
        /* keep task registered — tool responses use same request_id */
        if (sched->dispatch_tools) sched->dispatch_tools(sched, task, r, msg->request_id);
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
                for (size_t qi = task->active_turn->messages.head;
                     qi < task->active_turn->messages.tail; qi++)
                {
                    cJSON *item = task->active_turn->messages.data[qi];
                    cJSON_AddItemReferenceToArray(task->group->history, item);
                }
            }

            /* sync to session (persistence) */
            edc_session_add_turn(sched->session, task->active_turn);
        }
        edc_task_commit_active_turn(task);
        if (r->content) edc_task_set_output(task, sdsnew(r->content));
        sched_flush_redirects(task);
        task->completion_seq = sched->session ? sched->session->next_completion_seq++ : 0;
        edc_task_set_state(task, TASK_COMPLETED);
        edc_task_notify_dependents(task);

        /* auto-trigger dependents with queued input */
        {
            struct edc_task_dep *dep;
            LL_FOREACH(task->dependents, dep)
            {
                struct edc_task *dt = dep->target;
                if (dt->active_turn && dt->state == TASK_WAITING_INPUT)
                {
                    edc_task_set_state(dt, TASK_LLM_PROCESSING);
                    if (sched->send_llm) sched->send_llm(sched, dt, dt->current_request_id);
                }
            }
        }

        /* send_reply internally checks active_task */
        if (sched->send_reply) sched->send_reply(sched, task, r->content, msg->request_id);

        /* auto-reap disabled: tasks stay COMPLETED for cross-group join */
        /* to reclaim a task: use /task join <id> */
        (void)task;

        if (sched->save_session) sched->save_session(sched);
        return;
    }

    edc_scheduler_unregister_task(sched, msg->request_id);
    task->current_request_id = NULL;
    edc_task_set_state(task, TASK_ERROR);
    if (sched->send_reply)
        sched->send_reply(sched, task, "Internal error: unknown finish reason", msg->request_id);
}

static void sched_handle_tool_resp(struct edc_scheduler *sched, struct edc_actor_msg *msg)
{
    struct edc_task *task = edc_scheduler_find_task(sched, msg->request_id);
    if (!task || task->cancelled_request) return;

    struct edc_toolbox_actor_req *tr = msg->data;
    if (!tr) return;

    /* 添加 tool 消息到 active turn */
    if (task->active_turn && tr->result)
    {
        edc_turn_add_tool_message(task->active_turn, tr->call_id, tr->name, tr->result);
    }

    /* 若 tool chain 完成，发送续 LLM 请求 */
    if (!tr->next)
    {
        edc_task_set_state(task, TASK_LLM_PROCESSING);
        if (sched->send_llm)
            sched->send_llm(sched, task, msg->request_id);
         }
}

static void sched_handle_memory_done(struct edc_scheduler *sched, struct edc_actor_msg *msg)
{
    (void)msg;
    if (sched->session) edc_session_reload(sched->session, true);
}

/* ── Actor 入口 ────────────────────────────────────────────── */

void edc_scheduler_actor_handler(edc_actor_t *actor, edc_actor_msg_t *msg)
{
    struct edc_scheduler *sched = actor->state;
    if (!sched || !msg) return;

    switch (msg->type)
    {
    case BC_ACTOR_MSG_CHANNEL_RECV:  sched_handle_input(sched, msg);      break;
    case BC_ACTOR_MSG_LLM_RESPONSE:  sched_handle_llm_resp(sched, msg);  break;
    case BC_ACTOR_MSG_TOOL_RESPONSE: sched_handle_tool_resp(sched, msg); break;
    case BC_ACTOR_MSG_MEMORY_DONE:   sched_handle_memory_done(sched, msg); break;
    default: break;
    }

    edc_actor_msg_free(msg);
}
