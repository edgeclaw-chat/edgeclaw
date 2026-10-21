/* The bundled user manual.
 *
 * Two things are exercised for real, not asserted from a fixture:
 *   - the embedded table //:docs_embedded_c produces (non-empty, sorted, well-formed),
 *   - edc_init_env() releasing it into <edc_home>/docs, and the system prompt naming
 *     that path so the model can read it.
 *
 * This is the "how do I use EdgeClaw" answer path: the binary must carry the manual, put
 * it where the prompt says it is, and refresh it when the bundled copy changes. */

#include "edc_bundled_docs.h"
#include "edc_config.h"
#include "edc_context.h"
#include "edc_init_env.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
    for (size_t i = 0; i < edc_bundled_docs_count; i++)
    {
        if (strcmp(edc_bundled_docs_entries[i].path, path) == 0)
            return &edc_bundled_docs_entries[i];
    }
    return NULL;
}

static bool contains(const struct edc_bundled_file *file, const char *needle)
{
    if (!file || file->size == 0 || !file->data)
        return false;

    size_t needle_len = strlen(needle);
    if (needle_len > file->size)
        return false;

    for (size_t i = 0; i + needle_len <= file->size; i++)
    {
        if (memcmp(file->data + i, needle, needle_len) == 0)
            return true;
    }
    return false;
}

/* Whole-file read into a heap buffer; NULL when it cannot be read. */
static char *slurp(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0)
    {
        fclose(f);
        return NULL;
    }
    long size = ftell(f);
    if (size < 0 || fseek(f, 0, SEEK_SET) != 0)
    {
        fclose(f);
        return NULL;
    }
    char *buf = malloc((size_t)size + 1);
    if (!buf)
    {
        fclose(f);
        return NULL;
    }
    size_t got = fread(buf, 1, (size_t)size, f);
    fclose(f);
    buf[got] = 0;
    if (out_len)
        *out_len = got;
    return buf;
}

static bool write_bytes(const char *path, const char *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return false;
    bool ok = fwrite(data, 1, len, f) == len;
    fclose(f);
    return ok;
}

int main(void)
{
    /* Same variable the other config tests set: the secret module refuses to run
     * without a key when there is no terminal. (_putenv_s is the MinGW spelling;
     * setenv does not exist there.) */
#ifdef _WIN32
    _putenv_s("EDGECLAW_MASTER_KEY", "0123456789abcdef0123456789abcdef");
#else
    setenv("EDGECLAW_MASTER_KEY", "0123456789abcdef0123456789abcdef", 1);
#endif

    /* (1) The table itself: the binary must actually carry the manual. */
    T_CHECK(edc_bundled_docs_count > 0, "docs table is empty -- the manual is not embedded");
    const struct edc_bundled_file *manual = find("edgeclaw.md");
    T_CHECK(manual != NULL, "docs/edgeclaw.md is not in the bundle");
    T_CHECK(manual && manual->size > 1000, "docs/edgeclaw.md looks truncated");
    T_CHECK(contains(manual, "EdgeClaw"), "docs/edgeclaw.md does not mention EdgeClaw");
    T_CHECK(find("edgeclaw.1") != NULL, "the man page is not in the bundle");

    for (size_t i = 0; i < edc_bundled_docs_count; i++)
    {
        const struct edc_bundled_file *file = &edc_bundled_docs_entries[i];
        T_CHECK(file->path != NULL && file->path[0] != '\0', "entry has empty path");
        T_CHECK(file->path && file->path[0] != '/', "entry path is absolute");
        T_CHECK(file->path && strstr(file->path, "..") == NULL, "entry path escapes the docs dir");
        T_CHECK(file->data != NULL, "entry has no data");
        T_CHECK(file->data && file->data[file->size] == '\0', "entry payload is not NUL-terminated");
        if (i > 0)
            T_CHECK(strcmp(edc_bundled_docs_entries[i - 1].path, file->path) < 0,
                    "entries are not strictly sorted / contain duplicates");
    }

    /* The bundle is the MANUAL, not docs/: the six chapters below answer "how do I use
     * EdgeClaw", while the design notes beside them answer only "how is EdgeClaw built".
     * A glob would quietly ship the notes too, so the set is pinned from both sides --
     * 245 KB, pi-alignment.md 60 KB), so the set is pinned from both sides -- what has to
     * be in, and what must not be -- and the count keeps a stray glob from slipping back
     * in without failing here. */
    static const char *k_manual[] = {"edgeclaw.1", "edgeclaw.md", "input-syntax.zh.md",
                                     "local-shell-command.zh.md", "prompt-library.md",
                                     "tool-output-limits.zh.md"};
    const size_t manual_count = sizeof(k_manual) / sizeof(k_manual[0]);
    for (size_t i = 0; i < manual_count; i++)
    {
        char msg[128];
        snprintf(msg, sizeof(msg), "manual chapter %s is not in the bundle", k_manual[i]);
        T_CHECK(find(k_manual[i]) != NULL, msg);
    }
    T_CHECK(edc_bundled_docs_count == manual_count,
            "the bundle must be the manual chapters only (see DOCS_FILES in //BUILD)");
    T_CHECK(find("release-checklist.md") == NULL, "a maintenance note is embedded");
    T_CHECK(find("pi-alignment.md") == NULL, "a design note is embedded: pi-alignment.md");
    T_CHECK(find("design/shell.y") == NULL, "the grammar sources are embedded");

    /* (2) edc_init_env releases it to <home>/docs. */
    char home[] = "/tmp/edc_docs_XXXXXX";
    if (!mkdtemp(home))
    {
        fprintf(stderr, "mkdtemp failed\n");
        return 1;
    }

    T_CHECK(edc_init_env(home, true), "edc_init_env releases the manual");

    char path[512];
    snprintf(path, sizeof(path), "%s/docs/edgeclaw.md", home);
    size_t len = 0;
    char *on_disk = slurp(path, &len);
    T_CHECK(on_disk != NULL, "<home>/docs/edgeclaw.md was not written");
    T_CHECK(on_disk && manual && len == manual->size && memcmp(on_disk, manual->data, manual->size) == 0,
            "the released file does not match the bundled bytes");
    free(on_disk);

    /* The workspace release still happens alongside it (one init, both trees). */
    snprintf(path, sizeof(path), "%s/workspace/AGENTS.md", home);
    on_disk = slurp(path, NULL);
    T_CHECK(on_disk != NULL, "the workspace was not released next to the manual");
    free(on_disk);

    /* (3) The system prompt names the exact path -- this is the whole point: the model
     * must be able to find the manual without being told by the user. */
    struct edc_config *cfg = edc_config_load(home);
    T_CHECK(cfg != NULL, "config loads");
    sds prompt = cfg ? edc_context_build_system_prompt(cfg, NULL) : NULL;
    T_CHECK(prompt != NULL, "the system prompt is built");
    sds expected = sdscatprintf(sdsempty(), "%s/docs/edgeclaw.md", home);
    T_CHECK(prompt && strstr(prompt, expected) != NULL, "the system prompt does not name the manual");
    T_CHECK(prompt && strstr(prompt, "## Usage Manual") != NULL, "the manual section is missing");
    sdsfree(expected);
    sdsfree(prompt);

    /* A config without an edc_home (unit tests build one) must not print a dangling
     * path into the prompt. */
    sdsfree(cfg->edc_home);
    cfg->edc_home = NULL;
    prompt = edc_context_build_system_prompt(cfg, NULL);
    T_CHECK(prompt && strstr(prompt, "## Usage Manual") == NULL, "no home => no manual section");
    T_CHECK(prompt && strstr(prompt, "docs/edgeclaw.md") == NULL, "no home => no dangling path");
    sdsfree(prompt);
    edc_config_free(cfg);

    /* (4) Refresh: the docs tree tracks the binary, so a stale/hand-edited file is
     * restored on the next start (a NEW RELEASE must not leave an old manual behind). */
    snprintf(path, sizeof(path), "%s/docs/edgeclaw.md", home);
    T_CHECK(write_bytes(path, "STALE", 5), "the test can overwrite the released file");
    T_CHECK(edc_init_env(home, false), "edc_init_env refreshes the manual");
    on_disk = slurp(path, &len);
    T_CHECK(on_disk && manual && len == manual->size && memcmp(on_disk, manual->data, manual->size) == 0,
            "a stale manual was not refreshed");
    free(on_disk);

    printf("test_bundled_docs: %d passed, %d failed (entries=%zu)\n", passed, failed, edc_bundled_docs_count);
    return failed == 0 ? 0 : 1;
}
