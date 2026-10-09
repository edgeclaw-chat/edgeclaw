#include "edc_scheduler.h"
#include "edc_cmd_parse.h"
#include "edc_compat.h"
#include "edc_config.h"
#include "edc_context.h"
#include "edc_file_utils.h"
#include "edc_group_fold.h"
#include "edc_session.h"
#include "edc_session_manager.h"
#include "edc_session_store.h"
#include "edc_turn.h"
#include "log.h"
#include "uthash.h"
#include "utlist_fix.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* ── uthash wrapper: request_id → edc_task ──────────────────── */

struct edc_sched_task_entry
{
    void *request_id;
    struct edc_task *task;
    UT_hash_handle hh;
};

/* ── 前向声明 ──────────────────────────────────────────────── */

static void edc_scheduler_actor_handler(edc_actor_t *actor, edc_actor_msg_t *msg);
/* Sidebar-only task-tree snapshot, pulled by explicit session_id; the chat pane is the TUI's own. */
static void sched_handle_pull_snapshot(struct edc_scheduler *sched, const char *session_id);
static void sched_handle_task_snapshot(struct edc_scheduler *sched, struct edc_session *session, const char *reason);
static void sched_trace_notify(const char *reason, const char *session_id);
/** 终结态 (COMPLETED / CANCELLED / ERROR) —— 定义在下面的回收一节。 */
static bool sched_task_finished(struct edc_task *task);
static void sched_force_move_task(struct edc_scheduler *sched, struct edc_task *task, edc_task_step step,
                                  edc_task_life life);
/* executor 的出站 ops 表（定义在文件后半；这里前向声明，供 new 里 init 用）。 */
static const struct edc_executor_ops sched_executor_ops;

/* The task's owner session -- now edc_task_session() in edc_task.c. */

/* 前台组是 session 的属性 (terminal tab); scheduler 不持有副本。 */
static struct edc_task_group *sched_foreground(struct edc_session *session)
{
    return session ? session->foreground_group : NULL;
}

/* Inbound session label -> session object; NULL makes the caller reject the input. */
static struct edc_session *sched_resolve_session(struct edc_scheduler *sched, const char *session_id)
{
    if (!sched || !sched->sessions)
        return NULL;
    return edc_session_store_resolve_session(sched->sessions, session_id);
}

/* "Some tree changed": which sessions changed, deduped by pointer, flushed at message end. */
static void sched_mark_dirty(struct edc_scheduler *sched, struct edc_session *session, const char *reason)
{
    if (!sched || !session)
        return;
    for (size_t i = 0; i < sched->dirty_count; i++)
    {
        if (sched->dirty[i].session == session)
            return; /* 本条消息里已经标过它了 */
    }
    if (sched->dirty_count == sched->dirty_cap)
    {
        size_t cap = sched->dirty_cap ? sched->dirty_cap * 2 : 4;
        struct edc_sched_dirty *grown = realloc(sched->dirty, cap * sizeof(*grown));
        if (!grown)
            return;
        sched->dirty = grown;
        sched->dirty_cap = cap;
    }
    sched->dirty[sched->dirty_count].session = session;
    sched->dirty[sched->dirty_count].reason = reason;
    sched->dirty_count++;
}

/* Settle this message's consequences: notify each dirty session, then clear. */
static void sched_drain_dirty(struct edc_scheduler *sched)
{
    if (!sched || sched->dirty_count == 0)
        return;
    for (size_t i = 0; i < sched->dirty_count; i++)
    {
        struct edc_session *session = sched->dirty[i].session;
        const char *reason = sched->dirty[i].reason;
        const char *sid = (session && session->session_id) ? session->session_id : NULL;
        sched_trace_notify(reason, sid);
        if (sched->snapshot_changed && sid)
            sched->snapshot_changed(sched, sid, reason);
    }
    sched->dirty_count = 0;
}

/* next_task_id is a process-wide monotonic allocator; scanning live tasks cannot
 * recover it, so the high-water mark is persisted in a tiny file under the
 * workspace. A write failure degrades to memory-only monotonicity. */

static sds sched_task_seq_path(struct edc_scheduler *sched)
{
    if (!sched || !sched->config || !sched->config->workspace || sdslen(sched->config->workspace) == 0)
        return NULL;
    return edc_path_join(sched->config->workspace, ".task_seq", NULL);
}

static void sched_persist_task_seq(struct edc_scheduler *sched)
{
    sds path = sched_task_seq_path(sched);
    if (!path)
        return;

    sds tmp = sdscatprintf(sdsempty(), "%s.tmp", path);
    /* Plain stdio, not edc_file_*: scheduler tests do not set up the uv file loop. */
    FILE *f = fopen(tmp, "w");
    if (!f && errno == ENOENT && sched->config && sched->config->workspace)
    {
        /* The workspace dir may not exist yet; create it and retry once. */
        mkdir(sched->config->workspace, 0755);
        f = fopen(tmp, "w");
    }
    if (!f)
    {
        log_debug("task_seq: open %s for write failed: %s", tmp, strerror(errno));
    }
    else
    {
        if (fprintf(f, "%lld\n", (long long)sched->next_task_id) < 0)
            log_debug("task_seq: write %s failed: %s", tmp, strerror(errno));
        fclose(f);
        if (rename(tmp, path) != 0)
            log_warn("task_seq: rename %s -> %s failed: %s", tmp, path, strerror(errno));
    }

    sdsfree(tmp);
    sdsfree(path);
}

static void sched_load_task_seq(struct edc_scheduler *sched)
{
    sds path = sched_task_seq_path(sched);
    if (!path)
        return;

    FILE *f = fopen(path, "r");
    sdsfree(path);
    if (!f)
        return; /* 首次运行 / 文件缺失: 保留内存初值 */

    long long v = 0;
    if (fscanf(f, "%lld", &v) == 1 && v > sched->next_task_id)
        sched->next_task_id = v;
    fclose(f);
}

void edc_scheduler_observe_task_id(struct edc_scheduler *sched, const char *task_id)
{
    if (!sched || !task_id || !*task_id)
        return;

    /* 只认纯十进制编号; 旧存档/测试里的 "task-1" 之类按 0 处理, 不影响。 */
    char *end = NULL;
    long long id = strtoll(task_id, &end, 10);
    if (end == task_id || (end && *end != '\0'))
        return;

    if (id >= sched->next_task_id)
    {
        sched->next_task_id = id + 1;
        sched_persist_task_seq(sched);
    }
}

/* ── 生命周期 ──────────────────────────────────────────────── */

struct edc_scheduler *edc_scheduler_new(struct edc_config *config)
{
    struct edc_scheduler *sched = g_new0(struct edc_scheduler, 1);
    sched->config = config;
    sched->task_hash = NULL;
    sched->next_task_id = 1;

    /* 恢复上次运行的发号高水位, 保证重启后 task_id 不倒退、不重复。 */
    sched_load_task_seq(sched);

    edc_executor_init(&sched->executor, &sched_executor_ops, sched, config);

    sched->self = edc_actor_new("edc_scheduler", edc_scheduler_actor_handler, sched);

    return sched;
}

bool edc_scheduler_open(struct edc_scheduler *sched)
{
    if (!sched || sched->sessions)
        return false;

    struct edc_session_manager *sm = edc_session_manager_new(sched->config);
    if (!sm)
    {
        log_error("scheduler open: failed to create the session manager");
        return false;
    }
    sched->sessions = sm;
    sched->sessions_owned = true;
    sched->sessions_closed = false;

    /* Restore the saved task/group state of the active session. Safe here:
     * `sched` is fully constructed, and the load callbacks it triggers
     * (observe_task_id / add_pending_dep) target this same object. */
    struct edc_session *active = edc_session_manager_get_active(sm);
    if (active)
        edc_session_store_load_session(sm, sched, active);
    return true;
}

bool edc_scheduler_close(struct edc_scheduler *sched)
{
    if (!sched || !sched->sessions || sched->sessions_closed)
        return false;

    /* Exit = closing the process, not a user-driven /session remove: persist
     * everything as-is. An interrupted task is saved as LLM_PROCESSING /
     * TOOL_CALLING with its tool ledger intact and converges to SUSPENDED on
     * load, so the user can decide whether to resume -- hence no CANCELLED and no
     * second save after cancelling. */
    edc_session_store_save_all_sessions_state(sched->sessions, sched);

    /* In-flight turns belong to no live turn any more: drop them. */
    edc_session_store_remove_all_turns(sched->sessions, sched);

    sched->sessions_closed = true;
    return true;
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

    /* Groups and their tasks are owned by their session (freed in edc_session_free). */

    struct edc_pending_dep *pd, *pd_tmp;
    for (pd = sched->pending_deps; pd; pd = pd_tmp)
    {
        pd_tmp = pd->next;
        sdsfree(pd->target_task_id);
        sdsfree(pd->source_task_id);
        free(pd);
    }

    /* 脏表只持有 session 指针 —— session 归 session_manager 所有, 这里只丢数组。 */
    free(sched->dirty);
    sched->dirty = NULL;
    sched->dirty_count = 0;
    sched->dirty_cap = 0;

    /* The store is ours only when open() created it; a borrowed one (a test
     * fixture) belongs to its installer. Released last, after our own arrays are
     * gone, so no session pointer this object held outlives the sessions. */
    if (sched->sessions_owned && sched->sessions)
        edc_session_manager_free(sched->sessions);
    sched->sessions = NULL;
    sched->sessions_owned = false;

    if (sched->self)
        edc_actor_free(sched->self);
    free(sched);
}

/* ── 未解析的跨 session 依赖 ───────────────────────────────── */

void edc_scheduler_add_pending_dep(struct edc_scheduler *sched, const char *target_task_id,
                                   const char *source_task_id, edc_task_dep_type type, bool completed, bool cancelled,
                                   bool injected)
{
    if (!sched || !target_task_id || !source_task_id)
        return;
    struct edc_pending_dep *pd = g_new0(struct edc_pending_dep, 1);
    pd->target_task_id = sdsnew(target_task_id);
    pd->source_task_id = sdsnew(source_task_id);
    pd->type = type;
    pd->completed = completed;
    pd->cancelled = cancelled;
    pd->injected = injected;
    pd->next = NULL;

    /* Append at the tail: the reconcile cursor walks the list and may append while resolving. */
    if (!sched->pending_deps)
    {
        sched->pending_deps = pd;
        return;
    }
    struct edc_pending_dep *tail = sched->pending_deps;
    while (tail->next)
        tail = tail->next;
    tail->next = pd;
}

bool edc_scheduler_task_has_unresolved_deps(struct edc_scheduler *sched, struct edc_task *task)
{
    return edc_scheduler_first_unresolved_dep(sched, task) != NULL;
}

const char *edc_scheduler_first_unresolved_dep(struct edc_scheduler *sched, struct edc_task *task)
{
    if (!sched || !task || !task->task_id)
        return NULL;
    for (struct edc_pending_dep *pd = sched->pending_deps; pd; pd = pd->next)
    {
        if (pd->target_task_id && strcmp(pd->target_task_id, task->task_id) == 0)
            return pd->source_task_id;
    }
    return NULL;
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
    /* Persist on issue, so a reclaimed or killed task's id is never reused. */
    sched_persist_task_seq(sched);
    return strdup(buf);
}

struct edc_task *edc_scheduler_create_task(struct edc_scheduler *sched, struct edc_session *session, const char *desc,
                                           edc_channel_type channel_type)
{
    if (!sched || !session)
        return NULL;

    char *tid = edc_scheduler_gen_task_id(sched);
    /* Owned by the passed session; no session -> reject rather than guess. */
    const char *sid = session->session_id ? session->session_id : "sess-1";
    struct edc_task_group *fg = sched_foreground(session);
    const char *gid = fg ? fg->group_id : tid;

    struct edc_task *task = edc_task_new(tid, sid, gid, desc, channel_type);
    /* edc_task_new already leaves it READY; no forced WAITING_INPUT. */
    free(tid);

    /* Attach the raw input being handled (fork and system tasks do not). */
    if (sched->input_raw)
        task->raw_input = sdsnew(sched->input_raw);

    /* A new task changed the tree: mark dirty so it shows in the sidebar. */
    sched_mark_dirty(sched, session, "task");

    return task;
}

struct edc_task_group *edc_scheduler_create_group(struct edc_scheduler *sched, struct edc_session *session,
                                                  struct edc_task *leader)
{
    if (!sched || !session || !leader)
        return NULL;
    struct edc_task_group *group = edc_task_group_new(leader);
    edc_scheduler_add_group(sched, session, group);
    return group;
}

void edc_scheduler_add_group(struct edc_scheduler *sched, struct edc_session *session, struct edc_task_group *group)
{
    if (!sched || !group || !session)
        return;
    edc_session_add_group(session, group);
}

/* system task: real work (one fold LLM call), an ordinary persisted task, and
 * visible (rendered in its own sidebar block). The group is created with this task
 * as leader when none exists. */
struct edc_task *edc_scheduler_spawn_system_task(struct edc_scheduler *sched, struct edc_session *session,
                                                 const char *desc)
{
    if (!sched || !session)
        return NULL;

    struct edc_task_group *group = edc_session_find_system_group(session);

    char *tid = edc_scheduler_gen_task_id(sched);
    const char *gid = group ? group->group_id : tid;
    struct edc_task *task = edc_task_new(tid, session->session_id, gid, desc, edc_channel_adapter);
    free(tid);

    /* 两个属性: 产出交给 group 记忆; 容器是系统组。lifetime 走默认 (PERSISTENT) ——
     * 折叠任务必须活过重启, 好让没跑完的那一轮能被 resume (见 §4.6)。 */
    task->sink = EDC_SINK_GROUP_MEMORY;

    if (group)
    {
        edc_task_group_add_task(group, task);
    }
    else
    {
        group = edc_task_group_new(task);
        if (!group)
        {
            edc_task_free(task);
            return NULL;
        }
        group->kind = EDC_GROUP_KIND_SYSTEM;
        edc_session_add_group(session, group);
    }

    /* 侧栏立刻能看到它 (它可能就是当前唯一在跑的东西)。 */
    sched_mark_dirty(sched, session, "system-task");
    return task;
}

bool edc_scheduler_has_pending_system_task(struct edc_session *session)
{
    struct edc_task_group *group = edc_session_find_system_group(session);
    if (!group)
        return false;
    struct edc_task *t;
    LL_FOREACH2(group->members, t, group_next)
    {
        if (!sched_task_finished(t))
            return true;
    }
    return false;
}

bool edc_scheduler_attach_input(struct edc_scheduler *sched, struct edc_task *task, const char *input)
{
    (void)sched;
    if (!task || !input)
        return false;

    struct edc_turn *turn = edc_turn_new();
    turn->task_id = strdup(task->task_id);
    /* A bare user message (role/content only); decorations belong to the display path. */
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "role", "user");
    cJSON_AddStringToObject(msg, "content", input);
    edc_queue_push(&turn->messages, msg);
    task->active_turn = turn;
    return true;
}

/* Drive = hand a runnable task to the executor's START and pass the result to the
 * policy point. Request ids and registration live here; the next I/O is the
 * executor's. */
static void *sched_gen_request_id(void)
{
    /* Start high so generated ids never collide with small literals (pointer-keyed hash). */
    static int64_t seq = (int64_t)1 << 42;
    return (void *)(intptr_t)(++seq);
}

/* Runnable = has work and nobody is driving it; deps are read off step, not the edges.
 * A fold in flight for the task's group is a barrier: the group's work is held
 * until the fold lands (see edc_group_fold_holds_group). The fold task itself is
 * in the session's system group, so it is not held by its own fold. */
static bool sched_task_runnable(struct edc_scheduler *sched, struct edc_task *task)
{
    return task && edc_task_is_ready(task) && task->active_turn &&
           !edc_scheduler_task_has_unresolved_deps(sched, task) && !edc_group_fold_holds_group(task->group);
}

static void sched_drive_task(struct edc_scheduler *sched, struct edc_task *task);
static void sched_save_session(struct edc_scheduler *sched, struct edc_session *s);

/* 折叠触发的挂载点 (b): 一个任务**即将发请求**之前问一次"它所在的组该折了吗", 该折
 * 就先起折 —— `edc_group_fold_start` 给组内每个非终态成员挂一条 SEQUENCE dep (包括
 * 这个任务自己), 于是下面的谓词立刻把它停在 AWAIT_DEPS。屏障本身没有新机制 (B1):
 * 在飞的成员由 `tool_done` 在下一个请求边界 park, 折叠任务照旧由消息边界扫描派发。
 * 挂载点 (a) 在 `edc_scheduler_apply_advance` 尾部 (每次推进之后都查)。 */
static void sched_fold_before_request(struct edc_scheduler *sched, struct edc_task *task)
{
    if (!sched || !sched->sessions || !task || !task->group || !task->active_turn)
        return;
    /* 只有"即将发请求"的任务触发 (sink=GROUP_MEMORY 的折叠任务自己不折)。 */
    if (!edc_task_is_ready(task) || task->sink == EDC_SINK_GROUP_MEMORY)
        return;

    struct edc_session *session = task->group->session;
    if (!session || edc_scheduler_has_pending_system_task(session) || edc_group_fold_holds_group(task->group))
        return;
    if (!edc_group_fold_due(task->group, sched->config))
        return;
    if (edc_group_fold_start(sched, session, task->group))
        sched_save_session(sched, session); /* 折叠任务与它的 dep 边必须立刻可持久 */
}

/* 扫描的回调: 每个任务问一次"可跑吗"; 驱动成功 = 这一轮有进展。 */
struct sched_sweep_ctx
{
    struct edc_scheduler *sched;
    size_t driven;
};

static void sched_schedule_visit(void *ctx, struct edc_task *task)
{
    struct sched_sweep_ctx *sweep = ctx;
    /* 请求前先看折叠: 起折会让下面这个谓词为假 (新挂的 dep), 任务停在 AWAIT_DEPS。 */
    sched_fold_before_request(sweep->sched, task);
    if (!sched_task_runnable(sweep->sched, task))
        return;
    sched_drive_task(sweep->sched, task);
    sweep->driven++;
}

/* Scheduling: a full-table scan at the end of every inbound message -- there is no
 * tick, so the message boundary is the tick. It must NOT run after a single
 * advance: one message may contain several (a group cancel, a failure cascade),
 * and driving then would start a downstream task halfway through a cancel.
 * The predicate is the state, not the cause. Scan to a fixed point: a failed drive
 * broadcasts and may release SEQUENCE dependents earlier in the scan order. */
void edc_scheduler_schedule(struct edc_scheduler *sched)
{
    if (!sched || !sched->sessions)
        return;
    size_t driven;
    do
    {
        struct sched_sweep_ctx sweep = {.sched = sched, .driven = 0};
        edc_session_manager_foreach_task(sched->sessions, sched_schedule_visit, &sweep);
        driven = sweep.driven;
    } while (driven > 0);
}

/* Hand START to the executor, then pass the facts to the policy point. If nothing
 * was started, take the just-issued request_id back. */
static void sched_drive_task(struct edc_scheduler *sched, struct edc_task *task)
{
    void *req_id = sched_gen_request_id();
    edc_scheduler_register_task(sched, task, req_id);
    task->current_request_id = req_id;

    struct edc_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.kind = EDC_EVENT_START;
    ev.request_id = (const char *)req_id;

    struct edc_advance_result res = edc_executor_advance(&sched->executor, task, &ev);

    if (!res.settled && !edc_task_is_running(task))
    {
        edc_scheduler_unregister_task(sched, req_id);
        task->current_request_id = NULL;
    }
    edc_scheduler_apply_advance(sched, task, req_id, &res);
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

struct edc_task *edc_scheduler_find_task_by_id(struct edc_scheduler *sched, struct edc_session *session,
                                               const char *task_id)
{
    if (!sched || !task_id || !session)
        return NULL;
    struct edc_task_group *group;
    DL_FOREACH(session->groups, group)
    {
        struct edc_task *task = edc_task_group_find_task(group, task_id);
        if (task)
            return task;
    }
    return NULL;
}

/* ── 前台/后台 ──────────────────────────────────────────────── */

void edc_scheduler_set_foreground(struct edc_session *session, struct edc_task_group *group)
{
    /* The foreground group is a session property (terminal tab). */
    struct edc_session *owner = group ? group->session : session;
    if (owner)
        owner->foreground_group = group;
}

struct edc_task_group *edc_scheduler_get_foreground(struct edc_session *session)
{
    return sched_foreground(session);
}

const char *edc_scheduler_task_workdir(struct edc_task *task)
{
    /* Runs in the task's own bound directory; NULL when it has no owner. */
    struct edc_session *own = (task && task->group) ? task->group->session : NULL;
    return own ? edc_session_get_workdir(own) : NULL;
}

bool edc_scheduler_has_active_tasks(struct edc_scheduler *sched, struct edc_session *session)
{
    if (!sched || !session)
        return false;
    struct edc_task_group *g;
    DL_FOREACH(session->groups, g)
    {
        struct edc_task *t;
        LL_FOREACH2(g->members, t, group_next)
        {
            /* "Any task left running" = still alive. */
            if (edc_task_is_alive(t))
                return true;
        }
    }
    return false;
}

/* ── 注册/注销 ──────────────────────────────────────────────── */

void edc_scheduler_register_task(struct edc_scheduler *sched, struct edc_task *task, void *request_id)
{
    /* Registering a new request clears `cancelled_request`, or a resumed task drops
     * its own new response. Late responses of the old request are dropped by id. */
    if (task)
        task->cancelled_request = 0;

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

void edc_scheduler_send_signal_to_task_ptr(struct edc_scheduler *sched, struct edc_task *task, edc_task_signal sig)
{
    if (!sched || !task)
        return;

    /* The executor applies the signal; here we only build the event. */
    struct edc_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.kind = EDC_EVENT_SIGNAL;
    ev.as.signal.sig = sig;
    /* A signal carries no request_id: unregister the one the task remembers. */
    struct edc_advance_result res = edc_executor_advance(&sched->executor, task, &ev);
    edc_scheduler_apply_advance(sched, task, task->current_request_id, &res);
}

void edc_scheduler_send_signal_to_task(struct edc_scheduler *sched, struct edc_session *session, const char *task_id,
                                       edc_task_signal sig)
{
    struct edc_task *task = edc_scheduler_find_task_by_id(sched, session, task_id);
    if (!task)
        return;
    edc_scheduler_send_signal_to_task_ptr(sched, task, sig);
}

/* Signal a task: the task layer moves the phase, then the host's unified entry does
 * the bookkeeping. Serves host-driven fan-out (group/session cancel). */
static void sched_signal_one(struct edc_scheduler *sched, struct edc_task *task, edc_task_signal sig)
{
    edc_task_step step_before = task->step;
    edc_task_life life_before = task->life;

    edc_task_send_signal(task, sig);

    /* Phase unchanged = a duplicate cancel: nothing settled. */
    if (task->step == step_before && task->life == life_before)
        return;

    /* Cancel = explicit termination: unregister, reap, dirty, settle, all in one entry. */
    struct edc_advance_result res;
    memset(&res, 0, sizeof(res));
    res.outcome = EDC_STEP_DONE;
    res.changed = true;
    res.settled = true;
    edc_scheduler_apply_advance(sched, task, task->current_request_id, &res);
}

/* Signal a group: walk each member through the single-task path above. */
static void sched_signal_group_members(struct edc_scheduler *sched, struct edc_task_group *group, edc_task_signal sig,
                                       bool running_only)
{
    if (!sched || !group)
        return;
    struct edc_task *task, *next;
    LL_FOREACH_SAFE2(group->members, task, next, group_next)
    {
        if (running_only && !edc_task_is_running(task))
            continue;
        sched_signal_one(sched, task, sig);
    }
    /* 组自己的收尾 (leader 被取消 → 组标记孤儿) 归组模块。 */
    edc_task_group_finish_signal(group);
}

void edc_scheduler_send_signal_to_group(struct edc_scheduler *sched, struct edc_session *session, const char *group_id,
                                        edc_task_signal sig)
{
    if (!sched || !session)
        return;
    struct edc_task_group *group = NULL;
    DL_FOREACH(session->groups, group)
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

void edc_scheduler_send_signal_to_all(struct edc_scheduler *sched, struct edc_session *session, edc_task_signal sig)
{
    edc_scheduler_send_signal_to_session(sched, session, sig, false);
}

/* ── resume ────────────────────────────────────────────────── */

void edc_scheduler_resume_task(struct edc_scheduler *sched, struct edc_task *task)
{
    if (!sched || !task)
        return;

    /* Gate: only between-tasks checks stay here. An unresolved cross-session dep
     * holds the task back; unsatisfied deps send it back to waiting. */
    if (edc_scheduler_task_has_unresolved_deps(sched, task))
        return;

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
            /* Host-initiated move: same bookkeeping through the unified entry. */
            edc_task_mark_waiting_deps(task);
            edc_scheduler_task_moved(sched, task);
            return;
        }
    }

    /* 请求前先看折叠 (挂载点 (b)): 起折给本任务挂上 dep, 它停在 AWAIT_DEPS,
     * 折叠落地后由扫描重新驱动 —— 那次请求就会带上压过的上下文。 */
    sched_fold_before_request(sched, task);

    /* A fold in flight for this group parks the task instead of driving it: leave
     * it READY/ALIVE and let the message-end scan start it once the fold lands. */
    if (edc_group_fold_holds_group(task->group))
    {
        edc_task_mark_ready(task);
        edc_scheduler_task_moved(sched, task);
        return;
    }

    /* Drive: the within-task re-decision is the executor's START. */
    sched_drive_task(sched, task);
}

static bool sched_task_finished(struct edc_task *task)
{
    /* 终态 = 不再推进。SUSPENDED 不算 (它还会被 resume)。 */
    return edc_task_is_completed(task) || edc_task_is_cancelled(task) || edc_task_has_failed(task);
}

/* ── 前台任务状态通知 ─────────────────────────────────────── */

/* A phase change only marks dirty; the push is merged into one per message by the
 * actor handler, since these transitions produce no channel message but the TUI is
 * snapshot-driven. Writes always state the target (step, life). */

/* Force a phase, skipping the transition table: a fallback exit, landing at READY. */
static void sched_force_move_task(struct edc_scheduler *sched, struct edc_task *task, edc_task_step step,
                                  edc_task_life life)
{
    edc_task_force_move(task, step, life);
    if (sched)
        sched_mark_dirty(sched, edc_task_session(task), "state");
}

/* ── 内部：消息路由 ────────────────────────────────────────── */

static void sched_handle_input(struct edc_scheduler *sched, struct edc_actor_msg *msg)
{
    const struct edc_sched_input *input = msg->data;
    if (!input)
        return;

    /* The session comes from the message itself; unresolvable -> reject. */
    struct edc_session *session = sched_resolve_session(sched, input->session_id);
    if (!session)
    {
        log_warn("input dropped: no session for tag '%s'", input->session_id ? input->session_id : "(none)");
        return;
    }

    struct edc_task_group *fg = sched_foreground(session);
    struct edc_task *task = NULL;

    /* Record the raw text being handled (cleared on every exit of this function). */
    sched->input_raw = input->content;

    /* Slash commands and @group go to the command handler; anything else creates a task. */
    const bool is_plain = input->content && input->content[0] != '/' && input->content[0] != '@';

    /* The command handler takes core's inbound input (origin for the raw message). */
    if (sched->handle_command && sched->handle_command(sched, msg->from, input))
    {
        sched->input_raw = NULL;
        return;
    }

    /* 无命令处理器 (或未处理) → 兜底新建 task (本条输入就是它的 turn)。 */
    task = edc_scheduler_create_task(sched, session, input->content, input->type);
    if (!task)
        return;
    edc_session_set_active_task(session, task);

    if (!fg || fg->is_orphaned || fg->member_count == 0)
    {
        fg = edc_scheduler_create_group(sched, session, task);
        edc_scheduler_set_foreground(session, fg);
    }
    else
    {
        edc_task_group_add_task(fg, task);
    }

    /* Plain input gets the same python-like quoting stripped as the shell path. */
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
    /* Attaching the input is all that happens here; running it is the scan's job. */

    sched->input_raw = NULL; /* 这条输入处理完了 */
}

/* 输出重定向落盘 —— 已归位 edc_task.c 的 edc_task_flush_redirects()。 */

/* The executor's outbound ops. Each checks the callback is installed first. */

static bool ex_send_llm(void *ctx, struct edc_task *task, void *request_id)
{
    struct edc_scheduler *sched = ctx;
    /* An uninstalled port is the fact "cannot make progress" -- never swallowed. */
    if (!sched->send_llm)
        return false;
    sched->send_llm(sched, task, request_id);
    return true;
}

static void ex_dispatch_tools(void *ctx, struct edc_task *task, void *llm_resp, void *request_id)
{
    struct edc_scheduler *sched = ctx;
    if (sched->dispatch_tools)
        sched->dispatch_tools(sched, task, llm_resp, request_id);
}

static int ex_redispatch_tools(void *ctx, struct edc_task *task, void *request_id)
{
    struct edc_scheduler *sched = ctx;
    if (sched->redispatch_tools)
        return sched->redispatch_tools(sched, task, request_id);
    return 0;
}

static const struct edc_executor_ops sched_executor_ops = {
    .send_llm = ex_send_llm,
    .dispatch_tools = ex_dispatch_tools,
    .redispatch_tools = ex_redispatch_tools,
};

/* ── task 之间的边（依赖 DAG）──────────────────────────────── */

/* A queued dependent is started by the message-end scan, not from here. */

/* ── 跨容器的收尾 ──────────────────────────────────────────── */

/* 同包直调 store: 落盘是 store 的事, 这里只把"要存谁"递过去 (NULL = 活跃的)。 */
static void sched_save_session(struct edc_scheduler *sched, struct edc_session *s)
{
    if (!sched || !sched->sessions)
        return;
    edc_session_store_save_session_state(sched->sessions, sched, s);
}

/* The "no API key" text points the user at /connect: presentation, not advance. */
static sds sched_error_copy(struct edc_scheduler *sched, const char *notice)
{
    const char *err = notice ? notice : "LLM request failed";
    const sds key = (sched && sched->config) ? sched->config->llm_api_conf.api_key : NULL;
    if (!key || sdslen(key) == 0)
    {
        /* Without an API key the request can only fail, so point at /connect. */
        return sdscatprintf(sdsempty(),
                            "%s\n\nNo API key configured — run /connect to set the API URL, model and key.", err);
    }
    return sdsnew(err);
}

/* Settle callback for side-effect-moved tasks: their facts come from the task's
 * current state. Recursion is intentional (settling can move more tasks). */
static void sched_settle_cascaded(void *ctx, struct edc_task *task)
{
    struct edc_scheduler *sched = ctx;
    bool finished = sched_task_finished(task);

    struct edc_advance_result res;
    memset(&res, 0, sizeof(res));
    res.outcome = finished ? EDC_STEP_DONE : EDC_STEP_BLOCKED;
    res.changed = true;
    res.settled = finished;
    /* A cascaded task carries its own reason (do not fall back to a generic message). */
    res.notice = task->output;
    edc_scheduler_apply_advance(sched, task, task->current_request_id, &res);
}

/* Bookkeeping + settle for one task (steps 1-3). The public entry adds step 4. */
static void sched_apply_advance_one(struct edc_scheduler *sched, struct edc_task *task, void *req_id,
                                    const struct edc_advance_result *res)
{
    if (!sched || !task || !res)
        return;
    struct edc_session *own = edc_task_session(task);

    /* ① 索引: 进终态 = 不再欠任何请求 —— task_hash 里那一格该收了。 */
    if (res->settled && req_id)
        edc_scheduler_unregister_task(sched, req_id);

    /* (2) Phase changed: mark the tree dirty. */
    if (res->changed)
    {
        sched_mark_dirty(sched, own, "state");
    }

    if (!res->settled)
        return;

    /* ③ 结算: 进终态 → 六件"告诉外界"的事。**顺序与搬迁前逐字相同**。 */
    if (edc_task_is_completed(task))
    {
        /* Completion seq within the owning container; a GROUP_MEMORY task takes none. */
        if (task->sink != EDC_SINK_GROUP_MEMORY && own)
            edc_task_set_completion_seq(task, own->next_completion_seq++);
        else
            edc_task_set_completion_seq(task, 0);

        /* The container grew: notify once, or a silent turn commit would stall the count. */
        sched_mark_dirty(sched, own, "complete");

        /* Notify dependents; starting the queued is the scan's job. */
        edc_task_notify_dependents(task);

        /* Delivery by sink: CHANNEL replies, GROUP_MEMORY writes to the group. */
        if (task->sink == EDC_SINK_GROUP_MEMORY)
            edc_group_fold_deliver_memory(task, res->notice);
        else if (sched->send_reply)
            sched->send_reply(sched, task, res->notice, EDC_CHANNEL_MESSAGE_RESP_TYPE, req_id);

        /* The settlement subscribers: persistence. The fold check itself lives at the
         * tail of edc_scheduler_apply_advance, so every advance asks once (see (a)). */
        sched_save_session(sched, own);
    }
    else if (edc_task_is_cancelled(task))
    {
        /* An explicit cancel is persisted immediately, or a restart restores it as running. */
        sched_mark_dirty(sched, own, "foreground");
        sched_save_session(sched, own);
    }
    else /* ERROR */
    {
        sched_mark_dirty(sched, own, "state");
        if (sched->send_reply)
        {
            sds copy = sched_error_copy(sched, res->notice);
            sched->send_reply(sched, task, copy, EDC_CHANNEL_MESSAGE_SYS_TYPE, req_id);
            sdsfree(copy);
        }
    }
}

/* edc_scheduler_apply_advance -- the one policy point after an advance.
 *
 * The executor reports facts; this turns "what happened" into "whom to tell":
 * index / reap / dirty / dependents / delivery / fold / persistence. Step 4 settles
 * the tasks an operation moved as a side effect. */
void edc_scheduler_apply_advance(struct edc_scheduler *sched, struct edc_task *task, void *req_id,
                                 const struct edc_advance_result *res)
{
    if (!sched || !task || !res)
        return;

    sched_apply_advance_one(sched, task, req_id, res);

    /* ④ 清账: 递归 —— 清账本身还会连带改动别的 task。 */
    edc_task_drain_moves(task, sched_settle_cascaded, sched);

    /* ⑤ 折叠检查 (挂载点 (a)): 每次推进之后都问一次, 不只在"任务完成"那一支 ——
     * 一次工具应答推进也能把组的上下文顶过窗口, 而那时任务还没完成。起折给组内每个
     * 非终态成员挂 SEQUENCE dep, 在飞的成员到下一个请求边界 park (B1, 零新机制)。
     * 起折的 session 可能不是本任务那个 (后台会话也折), 所以用 try 回传的那个。 */
    if (sched->sessions)
    {
        struct edc_session *folded = NULL;
        if (edc_group_fold_try(sched, sched->sessions, sched->config, &folded) && folded)
            sched_save_session(sched, folded);
    }
}

/* Host-initiated transitions go through the same bookkeeping. */
void edc_scheduler_task_moved(struct edc_scheduler *sched, struct edc_task *task)
{
    struct edc_advance_result res;
    memset(&res, 0, sizeof(res));
    res.outcome = EDC_STEP_BLOCKED;
    res.changed = true;
    edc_scheduler_apply_advance(sched, task, NULL, &res);
}

/* The scheduler only resolves the task by request_id and drops late cancelled events. */

static void sched_handle_llm_resp(struct edc_scheduler *sched, struct edc_actor_msg *msg)
{
    struct edc_task *task = edc_scheduler_find_task(sched, msg->request_id);
    if (!task || task->cancelled_request)
        return;
    struct edc_advance_result res = edc_executor_advance_llm_resp(&sched->executor, task, msg->request_id, msg->data);
    edc_scheduler_apply_advance(sched, task, msg->request_id, &res);
}

static void sched_handle_tool_resp(struct edc_scheduler *sched, struct edc_actor_msg *msg)
{
    struct edc_task *task = edc_scheduler_find_task(sched, msg->request_id);
    if (!task || task->cancelled_request)
        return;
    struct edc_advance_result res = edc_executor_advance_tool_resp(&sched->executor, task, msg->request_id, msg->data);
    edc_scheduler_apply_advance(sched, task, msg->request_id, &res);

    /* A held send (a dep is outstanding) parked the task at AWAIT_DEPS without
     * sending: its request is over, so drop the mapping. It is re-driven with a
     * fresh id when the dep completes. */
    if (!res.settled && !edc_task_is_running(task) && task->current_request_id == msg->request_id)
    {
        edc_scheduler_unregister_task(sched, msg->request_id);
        task->current_request_id = NULL;
    }
}

static void sched_handle_tool_started(struct edc_scheduler *sched, struct edc_actor_msg *msg)
{
    struct edc_task *task = edc_scheduler_find_task(sched, msg->request_id);
    if (!task || task->cancelled_request)
        return;

    struct edc_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.kind = EDC_EVENT_TOOL_STARTED;
    ev.request_id = msg->request_id;
    ev.as.call_id = (const char *)msg->data;
    struct edc_advance_result res = edc_executor_advance(&sched->executor, task, &ev);
    edc_scheduler_apply_advance(sched, task, msg->request_id, &res);
}

/* Tool output echo (TOOL message): check the request is still alive via the task
 * hash (a late async callback's pointer is untrusted), require an owner session,
 * and refresh the todo dirty table. The display gate is in the frontend. */
static void sched_handle_tool_emit(struct edc_scheduler *sched, struct edc_actor_msg *msg)
{
    struct edc_tool_emit_view v;
    if (!edc_executor_view_tool_emit(msg->data, &v) || !v.content)
        return;

    struct edc_task *task = edc_scheduler_find_task(sched, msg->request_id);
    if (!task || task->cancelled_request)
        return;

    /* 没有属主就没有可投递的地方 (P3 ③: 不再回退到"当前绑定")。 */
    struct edc_session *own = edc_task_session(task);
    if (!own)
        return;
    /* The display gate is in the frontend; core only states the fact. */

    if (sched->send_tool_message)
        sched->send_tool_message(sched, task, v.header, v.content, v.render_pref, msg->request_id);

    /* todowrite changes no task state, so push a snapshot to refresh the TodoWidget. */
    if (v.header && strcmp(v.header, "todowrite") == 0)
        sched_mark_dirty(sched, own, "todo");
}

/* ── Actor 入口 ────────────────────────────────────────────── */

/* Serialize the current session's task tree as JSON for the TUI sidebar (on the
 * scheduler thread, so race-free). EDGECLAW_SNAPSHOT_TRACE=<path> logs each push. */
static void sched_trace_snapshot(const char *reason, sds json)
{
    const char *path = getenv("EDGECLAW_SNAPSHOT_TRACE");
    if (!path || !*path)
        return;
    FILE *f = fopen(path, "a");
    if (!f)
        return;
    fprintf(f, "pull reason=%s %s\n", reason ? reason : "?", json ? json : "");
    fclose(f);
}

/* The "event" half of the same trace: who changed, and why. */
static void sched_trace_notify(const char *reason, const char *session_id)
{
    const char *path = getenv("EDGECLAW_SNAPSHOT_TRACE");
    if (!path || !*path)
        return;
    FILE *f = fopen(path, "a");
    if (!f)
        return;
    fprintf(f, "notify reason=%s session=%s\n", reason ? reason : "?", session_id ? session_id : "?");
    fclose(f);
}


/* 前端点名要一棵树。解析不出来就不给 (前端拿到的是"没人应答", 不是别人的树)。 */
static void sched_handle_pull_snapshot(struct edc_scheduler *sched, const char *session_id)
{
    struct edc_session *session = sched_resolve_session(sched, session_id);
    if (!session)
    {
        log_warn("snapshot request dropped: no session for tag '%s'", session_id ? session_id : "(none)");
        return;
    }
    sched_handle_task_snapshot(sched, session, "pull");
}

static void sched_handle_task_snapshot(struct edc_scheduler *sched, struct edc_session *session, const char *reason)
{
    if (!sched || !session)
        return;

    cJSON *root = cJSON_CreateObject();
    struct edc_session *sess = session;
    cJSON_AddStringToObject(root, "session_id", sess->session_id ? sess->session_id : "");
    cJSON_AddStringToObject(root, "session_name", edc_session_get_name(sess));
    /* session messages + every task's in-flight turn messages. */
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
    /* The session's bound working directory ("" when unbound). The frontend shows
     * it in the footer and must not reach into the store for it. */
    const char *sess_wd = edc_session_get_workdir(sess);
    cJSON_AddStringToObject(root, "workdir", sess_wd ? sess_wd : "");

    cJSON *groups = cJSON_AddArrayToObject(root, "groups");
    struct edc_task_group *g;
    DL_FOREACH(session->groups, g)
    {
        cJSON *gj = cJSON_CreateObject();
        cJSON_AddStringToObject(gj, "id", g->group_id ? g->group_id : "");
        /* A pure projection: the scheduler neither reads nor filters on it. */
        cJSON_AddStringToObject(gj, "kind", edc_task_group_kind_name(g->kind));
        cJSON_AddNumberToObject(gj, "member_count", g->member_count);
        cJSON_AddBoolToObject(gj, "orphaned", g->is_orphaned);
        cJSON_AddBoolToObject(gj, "fg", sched_foreground(session) == g);
        /* Committed history (minus history[0]) + its tasks' in-flight turns. */
        size_t g_msg_count = g->history ? (size_t)cJSON_GetArraySize(g->history) : 0;
        if (g_msg_count > 0)
            g_msg_count -= 1;
        long long g_tokens_total = 0;
        long long g_tokens_prompt = 0;
        long long g_tokens_completion = 0;
        long long g_tokens_cached = 0;
        {
            struct edc_task *mt;
            LL_FOREACH2(g->members, mt, group_next)
            {
                if (mt->active_turn)
                    g_msg_count += edc_queue_length(&mt->active_turn->messages);
                g_tokens_total += mt->tokens_total;
                g_tokens_prompt += mt->tokens_prompt;
                g_tokens_completion += mt->tokens_completion;
                g_tokens_cached += mt->tokens_cached;
            }
        }
        /* The group's *current* context: the widest member conversation, as the
         * last reply's real usage plus an estimate of what was appended after it
         * (or a from-scratch estimate while no reply is trustworthy). Estimated
         * travels with the number -- the sidebar marks it, it never passes for a
         * provider measurement. */
        bool g_ctx_estimated = false;
        long long g_ctx_tokens = edc_context_group_ctx_tokens(g, &g_ctx_estimated);
        cJSON_AddNumberToObject(gj, "msg_count", (double)g_msg_count);
        cJSON_AddNumberToObject(gj, "tokens_total", (double)g_tokens_total);
        cJSON_AddNumberToObject(gj, "tokens_prompt", (double)g_tokens_prompt);
        cJSON_AddNumberToObject(gj, "tokens_completion", (double)g_tokens_completion);
        cJSON_AddNumberToObject(gj, "tokens_cached", (double)g_tokens_cached);
        cJSON_AddNumberToObject(gj, "ctx_tokens", (double)g_ctx_tokens);
        cJSON_AddBoolToObject(gj, "ctx_estimated", g_ctx_estimated);

        cJSON *tasks = cJSON_AddArrayToObject(gj, "tasks");
        struct edc_task *t;
        LL_FOREACH2(g->members, t, group_next)
        {
            cJSON *tj = cJSON_CreateObject();
            cJSON_AddStringToObject(tj, "id", t->task_id ? t->task_id : "");
            cJSON_AddStringToObject(tj, "state", edc_task_state_str(t));
            cJSON_AddBoolToObject(tj, "leader", t->is_group_leader);
            cJSON_AddBoolToObject(tj, "active", sess->active_task == t);
            cJSON_AddStringToObject(tj, "desc", t->description ? t->description : "");
            /* The raw input that created the task ("" when none). */
            cJSON_AddStringToObject(tj, "input", t->raw_input ? t->raw_input : "");
            /* Token usage for the sidebar (double, exact below 2^53). */
            cJSON_AddNumberToObject(tj, "tokens_prompt", (double)t->tokens_prompt);
            cJSON_AddNumberToObject(tj, "tokens_completion", (double)t->tokens_completion);
            cJSON_AddNumberToObject(tj, "tokens_total", (double)t->tokens_total);
            cJSON_AddNumberToObject(tj, "tokens_cached", (double)t->tokens_cached);
            /* Todos travel with the snapshot; the sidebar reads them from it alone. */
            if (t->todos && cJSON_GetArraySize(t->todos) > 0)
                cJSON_AddItemToObject(tj, "todos", cJSON_Duplicate(t->todos, 1));
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

    sched_trace_snapshot(reason, out);

    /* 应答只发给**点名要它**的前端 (拉取路径), 不再有主动推送。 */
    if (sched->snapshot_reply)
        sched->snapshot_reply(sched, out);
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
    case BC_ACTOR_MSG_TASK_SNAPSHOT:
        /* A pull names which tree it wants; nothing pushes a tree unasked. */
        sched_handle_pull_snapshot(sched, (const char *)msg->data);
        break;
    default:
        break;
    }

    /* One push per message at most, before msg is freed. */
    /* Every effect has landed; drive by state (a pure pull reaches here too). */
    edc_scheduler_schedule(sched);

    sched_drain_dirty(sched);

    edc_actor_msg_free(msg);
}

/* Message history of the selected task/group (source task->turns), or NULL. */

static void history_append_messages(const struct edc_task *task, cJSON *messages)
{
    for (size_t i = task->turns.head; i < task->turns.tail; i++)
    {
        const struct edc_turn *turn = (const struct edc_turn *)task->turns.data[i];
        for (size_t j = turn->messages.head; j < turn->messages.tail; j++)
            cJSON_AddItemToArray(messages, cJSON_Duplicate((cJSON *)turn->messages.data[j], 1));
    }
}

/* 组里一个成员那一块: {"id", "input", "messages"}。 */
static cJSON *history_task_block(const struct edc_task *task)
{
    cJSON *block = cJSON_CreateObject();
    cJSON_AddStringToObject(block, "id", task->task_id ? task->task_id : "");
    /* The raw input that created this task: what the user actually typed. */
    cJSON_AddStringToObject(block, "input", task->raw_input ? task->raw_input : "");
    history_append_messages(task, cJSON_AddArrayToObject(block, "messages"));
    return block;
}

char *edc_scheduler_task_history_json(struct edc_scheduler *sched, struct edc_session *session,
                                      const char *group_id, const char *task_id)
{
    if (!sched || !session || (!group_id && !task_id))
        return NULL;

    struct edc_task_group *g = NULL;
    if (group_id && *group_id)
    {
        DL_FOREACH(session->groups, g)
        {
            if (!g->group_id || strcmp(g->group_id, group_id) != 0)
                continue;
            cJSON *root = cJSON_CreateObject();
            cJSON_AddStringToObject(root, "kind", "group");
            cJSON_AddStringToObject(root, "id", group_id);
            cJSON *blocks = cJSON_AddArrayToObject(root, "tasks");
            struct edc_task *t = NULL;
            LL_FOREACH2(g->members, t, group_next)
                cJSON_AddItemToArray(blocks, history_task_block(t));
            char *out = cJSON_PrintUnformatted(root);
            cJSON_Delete(root);
            return out;
        }
        return NULL;
    }

    /* 单个 task: 用既有的权威查找 (它认识组内成员, 也认识跨 session 的懒加载)。 */
    struct edc_task *t = edc_scheduler_find_task_by_id(sched, session, task_id);
    if (!t)
        return NULL;
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "kind", "task");
    cJSON_AddStringToObject(root, "id", t->task_id ? t->task_id : "");
    cJSON_AddStringToObject(root, "input", t->raw_input ? t->raw_input : "");
    history_append_messages(t, cJSON_AddArrayToObject(root, "messages"));
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
}
