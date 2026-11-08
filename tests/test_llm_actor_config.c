/* Regression test for the app's provider update (TUI /connect) abort.
 *
 * edc_llm_actor_new() used to shallow-copy config->llm_api_conf, so the actor
 * shared sds pointers with the global config. When the app updated the config
 * (freeing/replacing those strings via edc_app_set_llm_config) and then
 * notified the actor, the actor's BC_ACTOR_MSG_LLM_SET_CONFIG handler called
 * sdsfree() on the already-freed pointer -> double free -> abort() in
 * malloc_printerr().
 *
 * This test drives the same sequence through the actor runner. With the bug
 * present it aborts; with the fix it completes and the actor owns its own copy.
 */
#include "edc_actor.h"
#include "edc_compat.h"
#include "edc_config.h"
#include "edc_llm_actor.h"
#include "sds.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int failures = 0;

#define CHECK(cond, msg)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            fprintf(stderr, "FAIL: %s\n", msg);                                                                        \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

int main(void)
{
    struct edc_config *cfg = g_new0(struct edc_config, 1);
    /* TEST_TMPDIR, not "/tmp": this is a workspace the code may touch, and "/tmp" does not
     * exist on Windows. */
    {
        const char *td = getenv("TEST_TMPDIR");
        if (!td || !*td)
            td = getenv("TMPDIR");
#ifdef _WIN32
        if (!td || !*td)
            td = getenv("TEMP");
#endif
        if (!td || !*td)
            td = ".";
        cfg->workspace = sdsnew(td);
    }
    cfg->llm_api_conf.api_url = sdsnew("https://old.example/v1");
    cfg->llm_api_conf.model = sdsnew("old-model");
    cfg->llm_api_conf.api_key = sdsnew("old-key");

    edc_actor_t *llm = edc_llm_actor_new(cfg);

    /* The actor must not alias the config's strings. */
    edc_llm_actor_state_t *st = llm->state;
    CHECK(st->api_conf.api_url != cfg->llm_api_conf.api_url, "actor url is not aliased to config");
    CHECK(st->api_conf.model != cfg->llm_api_conf.model, "actor model is not aliased to config");
    CHECK(st->api_conf.api_key != cfg->llm_api_conf.api_key, "actor key is not aliased to config");

    edc_actor_runner_t *runner = edc_actor_runner_new();
    edc_actor_runner_add(runner, llm);
    edc_actor_runner_start(runner);
    usleep(100 * 1000);

    /* Simulate edc_app_set_llm_config(): the app frees/replaces the config
     * strings first, then hands the new values to the actor through its mailbox.
     * With the shared-pointer bug, freeing the config string here leaves the
     * actor's api_conf dangling; the handler below then double-frees it. */
    sdsfree(cfg->llm_api_conf.api_url);
    cfg->llm_api_conf.api_url = sdsnew("https://new.example/v1");
    sdsfree(cfg->llm_api_conf.model);
    cfg->llm_api_conf.model = sdsnew("new-model");
    sdsfree(cfg->llm_api_conf.api_key);
    cfg->llm_api_conf.api_key = sdsnew("new-key");

    struct edc_llm_api_conf_update *u = g_new0(struct edc_llm_api_conf_update, 1);
    u->api_url = sdsnew("https://new.example/v1");
    u->model = sdsnew("new-model");
    u->api_key = sdsnew("new-key");
    edc_actor_send(llm, edc_actor_msg_new(BC_ACTOR_MSG_LLM_SET_CONFIG, NULL, NULL, u,
                                          (edc_actor_data_free_fn)edc_llm_api_conf_update_free));

    usleep(200 * 1000);

    CHECK(st->api_conf.api_url && strcmp(st->api_conf.api_url, "https://new.example/v1") == 0, "actor url updated");
    CHECK(st->api_conf.model && strcmp(st->api_conf.model, "new-model") == 0, "actor model updated");
    CHECK(st->api_conf.api_key && strcmp(st->api_conf.api_key, "new-key") == 0, "actor key updated");

    edc_actor_runner_free(runner);
    edc_llm_actor_free(llm);

    sdsfree(cfg->llm_api_conf.api_url);
    sdsfree(cfg->llm_api_conf.model);
    sdsfree(cfg->llm_api_conf.api_key);
    free(cfg);

    printf("test_llm_actor_config: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
