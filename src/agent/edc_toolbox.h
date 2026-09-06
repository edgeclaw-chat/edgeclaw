#include "sds.h"
#ifndef __BC_TOOLBOX_H__
#define __BC_TOOLBOX_H__

#include "edc_actor.h"
#include "edc_config.h"
#include "edc_error.h"
#include <cJSON.h>

struct edc_tool;

struct edc_toolbox
{
    struct edc_tool *tools;
    struct edc_config *conf;
    cJSON *schemas;
    edc_actor_t *scheduler;
    edc_actor_t *channel_actor;
};

struct edc_toolbox_call_ctx
{
    struct edc_toolbox *tb;
    edc_channel_type call_from_type;
    void *priv;
};

extern struct edc_toolbox_call_ctx *edc_toolbox_call_ctx_new(struct edc_toolbox *tb, edc_channel_type channel_type,
                                                             void *priv);
extern void edc_toolbox_call_ctx_free(struct edc_toolbox_call_ctx *);
extern struct edc_toolbox *edc_toolbox_new(struct edc_config *conf, edc_actor_t *scheduler, edc_actor_t *channel_actor);
extern void edc_toolbox_free(struct edc_toolbox *tb);

extern void edc_toolbox_register(struct edc_toolbox *tb, struct edc_tool *tool);
extern void edc_toolbox_register_defaults(struct edc_toolbox *tb);

extern void edc_toolbox_exec(struct edc_toolbox_call_ctx *, const char *name, cJSON *args,
                             void (*callback)(struct edc_toolbox_call_ctx *, sds, EdcError *));
#endif
