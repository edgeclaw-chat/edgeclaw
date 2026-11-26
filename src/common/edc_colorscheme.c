#include "edc_colorscheme.h"

#include "edc_error.h"
#include "edc_file_utils.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>

/* A colorscheme is a tiny file, but refusing to read an unbounded one keeps a
 * malformed install from stalling startup. 512 KiB is orders of magnitude more
 * than any real Vim theme (they are a few KB). */
#define EDC_CS_MAX_FILE (512u * 1024u)
#define EDC_CS_MAX_GROUPS 512
#define EDC_CS_MAX_LINKS 512
#define EDC_CS_MAX_NAME 96

/* ── raw colour values ───────────────────────────────────────────────────── */

enum cs_color_kind
{
    CS_UNSET = 0,
    CS_RGB,
    CS_NONE, /* "NONE": transparent / inherit */
    CS_FG_REF, /* "fg": Normal's foreground */
    CS_BG_REF, /* "bg": Normal's background */
};

struct cs_color
{
    enum cs_color_kind kind;
    uint32_t rgb;
};

struct cs_group
{
    char name[EDC_CS_MAX_NAME];
    struct cs_color fg;
    struct cs_color bg;
};

struct cs_link
{
    char from[EDC_CS_MAX_NAME];
    char to[EDC_CS_MAX_NAME];
};

struct cs_parser
{
    struct cs_group groups[EDC_CS_MAX_GROUPS];
    size_t group_count;
    struct cs_link links[EDC_CS_MAX_LINKS];
    size_t link_count;
};

static void cs_set_err(char *err, size_t cap, const char *msg)
{
    if (err && cap)
        snprintf(err, cap, "%s", msg);
}

static int cs_tolower(int c)
{
    return tolower((unsigned char)c);
}

static bool cs_ieq(const char *a, const char *b)
{
    while (*a && *b)
    {
        if (cs_tolower(*a) != cs_tolower(*b))
            return false;
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static bool cs_istarts(const char *s, const char *prefix)
{
    while (*prefix)
    {
        if (cs_tolower(*s) != cs_tolower(*prefix))
            return false;
        if (!*s)
            return false;
        s++;
        prefix++;
    }
    return true;
}

static bool cs_is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

static bool cs_valid_name(const char *s)
{
    if (!s || !*s)
        return false;
    for (const char *p = s; *p; p++)
    {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_' ||
              *p == '-'))
            return false;
    }
    return true;
}

/* ── named colours ────────────────────────────────────────────────────────── */

struct cs_named
{
    const char *name;
    uint32_t rgb;
};

/* The names colourschemes actually use: Vim's own 8, the xterm grey family,
 * and the common X11-ish extras. Case-insensitive lookup. */
static const struct cs_named k_named[] = {
    {"black", 0x000000},      {"white", 0xffffff},      {"red", 0xff0000},
    {"green", 0x008000},      {"yellow", 0xffff00},     {"blue", 0x0000ff},
    {"magenta", 0xff00ff},    {"purple", 0x800080},     {"cyan", 0x00ffff},
    {"gray", 0x808080},       {"grey", 0x808080},       {"orange", 0xffa500},
    {"brown", 0xa52a2a},      {"pink", 0xffc0cb},       {"gold", 0xffd700},
    {"coral", 0xff7f50},      {"salmon", 0xfa8072},     {"violet", 0xee82ee},
    {"indigo", 0x4b0082},     {"tan", 0xd2b48c},        {"khaki", 0xf0e68c},
    {"orchid", 0xda70d6},     {"plum", 0xdda0dd},       {"maroon", 0x800000},
    {"olive", 0x808000},      {"navy", 0x000080},       {"teal", 0x008080},
    {"aqua", 0x00ffff},       {"lime", 0x00ff00},       {"silver", 0xc0c0c0},
    {"darkgray", 0xa9a9a9},   {"darkgrey", 0xa9a9a9},   {"lightgray", 0xd3d3d3},
    {"lightgrey", 0xd3d3d3},  {"darkred", 0x8b0000},    {"darkgreen", 0x006400},
    {"darkblue", 0x00008b},   {"darkcyan", 0x008b8b},   {"darkmagenta", 0x8b008b},
    {"darkyellow", 0x808000}, {"lightred", 0xff6666},   {"lightgreen", 0x90ee90},
    {"lightblue", 0xadd8e6},  {"lightcyan", 0xe0ffff},  {"lightmagenta", 0xffb6c1},
    {"lightyellow", 0xffffe0},
};

static bool cs_parse_named(const char *s, uint32_t *out)
{
    char folded[64];
    size_t n = 0;
    for (; *s && n + 1 < sizeof(folded); s++)
        folded[n++] = (char)cs_tolower(*s);
    folded[n] = '\0';
    for (size_t i = 0; i < sizeof(k_named) / sizeof(k_named[0]); i++)
        if (strcmp(folded, k_named[i].name) == 0)
        {
            *out = k_named[i].rgb;
            return true;
        }
    return false;
}

/* xterm's 256-colour cube, for numeric ctermfg/ctermbg in themes that have no
 * GUI colours. Kept here rather than in C++ so the parser's output is complete
 * and the C++ side only has to map roles. */
static uint32_t cs_xterm256(int n)
{
    static const uint8_t cube[] = {0, 95, 135, 175, 215, 255};
    if (n < 0 || n > 255)
        return 0;
    if (n < 16)
    {
        static const uint32_t ansi[] = {0x000000, 0x800000, 0x008000, 0x808000, 0x000080, 0x800080,
                                        0x008080, 0xc0c0c0, 0x808080, 0xff0000, 0x00ff00, 0xffff00,
                                        0x0000ff, 0xff00ff, 0x00ffff, 0xffffff};
        return ansi[n];
    }
    if (n < 232)
    {
        int v = n - 16;
        int r = v / 36;
        int g = (v % 36) / 6;
        int b = v % 6;
        return ((uint32_t)cube[r] << 16) | ((uint32_t)cube[g] << 8) | (uint32_t)cube[b];
    }
    uint8_t g = (uint8_t)(8 + 10 * (n - 232));
    return ((uint32_t)g << 16) | ((uint32_t)g << 8) | (uint32_t)g;
}

static bool cs_parse_hex(const char *s, uint32_t *out)
{
    /* Accept #RRGGBB and #RGB (each nibble doubled), with either case. */
    if (*s != '#')
        return false;
    s++;
    size_t len = strlen(s);
    if (len == 6 || len == 3)
    {
        uint32_t v = 0;
        for (size_t i = 0; i < len; i++)
        {
            char c = (char)cs_tolower(s[i]);
            unsigned d;
            if (c >= '0' && c <= '9')
                d = (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f')
                d = (unsigned)(c - 'a' + 10);
            else
                return false;
            v = v * 16 + d;
        }
        if (len == 3)
            v = (((v >> 8) & 0xf) * 0x11u) << 16 | (((v >> 4) & 0xf) * 0x11u) << 8 | ((v & 0xf) * 0x11u);
        *out = v & 0xffffffu;
        return true;
    }
    return false;
}

/* Parse one `guifg=` / `ctermfg=` style value. */
static struct cs_color cs_parse_color_value(const char *v)
{
    struct cs_color c = {CS_UNSET, 0};
    if (!v || !*v)
        return c;
    if (cs_ieq(v, "none"))
    {
        c.kind = CS_NONE;
        return c;
    }
    if (cs_ieq(v, "fg"))
    {
        c.kind = CS_FG_REF;
        return c;
    }
    if (cs_ieq(v, "bg"))
    {
        c.kind = CS_BG_REF;
        return c;
    }
    if (cs_parse_hex(v, &c.rgb))
    {
        c.kind = CS_RGB;
        return c;
    }
    /* A plain decimal number is a cterm index; some themes use them for guifg
     * too, and mapping them is better than dropping the colour. */
    if (*v >= '0' && *v <= '9')
    {
        char *end = NULL;
        long n = strtol(v, &end, 10);
        if (end && *end == '\0' && n >= 0 && n <= 255)
        {
            c.kind = CS_RGB;
            c.rgb = cs_xterm256((int)n);
            return c;
        }
    }
    if (cs_parse_named(v, &c.rgb))
        c.kind = CS_RGB;
    return c;
}

/* ── tokenising one highlight line ───────────────────────────────────────── */

/* Fill |tokens| with whitespace-split words; stop at a Vim comment (`"`). */
static size_t cs_tokenize(char *line, char **tokens, size_t cap)
{
    size_t n = 0;
    char *p = line;
    /* Vimscript line comments start with a double quote. Values here never
     * legitimately contain one, so cutting there is safe. */
    char *quote = strchr(p, '"');
    if (quote)
        *quote = '\0';
    while (*p && n < cap)
    {
        while (*p && cs_is_space(*p))
            p++;
        if (!*p)
            break;
        tokens[n++] = p;
        while (*p && !cs_is_space(*p))
            p++;
        if (*p)
            *p++ = '\0';
    }
    return n;
}

static struct cs_group *cs_get_group(struct cs_parser *p, const char *name, bool create)
{
    for (size_t i = 0; i < p->group_count; i++)
        if (cs_ieq(p->groups[i].name, name))
            return &p->groups[i];
    if (!create || p->group_count >= EDC_CS_MAX_GROUPS)
        return NULL;
    struct cs_group *g = &p->groups[p->group_count++];
    memset(g, 0, sizeof(*g));
    snprintf(g->name, sizeof(g->name), "%s", name);
    return g;
}

static void cs_apply_kv(struct cs_group *g, char *token)
{
    char *eq = strchr(token, '=');
    if (!eq)
        return; /* gui=bold / cterm=italic: we do not paint attributes */
    *eq = '\0';
    const char *key = token;
    const char *val = eq + 1;
    struct cs_color parsed;
    if (cs_ieq(key, "guifg") || cs_ieq(key, "ctermfg") || cs_ieq(key, "fg"))
    {
        parsed = cs_parse_color_value(val);
        /* GUI colours win over cterm; an earlier guifg is not overwritten by a
         * later ctermfg on the same line. */
        if (cs_istarts(key, "gui") || g->fg.kind == CS_UNSET || g->fg.kind == CS_NONE)
        {
            if (parsed.kind != CS_UNSET || g->fg.kind == CS_UNSET)
                g->fg = parsed;
        }
    }
    else if (cs_ieq(key, "guibg") || cs_ieq(key, "ctermbg") || cs_ieq(key, "bg"))
    {
        parsed = cs_parse_color_value(val);
        if (cs_istarts(key, "gui") || g->bg.kind == CS_UNSET || g->bg.kind == CS_NONE)
        {
            if (parsed.kind != CS_UNSET || g->bg.kind == CS_UNSET)
                g->bg = parsed;
        }
    }
}

static void cs_parse_line(struct cs_parser *p, char *line)
{
    char *s = line;
    while (*s && cs_is_space(*s))
        s++;

    /* A command name ends at whitespace, at NUL, or at the `!` bang modifier
     * (`hi!`, `highlight!`) that real colorschemes use to force a redefine. */
    bool is_link_cmd = false;
    if (cs_istarts(s, "hilink") && (s[6] == '\0' || cs_is_space(s[6]) || s[6] == '!'))
    {
        is_link_cmd = true;
        s += 6;
    }
    else if (cs_istarts(s, "highlight") && (s[9] == '\0' || cs_is_space(s[9]) || s[9] == '!'))
        s += 9;
    else if (cs_istarts(s, "hi") && (s[2] == '\0' || cs_is_space(s[2]) || s[2] == '!'))
        s += 2;
    else
        return;

    /* Skip the bang modifier if present. */
    if (*s == '!')
        s++;

    char *tokens[80];
    size_t n = cs_tokenize(s, tokens, sizeof(tokens) / sizeof(tokens[0]));
    if (n == 0)
        return;

    if (is_link_cmd)
    {
        if (n < 2 || p->link_count >= EDC_CS_MAX_LINKS)
            return;
        snprintf(p->links[p->link_count].from, EDC_CS_MAX_NAME, "%s", tokens[0]);
        snprintf(p->links[p->link_count].to, EDC_CS_MAX_NAME, "%s", tokens[1]);
        p->link_count++;
        return;
    }

    size_t first_kv = 0;
    /* `hi default Foo ...`, `hi link Foo Bar` and `hi! link Foo Bar` are all valid
     * Vim. The modifiers come BEFORE the group name. */
    if (first_kv < n && cs_ieq(tokens[first_kv], "default"))
        first_kv++;
    if (first_kv < n && cs_ieq(tokens[first_kv], "link"))
    {
        if (first_kv + 2 >= n || p->link_count >= EDC_CS_MAX_LINKS)
            return;
        snprintf(p->links[p->link_count].from, EDC_CS_MAX_NAME, "%s", tokens[first_kv + 1]);
        snprintf(p->links[p->link_count].to, EDC_CS_MAX_NAME, "%s", tokens[first_kv + 2]);
        p->link_count++;
        return;
    }
    if (first_kv >= n)
        return;

    const char *name = tokens[first_kv];
    first_kv++;

    struct cs_group *g = cs_get_group(p, name, true);
    if (!g)
        return;
    for (size_t i = first_kv; i < n; i++)
        cs_apply_kv(g, tokens[i]);
}

/* ── link + reference resolution ─────────────────────────────────────────── */

static struct cs_group *cs_find_group(struct cs_parser *p, const char *name)
{
    return cs_get_group(p, name, false);
}

static void cs_resolve_one(struct cs_parser *p, struct cs_group *g, int depth)
{
    if (depth > 8)
        return;

    /* `hi link A B` etc. */
    for (size_t i = 0; i < p->link_count; i++)
    {
        if (!cs_ieq(p->links[i].from, g->name))
            continue;
        struct cs_group *target = cs_find_group(p, p->links[i].to);
        if (target && target != g)
        {
            /* The link makes A inherit B's unset-for-A fields. Resolve B first. */
            cs_resolve_one(p, target, depth + 1);
            if (g->fg.kind == CS_UNSET)
                g->fg = target->fg;
            if (g->bg.kind == CS_UNSET)
                g->bg = target->bg;
        }
        break;
    }

    struct cs_group *normal = cs_find_group(p, "Normal");
    if (!normal)
        return;
    if (g->fg.kind == CS_FG_REF || g->fg.kind == CS_BG_REF || g->bg.kind == CS_FG_REF || g->bg.kind == CS_BG_REF)
        cs_resolve_one(p, normal, depth + 1);
    if (g->fg.kind == CS_FG_REF)
        g->fg = normal->fg;
    if (g->fg.kind == CS_BG_REF)
        g->fg = normal->bg;
    if (g->bg.kind == CS_FG_REF)
        g->bg = normal->fg;
    if (g->bg.kind == CS_BG_REF)
        g->bg = normal->bg;
}

static void cs_resolve(struct cs_parser *p)
{
    /* Materialise every group named only as a link source or target. Otherwise a
     * `hi! link Todo Special` with no separate `hi Todo` line never becomes a
     * group, is never visited below, and roles mapped from Todo stay unset. */
    for (size_t i = 0; i < p->link_count; i++)
    {
        cs_get_group(p, p->links[i].from, true);
        cs_get_group(p, p->links[i].to, true);
    }
    for (size_t i = 0; i < p->group_count; i++)
        cs_resolve_one(p, &p->groups[i], 0);
}

/* ── group → role mapping ─────────────────────────────────────────────────── */

struct cs_binding
{
    int role;
    const char *group;
    bool background; /* take the group's bg rather than its fg */
};

/* Ordered by priority within a role; the first group that has a usable colour
 * wins. Names cover plain Vim, common plugin groups, and markdown. */
static const struct cs_binding k_bindings[] = {
    {EDC_CS_WINDOW_BG, "Normal", true},
    {EDC_CS_PANE_BG, "Normal", true},
    {EDC_CS_TEXT_DIM, "Comment", false},
    {EDC_CS_TEXT_DIM, "LineNr", false},
    {EDC_CS_TEXT_DIM, "NonText", false},
    {EDC_CS_TEXT, "Normal", false},
    {EDC_CS_LINE_DIM, "FoldColumn", false},
    {EDC_CS_LINE_DIM, "LineNr", false},
    {EDC_CS_LINE_DIM, "NonText", false},
    {EDC_CS_DIALOG_BG, "Pmenu", true},
    {EDC_CS_DIALOG_BG, "Visual", true},
    {EDC_CS_DIALOG_BG, "Folded", true},
    {EDC_CS_ACCENT, "Keyword", false},
    {EDC_CS_ACCENT, "Statement", false},
    {EDC_CS_ACCENT, "Function", false},
    {EDC_CS_ACCENT, "Identifier", false},
    {EDC_CS_ACCENT, "Special", false},
    {EDC_CS_OK, "DiffAdd", false},
    {EDC_CS_OK, "Added", false},
    {EDC_CS_WARN, "WarningMsg", false},
    {EDC_CS_WARN, "Todo", false},
    {EDC_CS_ERROR, "ErrorMsg", false},
    {EDC_CS_ERROR, "Error", false},
    {EDC_CS_INTERRUPTED, "Special", false},
    {EDC_CS_INTERRUPTED, "Number", false},
    {EDC_CS_THOUGHT_OPEN, "Todo", false},
    {EDC_CS_THOUGHT_CLOSED, "Comment", false},
    {EDC_CS_PROMPT, "Keyword", false},
    {EDC_CS_PROMPT, "Statement", false},
    {EDC_CS_USER_RULE, "Keyword", false},
    {EDC_CS_USER_RULE, "Statement", false},
    {EDC_CS_MD_H1, "markdownH1", false},
    {EDC_CS_MD_H1, "htmlH1", false},
    {EDC_CS_MD_H1, "Statement", false},
    {EDC_CS_MD_H1_BG, "markdownH1", true},
    {EDC_CS_MD_H1_BG, "htmlH1", true},
    {EDC_CS_MD_H2, "markdownH2", false},
    {EDC_CS_MD_H2, "htmlH2", false},
    {EDC_CS_MD_H2, "Constant", false},
    {EDC_CS_MD_H3, "markdownH3", false},
    {EDC_CS_MD_H3, "htmlH3", false},
    {EDC_CS_MD_H3, "Identifier", false},
    {EDC_CS_MD_H4, "markdownH4", false},
    {EDC_CS_MD_H4, "htmlH4", false},
    {EDC_CS_MD_H4, "PreProc", false},
    /* H5/H6 are first-class roles now: themes that define markdownH5/H6 get six
     * distinct heading colours instead of 5+6 collapsing onto md_h4. */
    {EDC_CS_MD_H5, "markdownH5", false},
    {EDC_CS_MD_H5, "htmlH5", false},
    {EDC_CS_MD_H5, "Statement", false},
    {EDC_CS_MD_H6, "markdownH6", false},
    {EDC_CS_MD_H6, "htmlH6", false},
    {EDC_CS_MD_H6, "String", false},
    {EDC_CS_MD_CODE, "markdownCode", false},
    {EDC_CS_MD_CODE, "String", false},
    /* Inline code deliberately avoids the fenced-code group: on themes where both
     * are the same colour the two kinds of code read as one. Constant / Number are
     * usually a different tint, so a `code span` stands apart from a fenced block. */
    {EDC_CS_MD_CODE_INLINE, "markdownCodeDelimiter", false},
    {EDC_CS_MD_CODE_INLINE, "Constant", false},
    {EDC_CS_MD_CODE_INLINE, "Number", false},
    {EDC_CS_MD_CODE_INLINE, "Include", false},
    {EDC_CS_MD_CODE_BG, "markdownCode", true},
    {EDC_CS_MD_CODE_BG, "markdownCodeBlock", true},
    {EDC_CS_MD_RULE, "markdownRule", false},
    {EDC_CS_MD_RULE, "markdownHeadingDelimiter", false},
    {EDC_CS_MD_RULE, "Comment", false},
    {EDC_CS_MD_QUOTE, "markdownQuote", false},
    {EDC_CS_MD_QUOTE, "markdownItalic", false},
    {EDC_CS_MD_QUOTE, "markdownEmphasis", false},
    {EDC_CS_MD_QUOTE, "String", false},
    {EDC_CS_MD_QUOTE, "Comment", false},
    {EDC_CS_MD_LIST, "markdownListMarker", false},
    {EDC_CS_MD_LIST, "markdownOrderedListMarker", false},
    {EDC_CS_MD_LIST, "Statement", false},
    /* Links take markdownLinkText first but fall back to a NAVIGATION-flavoured
     * group rather than Keyword (which is usually the heading/accent colour). */
    {EDC_CS_MD_LINK, "markdownLinkText", false},
    {EDC_CS_MD_LINK, "Underlined", false},
    {EDC_CS_MD_LINK, "Directory", false},
    {EDC_CS_MD_LINK, "Function", false},
    /* Table headers get their own role. Question / DiagnosticWarn are usually a
     * bright "attention" tint; markdownH1 is the last resort so most themes colour
     * the header row somehow. */
    {EDC_CS_MD_TABLE_HEAD, "markdownTableHead", false},
    {EDC_CS_MD_TABLE_HEAD, "Question", false},
    {EDC_CS_MD_TABLE_HEAD, "DiagnosticWarn", false},
    {EDC_CS_MD_TABLE_HEAD, "WarningMsg", false},
    {EDC_CS_MD_TABLE_HEAD, "markdownH1", false},
    /* PASTE_* are deliberately absent: the input chip's warm hue is content
     * semantics, not a Vim group, and the C++ side derives it from the theme. */
    {EDC_CS_MENTION_TEXT, "markdownListMarker", false},
    {EDC_CS_MENTION_TEXT, "Error", false},
    {EDC_CS_MENTION_TEXT, "Keyword", false},
};

static void cs_fill_roles(struct cs_parser *p, struct edc_colorscheme *out)
{
    for (size_t i = 0; i < sizeof(k_bindings) / sizeof(k_bindings[0]); i++)
    {
        const struct cs_binding *b = &k_bindings[i];
        if (out->set[b->role])
            continue;
        struct cs_group *g = cs_find_group(p, b->group);
        if (!g)
            continue;
        struct cs_color c = b->background ? g->bg : g->fg;
        if (c.kind == CS_RGB)
        {
            out->rgb[b->role] = c.rgb;
            out->set[b->role] = true;
        }
    }
}

/* ── public parser API ────────────────────────────────────────────────────── */

void edc_colorscheme_reset(struct edc_colorscheme *cs)
{
    if (!cs)
        return;
    memset(cs, 0, sizeof(*cs));
}

bool edc_colorscheme_parse(const char *text, size_t len, struct edc_colorscheme *out)
{
    if (!text || !out)
        return false;
    if (len == 0)
        len = strlen(text);

    char *buf = (char *)malloc(len + 1);
    if (!buf)
        return false;
    memcpy(buf, text, len);
    buf[len] = '\0';

    struct cs_parser parser;
    memset(&parser, 0, sizeof(parser));

    char *line = buf;
    while (line && *line)
    {
        char *end = strchr(line, '\n');
        if (end)
            *end = '\0';
        cs_parse_line(&parser, line);
        line = end ? end + 1 : NULL;
    }
    free(buf);

    cs_resolve(&parser);
    edc_colorscheme_reset(out);
    cs_fill_roles(&parser, out);

    /* A theme with only `highlight clear` and no groups is not usable. Normal's
     * foreground or background is the minimum signal we can build on. */
    return out->set[EDC_CS_TEXT] || out->set[EDC_CS_PANE_BG];
}

bool edc_colorscheme_parse_file(const char *path, struct edc_colorscheme *out, char *err, size_t err_cap)
{
    if (!path || !out)
    {
        cs_set_err(err, err_cap, "bad argument");
        return false;
    }
    EdcError *error = NULL;
    sds content = edc_file_read_capped(path, EDC_CS_MAX_FILE, NULL, &error);
    if (!content)
    {
        cs_set_err(err, err_cap, error && error->message ? error->message : "cannot read colorscheme");
        if (error)
            edc_error_free(error);
        return false;
    }
    const bool ok = edc_colorscheme_parse(content, (size_t)sdslen(content), out);
    sdsfree(content);
    if (!ok)
        cs_set_err(err, err_cap, "no usable highlight groups (Vimscript-computed themes are not supported)");
    return ok;
}

/* ── the colors directory ─────────────────────────────────────────────────── */

bool edc_colorscheme_dir(const char *edc_home, char *buf, size_t cap)
{
    if (!edc_home || !*edc_home || !buf || cap == 0)
        return false;
    int n = snprintf(buf, cap, "%s/colors", edc_home);
    return n > 0 && (size_t)n < cap;
}

bool edc_colorscheme_ensure_dir(const char *edc_home)
{
    char dir[1024];
    if (!edc_colorscheme_dir(edc_home, dir, sizeof(dir)))
        return false;
    return edc_dir_create_with_parents(dir, 0700, NULL);
}

bool edc_colorscheme_path(const char *edc_home, const char *name, char *buf, size_t cap)
{
    /* Reject anything that could escape the directory before it reaches a path. */
    if (!cs_valid_name(name))
        return false;
    char dir[1024];
    if (!edc_colorscheme_dir(edc_home, dir, sizeof(dir)))
        return false;
    int n = snprintf(buf, cap, "%s/%s.vim", dir, name);
    return n > 0 && (size_t)n < cap;
}

struct cs_list_ctx
{
    edc_cs_name_cb cb;
    void *ud;
    char **names;
    size_t count;
    size_t cap;
};

static void cs_list_entry(const char *name, int type, void *userdata)
{
    struct cs_list_ctx *ctx = (struct cs_list_ctx *)userdata;
    if (type != UV_DIRENT_FILE && type != UV_DIRENT_UNKNOWN)
        return;
    const char *dot = strrchr(name, '.');
    if (!dot || !cs_ieq(dot, ".vim"))
        return;
    size_t base = (size_t)(dot - name);
    if (base == 0 || base >= EDC_CS_MAX_NAME)
        return;
    char bare[EDC_CS_MAX_NAME];
    memcpy(bare, name, base);
    bare[base] = '\0';
    if (!cs_valid_name(bare))
        return;
    if (ctx->count == ctx->cap)
    {
        size_t next = ctx->cap ? ctx->cap * 2 : 16;
        char **grown = (char **)realloc(ctx->names, next * sizeof(char *));
        if (!grown)
            return;
        ctx->names = grown;
        ctx->cap = next;
    }
    ctx->names[ctx->count] = (char *)malloc(base + 1);
    if (!ctx->names[ctx->count])
        return;
    memcpy(ctx->names[ctx->count], bare, base + 1);
    ctx->count++;
}

static int cs_name_cmp(const void *a, const void *b)
{
    const char *const *sa = (const char *const *)a;
    const char *const *sb = (const char *const *)b;
    return strcmp(*sa, *sb);
}

size_t edc_colorscheme_list(const char *edc_home, edc_cs_name_cb cb, void *ud)
{
    char dir[1024];
    if (!edc_colorscheme_dir(edc_home, dir, sizeof(dir)))
        return 0;
    struct cs_list_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.cb = cb;
    ctx.ud = ud;
    if (!edc_dir_foreach(dir, cs_list_entry, &ctx, NULL))
        return 0;
    qsort(ctx.names, ctx.count, sizeof(char *), cs_name_cmp);
    for (size_t i = 0; i < ctx.count; i++)
    {
        if (cb)
            cb(ctx.names[i], ud);
        free(ctx.names[i]);
    }
    free(ctx.names);
    return ctx.count;
}

bool edc_colorscheme_load(const char *edc_home, const char *name, struct edc_colorscheme *out)
{
    if (!name || !*name || cs_ieq(name, "off") || cs_ieq(name, "builtin"))
        return false;
    char path[1200];
    if (!edc_colorscheme_path(edc_home, name, path, sizeof(path)))
        return false;
    return edc_colorscheme_parse_file(path, out, NULL, 0);
}

bool edc_colorscheme_install(const char *edc_home, const char *src_path, char *out_name, size_t name_cap, char *err,
                             size_t err_cap)
{
    if (!edc_home || !src_path || !out_name || name_cap == 0)
    {
        cs_set_err(err, err_cap, "bad argument");
        return false;
    }
    if (!edc_file_is_regular(src_path))
    {
        cs_set_err(err, err_cap, "source is not a regular file");
        return false;
    }

    /* Derive a safe name from the source's basename. */
    const char *slash = strrchr(src_path, '/');
#ifdef _WIN32
    const char *bslash = strrchr(src_path, '\\');
    if (bslash && (!slash || bslash > slash))
        slash = bslash;
#endif
    const char *base = slash ? slash + 1 : src_path;
    size_t len = strlen(base);
    if (len > 4 && cs_ieq(base + len - 4, ".vim"))
        len -= 4;
    if (len == 0 || len >= EDC_CS_MAX_NAME)
    {
        cs_set_err(err, err_cap, "bad theme name");
        return false;
    }
    char name[EDC_CS_MAX_NAME];
    size_t n = 0;
    for (size_t i = 0; i < len && n + 1 < sizeof(name); i++)
    {
        char c = base[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')
            name[n++] = c;
        else if (c == '.' && n > 0)
            name[n++] = '_';
    }
    name[n] = '\0';
    if (n == 0)
    {
        cs_set_err(err, err_cap, "bad theme name");
        return false;
    }

    /* Read with the same ceiling as the parser, then write into our own dir. */
    EdcError *error = NULL;
    sds content = edc_file_read_capped(src_path, EDC_CS_MAX_FILE, NULL, &error);
    if (!content)
    {
        cs_set_err(err, err_cap, error && error->message ? error->message : "cannot read source");
        if (error)
            edc_error_free(error);
        return false;
    }

    /* Verify it parses before installing -- a theme we cannot use is not an
     * install success, and the error tells the user why. */
    struct edc_colorscheme probe;
    if (!edc_colorscheme_parse(content, (size_t)sdslen(content), &probe))
    {
        cs_set_err(err, err_cap, "no usable highlight groups (Vimscript-computed themes are not supported)");
        sdsfree(content);
        return false;
    }

    if (!edc_colorscheme_ensure_dir(edc_home))
    {
        cs_set_err(err, err_cap, "cannot create the colors directory");
        sdsfree(content);
        return false;
    }
    char dst[1200];
    if (!edc_colorscheme_path(edc_home, name, dst, sizeof(dst)))
    {
        cs_set_err(err, err_cap, "bad theme name");
        sdsfree(content);
        return false;
    }
    if (!edc_file_write_safe(dst, content, (ssize_t)sdslen(content), NULL, &error))
    {
        cs_set_err(err, err_cap, error && error->message ? error->message : "cannot write colorscheme");
        if (error)
            edc_error_free(error);
        sdsfree(content);
        return false;
    }
    sdsfree(content);
    snprintf(out_name, name_cap, "%s", name);
    return true;
}
