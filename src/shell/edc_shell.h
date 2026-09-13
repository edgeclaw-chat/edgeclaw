#ifndef __EDC_SHELL_H__
#define __EDC_SHELL_H__

#include "edc_config.h"
#include "edc_task_types.h"
#include "edc_shell_vars.h"
#include "sds.h"
#include <stdbool.h>

struct edc_agent;
struct edc_actor;
struct edc_channel_message;
struct edc_cmd_task_opts;
struct edc_task;
struct edc_task_group;

/* ── Shell 执行 / 命令处理 ─────────────────────────────────── */

/* 执行非 / 开头的 shell 输入（@ 和普通文本），返回 true 表示已处理 */
bool edc_shell_execute(struct edc_agent *agent, struct edc_actor *actor, struct edc_channel_message *message);

/* 处理内置命令 (/whoami, /model)，返回 true=已处理 */
bool edc_shell_handle_builtin(struct edc_agent *agent, struct edc_actor *actor, struct edc_channel_message *message);

/* 通用 task 创建 + group 管理 + deps + turn + LLM 发送 */
struct edc_task *edc_shell_spawn_task(struct edc_agent *agent, struct edc_actor *actor, edc_channel_type channel_type,
                                      struct edc_cmd_task_opts *opts, struct edc_task *prev_task,
                                      struct edc_task_group *target_group, const char *reply_prefix,
                                      struct edc_task_group **out_group, sds redirect_stdout, bool redirect_append,
                                      sds redirect_stderr, edc_task_dep_type prev_dep_type);

/* 读取文件内容，失败时 *err_out 设错误消息 */
sds edc_shell_read_file(const char *path, sds *err_out);

/* ── 历史展开 ──────────────────────────────────────────────── */

sds edc_shell_expand_history(struct edc_agent *agent, const char *input);

/* ── 变量引用（$VAR）───────────────────────────────────────── */

sds edc_shell_collect_var_refs(struct edc_agent *agent, const char *input, sds **out_deps, int *out_dep_count,
                               sds *out_error);

#endif
