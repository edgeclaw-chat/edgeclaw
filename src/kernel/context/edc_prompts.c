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
    if (prompt->shadowed)
    {
        for (int i = 0; prompt->shadowed[i]; i++)
            sdsfree(prompt->shadowed[i]);
        free(prompt->shadowed);
    }
    free(prompt);
}

/* Remember that |path| lost the name to |prompt| (see struct edc_prompt.shadowed). */
static void edc_prompt_add_shadowed(struct edc_prompt *prompt, sds path)
{
    int n = 0;
    if (prompt->shadowed)
        while (prompt->shadowed[n])
            n++;
    prompt->shadowed = realloc(prompt->shadowed, (size_t)(n + 2) * sizeof(sds));
    prompt->shadowed[n] = path ? path : sdsnew("");
    prompt->shadowed[n + 1] = NULL;
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

    /* Stored AS WRITTEN: `$1` / `$@` / `${1:-x}` are our placeholder spelling -- the one pi
     * / Claude Code / opencode use, which is what lets their files run unedited -- and the
     * legacy `{{…}}` alias is accepted by the same expander (edc_prompt_placeholder_at). */
    prompt->body = trimmed;

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
                struct edc_prompt *winner = edc_prompts_find(out, p->name);
                if (winner)
                {
                    /* Same name from a later (less specific) root: the first wins, but SAY
                     * so -- otherwise editing the shadowed file looks like it does nothing. */
                    sds path = sdsdup(p->source ? p->source : "");
                    log_warn("prompt '%s': %s is shadowed by %s", p->name, path, winner->source ? winner->source : "?");
                    edc_prompt_add_shadowed(winner, path);
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

/* The placeholder grammar lives in edc_prompt_braced (defined below, with the arg rules);
 * the recogniser asks IT whether a spec is a form we know, so the grammar stays in one
 * place. */
static bool edc_prompt_braced(sds spec, const sds *args, int argc, sds *out, long last_positional);

/* Which kind of argument reference a spec is. */
enum edc_prompt_spec_kind
{
    SPEC_NONE,       /* not a form we know: copied verbatim */
    SPEC_ALL,        /* @ / @:-D / @:N / @:N:L */
    SPEC_POSITIONAL, /* N / N:-D */
};

/* Where a placeholder starts: our `$…` (the spelling pi / Claude Code / opencode use --
 * running THEIR files unedited is the point) or the legacy `{{…}}` alias. On success
 * |*out_spec| is the canonical inner form ("1", "1:-D", "@", "@:2:1") and the result is how
 * many bytes were consumed; 0 = not a placeholder (copy the byte verbatim).
 *
 * The grammar lives in ONE place: edc_prompt_braced is asked whether the spec is a form we
 * know (with no arguments a positional resolves to the empty string, so false really means
 * "unrecognised").
 *
 *   $1 … $9          one positional argument -- SINGLE digit, as in a shell: `$10` is `$1`
 *                    followed by a literal `0`, not the tenth argument (`${10}` says that)
 *   $@ / $ARGUMENTS  every argument
 *   ${1} ${10} ${1:-D} ${@} ${@:-D} ${@:N} ${@:N:L}
 *   \$               a literal `$` (handled by the callers, which see the backslash)
 *
 * `{{…}}` is NOT a placeholder here. It used to be a legacy alias of these forms, but that
 * made one spelling mean two things (`{{…}}` is the INPUT layer's task reference) and it was
 * ours alone -- opencode and pi substitute only the `$` forms. So a `{{1}}` in a body is
 * plain text now, exactly as it is for them. */
static size_t edc_prompt_placeholder_at(const char *p, sds *out_spec)
{
    const char *spec_start = NULL;
    size_t spec_len = 0;
    size_t consumed = 0;

    if (p[0] == '$' && p[1] != '\0')
    {
        const char *q = p + 1;
        if (*q == '{')
        {
            const char *close = strchr(q + 1, '}');
            if (!close)
                return 0;
            spec_start = q + 1;
            spec_len = (size_t)(close - (q + 1));
            consumed = (size_t)(close - p) + 1;
        }
        else if (*q == '@')
        {
            spec_start = q;
            spec_len = 1;
            consumed = 2;
        }
        else if (*q >= '0' && *q <= '9')
        {
            spec_start = q;
            spec_len = 1; /* single digit on purpose -- see above */
            consumed = 2;
        }
        else if (strncmp(q, "ARGUMENTS", 9) == 0)
        {
            spec_start = "@"; /* canonical: it means every argument */
            spec_len = 1;
            consumed = 10;
        }
        else
        {
            return 0;
        }
    }
    else
    {
        return 0;
    }

    sds spec = sdsnewlen(spec_start, spec_len);
    sds dummy = NULL;
    const bool known = edc_prompt_braced(spec, NULL, 0, &dummy, 0);
    if (dummy)
        sdsfree(dummy);
    if (!known)
    {
        sdsfree(spec);
        return 0;
    }
    *out_spec = spec;
    return consumed;
}

/* Which kind of argument reference a (already recognised) spec is. */
static enum edc_prompt_spec_kind edc_prompt_spec_kind_of(const char *spec, long *out_index)
{
    if (out_index)
        *out_index = 0;
    if (!spec || !*spec)
        return SPEC_NONE;
    if (spec[0] == '@')
        return SPEC_ALL;

    char *end = NULL;
    const long idx = strtol(spec, &end, 10);
    if (end != spec && idx >= 1)
    {
        if (out_index)
            *out_index = idx;
        return SPEC_POSITIONAL;
    }
    return SPEC_NONE;
}

/* Walk the body's placeholders: the HIGHEST positional index, and whether any argument
 * reference at all is present. This drives the argument rules in edc_prompt_expand. */
static void edc_prompt_scan_args(const sds body, long *out_last, bool *out_any)
{
    long last = 0;
    bool any = false;
    const char *s = body;
    size_t len = sdslen(body);

    for (size_t i = 0; i < len;)
    {
        if (s[i] == '\\' && i + 1 < len && s[i + 1] == '$')
        {
            i += 2; /* an escaped dollar is text, not a reference */
            continue;
        }
        sds spec = NULL;
        const size_t used = edc_prompt_placeholder_at(s + i, &spec);
        if (used == 0)
        {
            i++;
            continue;
        }

        long idx = 0;
        const enum edc_prompt_spec_kind kind = edc_prompt_spec_kind_of(spec, &idx);
        sdsfree(spec);
        if (kind == SPEC_ALL)
            any = true;
        else if (kind == SPEC_POSITIONAL)
        {
            any = true;
            if (idx > last)
                last = idx;
        }
        i += used;
    }

    if (out_last)
        *out_last = last;
    if (out_any)
        *out_any = any;
}

/* Recognise the inside of a placeholder; false = not a form we know (leave it verbatim).
 * |last_positional| is the highest positional index in the body: that ONE takes every
 * remaining argument, so `/prompt x a b c` with `{{1}}` (or `$1`) hands the body "a b c"
 * instead of dropping "b c" (opencode's rule -- see the header). */
static bool edc_prompt_braced(sds spec, const sds *args, int argc, sds *out, long last_positional)
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
            if ((long)idx == last_positional)
                *out = edc_prompt_join(args, (int)(idx - 1), argc); /* the rest */
            else
                *out = sdsnew(edc_prompt_arg(args, argc, (int)idx));
            return true;
        }
        if (strncmp(end, ":-", 2) == 0)
        {
            if ((long)idx == last_positional)
            {
                /* The LAST positional takes the remaining arguments -- but when there is
                 * nothing left to take, the default still applies (that is what `:-` is
                 * for). */
                sds rest = edc_prompt_join(args, (int)(idx - 1), argc);
                if (sdslen(rest) > 0)
                {
                    *out = rest;
                    return true;
                }
                sdsfree(rest);
                *out = sdsnew(end + 2);
                return true;
            }
            const char *cur = edc_prompt_arg(args, argc, (int)idx);
            *out = sdsnew((cur && *cur) ? cur : end + 2);
            return true;
        }
    }
    return false;
}

sds edc_prompt_expand(const struct edc_prompt *prompt, sds *args, int argc)
{
    if (!prompt || !prompt->body)
        return sdsempty();

    /* The argument rules (opencode's, verified 2026-10-16 -- packages/opencode/src/session/
     * prompt.ts): the highest positional placeholder takes the remaining arguments, and a
     * body with NO reference at all gets the user's arguments appended instead. Either way
     * what the user typed reaches the model. */
    long last_positional = 0;
    bool any_reference = false;
    edc_prompt_scan_args(prompt->body, &last_positional, &any_reference);

    sds out = sdsempty();
    const char *s = prompt->body;
    size_t len = sdslen(prompt->body);
    size_t i = 0;

    while (i < len)
    {
        if (s[i] == '\\' && i + 1 < len && s[i + 1] == '$')
        {
            out = sdscatlen(out, "$", 1); /* \$ -> a literal $ */
            i += 2;
            continue;
        }

        sds spec = NULL;
        const size_t used = edc_prompt_placeholder_at(s + i, &spec);
        if (used > 0)
        {
            sds value = NULL;
            const bool ok = edc_prompt_braced(spec, args, argc, &value, last_positional);
            sdsfree(spec);
            if (ok)
            {
                out = sdscatsds(out, value);
                sdsfree(value);
                i += used;
                continue;
            }
        }

        /* Not a placeholder: copied verbatim (`$HOME`, a bare `{`, an unknown `{{…}}`, the
         * input layer's `{{LAST}}`/`{{<id>}}` task references, ...). */
        out = sdscatlen(out, &s[i], 1);
        i++;
    }

    /* Nothing in the body took the arguments, and the user gave some: append them. A
     * template that never mentions its arguments (or a migrated skill body that simply does
     * not spell them out) still hands the user's words to the model -- never a silent drop. */
    if (!any_reference && argc > 0)
    {
        sds joined = edc_prompt_join(args, 0, argc);
        out = sdscatprintf(out, "\n\n%s", joined);
        sdsfree(joined);
    }

    return out;
}
