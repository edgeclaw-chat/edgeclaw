#include "edc_cmd_parse.h"
#include "edc_string_utils.h"
#include <stdlib.h>
#include <string.h>

#define DESC_MAX EDC_TASK_DESC_MAX

/* ── Quoted regions ──────────────────────────────────────────
 *
 * Quotes and escapes follow Python's string rules. The one EdgeClaw addition is
 * `{{…}}` interpolation, and that is the only thing the two quote kinds differ on:
 * single-quoted text is raw (literal = 1, no interpolation), double-quoted text
 * interpolates. Escapes are identical in both, and an unknown escape keeps its
 * backslash -- also as in Python. Not implemented: adjacent literal concatenation
 * (`"a" "b"`), which nothing here needs. */

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/* |*pp| points at a backslash; append what it names and move past the whole escape.
 * An unknown escape emits the backslash and leaves |*pp| on the escaped character, so
 * the caller's loop copies it next. */
static void append_escape(sds *out, const char **pp)
{
    const char *q = *pp + 1; /* the escaped character */
    switch (*q)
    {
    case 'n': *out = sdscatlen(*out, "\n", 1); *pp = q + 1; return;
    case 't': *out = sdscatlen(*out, "\t", 1); *pp = q + 1; return;
    case 'r': *out = sdscatlen(*out, "\r", 1); *pp = q + 1; return;
    case 'v': *out = sdscatlen(*out, "\v", 1); *pp = q + 1; return;
    case 'f': *out = sdscatlen(*out, "\f", 1); *pp = q + 1; return;
    case 'b': *out = sdscatlen(*out, "\b", 1); *pp = q + 1; return;
    case 'a': *out = sdscatlen(*out, "\a", 1); *pp = q + 1; return;
    case '\\':
    case '"':
    case '\'': *out = sdscatlen(*out, q, 1); *pp = q + 1; return;
    case 'x':
    {
        int hi = hex_digit(q[1]);
        if (hi >= 0)
        {
            int lo = hex_digit(q[2]);
            char ch = (char)((lo < 0) ? hi : hi * 16 + lo);
            *out = sdscatlen(*out, &ch, 1);
            *pp = q + ((lo < 0) ? 2 : 3);
            return;
        }
        break;
    }
    default: break;
    }

    *out = sdscatlen(*out, "\\", 1); /* unknown escape: the backslash stays */
    *pp = q;
}

/* Scan one quoted region: |q| points at the opening quote (one or three of them).
 * Returns the unescaped body (caller frees) and stores the position just past the
 * closing delimiter in *end_out; NULL when the region never closes. */
static sds scan_quoted(const char *q, bool triple, char quote, const char **end_out)
{
    const char *p = q + (triple ? 3 : 1);
    sds out = sdsempty();

    while (*p)
    {
        if (triple ? (p[0] == quote && p[1] == quote && p[2] == quote) : (*p == quote))
        {
            *end_out = p + (triple ? 3 : 1);
            return out;
        }
        if (*p == '\\' && p[1])
        {
            append_escape(&out, &p);
            continue;
        }
        out = sdscatlen(out, p, 1);
        p++;
    }

    sdsfree(out);
    return NULL;
}

sds edc_cmd_unquote(const char *s, const char **end_out)
{
    if (!s || !end_out)
        return NULL;

    bool triple = false;
    if (strlen(s) >= 3 && (strncmp(s, "\"\"\"", 3) == 0 || strncmp(s, "'''", 3) == 0))
        triple = true;
    if (!triple && *s != '"' && *s != '\'')
        return NULL;

    return scan_quoted(s, triple, *s, end_out);
}

void edc_cmd_parse_task_opts(sds arg, struct edc_cmd_task_opts *opts)
{
    if (!opts)
        return;
    memset(opts, 0, sizeof(*opts));
    if (!arg)
        return;

    sds remaining = sdsnew(arg);

    /* detect trailing & (background) */
    if (sdslen(remaining) > 0 && remaining[sdslen(remaining) - 1] == '&')
    {
        opts->bg = true;
        sdssetlen(remaining, sdslen(remaining) - 1);
        sdstrim(remaining, " \t");
    }

    /*
     * Input extraction follows a Python-style quote grammar:
     *   - argument starts with """ or ''': that whole block, which may span
     *     multiple lines, is taken as the input (up to the closing triple
     *     quote); anything after it is parsed for flags.
     *   - argument starts with a single '"' or '\'': that quoted chunk is the
     *     input, and everything after its closing quote is parsed for flags.
     *   - otherwise the entire remainder is the input verbatim, so pasted
     *     text that merely contains quotes (e.g. JSON) is never truncated;
     *     flags are not scanned in this mode.
     * Escapes are Python's in every case (see append_escape); the two quote kinds
     * differ only in {{…}} interpolation.
     */
    char *s = remaining;
    while (*s == ' ' || *s == '\t')
        s++;

    bool input_consumed = false;

    size_t left = sdslen(remaining) - (size_t)(s - remaining);
    bool triple = false;
    if (left >= 3 && (strncmp(s, "\"\"\"", 3) == 0 || strncmp(s, "'''", 3) == 0))
        triple = true;

    if (triple || *s == '"' || *s == '\'')
    {
        /* A quoted region opens the argument; everything after its closing delimiter
         * is the option area. An unterminated region is not one -- the whole text
         * stays the input, which is what pasted text relies on. */
        const char *end = NULL;
        opts->literal = (*s == '\'');
        opts->input = scan_quoted(s, triple, *s, &end);
        if (opts->input)
        {
            sdsrange(remaining, (int)(end - remaining), -1);
            sdstrim(remaining, " \t");
            input_consumed = true;
        }
    }

    if (!input_consumed)
    {
        /* unbalanced quotes / no leading quote: the whole text is the input */
        opts->input = sdsdup(remaining);
        sdsfree(remaining);
        remaining = NULL;
    }

    /* build desc: first DESC_MAX UTF-8 chars of input, truncated with "..." */
    if (opts->input)
    {
        opts->desc = edc_str_utf8_truncate(opts->input, DESC_MAX, true);
    }

    /* Parse flags from remaining. The block runs even when `remaining` is an
     * empty sds (which happens once a quoted input has consumed the whole
     * argument); the token loop is a no-op then, but `remaining` still has to
     * be released — the old `sdslen(...) > 0` guard skipped the free and leaked
     * one sds per such parse. */
    if (remaining)
    {
        sds work = sdsdup(remaining);
        char *p = work;
        while (*p)
        {
            while (*p == ' ')
                p++;
            if (*p == '\0')
                break;

            if (strncmp(p, "--bg", 4) == 0 && (p[4] == ' ' || p[4] == '\0'))
            {
                opts->bg = true;
                p += 4;
                continue;
            }
            /* --new-group: open a group of its own. Not a prefix of --bg nor --group. */
            if (strncmp(p, "--new-group", 11) == 0 && (p[11] == ' ' || p[11] == '\0'))
            {
                opts->new_group = true;
                p += 11;
                continue;
            }
            if (strncmp(p, "-d ", 3) == 0)
            {
                p += 3;
                while (*p == ' ')
                    p++;
                char *start = p;
                while (*p && *p != ' ')
                    p++;
                if (p > start)
                {
                    opts->dep_count++;
                    opts->dep_ids = realloc(opts->dep_ids, (size_t)opts->dep_count * sizeof(sds));
                    opts->dep_ids[opts->dep_count - 1] = sdsnewlen(start, p - start);
                }
                continue;
            }
            /* --with <id>: the explicit spelling of -d -- wait for that task AND inject its
             * output into this one's context (TASK_DEP_JOIN). Named "with" because that is
             * what it does; `-d` stays as the short form. */
            if (strncmp(p, "--with ", 7) == 0)
            {
                p += 7;
                while (*p == ' ')
                    p++;
                char *start = p;
                while (*p && *p != ' ')
                    p++;
                if (p > start)
                {
                    opts->dep_count++;
                    opts->dep_ids = realloc(opts->dep_ids, (size_t)opts->dep_count * sizeof(sds));
                    opts->dep_ids[opts->dep_count - 1] = sdsnewlen(start, p - start);
                }
                continue;
            }
            if (strncmp(p, "--group ", 8) == 0)
            {
                p += 8;
                while (*p == ' ')
                    p++;
                char *s = p;
                while (*p && *p != ' ')
                    p++;
                if (p > s)
                    opts->group_id = sdsnewlen(s, p - s);
                continue;
            }
            if (strncmp(p, "-g ", 3) == 0)
            {
                p += 3;
                while (*p == ' ')
                    p++;
                char *s = p;
                while (*p && *p != ' ')
                    p++;
                if (p > s)
                {
                    if (opts->group_id)
                        sdsfree(opts->group_id);
                    opts->group_id = sdsnewlen(s, p - s);
                }
                continue;
            }
            /* Anything else unrecognized is recorded, not skipped: the caller refuses the
             * line (option area = options only; see edc_cmd_task_opts.unknown_option). The
             * first one is kept for the message. */
            {
                char *start = p;
                while (*p && *p != ' ')
                    p++;
                if (!opts->unknown_option)
                    opts->unknown_option = sdsnewlen(start, (size_t)(p - start));
            }
        }
        sdsfree(work);
        sdsfree(remaining);
        remaining = NULL;
    }
}

bool edc_cmd_desc_needs_quotes(const char *arg)
{
    if (!arg)
        return false;

    const char *p = arg;
    while (*p == ' ' || *p == '\t')
        p++;

    /* Quote-led: everything after the closing quote is the option area, which is the
     * only shape where options are recognised (see edc_cmd_parse_task_opts). */
    if (*p == '"' || *p == '\'')
        return false;

    /* Walk the tokens; a token that starts with '-' (and is not just "-") is an
     * option the caller meant to pass, and it has nowhere to go. */
    while (*p)
    {
        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p)
            break;
        if (p[0] == '-' && p[1] && p[1] != ' ' && p[1] != '\t')
            return true;
        while (*p && *p != ' ' && *p != '\t')
            p++;
    }
    return false;
}

void edc_cmd_task_opts_free(struct edc_cmd_task_opts *opts)
{
    if (!opts)
        return;
    if (opts->desc)
        sdsfree(opts->desc);
    if (opts->input)
        sdsfree(opts->input);
    if (opts->group_id)
        sdsfree(opts->group_id);
    if (opts->unknown_option)
        sdsfree(opts->unknown_option);
    for (int i = 0; i < opts->dep_count; i++)
        sdsfree(opts->dep_ids[i]);
    free(opts->dep_ids);
    memset(opts, 0, sizeof(*opts));
}

/* ── the shell-line option vocabulary (one table, see edc_cmd_parse.h) ──────── */

/* Mirrors the TK_OPT / TK_OPTARG rules in edc_lex.l. When an option is added there, it
 * belongs here too -- otherwise the popup would offer what the lexer rejects, or stay
 * silent about what it accepts. The text stops at the argument shape so completion can
 * leave a space behind it ("-d " and not "-d <task>"). */
static const struct
{
    const char *text;
    const char *brief;
} k_shell_options[] = {
    {"--bg", "run the task in the background"},
    {"--new-group", "put the task in a group of its own"},
    {"-g <group>", "put the task in that existing group"},
    {"--group <id>", "same as -g"},
    {"-d <task>", "wait for that task and inject its output"},
    {"--with <task>", "same as -d"},
};
#define EDC_SHELL_OPTION_COUNT (sizeof(k_shell_options) / sizeof(k_shell_options[0]))

size_t edc_cmd_shell_option_count(void)
{
    return EDC_SHELL_OPTION_COUNT;
}

const char *edc_cmd_shell_option_text(size_t index)
{
    return index < EDC_SHELL_OPTION_COUNT ? k_shell_options[index].text : NULL;
}

const char *edc_cmd_shell_option_brief(size_t index)
{
    return index < EDC_SHELL_OPTION_COUNT ? k_shell_options[index].brief : NULL;
}
