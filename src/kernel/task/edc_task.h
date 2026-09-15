#ifndef __BC_TASK_H__
#define __BC_TASK_H__

#include "edc_config.h"
#include "edc_containers.h"
#include "edc_task_types.h"
#include "edc_turn.h"
#include "sds.h"
#include <cJSON.h>
#include <stdbool.h>
#include <stddef.h>

struct edc_session;
struct edc_config;
struct edc_task;

/* ── Task 状态 ───────────────────────────────────────────────
 *
 * 旧枚举 `edc_task_state` (TASK_IDLE / TASK_WAITING_INPUT / TASK_SUSPENDED / …)
 * 已随过渡桥一起退休 (R3b)。现在: **"在哪一步"是 step, "活着吗"是 life**,
 * 判断走下面那组谓词, 文案走 `edc_task_state_str` (它仍返回旧名字 —— 词表冻结,
 * 但那只是**字符串**, 不再是类型)。见 docs/retire-waiting-input.md §10.5。 */

/* ── step / life: 把"在哪一步"和"活着吗"拆开 ──────────────────
 *
 * runtime-model.md §1.4: 这两件事今天挤在 edc_task_state **一个** enum 里, 于是
 * 落盘时只能靠**粗化**保存 —— 一个正在等工具结果的 task 和一个刚建好还没跑的
 * task, 存下来变成同一个 SUSPENDED, 相位就丢了。resume 只能靠残留物 (工具账本 /
 * active_turn) 顺序探测把相位猜回来。
 *
 * 拆开之后: step 如实落盘、如实读回; life 回答"还活着吗" —— 重启后没有任何东西
 * 在跑, 于是 life 统一成 SUSPENDED, 但**不再动 step**。这是这次改动的全部要点。
 *
 * 两者正交: (step=DONE, life=COMPLETED) 与 (step=AWAIT_EXEC, life=SUSPENDED) 都合法。 */
typedef enum
{
    EDC_TASKSTEP_READY,          /* 就绪, 可以开始 (旧名 AWAIT_INPUT 也归到这里) */
    EDC_TASKSTEP_AWAIT_DEPS,     /* 在等依赖 */
    EDC_TASKSTEP_AWAIT_EXEC,     /* 在等 LLM (executor) 回话 */
    EDC_TASKSTEP_AWAIT_TOOLS,    /* 在等工具结果 */
    EDC_TASKSTEP_AWAIT_RESOURCE, /* 在等资源锁 (P1 起预留) */
    EDC_TASKSTEP_DONE,           /* 这一步走完了 */
} edc_task_step;

typedef enum
{
    EDC_LIFE_ALIVE,     /* 活着 */
    EDC_LIFE_SUSPENDED, /* 挂起: 用户 Ctrl+Z, 或重启后没有东西在跑 */
    EDC_LIFE_COMPLETED,
    EDC_LIFE_CANCELLED,
    EDC_LIFE_ERROR,
} edc_task_life;

extern const char *edc_task_step_name(edc_task_step step);
extern const char *edc_task_life_name(edc_task_life life);
extern edc_task_step edc_task_classify_step(const char *name);
extern edc_task_life edc_task_classify_life(const char *name);

/* ── sink / lifetime: 与 step、life 正交的两件事 ──────────────
 *
 * runtime-model.md §1.4 把这件事说得很直白: `step` / `life` / `sink` / `lifetime`
 * / `kind` 是**五个正交互质的**, 回答五个不同问题 —— 在哪 / 活着吗 / 结果给谁 /
 * 要不要活过重启 / 属于哪一类(展示)。它们各有**唯一**的读者, 所以谁都不许拿别人
 * 的字段做分支 (core 尤其不许; 那是老 `is_system` 的死因)。
 *
 * 这五个名字过去被**挤进 `edc_task_state` 一个 enum**。step/life 已经拆出来了
 * (P2/R3b); sink/lifetime 在这里补上 —— 它们是为 **system task** 准备的:
 * fold 是一个"把产出写进 group 记忆、活不过重启"的任务, 以前这个角色由一条
 * 专用协议 (`BC_ACTOR_MSG_GROUP_FOLD` / `_DONE`) 加一个专用 actor 扮演。 */

/* 产出去向。读者: executor (决定结果送到哪 / 占不占完成序号) 与**镜像装配**
 * ("这次调用带不带工具定义" —— 那是镜像装配自己的问题, 也是规则 6 指定它唯一
 * 能认识工具的地方)。**不是"唯一读者", 而是"同一条规则的必然推论"** ——
 * 判据见 runtime-model.md §5 规则 7: 拥有权与依赖方向, 不是读者数。 */
typedef enum
{
    EDC_SINK_CHANNEL,      /* 产出回给用户 —— 用户任务的默认 */
    EDC_SINK_GROUP_MEMORY, /* 产出交给所属 group 的记忆 —— fold 用 */
} edc_sink;

/* 是否需要活过重启。当前 4 处读者都是同一条规则的推论 (见 runtime-model.md §5
 * 规则 7): ①落盘 (edc_state_serializer.c) ②完成即回收 (scheduler 的回收队列 ——
 * 生命周期策略的家就在这里) ③不进折叠账 (edc_task_is_foldable) ④不顶折叠阈值
 * (edc_task_group.c)。**core 不得据此隐藏/跳过任何任务**。 */
typedef enum
{
    EDC_LIFETIME_PERSISTENT, /* 落盘, 重启后还在 —— 用户任务的默认 */
    EDC_LIFETIME_TRANSIENT,  /* 不落盘、完成即回收 —— system task */
} edc_lifetime;

extern const char *edc_task_sink_name(edc_sink sink);
extern edc_sink edc_task_classify_sink(const char *name);
extern const char *edc_task_lifetime_name(edc_lifetime lifetime);
extern edc_lifetime edc_task_classify_lifetime(const char *name);

/** 是否是"不落盘、完成即回收"的 system task (只看 lifetime)。 */
extern bool edc_task_is_transient(const struct edc_task *task);


/* ── 信号 ──────────────────────────────────────────────────── */

typedef enum
{
    TASK_SIGTERM, /* 优雅取消：取消 in-flight 请求，通知依赖者 */
    TASK_SIGKILL, /* 强制取消：不通知依赖者，deps_total 不减 */
} edc_task_signal;

/* ── Task 间依赖 ─────────────────────────────────────────────
 * edc_task_dep_type 定义在 core/edc_task_types.h 中 */

struct edc_task_dep
{
    struct edc_task *source; /* 依赖的源 task */
    struct edc_task *target; /* 被依赖的目标 task (this) */
    edc_task_dep_type type;
    bool completed;            /* 已完成 */
    bool cancelled;            /* 已取消 */
    bool injected;             /* 已注入到 LLM 上下文 */
    struct edc_task_dep *next; /* 单链表 (在 target 的 deps 链上) */
};

/* ── Tool 调用跟踪 (resume 账本) ─────────────────────────────
 * 只记录"尚未执行完"的工具调用, 供进程中断后 resume 判定:
 *   - PENDING   : 尚未派发给工具箱 → 重新执行;
 *   - EXECUTING : 已派发、结果未回 → 补一条"中途被中断"的响应。
 * 工具一旦返回结果即从表中删除 —— 表只跟踪未完成的工具。
 * 链序即执行序 (toolbox 顺序执行), 故任一时刻至多一条头部为 EXECUTING。 */

typedef enum
{
    TOOL_CALL_PENDING = 0,   /* 未派发 */
    TOOL_CALL_EXECUTING = 1, /* 已派发, 结果未回 */
} edc_tool_call_status;

struct edc_tool_call_record
{
    sds call_id; /* LLM 侧 tool_call_id, 用于响应回填/查表 */
    sds name;    /* 工具名 */
    cJSON *args; /* 调用参数 (重新执行时重建 req 用, 可为 NULL) */
    edc_tool_call_status status;
    struct edc_tool_call_record *next; /* 单链表, 顺序即派发顺序 */
};

/* ── Task ──────────────────────────────────────────────────── */

struct edc_task
{
    char *task_id;    /* PID — 唯一标识 */
    char *group_id;   /* PGID — 所属 group (leader 的 task_id) */
    char *session_id; /* SID — 所属 session */

    char *description;    /* 用户可见描述 */
    /* 在哪一步 / 活着吗 —— 拆开之后相位才能如实保存, 且落盘时不再被粗化。
     *
     * **这里是"任务在哪一步"的唯一来源** (runtime-model.md §8 判据 4)。过渡期那层
     * `edc_task_state state` 镜像已在 R3a 删掉: 它曾经只是"让 60 多处旧读法一行不用改"
     * 的脚手架, 而读侧与转移表都已改以 (step, life) 为键 (见 docs/retire-waiting-input.md
     * §10)。旧词表仍在, 但只作**显示名** (`edc_task_state_str`) 与过渡桥的输入,
     * 由 (step, life) 派生 —— 不再有第二份副本可以漂移。 */
    edc_task_step step;
    edc_task_life life;

    /* 与上面两个同理, 但回答的是另外两个问题 (见本文档顶部那两个 enum):
     * 产出给谁 (sink) / 要不要活过重启 (lifetime)。默认 (CHANNEL, PERSISTENT)
     * 就是用户任务; 只有 system task 会把它们改成 (GROUP_MEMORY, TRANSIENT),
     * 而那个组合的全部意义是: **fold 不需要专用协议, 也不需要隐藏**。 */
    edc_sink sink;
    edc_lifetime lifetime;

    int is_group_leader; /* 是否是 group leader */
    int auto_reap;       /* 完成后自动回收 (前台输入任务) */

    struct edc_task_group *group; /* 所属 group */

    EdcQueue turns;               /* FIFO: struct edc_turn* */
    struct edc_turn *active_turn; /* 当前活跃 turn */

    void *current_request_id; /* 发往 actor 的 request_id */
    int cancelled_request;    /* 标记请求已取消 */

    /* ── max_iter 迭代预算 ──
     * llm_rounds: 当前 turn (一次用户输入) 已发出的 LLM 请求数 —— 调度器
     * 每次调用 send_llm 时加一 (入口输入、依赖排队的自动触发、工具链续发)。
     * 工具链结束后若达到 config->max_iter 上限 (max_iter > 0), 调度器不再
     * 续发 LLM, 以收尾消息终结本轮, 防止模型在工具循环里空转烧预算。
     * max_iter <= 0 (含 -1 = 无限, 默认) 表示不限制。新一轮输入开始时
     * 清零 (scheduler)。 */
    int llm_rounds;

    /* ── Token 消耗统计 ──
     * 该 task 生命周期内所有 LLM 响应的 usage 累加值。一次用户输入可能触发
     * 多轮请求 (工具链续发、依赖触发), 同 turn 内每次 API 调用的 usage 都
     * 计入本 task; 跨 turn 持续累加, 直到 task 被回收。纯本地统计, 用于
     * 侧边栏按 task 展示 token 消耗, 不参与调度决策。 */
    long long tokens_prompt;     /* 累计 prompt tokens */
    long long tokens_completion; /* 累计 completion tokens */
    long long tokens_total;      /* 累计 total tokens */
    long long tokens_cached;     /* 累计命中 provider 缓存的 prompt tokens */

    edc_channel_type channel_type;

    cJSON *todos; /* JSON Array */

    /* ── Todo 同步节流 ──
     * todo_sync_gap: 距上次 todowrite 成功调用以来的 LLM 请求数
     * (每次为该 task 组装一次 LLM 请求即 +1, 见 edc_task_llm_request_poke)。
     * 达 EDC_TODO_SYNC_REMIND_GAP 仍未同步 → 该次请求队尾 append 一条
     * system reminder 提醒调用 todowrite, 并清零重新计数 —— 避免
     * 每轮请求都带 reminder 的唠叨, 也保证模型长时间闷头干活不更新清单时
     * 能被周期性唤醒。 */
    int todo_sync_gap; /* >= EDC_TODO_SYNC_REMIND_GAP 时触发提醒 (达阈值自动清零) */

    /* ── Fork 上下文继承 ── */
    struct edc_task *forked_from; /* 父 task (NULL = 非 fork) */
    uint32_t fork_point_completion_seq; /* fork 点: 父组的提交序水位 (仅元数据, 上下文切片由合成 turn 快照负责) */

    /* ── Subagent / Prompt 绑定 ── */
    sds prompt;      /* 自定义 system prompt (NULL = 用默认) */
    sds prompt_name; /* prompt 名称 (如 "code-reviewer") */

    /* ── Task IPC ── */
    struct edc_task_dep *deps;       /* 我依赖谁 */
    struct edc_task_dep *dependents; /* 谁依赖我 */
    size_t deps_total;               /* 总依赖数 */
    size_t deps_completed;           /* 已完成依赖数 */
    sds output;                      /* 本 task 最终输出 (供 JOIN 注入) */

    uint32_t completion_seq; /* 完成顺序 (0=未完成) */
    int has_deferred_vars;   /* $VAR 引用待展开 */
    bool turns_folded;       /* turns 已折叠进 group memory (摘要) */

    /* ── 未完成工具调用 (resume 账本) ── */
    struct edc_tool_call_record *tool_calls;

    /* ── 输出重定向 ── */
    sds redirect_stdout;  /* > / >> file */
    sds redirect_stderr;  /* 2> file */
    bool redirect_append; /* true = >>, false = > */

    /* ── 产出目标 (sink = GROUP_MEMORY 的 system task 用) ──
     * fold 任务的产出不是一条回话, 而是**某个组的记忆**: 写进哪个组、本轮折到
     * 哪个水位 (completion_seq 上限, 与折叠 prompt 里选中的那批任务一致)。
     * 两者都由触发折叠的一侧填好 (见 edc_group_fold.c); 任务自己不解释它们,
     * executor 只转交 (ops->deliver_group_memory), 落地在 scheduler。
     * TRANSIENT 任务不落盘, 所以这两个字段按 id 存没有必要 —— 是纯内存的。 */
    struct edc_task_group *sink_group;
    uint32_t fold_max_seq;

    /* ── 链表 ── */
    struct edc_task *group_next;  /* group 内下一成员 */
    struct edc_task *prev, *next; /* session 全局链表 */
};

/* ── 生命周期 ──────────────────────────────────────────────── */

extern struct edc_task *edc_task_new(const char *task_id, const char *session_id, const char *group_id,
                                     const char *desc, edc_channel_type channel_type);
extern struct edc_task *edc_task_fork(struct edc_task *parent, const char *task_id, const char *desc,
                                      uint32_t fork_point_completion_seq, edc_channel_type channel_type);
extern void edc_task_free(struct edc_task *task);

/* ── fork 快照 ────────────────────────────────────────────── */

/** 深拷贝 task 的全部 turn 消息到一个合成快照 turn。
 *  兼容两种态: 未 flush (消息在 turn->messages) 与已 flush (消息在 session->messages,
 *  用 recorded msg_range 定位)。返回 NULL 表示 task 无消息; 调用方负责 edc_turn_free。
 *  用于 fork 规则 2: 当 parent 的 turns 已被 L1 折叠时, 捞回 parent 自己的 turns 全文。 */
extern struct edc_turn *edc_task_snapshot_turns(struct edc_task *task, struct edc_session *session,
                                                const char *snapshot_task_id);

/* ── 状态机: 意图命名的写入口 ────────────────────────────────
 *
 * 一次转移同时说清"到哪一步"和"还活着吗"; 调用方说**意图**, 不拼目标态, 状态机以
 * (step, life) 为键校验。返回 false = 该转移不合法 (被状态机拒绝)。
 *
 * 它们取代了 `edc_task_set_state(task, TASK_X)`: 旧写法把两个维度藏在一个 enum
 * 值里, 挂起更是**说不出**目标 step (TASK_SUSPENDED 只动 life)。 */
extern bool edc_task_await_exec(struct edc_task *task);        /* 已发出 LLM 请求 */
extern bool edc_task_await_tools(struct edc_task *task);       /* 已发出工具调用 */
extern bool edc_task_mark_ready(struct edc_task *task);        /* 回就绪 (依赖齐了 / resume) */
extern bool edc_task_mark_waiting_deps(struct edc_task *task); /* 被依赖挡住, 不派发 */
extern bool edc_task_mark_completed(struct edc_task *task);
extern bool edc_task_mark_failed(struct edc_task *task);
extern bool edc_task_mark_cancelled(struct edc_task *task);
extern bool edc_task_mark_suspended(struct edc_task *task); /* 只动 life: 停在哪一步就停在哪一步 */

/* 低层原语: 直接说目标态 (校验 / 不校验)。一般调用方用上面的意图函数;
 * 说目标态的是 scheduler 的统一入口与 executor 的 port。
 * `force` 版本**不查转移表** —— 兜底出口要能把任务从死胡同里拽出来。 */
extern bool edc_task_move(struct edc_task *task, edc_task_step step, edc_task_life life);
extern void edc_task_force_move(struct edc_task *task, edc_task_step step, edc_task_life life);

/* ── 过渡桥: 已退休 (R3b) ────────────────────────────────────
 *
 * `edc_task_set_state(task, TASK_X)` / `edc_task_force_state(task, TASK_X)` 与
 * `edc_task_state_of` / `edc_task_classify_state` 都删了 —— 说目标态就是直接说
 * (step, life), 中间不再经一层把两个维度折叠起来的词表。 */

/* ── 读侧判定: 直接问 (step, life) ──────────────────────────
 *
 * 旧读法 `t->state == TASK_X` 的每一处, 问的都是下面这些问题之一 —— 换成谓词
 * 之后, 删除过渡镜像时这些调用点**一行都不用再改** (镜像一删, 它们照旧工作)。
 * 这就是"读侧迁移"这件事本身。
 *
 * 每条都带 life 条件, 因为 SUSPENDED **不动 step** (那正是 P2 要保住的东西):
 * 一个"挂起在等工具"的任务是 step=AWAIT_TOOLS && life=SUSPENDED —— 它**不是**
 * running、也不是 awaits_tools。旧词汇里没有"在哪一步"这个维度, 它只显示成
 * SUSPENDED; 谓词必须复现这一条, 否则挂起的任务会被误当成在跑。 */
static inline bool edc_task_is_alive(const struct edc_task *task) /* 还可能跑: 既没完成/取消/出错, 也没被挂起 */
{
    return task && task->life == EDC_LIFE_ALIVE;
}
static inline bool edc_task_is_ready(const struct edc_task *task) /* 带着 input 停着, 等着被派发 */
{
    return task && task->life == EDC_LIFE_ALIVE && task->step == EDC_TASKSTEP_READY;
}
static inline bool edc_task_awaits_deps(const struct edc_task *task) /* 被依赖挡住, 不派发 */
{
    return task && task->life == EDC_LIFE_ALIVE && task->step == EDC_TASKSTEP_AWAIT_DEPS;
}
static inline bool edc_task_is_completed(const struct edc_task *task)
{
    return task && task->life == EDC_LIFE_COMPLETED;
}
static inline bool edc_task_is_cancelled(const struct edc_task *task)
{
    return task && task->life == EDC_LIFE_CANCELLED;
}
static inline bool edc_task_has_failed(const struct edc_task *task)
{
    return task && task->life == EDC_LIFE_ERROR;
}
static inline bool edc_task_is_suspended(const struct edc_task *task)
{
    return task && task->life == EDC_LIFE_SUSPENDED;
}
/* 两个"在等对方回话"的相位: 等 LLM (executor) / 等工具结果。挂起在等工具的任务
 * 不算 —— 见 edc_task_is_running。 */
static inline bool edc_task_awaits_exec(const struct edc_task *task)
{
    return task && task->life == EDC_LIFE_ALIVE && task->step == EDC_TASKSTEP_AWAIT_EXEC;
}
static inline bool edc_task_awaits_tools(const struct edc_task *task)
{
    return task && task->life == EDC_LIFE_ALIVE && task->step == EDC_TASKSTEP_AWAIT_TOOLS;
}
/* "停着等驱动": 就绪或等依赖 —— 输入/输出都已装好, 只差一个事件。能接收
 * `/task join` 注入的就是这一类 (见 edc_commands.c)。 */
static inline bool edc_task_is_parked(const struct edc_task *task)
{
    return edc_task_is_ready(task) || edc_task_awaits_deps(task);
}
/* `/task resume` **命令门**: 挂起 (Ctrl+Z / 退出挂起) / 跑失败 / **被取消**。
 *
 * 取消算可续 —— 取消只表示"停下", 不表示"这活没了": 续跑时按消息与账本重新判定该
 * 回到哪一步 (取消那一步已经把 step 压成 DONE, 所以必须重判, 见
 * edc_scheduler_resume_task)。谓词与转移表 (edc_task.c 的 CANCELLED 行) 一致,
 * 命令层不再额外否决。 */
static inline bool edc_task_is_resumable(const struct edc_task *task)
{
    return task && (task->life == EDC_LIFE_SUSPENDED || task->life == EDC_LIFE_ERROR ||
                    task->life == EDC_LIFE_CANCELLED);
}

/* 对外词汇 —— 快照的 "state" 字段 / 日志 / 命令文案都用它。
 *
 * **词表冻结**: 返回的仍是旧名字 ("IDLE" / "WAITING_DEPS" / "LLM_PROCESSING" ...),
 * 因为它们是 TUI 颜色表、`/task status` 文本与日志的既有输入 (§6 硬规则: 外部行为
 * 一致)。它由 (step, life) **直接派生** (life 先说话), 不再是某个字段的转发;
 * `WAITING_INPUT` 这个词已经不会再出现 (那个相位退休了)。 */
extern const char *edc_task_state_str(const struct edc_task *task);

/* ── Todo 同步节流 ──────────────────────────────────────────
 * todo reminder 不再每轮请求都塞 (用户 2026-09-09 要求降频): 只有"隔了
 * EDC_TODO_SYNC_REMIND_GAP 条对话 (LLM 请求) 都没调用过 todowrite"
 * 时, 才在该次请求的消息列表末尾 append 一条 system reminder 提醒同步。 */

/* 阈值: 距上次 todowrite 调用 (或上次提醒) 满 EDC_TODO_SYNC_REMIND_GAP
 * 次 LLM 请求仍未同步, 则提醒一次。 */
#define EDC_TODO_SYNC_REMIND_GAP 10

/** todowrite 工具成功调用后调用: 清零节流计数 (重新从 0 计"隔了
 *  多少条对话")。 */
extern void edc_task_note_todo_sync(struct edc_task *task);

/** 每次为该 task 组装 LLM 请求 (send_llm) 前调用: 计数 +1; 返回 true 表示
 *  已达阈值、本次请求应 append todo reminder (内部已清零, 提醒一次后重新
 *  计数, 直到模型再次调用 todowrite 或再次超距)。 */
extern bool edc_task_llm_request_poke(struct edc_task *task);

/* ── Token 统计 ──
 * 累加一次 LLM 响应的 usage 到 task 的 token 账。四元组任一为正即累加;
 * 全 0 (provider 未返回 usage, 或错误响应) 时无操作。total <= 0 时用
 * prompt + completion 兜底 (与解析端一致)。线程/actor 语义: 只由调度器在
 * 处理该 task 的 LLM 响应时调用, 与 task 状态同线程。 */
extern void edc_task_add_usage(struct edc_task *task, long long prompt_tokens, long long completion_tokens,
                               long long total_tokens, long long cached_tokens);

/* ── Turn ──────────────────────────────────────────────────── */

extern void edc_task_add_turn(struct edc_task *task, struct edc_turn *turn);
extern void edc_task_commit_active_turn(struct edc_task *task);

/** 该 task 目前持有的 message 总数: 所有已提交 turn 的 messages 之和, 加上
 *  在途 active_turn 的 messages (若有)。一个 turn 可含多条 message (user +
 *  多轮 assistant/tool 往返), 因此这个数才是"消息数"; 与之相对,
 *  edc_queue_length(&task->turns) 只是已提交的轮数 (当前状态机下恒为 0/1)。 */
extern size_t edc_task_message_count(const struct edc_task *task);

/** 该 task 收到的所有用户输入 (各 turn 里 role=="user" 的消息 content, 以 '\n'
 *  连接)。返回新分配的 sds, 无输入返回 NULL, 调用方负责 sdsfree。用于
 *  /task status 展示 task 实际收到的输入, 而不只是 description。 */
extern sds edc_task_user_input(const struct edc_task *task, const cJSON *session_messages);

/* ── 依赖 (IPC) ────────────────────────────────────────────── */

extern bool edc_task_add_dependency(struct edc_task *target, struct edc_task *source, edc_task_dep_type type);
extern void edc_task_on_dep_completed(struct edc_task *target, struct edc_task *source);
extern void edc_task_on_dep_cancelled(struct edc_task *target, struct edc_task *source);
extern void edc_task_notify_dependents(struct edc_task *task);

/* ── 信号 ──────────────────────────────────────────────────── */

/** 任务是否"正在执行": 只有当进程正跑这个 turn 时才会出现的状态
 *  (LLM_PROCESSING / TOOL_CALLING)。与 edc_state_serializer 的中断归一化
 *  (normalize_interrupted_state) 用同一集合 —— 离开 session 时只取消这类任务,
 *  WAITING_INPUT / WAITING_DEPS / SUSPENDED 等原样保留。 */
extern bool edc_task_is_running(const struct edc_task *task);

extern void edc_task_send_signal(struct edc_task *task, edc_task_signal sig);

/* ── 未完成工具调用表 (resume 账本) ─────────────────────────
 * 生命周期: LLM 返回 tool_calls 时逐条 add (PENDING); toolbox 真正开始执行
 * 某个工具时经 BC_ACTOR_MSG_TOOL_STARTED 上报 → mark_executing(call_id) 置
 * EXECUTING; 工具返回结果时 remove (删记录); turn 提交/取消时 clear。
 * EXECUTING 由 toolbox 真实上报, 不再按链序推断。 */

/** 链尾追加一条 PENDING 记录。call_id/name 复制 (可为 NULL); args 深拷贝 (可为 NULL)。 */
extern struct edc_tool_call_record *edc_task_tool_call_add(struct edc_task *task, const char *call_id, const char *name,
                                                           cJSON *args);

/** 按 call_id 查找记录 (NULL = 无)。 */
extern struct edc_tool_call_record *edc_task_tool_call_find(struct edc_task *task, const char *call_id);

/** toolbox 上报"开始执行": 按 call_id 把记录置为 EXECUTING。返回该记录 (无则 NULL)。 */
extern struct edc_tool_call_record *edc_task_tool_call_mark_executing(struct edc_task *task, const char *call_id);

/** 删除记录 (工具返回结果 / resume 补中断响应)。返回是否删掉了一条。 */
extern bool edc_task_tool_call_remove(struct edc_task *task, const char *call_id);

/** 清空整张表。 */
extern void edc_task_tool_calls_clear(struct edc_task *task);

/** 表中是否还有记录 (PENDING 或 EXECUTING)。 */
extern bool edc_task_has_pending_tool_calls(const struct edc_task *task);

/* ── 输出 ──────────────────────────────────────────────────── */

extern void edc_task_set_output(struct edc_task *task, sds output);
extern sds edc_task_get_output(struct edc_task *task);

/* ── 折叠 ────────────────────────────────────────────────── */

/** task 是否可折叠: 已完成且 turns 尚未折叠进 group memory */
extern bool edc_task_is_foldable(struct edc_task *task);
/** task 的已提交 turn 消息总数 (用于折叠水位推进) */
extern size_t edc_task_turn_msg_count(struct edc_task *task);

/* ── Prompt ────────────────────────────────────────────────── */

extern void edc_task_set_prompt(struct edc_task *task, const char *prompt_name, const char *prompt_text);

/* ── 上下文构建 ────────────────────────────────────────────── */

extern cJSON *edc_task_get_context_messages(struct edc_task *task, struct edc_config *config);

#endif
