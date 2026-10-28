#include "edc_models.h"

#include "edc_bundled_models_catalog.h"
#include "log.h"

#include <cJSON.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Lazy, module-level cache of the parsed catalog (see edc_models.h). */

static cJSON *g_root = NULL;
static char **g_provider_ids = NULL;
static size_t g_provider_count = 0;
static char *g_source = NULL;
static bool g_loaded = false;

static bool contains_ci(const char *hay, const char *needle)
{
    if (!hay || !needle || !needle[0])
        return false;
    size_t nl = strlen(needle);
    for (const char *p = hay; *p; p++)
    {
        size_t i = 0;
        while (i < nl && p[i] && tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i]))
            i++;
        if (i == nl)
            return true;
    }
    return false;
}

static void free_all(void)
{
    if (g_provider_ids)
    {
        for (size_t i = 0; i < g_provider_count; i++)
            free(g_provider_ids[i]);
        free(g_provider_ids);
    }
    g_provider_ids = NULL;
    g_provider_count = 0;
    cJSON_Delete(g_root);
    g_root = NULL;
    free(g_source);
    g_source = NULL;
    g_loaded = false;
}

void edc_models_reset(void)
{
    free_all();
}

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0)
    {
        fclose(f);
        return NULL;
    }
    long n = ftell(f);
    if (n < 0)
    {
        fclose(f);
        return NULL;
    }
    rewind(f);
    char *buf = malloc((size_t)n + 1);
    if (!buf)
    {
        fclose(f);
        return NULL;
    }
    size_t r = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[r] = 0;
    return buf;
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

size_t edc_models_load(void)
{
    if (g_loaded)
        return g_provider_count;
    g_loaded = true;

    char *text = NULL;
    const char *env = getenv("EDGECLAW_MODELS_CATALOG");
    if (env && *env)
    {
        text = read_file(env);
        if (!text)
        {
            log_error("models: cannot read EDGECLAW_MODELS_CATALOG=%s", env);
            return 0;
        }
        g_source = strdup(env);
    }
    else
    {
        text = malloc(edc_bundled_models_catalog_size + 1);
        if (!text)
            return 0;
        memcpy(text, edc_bundled_models_catalog, edc_bundled_models_catalog_size);
        text[edc_bundled_models_catalog_size] = 0;
        g_source = strdup("<embedded>");
    }

    g_root = cJSON_Parse(text);
    free(text);
    if (!g_root)
    {
        log_error("models: catalog is not valid JSON");
        return 0;
    }

    cJSON *providers = cJSON_GetObjectItem(g_root, "providers");
    if (!cJSON_IsObject(providers))
    {
        log_error("models: catalog has no `providers` object");
        return 0;
    }

    size_t cap = (size_t)cJSON_GetArraySize(providers);
    g_provider_ids = calloc(cap ? cap : 1, sizeof(*g_provider_ids));
    if (!g_provider_ids)
        return 0;
    cJSON *p = NULL;
    cJSON_ArrayForEach(p, providers)
    {
        if (p->string)
            g_provider_ids[g_provider_count++] = strdup(p->string);
    }
    qsort(g_provider_ids, g_provider_count, sizeof(*g_provider_ids), cmp_str);
    log_info("models: loaded %zu providers from %s", g_provider_count, g_source);
    return g_provider_count;
}

static void ensure_loaded(void)
{
    if (!g_loaded)
        edc_models_load();
}

const char *edc_models_source(void)
{
    return g_source;
}

static const cJSON *provider_obj(const char *provider_id)
{
    if (!g_root || !provider_id)
        return NULL;
    const cJSON *providers = cJSON_GetObjectItem(g_root, "providers");
    const cJSON *p = providers ? cJSON_GetObjectItem(providers, provider_id) : NULL;
    return cJSON_IsObject(p) ? p : NULL;
}

size_t edc_models_provider_count(void)
{
    ensure_loaded();
    return g_provider_count;
}

const char *edc_models_provider_id(size_t index)
{
    ensure_loaded();
    return index < g_provider_count ? g_provider_ids[index] : NULL;
}

const char *edc_models_provider_name(const char *provider_id)
{
    ensure_loaded();
    const cJSON *p = provider_obj(provider_id);
    return p ? cJSON_GetStringValue(cJSON_GetObjectItem(p, "name")) : NULL;
}

const char *edc_models_provider_api(const char *provider_id)
{
    ensure_loaded();
    const cJSON *p = provider_obj(provider_id);
    return p ? cJSON_GetStringValue(cJSON_GetObjectItem(p, "api")) : NULL;
}

const struct cJSON *edc_models_provider_env(const char *provider_id)
{
    ensure_loaded();
    const cJSON *p = provider_obj(provider_id);
    const cJSON *e = p ? cJSON_GetObjectItem(p, "env") : NULL;
    return cJSON_IsArray(e) ? e : NULL;
}

static const cJSON *provider_models(const char *provider_id)
{
    const cJSON *p = provider_obj(provider_id);
    const cJSON *m = p ? cJSON_GetObjectItem(p, "models") : NULL;
    return cJSON_IsArray(m) ? m : NULL;
}

size_t edc_models_model_count(const char *provider_id)
{
    ensure_loaded();
    const cJSON *m = provider_models(provider_id);
    return m ? (size_t)cJSON_GetArraySize(m) : 0;
}

const char *edc_models_model_id_at(const char *provider_id, size_t index)
{
    ensure_loaded();
    const cJSON *m = provider_models(provider_id);
    return m ? cJSON_GetStringValue(cJSON_GetArrayItem(m, (int)index)) : NULL;
}

static const cJSON *model_obj(const char *model_id)
{
    if (!g_root || !model_id)
        return NULL;
    const cJSON *models = cJSON_GetObjectItem(g_root, "models");
    const cJSON *m = models ? cJSON_GetObjectItem(models, model_id) : NULL;
    return cJSON_IsObject(m) ? m : NULL;
}

bool edc_models_model_info(const char *model_id, struct edc_model_info *out)
{
    ensure_loaded();
    const cJSON *m = model_obj(model_id);
    if (!m)
        return false;
    if (out)
    {
        memset(out, 0, sizeof(*out));
        out->id = model_id;
        out->name = cJSON_GetStringValue(cJSON_GetObjectItem(m, "name"));
        const cJSON *c = cJSON_GetObjectItem(m, "context");
        out->context = cJSON_IsNumber(c) ? (int64_t)c->valuedouble : 0;
        const cJSON *o = cJSON_GetObjectItem(m, "output");
        out->output = cJSON_IsNumber(o) ? (int64_t)o->valuedouble : 0;
        out->tool_call = cJSON_IsTrue(cJSON_GetObjectItem(m, "tool_call"));
        out->reasoning = cJSON_IsTrue(cJSON_GetObjectItem(m, "reasoning"));
        out->attachment = cJSON_IsTrue(cJSON_GetObjectItem(m, "attachment"));
    }
    return true;
}

bool edc_models_provider_matches(const char *provider_id, const char *query)
{
    ensure_loaded();
    if (!query || !*query)
        return true;
    const char *name = edc_models_provider_name(provider_id);
    return contains_ci(provider_id, query) || contains_ci(name, query);
}

bool edc_models_model_matches(const char *model_id, const char *query)
{
    ensure_loaded();
    if (!query || !*query)
        return true;
    const cJSON *m = model_obj(model_id);
    const char *name = m ? cJSON_GetStringValue(cJSON_GetObjectItem(m, "name")) : NULL;
    return contains_ci(model_id, query) || contains_ci(name, query);
}

static bool array_has_string(const cJSON *arr, const char *s)
{
    if (!cJSON_IsArray(arr))
        return false;
    const cJSON *it = NULL;
    cJSON_ArrayForEach(it, arr)
    {
        if (cJSON_IsString(it) && strcmp(it->valuestring, s) == 0)
            return true;
    }
    return false;
}

bool edc_models_is_chat_model(const char *model_id)
{
    ensure_loaded();
    const cJSON *m = model_obj(model_id);
    if (!m)
        return false;
    if (cJSON_IsTrue(cJSON_GetObjectItem(m, "tool_call")))
        return true;
    const cJSON *mod = cJSON_GetObjectItem(m, "modalities");
    if (mod)
    {
        if (array_has_string(cJSON_GetObjectItem(mod, "input"), "text"))
            return true;
        if (array_has_string(cJSON_GetObjectItem(mod, "output"), "text"))
            return true;
    }
    return false;
}

const char *edc_models_provider_env_name(const char *provider_id, size_t index)
{
    ensure_loaded();
    const struct cJSON *env = edc_models_provider_env(provider_id);
    return env ? cJSON_GetStringValue(cJSON_GetArrayItem(env, (int)index)) : NULL;
}
