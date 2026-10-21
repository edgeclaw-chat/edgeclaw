#include "edc_bundled_workspace.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

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

static const struct edc_bundled_file *find(const char *path)
{
    for (size_t i = 0; i < edc_bundled_workspace_count; i++)
    {
        if (strcmp(edc_bundled_workspace_entries[i].path, path) == 0)
            return &edc_bundled_workspace_entries[i];
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

int main(void)
{
    /* The table must be non-empty: that is what lets edc_init_env() release a
     * workspace on first run without a source tree on disk. */
    T_CHECK(edc_bundled_workspace_count > 0, "workspace table is empty");

    /* Known top-level entries. (TOOLS.md is deliberately NOT here: there is no tool document
     * at all -- the request's `tools` field is the only catalog.) */
    const char *required[] = {
        "AGENTS.md",           "ssl/cacert.pem",
    };
    for (size_t i = 0; i < sizeof(required) / sizeof(required[0]); i++)
    {
        const struct edc_bundled_file *file = find(required[i]);
        T_CHECK(file != NULL, required[i]);
        T_CHECK(file && file->size > 0, required[i]);
        T_CHECK(file && file->data != NULL, required[i]);
    }

    /* Every entry must be a valid, relative, '/'-separated path with a
     * NUL-terminated payload (the generator emits C string literals). */
    size_t skill_count = 0;
    for (size_t i = 0; i < edc_bundled_workspace_count; i++)
    {
        const struct edc_bundled_file *file = &edc_bundled_workspace_entries[i];
        T_CHECK(file->path != NULL && file->path[0] != '\0', "entry has empty path");
        T_CHECK(file->path && file->path[0] != '/', "entry path is absolute");
        T_CHECK(file->path && strstr(file->path, "..") == NULL, "entry path escapes workspace");
        T_CHECK(file->data != NULL, "entry has no data");
        T_CHECK(file->data && file->data[file->size] == '\0', "entry payload is not NUL-terminated");
        if (file->path && strncmp(file->path, "skills/", 7) == 0)
            skill_count++;
    }
    T_CHECK(skill_count >= 1, "no skill files bundled");

    /* Entries are stored in a stable path-sorted order (regression guard for
     * the generator's deterministic output). */
    for (size_t i = 1; i < edc_bundled_workspace_count; i++)
    {
        T_CHECK(strcmp(edc_bundled_workspace_entries[i - 1].path, edc_bundled_workspace_entries[i].path) < 0,
                "entries are not strictly sorted / contain duplicates");
    }

    /* Sanity check on well-known content. */
    T_CHECK(contains(find("AGENTS.md"), "EdgeClaw"), "AGENTS.md does not mention EdgeClaw");
    const struct edc_bundled_file *cacert = find("ssl/cacert.pem");
    T_CHECK(cacert && cacert->size > 100000, "cacert.pem looks truncated");
    T_CHECK(contains(cacert, "-----BEGIN CERTIFICATE-----"), "cacert.pem is not a PEM bundle");

    printf("test_bundled_workspace: %d passed, %d failed (entries=%zu)\n", passed, failed,
           edc_bundled_workspace_count);
    return failed == 0 ? 0 : 1;
}
