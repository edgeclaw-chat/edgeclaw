#include "edc_cli_history.h"
#include "sds.h"
#include <stdlib.h>
#include <string.h>

/* 进程一份 (见头文件: 前端状态, 不是运行时装配)。初始容量与老实现一致 (8),
 * 增长策略也一致 (翻倍) —— 只把存储从 `struct edc_app` 里挪出来。 */
static sds *g_history = NULL;
static int g_history_count = 0;
static int g_history_capacity = 0;

void edc_cli_history_add(const char *line)
{
    if (!line || !*line)
        return;
    if (g_history_count > 0 && strcmp(g_history[g_history_count - 1], line) == 0)
        return;

    if (g_history_count >= g_history_capacity)
    {
        g_history_capacity = g_history_capacity ? g_history_capacity * 2 : 8;
        g_history = realloc(g_history, (size_t)g_history_capacity * sizeof(sds));
    }
    g_history[g_history_count++] = sdsnew(line);
}

int edc_cli_history_count(void)
{
    return g_history_count;
}

const char *edc_cli_history_at(int index)
{
    if (index < 0 || index >= g_history_count)
        return NULL;
    return g_history[index];
}

void edc_cli_history_clear(void)
{
    for (int i = 0; i < g_history_count; i++)
        sdsfree(g_history[i]);
    free(g_history);
    g_history = NULL;
    g_history_count = 0;
    g_history_capacity = 0;
}
