#include "src/edc_actor.h"
#include "src/edc_config.h"
#include "src/edc_llm_actor.h"
#include <stdio.h>

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

        GString *content = resp->content;
        fprintf(stderr, "[ASSISTANT]: %s\n", content->str);
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
    const char *home_dir = g_get_home_dir();
    char *root = g_build_filename(home_dir, ".edgeclaw", NULL);
    struct edc_config *conf = edc_config_load(root);
    free(root);

    char *cert_pem = g_build_filename(conf->workspace, "cacert.pem", NULL);
    edc_network_async_t *http = edc_network_async_new(cert_pem);
    http->mgr.dns4.url = "udp://" DNS_SERVER_IP ":53";

    edc_actor_t *llm_actor = edc_llm_actor_new(&conf->llm_api_conf, http);
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

    struct edc_llm_actor_req *req = edc_llm_actor_req_new(msg_lst, NULL, NULL);
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
