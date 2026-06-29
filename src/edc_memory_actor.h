#ifndef __BC_MEMORY_ACTOR_H__
#define __BC_MEMORY_ACTOR_H__

#include "edc_actor.h"
#include "cJSON.h"

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
};

extern edc_actor_t *edc_memory_actor_new(const char *workspace, const size_t history_window, edc_actor_t *llm_actor);
extern void edc_memory_actor_free(edc_actor_t *actor);
extern bool edc_memory_actor_is_compressing(edc_actor_t *actor);
#endif
