/* context_window: the config key that drives the fold trigger.
 *
 * The three producers are exercised for real (no hand-fed expectations):
 *   - edc_init_env(home, true)  creates config.json and materializes every key
 *   - edc_init_env(home, false) tops up an existing file without clobbering it
 *   - edc_config_load(home)     reads the file the way startup does
 *
 * The window is the **only** trigger for folding and it has no "unconfigured"
 * state, so <= 0 is not a value: the default (1M) applies whenever the key is
 * missing, non-numeric, or not positive, and the top-up writes that back. The
 * retired `group_fold_msg_threshold` is dropped from the file. */

#include "cJSON.h"
#include "edc_config.h"
#include "edc_group_fold.h"
#include "edc_init_env.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int passed = 0;
static int failed = 0;

#define T_PASS()                                                                                                       \
    do                                                                                                                 \
    {                                                                                                                  \
        passed++;                                                                                                      \
    } while (0)
#define T_FAIL(msg)                                                                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);                                                  \
        failed++;                                                                                                      \
    } while (0)
#define T_CHECK(cond, msg)                                                                                             \
    do                                                                                                                 \
    {                                                                                                                  \
        if (cond)                                                                                                      \
            T_PASS();                                                                                                  \
        else                                                                                                           \
            T_FAIL(msg);                                                                                               \
    } while (0)

static char *conf_path(const char *home)
{
    static char buf[512];
    snprintf(buf, sizeof(buf), "%s/config.json", home);
    return buf;
}

/* stdio on purpose: edc_file_* wants a uv file loop and this test runs none. */
static char *read_all(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    if (!buf)
    {
        fclose(f);
        return NULL;
    }
    size_t got = fread(buf, 1, (size_t)n, f);
    buf[got] = '\0';
    fclose(f);
    return buf;
}

static bool write_all(const char *path, const char *s)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return false;
    size_t n = strlen(s);
    bool ok = fwrite(s, 1, n, f) == n;
    fclose(f);
    return ok;
}

/* Read-modify-write config.json through cJSON, so the test never depends on the
 * writer's spacing. `replace` adds the key when it is absent — cJSON's
 * ReplaceItemInObject is a *no-op* on a missing key, which made an earlier version
 * of this test assert against a file that never changed. */
static bool edit_config(const char *home, const char *key, int value, bool remove)
{
    char *text = read_all(conf_path(home));
    if (!text)
        return false;
    cJSON *conf = cJSON_Parse(text);
    free(text);
    if (!conf)
        return false;

    cJSON_DeleteItemFromObject(conf, key);
    if (!remove)
        cJSON_AddItemToObject(conf, key, cJSON_CreateNumber(value));

    char *out = cJSON_Print(conf);
    cJSON_Delete(conf);
    if (!out)
        return false;
    bool ok = write_all(conf_path(home), out);
    free(out);
    return ok;
}

static bool file_has_key(const char *home, const char *key)
{
    char *text = read_all(conf_path(home));
    if (!text)
        return false;
    bool found = strstr(text, key) != NULL;
    free(text);
    return found;
}

/* The value as it sits in the file (0 when absent or non-numeric), so a repaired
 * value can be asserted on disk and not only through the reader. */
static int file_int_value(const char *home, const char *key)
{
    char *text = read_all(conf_path(home));
    if (!text)
        return -1;
    cJSON *conf = cJSON_Parse(text);
    free(text);
    if (!conf)
        return -1;
    cJSON *item = cJSON_GetObjectItem(conf, key);
    int value = (item && cJSON_IsNumber(item)) ? item->valueint : 0;
    cJSON_Delete(conf);
    return value;
}

int main(void)
{
    /* The secret module (api_key encryption) refuses to run without a key when
     * there is no terminal; the same variable the other tests set. */
    setenv("EDGECLAW_MASTER_KEY", "0123456789abcdef0123456789abcdef", 1);

    char tmpl[] = "/tmp/edc_cfgwin_XXXXXX";
    char *home = mkdtemp(tmpl);
    if (!home)
    {
        fprintf(stderr, "FAIL: mkdtemp\n");
        return 1;
    }

    /* (1) first run: config.json is created with the key materialized. */
    T_CHECK(edc_init_env(home, true), "edc_init_env(setup=true) creates config.json");
    T_CHECK(file_has_key(home, "context_window"), "the create path wrote the key into config.json");

    struct edc_config *cfg = edc_config_load(home);
    T_CHECK(cfg != NULL, "config.json loads");
    T_CHECK(cfg && cfg->context_window == EDC_CONFIG_DEFAULT_CONTEXT_WINDOW, "fresh config: default window");
    T_CHECK(EDC_CONFIG_DEFAULT_CONTEXT_WINDOW == 1024 * 1024, "the default is 1M (deepseek-v4-flash's real window)");
    edc_config_free(cfg);

    /* (2) a declared window is read verbatim. */
    T_CHECK(edit_config(home, "context_window", 131072, false), "set context_window=131072");
    cfg = edc_config_load(home);
    T_CHECK(cfg && cfg->context_window == 131072, "declared window round-trips");
    edc_config_free(cfg);

    /* (3) the top-up must not clobber a value the user already set. */
    T_CHECK(edc_init_env(home, false), "top-up an existing config.json");
    cfg = edc_config_load(home);
    T_CHECK(cfg && cfg->context_window == 131072, "top-up keeps the declared window");
    edc_config_free(cfg);

    /* (4) a key removed by hand is re-materialized by the top-up — the path that
     * runs on every startup of an existing file (the create path is (1)). */
    T_CHECK(edit_config(home, "context_window", 0, true), "remove the key by hand");
    T_CHECK(edc_init_env(home, false), "top-up a config that is missing the key");
    T_CHECK(file_has_key(home, "context_window"), "top-up materialized the key");
    cfg = edc_config_load(home);
    T_CHECK(cfg && cfg->context_window == EDC_CONFIG_DEFAULT_CONTEXT_WINDOW, "re-materialized key reads the default");
    edc_config_free(cfg);

    /* (5) with the key gone and no top-up, the reader itself falls back. */
    T_CHECK(edit_config(home, "context_window", 0, true), "remove the key again");
    cfg = edc_config_load(home);
    T_CHECK(cfg && cfg->context_window == EDC_CONFIG_DEFAULT_CONTEXT_WINDOW, "missing key falls back to the default");
    edc_config_free(cfg);

    /* (6) an explicit 0 (or a negative) is *repaired*, not honoured: the fold has
     * no branch for an unknown window, so a stale config.json must not be able to
     * switch folding off. The file is rewritten, so it shows the truth. */
    T_CHECK(edit_config(home, "context_window", 0, false), "set context_window=0 by hand");
    T_CHECK(file_int_value(home, "context_window") == 0, "前置: 文件里真的写着 0 (没这一步断言会空转)");
    cfg = edc_config_load(home);
    T_CHECK(cfg && cfg->context_window == EDC_CONFIG_DEFAULT_CONTEXT_WINDOW, "the reader repairs an explicit 0");
    edc_config_free(cfg);
    T_CHECK(edc_init_env(home, false), "top-up a config holding 0");
    T_CHECK(file_int_value(home, "context_window") == EDC_CONFIG_DEFAULT_CONTEXT_WINDOW,
            "the file itself was rewritten to the default (not left lying)");

    T_CHECK(edit_config(home, "context_window", -5, false), "set context_window=-5 by hand");
    T_CHECK(edc_init_env(home, false), "top-up a config holding a negative window");
    T_CHECK(file_int_value(home, "context_window") == EDC_CONFIG_DEFAULT_CONTEXT_WINDOW, "a negative is repaired too");

    /* (6b) the retired message-count knob is dropped from the file. */
    T_CHECK(edit_config(home, "group_fold_msg_threshold", 200, false), "put the retired key back by hand");
    T_CHECK(file_has_key(home, "group_fold_msg_threshold"), "前置: 文件里真的有那个键");
    T_CHECK(edc_init_env(home, false), "top-up drops it");
    T_CHECK(!file_has_key(home, "group_fold_msg_threshold"), "group_fold_msg_threshold is gone from config.json");

    /* (7) fold policy constants (decisions 13; a/b/c 见 edc_group_fold.h). */
    T_CHECK(EDC_FOLD_RESERVE_TOKENS == 16384, "reserve = 16k");
    T_CHECK(EDC_FOLD_KEEP_RECENT_TOKENS == 20000, "keep_recent = 20k");
    T_CHECK(EDC_FOLD_SUMMARY_MAX_TOKENS == 32768, "摘要预算 = 32768 (不再由 reserve 推: 0.8 x reserve 太小)");
    T_CHECK(EDC_FOLD_MAX_CORPUS_TOKENS == EDC_FOLD_SUMMARY_MAX_TOKENS * 4, "语料上限 = 4 x 摘要预算 (压缩比可达成)");
    T_CHECK(EDC_FOLD_MAX_CORPUS_TOKENS == 131072, "就是 131072");
    T_CHECK(EDC_FOLD_MAX_FAILURES == 3, "连续失败上限 = 3");

    printf("test_config_window: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
