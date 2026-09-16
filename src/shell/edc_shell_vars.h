#ifndef __EDC_SHELL_VARS_H__
#define __EDC_SHELL_VARS_H__

#include "sds.h"
#include <stdbool.h>

/* ── $VAR 展开原语（leaf: 只依赖 common，不认识 app）─────────
 *
 * 这一层刻意与 shell 执行器（edc_shell.c / edc_commands.c，它们依赖
 * //src/app:app）分开：app 侧（edc_scheduler 的 /task 解析、
 * edc_app 的变量展开）需要这两个入口，但绝不能反向依赖 shell 执行器，
 * 否则形成 app ↔ shell 的循环。见 //src/shell:shell_parse。 */

/* 展开 $VAR 时用此回调向调用方查询变量值。
 * name: 变量名（如 "LAST", "$"）
 * user_data: 调用方上下文
 * 返回: 变量值（sds），失败返回 NULL；调用方负责 sdsfree */
typedef sds (*edc_shell_var_resolver_fn)(const char *name, void *user_data);

/* 展开 $VAR 引用（expand 阶段，deps 已完成后调用）。
 * resolver: 解析变量值的回调
 * 成功返回 expanded string，失败返回 NULL 并设置 *out_error */
sds edc_shell_expand_vars(const char *input, edc_shell_var_resolver_fn resolver, void *user_data, sds *out_error);

#endif
