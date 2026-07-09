#ifndef __EDC_COMPLETION_H__
#define __EDC_COMPLETION_H__

#include "sds.h"
#include <stdbool.h>
#include <stddef.h>

/* ---- completion candidate ---- */
typedef struct {
    sds    text;        /* 补全文本 */
    sds    display;     /* 展示文本（可含颜色/描述） */
    double score;       /* 排序分数，越高越好 */
    int    source;      /* 来源：0=cmd, 1=file, 2=history */
} edc_completion_candidate_t;

/* ---- completion result set ---- */
#define EDC_COMPLETION_MAX 256
typedef struct {
    edc_completion_candidate_t items[EDC_COMPLETION_MAX];
    int                        count;
    int                        selected;   /* 当前选中项索引，-1 表示未选择 */
    sds                        common_prefix; /* 所有候选项的最长公共前缀 */
    sds                        token;         /* 用户输入的前缀 token */
} edc_completion_result_t;

/* ---- completion source: callback that fills result ---- */
typedef void (*edc_completion_source_fn)(const char *token,
                                         edc_completion_result_t *result,
                                         void *user_data);

/* ---- core API ---- */

/* 初始化 / 释放 */
void edc_completion_init(void);
void edc_completion_free(void);

/* 注册一个补全源（如命令、文件、历史等） */
void edc_completion_register_source(edc_completion_source_fn source, void *user_data);

/* 添加命令到内部 Trie（用于 /command 补全） */
void edc_completion_add_command(const char *command);

/* 添加 history 条目（用于基于历史的补全） */
void edc_completion_add_history(const char *line);

/* 核心：执行补全，返回结果集 */
void edc_completion_collect(const char *line_buffer,
                            int cursor_pos,
                            edc_completion_result_t *result);

/* 释放结果集 */
void edc_completion_result_free(edc_completion_result_t *result);

/* 公共前缀提取：在候选项中找最长公共前缀 */
sds  edc_completion_lcp(edc_completion_result_t *result);

/* 模糊匹配评分 (0.0 ~ 1.0)，用于回退场景 */
double edc_completion_fuzzy_score(const char *pattern, const char *target);

#endif /* __EDC_COMPLETION_H__ */
