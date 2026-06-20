#ifndef __BC_LLM_TOOLBOX_ACTOR_H__
#define __BC_LLM_TOOLBOX_ACTOR_H__

#include "edc_actor.h"
#include "edc_toolbox.h"

typedef struct
{
    struct edc_toolbox *toolbox;
} edc_toolbox_actor_state_t;

struct edc_channel;
struct edc_toolbox_actor_req
{
    gchar *name;
    cJSON *args;
    gchar *call_id;
    struct edc_channel *call_from;
    GString *result;
    struct edc_toolbox_actor_req *next;
};

extern edc_actor_t *edc_toolbox_actor_new(struct edc_toolbox *toolbox);
extern void edc_toolbox_actor_free(edc_actor_t *actor);
extern struct edc_toolbox_actor_req *edc_toolbox_actor_req_new(gchar *name, cJSON *args, gchar *call_id,
                                                             struct edc_channel *call_from);
extern void edc_toolbox_actor_req_free(struct edc_toolbox_actor_req *req);
#endif
