#include "src/edc_actor.h"
#include "src/edc_channel_actor.h"
#include "src/edc_config.h"
#include "src/edc_network_async.h"
#include <stdio.h>

static void edc_observe_channel_handler(edc_actor_t *actor, edc_actor_msg_t *message)
{
    switch (message->type)
    {
    case BC_ACTOR_MSG_CHANNEL_RECV: {
        struct edc_channel_message *input = message->data;
        fprintf(stderr, "[USER]: %s\n", input->content);

        gchar *content = input->content;
        input->content = g_strdup_printf("%s%s", content, content);
        g_free(content);

        message->type = BC_ACTOR_MSG_CHANNEL_SEND;
        edc_actor_send(message->from, message);
        break;
    }
    default:
        break;
    }
}

int main()
{
    mg_log_set(MG_LL_DEBUG);
    const gchar *home_dir = g_get_home_dir();
    gchar *root = g_build_filename(home_dir, ".edgeclaw", NULL);
    struct edc_config *conf = edc_config_load(root);
    g_free(root);

    gchar *cert_pem = g_build_filename(conf->workspace, "cacert.pem", NULL);
    edc_network_async_t *http = edc_network_async_new(cert_pem);
    // http->mgr.dns4.url = "udp://114.114.114.114:53";
    http->mgr.dns4.url = "udp://10.33.25.62:53";

    edc_actor_t *actor = edc_channel_actor_new(&conf->channel, http);
    edc_actor_t *observer = edc_actor_new("observer", edc_observe_channel_handler, NULL);
    edc_channel_actor_set_observer(actor, observer);

    edc_actor_runner_t *runner = edc_actor_runner_new();
    edc_actor_runner_add(runner, actor);
    edc_actor_runner_add(runner, observer);
    edc_actor_runner_start(runner);

    while (1)
    {
        sleep(10);
    }

    edc_actor_runner_free(runner);
    return 0;
}
