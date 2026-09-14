#include "edc_session_store.h"
#include "edc_scheduler.h"
#include "edc_session.h"
#include "edc_session_manager.h"
#include "edc_task.h"
#include "log.h"
#include "sds.h"
#include "utlist_fix.h"
#include <stdlib.h>
#include <string.h>

/* ── 跨 session task 解析 (S2) ───────────────────────────────
 * task id 全局唯一 (见 edc_session_manager_find_task_any), 所以命中唯一。
 * 内存查找只覆盖"本 run 已激活"的 session; 若任务属于一个只在磁盘上、尚未
 * 激活的 session, 就用 session_manager 的索引 (task_id → session_id, 由各
 * 未加载 session.json 解析一次而来) 定位属主, 再按需加载。 */
struct edc_task *edc_session_store_resolve_task(struct edc_session_manager *sm, struct edc_scheduler *sched,
                                                const char *task_id, struct edc_session **owner_out)
{
    if (owner_out)
        *owner_out = NULL;
    if (!sm || !task_id)
        return NULL;

    /* 1. 内存优先: 本 run 已激活的 session 里直接找。 */
    struct edc_session *owner = NULL;
    struct edc_task *t = edc_session_manager_find_task_any(sm, task_id, &owner);
    if (t)
    {
        if (owner_out)
            *owner_out = owner;
        return t;
    }

    /* 2. 未命中: 用 session_manager 的索引定位属主 session (解析各未加载
     *    session.json 一次), 命中则按需加载。 */
    struct edc_session *s = edc_session_manager_session_for_task(sm, task_id);
    if (!s)
        return NULL;
    if (!s->state_loaded)
    {
        /* Activate the owner session (also reconciles any cross-session deps it
         * reveals). Loading is side-effect-free for the active session. */
        edc_session_store_load_session(sm, sched, s);
    }

    t = edc_session_manager_find_task_any(sm, task_id, &owner);
    if (owner_out)
        *owner_out = owner;
    return t;
}

/* ── 未解析依赖对账 (S3) ───────────────────────────────────── */

/* Re-apply a persisted dep edge's flags once the edge is wired late. Mirrors the
 * logic in deser_deps for the local path. */
static void apply_dep_flags(struct edc_task *target, struct edc_task *source, bool completed,
                                      bool cancelled, bool injected)
{
    if (completed)
        edc_task_on_dep_completed(target, source);
    if (cancelled)
        edc_task_on_dep_cancelled(target, source);
    if (injected)
    {
        struct edc_task_dep *dep;
        LL_FOREACH(target->deps, dep)
        {
            if (dep->source == source)
            {
                dep->injected = true;
                break;
            }
        }
    }
}

static void free_pending_dep(struct edc_pending_dep *pd)
{
    sdsfree(pd->target_task_id);
    sdsfree(pd->source_task_id);
    free(pd);
}

bool edc_session_store_load_session(struct edc_session_manager *sm, struct edc_scheduler *sched,
                                    struct edc_session *session)
{
    if (!session)
        return false;
    bool ok = edc_session_load_state(session, sched);
    /* 内存即权威: 即使没有存档 (全新 session) 也标记为已加载, 之后不再重载。 */
    session->state_loaded = true;
    if (!sched || !sched->reconciling)
        edc_session_store_reconcile_pending_deps(sm, sched);
    return ok;
}

void edc_session_store_reconcile_pending_deps(struct edc_session_manager *sm, struct edc_scheduler *sched)
{
    if (!sm || !sched)
        return;
    if (sched->reconciling)
        return; /* re-entrant call from resolve_task: let the outer loop drain */
    sched->reconciling = true;

    /* Each pass resolves what it can; loading an owner session may reveal more
     * pending edges, so loop until a pass makes no progress. */
    bool progress = true;
    while (progress)
    {
        progress = false;
        struct edc_pending_dep **link = &sched->pending_deps;
        while (*link)
        {
            struct edc_pending_dep *pd = *link;

            struct edc_task *target = edc_session_manager_find_task_any(sm, pd->target_task_id, NULL);
            if (!target)
            {
                /* Dependent task is gone (e.g. its session was removed). */
                *link = pd->next;
                free_pending_dep(pd);
                progress = true;
                continue;
            }

            struct edc_task *source = edc_session_store_resolve_task(sm, sched, pd->source_task_id, NULL);
            if (source)
            {
                /* add_dependency returns false for an already-wired edge or a
                 * cycle; either way there is nothing more to do for this entry. */
                if (edc_task_add_dependency(target, source, pd->type))
                    apply_dep_flags(target, source, pd->completed, pd->cancelled, pd->injected);
                *link = pd->next;
                free_pending_dep(pd);
                progress = true;
                continue;
            }

            /* Unresolved. A completed/cancelled edge was already satisfied when
             * saved, so dropping it cannot cause a premature run. Anything else
             * must HOLD the target (resume gate) — never run with a missing dep. */
            if (pd->completed || pd->cancelled)
            {
                *link = pd->next;
                free_pending_dep(pd);
                progress = true;
                continue;
            }

            link = &pd->next;
        }
    }

    /* Whatever is left is genuinely unresolvable: surface it, and rely on the
     * resume gate to keep the target from running. */
    for (struct edc_pending_dep *pd = sched->pending_deps; pd; pd = pd->next)
        log_warn("cross-session dep unresolved: task `%s` waits on `%s` (owner session not found)", pd->target_task_id,
                 pd->source_task_id);

    sched->reconciling = false;
}

/* ── session 标签 → session 对象 (P3 读口) ─────────────────── */
struct edc_session *edc_session_store_resolve_session(struct edc_session_manager *sm, const char *session_id)
{
    if (!sm)
        return NULL;
    if (session_id && *session_id)
        return edc_session_manager_find(sm, session_id);
    return edc_session_manager_get_active(sm);
}

/* ── 会话生命周期 (信号 + 落盘) ────────────────────────────── */

/* 退出落盘辅助: 全量 history + **每个已加载** session 的 state (含后台 session)。
 * 前台组已随 session 保存 (session->foreground_group), 逐 session save_state
 * 不会互相污染。本 run 从未激活过的 session 会被跳过 —— 它们的内存对象只是
 * 由 sessions.json 拼出的 id/name 空壳, 写盘会覆盖磁盘上更新的状态 (含绑定
 * 的工作目录), 见 edc_session_manager_save_loaded_states。 */
void edc_session_store_save_all_sessions_state(struct edc_session_manager *sm, struct edc_scheduler *sched)
{
    if (!sm || !sched)
        return;
    edc_session_manager_save_all(sm);
    edc_session_manager_save_loaded_states(sm, sched);
}

/* 用户 /stop (/session stop): 取消当前 session 全部非终结 turns 后立即保存状态,
 * 使 CANCELLED 跨重启保留。
 * 注: 目标不是当前 active session 时 (如 /session stop <other>), 走
 * edc_scheduler_send_signal_to_session(sched, target, sig, true) —— 目标由调用方
 * 显式给 (P3 ③: scheduler 已经没有"当前 session"这个东西了)。 */
void edc_session_store_stop_all_turns(struct edc_session_manager *sm, struct edc_scheduler *sched)
{
    if (!sched)
        return;
    struct edc_session *active = edc_session_manager_get_active(sm);
    edc_scheduler_send_signal_to_all(sched, active, TASK_SIGTERM);
    if (sched->save_session)
        sched->save_session(sched, active);
}

void edc_session_store_remove_all_turns(struct edc_session_manager *sm, struct edc_scheduler *sched)
{
    if (!sched)
        return;
    edc_scheduler_send_signal_to_all(sched, edc_session_manager_get_active(sm), TASK_SIGTERM);
}
