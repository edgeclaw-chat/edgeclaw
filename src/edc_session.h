#ifndef __BC_SESSION_H__
#define __BC_SESSION_H__

#include <cJSON.h>
#include <glib.h>

struct edc_config;

struct edc_session
{
    cJSON *messages;
    struct edc_config *config;
    int save_skip_count;
    GList *timestamps;
};

struct edc_turn;
extern struct edc_session *edc_session_new(struct edc_config *config);
extern void edc_session_reload(struct edc_session *session, gboolean load_history);
extern void edc_session_free(struct edc_session *session);

extern void edc_session_add_turn(struct edc_session *sess, struct edc_turn *turn);

extern cJSON *edc_session_get_messages(struct edc_session *session);
extern gsize edc_session_get_message_count(struct edc_session *session);
extern gboolean edc_session_save_history(struct edc_session *session);

#endif
