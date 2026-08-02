#include "edc_string_utils.h"
#include "edc_actor.h"
#include "edc_channel_actor.h"
#include "edc_config.h"
#include "edc_file_utils.h"
#include <stdio.h>
#include <unistd.h>

static void edc_observe_channel_handler(edc_actor_t *actor, edc_actor_msg_t *message)
{
    switch (message->type)
    {
    case BC_ACTOR_MSG_CHANNEL_RECV: {
        struct edc_channel_message *input = message->data;
        fprintf(stderr, "[USER]: %s\n", input->content);

        char *content = input->content;
        input->content = edc_strdup_printf("%s%s", content, content);
        free(content);

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
    sds home_dir = edc_homedir();
    sds root = edc_path_join(home_dir, ".edgeclaw", NULL);
    struct edc_config *conf = edc_config_load(root);
    sdsfree(root);
    sdsfree(home_dir);

    edc_actor_t *actor = edc_channel_actor_new(&conf->channel, conf->workspace);
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
