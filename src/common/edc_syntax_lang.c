#include "edc_syntax_lang.h"

#include <string.h>

/* Single source of truth for languages and their name/extension aliases.
 * Keep in sync with the highlight queries embedded in //src/common:embedded_queries
 * (a language listed here must have a query file). */
typedef struct {
    const char *canonical;
    const char *aliases[6]; /* NULL-terminated */
} lang_alias;

static const lang_alias kLangs[] = {
    {"c", {"h", "hpp", NULL}},
    {"cpp", {"cc", "cxx", "c++", NULL}},
    {"python", {"py", NULL}},
    {"bash", {"sh", "shell", "zsh", NULL}},
    {"json", {NULL}},
    {"go", {"golang", NULL}},
    {"javascript", {"js", "node", NULL}},
    {"typescript", {"ts", NULL}},
    {"tsx", {"jsx", NULL}},
    {"rust", {"rs", NULL}},
    {"java", {NULL}},
    {"css", {NULL}},
    {"ruby", {"rb", NULL}},
};

static const char *lookup(const char *name, size_t len)
{
    for (size_t i = 0; i < sizeof(kLangs) / sizeof(kLangs[0]); i++) {
        if (strlen(kLangs[i].canonical) == len &&
            memcmp(kLangs[i].canonical, name, len) == 0)
            return kLangs[i].canonical;
        for (size_t j = 0; j < 6 && kLangs[i].aliases[j]; j++) {
            const char *a = kLangs[i].aliases[j];
            if (strlen(a) == len && memcmp(a, name, len) == 0)
                return kLangs[i].canonical;
        }
    }
    return NULL;
}

const char *edc_syntax_lang_for_path(const char *path)
{
    if (!path)
        return NULL;
    const char *dot = strrchr(path, '.');
    if (!dot || !dot[1])
        return NULL;
    return lookup(dot + 1, strlen(dot + 1));
}

const char *edc_syntax_normalize_lang(const char *name)
{
    if (!name)
        return NULL;
    return lookup(name, strlen(name));
}
