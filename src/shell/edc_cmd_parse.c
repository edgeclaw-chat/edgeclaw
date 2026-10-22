#include "edc_cmd_parse.h"
#include "edc_string_utils.h"
#include <stdlib.h>
#include <string.h>

#define DESC_MAX EDC_TASK_DESC_MAX

/* ── Quoted regions ───────────────────────────────────────────
 *
 * Quotes and escapes follow BASH: see the quoting block below for the rules (and for
 * why the old Python escapes had to go). The one EdgeClaw addition is `{{…}}`
 * interpolation: `"…"` / `"""…"""` interpolate; the raw and ANSI-C forms do not.
 * Not implemented: adjacent literal concatenation (`"a" "b"`), which nothing here
 * needs. */

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

/* ── Quoting: bash's rules, not Python's ───────────────────────────────
 *
 *   '…'        RAW. Every byte is copied; a backslash is just a backslash.
 *              bash has no escape inside single quotes at all, and `\'` does not
 *              exist (to get a quote inside, close and reopen: 'it'\''s').
 *   "…"        literal except `\"`, `\\` and `\<newline>` (line continuation).
 *              That is the complete bash list once `$`/backtick expansion is out
 *              of the picture -- and this layer never expands anything. A plain
 *              `\n` here is therefore TWO characters, exactly as in bash.
 *   '''…'''    ANSI-C, the in-tree alias of bash's `$'…'` (see ansi_c_escape).
 *   $'…'       the same thing, spelled the way bash spells it.
 *   """…"""    `"…"` rules with the body allowed to hold a bare `"`.
 *
 * The previous implementation decoded Python escapes (`\n`, `\t`, `\xNN`) in BOTH
 * plain quote kinds, so `'a\nb'` produced a newline -- something bash never does.
 */

/* Append |cp| as UTF-8 (used by \u / \U). Invalid values become '?'. */
static void append_codepoint(sds *out, unsigned long cp)
{
    if (cp <= 0x7F)
    {
        const char one = (char)cp;
        *out = sdscatlen(*out, &one, 1);
    }
    else if (cp <= 0x7FF)
    {
        char buf[2];
        buf[0] = (char)(0xC0 | (cp >> 6));
        buf[1] = (char)(0x80 | (cp & 0x3F));
        *out = sdscatlen(*out, buf, 2);
    }
    else if (cp <= 0xFFFF)
    {
        char buf[3];
        buf[0] = (char)(0xE0 | (cp >> 12));
        buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[2] = (char)(0x80 | (cp & 0x3F));
        *out = sdscatlen(*out, buf, 3);
    }
    else if (cp <= 0x10FFFF)
    {
        char buf[4];
        buf[0] = (char)(0xF0 | (cp >> 18));
        buf[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        buf[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[3] = (char)(0x80 | (cp & 0x3F));
        *out = sdscatlen(*out, buf, 4);
    }
    else
    {
        *out = sdscatlen(*out, "?", 1);
    }
}

/* |*pp| points at a backslash inside an ANSI-C region ($'…' / '''…'''): append what
 * it names and move past the whole escape. The table is bash's; an unknown escape
 * keeps its backslash (`$'\q'` -> `\q`), again as bash does. */
static void ansi_c_escape(sds *out, const char **pp)
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
    case 'e': *out = sdscatlen(*out, "\033", 1); *pp = q + 1; return;
    case '\\':
    case '"':
    case '\'':
    case '?': *out = sdscatlen(*out, q, 1); *pp = q + 1; return;
    case 'x':
    {
        const int hi = hex_digit(q[1]);
        if (hi >= 0)
        {
            const int lo = hex_digit(q[2]);
            const char ch = (char)((lo < 0) ? hi : hi * 16 + lo);
            *out = sdscatlen(*out, &ch, 1);
            *pp = q + ((lo < 0) ? 2 : 3);
            return;
        }
        break;
    }
    case 'u':
    case 'U':
    {
        const int want = (*q == 'u') ? 4 : 8;
        unsigned long cp = 0;
        int got = 0;
        while (got < want && hex_digit(q[1 + got]) >= 0)
        {
            cp = cp * 16 + (unsigned long)hex_digit(q[1 + got]);
            got++;
        }
        if (got > 0)
        {
            append_codepoint(out, cp);
            *pp = q + 1 + got;
            return;
        }
        break;
    }
    default: break;
    }

    if (*q >= '0' && *q <= '7') /* \0NNN: up to three octal digits */
    {
        int v = 0;
        int used = 0;
        while (used < 3 && q[used] >= '0' && q[used] <= '7')
        {
            v = v * 8 + (q[used] - '0');
            used++;
        }
        const char ch = (char)(v & 0xFF);
        *out = sdscatlen(*out, &ch, 1);
        *pp = q + used;
        return;
    }

    *out = sdscatlen(*out, "\\", 1); /* unknown escape: the backslash stays */
    *pp = q;
}

typedef enum
{
    EDC_Q_RAW,    /* '…'            */
    EDC_Q_DOUBLE, /* "…"            */
    EDC_Q_ANSI,   /* '''…''' / $'…' */
} edc_quote_mode;

/* Scan the region that opens at |s| (|opener| = how many copies of the quote
 * character open it: 1 or 3). Returns the value with the delimiters removed
 * (caller frees) and stores the position just past the closing delimiter in
 * |*end_out|; NULL when the region never closes. */
static sds scan_region(const char *s, edc_quote_mode mode, int opener, const char **end_out)
{
    const char q = s[0];
    const char *p = s + opener;
    sds out = sdsempty();

    while (*p)
    {
        bool closes = true;
        for (int i = 0; i < opener; i++)
        {
            if (p[i] != q)
            {
                closes = false;
                break;
            }
        }
        if (closes)
        {
            *end_out = p + opener;
            return out;
        }

        if (mode == EDC_Q_ANSI && *p == '\\' && p[1])
        {
            ansi_c_escape(&out, &p);
            continue;
        }
        if (mode == EDC_Q_DOUBLE && *p == '\\' && p[1])
        {
            if (p[1] == '"' || p[1] == '\\')
            {
                out = sdscatlen(out, p + 1, 1);
                p += 2;
                continue;
            }
            if (p[1] == '\n') /* line continuation: both characters vanish */
            {
                p += 2;
                continue;
            }
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

    /* `$'…'`: ANSI-C quoting -- bash's only escape-carrying form. */
    if (s[0] == '$' && s[1] == '\'')
        return scan_region(s + 1, EDC_Q_ANSI, 1, end_out);

    if (strlen(s) >= 3)
    {
        if (strncmp(s, "\"\"\"", 3) == 0)
            return scan_region(s, EDC_Q_DOUBLE, 3, end_out);
        if (strncmp(s, "'''", 3) == 0) /* in-tree alias of $'…' */
            return scan_region(s, EDC_Q_ANSI, 3, end_out);
    }

    if (*s == '"')
        return scan_region(s, EDC_Q_DOUBLE, 1, end_out);
    if (*s == '\'')
        return scan_region(s, EDC_Q_RAW, 1, end_out);
    return NULL;
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
     * Input extraction follows bash's quote grammar:
     *   - argument starts with """ / ''' / $': that whole block, which may span
     *     multiple lines, is taken as the input (up to the closing 
     *     delimiter); anything after it is parsed for flags.
     *   - argument starts with a single '"' or '\'': that quoted chunk is the
     *     input, and everything after its closing quote is parsed for flags.
     *   - otherwise the entire remainder is the input verbatim, so pasted
     *     text that merely contains quotes (e.g. JSON) is never truncated;
     *     flags are not scanned in this mode.
     * Escapes follow bash (see the quoting block above): the plain quotes carry none, the
     * ANSI-C forms do, and "literal" also means "do not interpolate {{…}}".
     */
    char *s = remaining;
    while (*s == ' ' || *s == '\t')
        s++;

    bool input_consumed = false;

    /* A quoted region opens the argument; everything after its closing delimiter is the
     * option area. An unterminated region is not one -- the whole text stays the input,
     * which is what pasted text relies on. `'…'`, the ANSI-C forms (`'''…'''`, `$'…'`)
     * are literal: no {{…}} interpolation. `"…"` and `"""…"""` interpolate. */
    if (*s == '"' || *s == '\'' || (*s == '$' && s[1] == '\''))
    {
        const char *end = NULL;
        opts->input = edc_cmd_unquote(s, &end);
        if (opts->input)
        {
            opts->literal = (*s == '\'' || *s == '$');
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
