/* The built-in colorscheme bundle.
 *
 * Exercises for real:
 *   - the embedded table //:colors_embedded_c is non-empty and well-formed;
 *   - edc_init_env releases it to <home>/colors and the bytes match the bundle;
 *   - a fresh config.json selects tokyonight-moon as its default theme;
 *   - a stale/hand-edited theme file is restored on the next start (tracks binary);
 *   - an existing config's theme choice is never overwritten. */

#include "edc_bundled_colors.h"
#include "edc_config.h"
#include "edc_init_env.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "test_fs_util.h"

static int passed = 0;
static int failed = 0;

#define T_CHECK(cond, msg)                                                                                             \
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

static const struct edc_bundled_file *find(const char *path)
{
    for (size_t i = 0; i < edc_bundled_colors_count; i++)
        if (strcmp(edc_bundled_colors_entries[i].path, path) == 0)
            return &edc_bundled_colors_entries[i];
    return NULL;
}

static char *slurp(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long n = ftell(f);
    if (n < 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    char *buf = malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = '\0';
    if (out_len) *out_len = got;
    return buf;
}

static bool write_bytes(const char *path, const char *data, size_t n)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return false;
    size_t wrote = fwrite(data, 1, n, f);
    fclose(f);
    return wrote == n;
}

int main(void)
{
    /* The secret module refuses to run without a key when there is no terminal
     * (edc_init_env creates config.json, which encrypts the api_key). */
#ifdef _WIN32
    _putenv_s("EDGECLAW_MASTER_KEY", "0123456789abcdef0123456789abcdef");
#else
    setenv("EDGECLAW_MASTER_KEY", "0123456789abcdef0123456789abcdef", 1);
#endif

    /* (1) the embedded table */
    T_CHECK(edc_bundled_colors_count > 0, "the colorscheme bundle is empty");
    const struct edc_bundled_file *moon = find("tokyonight-moon.vim");
    T_CHECK(moon != NULL, "tokyonight-moon.vim is not in the bundle");
    T_CHECK(moon && moon->size > 1000 && moon->data, "the bundled theme has real content");

    char home[1024];
    test_tmp_template(home, sizeof(home), "edc_colors");
    if (!mkdtemp(home))
    {
        fprintf(stderr, "mkdtemp failed\n");
        return 1;
    }

    /* (2) first init releases it, bytes identical to the bundle */
    T_CHECK(edc_init_env(home, true), "edc_init_env releases the colorschemes");
    char path[512];
    snprintf(path, sizeof(path), "%s/colors/tokyonight-moon.vim", home);
    size_t len = 0;
    char *on_disk = slurp(path, &len);
    T_CHECK(on_disk != NULL, "<home>/colors/tokyonight-moon.vim was not written");
    T_CHECK(on_disk && moon && len == moon->size && memcmp(on_disk, moon->data, moon->size) == 0,
            "the released theme does not match the bundled bytes");
    free(on_disk);

    /* (3) a fresh config selects the bundled theme by default */
    struct edc_config *cfg = edc_config_load(home);
    T_CHECK(cfg != NULL, "config loads");
    T_CHECK(cfg && cfg->theme && strcmp(cfg->theme, "tokyonight-moon") == 0,
            "a fresh config defaults to tokyonight-moon");
    edc_config_free(cfg);

    /* (4) refresh: a stale/hand-edited theme file is restored, like the manual */
    T_CHECK(write_bytes(path, "STALE", 5), "the test can overwrite the released theme");
    T_CHECK(edc_init_env(home, false), "edc_init_env refreshes the colorschemes");
    on_disk = slurp(path, &len);
    T_CHECK(on_disk && moon && len == moon->size && memcmp(on_disk, moon->data, moon->size) == 0,
            "a stale theme was not refreshed to the bundled bytes");
    free(on_disk);

    /* (5) an existing theme choice is never overwritten by init */
    T_CHECK(edc_config_update_theme(home, "my-custom-theme"), "set a custom theme on the existing config");
    T_CHECK(edc_init_env(home, false), "edc_init_env on an existing config");
    cfg = edc_config_load(home);
    T_CHECK(cfg && cfg->theme && strcmp(cfg->theme, "my-custom-theme") == 0,
            "an existing config keeps its own theme choice");
    edc_config_free(cfg);

    printf("test_bundled_colors: %d passed, %d failed (entries=%zu)\n", passed, failed, edc_bundled_colors_count);
    return failed == 0 ? 0 : 1;
}
