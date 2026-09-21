#include "edc_scheduler.h"
#include "edc_cmd_parse.h"
#include "edc_compat.h"
#include "edc_config.h"
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
/* 任务树快照: 只服务侧边栏 (任务树 + todo + 计数)。对话内容不再随快照下发
 * —— 聊天区由 TUI 自己持久化/回放显示流 (见 edc_tui_window.cpp)。 */
/* 显式拉取: 前端点名要 `session_id` 那棵树 (BC_ACTOR_MSG_TASK_SNAPSHOT 的 data)。 */
static void sched_handle_pull_snapshot(struct edc_scheduler *sched, const char *session_id);
static void sched_handle_task_snapshot(struct edc_scheduler *sched, struct edc_session *session, const char *reason);
static void sched_trace_notify(const char *reason, const char *session_id);
/* TRANSIENT 任务的回收: 状态迁移/信号改变相位时入队, 消息末尾统一结算
 * (见下方 "TRANSIENT 任务的回收" 一节)。信号路径在文件前部, 故这里前向声明。 */
static void sched_queue_reap(struct edc_scheduler *sched, struct edc_task *task);
/** 终结态 (COMPLETED / CANCELLED / ERROR) —— 定义在下面的回收一节。 */
static bool sched_task_finished(struct edc_task *task);
static void sched_force_move_task(struct edc_scheduler *sched, struct edc_task *task, edc_task_step step,
                                  edc_task_life life);
/* executor 的出站 ops 表（定义在文件后半；这里前向声明，供 new 里 init 用）。 */
static const struct edc_executor_ops sched_executor_ops;

/* 任务的属主 session —— 已归位 edc_task.c 的 edc_task_session() (它只是
 * task->group->session, 是 task 自己的关系, 不该由 scheduler 代言)。 */

/* 前台组是 session 的属性 (terminal tab); scheduler 不持有副本。 */
static struct edc_task_group *sched_foreground(struct edc_session *session)
{
    return session ? session->foreground_group : NULL;
}

/* 入站消息的 session 标签 → session 对象: 同包直调 store 的读口。
 * 没有 store (单测没装) 或标签解析不出来 → NULL: 调用方**拒绝**这条输入, 不猜。 */
static struct edc_session *sched_resolve_session(struct edc_scheduler *sched, const char *session_id)
{
    if (!sched || !sched->sessions)
        return NULL;
    return edc_session_store_resolve_session(sched->sessions, session_id);
}

/* ── "某棵树的某处变了" (事件, 不带树; 见 edc_scheduler_snapshot_changed_fn) ──
 *
 * 只记录**哪个 session** 变了 (按指针去重), 由 actor handler 在本条消息末尾
 * 一次性通知出去。前端收到通知后自己决定要不要拉 —— 于是没人在看的那棵树
 * 一次都不会被序列化。 */
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

/* 本条消息引起的后果结算: 把脏 session 通知出去。一个一个发 (每个 session 一条
 * 小消息), 发完清空 —— 下一次变化重新开始攒。 */
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

    /* 回收队列只持有指针 —— 任务归 session 所有, 这里只丢数组 (session 关闭时
     * 才真正 free 它们)。 */
    free(sched->reap_pending);
    sched->reap_pending = NULL;
    sched->reap_count = 0;

    /* 脏表只持有 session 指针 —— session 归 session_manager 所有, 这里只丢数组。 */
    free(sched->dirty);
    sched->dirty = NULL;
    sched->dirty_count = 0;
    sched->dirty_cap = 0;
    sched->reap_cap = 0;

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

struct edc_task *edc_scheduler_create_task(struct edc_scheduler *sched, struct edc_session *session, const char *desc,
                                           edc_channel_type channel_type)
{
    if (!sched || !session)
        return NULL;

    char *tid = edc_scheduler_gen_task_id(sched);
    /* 任务归属**显式传进来的** session (P3: 不再读"当前绑定"的那份副本)。
     * 无 session 直接拒绝 —— 猜一个 session 出来正是这份副本存在时的老毛病。 */
    const char *sid = session->session_id ? session->session_id : "sess-1";
    struct edc_task_group *fg = sched_foreground(session);
    const char *gid = fg ? fg->group_id : tid;

    struct edc_task *task = edc_task_new(tid, sid, gid, desc, channel_type);
    /* 不再强制置成 WAITING_INPUT: 那是旧模型"任务是个长期会话、停在原地等输入"的
     * 残留。edc_task_new 已经把它置成 READY (= IDLE 镜像) —— "空闲/就绪, 可被驱动"。
     * 旧的那一行存在的意义只是**绕开 IDLE 那行转移校验**; 两行合一之后不需要了。
     * 见 docs/retire-waiting-input.md §4 第 5 行。 */
    free(tid);

    /* 正在处理的那条输入就是这条任务的原文: 挂到任务上 (落盘 + 随快照下发, 前端在
     * 选中该 task/组时把它渲染成该 task 的第一条 user message)。普通输入 / `@task` /
     * `/task new` / 兜底建任务全经过本函数; fork 与 system task 不经过 (它们不是由
     * 某条输入建的, 原文为 NULL)。 */
    if (sched->input_raw)
        task->raw_input = sdsnew(sched->input_raw);

    /* 树上多了一个任务 = 这棵树变了 → 置脏 (本条消息末尾推一次快照)。
     * 从前这口脏是"派发"那一步顺手置的 (sched_move_task); 派发统一归调度之后,
     * 它必须由**创建**自己说 —— 否则 --suspended / 等依赖的任务不会出现在侧栏。 */
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

/* ── system task: 有活干 + TRANSIENT + 可见 ──────────────────
 *
 * 老 `Group system` 是一个**空容器**: 没活干、会落盘、只能靠 `is_system` 特判
 * 隐藏 (5 处 skip, 漏一处就冒出来)。这里造出来的相反 —— 每个 system task 都带着
 * 真活干 (一次折叠 LLM 调用), 不落盘 (TRANSIENT), 且**不隐藏** (照常出现在快照与
 * `/task list` 里, 只是侧栏画在另一块)。三个属性各自"**一条规则 + 一个谓词**", 读者是必然的推论而非"唯一一处",
 * 判据见 runtime-model.md §5 规则 7 与 edc_task.h。
 *
 * 组是复用还是新建: 有系统组就加进去, 没有就以它当 leader 建一个 (group_id 就是
 * 它的 task_id —— 与用户组同一套规则, 没有魔法 id)。 */
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

    /* 三个属性: 不落盘、完成即回收; 产出交给 group 记忆; 容器是系统组。 */
    task->lifetime = EDC_LIFETIME_TRANSIENT;
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
    /* input 装成**裸的** user 消息 (只有 role/content): 派发出去的是"任务的输入",
     * 不是一条聊天气泡 —— render/ts 那层装饰属于展示路径 (shell 的输入回显会加)。
     * 折叠 prompt 因此与旧实现发给 LLM 的请求逐字相同 (也才可比)。 */
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "role", "user");
    cJSON_AddStringToObject(msg, "content", input);
    edc_queue_push(&turn->messages, msg);
    task->active_turn = turn;
    return true;
}

/* ── 驱动 + 调度（唯一入口）──────────────────────────────────
 *
 * **驱动** = 把一个"可跑"的任务交给 executor 的 START 事件, 并把结果过策略点。
 * request_id 是 task 之间的索引 (task_hash 以它为键), executor 不认识它 —— 所以
 * **发号与登记归这里**, 而"下一步发什么 I/O"归 executor。
 *
 * 从前这件事被抄了 6 份 (shell spawn / /task new / /task fork / dispatch_task /
 * 排队的依赖 / resume), 而且顺序各不相同: shell 先登记再判依赖, /task new 反过来
 * —— 于是"排队等依赖"的任务在两条路上一个带 request_id、一个是 NULL, 回包路由
 * 结果完全不同 (实测: NULL 那条的响应永远找不到任务)。现在只有这一个出口。 */
static void *sched_gen_request_id(void)
{
    /* 高位段起步: 进程内唯一, 不会与测试/调用方自造的小整数字面量撞 (task_hash
     * 以指针为键, 撞了会把响应路由到别的任务)。 */
    static int64_t seq = (int64_t)1 << 42;
    return (void *)(intptr_t)(++seq);
}

/* 可跑 = 有活干、且此刻没人推它。依赖齐不齐**不问边**, 直接读 step —— 状态机
 * 就是那个问题的权威 (只有"依赖齐了"与"依赖作废"两条路能把任务放进 READY)。 */
static bool sched_task_runnable(struct edc_scheduler *sched, struct edc_task *task)
{
    return task && edc_task_is_ready(task) && task->active_turn &&
           !edc_scheduler_task_has_unresolved_deps(sched, task);
}

static void sched_drive_task(struct edc_scheduler *sched, struct edc_task *task);

/* 扫描的回调: 每个任务问一次"可跑吗"; 驱动成功 = 这一轮有进展。 */
struct sched_sweep_ctx
{
    struct edc_scheduler *sched;
    size_t driven;
};

static void sched_schedule_visit(void *ctx, struct edc_task *task)
{
    struct sched_sweep_ctx *sweep = ctx;
    if (!sched_task_runnable(sweep->sched, task))
        return;
    sched_drive_task(sweep->sched, task);
    sweep->driven++;
}

/* ── 调度：本条消息末尾的全表扫 ─────────────────────────────
 *
 * **表 = 内存里的全部任务** (session 链序 × 组链序 × 成员链序, 顺序固定)。枚举走
 * store 的只读口 `edc_session_manager_foreach_task` —— 未加载的 session 还没有树,
 * 所以"全表"天然等于"本进程已知的任务": 没有别的任务集合可扫, 也不需要额外过滤。
 *
 * **时机 = 每条入站消息的末尾** (actor handler 里, 回收之后、推快照之前)。没有
 * tick, 所以消息边界就是 tick: 所有能改 task 相位的事都从 handler 进来, 在这里
 * 扫一次就"不可能漏"。**不能放在单次推进之后** —— 一条消息里会有多次推进 (/session
 * stop 逐个取消成员、失败级联传播), 那样会在取消进行到一半时把下游启动; 放在消息
 * 末尾, 看到的是整条消息的最终状态, 因此"取消整组"之后一个请求都不会多发。
 *
 * **判据在状态上, 不在因果上**: 谁把任务弄成 READY 的 (完成 / 取消 / 失败 / 建边 /
 * resume 兜底) 无关紧要 —— 这就是"转 READY 就顺手发出去"的完整实现, 它不需要任何
 * 簿记 (没有就绪队列, 没有待驱动集, 扫描只读 task 的字段)。
 *
 * **扫到不动点**: 一次驱动**成功**时不产生新的可跑者, 但驱动**失败**会级联 ——
 * 失败要广播, 它的 SEQUENCE 依赖者会被放行成 READY, 而那个任务可能排在扫描顺序的
 * 更前面 (已经被看过一遍了)。所以扫完一轮还有进展就再扫一轮。
 *
 * **终止性**: 每个被驱动的任务这一轮必然离开 READY —— 要么进在途相位
 * (`AWAIT_EXEC` / `AWAIT_TOOLS`), 要么进终态 (`ERROR`, 失败出口**不会**把任务留在
 * 原地: 推不动就是失败)。所以可跑者数量每一轮严格减少, 轮数上界 = 可跑任务数 + 1。 */
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

/* 交给 executor 的 START, 再把事实过统一策略点。
 *
 * `!settled && !running` = 这次驱动什么都没发起 (就绪兜底 / 状态机拒绝): 刚发的
 * request_id 白发了, 当场收回 —— 否则 task_hash 会留下一格没人认领的索引, 而扫描
 * 下一条消息还会再驱动它一次。 */
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
    /* 前台组是 session 的属性 (terminal tab)。group 非 NULL 时属主由它自己推出;
     * NULL (/task bg) 时用调用方给的 session。 */
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
    /* 属主 session: 后台 session 的任务在"切走"后仍按自己的绑定目录执行,
     * 而不是"当前正在显示"的那个 session 的目录 (P3 ③ 起后者已不存在于 core)。
     * 没有属主就返回 NULL —— 由调用方兜底, core 不猜。 */
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
    /* 登记一个新请求 = 这个任务**又有在途请求了**。
     *
     * `cancelled_request` 是"当前那条请求已作废"的标记 (取消/挂起时置位, 应答处理器
     * 见到它就丢弃迟到应答), 它的生命周期 **到下一次发出请求为止** —— 所以在这里清。
     * 不清的后果 (2026-09-15 真机 511): 取消过的任务 resume 后显示"执行中", 但那条
     * 新请求的应答被同一个标记丢掉, 于是永远没有回音 (账本/消息都对, 只是没人接)。
     *
     * 这里清是安全的: 旧请求的迟到应答靠 **request_id 注销** 丢弃 (取消时已反注册,
     * 查表查不到), 与这个标记无关; 而新请求的 id 是刚注册的, 只有它可能命中。 */
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

    /* 信号是一次**事件**（§2.8）：反注册 in-flight 请求、改 task 状态两步在
     * executor；这里只构造事件。 */
    struct edc_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.kind = EDC_EVENT_SIGNAL;
    ev.as.signal.sig = sig;
    /* 信号路径不携带 request_id —— 要反注册的是 task 自己记着的那个
     * （exec_signal 不再碰它: 反注册是宿主的索引）。 */
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

/* 对一个 task 发信号 = task 层改相位 + 宿主的统一入口收尾 (索引 / 回收队列 / 置脏 /
 * 结算), 两步挨着, 中间没有别的东西可以插进来。
 *
 * 单任务路径 (`send_signal_to_task_ptr`) 经 executor 事件走同一条入口; 这个函数给
 * **宿主自己发起的扇出**用 (组级/会话级取消)。两条路落到同一个入口, 所以不会再出现
 * "一条路记得做簿记、另一条忘了"。 */
static void sched_signal_one(struct edc_scheduler *sched, struct edc_task *task, edc_task_signal sig)
{
    edc_task_step step_before = task->step;
    edc_task_life life_before = task->life;

    edc_task_send_signal(task, sig);

    /* 相位没动 = 这次信号什么也没决定 (重复取消是幂等的) —— 那就没有"终止"可结算,
     * 更不能拿 settled 去撞结算里那条 completed 分支 (那会再发一次回话)。 */
    if (task->step == step_before && task->life == life_before)
        return;

    /* 取消 = 明确终止: 反注册 (不再欠任何请求) + 回收队列 + 置脏 + 结算 (推快照 +
     * 落盘) 由统一入口一次做完。 */
    struct edc_advance_result res;
    memset(&res, 0, sizeof(res));
    res.outcome = EDC_STEP_DONE;
    res.changed = true;
    res.settled = true;
    edc_scheduler_apply_advance(sched, task, task->current_request_id, &res);
}

/* 对单个 group 发信号: **逐个成员走上面那条单任务路径**。
 * running_only=true 时只处理 edc_task_is_running() 的成员 (离开 session 的
 * "只取消正在跑的"), 其余成员的相位与注册都原样保留。
 *
 * 从前这里是三段手工循环 (先反注册 / 交给组模块逐成员发 / 末了补回收队列), 因为它
 * 绕开了统一入口 —— 而手工补丁注定漏: 漏了置脏、漏了落盘, 靠调用方各自记得 save
 * 兜着。现在一个循环、一条路径, 补丁没有存在的理由。 */
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

    /* ── 闸门: 只有 task **之间**的判定留在这里 ──────────────────
     *
     * 1. 无主的跨 session 依赖 (源恢复不出来) → 扣住不续: 在这里续跑会按"少一条边"
     *    重算 deps_total, 于是任务抢在依赖前面跑。
     * 2. 依赖未满足 → 挂回等待 (重数边是必要的: 边可能在这一轮里被作废/摘掉)。 */
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
            /* 宿主自己发起的转移: 走**统一入口**的簿记 (回收队列 + 置脏), 不自己拼 ——
             * 这就是 edc_scheduler_task_moved 存在的意思。 */
            edc_task_mark_waiting_deps(task);
            edc_scheduler_task_moved(sched, task);
            return;
        }
    }

    /* ── 驱动: task **之内**的重判归 executor 的 START ────────────
     *
     * 从前这里是四段 (补悬空响应 / 重派账本 / 续发 LLM / 兜底回就绪), 与另外 5 个
     * "派发"入口各抄一份。现在它们合成 `EDC_EVENT_START` 一处实现 —— resume 与
     * "任务转 READY 之后由扫描驱动"走的是**同一条路**, 所以不可能再出现"两条路
     * 只有一条接上了"这类偏差。 */
    sched_drive_task(sched, task);
}

/* ── TRANSIENT 任务的回收 (完成即回收) ───────────────────────
 *
 * `lifetime` 的语义是"这个任务不留下来", 它有两个后果、同一个来源谓词
 * (edc_task_is_transient): 不写盘 (持久化层, edc_state_serializer.c) + 完成即回收
 * (这里)。两处问的是同一个问题 —— "它要不要活过自己这摊活" —— 而不是"按 kind
 * 把它藏起来": 系统任务跑完就没了, 于是盘上不会有僵尸, 也不需要任何地方跳过它。
 *
 * 回收分两步, 都在本条消息末尾结算 (sched_drain_reap_pending):
 *   1. 入队 —— 状态迁移那一刻只排队, 绝不 free (那条路径上还有人拿着指针);
 *   2. 结算 —— 摘链、清 active_task、free; 组空了就删组。 */

static bool sched_task_finished(struct edc_task *task)
{
    /* 终态 = 不再推进。SUSPENDED 不算 (它还会被 resume)。 */
    return edc_task_is_completed(task) || edc_task_is_cancelled(task) || edc_task_has_failed(task);
}

static void sched_queue_reap(struct edc_scheduler *sched, struct edc_task *task)
{
    if (!sched || !task || !edc_task_is_transient(task) || !sched_task_finished(task))
        return;
    /* 同一任务只入队一次 (兜底路径可能再迁一次)。 */
    for (size_t i = 0; i < sched->reap_count; i++)
        if (sched->reap_pending[i] == task)
            return;
    if (sched->reap_count == sched->reap_cap)
    {
        size_t cap = sched->reap_cap ? sched->reap_cap * 2 : 4;
        struct edc_task **grown = realloc(sched->reap_pending, cap * sizeof(*grown));
        if (!grown)
            return;
        sched->reap_pending = grown;
        sched->reap_cap = cap;
    }
    sched->reap_pending[sched->reap_count++] = task;
}

static void sched_reap_one(struct edc_scheduler *sched, struct edc_task *task)
{
    struct edc_task_group *group = task->group;
    struct edc_session *owner = group ? group->session : NULL;

    /* 完成的请求不该还挂在表上; 万一挂着自己摘掉, 免得 hash 里留野指针。 */
    if (task->current_request_id)
    {
        edc_scheduler_unregister_task(sched, task->current_request_id);
        task->current_request_id = NULL;
    }
    /* 它不该被当成前台 active task, 但真被当成过就摘掉 —— 否则 session 上留
     * 一个悬垂指针。 */
    if (owner && owner->active_task == task)
        edc_session_set_active_task(owner, NULL);

    if (!group)
    {
        edc_task_free(task);
        return;
    }

    edc_task_group_remove_task(group, task);
    edc_task_free(task);
    /* 空了就删 (runtime-model.md §4.6): 系统组不是容器, 没有活干就不该存在。
     * 这一步是**结构性**的 (空组没有可承载的东西), 与 kind 无关。 */
    if (group->member_count == 0)
    {
        if (owner)
        {
            /* 系统组不该是前台组, 但万一被 `/task fg` 指过 —— 摘掉, 免得 session
             * 上留一个已释放的指针。 */
            if (owner->foreground_group == group)
                owner->foreground_group = NULL;
            edc_session_remove_group(owner, group);
        }
        edc_task_group_free(group);
    }
}

/* 结算点: 把待回收的 TRANSIENT 任务真正摘掉 (见 edc_scheduler.h 的声明)。
 * actor handler 在每条消息末尾调用; 任何在消息之外推进过任务状态的调用方
 * (测试、脚本路径) 也应调用它, 否则队列里的任务会留到下一轮。 */
void edc_scheduler_drain_reap_pending(struct edc_scheduler *sched)
{
    if (!sched || sched->reap_count == 0)
        return;
    while (sched->reap_count > 0)
    {
        struct edc_task *task = sched->reap_pending[--sched->reap_count];
        sched_reap_one(sched, task);
        /* 树上少了一行 (可能整组没了) —— 置脏, 由本消息末尾统一通知属主 session。 */
        sched_mark_dirty(sched, edc_task_session(task), "reap");
    }
}

/* ── 前台任务状态通知 ─────────────────────────────────────── */

/* 状态迁移**只置脏** —— 真正下发由 actor handler 在处理完本条消息之后合并做一次
 * (见 edc_scheduler_actor_handler)。
 *
 * 为什么要置脏而不是直接推: 依赖完成 / 取消信号 / LLM 与工具链的内部切换这类迁移
 * 不产生任何 channel 消息, 而 TUI 的"处理中"动画与侧边栏完全由快照驱动 —— 不置脏
 * 就只能等下一条消息或按键才碰巧发现状态变了 (从前更糟: 闸门只放行前台 active
 * task, 后台任务的变化靠 TUI 的 500ms 轮询兜底, 而那个轮询写在 Render() 里,
 * ftxui 只在有帧请求时才调它)。
 *
 * 落笔一律说**目标态** (step, life); 宿主自己发起的转移统一走
 * `edc_scheduler_task_moved` (同一个策略点) —— 调度把"派发"也收进那条路径之后,
 * 这里不再有第二个拼状态机调用的地方。 */

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
    /* force 也可能把 TRANSIENT 任务直接送到终态 (兜底/取消) —— 同一条回收规则。 */
    sched_queue_reap(sched, task);
    if (sched)
        sched_mark_dirty(sched, edc_task_session(task), "state");
}

/* ── 内部：消息路由 ────────────────────────────────────────── */

static void sched_handle_input(struct edc_scheduler *sched, struct edc_actor_msg *msg)
{
    const struct edc_sched_input *input = msg->data;
    if (!input)
        return;

    /* 这条输入属于哪个 session: 由**消息自己**说 (P3: 不再读 scheduler 身上那份
     * "当前绑定"的副本)。标签为空时由 store 给默认值 (活跃 session),
     * 于是 crontab 这类没有 session 概念的入站行为不变; 而 TUI/CLI 永远显式带 id。
     * 解析不出来就**拒绝** —— 猜一个 session 出来正是老实现"切错台"的来源。 */
    struct edc_session *session = sched_resolve_session(sched, input->session_id);
    if (!session)
    {
        log_warn("input dropped: no session for tag '%s'", input->session_id ? input->session_id : "(none)");
        return;
    }

    struct edc_task_group *fg = sched_foreground(session);
    struct edc_task *task = NULL;

    /* 记下**正在处理的这条输入的原文** (见 edc_scheduler.h 的 `input_raw`):
     * `edc_scheduler_create_task` 见它非空, 就把原文挂到新任务上并发出"任务已创建"
     * 事件 —— 那是前端唯一的归属依据。处理期间有效, 所以本函数**每个出口都要清掉**
     * (下面两处; 新增出口时别忘)。 */
    sched->input_raw = input->content;

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

    /* 命令处理器 (shell): /… 与 @… 由它处理, 普通输入也由它建 task。
     * 参数是 core 的入站输入 —— 处理器要原始消息时从 `input->origin` 取。 */
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
    /* 到此为止: **装输入**是这里的事, "把它跑起来"是消息末尾扫描的事 (唯一驱动
     * 入口)。从前这里还自己登记 request_id + AWAIT_EXEC + send_llm —— 那是 6 份
     * 拷贝里的一份, 而它的 id 甚至来自入站消息 (与 task_hash 的键语义不同)。 */

    sched->input_raw = NULL; /* 这条输入处理完了 */
}

/* 输出重定向落盘 —— 已归位 edc_task.c 的 edc_task_flush_redirects()。 */

/* ── executor 的出站 ops（只剩"发起下一步 I/O"的两条）─────────
 * 准入判据与完整的迁移表见 edc_executor.h 与
 * docs/executor-outbound-boundary.md §2。其余 9 项已从接口里消失 —— 它们变成
 * `struct edc_advance_result`（事实）+ 下面的 edc_scheduler_apply_advance（策略）。
 * 一律先查回调是否注入（与今天 `if (sched->send_llm)` 的形状一致）。 */

static bool ex_send_llm(void *ctx, struct edc_task *task, void *request_id)
{
    struct edc_scheduler *sched = ctx;
    /* 没装端口就是"推不动"这个事实本身 (见 edc_executor.h 的 ops 说明) —— 不能
     * 静默吞掉: 那正是"任务看着在跑、其实没人接"的来源。 */
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

/* 依赖链上"带着 turn 排队"的目标由**消息末尾的扫描**启动 (判据是状态, 不是
 * "谁的 dependents") —— 所以这里不再有 sched_start_queued_dependents。
 * 那条路是"完成"这一种终结方式的特例: 取消/失败把下游转成 READY 时它不在场,
 * 于是两种终结方式对下游的处置不同 (一个启动、一个永久干等)。 */

/* ── 跨容器的收尾 ──────────────────────────────────────────── */

/* 同包直调 store: 落盘是 store 的事, 这里只把"要存谁"递过去 (NULL = 活跃的)。 */
static void sched_save_session(struct edc_scheduler *sched, struct edc_session *s)
{
    if (!sched || !sched->sessions)
        return;
    edc_session_store_save_session_state(sched->sessions, sched, s);
}

/* 没配 API key 时的错误文案 —— 提示用户去 /connect。
 * 这段原来在 executor 里（它当时直读 config 的 api_key）, 但"怎么跟用户说"
 * 是**展示**, 不是推进。 */
static sds sched_error_copy(struct edc_scheduler *sched, const char *notice)
{
    const char *err = notice ? notice : "LLM request failed";
    const sds key = (sched && sched->config) ? sched->config->llm_api_conf.api_key : NULL;
    if (!key || sdslen(key) == 0)
    {
        /* First run: with no API key the request can only fail, so point the
         * user at the /connect dialog instead of just reporting the error. */
        return sdscatprintf(sdsempty(),
                            "%s\n\nNo API key configured — run /connect to set the API URL, model and key.", err);
    }
    return sdsnew(err);
}

/* 清账时的回调: **被连带动过**的 task (JOIN 级联取消 / SEQUENCE 依赖失效后转 READY)
 * 也走同一套簿记 + 结算。它们没有"调用者"能声明发生了什么, 所以事实由 task 现在的
 * 样子回答: 已经进终态 = 这次连带把它送进了终态, 该结算; 否则只是相位动了。
 *
 * 递归是**故意**的: 清账本身还会连带改动别的 task (A 取消 → B 级联取消 → C), 而债
 * 记在各自的发起者头上, 所以每清一个就顺带清它欠的那些。 */
static void sched_settle_cascaded(void *ctx, struct edc_task *task)
{
    struct edc_scheduler *sched = ctx;
    bool finished = sched_task_finished(task);

    struct edc_advance_result res;
    memset(&res, 0, sizeof(res));
    res.outcome = finished ? EDC_STEP_DONE : EDC_STEP_BLOCKED;
    res.changed = true;
    res.settled = finished;
    /* 连带进终态的 task 自带原因 (失败传播会写"它的依赖 xxx 失败了"), 用它当通知文案:
     * notice 留 NULL 时宿主会退回一句通用的 "LLM request failed" —— 对一个**从没发出过
     * LLM 请求**的依赖者是假话。取消态的连带受害者走另一条分支 (不发通知), 取不到也不影响。 */
    res.notice = task->output;
    edc_scheduler_apply_advance(sched, task, task->current_request_id, &res);
}

/* 一个 task 的簿记 + 结算 (① 索引 ② 簿记 ③ 结算)。
 * 对外的入口在下面 —— 它再加第 ④ 步 (清这次操作欠下的债)。 */
static void sched_apply_advance_one(struct edc_scheduler *sched, struct edc_task *task, void *req_id,
                                    const struct edc_advance_result *res)
{
    if (!sched || !task || !res)
        return;
    struct edc_session *own = edc_task_session(task);

    /* ① 索引: 进终态 = 不再欠任何请求 —— task_hash 里那一格该收了。 */
    if (res->settled && req_id)
        edc_scheduler_unregister_task(sched, req_id);

    /* ② 簿记: 相位变了 → TRANSIENT 回收队列（守卫在 sched_queue_reap 里, 不是
     *    终态的它一概不动） + 置脏。 */
    if (res->changed)
    {
        sched_queue_reap(sched, task);
        sched_mark_dirty(sched, own, "state");
    }

    if (!res->settled)
        return;

    /* ③ 结算: 进终态 → 六件"告诉外界"的事。**顺序与搬迁前逐字相同**。 */
    if (edc_task_is_completed(task))
    {
        /* 完成序号 = 属主容器内的完成次序（折叠水位、$N 输出引用都读它）。
         * `sink=GROUP_MEMORY` 的 system task 不进这个序列: 它不是用户完成次序里
         * 的一环 —— 连它所在容器的计数器也不动, 于是折叠不改变任何用户可见的序号。 */
        if (task->sink != EDC_SINK_GROUP_MEMORY && own)
            edc_task_set_completion_seq(task, own->next_completion_seq++);
        else
            edc_task_set_completion_seq(task, 0);

        /* 容器消息数刚因 turn 提交而增长。被更新输入顶替、或在后台/依赖链中完成
         * 的任务其 turn 是"静默"的, 需要主动通知一次, 否则侧边栏统计会停在旧值。 */
        sched_mark_dirty(sched, own, "complete");

        /* task 之间的边: 通知依赖者 (取消/失败的传播也走这条); **启动**排队者不在这里
         * —— 那是消息末尾扫描的事 (判据是状态)。 */
        edc_task_notify_dependents(task);

        /* 产出交付: 按 sink 选路（**分流在这里**, 不在 executor）。
         * CHANNEL = 回给用户 (send_reply 内部还会查 active_task 闸门);
         * GROUP_MEMORY = 写进目标组的记忆 (fold), 不产生任何回话。 */
        if (task->sink == EDC_SINK_GROUP_MEMORY)
            edc_group_fold_deliver_memory(task, res->notice);
        else if (sched->send_reply)
            sched->send_reply(sched, task, res->notice, EDC_CHANNEL_MESSAGE_RESP_TYPE, req_id);

        /* 收尾结构事件的两个订阅者: 内容轴折叠（折不该折由 group_fold 自己决定）
         * + 持久化（落盘是容器的语义: "盘上的状态变了"没有别的结构信号）。 */
        if (sched->sessions)
            edc_group_fold_try(sched, sched->sessions, sched->config);
        sched_save_session(sched, own);
    }
    else if (edc_task_is_cancelled(task))
    {
        /* 显式取消要立刻落盘 + 推快照: CANCELLED 只在内存里, 不保存的话重启后会
         * 按 session.json 里的旧状态恢复, 取消看起来像没生效。 */
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

/* ══ edc_scheduler_apply_advance —— 一次推进之后**唯一**的策略点 ═══════
 *
 * executor 只报事实（`struct edc_advance_result`）; 这里把"发生了什么"翻译成
 * "该告诉谁": 索引 / 回收队列 / 脏表 / 依赖者 / 产出交付 / 折叠 / 落盘。
 *
 * **这不是"重放 executor 的分支"** —— 它读的是**结果**（任务进终态了吗）, 不是
 * **过程**（走的哪个 case）。判据见 docs/executor-outbound-boundary.md §2。
 *
 * 四个事件调用点（LLM 响应 / 工具响应 / 工具开始 / 信号）都走它; 宿主自己发起
 * 的转移（派发、命令层）走 edc_scheduler_task_moved —— **一个入口 = 不可能漏推**。
 *
 * ④ 清账: 一次操作还会**连带**改动别的 task (JOIN 级联取消、依赖失效后转 READY),
 * 那些 task 不在调用者的名单里, 却欠同一份簿记。债记在 task 层 (它认识依赖图),
 * 在这里还 —— 于是"宿主忘了给某个 task 做簿记"这种漏法在结构上不存在。 */
void edc_scheduler_apply_advance(struct edc_scheduler *sched, struct edc_task *task, void *req_id,
                                 const struct edc_advance_result *res)
{
    if (!sched || !task || !res)
        return;

    sched_apply_advance_one(sched, task, req_id, res);

    /* ④ 清账: 递归 —— 清账本身还会连带改动别的 task。 */
    edc_task_drain_moves(task, sched_settle_cascaded, sched);
}

/* 宿主自己发起的转移也要过**同一套簿记** —— 否则改了状态却不推 TUI（2026-09-18
 * 实测的三条旁路）。调度把"派发"整个收进 `sched_drive_task` 之后，这条路的调用者
 * 只剩 `resume` 的闸门（"依赖未满足 → 挂回等待"那一格）。 */
void edc_scheduler_task_moved(struct edc_scheduler *sched, struct edc_task *task)
{
    struct edc_advance_result res;
    memset(&res, 0, sizeof(res));
    res.outcome = EDC_STEP_BLOCKED;
    res.changed = true;
    edc_scheduler_apply_advance(sched, task, NULL, &res);
}

/* ── actor 事件 → 归一化 → executor ──────────────────────────
 * scheduler 只负责"按 request_id 找 task"与"丢弃已取消的迟到事件" ——
 * 前者是 task 之间的索引，后者是取消状态（task 之间），都不属于 task 之内。 */

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

/* toolbox 把工具结果回显到终端 (TOOL 消息)。这一跳只出**内核独有的权威**,
 * 不是"替通道转发":
 *   1. **请求还活着吗** —— 按 request_id 在 task 哈希里查 (异步工具回调可能迟到,
 *      迟到时调用方手里那个 task 指针已经不可信); 查不到 / 已取消 → 丢弃;
 *   2. **属主是谁** —— 没有属主 session 就没有可投递的地方 (P3 ③: 不回落"当前绑定");
 *   3. **todowrite 的脏表** —— 见函数末尾那次补推。
 * 显示本身 (消息形状 + 投递) 归通道, 工具域决定"产出什么"; 展示闸门 (只显示显示中
 * session 的 active task) 在**前端** —— 这里只声明事实 `from_active_task`。 */
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
    /* 展示闸门已移到前端 (P3 ③): core 不再问"谁在看" (它已经没有"当前 session"
     * 这份副本了)。它只把**事实**随消息声明出去 —— "这条输出来自它 session 的
     * active task 吗" —— 由前端按"我正在显示哪棵树"决定显不显示。 */

    if (sched->send_tool_message)
        sched->send_tool_message(sched, task, v.header, v.content, v.render_pref, msg->request_id);

    /* todowrite 每次成功更新 (含清空) 都会回显一条 TOOL 消息; 该消息只
     * 携带文本, todo 本身已挂到 task->todos。多轮工具链内 task 状态不再
     * 变化 (TOOL_CALLING 期间), 不会触发状态通知快照 —— 这里主动补推一次,
     * 让 TUI 的 TodoWidget 立即按新清单刷新 (规则3)。 */
    if (v.header && strcmp(v.header, "todowrite") == 0)
        sched_mark_dirty(sched, own, "todo");
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
    fprintf(f, "pull reason=%s %s\n", reason ? reason : "?", json ? json : "");
    fclose(f);
}

/* 同一份 trace 的"事件"那一半: 通知不带树, 只记 谁 + 为什么。
 * 两条线合起来能回答"某个变化是怎么到达前端的" —— 事件 → 拉取 → 树。 */
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
    /* 一次**读**：先把已经入队的后果结算掉 (TRANSIENT 回收), 再建树 —— 否则应答里
     * 会带着一个不该再出现的任务, 而前端只在收到通知时才重拉 (它没有第二次机会
     * 去纠正这一帧)。生产上上一条消息早就结算完了, 这里多半是空操作。 */
    edc_scheduler_drain_reap_pending(sched);

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
    DL_FOREACH(session->groups, g)
    {
        cJSON *gj = cJSON_CreateObject();
        cJSON_AddStringToObject(gj, "id", g->group_id ? g->group_id : "");
        /* group 的类别 —— **纯投影, 不做任何判断**。它是 `kind` 的读者之一
         * (展示层), 而这里只是把它写进给 TUI 的快照; scheduler 自己不看它,
         * 更不据它隐藏/过滤任何东西 (system task 与用户任务一样出现在这份
         * 快照里, 区别只在于 TUI 把它画到另一个块)。 */
        cJSON_AddStringToObject(gj, "kind", edc_task_group_kind_name(g->kind));
        cJSON_AddNumberToObject(gj, "member_count", g->member_count);
        cJSON_AddBoolToObject(gj, "orphaned", g->is_orphaned);
        cJSON_AddBoolToObject(gj, "fg", sched_foreground(session) == g);
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
            /* 这条 task 是由哪条用户输入建成的 (原文, 即 task->raw_input)。
             * 随快照下发, 因为前端要在**选中该 task/组时**把它渲染成该 task 的
             * 第一条 user message —— 直接按 id 取, 不做任何"哪条输入对应哪个
             * task"的推断 (输入回显本身不带 task id)。"" = 不是由输入建的
             * (fork 的孩子 / system task)。 */
            cJSON_AddStringToObject(tj, "input", t->raw_input ? t->raw_input : "");
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
        /* 拉取: 前端**点名**要哪棵树 (data = session_id)。不点名就没有树 ——
         * ①b 之后没有任何"顺带把树推给你"的路径。 */
        sched_handle_pull_snapshot(sched, (const char *)msg->data);
        break;
    default:
        break;
    }

    /* 本消息引起的状态变化合并成**一次**下发 (最多一次/消息)。放在释放 msg 之前:
     * 快照要读整棵 task 树, 而 handler 放掉 msg 后有些路径会连带释放负载。 */
    /* 先结算回收 (TRANSIENT 任务跑完就没了), 再调度 (本条消息把谁变成了"可跑"),
     * 最后下发快照 —— 顺序反过来的话, 这一帧还会画出那个刚被回收的任务 / 画不出
     * 刚被驱动起来的任务。 */
    edc_scheduler_drain_reap_pending(sched);

    /* 调度: 本条消息的全部效果都已落地, 现在按**状态**认人 (唯一的驱动入口在
     * edc_scheduler_schedule 的注释里)。纯读消息 (拉快照) 也走到这里 —— 扫描只认状态,
     * 不认消息类型; 那正是"没有 tick, 消息边界就是 tick"的直接推论。 */
    edc_scheduler_schedule(sched);

    sched_drain_dirty(sched);

    edc_actor_msg_free(msg);
}

/* ── 按 id 拉取消息历史 (前端选中的 task / 组) ──────────────────────────
 * 前端选中一个 task 或一个组时经通道发请求过来, 这里给出它的消息历史。数据源是
 * `task->turns` —— 就是喂给 LLM 的那份对话; 消息**原样**带上 (每条都带 ts/render,
 * 见 edc_turn_add_message), 所以渲染端不需要猜渲染偏好, 也不必从屏幕上的先后顺序
 * 推断任何东西。组: 按成员顺序分块, 不跨 task 交错。
 * 返回 malloc 的 JSON 字符串 (调用方 free); 没有这个 task/组返回 NULL。 */

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
    /* 这条 task 是由哪条输入建成的 (原文)。前端拿它当该 task 的第一条 user 消息 ——
     * turn 里的那条是 parser 之后的, 用户敲的原样是这份。 */
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
