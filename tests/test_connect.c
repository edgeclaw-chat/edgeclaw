#include "cJSON.h"
#include "edc_config.h"
#include "edc_error.h"
#include "edc_file_utils.h"
#include "edc_secret.h"
#include "sds.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* mkdtemp lives in <unistd.h> on macOS and in <stdlib.h> on glibc; including both is
 * the portable answer. */
#include <unistd.h>
#include <uv.h>

static int passed = 0;
static int failed = 0;

#define CHECK(cond, msg)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        if (cond)                                                                                                      \
            passed++;                                                                                                  \
        else                                                                                                           \
        {                                                                                                              \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);                                              \
            failed++;                                                                                                  \
        }                                                                                                              \
    } while (0)

int main(void)
{
    setenv("EDGECLAW_MASTER_KEY", "test-master-key", 1);
    edc_file_utils_init(uv_default_loop());

    char tmpl[] = "/tmp/edc_connect_XXXXXX";
    char *home = mkdtemp(tmpl);
    if (!home)
    {
        fprintf(stderr, "FAIL: mkdtemp\n");
        return 1;
    }

    sds cfg = edc_path_join(home, "config.json", NULL);
    EdcError *error = NULL;
    const char *initial = "{\n  \"api_url\": \"https://old/v1\",\n  \"model\": \"old-model\"\n}\n";
    CHECK(edc_file_write(cfg, initial, -1, &error), "write initial config");
    CHECK(edc_secret_init(false, home), "secret init from env key");

    /* Full update: url + model + key (key must be encrypted on disk). */
    CHECK(edc_config_update_llm(home, "https://new/v1", "new-model", "sk-secret"), "update_llm full");

    sds back = edc_file_read(cfg, &error);
    CHECK(back != NULL, "read config back");
    cJSON *j = back ? cJSON_Parse(back) : NULL;
    CHECK(j != NULL, "parse config back");
    if (j)
    {
        cJSON *u = cJSON_GetObjectItem(j, "api_url");
        CHECK(u && cJSON_IsString(u) && strcmp(u->valuestring, "https://new/v1") == 0, "api_url updated");
        cJSON *m = cJSON_GetObjectItem(j, "model");
        CHECK(m && cJSON_IsString(m) && strcmp(m->valuestring, "new-model") == 0, "model updated");
        cJSON *k = cJSON_GetObjectItem(j, "api_key");
        CHECK(k && cJSON_IsString(k) && strncmp(k->valuestring, "EDGECLAW_ENC:", 13) == 0, "api_key stored encrypted");
        sds dec = edc_secret_decrypt(k && cJSON_IsString(k) ? k->valuestring : "");
        CHECK(dec && strcmp(dec, "sk-secret") == 0, "api_key round-trips through decrypt");
        sdsfree(dec);
        cJSON_Delete(j);
    }

    /* Partial update: NULL leaves a field untouched. */
    CHECK(edc_config_update_llm(home, NULL, "model-2", NULL), "update_llm partial");
    sds back2 = edc_file_read(cfg, &error);
    cJSON *j2 = back2 ? cJSON_Parse(back2) : NULL;
    CHECK(j2 != NULL, "parse config back (2)");
    if (j2)
    {
        cJSON *u = cJSON_GetObjectItem(j2, "api_url");
        CHECK(u && cJSON_IsString(u) && strcmp(u->valuestring, "https://new/v1") == 0, "url unchanged by NULL");
        cJSON *m = cJSON_GetObjectItem(j2, "model");
        CHECK(m && cJSON_IsString(m) && strcmp(m->valuestring, "model-2") == 0, "model updated again");
        CHECK(cJSON_GetObjectItem(j2, "api_key") != NULL, "api_key still present");
        cJSON_Delete(j2);
    }

    sdsfree(back2);
    sdsfree(back);
    sdsfree(cfg);
    edc_secret_cleanup();

    printf("test_connect: %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
