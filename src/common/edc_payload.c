#include "edc_payload.h"

#include "sds.h"

#include <cJSON.h>
#include <stdlib.h>
#include <string.h>

struct edc_payload *edc_payload_new(void)
{
    struct edc_payload *payload = calloc(1, sizeof(*payload));
    return payload;
}

void edc_payload_free(struct edc_payload *payload)
{
    if (!payload)
        return;
    for (int i = 0; i < payload->n; i++)
    {
        sdsfree(payload->items[i].key);
        sdsfree(payload->items[i].text);
    }
    free(payload->items);
    free(payload);
}

static int find_key(const struct edc_payload *payload, const char *key)
{
    if (!payload || !key)
        return -1;
    for (int i = 0; i < payload->n; i++)
        if (strcmp(payload->items[i].key, key) == 0)
            return i;
    return -1;
}

bool edc_payload_set(struct edc_payload *payload, const char *key, const char *text)
{
    if (!payload || !key || !*key)
        return false;

    const int at = find_key(payload, key);
    if (at >= 0)
    {
        /* Overwrite in place: the entry keeps its position, so a re-paste that reuses
         * a key does not reorder the blocks. */
        sdsfree(payload->items[at].text);
        payload->items[at].text = sdsnew(text ? text : "");
        return true;
    }

    if (payload->n == payload->cap)
    {
        payload->cap = payload->cap ? payload->cap * 2 : 4;
        payload->items = realloc(payload->items, sizeof(*payload->items) * (size_t)payload->cap);
    }
    payload->items[payload->n].key = sdsnew(key);
    payload->items[payload->n].text = sdsnew(text ? text : "");
    payload->n++;
    return true;
}

const char *edc_payload_get(const struct edc_payload *payload, const char *key)
{
    const int at = find_key(payload, key);
    return at >= 0 ? payload->items[at].text : NULL;
}

bool edc_payload_remove(struct edc_payload *payload, const char *key)
{
    const int at = find_key(payload, key);
    if (at < 0)
        return false;
    sdsfree(payload->items[at].key);
    sdsfree(payload->items[at].text);
    for (int i = at; i + 1 < payload->n; i++)
        payload->items[i] = payload->items[i + 1];
    payload->n--;
    return true;
}

int edc_payload_count(const struct edc_payload *payload)
{
    return payload ? payload->n : 0;
}

char *edc_payload_to_json(const struct edc_payload *payload)
{
    if (edc_payload_empty(payload))
        return NULL;

    cJSON *root = cJSON_CreateObject();
    if (!root)
        return NULL;
    for (int i = 0; i < payload->n; i++)
        cJSON_AddStringToObject(root, payload->items[i].key, payload->items[i].text);

    char *printed = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!printed)
        return NULL;
    sds out = sdsnew(printed);
    cJSON_free(printed);
    return out;
}

struct edc_payload *edc_payload_from_json(const char *json)
{
    if (!json || !*json)
        return NULL;

    cJSON *root = cJSON_Parse(json);
    if (!root || !cJSON_IsObject(root))
    {
        cJSON_Delete(root);
        return NULL;
    }

    struct edc_payload *payload = edc_payload_new();
    if (!payload)
    {
        cJSON_Delete(root);
        return NULL;
    }
    for (cJSON *it = root->child; it; it = it->next)
    {
        const char *text = cJSON_GetStringValue(it);
        if (it->string && text)
            edc_payload_set(payload, it->string, text);
    }
    cJSON_Delete(root);

    if (edc_payload_empty(payload))
    {
        edc_payload_free(payload);
        return NULL;
    }
    return payload;
}

/* Longest key first: one key may be a prefix of another (`[Pasted #1 …]` vs a key that
 * continues past it), and matching the shorter one first would leave the tail behind. */
static void sort_by_key_len(const struct edc_payload *payload, int *order)
{
    for (int i = 0; i < payload->n; i++)
        order[i] = i;
    for (int i = 1; i < payload->n; i++)
    {
        const int cur = order[i];
        int j = i - 1;
        while (j >= 0 && strlen(payload->items[order[j]].key) < strlen(payload->items[cur].key))
        {
            order[j + 1] = order[j];
            j--;
        }
        order[j + 1] = cur;
    }
}

char *edc_payload_expand(const char *text, const struct edc_payload *payload)
{
    if (!text)
        return sdsempty();
    if (edc_payload_empty(payload))
        return sdsnew(text);

    int stack_order[8];
    int *order = payload->n <= 8 ? stack_order : malloc(sizeof(int) * (size_t)payload->n);
    sort_by_key_len(payload, order);

    sds out = sdsempty();
    const size_t n = strlen(text);
    size_t p = 0;
    while (p < n)
    {
        int hit = -1;
        for (int k = 0; k < payload->n; k++)
        {
            const sds key = payload->items[order[k]].key;
            if (key[0] == text[p] && strncmp(text + p, key, strlen(key)) == 0)
            {
                hit = order[k];
                break;
            }
        }
        if (hit < 0)
        {
            out = sdscatlen(out, text + p, 1);
            p++;
            continue;
        }
        out = sdscat(out, payload->items[hit].text);
        p += strlen(payload->items[hit].key);
        /* Deliberately no re-scan of what was just appended: the substituted text is
         * final (a pasted block is not a template). */
    }

    if (order != stack_order)
        free(order);
    return out;
}

void edc_payload_string_free(char *string)
{
    sdsfree(string);
}

int edc_payload_unreferenced(const char *text, const struct edc_payload *payload, const char **out, int cap)
{
    if (!text || edc_payload_empty(payload) || !out || cap <= 0)
        return 0;
    /* An entry whose placeholder no longer appears in the draft has nothing pointing at
     * it: its bytes would either be dropped silently or, worse, sent under a placeholder
     * the reader can no longer see. The caller decides (EdcInput drops them as the user
     * edits; the submit path can refuse to lose one). */
    int written = 0;
    for (int i = 0; i < payload->n && written < cap; i++)
        if (!strstr(text, payload->items[i].key))
            out[written++] = payload->items[i].key;
    return written;
}
