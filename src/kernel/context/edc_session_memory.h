#ifndef __BC_SESSION_MEMORY_H__
#define __BC_SESSION_MEMORY_H__

#include <cJSON.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "sds.h"

struct edc_scheduler;
struct edc_session;
struct edc_task;
struct edc_task_group;

/* L2: session 级记忆 (设计见 docs/l2-session-memory.md)。

 * 两半, 各自单写者:
 *   - **事件表** (entries 的变更日志, seq 单调): L2 任务写。
 *     它不给任何 task 的上下文里塞东西 —— 每个 task 记一个水位
 *     (`task->memo_seen_seq`), 请求装配时取差集当"补丁"注入队尾。
 *   - **条目表** (索引 + 明细): 索引每轮常驻头部, 明细按需 (P4d 的
 *     `memory_get`)。

 * 关键不变量 (与 L1 同族, 见 pi-alignment.md):
 *   - 事件 seq 单调递增、**永不复用**; `next_seq` = 下一个要分配的 seq
 *     (无事件时 0)。task 的"看过"水位与它比较: `seq > memo_seen_seq`。
 *   - **L2 没有自己的水位**: 触发是"折叠落地"这个事件, 输入是"刚落地的
 *     group memory"这个快照, 而 L2 与 L1 由依赖边串行 ⇒ 同一份材料不会跑两遍
 *     (见 docs/l2-session-memory.md §4.2)。 */

enum edc_memo_event_kind
{
    EDC_MEMO_CREATED,
    EDC_MEMO_UPDATED,
    EDC_MEMO_SUPERSEDED,
};

struct edc_memo_event
{
    uint64_t seq;
    char *entry_id;
    enum edc_memo_event_kind kind;
    char *title;
    char *summary;
};

struct edc_memo_entry
{
    char *id;
    char *title;
    char *summary;
    sds detail;    /* 明细全文: 不进常驻上下文, 只由 memory_get 现取 (P4d) */
    char *sources; /* 来源: 组 id / 覆盖的 completion_seq 区间 (审计) */
    char *proposed_by; /* 谁提议写的 (前台 memory_write 的 task id; 策展人写的为 NULL) */
    uint64_t created_seq;
    uint64_t updated_seq;
    bool superseded;
};

struct edc_session_memory
{
    struct edc_memo_event *events;
    size_t event_count, event_cap;

    struct edc_memo_entry *entries;
    size_t entry_count, entry_cap;

    uint64_t next_seq;      /* 下一个要分配的 seq (0 = 还没有事件) */
    uint64_t next_entry_id; /* 下一个要分配的条目号 (id = "m<n>", 永不复用) */
    uint64_t pruned_before; /* 事件清理后的下界 (P4e; 现在恒 0) */
    int fail_count;         /* 连续失败, 只在内存 */
};

extern const char *edc_memo_event_kind_name(enum edc_memo_event_kind kind);
extern enum edc_memo_event_kind edc_memo_event_classify_kind(const char *name);

/* 策展调用的输出上限 (ops JSON 很小; 这只是不封顶的保险) 与条目总数上限
 * (超上限的批次整批拒绝 —— 宁可不记, 也不让索引失控)。 */
#define EDC_SESSION_MEMORY_MAX_TOKENS 8192
#define EDC_SESSION_MEMORY_MAX_ENTRIES 200

extern struct edc_session_memory *edc_session_memory_new(void);
extern void edc_session_memory_free(struct edc_session_memory *m);

/* 追加一个事件 (返回它拿到的 seq; 分配失败返回 0)。 */
extern uint64_t edc_session_memory_add_event(struct edc_session_memory *m, const char *entry_id,
                                             enum edc_memo_event_kind kind, const char *title, const char *summary);

/* 条目: 查 / 增改 (按 id, 存在则更新)。返回条目的指针 (归 m 所有)。 */
extern struct edc_memo_entry *edc_session_memory_entry(struct edc_session_memory *m, const char *id);
extern struct edc_memo_entry *edc_session_memory_upsert_entry(struct edc_session_memory *m, const char *id,
                                                              const char *title, const char *summary,
                                                              const char *detail, const char *sources);

/* 记"谁提议写的" (前台 memory_write 用; 归 m 所有)。 */
extern void edc_session_memory_note_proposer(struct edc_memo_entry *entry, const char *task_id);

/* ── 给工具与通知用的读/写口 ───────────────────────────────── */

/* 索引行 (一行一条, 形如 "- [m12] 标题 — 摘要", 退休的加 "(retired)"), 按 id 升序
 * 保证翻页稳定。返回的 sds 归调用方; `*out_total` 给条目总数 (可为 NULL)。 */
extern sds edc_session_memory_index_lines(const struct edc_session_memory *m, size_t offset, size_t n,
                                          size_t *out_total);

/* 搜索: 标题/摘要/明细的子串匹配 (大小写不敏感), 返回命中条目的索引行 (上限 cap 条),
 * `*out_hits` 给命中总数 (可为 NULL)。 */
extern sds edc_session_memory_search_lines(const struct edc_session_memory *m, const char *query, size_t cap,
                                           size_t *out_hits);

/* 队尾通知 (请求期临时物, 见 docs/l2-session-memory.md §1.2.1/§3.2):
 * 自 `seen` 以来**新建/更新**的条目, 一行一条 (同一 id 只出现最新那条), 最多 cap 条,
 * 带总条数与用法提示。返回 NULL = 没有新东西 (不注入)。
 * `*out_max_injected` (可 NULL) = **实际注入**的最大事件 seq —— 请求被模型真的读到之后,
 * 水位才推进到它 (见 edc_task_memo_commit); 截断掉的老事件由水位一并标记为已看,
 * 通知里那句 "N more" 与 memory_list/memory_search 就是它们的兜底。 */
extern sds edc_session_memory_notice(const struct edc_session_memory *m, uint64_t seen, size_t cap,
                                     uint64_t *out_max_injected);

/* 某条目的明细全文 (没有明细时退化成索引行); 条目不存在返回 NULL。 */
extern sds edc_session_memory_get_detail(const struct edc_session_memory *m, const char *id);

/* 新建条目: 分配新 id (m<n>, 永不复用) + 写入 title/summary/detail。
 * 返回新 id (sds, 调用方 sdsfree); 失败返回 NULL。**不**写事件 —— 事件由调用方
 * (前台 memory_write 或 L2 落地) 按 ops 语义追加。 */
extern sds edc_session_memory_create_entry(struct edc_session_memory *m, const char *title, const char *summary,
                                           const char *detail, const char *proposed_by);

/* 出生点采纳: 新 task 的"看过"水位 = **0** —— 它正是"没有最新条目"的那个,
 * 于是它的第一次请求会自带一次回填通知 (最新 cap 条 + "N more")。这解决了
 * "session 内新建 group 看不到最新 session 记忆"。**只在 task 的出生点**调用
 * (用户 task = create_task, fork 的子 task / 新组的 leader = create_group,
 * 系统 task = start_system_task)。 */
extern void edc_session_memory_adopt_task(struct edc_session *session, struct edc_task *task);

/* ── L2 策展任务 (见 docs/l2-session-memory.md §4) ─────────── */

/* 策展调用的 system 说明 (自足请求: 只有这一段 + 它自己的 turn)。 */
extern const char *edc_session_memory_system_prompt(void);

/* 折叠落地 ⇒ 建 L2 策展任务, 并把链挂成:
 *     被折组的非终态成员 ──SEQUENCE──► L2 ──SEQUENCE──► L1(刚落地, 声明式)
 * 于是"成员的恢复"发生在策展之后, 而 L2 读得到刚落地的 group 记忆。
 * 返回建好的 L2 任务 (失败 NULL)。 */
extern struct edc_task *edc_session_memory_start(struct edc_scheduler *sched, struct edc_session *session,
                                                 struct edc_task_group *folded_group, struct edc_task *fold_task);

/* ops 契约的解析 + 应用 (先整批校验再应用; 见 docs/l2-session-memory.md §4.7)。
 * `sources` 记到新建条目上 (通常是来源组 id; 可 NULL)。 */
extern bool edc_session_memory_apply_ops(struct edc_session_memory *m, const char *text, const char *sources);

/* 手动策展 (`/session memo compact [focus]`): 不等折叠, 现在就跑一次 ——
 * 材料 = **所有**有记忆的组的 memory + 现有条目索引 (+ focus)。已有 system task
 * 在飞时拒绝 (一次一个, 与自动路径同一条闸门)。 */
extern struct edc_task *edc_session_memory_start_manual(struct edc_scheduler *sched, struct edc_session *session,
                                                        const char *focus);

/* `/session memo show` 的文本 (纯函数, 便于断言): id 为空 = 列全部条目 + 总数;
 * 给了 id = 那一条的完整内容 (不存在则给出提示)。调用方 sdsfree。 */
extern sds edc_session_memory_show_text(const struct edc_session_memory *m, const char *id);

/* `/session memo search <q>` 的文本 (纯函数): 命中条目的索引行 + 计数。 */
extern sds edc_session_memory_search_text(const struct edc_session_memory *m, const char *query);

/* L2 落地: 解析 ops 契约并应用。**先整批校验, 再应用** —— 任一条不合法就什么都不落
 * (老条目与事件表一动不动), 返回 false。 */
extern bool edc_session_memory_deliver(struct edc_task *task, const char *content);

/* 落盘 (schema v7 的 `session_memory` 块)。 */
extern cJSON *edc_session_memory_to_json(const struct edc_session_memory *m);
extern struct edc_session_memory *edc_session_memory_from_json(const cJSON *json);

#endif
