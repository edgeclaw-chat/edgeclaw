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

/* ── Task 状态 ─────────────────────────────────────────────── */

typedef enum
{
    TASK_IDLE,           /* 刚创建，未开始 */
    TASK_WAITING_DEPS,   /* 等待依赖 task 完成 */
    TASK_WAITING_INPUT,  /* 等待用户输入 */
    TASK_LLM_PROCESSING, /* LLM 生成中 */
    TASK_TOOL_CALLING,   /* 执行工具调用链 */
    TASK_SUSPENDED,      /* 已暂停 (Ctrl+Z) */
    TASK_COMPLETED,      /* 任务完成 */
    TASK_CANCELLED,      /* 被取消 */
    TASK_ERROR,          /* 出错 */
} edc_task_state;

extern const char *edc_task_state_name(edc_task_state state);

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

/* ── Task ──────────────────────────────────────────────────── */

struct edc_task
{
    char *task_id;    /* PID — 唯一标识 */
    char *group_id;   /* PGID — 所属 group (leader 的 task_id) */
    char *session_id; /* SID — 所属 session */

    char *description;    /* 用户可见描述 */
    edc_task_state state; /* 当前状态 */

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
     * 续发 LLM, 以收尾消息终结本轮, 防止模型在工具循环里空转烧预算。新
     * 输入开始新一轮时清零 (scheduler)。 */
    int llm_rounds;

    edc_channel_type channel_type;

    cJSON *todolist; /* JSON Array */

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
    size_t fork_point_msg_count;  /* fork 时刻父 group 消息总数 (仅元数据, 上下文切片由合成 turn 快照负责) */

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

    /* ── 输出重定向 ── */
    sds redirect_stdout;  /* > / >> file */
    sds redirect_stderr;  /* 2> file */
    bool redirect_append; /* true = >>, false = > */

    /* ── 链表 ── */
    struct edc_task *group_next;  /* group 内下一成员 */
    struct edc_task *prev, *next; /* session 全局链表 */
};

/* ── 生命周期 ──────────────────────────────────────────────── */

extern struct edc_task *edc_task_new(const char *task_id, const char *session_id, const char *group_id,
                                     const char *desc, edc_channel_type channel_type);
extern struct edc_task *edc_task_fork(struct edc_task *parent, const char *task_id, const char *desc,
                                      size_t fork_point_msg_count, edc_channel_type channel_type);
extern void edc_task_free(struct edc_task *task);

/* ── fork 快照 ────────────────────────────────────────────── */

/** 深拷贝 task 的全部 turn 消息到一个合成快照 turn。
 *  兼容两种态: 未 flush (消息在 turn->messages) 与已 flush (消息在 session->messages,
 *  用 recorded msg_range 定位)。返回 NULL 表示 task 无消息; 调用方负责 edc_turn_free。
 *  用于 fork 规则 2: 当 parent 的 turns 已被 L1 折叠时, 捞回 parent 自己的 turns 全文。 */
extern struct edc_turn *edc_task_snapshot_turns(struct edc_task *task, struct edc_session *session,
                                                const char *snapshot_task_id);

/* ── 状态机 ────────────────────────────────────────────────── */

extern bool edc_task_set_state(struct edc_task *task, edc_task_state new_state);
extern edc_task_state edc_task_classify_state(const char *name);

/* ── Todo 同步节流 ──────────────────────────────────────────
 * todo reminder 不再每轮请求都塞 (用户 2026-09-09 要求降频): 只有"隔了
 * EDC_TODO_SYNC_REMIND_GAP 条对话 (LLM 请求) 都没调用过 todowrite"
 * 时, 才在该次请求的消息列表末尾 append 一条 system reminder 提醒同步。 */

/* 阈值: 距上次 todowrite 调用 (或上次提醒) 满 10 次 LLM 请求仍未
 * 同步, 则提醒一次。 */
#define EDC_TODO_SYNC_REMIND_GAP 10

/** todowrite 工具成功调用后调用: 清零节流计数 (重新从 0 计"隔了
 *  多少条对话")。 */
extern void edc_task_note_todo_sync(struct edc_task *task);

/** 每次为该 task 组装 LLM 请求 (send_llm) 前调用: 计数 +1; 返回 true 表示
 *  已达阈值、本次请求应 append todo reminder (内部已清零, 提醒一次后重新
 *  计数, 直到模型再次调用 todowrite 或再次超距)。 */
extern bool edc_task_llm_request_poke(struct edc_task *task);

/* ── Turn ──────────────────────────────────────────────────── */

extern void edc_task_add_turn(struct edc_task *task, struct edc_turn *turn);
extern void edc_task_commit_active_turn(struct edc_task *task);

/* ── 依赖 (IPC) ────────────────────────────────────────────── */

extern bool edc_task_add_dependency(struct edc_task *target, struct edc_task *source, edc_task_dep_type type);
extern void edc_task_on_dep_completed(struct edc_task *target, struct edc_task *source);
extern void edc_task_on_dep_cancelled(struct edc_task *target, struct edc_task *source);
extern void edc_task_notify_dependents(struct edc_task *task);

/* ── 信号 ──────────────────────────────────────────────────── */

extern void edc_task_send_signal(struct edc_task *task, edc_task_signal sig);

/* ── 输出 ──────────────────────────────────────────────────── */

extern void edc_task_set_output(struct edc_task *task, sds output);
extern sds edc_task_get_output(struct edc_task *task);

/* ── 折叠 ────────────────────────────────────────────────── */

/** task 是否可折叠: 已完成且 turns 尚未折叠进 group memory */
extern bool edc_task_is_foldable(struct edc_task *task);
/** task 的已提交 turn 消息总数 (用于 fold_cursor 推进) */
extern size_t edc_task_turn_msg_count(struct edc_task *task);

/* ── Prompt ────────────────────────────────────────────────── */

extern void edc_task_set_prompt(struct edc_task *task, const char *prompt_name, const char *prompt_text);

/* ── 上下文构建 ────────────────────────────────────────────── */

extern cJSON *edc_task_get_context_messages(struct edc_task *task, struct edc_config *config);

#endif
