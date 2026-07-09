#include "edc_completion.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <dirent.h>
#include <sys/stat.h>

/* ================================================================
 *  1. Trie — 用于命令/子命令的快速前缀匹配
 * ================================================================ */

#define TRIE_CHILDREN 128  /* ASCII 范围足够 */

typedef struct trie_node {
    struct trie_node *children[TRIE_CHILDREN];
    bool              is_terminal;   /* 是否是一个完整命令 */
    int               freq;          /* 使用频率 */
} trie_node_t;

static trie_node_t *g_cmd_trie = NULL;

static trie_node_t *trie_node_new(void) {
    trie_node_t *n = (trie_node_t *)calloc(1, sizeof(trie_node_t));
    return n;
}

static void trie_insert(trie_node_t *root, const char *key) {
    trie_node_t *cur = root;
    for (const unsigned char *p = (const unsigned char *)key; *p; p++) {
        int idx = *p;
        if (idx >= TRIE_CHILDREN) idx = TRIE_CHILDREN - 1;
        if (!cur->children[idx])
            cur->children[idx] = trie_node_new();
        cur = cur->children[idx];
    }
    cur->is_terminal = true;
    cur->freq++;
}

/* 递归收集所有以 node 为根的完整词 */
static void trie_collect(trie_node_t *node, sds prefix,
                         edc_completion_result_t *result) {
    if (!node) return;
    if (node->is_terminal && result->count < EDC_COMPLETION_MAX) {
        int idx = result->count;
        result->items[idx].text    = sdsdup(prefix);
        result->items[idx].display = sdsdup(prefix);
        result->items[idx].score   = (double)node->freq;
        result->items[idx].source  = 0;
        result->count++;
    }
    for (int i = 0; i < TRIE_CHILDREN; i++) {
        if (node->children[i]) {
            char ch = (char)i;
            sds child_prefix = sdscatlen(sdsdup(prefix), &ch, 1);
            trie_collect(node->children[i], child_prefix, result);
        }
    }
}

/* 前缀搜索：找到 prefix 对应的节点，然后收集所有子节点 */
static void trie_prefix_search(trie_node_t *root, const char *prefix,
                               edc_completion_result_t *result) {
    trie_node_t *cur = root;
    for (const unsigned char *p = (const unsigned char *)prefix; *p; p++) {
        int idx = *p;
        if (idx >= TRIE_CHILDREN) idx = TRIE_CHILDREN - 1;
        if (!cur->children[idx]) return;  /* 无匹配 */
        cur = cur->children[idx];
    }
    /* cur 是 prefix 的节点，收集所有子节点 */
    sds p = sdsnew(prefix);
    trie_collect(cur, p, result);
}

static void trie_free_node(trie_node_t *node) {
    if (!node) return;
    for (int i = 0; i < TRIE_CHILDREN; i++)
        trie_free_node(node->children[i]);
    free(node);
}

/* ================================================================
 *  2. 命令补全源 — 内部 Trie
 * ================================================================ */

static void source_commands(const char *token,
                            edc_completion_result_t *result,
                            void *user_data) {
    (void)user_data;
    if (!g_cmd_trie || !token || token[0] != '/') return;
    /* token 是 "/tas" 这种形式，直接送入 trie */
    trie_prefix_search(g_cmd_trie, token, result);
}

/* ================================================================
 *  3. 文件路径补全源
 * ================================================================ */

/* 从路径中提取目录部分和文件前缀部分 */
static void path_split(const char *token, sds *dir_part, sds *file_part) {
    const char *last_slash = strrchr(token, '/');
    if (last_slash) {
        *dir_part  = sdsnewlen(token, (size_t)(last_slash - token + 1)); /* 含 / */
        *file_part = sdsnew(last_slash + 1);
    } else {
        *dir_part  = sdsnew("./");   /* 当前目录 */
        *file_part = sdsnew(token);
    }
}

static void source_files(const char *token,
                         edc_completion_result_t *result,
                         void *user_data) {
    (void)user_data;
    if (!token || !token[0]) return;

    sds dir_part, file_part;
    path_split(token, &dir_part, &file_part);

    DIR *d = opendir(dir_part);
    if (!d) { sdsfree(dir_part); sdsfree(file_part); return; }

    size_t file_len = sdslen(file_part);
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL && result->count < EDC_COMPLETION_MAX) {
        if (entry->d_name[0] == '.') continue; /* 跳过隐藏文件 */

        /* 前缀匹配 */
        if (strncmp(entry->d_name, file_part, file_len) != 0) continue;

        int idx = result->count;
        /* 构建完整路径 */
        sds full = sdscatprintf(sdsdup(dir_part), "%s", entry->d_name);

        /* 如果是目录，加 / 后缀 */
        struct stat st;
        bool is_dir = false;
        sds display_path = sdsdup(full);
        if (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) {
            is_dir = true;
            full = sdscat(full, "/");
            display_path = sdscat(display_path, "/");
        }

        result->items[idx].text    = full;
        result->items[idx].display = display_path;
        /* 目录优先级略高 */
        result->items[idx].score   = is_dir ? 2.0 : 1.0;
        result->items[idx].source  = 1;
        result->count++;
    }
    closedir(d);
    sdsfree(dir_part);
    sdsfree(file_part);
}

/* ================================================================
 *  4. History 补全源 — 带 Frecency 评分
 * ================================================================ */

#define HISTORY_MAX_ENTRIES 2000

typedef struct {
    char   *line;
    int     count;     /* 使用次数 */
    double  last_used; /* 单调递增的时间戳 */
} history_entry_t;

static history_entry_t *g_history = NULL;
static int              g_history_count = 0;
static int              g_history_cap   = 0;
static double           g_tick          = 0.0;

static void source_history(const char *token,
                           edc_completion_result_t *result,
                           void *user_data) {
    (void)user_data;
    if (!token || !token[0]) return;

    size_t tlen = strlen(token);

    for (int i = 0; i < g_history_count && result->count < EDC_COMPLETION_MAX; i++) {
        /* 快速前缀过滤 */
        if (strncmp(g_history[i].line, token, tlen) != 0) continue;

        /* Frecency 评分:
         *   score = log2(1 + count) * 2.0  +  recency_bonus
         *   recency_bonus = 1.0 / (1.0 + tick_diff)  (越近越高)
         */
        double freq_score = 2.0 * log2(1.0 + (double)g_history[i].count);
        double tick_diff  = g_tick - g_history[i].last_used;
        if (tick_diff < 0) tick_diff = 0;
        double recency_score = 1.0 / (1.0 + tick_diff);

        int idx = result->count;
        result->items[idx].text    = sdsnew(g_history[i].line);
        result->items[idx].display = sdsnew(g_history[i].line);
        result->items[idx].score   = freq_score + recency_score;
        result->items[idx].source  = 2;
        result->count++;
    }
}

/* ---- 公开的 history 添加接口 ---- */
void edc_completion_add_history(const char *line) {
    if (!line || !line[0]) return;

    g_tick += 1.0;

    /* 查找是否已存在 */
    for (int i = 0; i < g_history_count; i++) {
        if (strcmp(g_history[i].line, line) == 0) {
            g_history[i].count++;
            g_history[i].last_used = g_tick;
            return;
        }
    }

    /* 扩容 */
    if (g_history_count >= g_history_cap) {
        g_history_cap = g_history_cap ? g_history_cap * 2 : 64;
        g_history = (history_entry_t *)realloc(g_history,
                        (size_t)g_history_cap * sizeof(history_entry_t));
    }

    g_history[g_history_count].line      = strdup(line);
    g_history[g_history_count].count     = 1;
    g_history[g_history_count].last_used = g_tick;
    g_history_count++;
}

/* ================================================================
 *  5. 模糊匹配 (Subsequence scoring, fzy-style)
 * ================================================================ */

double edc_completion_fuzzy_score(const char *pattern, const char *target) {
    if (!pattern || !target) return 0.0;

    size_t plen = strlen(pattern);
    size_t tlen = strlen(target);
    if (plen == 0) return 1.0;
    if (tlen == 0) return 0.0;
    if (plen > tlen) return 0.0;

    double score = 0.0;
    size_t pi = 0;
    int    prev_match = -1;
    int    first_match = -1;

    for (size_t ti = 0; ti < tlen && pi < plen; ti++) {
        if (tolower((unsigned char)target[ti]) == tolower((unsigned char)pattern[pi])) {
            /* 匹配点得分：
             *   - 首字母匹配 bonus
             *   - 连续匹配 bonus
             *   - gap penalty
             */
            double pt_score = 1.0;

            /* 驼峰/大写字母 bonus (类似 fzf) */
            if (isupper((unsigned char)target[ti]))
                pt_score += 0.5;

            /* 单词边界 bonus (前一个是分隔符) */
            if (ti > 0 && (target[ti-1] == '_' || target[ti-1] == '-' ||
                           target[ti-1] == '/' || target[ti-1] == ' '))
                pt_score += 0.5;

            /* 连续匹配 bonus */
            if (prev_match >= 0 && (int)ti == prev_match + 1)
                pt_score += 0.3;

            /* gap penalty */
            if (prev_match >= 0) {
                int gap = (int)ti - prev_match - 1;
                pt_score -= (double)gap * 0.05;
            }

            if (pt_score < 0) pt_score = 0;
            score += pt_score;
            prev_match = (int)ti;
            if (first_match < 0) first_match = (int)ti;
            pi++;
        }
    }

    /* 未完全匹配 */
    if (pi < plen) return 0.0;

    /* 归一化：除以 target 长度，加上匹配位置 bonus */
    score = score / (double)tlen;

    /* 匹配越靠前越好 */
    if (first_match >= 0)
        score += 0.2 * (1.0 - (double)first_match / (double)tlen);

    /* 前缀完全匹配 bonus */
    if (strncasecmp(target, pattern, plen) == 0)
        score += 0.3;

    if (score > 1.0) score = 1.0;
    return score;
}

/* ================================================================
 *  6. 候选排序 (按 score 降序)
 * ================================================================ */

static int candidate_cmp(const void *a, const void *b) {
    const edc_completion_candidate_t *ca = (const edc_completion_candidate_t *)a;
    const edc_completion_candidate_t *cb = (const edc_completion_candidate_t *)b;
    if (cb->score > ca->score) return 1;
    if (cb->score < ca->score) return -1;
    return strcmp(ca->text, cb->text);
}

static void result_sort(edc_completion_result_t *result) {
    qsort(result->items, (size_t)result->count,
          sizeof(edc_completion_candidate_t), candidate_cmp);
}

/* ================================================================
 *  7. 去重
 * ================================================================ */

static void result_dedup(edc_completion_result_t *result) {
    for (int i = 0; i < result->count; i++) {
        for (int j = i + 1; j < result->count; ) {
            if (strcmp(result->items[i].text, result->items[j].text) == 0) {
                /* 合并 score (取较高者) */
                if (result->items[j].score > result->items[i].score)
                    result->items[i].score = result->items[j].score;
                /* 移除 j */
                sdsfree(result->items[j].text);
                sdsfree(result->items[j].display);
                memmove(&result->items[j], &result->items[j+1],
                        (size_t)(result->count - j - 1) * sizeof(edc_completion_candidate_t));
                result->count--;
            } else {
                j++;
            }
        }
    }
}

/* ================================================================
 *  8. 公共前缀提取 (LCP)
 * ================================================================ */

sds edc_completion_lcp(edc_completion_result_t *result) {
    if (result->count == 0) return sdsempty();
    if (result->count == 1) return sdsdup(result->items[0].text);

    const char *first = result->items[0].text;
    size_t max_len = strlen(first);

    for (size_t len = 0; len < max_len; len++) {
        char c = first[len];
        for (int i = 1; i < result->count; i++) {
            if (len >= strlen(result->items[i].text) ||
                result->items[i].text[len] != c) {
                return sdsnewlen(first, len);
            }
        }
    }
    return sdsnew(first);
}

/* ================================================================
 *  9. 多源注册机制
 * ================================================================ */

#define MAX_SOURCES 8

static struct {
    edc_completion_source_fn fn;
    void                    *user_data;
} g_sources[MAX_SOURCES];
static int g_source_count = 0;

void edc_completion_register_source(edc_completion_source_fn source, void *user_data) {
    if (g_source_count < MAX_SOURCES) {
        g_sources[g_source_count].fn        = source;
        g_sources[g_source_count].user_data = user_data;
        g_source_count++;
    }
}

/* ================================================================
 *  10. 核心入口：收集补全候选项
 * ================================================================ */

void edc_completion_collect(const char *line_buffer,
                            int cursor_pos,
                            edc_completion_result_t *result) {
    memset(result, 0, sizeof(*result));
    result->selected = -1;

    if (!line_buffer || cursor_pos <= 0) return;

    /* 提取当前 token：从 cursor_pos 向左到空格/行首 */
    int start = cursor_pos;
    while (start > 0 && line_buffer[start - 1] != ' ')
        start--;
    int token_len = cursor_pos - start;
    if (token_len == 0) return;

    char *token = strndup(line_buffer + start, (size_t)token_len);
    result->token = sdsnew(token);

    /* 从所有注册源收集 */
    for (int i = 0; i < g_source_count; i++) {
        if (result->count >= EDC_COMPLETION_MAX) break;
        g_sources[i].fn(token, result, g_sources[i].user_data);
    }

    free(token);

    /* 如果没有前缀匹配结果，尝试模糊匹配 */
    if (result->count == 0 && result->token && sdslen(result->token) > 0) {
        /* 对 history 做模糊匹配（仅 history 源，因为命令/file 前缀已精确） */
        for (int i = 0; i < g_history_count && result->count < EDC_COMPLETION_MAX; i++) {
            double s = edc_completion_fuzzy_score(result->token, g_history[i].line);
            if (s > 0.3) {  /* 阈值 */
                int idx = result->count;
                result->items[idx].text    = sdsnew(g_history[i].line);
                result->items[idx].display = sdsnew(g_history[i].line);
                result->items[idx].score   = s * 5.0;
                result->items[idx].source  = 2;
                result->count++;
            }
        }
    }

    /* 去重 + 排序 */
    result_dedup(result);
    result_sort(result);

    /* 计算公共前缀 */
    result->common_prefix = edc_completion_lcp(result);
}

/* ================================================================
 *  11. 生命周期
 * ================================================================ */

void edc_completion_init(void) {
    g_cmd_trie = trie_node_new();
    g_source_count = 0;

    /* 默认注册三个源 */
    edc_completion_register_source(source_commands, NULL);
    edc_completion_register_source(source_files,   NULL);
    edc_completion_register_source(source_history,  NULL);
}

void edc_completion_free(void) {
    trie_free_node(g_cmd_trie);
    g_cmd_trie = NULL;
    g_source_count = 0;
    for (int i = 0; i < g_history_count; i++)
        free(g_history[i].line);
    free(g_history);
    g_history = NULL;
    g_history_count = 0;
    g_history_cap = 0;
}

void edc_completion_add_command(const char *command) {
    if (g_cmd_trie && command)
        trie_insert(g_cmd_trie, command);
}

void edc_completion_result_free(edc_completion_result_t *result) {
    if (!result) return;
    for (int i = 0; i < result->count; i++) {
        sdsfree(result->items[i].text);
        sdsfree(result->items[i].display);
    }
    sdsfree(result->common_prefix);
    sdsfree(result->token);
    memset(result, 0, sizeof(*result));
}
