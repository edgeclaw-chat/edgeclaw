#include "edc_prompts.h"

#include "edc_compat.h"
#include "edc_error.h"
#include "edc_file_utils.h"
#include "edc_resources.h"
#include "log.h"
#include "utarray.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* The description shown in the listing when there is no front matter: the first
 * non-empty line, truncated (a template's first line is often the whole sentence). */
#define EDC_PROMPT_DESC_FALLBACK_MAX 120

static UT_icd ut_prompt_ptr_icd = {sizeof(struct edc_prompt *), NULL, NULL, NULL};

void edc_prompt_free(struct edc_prompt *prompt)
{
    if (!prompt)
        return;
    if (prompt->name)
        sdsfree(prompt->name);
    if (prompt->description)
        sdsfree(prompt->description);
    if (prompt->argument_hint)
        sdsfree(prompt->argument_hint);
    if (prompt->body)
        sdsfree(prompt->body);
    if (prompt->source)
        sdsfree(prompt->source);
    free(prompt);
}

/* A bare file name only: no separator, not dot-prefixed, nothing outside [A-Za-z0-9._-].
 * This is what keeps a name from reaching outside the prompts directory. */
static bool edc_prompt_name_is_safe(const char *name)
{
    if (!name || !*name)
        return false;
    if (name[0] == '.')
        return false;
    for (const char *p = name; *p; p++)
    {
        unsigned char c = (unsigned char)*p;
        if (!(isalnum(c) || c == '-' || c == '_' || c == '.'))
            return false;
    }
    return true;
}

/* First non-empty line of |text|, trimmed and truncated; never NULL. */
static sds edc_prompt_first_line(const char *text)
{
    const char *p = text;
    while (*p)
    {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        sds line = sdstrim(sdsnewlen(p, len), " \t\r");
        if (sdslen(line) > 0)
        {
            if (sdslen(line) > EDC_PROMPT_DESC_FALLBACK_MAX)
                sdsrange(line, 0, EDC_PROMPT_DESC_FALLBACK_MAX - 1);
            return line;
        }
        sdsfree(line);
        if (!eol)
            break;
        p = eol + 1;
    }
    return sdsempty();
}

/* `description: "text"` / `argument-hint: '[focus]'` -- drop a matching pair of
 * surrounding quotes (the one bit of YAML scalar syntax a template actually uses). */
static sds edc_prompt_unquote(sds value)
{
    size_t n = sdslen(value);
    if (n >= 2 && (value[0] == '"' || value[0] == '\'') && value[0] == value[n - 1])
    {
        sds inner = sdsnewlen(value + 1, n - 2);
        sdsfree(value);
        return inner;
    }
    return value;
}

/* Parse `key: value` lines. Split on the FIRST ':' so a value may contain colons
 * (the sibling skills parser splits on every ':' -- deliberately not copied here). */
static void edc_prompt_parse_front_matter(sds meta, struct edc_prompt *prompt)
{
    int line_count = 0;
    sds *lines = sdssplitlen(meta, sdslen(meta), "\n", 1, &line_count);
    if (!lines)
        return;

    for (int i = 0; i < line_count; i++)
    {
        char *colon = strchr(lines[i], ':');
        if (!colon)
            continue;
        sds key = sdstrim(sdsnewlen(lines[i], (size_t)(colon - lines[i])), " \t\r");
        sds value = sdstrim(sdsnew(colon + 1), " \t\r");
        if (sdslen(key) == 0 || sdslen(value) == 0)
        {
            sdsfree(key);
            sdsfree(value);
            continue;
        }
        if (strcmp(key, "description") == 0)
        {
            if (prompt->description)
                sdsfree(prompt->description);
            prompt->description = edc_prompt_unquote(value);
        }
        else if (strcmp(key, "argument-hint") == 0)
        {
            if (prompt->argument_hint)
                sdsfree(prompt->argument_hint);
            prompt->argument_hint = edc_prompt_unquote(value);
        }
        else
        {
            sdsfree(value); /* unknown keys are ignored, not fatal */
        }
        sdsfree(key);
    }
    sdsfreesplitres(lines, line_count);
}

static struct edc_prompt *edc_prompt_parse(const char *name, const char *content)
{
    const char *body = content;

    struct edc_prompt *prompt = EDC_NEW0(struct edc_prompt, 1);
    prompt->name = sdsnew(name);

    if (strncmp(content, "---\n", 4) == 0)
    {
        const char *meta_start = content + 4;
        const char *meta_end = strstr(meta_start, "\n---");
        if (!meta_end)
        {
            /* Opened but never closed: a malformed file. Refused loudly -- the file's
             * body would otherwise be half front matter. */
            log_warn("prompt '%s': front matter opened with '---' but never closed; skipped", name);
            edc_prompt_free(prompt);
            return NULL;
        }
        sds meta = sdsnewlen(meta_start, (size_t)(meta_end - meta_start));
        edc_prompt_parse_front_matter(meta, prompt);
        sdsfree(meta);
        body = meta_end + 4; /* past "\n---" */
    }

    sds trimmed = sdstrim(sdsnew(body), " \t\r\n");
    if (sdslen(trimmed) == 0)
    {
        log_warn("prompt '%s': empty body; skipped", name);
        sdsfree(trimmed);
        edc_prompt_free(prompt);
        return NULL;
    }

    /* `$` belongs to the template layer, so translate their spelling unconditionally:
     * `$1` / `$@` / `$ARGUMENTS` / `${1:-x}` become our `{{…}}`. Task references are NOT
     * `$` any more (they are `{{LAST}}` / `{{<id>}}` / `{{SELF}}`, which this leaves
     * alone), so there is nothing to collide with and no per-file dialect to track. */
    prompt->body = edc_prompt_translate_dollar(trimmed);
    sdsfree(trimmed);

    if (!prompt->description)
        prompt->description = edc_prompt_first_line(prompt->body);

    return prompt;
}

struct edc_prompt *edc_prompt_load(const char *dir, const char *name)
{
    if (!dir || !edc_prompt_name_is_safe(name))
        return NULL;

    sds file = sdscat(sdsnew(name), ".md");
    sds path = edc_path_join(dir, file, NULL);
    sdsfree(file);

    EdcError *error = NULL;
    sds content = edc_file_read(path, &error);
    sdsfree(path);
    if (!content)
    {
        edc_error_free(error); /* absence is normal */
        return NULL;
    }

    /* The `$` -> `{{…}}` translation happens in edc_prompt_parse, so every path that
     * parses a template gets it exactly once. */
    struct edc_prompt *prompt = edc_prompt_parse(name, content);
    sdsfree(content);
    if (!prompt)
        return NULL;

    prompt->source = sdsdup(dir);
    return prompt;
}

/* ── Listing ───────────────────────────────────────────────── */

struct edc_prompts_scan
{
    UT_array *prompts;
    sds dir;
};

static void edc_prompts_scan_cb(const char *name, int type, void *userdata)
{
    (void)type;
    struct edc_prompts_scan *scan = (struct edc_prompts_scan *)userdata;

    size_t len = strlen(name);
    if (len <= 3 || strcmp(name + len - 3, ".md") != 0)
        return;

    sds base = sdsnewlen(name, len - 3);
    struct edc_prompt *prompt = edc_prompt_load(scan->dir, base);
    sdsfree(base);
    if (prompt)
        utarray_push_back(scan->prompts, &prompt);
}

static int edc_prompt_name_cmp(const void *a, const void *b)
{
    const struct edc_prompt *const *pa = (const struct edc_prompt *const *)a;
    const struct edc_prompt *const *pb = (const struct edc_prompt *const *)b;
    return strcmp((*pa)->name, (*pb)->name);
}

UT_array *edc_prompts_list(const char *dir)
{
    UT_array *prompts;
    utarray_new(prompts, &ut_prompt_ptr_icd);
    if (!dir)
        return prompts;

    struct edc_prompts_scan scan = {.prompts = prompts, .dir = sdsnew(dir)};
    EdcError *error = NULL;
    edc_dir_foreach(dir, edc_prompts_scan_cb, &scan, &error);
    edc_error_free(error);
    sdsfree(scan.dir);

    utarray_sort(prompts, edc_prompt_name_cmp);
    return prompts;
}

/* ── Over the resource roots (edc_resources.h) ─────────────── */
/* Where a root may keep its templates: pi uses prompts/, opencode and Claude Code use
 * command/ (+ commands/). A nested file name cannot be addressed by `/prompt <name>` (the
 * name is a bare file name), so only the flat files are read. */
static const char *const k_prompt_subdirs[] = {"prompts", "command", "commands"};

static struct edc_prompt *edc_prompts_find(UT_array *prompts, const sds name)
{
    for (size_t i = 0; i < utarray_len(prompts); i++)
    {
        struct edc_prompt **slot = (struct edc_prompt **)utarray_eltptr(prompts, (unsigned)i);
        if (slot && *slot && strcmp((*slot)->name, name) == 0)
            return *slot;
    }
    return NULL;
}

struct edc_prompt *edc_prompt_load_roots(const UT_array *roots, const char *name)
{
    if (!roots)
        return NULL;

    for (size_t r = 0; r < utarray_len(roots); r++)
    {
        struct edc_resource_dir **slot = (struct edc_resource_dir **)utarray_eltptr(roots, (unsigned)r);
        if (!slot || !*slot)
            continue;
        struct edc_resource_dir *root = *slot;

        for (size_t s = 0; s < sizeof(k_prompt_subdirs) / sizeof(k_prompt_subdirs[0]); s++)
        {
            sds dir = edc_path_join(root->path, k_prompt_subdirs[s], NULL);
            struct edc_prompt *p = edc_prompt_load(dir, name);
            sdsfree(dir);
            if (p)
            {
                /* Name the ROOT, not the subdirectory it happened to sit in: that is what
                 * "where did this come from" means to the user. */
                sdsfree(p->source);
                p->source = sdsdup(root->path);
                return p;
            }
        }
    }
    return NULL;
}

UT_array *edc_prompts_list_roots(const UT_array *roots)
{
    UT_array *out;
    utarray_new(out, &ut_prompt_ptr_icd);
    if (!roots)
        return out;

    for (size_t r = 0; r < utarray_len(roots); r++)
    {
        struct edc_resource_dir **slot = (struct edc_resource_dir **)utarray_eltptr(roots, (unsigned)r);
        if (!slot || !*slot)
            continue;
        struct edc_resource_dir *root = *slot;

        for (size_t s = 0; s < sizeof(k_prompt_subdirs) / sizeof(k_prompt_subdirs[0]); s++)
        {
            sds dir = edc_path_join(root->path, k_prompt_subdirs[s], NULL);
            UT_array *one = edc_prompts_list(dir);
            sdsfree(dir);

            for (size_t i = 0; i < utarray_len(one); i++)
            {
                struct edc_prompt **ps = (struct edc_prompt **)utarray_eltptr(one, (unsigned)i);
                struct edc_prompt *p = ps ? *ps : NULL;
                if (!p)
                    continue;
                if (edc_prompts_find(out, p->name))
                {
                    edc_prompt_free(p); /* shadowed by a higher-precedence root */
                    continue;
                }
                sdsfree(p->source);
                p->source = sdsdup(root->path);
                utarray_push_back(out, &p);
            }
            utarray_free(one); /* the array only: its elements moved into |out| */
        }
    }

    utarray_sort(out, edc_prompt_name_cmp);
    return out;
}

/* ── Argument splitting ────────────────────────────────────── */

sds *edc_prompt_split_args(const char *s, int *out_count)
{
    *out_count = 0;
    if (!s)
        return NULL;

    sds *args = NULL;
    int count = 0;
    const char *p = s;

    while (*p)
    {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
            p++;
        if (!*p)
            break;

        sds arg = sdsempty();
        char quote = 0;
        while (*p)
        {
            if (quote == 0 && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
                break;
            if (quote == 0 && (*p == '"' || *p == '\''))
            {
                quote = *p++;
                continue;
            }
            if (quote != 0 && *p == quote)
            {
                quote = 0;
                p++;
                continue;
            }
            if (*p == '\\' && *(p + 1))
            {
                arg = sdscatlen(arg, p + 1, 1);
                p += 2;
                continue;
            }
            arg = sdscatlen(arg, p, 1);
            p++;
        }

        args = realloc(args, sizeof(sds) * (size_t)(count + 1));
        args[count++] = arg;
    }

    *out_count = count;
    return args;
}

void edc_prompt_args_free(sds *args, int count)
{
    for (int i = 0; i < count; i++)
        sdsfree(args[i]);
    free(args);
}

/* ── Expansion ─────────────────────────────────────────────── */

/* 1-based positional lookup: out of range is the empty string, not a literal. */
static const char *edc_prompt_arg(const sds *args, int argc, int n)
{
    if (n < 1 || n > argc)
        return "";
    return args[n - 1];
}

static sds edc_prompt_join(const sds *args, int from, int to)
{
    sds out = sdsempty();
    for (int i = from; i < to; i++)
    {
        if (i > from)
            out = sdscat(out, " ");
        out = sdscat(out, args[i]);
    }
    return out;
}

/* Recognise the inside of `{{…}}`; false = not a form we know (leave it verbatim). */
static bool edc_prompt_braced(sds spec, const sds *args, int argc, sds *out)
{
    const char *p = spec;
    if (!p || !*p)
        return false;

    if (*p == '@')
    {
        const char *rest = p + 1;
        if (*rest == '\0')
        {
            *out = edc_prompt_join(args, 0, argc);
            return true;
        }
        if (strncmp(rest, ":-", 2) == 0)
        {
            *out = (argc > 0) ? edc_prompt_join(args, 0, argc) : sdsnew(rest + 2);
            return true;
        }
        if (*rest == ':')
        {
            const char *nump = rest + 1;
            char *end = NULL;
            long first = strtol(nump, &end, 10);
            if (end == nump)
                return false;
            int from = (first <= 0) ? 0 : (int)(first - 1);
            if (from > argc)
                from = argc;

            if (*end == '\0')
            {
                *out = edc_prompt_join(args, from, argc);
                return true;
            }
            if (*end == ':' && end[1])
            {
                char *end2 = NULL;
                long len = strtol(end + 1, &end2, 10);
                if (end2 == end + 1 || *end2 != '\0')
                    return false;
                int to = (len < 0) ? from : from + (int)len;
                if (to > argc)
                    to = argc;
                *out = edc_prompt_join(args, from, to);
                return true;
            }
        }
        return false;
    }

    /* A positional index. Multi-digit on purpose: `{{10}}` is the tenth argument (the
     * `$10` spelling this replaced could not say that). */
    if (isdigit((unsigned char)*p))
    {
        char *end = NULL;
        long idx = strtol(p, &end, 10);
        if (end == p || idx < 1 || idx > 1000000)
            return false;
        if (*end == '\0')
        {
            *out = sdsnew(edc_prompt_arg(args, argc, (int)idx));
            return true;
        }
        if (strncmp(end, ":-", 2) == 0)
        {
            const char *cur = edc_prompt_arg(args, argc, (int)idx);
            *out = sdsnew((cur && *cur) ? cur : end + 2);
            return true;
        }
    }
    return false;
}

/* ── `$` dialect: pi / Claude Code / opencode templates ────── */

static bool edc_prompt_is_digit(char c)
{
    return c >= '0' && c <= '9';
}

/* Is |inner| (the text between `${` and `}`) one of the forms we can spell with `{{…}}`?
 * Accepts `N`, `N:-D`, `@`, `@:-D`, `@:N`, `@:N:L` -- anything else is left to the input
 * layer (see the header). */
static bool edc_prompt_brace_ok(const sds inner)
{
    const char *s = inner;
    size_t n = sdslen(inner);
    if (n == 0)
        return false;

    if (s[0] == '@')
    {
        if (n == 1)
            return true; /* ${@} */
        if (n >= 3 && s[1] == ':' && s[2] == '-')
            return true; /* ${@:-D} */
        if (n >= 2 && s[1] == ':')
        {
            size_t i = 2;
            size_t d1 = 0;
            while (i < n && edc_prompt_is_digit(s[i]))
            {
                i++;
                d1++;
            }
            if (d1 == 0)
                return false;
            if (i == n)
                return true; /* ${@:N} */
            if (s[i] != ':')
                return false;
            i++;
            size_t d2 = 0;
            while (i < n && edc_prompt_is_digit(s[i]))
            {
                i++;
                d2++;
            }
            return d2 > 0 && i == n; /* ${@:N:L} */
        }
        return false;
    }

    if (edc_prompt_is_digit(s[0]))
    {
        size_t i = 0;
        while (i < n && edc_prompt_is_digit(s[i]))
            i++;
        if (i == n)
            return true; /* ${N} */
        return n - i >= 3 && s[i] == ':' && s[i + 1] == '-'; /* ${N:-D} */
    }
    return false;
}

sds edc_prompt_translate_dollar(const char *body)
{
    sds out = sdsempty();
    const char *p = body ? body : "";

    while (*p)
    {
        if (*p == '\\' && *(p + 1) == '$')
        {
            out = sdscatlen(out, "$", 1); /* \$ -> a literal $ */
            p += 2;
            continue;
        }
        if (*p != '$')
        {
            out = sdscatlen(out, p, 1);
            p++;
            continue;
        }

        const char *q = p + 1;
        sds rep = NULL;
        size_t used = 0;

        if (*q == '@')
        {
            rep = sdsnew("{{@}}");
            used = 2;
        }
        else if (edc_prompt_is_digit(*q))
        {
            rep = sdscatprintf(sdsempty(), "{{%c}}", *q);
            used = 2;
        }
        else if (strncmp(q, "ARGUMENTS", 9) == 0)
        {
            rep = sdsnew("{{@}}");
            used = 10;
        }
        else if (*q == '{')
        {
            const char *close = strchr(q + 1, '}');
            if (close)
            {
                sds inner = sdsnewlen(q + 1, (size_t)(close - q - 1));
                if (edc_prompt_brace_ok(inner))
                {
                    rep = sdscatprintf(sdsempty(), "{{%s}}", inner);
                    used = (size_t)(close - p) + 1;
                }
                sdsfree(inner);
            }
        }

        if (rep)
        {
            out = sdscatsds(out, rep);
            sdsfree(rep);
            p += used;
        }
        else
        {
            /* Not a template parameter: leave the `$` to the input layer. */
            out = sdscatlen(out, "$", 1);
            p++;
        }
    }
    return out;
}

sds edc_prompt_expand(const struct edc_prompt *prompt, sds *args, int argc)
{
    if (!prompt || !prompt->body)
        return sdsempty();

    sds out = sdsempty();
    const char *s = prompt->body;
    size_t len = sdslen(prompt->body);
    size_t i = 0;

    while (i < len)
    {
        if (s[i] == '{' && i + 1 < len && s[i + 1] == '{')
        {
            const char *close = strstr(s + i + 2, "}}");
            if (close)
            {
                sds spec = sdsnewlen(s + i + 2, (size_t)(close - (s + i + 2)));
                sds value = NULL;
                bool ok = edc_prompt_braced(spec, args, argc, &value);
                sdsfree(spec);
                if (ok)
                {
                    out = sdscatsds(out, value);
                    sdsfree(value);
                    i = (size_t)(close - s) + 2;
                    continue;
                }
            }
        }

        /* Everything else -- an unrecognised `{{…}}`, a lone `{`, and every `$` -- is
         * copied verbatim. (`$` is special in neither layer: the ecosystem's parameter
         * spellings were translated at load, and task references are `{{…}}`.) */
        out = sdscatlen(out, &s[i], 1);
        i++;
    }

    return out;
}
