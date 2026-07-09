#ifndef __BC_TASK_H__
#define __BC_TASK_H__

#include "sds.h"
#include "edc_containers.h"
#include "edc_turn.h"
#include <cJSON.h>
#include <stdbool.h>
#include <stddef.h>

struct edc_channel;
struct edc_session;
struct edc_config;

/* ── Task 状态 ─────────────────────────────────────────────── */

typedef enum {
    TASK_IDLE,            /* 刚创建，未开始 */
    TASK_WAITING_DEPS,    /* 等待依赖 task 完成 */
    TASK_WAITING_INPUT,   /* 等待用户输入 */
    TASK_LLM_PROCESSING,  /* LLM 生成中 */
    TASK_TOOL_CALLING,    /* 执行工具调用链 */
    TASK_SUSPENDED,       /* 已暂停 (Ctrl+Z) */
    TASK_COMPLETED,       /* 任务完成 */
    TASK_CANCELLED,       /* 被取消 */
    TASK_ERROR,           /* 出错 */
} edc_task_state;

extern const char *edc_task_state_name(edc_task_state state);

/* ── 信号 ──────────────────────────────────────────────────── */

typedef enum {
    TASK_SIGTERM,   /* 优雅取消：取消 in-flight 请求，通知依赖者 */
    TASK_SIGKILL,   /* 强制取消：不通知依赖者，deps_total 不减 */
} edc_task_signal;

/* ── Task 间依赖 ───────────────────────────────────────────── */

typedef enum {
    TASK_DEP_JOIN,       /* 等待源 task 完成，将源 output 注入自己上下文 */
    TASK_DEP_PIPE,       /* 流式传输（后续扩展） */
    TASK_DEP_SEQUENCE,   /* 等待源 task 完成，不注入 output（; 语义） */
} edc_task_dep_type;

struct edc_task_dep {
    struct edc_task *source;       /* 依赖的源 task */
    struct edc_task *target;       /* 被依赖的目标 task (this) */
    edc_task_dep_type type;
    bool completed;                /* 已完成 */
    bool cancelled;                /* 已取消 */
    bool injected;                 /* 已注入到 LLM 上下文 */
    struct edc_task_dep *next;     /* 单链表 (在 target 的 deps 链上) */
};

/* ── Task ──────────────────────────────────────────────────── */

struct edc_task {
    char *task_id;                  /* PID — 唯一标识 */
    char *group_id;                 /* PGID — 所属 group (leader 的 task_id) */
    char *session_id;               /* SID — 所属 session */

    char *description;              /* 用户可见描述 */
    edc_task_state state;           /* 当前状态 */

    int is_group_leader;            /* 是否是 group leader */
    int auto_reap;                  /* 完成后自动回收 (前台输入任务) */

    struct edc_task_group *group;   /* 所属 group */

    EdcQueue turns;                 /* FIFO: struct edc_turn* */
    struct edc_turn *active_turn;   /* 当前活跃 turn */

    void *current_request_id;       /* 发往 actor 的 request_id */
    int cancelled_request;          /* 标记请求已取消 */

    struct edc_channel *channel;

    cJSON *todolist;                /* JSON Array */

    /* ── Fork 上下文继承 ── */
    struct edc_task *forked_from;   /* 父 task (NULL = 非 fork) */
    size_t fork_point_msg_count;    /* fork 时刻 session 消息总数 (切片点) */

    /* ── Subagent / Prompt 绑定 ── */
    sds prompt;                     /* 自定义 system prompt (NULL = 用默认) */
    sds prompt_name;                /* prompt 名称 (如 "code-reviewer") */

    /* ── Task IPC ── */
    struct edc_task_dep *deps;          /* 我依赖谁 */
    struct edc_task_dep *dependents;    /* 谁依赖我 */
    size_t deps_total;                  /* 总依赖数 */
    size_t deps_completed;              /* 已完成依赖数 */
    sds output;                         /* 本 task 最终输出 (供 JOIN 注入) */

    uint32_t completion_seq;            /* 完成顺序 (0=未完成) */
    int has_deferred_vars;              /* $VAR 引用待展开 */

    /* ── 输出重定向 ── */
    sds redirect_stdout;            /* > / >> file */
    sds redirect_stderr;            /* 2> file */
    bool redirect_append;           /* true = >>, false = > */

    /* ── 链表 ── */
    struct edc_task *group_next;    /* group 内下一成员 */
    struct edc_task *prev, *next;   /* session 全局链表 */
};

/* ── 生命周期 ──────────────────────────────────────────────── */

extern struct edc_task *edc_task_new(const char *task_id, const char *session_id, const char *group_id,
                                     const char *desc, struct edc_channel *channel);
extern struct edc_task *edc_task_fork(struct edc_task *parent, const char *task_id, const char *desc,
                                      size_t fork_point_msg_count, struct edc_channel *channel);
extern void edc_task_free(struct edc_task *task);

/* ── 状态机 ────────────────────────────────────────────────── */

extern bool edc_task_set_state(struct edc_task *task, edc_task_state new_state);
extern edc_task_state edc_task_classify_state(const char *name);

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

/* ── Prompt ────────────────────────────────────────────────── */

extern void edc_task_set_prompt(struct edc_task *task, const char *prompt_name, const char *prompt_text);

/* ── 上下文构建 ────────────────────────────────────────────── */

extern cJSON *edc_task_get_context_messages(struct edc_task *task, struct edc_config *config);

#endif
