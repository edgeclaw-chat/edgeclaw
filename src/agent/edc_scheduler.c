#include "edc_scheduler.h"
#include "edc_channel_actor.h"
#include "edc_cmd_parse.h"
#include "edc_compat.h"
#include "edc_config.h"
#include "edc_context.h"
#include "edc_file_utils.h"
#include "edc_session.h"
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
/* 任务树快照: 只服务侧边栏 (任务树 + todo + 计数)。对话内容不再随快照下发
 * —— 聊天区由 TUI 自己持久化/回放显示流 (见 edc_tui_window.cpp)。 */
static void sched_handle_task_snapshot(struct edc_scheduler *sched, const char *reason);
static void sched_move_task(struct edc_scheduler *sched, struct edc_task *task, edc_task_step step, edc_task_life life);
static void sched_force_move_task(struct edc_scheduler *sched, struct edc_task *task, edc_task_step step,
                                  edc_task_life life);
/* executor 的出站 ops 表（定义在文件后半；这里前向声明，供 new 里 init 用）。 */
static const struct edc_executor_ops sched_executor_ops;

/* 任务的属主 session: 被切到后台的 session 里的任务完成/取消时, turn、快照与
 * 落盘都要写回**它自己**, 而不是当前绑定的 sched->session (否则串台)。group
 * 没有属主时 (极端/测试) 回退到当前 session。 */
static struct edc_session *sched_task_session(struct edc_scheduler *sched, struct edc_task *task)
{
    if (task && task->group && task->group->session)
        return task->group->session;
    return sched ? sched->session : NULL;
}

/* ── Task 序号持久化 ─────────────────────────────────────────
 * next_task_id 是**进程级**单调递增的发号器 (跨 session 共用): 一台机器上
 * 任何时刻都不该再发出已经用过的 task_id。但它的值不能只靠"扫存活任务取最
 * 大值"来恢复 —— 任务被 /task join 回收、或发号发生在当时未激活的其它
 * session 里时, 这些编号在重启后都无从反推, 计数器会退回起点而重复发号。
 * 故把发号高水位单独落盘到 workspace 下一个极小文件, 每次发号即续写; 加载
 * 时取"文件值"与"存活任务最大值 + 1"的较大者 (后者兼容改动前的旧存档)。
 * 写失败 (只读 fs / 目录未建) 不致命: 退化为改动前的行为, 仍是内存内单调。 */

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
    /* 纯 stdio, 不依赖 uv file loop: scheduler 单测不初始化 file utils, 用
     * edc_file_* 会触发空 loop 解引用; 计数器文件极小, stdio 足够。 */
    FILE *f = fopen(tmp, "w");
    if (!f && errno == ENOENT && sched->config && sched->config->workspace)
    {
        /* 全新环境里 workspace 目录可能还没建立 (首个 session 落盘时才会建),
         * 补建后重试一次, 否则首批 task_id 会静默丢掉持久化。 */
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

struct edc_scheduler *edc_scheduler_new(struct edc_session *session, struct edc_config *config)
{
    struct edc_scheduler *sched = g_new0(struct edc_scheduler, 1);
    sched->session = session;
    sched->config = config;
    sched->task_hash = NULL;
    sched->next_task_id = 1;

    /* 恢复上次运行的发号高水位, 保证重启后 task_id 不倒退、不重复。 */
    sched_load_task_seq(sched);

    edc_executor_init(&sched->executor, &sched_executor_ops, sched);

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

    /* Groups (and their tasks) are owned by their session and freed in
     * edc_session_free — the scheduler must not free them here, or sessions
     * other than the currently bound one would leak. */

    struct edc_pending_dep *pd, *pd_tmp;
    for (pd = sched->pending_deps; pd; pd = pd_tmp)
    {
        pd_tmp = pd->next;
        sdsfree(pd->target_task_id);
        sdsfree(pd->source_task_id);
        free(pd);
    }

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

    /* Append at the tail: the reconcile loop walks the list with a `*link`
     * cursor, and resolving an entry may append new entries (loaded sessions
     * reveal more deps). Prepending would move the head under that cursor and
     * corrupt the list. */
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
    /* 发号即落盘高水位: 即便该编号所在任务随后被回收、进程被 kill, 重启后也
     * 不会再把它发出去。 */
    sched_persist_task_seq(sched);
    return strdup(buf);
}

struct edc_task *edc_scheduler_create_task(struct edc_scheduler *sched, const char *desc, edc_channel_type channel_type)
{
    if (!sched)
        return NULL;

    char *tid = edc_scheduler_gen_task_id(sched);
    /* 任务归属调度器当前绑定的 session (经 edc_scheduler_set_session 切换),
     * 不再硬编码 "sess-1"。无 session 时 (测试/极端情况) 才回退占位 id。 */
    const char *sid = (sched->session && sched->session->session_id) ? sched->session->session_id : "sess-1";
    struct edc_task_group *fg = edc_scheduler_get_foreground(sched);
    const char *gid = fg ? fg->group_id : tid;

    struct edc_task *task = edc_task_new(tid, sid, gid, desc, channel_type);
    /* 不再强制置成 WAITING_INPUT: 那是旧模型"任务是个长期会话、停在原地等输入"的
     * 残留。edc_task_new 已经把它置成 READY (= IDLE 镜像) —— "空闲/就绪, 可被驱动"。
     * 旧的那一行存在的意义只是**绕开 IDLE 那行转移校验**; 两行合一之后不需要了。
     * 见 docs/retire-waiting-input.md §4 第 5 行。 */
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
    /* 前台组是 session 的属性 (terminal tab), scheduler 不再持有副本。这里只
     * 写属主 session: group 为 NULL (如 /task bg) 时清掉当前 session 的前台组。 */
    struct edc_session *owner = group ? group->session : sched->session;
    if (owner)
        owner->foreground_group = group;
}

struct edc_task_group *edc_scheduler_get_foreground(struct edc_scheduler *sched)
{
    if (!sched || !sched->session)
        return NULL;
    return sched->session->foreground_group;
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
    /* 前台组是目标 session 自己的属性, 绑定 session 即恢复其前台组 ——
     * 无需再拷贝到 scheduler (新建 session 为 NULL, 首个任务会自建)。 */
    sched->session = session;
}

/* 从 scheduler actor 取当前 session 绑定的工作目录 (未绑定返回 NULL)。
 * 工具层拿到的只有 actor 指针 (tb->scheduler), actor->state 即 scheduler 结构
 * (见 edc_scheduler_new)。 */
const char *edc_scheduler_session_workdir(edc_actor_t *scheduler_actor)
{
    struct edc_scheduler *sched = scheduler_actor ? (struct edc_scheduler *)scheduler_actor->state : NULL;
    return (sched && sched->session) ? edc_session_get_workdir(sched->session) : NULL;
}

const char *edc_scheduler_task_workdir(edc_actor_t *scheduler_actor, struct edc_task *task)
{
    /* 属主 session 优先: 后台 session 的任务在"切走"后仍应按自己的绑定目录
     * 执行, 而不是当前正在显示的 session 的目录。 */
    struct edc_session *own = (task && task->group) ? task->group->session : NULL;
    if (own)
        return edc_session_get_workdir(own);
    return edc_scheduler_session_workdir(scheduler_actor);
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
            /* "还有任务没跑完" = 还活着 (life==ALIVE)。旧写法把四个终态/挂起态
             * 一个个列出来, 问的正是这一个问题。 */
            if (edc_task_is_alive(t))
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

void edc_scheduler_send_signal_to_task_ptr(struct edc_scheduler *sched, struct edc_task *task, edc_task_signal sig)
{
    if (!sched || !task)
        return;

    /* 信号是一次**事件**（§2.8）：反注册 in-flight 请求、改 task 状态、
     * 显式取消立即落盘这三步都搬进了 executor，这里只构造事件。
     * persist=true —— 这条路径只被用户命令 (/task stop) 调用。 */
    struct edc_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.kind = EDC_EVENT_SIGNAL;
    ev.as.signal.sig = sig;
    ev.as.signal.persist = true;
    edc_executor_advance(&sched->executor, task, &ev);
}

void edc_scheduler_send_signal_to_task(struct edc_scheduler *sched, const char *task_id, edc_task_signal sig)
{
    struct edc_task *task = edc_scheduler_find_task_by_id(sched, task_id);
    if (!task)
        return;
    edc_scheduler_send_signal_to_task_ptr(sched, task, sig);
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

    /* Resume gate: an unresolved cross-session dependency (source not restorable)
     * must hold the task. Resuming here would recompute deps_total without that
     * edge and run the task before its dependency. */
    if (edc_scheduler_task_has_unresolved_deps(sched, task))
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
            sched_move_task(sched, task, EDC_TASKSTEP_AWAIT_DEPS, EDC_LIFE_ALIVE);
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
                const char *note = "[interrupted: this tool was still running when the process restarted; "
                                   "it did not produce a result]";
                edc_turn_add_tool_message(task->active_turn, rec->call_id, rec->name ? rec->name : "", note);
            }
            edc_task_tool_call_remove(task, rec->call_id);
        }

        /* 2b. 仍未执行 (PENDING) 的: 重新派发 */
        if (edc_task_has_pending_tool_calls(task))
        {
            void *req_id = sched_gen_resume_request_id();
            sched_move_task(sched, task, EDC_TASKSTEP_AWAIT_TOOLS, EDC_LIFE_ALIVE);
            edc_scheduler_register_task(sched, task, req_id);
            task->current_request_id = req_id;
            /* 重发成功即等 toolbox 回来; 各工具的 EXECUTING 由 toolbox 的
             * TOOL_STARTED 上报后按 call_id 置位 —— 不再按链序预标。 */
            if (!sched->redispatch_tools || sched->redispatch_tools(sched, task, req_id) <= 0)
            {
                /* 无重发通道 / 无可派发 → 退回就绪, 避免卡死 */
                edc_scheduler_unregister_task(sched, req_id);
                task->current_request_id = NULL;
                sched_force_move_task(sched, task, EDC_TASKSTEP_READY, EDC_LIFE_ALIVE);
            }
            return;
        }
        /* 2c. 只有 EXECUTING 且已补完响应 → 所有工具都有结果, 落到下面续 LLM */
    }

    /* 3. 有待跑的 turn → LLM_PROCESSING, 重发 LLM */
    if (task->active_turn)
    {
        void *req_id = sched_gen_resume_request_id();
        sched_move_task(sched, task, EDC_TASKSTEP_AWAIT_EXEC, EDC_LIFE_ALIVE);
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
            sched_force_move_task(sched, task, EDC_TASKSTEP_READY, EDC_LIFE_ALIVE);
        }
        return;
    }

    /* 4. 无待跑内容 → 就绪 (READY) */
    sched_force_move_task(sched, task, EDC_TASKSTEP_READY, EDC_LIFE_ALIVE);
}

/* ── 前台任务状态通知 ─────────────────────────────────────── */

/* 前台 group 的 active task 状态变化后立即推送一次 task 树快照。很多这类
 * 迁移不产生任何 channel 消息(依赖完成后自动触发、取消信号、LLM/工具链
 * 内部切换等), 而 TUI 的 "处理中" 动画与侧边栏完全由快照驱动; 若不主动
 * 推送, 空闲状态的 TUI 没有任何帧在跑轮询, 只能等下一条消息/按键才碰巧
 * 发现状态变了。快照在 scheduler 线程同步生成, 与 TUI 拉取共用同一路径。 */
/* 统一的状态迁移入口。设置状态后**只置脏** —— 真正下发由 actor handler 在
 * 处理完本条消息之后合并做一次 (见 edc_scheduler_actor_handler)。
 *
 * 以前这里直接推, 但闸门只放行**前台 active task**: 后台任务的状态变化没有任何
 * 推送, 侧栏只能靠 TUI 那个 500ms 轮询兜底 —— 而那个轮询写在 Render() 里, ftxui
 * 只在有帧请求时才调 Render, 于是它靠工作条动画续命, 全部空闲时根本不跑。
 * 现在任何 task 的变化都置脏: 后台行的更新不再依赖屏幕上有没有动画。
 *
 * 说的是**目标态** (step, life) 而不是旧词 —— 这个入口同时也是 executor 的
 * port (见 ex_set_task_state), 而 executor 只知道"任务该到哪一步了"。 */
static void sched_move_task(struct edc_scheduler *sched, struct edc_task *task, edc_task_step step, edc_task_life life)
{
    edc_task_move(task, step, life);
    if (sched)
        sched->snapshot_pending = true;
}

/* 强制置相位 (不查转移表) —— 给**兜底出口**用。
 *
 * 派不出去时不能让任务卡在 AWAIT_EXEC / AWAIT_TOOLS: 那是死胡同, resume 会永远
 * 重入。旧代码这里写的是 set_state(WAITING_INPUT), 但 TOOL_CALLING / LLM_PROCESSING
 * → WAITING_INPUT **本来就不在转移表里**, set_state 静默失败 —— 兜底其实没生效,
 * 任务就卡在那里。落点是 READY (退休 WAITING_INPUT 之后与 IDLE 同义)。
 * 见 docs/retire-waiting-input.md §4 第 8 行。 */
static void sched_force_move_task(struct edc_scheduler *sched, struct edc_task *task, edc_task_step step,
                                  edc_task_life life)
{
    edc_task_force_move(task, step, life);
    if (sched)
        sched->snapshot_pending = true;
}

/* ── 内部：消息路由 ────────────────────────────────────────── */

static void sched_handle_input(struct edc_scheduler *sched, struct edc_actor_msg *msg)
{
    struct edc_channel_message *input = msg->data;
    if (!input)
        return;

    struct edc_task_group *fg = edc_scheduler_get_foreground(sched);
    struct edc_task *task = NULL;

    /* 斜杠命令 / @组寻址 交给命令处理器 (shell); 普通输入**一律新建 task**。
     *
     * 这里以前有两条 "attach 到前台 group 中停着的任务" 的路 (先看 active task 是否
     * 停着, 再扫第一个 IDLE 成员)。那是旧模型的产物 —— 任务是一个长期会话, 停着等
     * 下一条消息喂进来。新模型里 task 创建时就带着它的 input, 休眠态走
     * SUSPENDED + /task resume, 于是**没有任何任务需要被 attach**: 那些"停着的任务"
     * (fork leader、/task join 的 Joined task、@task 无正文) 都是 turn-less 残渣,
     * 已在各自的创建点改成拒绝或带上 input, 落盘判据也不再收它们。
     * 见 docs/retire-waiting-input.md §4 第 10 行。 */
    const bool is_plain = input->content && input->content[0] != '/' && input->content[0] != '@';

    /* 命令处理器 (shell): /… 与 @… 由它处理, 普通输入也由它建 task。 */
    if (sched->handle_command && sched->handle_command(sched, msg->from, input))
        return;

    /* 无命令处理器 (或未处理) → 兜底新建 task (本条输入就是它的 turn)。 */
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

    /* 普通输入也遵循 python-like 引号解析："""…""" / '''…''' / '…' / "…" 的
     * 引号在建 task 时也要剥掉，与 @ 新建 task 的 shell 路径一致。
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

    sched_move_task(sched, task, EDC_TASKSTEP_AWAIT_EXEC, EDC_LIFE_ALIVE);
    if (sched->send_llm)
    {
        task->llm_rounds++; /* 发出一条 LLM 请求 → 消耗一轮迭代预算 */
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

/* ── executor 的出站 ops ─────────────────────────────────────
 * 每条都对应今天在 sched_handle_* 里的那个调用点，逐条搬过来。
 * 一律先查回调是否注入（与今天 `if (sched->send_llm)` 的形状一致）。 */

static void ex_send_llm(void *ctx, struct edc_task *task, void *request_id)
{
    struct edc_scheduler *sched = ctx;
    if (sched->send_llm)
        sched->send_llm(sched, task, request_id);
}

static void ex_send_reply(void *ctx, struct edc_task *task, const char *content, edc_channel_message_type type,
                          void *request_id)
{
    struct edc_scheduler *sched = ctx;
    if (sched->send_reply)
        sched->send_reply(sched, task, content, type, request_id);
}

static void ex_send_tool_message(void *ctx, struct edc_task *task, const char *header, const char *content,
                                 edc_render_pref_t render_pref, void *request_id)
{
    struct edc_scheduler *sched = ctx;
    if (sched->send_tool_message)
        sched->send_tool_message(sched, task, header, content, render_pref, request_id);
}

static void ex_dispatch_tools(void *ctx, struct edc_task *task, void *llm_resp, void *request_id)
{
    struct edc_scheduler *sched = ctx;
    if (sched->dispatch_tools)
        sched->dispatch_tools(sched, task, llm_resp, request_id);
}

static void ex_set_task_state(void *ctx, struct edc_task *task, edc_task_step step, edc_task_life life)
{
    sched_move_task((struct edc_scheduler *)ctx, task, step, life);
}

static void ex_unregister_task(void *ctx, void *request_id)
{
    edc_scheduler_unregister_task((struct edc_scheduler *)ctx, request_id);
}

static void ex_notify_dependents(void *ctx, struct edc_task *task)
{
    (void)ctx;
    edc_task_notify_dependents(task);
}

static void ex_flush_redirects(void *ctx, struct edc_task *task)
{
    (void)ctx;
    sched_flush_redirects(task);
}

static void ex_task_snapshot(void *ctx)
{
    sched_handle_task_snapshot((struct edc_scheduler *)ctx, "tool");
}

/* 展示逻辑: 这个 task 此刻在被显示吗 (规则 8 的 active-task 闸门)。 */
static bool ex_should_snapshot_on_complete(void *ctx, struct edc_task *task)
{
    struct edc_scheduler *sched = ctx;
    return !sched->session || edc_session_get_active_task(sched->session) != task ||
           task->group != sched->session->foreground_group;
}

/* task 之间的事: 依赖链上排队的输入自动启动。 */
static void ex_start_queued_dependents(void *ctx, struct edc_task *task)
{
    struct edc_scheduler *sched = ctx;
    struct edc_task_dep *dep;
    LL_FOREACH(task->dependents, dep)
    {
        struct edc_task *dt = dep->target;
        /* 依赖链上排队的输入: 目标**带着 turn 停着** (READY = IDLE) 就自动启动。 */
        if (dt->active_turn && edc_task_is_ready(dt))
        {
            sched_move_task(sched, dt, EDC_TASKSTEP_AWAIT_EXEC, EDC_LIFE_ALIVE);
            if (sched->send_llm)
            {
                dt->llm_rounds++; /* 排队的依赖输入开始消耗预算 */
                sched->send_llm(sched, dt, dt->current_request_id);
            }
        }
    }
}

static struct edc_session *ex_task_session(void *ctx, struct edc_task *task)
{
    return sched_task_session((struct edc_scheduler *)ctx, task);
}

/* 表达式原样搬自 sched_handle_llm_resp 的 `no_key`（含 config 为 NULL 时判 false）。 */
static bool ex_no_api_key(void *ctx)
{
    struct edc_scheduler *sched = ctx;
    return sched->config &&
           (!sched->config->llm_api_conf.api_key || sdslen(sched->config->llm_api_conf.api_key) == 0);
}

static int ex_max_iter(void *ctx)
{
    struct edc_scheduler *sched = ctx;
    return (sched->config && sched->config->max_iter > 0) ? sched->config->max_iter : 0;
}

static void ex_trigger_memory(void *ctx)
{
    struct edc_scheduler *sched = ctx;
    if (sched->trigger_memory)
        sched->trigger_memory(sched);
}

static void ex_save_session(void *ctx, struct edc_session *s)
{
    struct edc_scheduler *sched = ctx;
    if (sched->save_session)
        sched->save_session(sched, s);
}

/* executor 侧的接口名保留 (改它要动 ops 表), 但 C3 之后语义是"有状态变化, 该下发
 * 了" —— 具体是哪个 task 不重要: **任何** task 的变化都要让侧栏知道, 这正是删掉
 * 那个 500ms 轮询的前提。 */
static void ex_notify_foreground_state(void *ctx, struct edc_task *task)
{
    (void)task;
    struct edc_scheduler *sched = ctx;
    if (sched)
        sched->snapshot_pending = true;
}

static const struct edc_executor_ops sched_executor_ops = {
    .send_llm = ex_send_llm,
    .send_reply = ex_send_reply,
    .send_tool_message = ex_send_tool_message,
    .dispatch_tools = ex_dispatch_tools,
    .set_task_state = ex_set_task_state,
    .unregister_task = ex_unregister_task,
    .notify_dependents = ex_notify_dependents,
    .flush_redirects = ex_flush_redirects,
    .task_snapshot = ex_task_snapshot,
    .should_snapshot_on_complete = ex_should_snapshot_on_complete,
    .start_queued_dependents = ex_start_queued_dependents,
    .task_session = ex_task_session,
    .no_api_key = ex_no_api_key,
    .max_iter = ex_max_iter,
    .trigger_memory = ex_trigger_memory,
    .save_session = ex_save_session,
    .notify_foreground_state = ex_notify_foreground_state,
};

/* ── actor 事件 → 归一化 → executor ──────────────────────────
 * scheduler 只负责"按 request_id 找 task"与"丢弃已取消的迟到事件" ——
 * 前者是 task 之间的索引，后者是取消状态（task 之间），都不属于 task 之内。 */

static void sched_handle_llm_resp(struct edc_scheduler *sched, struct edc_actor_msg *msg)
{
    struct edc_task *task = edc_scheduler_find_task(sched, msg->request_id);
    if (!task || task->cancelled_request)
        return;
    edc_executor_advance_llm_resp(&sched->executor, task, msg->request_id, msg->data);
}

static void sched_handle_tool_resp(struct edc_scheduler *sched, struct edc_actor_msg *msg)
{
    struct edc_task *task = edc_scheduler_find_task(sched, msg->request_id);
    if (!task || task->cancelled_request)
        return;
    edc_executor_advance_tool_resp(&sched->executor, task, msg->request_id, msg->data);
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
    edc_executor_advance(&sched->executor, task, &ev);
}

/* toolbox 请求把工具结果回显到终端 (TOOL 消息)。与 thought/RESP 一致，
 * 只在调度线程上检查 session->active_task：后台任务的工具输出一律静默。 */
static void sched_handle_tool_emit(struct edc_scheduler *sched, struct edc_actor_msg *msg)
{
    struct edc_tool_emit_view v;
    if (!edc_executor_view_tool_emit(msg->data, &v) || !v.content)
        return;

    struct edc_task *task = edc_scheduler_find_task(sched, msg->request_id);
    if (!task || task->cancelled_request)
        return;

    /* 当前显示的 session 只放行 active task; 后台 session 的工具输出照发,
     * 由前端按其 session_id 静默写进它自己的流。 */
    struct edc_session *own = sched_task_session(sched, task);
    if (!own)
        return;
    if (own == sched->session && edc_session_get_active_task(own) != task)
        return;

    if (sched->send_tool_message)
        sched->send_tool_message(sched, task, v.header, v.content, v.render_pref, msg->request_id);

    /* todowrite 每次成功更新 (含清空) 都会回显一条 TOOL 消息; 该消息只
     * 携带文本, todo 本身已挂到 task->todos。多轮工具链内 task 状态不再
     * 变化 (TOOL_CALLING 期间), 不会触发状态通知快照 —— 这里主动补推一次,
     * 让 TUI 的 TodoWidget 立即按新清单刷新 (规则3)。 */
    if (v.header && strcmp(v.header, "todowrite") == 0)
        sched_handle_task_snapshot(sched, "todo");
}

/* ── Actor 入口 ────────────────────────────────────────────── */

/* 序列化当前 session 的 task 树 (group → members)，供 TUI 侧边栏渲染。
 * 用 cJSON 生成 JSON 文本，在 scheduler 线程上执行，天然无并发。 */
/* 端到端检查钩子。设了 `EDGECLAW_SNAPSHOT_TRACE=<path>` 就把每次快照推送追加
 * 一行：原因 + **实际下发的 JSON**（即侧栏被告诉的全部内容）。
 *
 * 为什么需要它：TUI 那个 500ms 轮询是后台任务状态变化的唯一通路（推送只覆盖
 * 前台 active task）。要删掉轮询，就得证明后台任务的变化改由**状态变化**送达 ——
 * 那是渲染层的性质，比对产物看不出来。原因标记让检查具备判别性：删轮询之前，
 * 后台任务的变化只会以 reason=poll 出现；之后必须出现 reason=state。
 *
 * 不设该变量时：一次 getenv，零输出，零开销。 */
static void sched_trace_snapshot(const char *reason, sds json)
{
    const char *path = getenv("EDGECLAW_SNAPSHOT_TRACE");
    if (!path || !*path)
        return;
    FILE *f = fopen(path, "a");
    if (!f)
        return;
    fprintf(f, "push reason=%s %s\n", reason ? reason : "?", json ? json : "");
    fclose(f);
}


static void sched_handle_task_snapshot(struct edc_scheduler *sched, const char *reason)
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
        cJSON_AddBoolToObject(gj, "fg", edc_scheduler_get_foreground(sched) == g);
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
            cJSON_AddStringToObject(tj, "state", edc_task_state_str(t));
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
             * todos (item 数组, 同 todowrite 参数形状)。TUI 借此把
             * fg 组 active task 的清单渲染到右侧 TodoWidget —— 快照是侧边栏
             * (任务树 + todo) 的唯一数据源, 不再需要旁路 payload 通道。 */
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
    case BC_ACTOR_MSG_GROUP_FOLD_DONE:
        /* L1 折叠完成, 持久化 state (turns_folded/memory/折叠水位)。
         * 折叠消息不带 group, 无法定位属主 session, 退化为保存当前 session;
         * 后台 session 的折叠结果会在它下次被保存时落盘。 */
        if (sched->save_session)
            sched->save_session(sched, sched->session);
        break;
    case BC_ACTOR_MSG_TASK_SNAPSHOT:
        sched_handle_task_snapshot(sched, "poll");
        break;
    default:
        break;
    }

    /* 本消息引起的状态变化合并成**一次**下发 (最多一次/消息)。放在释放 msg 之前:
     * 快照要读整棵 task 树, 而 handler 放掉 msg 后有些路径会连带释放负载。 */
    if (sched->snapshot_pending)
    {
        sched->snapshot_pending = false;
        sched_handle_task_snapshot(sched, "state");
    }

    edc_actor_msg_free(msg);
}
