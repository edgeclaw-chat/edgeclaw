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

/* ── 状态机 ────────────────────────────────────────────────── */

extern bool edc_task_set_state(struct edc_task *task, edc_task_state new_state);
extern edc_task_state edc_task_classify_state(const char *name);

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
