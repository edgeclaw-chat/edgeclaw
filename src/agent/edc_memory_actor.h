#ifndef __BC_MEMORY_ACTOR_H__
#define __BC_MEMORY_ACTOR_H__

#include "cJSON.h"
#include "edc_actor.h"

struct edc_task_group;

struct edc_memory_actor_state
{
    const char *workspace;
    edc_actor_t *llm_actor;
    edc_actor_msg_t *compress_msg;
    cJSON *compress_msg_lst;
    cJSON *compress_tools;
    size_t compress_index;
    size_t history_window;
    bool compressing;

    /* group fold (L1 折叠: task turns → group memory) */
    struct edc_task_group *fold_group; /* 正在折叠的 group (非 NULL 表示折叠中) */
    edc_actor_msg_t *fold_msg;         /* 原始 GROUP_FOLD 消息 (保留 from/request_id) */
    uint32_t fold_max_seq;             /* 本次折叠的 completion_seq 上限 */
};

extern edc_actor_t *edc_memory_actor_new(const char *workspace, const size_t history_window, edc_actor_t *llm_actor);
extern void edc_memory_actor_free(edc_actor_t *actor);
extern bool edc_memory_actor_is_compressing(edc_actor_t *actor);
#endif
