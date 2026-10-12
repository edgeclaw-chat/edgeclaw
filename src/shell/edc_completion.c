#include "edc_completion.h"
#include "edc_compat.h"
#include <ctype.h>
#include <dirent.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* ================================================================
 *  1. Trie -- fast prefix matching for commands/subcommands
 * ================================================================ */

#define TRIE_CHILDREN 128 /* the ASCII range is enough */

typedef struct trie_node
{
    struct trie_node *children[TRIE_CHILDREN];
    bool is_terminal; /* whether it is a complete command */
    int freq;         /* usage frequency */
} trie_node_t;

static trie_node_t *g_cmd_trie = NULL;

static trie_node_t *trie_node_new(void)
{
    trie_node_t *n = (trie_node_t *)calloc(1, sizeof(trie_node_t));
    return n;
}

static void trie_insert(trie_node_t *root, const char *key)
{
    trie_node_t *cur = root;
    for (const unsigned char *p = (const unsigned char *)key; *p; p++)
    {
        int idx = *p;
        if (idx >= TRIE_CHILDREN)
            idx = TRIE_CHILDREN - 1;
        if (!cur->children[idx])
            cur->children[idx] = trie_node_new();
        cur = cur->children[idx];
    }
    cur->is_terminal = true;
    cur->freq++;
}

/* Recursively collect every complete word rooted at node */
static void trie_collect(trie_node_t *node, sds prefix, edc_completion_result_t *result)
{
    if (!node)
        return;
    if (node->is_terminal && result->count < EDC_COMPLETION_MAX)
    {
        int idx = result->count;
        result->items[idx].text = sdsdup(prefix);
        result->items[idx].display = sdsdup(prefix);
        result->items[idx].score = (double)node->freq;
        result->items[idx].source = 0;
        result->count++;
    }
    for (int i = 0; i < TRIE_CHILDREN; i++)
    {
        if (node->children[i])
        {
            char ch = (char)i;
            sds child_prefix = sdscatlen(sdsdup(prefix), &ch, 1);
            trie_collect(node->children[i], child_prefix, result);
        }
    }
}

/* Prefix search: find the node for prefix, then collect all child nodes */
static void trie_prefix_search(trie_node_t *root, const char *prefix, edc_completion_result_t *result)
{
    trie_node_t *cur = root;
    for (const unsigned char *p = (const unsigned char *)prefix; *p; p++)
    {
        int idx = *p;
        if (idx >= TRIE_CHILDREN)
            idx = TRIE_CHILDREN - 1;
        if (!cur->children[idx])
            return; /* no match */
        cur = cur->children[idx];
    }
    /* cur is the node for prefix; collect all child nodes */
    sds p = sdsnew(prefix);
    trie_collect(cur, p, result);
}

static void trie_free_node(trie_node_t *node)
{
    if (!node)
        return;
    for (int i = 0; i < TRIE_CHILDREN; i++)
        trie_free_node(node->children[i]);
    free(node);
}

/* ================================================================
 *  2. Command completion source -- the internal Trie
 * ================================================================ */

static void source_commands(const char *token, edc_completion_result_t *result, void *user_data)
{
    (void)user_data;
    if (!g_cmd_trie || !token || token[0] != '/')
        return;
    /* token looks like "/tas"; feed it straight to the trie */
    trie_prefix_search(g_cmd_trie, token, result);
}

/* ================================================================
 *  3. File-path completion source
 * ================================================================ */

/* Split a path into its directory part and file-prefix part */
static void path_split(const char *token, sds *dir_part, sds *file_part)
{
    const char *last_slash = strrchr(token, '/');
    if (last_slash)
    {
        *dir_part = sdsnewlen(token, (size_t)(last_slash - token + 1)); /* includes the / */
        *file_part = sdsnew(last_slash + 1);
    }
    else
    {
        *dir_part = sdsnew("./"); /* the current directory */
        *file_part = sdsnew(token);
    }
}

static void source_files(const char *token, edc_completion_result_t *result, void *user_data)
{
    (void)user_data;
    if (!token || !token[0])
        return;

    sds dir_part, file_part;
    path_split(token, &dir_part, &file_part);

    DIR *d = opendir(dir_part);
    if (!d)
    {
        sdsfree(dir_part);
        sdsfree(file_part);
        return;
    }

    size_t file_len = sdslen(file_part);
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL && result->count < EDC_COMPLETION_MAX)
    {
        if (entry->d_name[0] == '.')
            continue; /* skip hidden files */

        /* prefix match */
        if (strncmp(entry->d_name, file_part, file_len) != 0)
            continue;

        int idx = result->count;
        /* build the full path */
        sds full = sdscatprintf(sdsdup(dir_part), "%s", entry->d_name);

        /* append a / suffix when it is a directory */
        struct stat st;
        bool is_dir = false;
        sds display_path = sdsdup(full);
        if (stat(full, &st) == 0 && S_ISDIR(st.st_mode))
        {
            is_dir = true;
            full = sdscat(full, "/");
            display_path = sdscat(display_path, "/");
        }

        result->items[idx].text = full;
        result->items[idx].display = display_path;
        /* directories rank slightly higher */
        result->items[idx].score = is_dir ? 2.0 : 1.0;
        result->items[idx].source = 1;
        result->count++;
    }
    closedir(d);
    sdsfree(dir_part);
    sdsfree(file_part);
}

/* ================================================================
 *  4. History completion source -- with Frecency scoring
 * ================================================================ */

#define HISTORY_MAX_ENTRIES 2000

typedef struct
{
    char *line;
    int count;        /* usage count */
    double last_used; /* monotonically increasing timestamp */
} history_entry_t;

static history_entry_t *g_history = NULL;
static int g_history_count = 0;
static int g_history_cap = 0;
static double g_tick = 0.0;

static void source_history(const char *token, edc_completion_result_t *result, void *user_data)
{
    (void)user_data;
    if (!token || !token[0])
        return;

    size_t tlen = strlen(token);

    for (int i = 0; i < g_history_count && result->count < EDC_COMPLETION_MAX; i++)
    {
        /* fast prefix filter */
        if (strncmp(g_history[i].line, token, tlen) != 0)
            continue;

        /* Frecency score:
         *   score = log2(1 + count) * 2.0  +  recency_bonus
         *   recency_bonus = 1.0 / (1.0 + tick_diff)  (the more recent, the higher)
         */
        double freq_score = 2.0 * log2(1.0 + (double)g_history[i].count);
        double tick_diff = g_tick - g_history[i].last_used;
        if (tick_diff < 0)
            tick_diff = 0;
        double recency_score = 1.0 / (1.0 + tick_diff);

        int idx = result->count;
        result->items[idx].text = sdsnew(g_history[i].line);
        result->items[idx].display = sdsnew(g_history[i].line);
        result->items[idx].score = freq_score + recency_score;
        result->items[idx].source = 2;
        result->count++;
    }
}

/* ---- public history-add interface ---- */
void edc_completion_add_history(const char *line)
{
    if (!line || !line[0])
        return;

    g_tick += 1.0;

    /* check whether it already exists */
    for (int i = 0; i < g_history_count; i++)
    {
        if (strcmp(g_history[i].line, line) == 0)
        {
            g_history[i].count++;
            g_history[i].last_used = g_tick;
            return;
        }
    }

    /* grow */
    if (g_history_count >= g_history_cap)
    {
        g_history_cap = g_history_cap ? g_history_cap * 2 : 64;
        g_history = (history_entry_t *)realloc(g_history, (size_t)g_history_cap * sizeof(history_entry_t));
    }

    g_history[g_history_count].line = strdup(line);
    g_history[g_history_count].count = 1;
    g_history[g_history_count].last_used = g_tick;
    g_history_count++;
}

/* ================================================================
 *  5. Fuzzy matching (subsequence scoring, fzy-style)
 * ================================================================ */

double edc_completion_fuzzy_score(const char *pattern, const char *target)
{
    if (!pattern || !target)
        return 0.0;

    size_t plen = strlen(pattern);
    size_t tlen = strlen(target);
    if (plen == 0)
        return 1.0;
    if (tlen == 0)
        return 0.0;
    if (plen > tlen)
        return 0.0;

    double score = 0.0;
    size_t pi = 0;
    int prev_match = -1;
    int first_match = -1;

    for (size_t ti = 0; ti < tlen && pi < plen; ti++)
    {
        if (tolower((unsigned char)target[ti]) == tolower((unsigned char)pattern[pi]))
        {
            /* Match-point score:
             *   - first-letter match bonus
             *   - consecutive-match bonus
             *   - gap penalty
             */
            double pt_score = 1.0;

            /* camelCase/uppercase bonus (like fzf) */
            if (isupper((unsigned char)target[ti]))
                pt_score += 0.5;

            /* word-boundary bonus (the previous char is a separator) */
            if (ti > 0 &&
                (target[ti - 1] == '_' || target[ti - 1] == '-' || target[ti - 1] == '/' || target[ti - 1] == ' '))
                pt_score += 0.5;

            /* consecutive-match bonus */
            if (prev_match >= 0 && (int)ti == prev_match + 1)
                pt_score += 0.3;

            /* gap penalty */
            if (prev_match >= 0)
            {
                int gap = (int)ti - prev_match - 1;
                pt_score -= (double)gap * 0.05;
            }

            if (pt_score < 0)
                pt_score = 0;
            score += pt_score;
            prev_match = (int)ti;
            if (first_match < 0)
                first_match = (int)ti;
            pi++;
        }
    }

    /* not a full match */
    if (pi < plen)
        return 0.0;

    /* Normalize: divide by the target length, plus a match-position bonus */
    score = score / (double)tlen;

    /* the earlier the match, the better */
    if (first_match >= 0)
        score += 0.2 * (1.0 - (double)first_match / (double)tlen);

    /* exact-prefix-match bonus */
    if (strncasecmp(target, pattern, plen) == 0)
        score += 0.3;

    if (score > 1.0)
        score = 1.0;
    return score;
}

/* ================================================================
 *  6. Candidate sorting (by descending score)
 * ================================================================ */

static int candidate_cmp(const void *a, const void *b)
{
    const edc_completion_candidate_t *ca = (const edc_completion_candidate_t *)a;
    const edc_completion_candidate_t *cb = (const edc_completion_candidate_t *)b;
    if (cb->score > ca->score)
        return 1;
    if (cb->score < ca->score)
        return -1;
    return strcmp(ca->text, cb->text);
}

static void result_sort(edc_completion_result_t *result)
{
    qsort(result->items, (size_t)result->count, sizeof(edc_completion_candidate_t), candidate_cmp);
}

/* ================================================================
 *  7. Dedup
 * ================================================================ */

static void result_dedup(edc_completion_result_t *result)
{
    for (int i = 0; i < result->count; i++)
    {
        for (int j = i + 1; j < result->count;)
        {
            if (strcmp(result->items[i].text, result->items[j].text) == 0)
            {
                /* merge the score (take the higher) */
                if (result->items[j].score > result->items[i].score)
                    result->items[i].score = result->items[j].score;
                /* remove j */
                sdsfree(result->items[j].text);
                sdsfree(result->items[j].display);
                memmove(&result->items[j], &result->items[j + 1],
                        (size_t)(result->count - j - 1) * sizeof(edc_completion_candidate_t));
                result->count--;
            }
            else
            {
                j++;
            }
        }
    }
}

/* ================================================================
 *  8. Common-prefix extraction (LCP)
 * ================================================================ */

sds edc_completion_lcp(edc_completion_result_t *result)
{
    if (result->count == 0)
        return sdsempty();
    if (result->count == 1)
        return sdsdup(result->items[0].text);

    const char *first = result->items[0].text;
    size_t max_len = strlen(first);

    for (size_t len = 0; len < max_len; len++)
    {
        char c = first[len];
        for (int i = 1; i < result->count; i++)
        {
            if (len >= strlen(result->items[i].text) || result->items[i].text[len] != c)
            {
                return sdsnewlen(first, len);
            }
        }
    }
    return sdsnew(first);
}

/* ================================================================
 *  9. Multi-source registration
 * ================================================================ */

#define MAX_SOURCES 8

static struct
{
    edc_completion_source_fn fn;
    void *user_data;
} g_sources[MAX_SOURCES];
static int g_source_count = 0;

void edc_completion_register_source(edc_completion_source_fn source, void *user_data)
{
    if (g_source_count < MAX_SOURCES)
    {
        g_sources[g_source_count].fn = source;
        g_sources[g_source_count].user_data = user_data;
        g_source_count++;
    }
}

/* ================================================================
 *  10. Core entry: collect completion candidates
 * ================================================================ */

void edc_completion_collect(const char *line_buffer, int cursor_pos, edc_completion_result_t *result)
{
    memset(result, 0, sizeof(*result));
    result->selected = -1;

    if (!line_buffer || cursor_pos <= 0)
        return;

    /* Extract the current token: from cursor_pos left to a space / start of line */
    int start = cursor_pos;
    while (start > 0 && line_buffer[start - 1] != ' ')
        start--;
    int token_len = cursor_pos - start;
    if (token_len == 0)
        return;

    char *token = strndup(line_buffer + start, (size_t)token_len);
    result->token = sdsnew(token);

    /* collect from every registered source */
    for (int i = 0; i < g_source_count; i++)
    {
        if (result->count >= EDC_COMPLETION_MAX)
            break;
        g_sources[i].fn(token, result, g_sources[i].user_data);
    }

    free(token);

    /* if there is no prefix match, try fuzzy matching */
    if (result->count == 0 && result->token && sdslen(result->token) > 0)
    {
        /* fuzzy-match against history only (commands/files already prefix-match exactly) */
        for (int i = 0; i < g_history_count && result->count < EDC_COMPLETION_MAX; i++)
        {
            double s = edc_completion_fuzzy_score(result->token, g_history[i].line);
            if (s > 0.3)
            { /* threshold */
                int idx = result->count;
                result->items[idx].text = sdsnew(g_history[i].line);
                result->items[idx].display = sdsnew(g_history[i].line);
                result->items[idx].score = s * 5.0;
                result->items[idx].source = 2;
                result->count++;
            }
        }
    }

    /* dedup + sort */
    result_dedup(result);
    result_sort(result);

    /* compute the common prefix */
    result->common_prefix = edc_completion_lcp(result);
}

/* ================================================================
 *  11. Lifetime
 * ================================================================ */

void edc_completion_init(void)
{
    g_cmd_trie = trie_node_new();
    g_source_count = 0;

    /* register the three default sources */
    edc_completion_register_source(source_commands, NULL);
    edc_completion_register_source(source_files, NULL);
    edc_completion_register_source(source_history, NULL);
}

void edc_completion_free(void)
{
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

void edc_completion_add_command(const char *command)
{
    if (g_cmd_trie && command)
        trie_insert(g_cmd_trie, command);
}

void edc_completion_result_free(edc_completion_result_t *result)
{
    if (!result)
        return;
    for (int i = 0; i < result->count; i++)
    {
        sdsfree(result->items[i].text);
        sdsfree(result->items[i].display);
    }
    sdsfree(result->common_prefix);
    sdsfree(result->token);
    memset(result, 0, sizeof(*result));
}
