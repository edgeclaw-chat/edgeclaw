#include "log.h"
#include "src/edc_actor.h"
#include "src/edc_config.h"
#include "src/edc_file_utils.h"
#include "src/edc_llm_actor.h"
#include "src/edc_memory_actor.h"
#include "src/edc_secret.h"

static void observer_handler(edc_actor_t *actor, edc_actor_msg_t *msg)
{
    switch (msg->type)
    {
    case BC_ACTOR_MSG_MEMORY_DONE: {
        printf("memory done\n");
        exit(0);
        break;
    }
    }
}

int main()
{
    sds home_dir = edc_homedir();
    sds root = edc_path_join(home_dir, ".edgeclaw", NULL);

    if (!edc_secret_init(FALSE, root))
    {
        log_error("Failed to initialize secret module");
        sdsfree(root);
        sdsfree(home_dir);
        return -1;
    }

    struct edc_config *conf = edc_config_load(root);

    edc_secret_lockdown();

    sds cacert_path = edc_path_join(conf->workspace, "cacert.pem", NULL);
    edc_network_async_t *http = edc_network_async_new(cacert_path);
    sdsfree(cacert_path);
    edc_actor_t *llm_actor = edc_llm_actor_new(&conf->llm_api_conf, http);
    edc_actor_t *actor = edc_memory_actor_new(conf->workspace, 9, llm_actor);

    edc_actor_t *observer = edc_actor_new("observer", observer_handler, NULL);

    edc_actor_runner_t *runner = edc_actor_runner_new();
    edc_actor_runner_add(runner, llm_actor);
    edc_actor_runner_add(runner, actor);
    edc_actor_runner_add(runner, observer);

    edc_actor_runner_start(runner);
    edc_actor_send(actor, edc_actor_msg_new(BC_ACTOR_MSG_MEMORY_COMPRESS, observer, NULL, NULL, NULL));
    while (TRUE)
    {
        sleep(10);
    }

    edc_actor_runner_stop(runner);
    edc_actor_free(actor);
    edc_actor_free(llm_actor);
    edc_actor_runner_free(runner);
    edc_network_async_free(http);
    return 0;
}
