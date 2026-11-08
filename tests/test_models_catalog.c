/* The AI-model catalog read API (edc_models).
 *
 * The catalog itself is a GENERATED build input (tools/gen_models.py), not committed, so
 * these tests do NOT assume its contents: the API is exercised against a fixture
 * (EDGECLAW_MODELS_CATALOG), and the embedded copy is only checked to load safely (it is
 * an empty catalog on a clean checkout). */

#include "edc_compat.h"
#include "edc_models.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int test_failed = 0;
static int test_passed = 0;

#define TEST(name)                                                                                                     \
    do                                                                                                                 \
    {                                                                                                                  \
        log_info("  TEST: %s", name);                                                                                  \
    } while (0)
#define CHECK(cond, msg)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            log_error("    FAIL: %s", msg);                                                                            \
            test_failed++;                                                                                             \
        }                                                                                                              \
        else                                                                                                           \
        {                                                                                                              \
            test_passed++;                                                                                             \
        }                                                                                                              \
    } while (0)

static const char *kFixture = "{\n"
                              "  \"models\": {\n"
                              "    \"m-alpha\": {\"name\":\"Alpha\",\"context\":1000,\"output\":100,"
                              "\"tool_call\":true,\"reasoning\":true},\n"
                              "    \"m-beta\": {\"name\":\"Beta\",\"context\":2000,\"output\":200}\n"
                              "  },\n"
                              "  \"providers\": {\n"
                              "    \"zeta\": {\"id\":\"zeta\",\"name\":\"Zeta\",\"api\":\"https://zeta/v1\","
                              "\"env\":[\"ZETA_KEY\"],\"models\":[\"m-beta\"]},\n"
                              "    \"alpha\": {\"id\":\"alpha\",\"name\":\"Alpha Inc\",\"api\":\"https://alpha/v1\","
                              "\"env\":[\"ALPHA_KEY\"],\"models\":[\"m-alpha\",\"m-beta\"]}\n"
                              "  }\n"
                              "}\n";

static void test_api_with_fixture(void)
{
    TEST("API against a fixture: providers / models / model info / matches");
    /* Bazel's per-test TEST_TMPDIR, not "/tmp": that is POSIX-only -- on Windows it means
     * "\tmp" on the current drive, where fopen() fails on a machine with no such directory,
     * and then every assertion below fails for want of a fixture. */
    char path_buf[1024];
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
        snprintf(path_buf, sizeof(path_buf), "%s/test_models_catalog_fixture.json", td);
    }
    const char *path = path_buf;
    FILE *f = fopen(path, "w");
    if (f)
    {
        fputs(kFixture, f);
        fclose(f);
    }
    setenv("EDGECLAW_MODELS_CATALOG", path, 1);
    edc_models_reset();

    CHECK(edc_models_load() == 2, "two providers");
    CHECK(edc_models_load() == 2, "load is idempotent");
    CHECK(edc_models_source() && strcmp(edc_models_source(), path) == 0, "source is the file");

    CHECK(edc_models_provider_id(0) && strcmp(edc_models_provider_id(0), "alpha") == 0, "ids are sorted [0]");
    CHECK(edc_models_provider_id(1) && strcmp(edc_models_provider_id(1), "zeta") == 0, "ids are sorted [1]");
    CHECK(edc_models_provider_id(2) == NULL, "out-of-range id -> NULL");

    CHECK(edc_models_provider_name("alpha") && strcmp(edc_models_provider_name("alpha"), "Alpha Inc") == 0, "name");
    CHECK(edc_models_provider_api("alpha") && strcmp(edc_models_provider_api("alpha"), "https://alpha/v1") == 0,
          "base url");
    CHECK(edc_models_provider_env("alpha") != NULL, "key env var present");
    CHECK(edc_models_provider_api("no-such") == NULL, "unknown provider -> NULL api");

    CHECK(edc_models_model_count("alpha") == 2 && edc_models_model_count("zeta") == 1, "model counts");
    CHECK(edc_models_model_id_at("alpha", 0) && strcmp(edc_models_model_id_at("alpha", 0), "m-alpha") == 0,
          "model id at 0");
    CHECK(edc_models_model_count("no-such") == 0, "unknown provider -> 0 models");

    struct edc_model_info info;
    CHECK(edc_models_model_info("m-alpha", &info), "model info exists");
    CHECK(info.context == 1000 && info.output == 100, "context / output");
    CHECK(info.tool_call && info.reasoning && !info.attachment, "capability flags");
    CHECK(info.name && strcmp(info.name, "Alpha") == 0, "model name");
    CHECK(edc_models_model_info("m-beta", &info) && info.context == 2000, "second model");
    CHECK(!edc_models_model_info("no-such-model", NULL), "unknown model -> false");

    CHECK(edc_models_provider_matches("alpha", "alp"), "provider id substring");
    CHECK(edc_models_provider_matches("alpha", "Alpha Inc"), "provider name, case-insensitive");
    CHECK(!edc_models_provider_matches("alpha", "zeta"), "provider no-match");
    CHECK(edc_models_provider_matches("alpha", ""), "empty query matches all");
    CHECK(edc_models_model_matches("m-alpha", "ALPHA"), "model match by id/name, case-insensitive");
    CHECK(!edc_models_model_matches("m-alpha", "zeta"), "model no-match");

    CHECK(edc_models_is_chat_model("m-alpha"), "m-alpha (tool_call) is a chat model");
    CHECK(!edc_models_is_chat_model("m-beta"), "m-beta (no tool_call / text) is not a chat model");
    CHECK(edc_models_provider_env_name("alpha", 0) &&
              strcmp(edc_models_provider_env_name("alpha", 0), "ALPHA_KEY") == 0,
          "provider key env var name");
    CHECK(edc_models_provider_env_name("alpha", 1) == NULL, "env var out-of-range -> NULL");

    unsetenv("EDGECLAW_MODELS_CATALOG");
    edc_models_reset();
}

static void test_embedded_is_safe(void)
{
    TEST("the embedded catalog loads safely (empty on a clean checkout)");
    size_t n = edc_models_load();
    CHECK(edc_models_source() && strcmp(edc_models_source(), "<embedded>") == 0, "source is <embedded>");
    CHECK(edc_models_provider_count() == n, "count matches");
    CHECK(edc_models_provider_id(n) == NULL, "out-of-range -> NULL");
    CHECK(edc_models_provider_api("no-such") == NULL, "unknown provider -> NULL");
    CHECK(edc_models_model_count("no-such") == 0, "unknown provider -> 0 models");
    CHECK(!edc_models_model_info("no-such", NULL), "unknown model -> false");
    if (n > 0)
        CHECK(edc_models_provider_id(0) != NULL, "first provider id when the catalog is non-empty");
}

int main(void)
{
    test_api_with_fixture();
    test_embedded_is_safe();

    log_info("  ====== %d passed, %d failed ======", test_passed, test_failed);
    return test_failed ? 1 : 0;
}
