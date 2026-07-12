#ifndef __EDC_SHELL_H__
#define __EDC_SHELL_H__

#include "edc_task.h"
#include "sds.h"
#include <stdbool.h>

struct edc_agent;
struct edc_actor;
struct edc_channel;
struct edc_channel_message;
struct edc_cmd_task_opts;
struct edc_task;
struct edc_task_group;

/* 执行非 / 开头的 shell 输入（@ 和普通文本），返回 true 表示已处理 */
bool edc_shell_execute(struct edc_agent *agent, struct edc_actor *actor,
                       struct edc_channel_message *message);

/* 处理新的内置命令 (/jobs, /cancel, /tasks)，返回 true=已处理 */
bool edc_shell_handle_builtin(struct edc_agent *agent, struct edc_actor *actor,
                              struct edc_channel_message *message);

/* 通用 task 创建 + group 管理 + deps + turn + LLM 发送。
 * target_group != NULL 时 add 到该 group，NULL 时新建。
 * out_group 接收实际使用的 group（用于后续 pipe 段复用）。
 * redirect_stdout/redirect_stderr 非 NULL 时设置到 task 上。 */
struct edc_task *edc_shell_spawn_task(
    struct edc_agent *agent, struct edc_actor *actor,
    struct edc_channel *channel,
    struct edc_cmd_task_opts *opts,
    struct edc_task *prev_task,
    struct edc_task_group *target_group,
    const char *reply_prefix,
    struct edc_task_group **out_group,
    sds redirect_stdout, bool redirect_append, sds redirect_stderr,
    edc_task_dep_type prev_dep_type);

/* Pipeline 拆分（引号感知的 | 拆分），返回 sds 数组，*count 为段数。
 * 调用方负责 sdsfree 每个元素和 free 数组。 */
sds *edc_shell_split_pipeline(const char *input, int *count);

/* 从 segment 中剥离重定向语法（> >> 2> <），返回清理后的 segment 和重定向信息 */
void edc_shell_strip_redirects(sds *segment, sds *out_stdout, bool *out_append,
                                sds *out_stderr, sds *out_input_file);

/* ; 分隔符拆分（引号感知），返回 sds 数组，*count 为段数 */
sds *edc_shell_split_sequential(const char *input, int *count);

/* 读取文件内容，失败时 *err_out 设错误消息 */
sds edc_shell_read_file(const char *path, sds *err_out);

/* 历史展开 (!n, !!)，需要 agent->history 中有缓存的历史记录 */
sds edc_shell_expand_history(struct edc_agent *agent, const char *input);

/* 展开 $VAR 引用（expand 阶段，deps 已完成后调用）。
 * out_error 非 NULL 时接收错误消息。成功返回 expanded string，失败返回 NULL。 */
sds edc_shell_expand_vars(struct edc_agent *agent, const char *input, sds *out_error);

#endif
