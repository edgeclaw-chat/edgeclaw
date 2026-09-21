#include "edc_task.h"
#include "edc_compat.h"
#include "edc_context.h"
#include "edc_session.h"
#include "edc_task_group.h"
#include "utlist_fix.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ── 状态名映射: 已退休 (R3b) ──────────────────────────────
 *
 * 旧的 `edc_task_state_name(enum)` / `edc_task_classify_state(str)` 随旧枚举一起删掉了。
 * 显示名现在只有一处来源: `edc_task_state_str` —— 由 (step, life) 直接出。
 * 反向 (串 → 状态) 从来没有 src 调用方: 落盘的 step/life 走 classify_step/life。 */

/* ── 状态转换校验 ──────────────────────────────────────────── */

/* ── step / life: 名称、解析、映射 ─────────────────────────── */

const char *edc_task_step_name(edc_task_step step)
{
    switch (step)
    {
    case EDC_TASKSTEP_READY:
        return "READY";
    case EDC_TASKSTEP_AWAIT_DEPS:
        return "AWAIT_DEPS";
    case EDC_TASKSTEP_AWAIT_EXEC:
        return "AWAIT_EXEC";
    case EDC_TASKSTEP_AWAIT_TOOLS:
        return "AWAIT_TOOLS";
    case EDC_TASKSTEP_AWAIT_RESOURCE:
        return "AWAIT_RESOURCE";
    case EDC_TASKSTEP_DONE:
        return "DONE";
    }
    return "READY";
}

const char *edc_task_life_name(edc_task_life life)
{
    switch (life)
    {
    case EDC_LIFE_ALIVE:
        return "ALIVE";
    case EDC_LIFE_SUSPENDED:
        return "SUSPENDED";
    case EDC_LIFE_COMPLETED:
        return "COMPLETED";
    case EDC_LIFE_CANCELLED:
        return "CANCELLED";
    case EDC_LIFE_ERROR:
        return "ERROR";
    }
    return "ALIVE";
}

edc_task_step edc_task_classify_step(const char *name)
{
    if (!name)
        return EDC_TASKSTEP_READY;
    if (strcmp(name, "AWAIT_DEPS") == 0)
        return EDC_TASKSTEP_AWAIT_DEPS;
    /* "AWAIT_INPUT" 是**旧名**, 从来就等同于 READY (task 创建时就带着 input, 没有
     * "等用户输入"这个相位)。必须收这一条: 前一个二进制写出的 v5 文件、以及
     * tools/migrate_session_v4_to_v5.py 早先的产物里, 停着的任务 step 都是它 ——
     * 不折成 READY 就会载入成不可 attach 的僵尸 (读侧已改按 READY/IDLE 判定)。 */
    if (strcmp(name, "AWAIT_INPUT") == 0)
        return EDC_TASKSTEP_READY;
    if (strcmp(name, "AWAIT_EXEC") == 0)
        return EDC_TASKSTEP_AWAIT_EXEC;
    if (strcmp(name, "AWAIT_TOOLS") == 0)
        return EDC_TASKSTEP_AWAIT_TOOLS;
    if (strcmp(name, "AWAIT_RESOURCE") == 0)
        return EDC_TASKSTEP_AWAIT_RESOURCE;
    if (strcmp(name, "DONE") == 0)
        return EDC_TASKSTEP_DONE;
    return EDC_TASKSTEP_READY;
}

edc_task_life edc_task_classify_life(const char *name)
{
    if (!name)
        return EDC_LIFE_ALIVE;
    if (strcmp(name, "SUSPENDED") == 0)
        return EDC_LIFE_SUSPENDED;
    if (strcmp(name, "COMPLETED") == 0)
        return EDC_LIFE_COMPLETED;
    if (strcmp(name, "CANCELLED") == 0)
        return EDC_LIFE_CANCELLED;
    if (strcmp(name, "ERROR") == 0)
        return EDC_LIFE_ERROR;
    return EDC_LIFE_ALIVE;
}

/* ── sink / lifetime: 名称、解析 (与 step/life 同一套做法) ──
 *
 * 这两个属性今天**不落盘** (TRANSIENT 任务根本不进 session.json; PERSISTENT 任务的
 * 取值恒为默认), 所以 `_name` 只服务日志/调试, `_classify` 暂无落盘调用方 —— 与
 * step/life 成套留着: 它们是同一族 (五个正交属性), 落盘一旦需要就有一致的入口。 */

const char *edc_task_sink_name(edc_sink sink)
{
    switch (sink)
    {
    case EDC_SINK_CHANNEL:
        return "CHANNEL";
    case EDC_SINK_GROUP_MEMORY:
        return "GROUP_MEMORY";
    }
    return "CHANNEL";
}

edc_sink edc_task_classify_sink(const char *name)
{
    if (!name)
        return EDC_SINK_CHANNEL;
    if (strcmp(name, "GROUP_MEMORY") == 0)
        return EDC_SINK_GROUP_MEMORY;
    return EDC_SINK_CHANNEL;
}

const char *edc_task_lifetime_name(edc_lifetime lifetime)
{
    switch (lifetime)
    {
    case EDC_LIFETIME_PERSISTENT:
        return "PERSISTENT";
    case EDC_LIFETIME_TRANSIENT:
        return "TRANSIENT";
    }
    return "PERSISTENT";
}

edc_lifetime edc_task_classify_lifetime(const char *name)
{
    if (!name)
        return EDC_LIFETIME_PERSISTENT;
    if (strcmp(name, "TRANSIENT") == 0)
        return EDC_LIFETIME_TRANSIENT;
    return EDC_LIFETIME_PERSISTENT;
}

bool edc_task_is_transient(const struct edc_task *task)
{
    return task && task->lifetime == EDC_LIFETIME_TRANSIENT;
}

/* 派生的显示名 (快照 / 日志 / `/task status|list|join` 文案)。见 edc_task.h: 词表冻结。
 *
 * 旧有的两级 (state_of → state_name) 随旧枚举一起退休 (R3b): 现在 (step, life) **直接**
 * 出词, 顺序与原实现逐字一致 —— **life 先说话** (挂起/终态掩掉 step), 然后才轮到 step。
 * 两个"畸形但必须不崩"的组合沿用旧的兜底口径: (DONE, ALIVE) → IDLE,
 * (AWAIT_RESOURCE, ALIVE) → WAITING_DEPS (旧枚举里没有对应的词)。
 * `WAITING_INPUT` 这个串**再也不会被返回** —— 那个相位退休了 (旧名 = READY)。 */
const char *edc_task_state_str(const struct edc_task *task)
{
    if (!task)
        return "IDLE";
    switch (task->life)
    {
    case EDC_LIFE_SUSPENDED:
        return "SUSPENDED";
    case EDC_LIFE_COMPLETED:
        return "COMPLETED";
    case EDC_LIFE_CANCELLED:
        return "CANCELLED";
    case EDC_LIFE_ERROR:
        return "ERROR";
    case EDC_LIFE_ALIVE:
        break;
    }
    switch (task->step)
    {
    case EDC_TASKSTEP_READY:
        return "IDLE";
    case EDC_TASKSTEP_AWAIT_DEPS:
        return "WAITING_DEPS";
    case EDC_TASKSTEP_AWAIT_EXEC:
        return "LLM_PROCESSING";
    case EDC_TASKSTEP_AWAIT_TOOLS:
        return "TOOL_CALLING";
    case EDC_TASKSTEP_AWAIT_RESOURCE:
        return "WAITING_DEPS"; /* 旧枚举没有对应值 */
    case EDC_TASKSTEP_DONE:
        break;
    }
    /* 兜底: DONE 但仍 ALIVE 的畸形组合。没有任何路径产出它。 */
    return "IDLE";
}

static bool edc_task_is_valid_transition(edc_task_step from_step, edc_task_life from_life, edc_task_step to_step,
                                         edc_task_life to_life);

/* 目标态: 一次转移同时说清"到哪一步"和"还活着吗"。这两件事过去挤在 `edc_task_state`
 * **一个** enum 里 —— 于是"挂起"既表示"跑到一半停住"又表示"重启后什么都没在跑",
 * 相位只能靠粗化保存。拆开 step/life 的意义在这里落地: **状态机自己也不再丢掉相位**
 * (校验用 (step, life), 不再用把 step 塌掉的旧词)。 */
/* ── 低层原语: 直接说目标态 ─────────────────────────────────
 * 一般调用方用上面的意图函数; 说目标态的两处是 scheduler 的统一入口与 executor 的
 * port —— 它们**先说话再置脏**, 不自己拼意图 (见 edc_scheduler.c / edc_executor.h)。 */
bool edc_task_move(struct edc_task *task, edc_task_step step, edc_task_life life)
{
    if (!task)
        return false;
    if (!edc_task_is_valid_transition(task->step, task->life, step, life))
        return false;
    task->step = step;
    task->life = life;
    return true;
}

/* ── 意图命名的写入口: 调用方说"我要干什么", 不拼目标态 ──────
 * 取代 `edc_task_set_state(task, TASK_X)`: 旧写法把"到哪一步"和"活着吗"藏在一个
 * enum 值里, 挂起更是**说不出**目标 step (TASK_SUSPENDED 只动 life)。返回 false =
 * 该转移不合法 (被状态机拒绝), 与旧 set_state 的返回值语义一致。 */
bool edc_task_await_exec(struct edc_task *task)
{
    return edc_task_move(task, EDC_TASKSTEP_AWAIT_EXEC, EDC_LIFE_ALIVE);
}

bool edc_task_await_tools(struct edc_task *task)
{
    return edc_task_move(task, EDC_TASKSTEP_AWAIT_TOOLS, EDC_LIFE_ALIVE);
}

bool edc_task_mark_ready(struct edc_task *task)
{
    return edc_task_move(task, EDC_TASKSTEP_READY, EDC_LIFE_ALIVE);
}

bool edc_task_mark_waiting_deps(struct edc_task *task)
{
    return edc_task_move(task, EDC_TASKSTEP_AWAIT_DEPS, EDC_LIFE_ALIVE);
}

bool edc_task_mark_completed(struct edc_task *task)
{
    return edc_task_move(task, EDC_TASKSTEP_DONE, EDC_LIFE_COMPLETED);
}

/* 失败要**广播**给依赖者 (定义在依赖那一段里, 见下方)。 */
static void task_notify_dependents_failed(struct edc_task *task);

/* 失败 = 置 ERROR **并且告诉自己所有的依赖者**。广播放在这里而不是调用方, 是为了让
 * "任务进 ERROR"这件事**无论谁发起**都传播: executor 的两个出口 (LLM 报错 / 未知
 * finish reason) 都不用记得做这件事, 将来新的失败点也不会漏。
 *
 * 与取消的分工同构: `edc_task_send_signal` = 置取消 + 广播, `edc_task_mark_failed`
 * = 置失败 + 广播 —— 两者都不做宿主簿记 (那是 `edc_scheduler_apply_advance` 的事),
 * 只把"连带动了谁的相位"记在发起者账上 (`task_record_move`)。 */
bool edc_task_mark_failed(struct edc_task *task)
{
    if (!task)
        return false;
    /* 自转移失败 = 它本来就没失败 (终态/非法相位), 那就什么都不该广播 —— 幂等。 */
    if (!edc_task_move(task, EDC_TASKSTEP_DONE, EDC_LIFE_ERROR))
        return false;
    task_notify_dependents_failed(task);
    return true;
}

bool edc_task_mark_cancelled(struct edc_task *task)
{
    return edc_task_move(task, EDC_TASKSTEP_DONE, EDC_LIFE_CANCELLED);
}

/* 挂起**只说 life**: 停在哪一步就停在哪一步 —— 这正是 step/life 拆分要保住的东西
 * (旧词 TASK_SUSPENDED 也是这个语义, 只是说不出口)。 */
bool edc_task_mark_suspended(struct edc_task *task)
{
    return task && edc_task_move(task, task->step, EDC_LIFE_SUSPENDED);
}

/* ── 转移表 (键 = (step, life)) ─────────────────────────────
 * 与旧表逐条对照。旧表以**折叠过的旧词**为键, 所以新表只能与它在实际可达的转移上
 * 等价 —— 唯一等价不了的那一格写在下面 (READY → READY)。 */
static bool edc_task_is_valid_transition(edc_task_step from_step, edc_task_life from_life, edc_task_step to_step,
                                         edc_task_life to_life)
{
    /* 同一个 (step, life) 之间没有"转移"可言。
     *
     * ⚠️ 与旧表的一处**不可能等价**: 旧代码判
     *     IDLE          → WAITING_INPUT   合法   (两个旧名, 同一个落点)
     *     WAITING_INPUT → WAITING_INPUT   非法
     * —— 同一个源、同一个目标却一真一假, 因为旧表的键是**名字**而不是状态。新表的键
     * 是 (step, life), 这种"同名不同行"无法表示, 两条统一按自转移处理。实际路径不受
     * 影响: 所有"回 READY"的调用点都先判 `edc_task_awaits_deps()` (源必为 AWAIT_DEPS),
     * 没有一条会从 READY 发起。这条残渣随 WAITING_INPUT 一起退休
     * (见 docs/retire-waiting-input.md §4 第 3 行)。 */
    if (from_step == to_step && from_life == to_life)
        return false;

    switch (from_life)
    {
    case EDC_LIFE_COMPLETED:
        return false; /* terminal */
    case EDC_LIFE_CANCELLED:
        /* resume: 取消只是"停下", 不是"这活没了" —— 与 SUSPENDED / ERROR 同一组目标。
         * 取消时 step 已被压成 DONE, 所以续跑必须按消息 / 账本重判该回哪一步
         * (edc_scheduler_resume_task)。 */
        return (to_life == EDC_LIFE_ALIVE &&
                (to_step == EDC_TASKSTEP_READY || to_step == EDC_TASKSTEP_AWAIT_DEPS ||
                 to_step == EDC_TASKSTEP_AWAIT_EXEC || to_step == EDC_TASKSTEP_AWAIT_TOOLS)) ||
               (to_step == EDC_TASKSTEP_DONE && to_life == EDC_LIFE_CANCELLED);
    case EDC_LIFE_SUSPENDED:
        /* resume 重新判定: 回依赖等待 / 就绪 / 重新请求 LLM / 继续工具链, 或取消 */
    case EDC_LIFE_ERROR:
        /* 失败的那一轮续跑 (与 SUSPENDED 同一组目标), 或取消。 */
        return (to_life == EDC_LIFE_ALIVE &&
                (to_step == EDC_TASKSTEP_READY || to_step == EDC_TASKSTEP_AWAIT_DEPS ||
                 to_step == EDC_TASKSTEP_AWAIT_EXEC || to_step == EDC_TASKSTEP_AWAIT_TOOLS)) ||
               (to_step == EDC_TASKSTEP_DONE && to_life == EDC_LIFE_CANCELLED);
    case EDC_LIFE_ALIVE:
        break;
    }

    switch (from_step)
    {
    /* READY = "带着 input 停着, 等着被派发": 派发 / 撞上依赖 / 取消 / 挂起 / **失败**。
     *
     * 失败那一格是后加的 (2026-09-18, 驱动统一进 executor 的 START): 驱动一个就绪任务
     * 时也可能**发起不了任何 I/O** —— 宿主没装 LLM / 工具箱端口、工具链重派不出去、
     * 相位不允许这一步。那时它是"这摊活跑不成", 而不是"停着等派发"; 不许它失败,
     * 它就只能永远留在 READY 被反复扫描 (而调度是扫到不动点的)。
     * 挂起那一格来自 "以挂起状态创建" (2026-09-14): 还没开始跑、但输入已装好的
     * 任务需要一条进入挂起的入口 —— 在此之前 SUSPENDED 只能从 AWAIT_TOOLS 到达,
     * 那是"跑到一半挂起"。往返是对称的 (SUSPENDED 行允许 resume 回
     * READY / AWAIT_DEPS / AWAIT_EXEC / AWAIT_TOOLS)。 */
    case EDC_TASKSTEP_READY:
        return (to_life == EDC_LIFE_ALIVE &&
                (to_step == EDC_TASKSTEP_AWAIT_EXEC || to_step == EDC_TASKSTEP_AWAIT_DEPS)) ||
               (to_step == EDC_TASKSTEP_DONE && (to_life == EDC_LIFE_CANCELLED || to_life == EDC_LIFE_ERROR)) ||
               (to_step == EDC_TASKSTEP_READY && to_life == EDC_LIFE_SUSPENDED);
    case EDC_TASKSTEP_AWAIT_DEPS:
    case EDC_TASKSTEP_AWAIT_RESOURCE: /* 旧词表里没有这一步 (镜像折成 WAITING_DEPS): 沿用同一行 */
        /* 依赖齐了 → 回就绪; 或取消 / 失败 */
        return (to_step == EDC_TASKSTEP_READY && to_life == EDC_LIFE_ALIVE) ||
               (to_step == EDC_TASKSTEP_DONE && (to_life == EDC_LIFE_CANCELLED || to_life == EDC_LIFE_ERROR));
    case EDC_TASKSTEP_AWAIT_EXEC:
        return (to_life == EDC_LIFE_ALIVE && to_step == EDC_TASKSTEP_AWAIT_TOOLS) ||
               (to_step == EDC_TASKSTEP_DONE &&
                (to_life == EDC_LIFE_COMPLETED || to_life == EDC_LIFE_CANCELLED || to_life == EDC_LIFE_ERROR));
    case EDC_TASKSTEP_AWAIT_TOOLS:
        return (to_life == EDC_LIFE_ALIVE && to_step == EDC_TASKSTEP_AWAIT_EXEC) ||
               (to_step == EDC_TASKSTEP_DONE &&
                (to_life == EDC_LIFE_COMPLETED || to_life == EDC_LIFE_CANCELLED || to_life == EDC_LIFE_ERROR)) ||
               (to_step == EDC_TASKSTEP_AWAIT_TOOLS && to_life == EDC_LIFE_SUSPENDED); /* 跑到一半挂起 */
    case EDC_TASKSTEP_DONE:
        /* 畸形组合: DONE 却还 ALIVE (显示名折成 IDLE, 见 edc_task_state_str 的兜底)。
         * 没有任何路径产出它; 沿用 READY 一行, 免得落到 default 之外的行为。 */
        return (to_life == EDC_LIFE_ALIVE &&
                (to_step == EDC_TASKSTEP_AWAIT_EXEC || to_step == EDC_TASKSTEP_AWAIT_DEPS)) ||
               (to_step == EDC_TASKSTEP_DONE && to_life == EDC_LIFE_CANCELLED) ||
               (to_step == EDC_TASKSTEP_DONE && to_life == EDC_LIFE_SUSPENDED);
    }
    return false;
}

/* ── 绕过状态机的写入口 (装配初始态 / 摆场景 / 构造"不可能状态") ──
 * 不校验, 直接落到 (step, life)。src 里只被 edc_task_new / edc_task_fork 用 (它们要
 * 装配一个初始态)。旧词的过渡桥 (`edc_task_set_state` / `edc_task_force_state`) 已随
 * 旧枚举一起退休 (R3b) —— 说目标态就是直接说 (step, life), 中间不再经一层词表。 */
void edc_task_force_move(struct edc_task *task, edc_task_step step, edc_task_life life)
{
    if (!task)
        return;
    task->step = step;
    task->life = life;
}

/* ── 生命周期 ──────────────────────────────────────────────── */

struct edc_task *edc_task_new(const char *task_id, const char *session_id, const char *group_id, const char *desc,
                              edc_channel_type channel_type)
{
    struct edc_task *task = g_new0(struct edc_task, 1);
    task->task_id = strdup(task_id);
    task->session_id = strdup(session_id);
    task->group_id = strdup(group_id);
    task->description = strdup(desc ? desc : "");
    /* 新任务: "带着 input 停着, 等着被派发" —— 没有"等用户输入"这个相位,
     * 所以直接置 (READY, ALIVE) (旧词写作 TASK_IDLE/TASK_WAITING_INPUT 两行)。 */
    edc_task_force_move(task, EDC_TASKSTEP_READY, EDC_LIFE_ALIVE);
    /* 另外两个属性 (sink / lifetime): 默认就是用户任务 —— 产出回给用户、落盘。
     * g_new0 已经把两个 0 值 (CHANNEL / PERSISTENT) 置好, 这里**显式**写一遍:
     * 它们由 enum 顺序保证, 而 enum 顺序不该是这里正确性的依赖。 */
    task->sink = EDC_SINK_CHANNEL;
    task->lifetime = EDC_LIFETIME_PERSISTENT;
    task->is_group_leader = 0;
    task->channel_type = channel_type;

    edc_queue_init(&task->turns);

    return task;
}

struct edc_task *edc_task_fork(struct edc_task *parent, const char *task_id, const char *desc,
                               uint32_t fork_point_completion_seq, edc_channel_type channel_type)
{
    if (!parent)
        return NULL;

    char auto_id[64];
    if (!task_id)
    {
        static int fork_seq = 0;
        snprintf(auto_id, sizeof(auto_id), "ft%d", ++fork_seq);
        task_id = auto_id;
    }

    struct edc_task *child = edc_task_new(task_id, parent->session_id, parent->group_id, desc, channel_type);
    child->forked_from = parent;
    child->fork_point_completion_seq = fork_point_completion_seq;
    /* 子任务与 edc_task_new 一样落在 READY (= IDLE 镜像); 不再强制 WAITING_INPUT。 */
    return child;
}

/* ── fork 快照 ────────────────────────────────────────────── */

struct edc_turn *edc_task_snapshot_turns(struct edc_task *task, struct edc_session *session,
                                         const char *snapshot_task_id)
{
    (void)session; /* turn 现在自己持有消息 */
    if (!task)
        return NULL;

    struct edc_turn *snap = edc_turn_new();
    if (snapshot_task_id)
        snap->task_id = strdup(snapshot_task_id);

    for (size_t qi = task->turns.head; qi < task->turns.tail; qi++)
    {
        struct edc_turn *turn = task->turns.data[qi];
        if (!turn)
            continue;

        for (size_t mj = turn->messages.head; mj < turn->messages.tail; mj++)
            edc_queue_push(&snap->messages, cJSON_Duplicate((cJSON *)turn->messages.data[mj], 1));
    }

    if (edc_queue_length(&snap->messages) == 0)
    {
        edc_turn_free(snap);
        return NULL;
    }
    return snap;
}

static void edc_task_free_turns(struct edc_task *task)
{
    struct edc_turn *turn;
    while ((turn = edc_queue_pop(&task->turns)) != NULL)
    {
        edc_turn_free(turn);
    }

    if (task->active_turn)
    {
        edc_turn_free(task->active_turn);
        task->active_turn = NULL;
    }

    edc_queue_destroy(&task->turns);
}

static void edc_task_free_deps(struct edc_task *task)
{
    struct edc_task_dep *dep, *tmp;

    LL_FOREACH_SAFE(task->deps, dep, tmp)
    {
        if (dep->source)
        {
            struct edc_task_dep *sd, *sd_tmp;
            LL_FOREACH_SAFE(dep->source->dependents, sd, sd_tmp)
            {
                if (sd->target == task)
                {
                    LL_DELETE(dep->source->dependents, sd);
                    free(sd);
                }
            }
        }
        LL_DELETE(task->deps, dep);
        free(dep);
    }

    LL_FOREACH_SAFE(task->dependents, dep, tmp)
    {
        if (dep->target)
        {
            struct edc_task_dep *td, *td_tmp;
            LL_FOREACH_SAFE(dep->target->deps, td, td_tmp)
            {
                if (td->source == task)
                {
                    LL_DELETE(dep->target->deps, td);
                    free(td);
                }
            }
            dep->target->deps_total--;
        }
        LL_DELETE(task->dependents, dep);
        free(dep);
    }
}

void edc_task_free(struct edc_task *task)
{
    if (!task)
        return;

    edc_task_free_turns(task);
    edc_task_free_deps(task);
    edc_task_tool_calls_clear(task);
    /* 没被宿主取走的连带改动名单 (纯内存): cb = NULL 只清空。任务都要释放了,
     * 名单也就没有意义 —— 记的债本来只对"还活着的宿主"有意义。 */
    edc_task_drain_moves(task, NULL, NULL);

    free(task->task_id);
    free(task->group_id);
    free(task->session_id);
    free(task->description);

    if (task->raw_input)
        sdsfree(task->raw_input);

    if (task->prompt)
        sdsfree(task->prompt);
    if (task->prompt_name)
        sdsfree(task->prompt_name);
    if (task->output)
        sdsfree(task->output);

    if (task->redirect_stdout)
        sdsfree(task->redirect_stdout);
    if (task->redirect_stderr)
        sdsfree(task->redirect_stderr);

    if (task->todos)
        cJSON_Delete(task->todos);

    free(task);
}

/* ── Turn ──────────────────────────────────────────────────── */

void edc_task_add_turn(struct edc_task *task, struct edc_turn *turn)
{
    if (!task || !turn)
        return;
    edc_queue_push(&task->turns, turn);
}

void edc_task_commit_active_turn(struct edc_task *task)
{
    if (!task || !task->active_turn)
        return;
    /* turn 提交 = 本轮所有工具都已完成 (记录已在响应时删除); 账本清空 */
    edc_task_tool_calls_clear(task);
    edc_queue_push(&task->turns, task->active_turn);
    task->active_turn = NULL;
}

size_t edc_task_message_count(const struct edc_task *task)
{
    if (!task)
        return 0;
    size_t n = 0;
    for (size_t i = task->turns.head; i < task->turns.tail; i++)
    {
        struct edc_turn *turn = (struct edc_turn *)task->turns.data[i];
        if (turn)
            n += edc_queue_length(&turn->messages);
    }
    if (task->active_turn)
        n += edc_queue_length(&task->active_turn->messages);
    return n;
}

/* 把一条 role=="user" 消息的 content 追加到 *out (以 '\n' 连接; *out 为
 * NULL 时新建)。 */
static void append_user_msg(const cJSON *msg, sds *out)
{
    if (!msg || !out)
        return;
    const char *role = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "role"));
    if (!role || strcmp(role, "user") != 0)
        return;
    const char *content = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "content"));
    if (!content)
        return;
    *out = *out ? sdscatprintf(*out, "\n%s", content) : sdsnew(content);
}

/* 收集一个 turn 里的 user 输入（'\n' 连接）。turn 自己持有消息。 */
static void collect_user_msgs(const struct edc_turn *turn, sds *out)
{
    if (!turn || !out)
        return;
    for (size_t j = turn->messages.head; j < turn->messages.tail; j++)
        append_user_msg((const cJSON *)turn->messages.data[j], out);
}

sds edc_task_user_input(const struct edc_task *task, const cJSON *session_messages)
{
    (void)session_messages; /* turn 持有消息, 不再需要 session 消息数组 */
    if (!task)
        return NULL;
    sds out = NULL;
    for (size_t i = task->turns.head; i < task->turns.tail; i++)
        collect_user_msgs((const struct edc_turn *)task->turns.data[i], &out);
    collect_user_msgs(task->active_turn, &out);
    return out;
}

/* ── Todo 同步节流 ────────────────────────────────────────── */

void edc_task_note_todo_sync(struct edc_task *task)
{
    if (!task)
        return;
    task->todo_sync_gap = 0;
}

bool edc_task_llm_request_poke(struct edc_task *task)
{
    if (!task)
        return false;
    task->todo_sync_gap++;
    if (task->todo_sync_gap >= EDC_TODO_SYNC_REMIND_GAP)
    {
        /* 达阈值: 本次请求 append reminder 提醒一次, 清零重计 —— 避免后续
         * 每次请求都重复提醒 (直到模型调用 todowrite 或再次超距) */
        task->todo_sync_gap = 0;
        return true;
    }
    return false;
}

/* ── Token 统计 ────────────────────────────────────────────── */

void edc_task_add_usage(struct edc_task *task, long long prompt_tokens, long long completion_tokens,
                        long long total_tokens, long long cached_tokens)
{
    if (!task)
        return;

    /* 全 0 = provider 没给 usage (或错误响应), 不写任何字段, 保持计数干净 */
    if (prompt_tokens <= 0 && completion_tokens <= 0 && total_tokens <= 0 && cached_tokens <= 0)
        return;

    if (prompt_tokens < 0)
        prompt_tokens = 0;
    if (completion_tokens < 0)
        completion_tokens = 0;
    if (cached_tokens < 0)
        cached_tokens = 0;
    if (total_tokens <= 0)
        total_tokens = prompt_tokens + completion_tokens;

    task->tokens_prompt += prompt_tokens;
    task->tokens_completion += completion_tokens;
    task->tokens_total += total_tokens;
    task->tokens_cached += cached_tokens;
}

/* ── 依赖 (IPC) ────────────────────────────────────────────── */

/*
 * DFS cycle detection: check if 'source' can reach 'target' by following
 * existing dependency edges. If yes, adding target→source would create a
 * cycle, causing permanent deadlock.
 */
#define EDC_TASK_MAX_DEPTH 64

static bool edc_task_can_reach(struct edc_task *from, struct edc_task *to, int depth)
{
    if (depth > EDC_TASK_MAX_DEPTH)
        return false;
    if (from == to)
        return true;

    struct edc_task_dep *dep;
    LL_FOREACH(from->deps, dep)
    {
        if (edc_task_can_reach(dep->source, to, depth + 1))
            return true;
    }
    return false;
}

bool edc_task_add_dependency(struct edc_task *target, struct edc_task *source, edc_task_dep_type type)
{
    if (!target || !source || target == source)
        return false;

    /* 环检测: source 能到达 target 则拒绝 */
    if (edc_task_can_reach(source, target, 0))
        return false;

    /* 检查是否已有相同依赖 */
    struct edc_task_dep *existing;
    LL_FOREACH(target->deps, existing)
    {
        if (existing->source == source)
            return false;
    }

    /* 在 target->deps 中添加节点 */
    struct edc_task_dep *dep_t = g_new0(struct edc_task_dep, 1);
    dep_t->source = source;
    dep_t->target = target;
    dep_t->type = type;
    LL_APPEND(target->deps, dep_t);

    /* 在 source->dependents 中添加对应节点 */
    struct edc_task_dep *dep_s = g_new0(struct edc_task_dep, 1);
    dep_s->source = source;
    dep_s->target = target;
    dep_s->type = type;
    LL_APPEND(source->dependents, dep_s);

    target->deps_total++;

    /* 若 source 已完成，立即标记 dep 为 completed */
    if (edc_task_is_completed(source))
    {
        edc_task_on_dep_completed(target, source);
    }
    /* 若 target 刚获得依赖，进入 WAITING_DEPS。
     * READY 的镜像就是 IDLE; 退休 WAITING_INPUT 后两者同落点, 不再并列。 */
    else if (edc_task_is_ready(target))
    {
        edc_task_mark_waiting_deps(target);
    }

    return true;
}

/* 台账: 连带改动的记账函数 (定义在依赖那一段里, 见下方)。 */
static void task_record_move(struct edc_task *source, struct edc_task *moved);

void edc_task_on_dep_completed(struct edc_task *target, struct edc_task *source)
{
    if (!target || !source)
        return;

    struct edc_task_dep *dep;
    LL_FOREACH(target->deps, dep)
    {
        if (dep->source == source && !dep->completed && !dep->cancelled)
        {
            dep->completed = true;
            target->deps_completed++;

            /* 同时标记 source->dependents 中的对应节点 */
            struct edc_task_dep *sd;
            LL_FOREACH(source->dependents, sd)
            {
                if (sd->target == target && !sd->completed)
                {
                    sd->completed = true;
                    break;
                }
            }
            break;
        }
    }

    if (target->deps_completed >= target->deps_total && edc_task_awaits_deps(target))
    {
        /* 完成 → 依赖者转 READY, 也是**相位改动**: 宿主对每个动了相位的 task 都欠
         * 同一份簿记 (置脏), 而它不在调用者的名单里 (调用者只点了源的名)。与取消 /
         * 失败两条路同构地记一笔债 (重复是幂等的: 相位没真动就不记)。
         *
         * 为什么从前没记: 那时"启动排队的依赖者"走 sched_start_queued_dependents,
         * 顺手用 sched_move_task 置了脏 —— 一处特例替三条路兜着。现在驱动统一归
         * 消息末尾的全表扫描 (它按状态认人, 不认谁把它弄成这样的), 特例没了,
         * 簿记就得由这条不变量自己提供。 */
        edc_task_step step_before = target->step;
        edc_task_life life_before = target->life;
        edc_task_mark_ready(target);
        if (target->step != step_before || target->life != life_before)
            task_record_move(source, target);
    }
}

/* ── 连带改动的回报 (见 edc_task.h) ─────────────────────────
 *
 * 取消/依赖失效会**连带**改动别的 task, 那些 task 不在调用者的名单里, 宿主却对每个
 * 动了相位的 task 都欠同一份簿记。这里记账, 宿主过统一入口时清账 (`edc_task_drain_moves`)。 */

/* 记一笔债务: |moved| 的相位被 |source| 发起的这次操作连带动过了。
 * 债记在**发起者**头上 —— 宿主正要结算的就是它, 清账时顺手带走。 */
static void task_record_move(struct edc_task *source, struct edc_task *moved)
{
    if (!source || !moved)
        return;
    struct edc_task_move_report *r = malloc(sizeof(*r));
    if (!r)
        return; /* 记账失败只丢一次通知, 不改变任何相位 */
    r->task = moved;
    r->next = source->moved_report;
    source->moved_report = r;
}

/* 摘掉 target 对 source 的那条依赖边 (两侧链表都要摘)。用在"JOIN 的源已经失败"上:
 * 这条依赖**永远满足不了**, 留着它, resume 重判依赖时会把 target 又搬回 AWAIT_DEPS
 * —— 那就是永久卡死。target 与**别的**源的边一概不动 (那些源还活着, 产出仍要注入)。
 *
 * 注意这里选择**摘掉**而不是像取消那样把边标成 cancelled: 落盘时 cancelled 标记会被
 * 读回来重放 (`edc_state_serializer.c` 的 `on_dep_cancelled` 那条), 而 JOIN 边的重放
 * 语义是"连带走取消" —— 一个失败(可 resume 的 ERROR)的 task 重启后会变成被取消,
 * 状态在盘上来回变形。边不留痕就不存在这个歧义。 */
static void task_unlink_dep(struct edc_task *target, struct edc_task *source)
{
    if (!target || !source)
        return;

    struct edc_task_dep *dep, *tmp;
    LL_FOREACH_SAFE(target->deps, dep, tmp)
    {
        if (dep->source == source)
        {
            LL_DELETE(target->deps, dep);
            free(dep);
            if (target->deps_total > 0)
                target->deps_total--;
            break;
        }
    }

    struct edc_task_dep *sd, *sd_tmp;
    LL_FOREACH_SAFE(source->dependents, sd, sd_tmp)
    {
        if (sd->target == target)
        {
            LL_DELETE(source->dependents, sd);
            free(sd);
            break;
        }
    }
}

/* 源**终结了、再也不会产出**时, 把 target 对它的那条 SEQUENCE 依赖作废: SEQUENCE 本来
 * 就不要源的产出 (只是"跑在它后面"), 顺序已经走完 —— 抹掉这条边, target 仍可运行。
 * 取消与失败在这一条上语义完全相同 (源是死了还是没产出, 对 SEQUENCE 没区别), 共用这份实现。 */
static void task_void_sequence_dep(struct edc_task_dep *dep, struct edc_task *target, struct edc_task *source)
{
    dep->cancelled = true;

    /* 同时标记 source->dependents 中的对应节点 */
    struct edc_task_dep *sd;
    LL_FOREACH(source->dependents, sd)
    {
        if (sd->target == target && !sd->cancelled)
        {
            sd->cancelled = true;
            break;
        }
    }

    /* 重算: 只计算未取消的依赖 */
    size_t active_deps = 0;
    struct edc_task_dep *d;
    LL_FOREACH(target->deps, d)
    {
        if (!d->cancelled)
            active_deps++;
    }
    target->deps_total = active_deps;

    if (target->deps_total == 0 && edc_task_awaits_deps(target))
    {
        /* 抹掉最后一条失效依赖 → 这个 task 从 AWAIT_DEPS 转 READY, 同样是相位改动
         * (侧栏的依赖等待就解除了), 同样记债。 */
        edc_task_step step_before = target->step;
        edc_task_life life_before = target->life;
        edc_task_mark_ready(target);
        if (target->step != step_before || target->life != life_before)
            task_record_move(source, target);
    }
}

void edc_task_drain_moves(struct edc_task *task, void (*cb)(void *ctx, struct edc_task *moved), void *ctx)
{
    if (!task)
        return;
    /* 取到空为止: 回调里可能**又填进来** (清账本身还会连带改动别的 task)。
     * cb = NULL 时只清空名单 (释放 task 时用)。 */
    while (task->moved_report)
    {
        struct edc_task_move_report *r = task->moved_report;
        task->moved_report = r->next;
        if (cb)
            cb(ctx, r->task);
        free(r);
    }
}

void edc_task_on_dep_cancelled(struct edc_task *target, struct edc_task *source)
{
    if (!target || !source)
        return;

    /* 找到这条尚在等待的依赖，按类型决定语义 (完成/已取消/不存在 → 无事可做) */
    struct edc_task_dep *dep = NULL;
    LL_FOREACH(target->deps, dep)
    {
        if (dep->source == source && !dep->completed && !dep->cancelled)
            break;
    }
    if (!dep)
        return;

    if (dep->type == TASK_DEP_JOIN)
    {
        /* JOIN: 源的 output 永远不会来了 —— 这条依赖无法满足，本 task 也就
         * 无法作为完整输入运行，于是级联取消。edc_task_send_signal 会递归
         * 取消本 task 自己的依赖者，并清空它的 deps；因此 dep 随后即失效，
         * 必须立即 return，不能再解引用。 */
        edc_task_step step_before = target->step;
        edc_task_life life_before = target->life;
        edc_task_send_signal(target, TASK_SIGTERM);
        /* 级联取消也是**相位改动** —— 这个 task 从此进终态, 宿主却只点了源 task 的
         * 名。记一笔债 (重复取消是幂等的: 相位没真动就不记)。 */
        if (target->step != step_before || target->life != life_before)
            task_record_move(source, target);
        return;
    }

    /* SEQUENCE(`;`): 只是顺序约束，不需要源 output —— 抹去这条依赖即可，
     * 本 task 仍可运行。 */
    task_void_sequence_dep(dep, target, source);
}

/* 源**失败**时对依赖者的处置 —— 与取消同构 (源都不会再产出了), 只是终态不同:
 * 取消是"人让它停", 失败是"它跑不成"; 而 ERROR 与 CANCELLED 都在 `/task resume`
 * 接受的三类里, 所以两种终态都给了用户出路。 */
void edc_task_on_dep_failed(struct edc_task *target, struct edc_task *source)
{
    if (!target || !source)
        return;

    /* 找到这条尚在等待的依赖，按类型决定语义 (完成/已作废/不存在 → 无事可做) */
    struct edc_task_dep *dep = NULL;
    LL_FOREACH(target->deps, dep)
    {
        if (dep->source == source && !dep->completed && !dep->cancelled)
            break;
    }
    if (!dep)
        return;

    if (dep->type == TASK_DEP_JOIN)
    {
        /* JOIN: 源的 output 永远不会来了 —— 这条依赖无法满足, 本 task 也就无法作为
         * 完整输入运行, 于是级联失败。edc_task_mark_failed 会递归广播给 target 自己的
         * 依赖者; 摘边必须**先**做 (见 task_unlink_dep 的注释: 不摘就 resume 不回来)。 */
        edc_task_step step_before = target->step;
        edc_task_life life_before = target->life;

        /* 失败的原因要说得出来。executor 那条路上带来的是 LLM 的错误文案, 这条路上
         * 什么都没有 —— 把"谁的产出再也来不了"写进 output (失败任务的 output 就是它的
         * 失败原因, 与 executor 那条路一致), 宿主结算时把它当通知发给用户。留 NULL 会
         * 让宿主退回一句通用的 "LLM request failed" —— 对一个**从没发出过请求**的
         * 依赖者是假话。 */
        if (source->task_id)
            edc_task_set_output(target, sdscatprintf(sdsempty(), "[failed: its dependency %s failed, so the output "
                                                                 "it was waiting for will never arrive]",
                                                     source->task_id));
        else
            edc_task_set_output(target, sdsnew("[failed: a dependency failed, so the output it was waiting for will "
                                               "never arrive]"));

        task_unlink_dep(target, source);
        edc_task_mark_failed(target);
        /* 级联失败也是**相位改动** —— 这个 task 从此进终态, 宿主却只点了源 task 的
         * 名。记一笔债 (重复失败是幂等的: 相位没真动就不记)。 */
        if (target->step != step_before || target->life != life_before)
            task_record_move(source, target);
        return;
    }

    /* SEQUENCE(`;`): 与取消那条路逐字同构 —— 源死在前头, 顺序已走完, 抹边放行。 */
    task_void_sequence_dep(dep, target, source);
}

/* 失败广播: 与 edc_task_send_signal 里取消的广播同一手法 —— 先把依赖者快照成数组
 * 再逐个处理, 因为 JOIN 的级联失败会在回调里递归摘掉 `dependents` 上的节点 (更深一层
 * 的 `task_unlink_dep` 会改这条链表, LL_FOREACH_SAFE 也救不了)。级联不释放 task 本身
 * (只释放 dep 节点), 所以暂存裸指针是安全的。 */
static void task_notify_dependents_failed(struct edc_task *task)
{
    if (!task)
        return;

    size_t n = 0;
    struct edc_task_dep *dep;
    LL_FOREACH(task->dependents, dep)
    n++;
    if (n == 0)
        return;

    struct edc_task **targets = malloc(n * sizeof(*targets));
    if (!targets)
        return; /* 快照失败只丢一次广播, 不改变任何相位 */
    size_t i = 0;
    LL_FOREACH(task->dependents, dep)
    targets[i++] = dep->target;
    for (size_t k = 0; k < i; k++)
        edc_task_on_dep_failed(targets[k], task);
    free(targets);
}

void edc_task_notify_dependents(struct edc_task *task)
{
    if (!task)
        return;

    struct edc_task_dep *dep, *tmp;
    LL_FOREACH_SAFE(task->dependents, dep, tmp)
    {
        if (!dep->cancelled && !dep->completed)
        {
            dep->completed = true;
            edc_task_on_dep_completed(dep->target, task);
        }
    }
}

/* ── 信号 ──────────────────────────────────────────────────── */

bool edc_task_is_running(const struct edc_task *task)
{
    /* 正跑着 = ALIVE 且在等 executor/工具的答复。挂起在等工具的任务是
     * step=AWAIT_TOOLS && life=SUSPENDED —— 不算 running (旧词汇里它只显示成
     * SUSPENDED, 这里必须一致)。 */
    return edc_task_awaits_exec(task) || edc_task_awaits_tools(task);
}

void edc_task_send_signal(struct edc_task *task, edc_task_signal sig)
{
    if (!task)
        return;

    task->cancelled_request = 1;

    if (sig == TASK_SIGTERM)
    {
        /* 先把依赖者快照成数组再逐个处理: JOIN 的级联取消会在回调里递归清理
         * 各 task 的 deps, 因而把节点从 task->dependents 上摘掉 —— 边遍历边
         * 改这条链表, LL_FOREACH_SAFE 也救不了 (它只缓存当前节点的 next,
         * 而 next 本身可能被更深的递归 freeing)。取消过程不会释放 task 本身
         * (只释放 dep 节点), 所以暂存裸指针是安全的。 */
        size_t n = 0;
        struct edc_task_dep *dep;
        LL_FOREACH(task->dependents, dep)
        n++;
        if (n > 0)
        {
            struct edc_task **targets = malloc(n * sizeof(*targets));
            if (targets)
            {
                size_t i = 0;
                LL_FOREACH(task->dependents, dep)
                targets[i++] = dep->target;
                for (size_t k = 0; k < i; k++)
                    edc_task_on_dep_cancelled(targets[k], task);
                free(targets);
            }
        }
    }

    /* 移除自己的 deps (不再等待别人) */
    {
        struct edc_task_dep *dep, *tmp;
        LL_FOREACH_SAFE(task->deps, dep, tmp)
        {
            if (dep->source)
            {
                struct edc_task_dep *sd, *sd_tmp;
                LL_FOREACH_SAFE(dep->source->dependents, sd, sd_tmp)
                {
                    if (sd->target == task)
                    {
                        LL_DELETE(dep->source->dependents, sd);
                        free(sd);
                    }
                }
            }
            LL_DELETE(task->deps, dep);
            free(dep);
        }
        task->deps = NULL;
        task->deps_total = 0;
    }

    /* 任务被取消: 在途工具追账已无意义, 丢弃账本 */
    edc_task_tool_calls_clear(task);
    edc_task_mark_cancelled(task);
}

/* ── 未完成工具调用表 (resume 账本) ────────────────────────── */

static void edc_task_tool_call_record_free(struct edc_tool_call_record *rec)
{
    if (!rec)
        return;
    if (rec->call_id)
        sdsfree(rec->call_id);
    if (rec->name)
        sdsfree(rec->name);
    if (rec->args)
        cJSON_Delete(rec->args);
    free(rec);
}

struct edc_tool_call_record *edc_task_tool_call_add(struct edc_task *task, const char *call_id, const char *name,
                                                    cJSON *args)
{
    if (!task)
        return NULL;
    struct edc_tool_call_record *rec = g_new0(struct edc_tool_call_record, 1);
    rec->call_id = call_id ? sdsnew(call_id) : NULL;
    rec->name = name ? sdsnew(name) : NULL;
    rec->args = args ? cJSON_Duplicate(args, 1) : NULL;
    rec->status = TOOL_CALL_PENDING;
    LL_APPEND(task->tool_calls, rec);
    return rec;
}

struct edc_tool_call_record *edc_task_tool_call_find(struct edc_task *task, const char *call_id)
{
    if (!task || !call_id)
        return NULL;
    struct edc_tool_call_record *rec;
    LL_FOREACH(task->tool_calls, rec)
    {
        if (rec->call_id && strcmp(rec->call_id, call_id) == 0)
            return rec;
    }
    return NULL;
}

struct edc_tool_call_record *edc_task_tool_call_mark_executing(struct edc_task *task, const char *call_id)
{
    struct edc_tool_call_record *rec = edc_task_tool_call_find(task, call_id);
    if (rec)
        rec->status = TOOL_CALL_EXECUTING;
    return rec;
}

bool edc_task_tool_call_remove(struct edc_task *task, const char *call_id)
{
    if (!task || !call_id)
        return false;
    struct edc_tool_call_record *rec, *tmp;
    LL_FOREACH_SAFE(task->tool_calls, rec, tmp)
    {
        if (rec->call_id && strcmp(rec->call_id, call_id) == 0)
        {
            LL_DELETE(task->tool_calls, rec);
            edc_task_tool_call_record_free(rec);
            return true;
        }
    }
    return false;
}

void edc_task_tool_calls_clear(struct edc_task *task)
{
    if (!task)
        return;
    struct edc_tool_call_record *rec, *tmp;
    LL_FOREACH_SAFE(task->tool_calls, rec, tmp)
    {
        LL_DELETE(task->tool_calls, rec);
        edc_task_tool_call_record_free(rec);
    }
    task->tool_calls = NULL;
}

bool edc_task_has_pending_tool_calls(const struct edc_task *task)
{
    return task && task->tool_calls != NULL;
}

/* ── 悬空 tool_call 的修补 (驱动前置) ────────────────────────
 *
 * 见 edc_task.h 的声明处: 为什么按消息而不是按账本、为什么归 task 模块。
 * 幂等: 已有 tool 响应的 call_id 跳过 (重复驱动不会补第二条)。 */

static const char *const kInterruptedToolNote =
    "[interrupted: this tool was still running when the process restarted; it did not produce a result]";

static bool task_turn_has_tool_response(struct edc_turn *turn, size_t upto, const char *call_id)
{
    for (size_t i = 0; i < upto; i++)
    {
        cJSON *m = turn->messages.data[i];
        cJSON *role = cJSON_GetObjectItem(m, "role");
        cJSON *tcid = cJSON_GetObjectItem(m, "tool_call_id");
        if (role && cJSON_IsString(role) && strcmp(role->valuestring, "tool") == 0 && tcid && cJSON_IsString(tcid) &&
            strcmp(tcid->valuestring, call_id) == 0)
            return true;
    }
    return false;
}

void edc_task_repair_dangling_tool_calls(struct edc_task *task)
{
    struct edc_turn *turn = (task && task->active_turn) ? task->active_turn : NULL;
    if (!turn)
        return;

    /* 只扫**修补之前**的既有消息: 补进去的 tool 消息不参与遍历。队列会因追加而
     * realloc, 所以 base 指针每轮重取 (不能缓存数组首地址)。 */
    size_t n = turn->messages.tail;
    for (size_t i = 0; i < n; i++)
    {
        cJSON *m = turn->messages.data[i];
        cJSON *role = cJSON_GetObjectItem(m, "role");
        cJSON *tcs = cJSON_GetObjectItem(m, "tool_calls");
        if (!role || !cJSON_IsString(role) || strcmp(role->valuestring, "assistant") != 0 || !tcs ||
            !cJSON_IsArray(tcs))
            continue;

        cJSON *tc = NULL;
        cJSON_ArrayForEach(tc, tcs)
        {
            cJSON *id = cJSON_GetObjectItem(tc, "id");
            if (!id || !cJSON_IsString(id) || !id->valuestring || !*id->valuestring)
                continue;

            const struct edc_tool_call_record *rec = edc_task_tool_call_find(task, id->valuestring);
            if (rec && rec->status != TOOL_CALL_EXECUTING)
                continue; /* PENDING: 交给驱动段真派发, 不要造假响应 */
            if (task_turn_has_tool_response(turn, n, id->valuestring))
                continue; /* 已经有结果了 (幂等) */

            cJSON *fn = cJSON_GetObjectItem(tc, "function");
            cJSON *nm = fn ? cJSON_GetObjectItem(fn, "name") : NULL;
            edc_turn_add_tool_message(turn, id->valuestring, (nm && cJSON_IsString(nm)) ? nm->valuestring : "",
                                      kInterruptedToolNote);
            /* 账本里若还留着这条 (SUSPENDED / ERROR 路径: EXECUTING 已派发、结果未回),
             * 一并删掉 —— 它已经有响应了, 不该再被当成未完成。 */
            edc_task_tool_call_remove(task, id->valuestring);
        }
    }
}

/* ── 输出 ──────────────────────────────────────────────────── */

void edc_task_set_output(struct edc_task *task, sds output)
{
    if (!task)
        return;
    if (task->output)
        sdsfree(task->output);
    task->output = output;
}

sds edc_task_get_output(struct edc_task *task)
{
    if (!task)
        return NULL;
    return task->output;
}

void edc_task_set_completion_seq(struct edc_task *task, uint32_t seq)
{
    if (!task)
        return;
    task->completion_seq = seq;
}

/* ── 折叠 ────────────────────────────────────────────────── */

/* 可折叠 = 已终结、且还没折过、且**这个任务是要留下来的** (PERSISTENT)。
 *
 * 第三条来自 lifetime, 是 P0b 把一个真 bug 逼出来的: 折叠任务自己也是一个
 * "COMPLETED && !turns_folded" 的任务, 它一完成就会触发下一轮触发检查 ——
 * 于是系统组 (成员全是 TRANSIENT) 也会被当成可折叠对象, 折出自己的 prompt
 * 和摘要 (实测夹具里立刻多出一次 LLM 请求)。
 *
 * 规则本身很直白: **折叠是为"活过重启"的那部分历史做压缩** (它决定重启后还剩
 * 多少细节), 所以一个"不落盘、完成即回收"的任务根本不该进这个账 —— 它没有
 * 需要被压缩的未来。于是 lifetime 在这里再读一次: 不是特判隐藏, 而是同一条
 * "它不留下来"的规则在折叠口径上的必然结论。 */
bool edc_task_is_foldable(struct edc_task *task)
{
    return !edc_task_is_transient(task) && edc_task_is_completed(task) && !task->turns_folded;
}

size_t edc_task_turn_msg_count(struct edc_task *task)
{
    if (!task)
        return 0;

    size_t n = 0;
    for (size_t qi = task->turns.head; qi < task->turns.tail; qi++)
    {
        struct edc_turn *turn = task->turns.data[qi];
        n += edc_queue_length(&turn->messages);
    }
    return n;
}

/* ── Prompt ────────────────────────────────────────────────── */

void edc_task_set_prompt(struct edc_task *task, const char *prompt_name, const char *prompt_text)
{
    if (!task)
        return;

    if (task->prompt_name)
    {
        sdsfree(task->prompt_name);
        task->prompt_name = NULL;
    }
    if (task->prompt)
    {
        sdsfree(task->prompt);
        task->prompt = NULL;
    }

    if (prompt_name)
        task->prompt_name = sdsnew(prompt_name);
    if (prompt_text)
        task->prompt = sdsnew(prompt_text);
}

/* ── 上下文构建 ────────────────────────────────────────────── */

cJSON *edc_task_get_context_messages(struct edc_task *task, struct edc_config *config)
{
    if (!task)
        return cJSON_CreateArray();

    cJSON *messages = cJSON_CreateArray();

    /* 1. System prompt */
    if (task->prompt)
    {
        cJSON *sys_msg = cJSON_CreateObject();
        cJSON_AddStringToObject(sys_msg, "role", "system");
        cJSON_AddStringToObject(sys_msg, "content", task->prompt);
        cJSON_AddItemToArray(messages, sys_msg);
    }
    else if (config)
    {
        sds default_prompt = edc_context_build_system_prompt(config, NULL);
        cJSON *sys_msg = cJSON_CreateObject();
        cJSON_AddStringToObject(sys_msg, "role", "system");
        cJSON_AddStringToObject(sys_msg, "content", default_prompt);
        cJSON_AddItemToArray(messages, sys_msg);
        sdsfree(default_prompt);
    }

    /* 2. 已完成的 turn 消息 */
    for (size_t qi = task->turns.head; qi < task->turns.tail; qi++)
    {
        struct edc_turn *turn = task->turns.data[qi];
        for (size_t mj = turn->messages.head; mj < turn->messages.tail; mj++)
        {
            cJSON *item = turn->messages.data[mj];
            cJSON_AddItemReferenceToArray(messages, item);
        }
    }

    /* 3. 已完成依赖 task 的 output 注入 */
    struct edc_task_dep *dep;
    LL_FOREACH(task->deps, dep)
    {
        if (dep->completed && dep->source->output)
        {
            sds note = sdscatprintf(sdsempty(), "[子任务 %s 结果]:\n%s", dep->source->description, dep->source->output);
            cJSON *sys = cJSON_CreateObject();
            cJSON_AddStringToObject(sys, "role", "system");
            cJSON_AddStringToObject(sys, "content", note);
            cJSON_AddItemToArray(messages, sys);
            sdsfree(note);
        }
    }

    return messages;
}

/* ── 关系 / 出站 (见 edc_task.h) ───────────────────────────── */

struct edc_session *edc_task_session(const struct edc_task *task)
{
    if (!task || !task->group)
        return NULL;
    return task->group->session;
}

void edc_task_flush_redirects(struct edc_task *task)
{
    if (!task)
        return;

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
