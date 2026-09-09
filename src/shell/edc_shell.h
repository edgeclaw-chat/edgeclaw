#ifndef __EDC_SHELL_H__
#define __EDC_SHELL_H__

#include "edc_config.h"
#include "edc_task_types.h"
#include "sds.h"
#include <stdbool.h>

struct edc_agent;
struct edc_actor;
struct edc_channel_message;
struct edc_cmd_task_opts;
struct edc_task;
struct edc_task_group;

/* ── 变量解析回调 ─────────────────────────────────────────── */

/* shell 需要展开 $VAR 时调用此回调，由调用方（agent）实现具体查询逻辑
 * name: 变量名（如 "LAST", "$"）
 * user_data: 调用方传入的上下文
 * 返回: 变量值（sds），失败返回 NULL；调用方负责 sdsfree */
typedef sds (*edc_shell_var_resolver_fn)(const char *name, void *user_data);

/* ── Shell 执行 / 命令处理 ─────────────────────────────────── */

/* 执行非 / 开头的 shell 输入（@ 和普通文本），返回 true 表示已处理 */
bool edc_shell_execute(struct edc_agent *agent, struct edc_actor *actor, struct edc_channel_message *message);

/* 处理内置命令 (/whoami, /model, /history)，返回 true=已处理 */
bool edc_shell_handle_builtin(struct edc_agent *agent, struct edc_actor *actor, struct edc_channel_message *message);

/* 通用 task 创建 + group 管理 + deps + turn + LLM 发送 */
struct edc_task *edc_shell_spawn_task(struct edc_agent *agent, struct edc_actor *actor, edc_channel_type channel_type,
                                      struct edc_cmd_task_opts *opts, struct edc_task *prev_task,
                                      struct edc_task_group *target_group, const char *reply_prefix,
                                      struct edc_task_group **out_group, sds redirect_stdout, bool redirect_append,
                                      sds redirect_stderr, edc_task_dep_type prev_dep_type);

/* ── Pipeline / 重定向 / 分拆 ──────────────────────────────── */

/* Pipeline 拆分（引号感知的 | 拆分） */
sds *edc_shell_split_pipeline(const char *input, int *count);

/* 从 segment 中剥离重定向语法（> >> 2> <） */
void edc_shell_strip_redirects(sds *segment, sds *out_stdout, bool *out_append, sds *out_stderr, sds *out_input_file);

/* ; 分隔符拆分（引号感知） */
sds *edc_shell_split_sequential(const char *input, int *count);

/* 读取文件内容，失败时 *err_out 设错误消息 */
sds edc_shell_read_file(const char *path, sds *err_out);

/* ── 历史展开 ──────────────────────────────────────────────── */

sds edc_shell_expand_history(struct edc_agent *agent, const char *input);

/* ── 变量引用（$VAR）───────────────────────────────────────── */

sds edc_shell_collect_var_refs(struct edc_agent *agent, const char *input, sds **out_deps, int *out_dep_count,
                               sds *out_error);

/* 展开 $VAR 引用（expand 阶段，deps 已完成后调用）。
 * resolver: 解析变量值的回调
 * 成功返回 expanded string，失败返回 NULL 并设置 *out_error */
sds edc_shell_expand_vars(const char *input, edc_shell_var_resolver_fn resolver, void *user_data, sds *out_error);

#endif
