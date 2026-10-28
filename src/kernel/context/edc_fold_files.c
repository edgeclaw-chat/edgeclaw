#include "edc_fold_files.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/* ── storage ───────────────────────────────────────────────── */

struct edc_fold_files *edc_fold_files_new(void)
{
    struct edc_fold_files *f = calloc(1, sizeof(*f));
    return f;
}

void edc_fold_files_free(struct edc_fold_files *f)
{
    if (!f)
        return;
    for (size_t i = 0; i < f->modified_n; i++)
        free(f->modified[i]);
    free(f->modified);
    for (size_t i = 0; i < f->read_n; i++)
        free(f->read[i]);
    free(f->read);
    free(f);
}

struct edc_fold_files *edc_fold_files_dup(const struct edc_fold_files *f)
{
    struct edc_fold_files *d = edc_fold_files_new();
    if (!d || !f)
        return d;
    for (size_t i = 0; i < f->modified_n; i++)
        edc_fold_files_add_modified(d, f->modified[i]);
    for (size_t i = 0; i < f->read_n; i++)
        edc_fold_files_add_read(d, f->read[i]);
    return d;
}

size_t edc_fold_files_count(const struct edc_fold_files *f)
{
    return f ? (f->modified_n + f->read_n) : 0;
}

/* ── list primitives ───────────────────────────────────────── */

static int list_index_of(char **list, size_t n, const char *path)
{
    for (size_t i = 0; i < n; i++)
    {
        if (strcmp(list[i], path) == 0)
            return (int)i;
    }
    return -1;
}

/* Append |path| unless present. Returns 0 on allocation failure. */
static int list_append_unique(char ***list, size_t *n, const char *path)
{
    if (!path || !*path)
        return 1;
    if (list_index_of(*list, *n, path) >= 0)
        return 1;
    char **grown = realloc(*list, (*n + 1) * sizeof(char *));
    if (!grown)
        return 0;
    *list = grown;
    grown[*n] = strdup(path);
    if (!grown[*n])
        return 0;
    (*n)++;
    return 1;
}

/* Keep only the newest |max| entries (the oldest are at the FRONT: the list is in
 * first-seen order). */
static void list_cap(char ***list, size_t *n, size_t max)
{
    if (*n <= max)
        return;
    const size_t drop = *n - max;
    for (size_t i = 0; i < drop; i++)
        free((*list)[i]);
    memmove(*list, *list + drop, max * sizeof(char *));
    *n = max;
}

/* Remove |path| from the list if present (frees the slot, preserves order). */
static void list_remove(char ***list, size_t *n, const char *path)
{
    const int at = list_index_of(*list, *n, path);
    if (at < 0)
        return;
    free((*list)[at]);
    memmove(*list + at, *list + at + 1, (*n - (size_t)at - 1) * sizeof(char *));
    (*n)--;
}

/* Read-only access to the two lists, in stored order.
 *
 * The first version of the persistence layer iterated the struct's arrays itself and got the
 * SHAPE wrong (it treated the tracked lists as one flat array of paths, while the module
 * keeps two). Another copy of "how this is laid out" is how that happens, so readers outside
 * this file go through here. */
size_t edc_fold_files_modified_count(const struct edc_fold_files *f)
{
    return f ? f->modified_n : 0;
}

size_t edc_fold_files_read_count(const struct edc_fold_files *f)
{
    return f ? f->read_n : 0;
}

const char *edc_fold_files_modified_at(const struct edc_fold_files *f, size_t i)
{
    return (f && i < f->modified_n) ? f->modified[i] : NULL;
}

const char *edc_fold_files_read_at(const struct edc_fold_files *f, size_t i)
{
    return (f && i < f->read_n) ? f->read[i] : NULL;
}

void edc_fold_files_add_modified(struct edc_fold_files *f, const char *path)
{
    if (!f)
        return;
    list_append_unique(&f->modified, &f->modified_n, path);
}

void edc_fold_files_add_read(struct edc_fold_files *f, const char *path)
{
    if (!f)
        return;
    list_append_unique(&f->read, &f->read_n, path);
}

void edc_fold_files_merge(struct edc_fold_files *dst, const struct edc_fold_files *src)
{
    if (!dst)
        return;
    if (src)
    {
        for (size_t i = 0; i < src->modified_n; i++)
            edc_fold_files_add_modified(dst, src->modified[i]);
        for (size_t i = 0; i < src->read_n; i++)
            edc_fold_files_add_read(dst, src->read[i]);
    }

    /* A file that was read and later edited is not "read-only": it belongs in the
     * modified list only. Without this, the model sees the same path in both blocks and
     * cannot tell which one is current. */
    for (size_t i = 0; i < dst->read_n;)
    {
        if (list_index_of(dst->modified, dst->modified_n, dst->read[i]) >= 0)
            list_remove(&dst->read, &dst->read_n, dst->read[i]);
        else
            i++;
    }

    list_cap(&dst->modified, &dst->modified_n, EDC_FOLD_MAX_TRACKED_FILES);
    list_cap(&dst->read, &dst->read_n, EDC_FOLD_MAX_TRACKED_FILES);
}

/* ── text: the projection ──────────────────────────────────── */

/* Drop every "<tag> ... </tag>" block from |text|.
 *
 * The reason it is needed: on every re-fold the corpus carries `<previous-summary>`
 * (the old memory) and the instruction asks for a complete new summary, so the model
 * echoing our own blocks back is NORMAL. The blocks are appended below from the
 * structure, so an echoed copy would duplicate them.
 *
 * Note the `head = sdscatsds(head, tail)` -- sdscatsds returns its FIRST argument
 * (possibly realloc'd), so `head` IS the buffer we continue with. An earlier version
 * assigned and then freed `head`, which freed the very string the next iteration's
 * strstr() read (use-after-free, reachable on the first echoed block). */
static sds strip_block(sds text, const char *tag)
{
    if (!text)
        return text;
    sds open_tag = sdscatprintf(sdsempty(), "<%s>", tag);
    sds close_tag = sdscatprintf(sdsempty(), "</%s>", tag);
    for (;;)
    {
        const char *b = strstr(text, open_tag);
        if (!b)
            break;
        const char *e = strstr(b, close_tag);
        if (!e)
            break;
        size_t start = (size_t)(b - text);
        size_t end = (size_t)(e - text) + sdslen(close_tag);
        sds head = sdsnewlen(text, start);
        sds tail = sdsnew(text + end);
        sdsfree(text);
        head = sdscatsds(head, tail);
        sdsfree(tail);
        text = head;
    }
    sdsfree(open_tag);
    sdsfree(close_tag);
    return text;
}

sds edc_fold_files_body(const char *summary)
{
    sds body = sdsnew(summary ? summary : "");
    body = strip_block(body, "read-files");
    body = strip_block(body, "modified-files");
    /* Only spaces and newlines: a trailing TAB is left alone, exactly as before, because
     * the bytes the model sees must not change for anything already folded. */
    while (sdslen(body) > 0 && (body[sdslen(body) - 1] == '\n' || body[sdslen(body) - 1] == ' '))
        sdsrange(body, 0, -2);
    return body;
}

sds edc_fold_files_render(const char *body, const struct edc_fold_files *f)
{
    sds out = sdsnew(body ? body : "");
    if (f && f->read_n > 0)
    {
        out = sdscat(out, "\n\n<read-files>\n");
        for (size_t i = 0; i < f->read_n; i++)
            out = sdscatprintf(out, "%s\n", f->read[i]);
        out = sdscat(out, "</read-files>");
    }
    if (f && f->modified_n > 0)
    {
        out = sdscat(out, "\n\n<modified-files>\n");
        for (size_t i = 0; i < f->modified_n; i++)
            out = sdscatprintf(out, "%s\n", f->modified[i]);
        out = sdscat(out, "</modified-files>");
    }
    /* Unconditional: the memory is one message either way, and its last byte must not
     * depend on whether any file was touched. */
    return sdscat(out, "\n");
}

int64_t edc_fold_files_token_estimate(const struct edc_fold_files *f)
{
    if (!f || (f->read_n == 0 && f->modified_n == 0))
        return 0;
    int64_t est = 0;
    /* The tags and the blank line before each block, then the paths. The constant is
     * small and only ever used to decide when to fold, so precision is not the point --
     * forgetting the blocks entirely is. */
    if (f->read_n > 0)
        est += 8; /* "\n\n<read-files>\n" + "</read-files>" */
    if (f->modified_n > 0)
        est += 12; /* "\n\n<modified-files>\n" + "</modified-files>" */
    for (size_t i = 0; i < f->read_n; i++)
        est += (int64_t)(strlen(f->read[i]) / 4) + 1;
    for (size_t i = 0; i < f->modified_n; i++)
        est += (int64_t)(strlen(f->modified[i]) / 4) + 1;
    return est;
}

/* ── persistence ───────────────────────────────────────────── */

static cJSON *list_to_json(char **list, size_t n)
{
    cJSON *arr = cJSON_CreateArray();
    for (size_t i = 0; i < n; i++)
        cJSON_AddItemToArray(arr, cJSON_CreateString(list[i]));
    return arr;
}

cJSON *edc_fold_files_to_json(const struct edc_fold_files *f)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddItemToObject(j, "modified", list_to_json(f ? f->modified : NULL, f ? f->modified_n : 0));
    cJSON_AddItemToObject(j, "read", list_to_json(f ? f->read : NULL, f ? f->read_n : 0));
    return j;
}

static void list_from_json(const cJSON *j, const char *key, struct edc_fold_files *f, bool modified)
{
    const cJSON *arr = j ? cJSON_GetObjectItem(j, key) : NULL;
    if (!cJSON_IsArray(arr))
        return;
    const cJSON *it = NULL;
    cJSON_ArrayForEach(it, arr)
    {
        if (!cJSON_IsString(it))
            continue;
        if (modified)
            edc_fold_files_add_modified(f, it->valuestring);
        else
            edc_fold_files_add_read(f, it->valuestring);
    }
}

struct edc_fold_files *edc_fold_files_from_json(const cJSON *j)
{
    struct edc_fold_files *f = edc_fold_files_new();
    if (!f)
        return NULL;
    list_from_json(j, "modified", f, true);
    list_from_json(j, "read", f, false);
    return f;
}
