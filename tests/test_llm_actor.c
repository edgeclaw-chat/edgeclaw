#include "edc_string.h"
#include "edc_actor.h"
#include "edc_config.h"
#include "edc_file_utils.h"
#include "edc_llm_actor.h"
#include "edc_secret.h"
#include <stdio.h>
#include <unistd.h>

static void edc_observe_llm_handler(edc_actor_t *actor, edc_actor_msg_t *message)
{
    switch (message->type)
    {
    case BC_ACTOR_MSG_LLM_RESPONSE: {
        struct edc_llm_actor_resp *resp = message->data;
        if (resp->error)
        {
            fprintf(stderr, "[ERROR]: %s\n", resp->error->message);
            break;
        }

        sds content = resp->content;
        fprintf(stderr, "[ASSISTANT]: %s\n", content);
        break;
    }
    default:
        break;
    }

    edc_actor_msg_free(message);
}

#ifndef DNS_SERVER_IP
#define DNS_SERVER_IP "114.114.114.114"
#endif
int main(void)
{
    sds home_dir = edc_homedir();
    sds root = edc_path_join(home_dir, ".edgeclaw", NULL);

    if (!edc_secret_init(false, root))
    {
        fprintf(stderr, "Failed to initialize secret module");
        sdsfree(root);
        return -1;
    }
    struct edc_config *conf = edc_config_load(root);
    sdsfree(root);
    sdsfree(home_dir);

    edc_actor_t *llm_actor = edc_llm_actor_new(conf);
    edc_actor_t *observe_actor = edc_actor_new("edc_observer", edc_observe_llm_handler, NULL);

    edc_actor_runner_t *runner = edc_actor_runner_new();
    edc_actor_runner_add(runner, llm_actor);
    edc_actor_runner_add(runner, observe_actor);
    edc_actor_runner_start(runner);

    cJSON *msg_lst = cJSON_CreateArray();
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "role", "user");
    cJSON_AddStringToObject(msg, "content", "什么是敏捷开发?");
    cJSON_AddItemToArray(msg_lst, msg);

    /* Two arguments, not three: fe9e7e9 ("make the database the session's only copy")
     * removed the trailing EdcQueue *turn -- a turn is rows in the database now, not an
     * in-memory queue handed to the request. The call used to pass NULL for both `tools`
     * and `turn`, so dropping the last one is exactly equivalent. */
    struct edc_llm_actor_req *req = edc_llm_actor_req_new(msg_lst, NULL);
    edc_actor_send(llm_actor, edc_actor_msg_new(BC_ACTOR_MSG_LLM_REQUEST, observe_actor, NULL, req,
                                                (edc_actor_data_free_fn)edc_llm_actor_req_free));

    while (1)
    {
        sleep(10);
    }

    edc_actor_free(observe_actor);
    edc_actor_free(llm_actor);

    return 0;
}
