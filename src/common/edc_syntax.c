#include "edc_syntax.h"

#include "edc_embedded_queries.h"

#include <tree_sitter/api.h>

#include <stdlib.h>
#include <string.h>

/*
 * Syntax highlighting engine.
 *
 * Each language contributes a grammar entry point (tree_sitter_<lang>) and a
 * highlight query compiled into the binary by //src/common:embedded_queries.
 * The query tags syntax nodes with nvim-treesitter-style capture names;
 * map_capture() folds them into the small, stable edc_syntax_group set so the
 * theme layer does not depend on grammar-specific capture names.
 */

/* Grammar entry points are C symbols from the parser.c archives. */
const TSLanguage *tree_sitter_c(void);
const TSLanguage *tree_sitter_cpp(void);
const TSLanguage *tree_sitter_python(void);
const TSLanguage *tree_sitter_bash(void);
const TSLanguage *tree_sitter_json(void);
const TSLanguage *tree_sitter_go(void);
const TSLanguage *tree_sitter_javascript(void);
const TSLanguage *tree_sitter_typescript(void);
const TSLanguage *tree_sitter_tsx(void);
const TSLanguage *tree_sitter_rust(void);

#define NALIASES 4

typedef struct {
    const char *name;
    const char *aliases[NALIASES]; /* NULL-terminated, excludes primary name */
    const TSLanguage *(*language)(void);
    const char *query;
    size_t query_size;
} syntax_lang;

static const syntax_lang LANGS[] = {
    {"c", {"h", "hpp", NULL}, tree_sitter_c,
     edc_query_c, EDC_QUERY_C_SIZE},
    {"cpp", {"cc", "cxx", "c++", NULL}, tree_sitter_cpp,
     edc_query_cpp, EDC_QUERY_CPP_SIZE},
    {"python", {"py", NULL}, tree_sitter_python,
     edc_query_python, EDC_QUERY_PYTHON_SIZE},
    {"bash", {"sh", "shell", "zsh", NULL}, tree_sitter_bash,
     edc_query_bash, EDC_QUERY_BASH_SIZE},
    {"json", {NULL}, tree_sitter_json,
     edc_query_json, EDC_QUERY_JSON_SIZE},
    {"go", {"golang", NULL}, tree_sitter_go,
     edc_query_go, EDC_QUERY_GO_SIZE},
    {"javascript", {"js", "node", NULL}, tree_sitter_javascript,
     edc_query_javascript, EDC_QUERY_JAVASCRIPT_SIZE},
    {"typescript", {"ts", NULL}, tree_sitter_typescript,
     edc_query_typescript, EDC_QUERY_TYPESCRIPT_SIZE},
    {"tsx", {"jsx", NULL}, tree_sitter_tsx,
     edc_query_tsx, EDC_QUERY_TSX_SIZE},
    {"rust", {"rs", NULL}, tree_sitter_rust,
     edc_query_rust, EDC_QUERY_RUST_SIZE},
};

static const size_t NLANGS = sizeof(LANGS) / sizeof(LANGS[0]);

static const syntax_lang *find_lang(const char *lang)
{
    if (!lang)
        return NULL;
    for (size_t i = 0; i < NLANGS; ++i) {
        if (strcmp(lang, LANGS[i].name) == 0)
            return &LANGS[i];
        for (size_t j = 0; j < NALIASES && LANGS[i].aliases[j]; ++j)
            if (strcmp(lang, LANGS[i].aliases[j]) == 0)
                return &LANGS[i];
    }
    return NULL;
}

/* Capture name -> semantic group. Prefix match handles dotted refinements
 * like "keyword.return" or "function.method.call". Longer/specific prefixes
 * are checked before short ones so "constant.builtin" is not mislabeled. */
static edc_syntax_group map_capture(const char *name, uint32_t len)
{
    if (len >= 11 && memcmp(name, "punctuation", 11) == 0)
        return EDC_SYNTAX_PUNCTUATION;
    if (len >= 8 && memcmp(name, "constant", 8) == 0)
        return EDC_SYNTAX_CONSTANT;
    if (len >= 8 && memcmp(name, "operator", 8) == 0)
        return EDC_SYNTAX_OPERATOR;
    if (len >= 8 && memcmp(name, "variable", 8) == 0)
        return EDC_SYNTAX_VARIABLE;
    if (len >= 8 && memcmp(name, "function", 8) == 0)
        return EDC_SYNTAX_FUNCTION;
    if (len >= 7 && memcmp(name, "comment", 7) == 0)
        return EDC_SYNTAX_COMMENT;
    if (len >= 7 && memcmp(name, "keyword", 7) == 0)
        return EDC_SYNTAX_KEYWORD;
    if (len >= 6 && memcmp(name, "string", 6) == 0)
        return EDC_SYNTAX_STRING;
    if (len >= 6 && memcmp(name, "number", 6) == 0)
        return EDC_SYNTAX_NUMBER;
    if (len >= 4 && memcmp(name, "type", 4) == 0)
        return EDC_SYNTAX_TYPE;
    return EDC_SYNTAX_COUNT; /* unknown capture: ignore */
}

static int span_cmp(const void *a, const void *b)
{
    const edc_syntax_span *sa = a, *sb = b;
    if (sa->start_byte < sb->start_byte)
        return -1;
    if (sa->start_byte > sb->start_byte)
        return 1;
    if (sa->end_byte < sb->end_byte)
        return -1;
    if (sa->end_byte > sb->end_byte)
        return 1;
    return 0;
}

/* Higher value wins when several captures cover the exact same byte range.
 * nvim-treesitter queries often attach several capture names to one node (e.g.
 * a Python parameter is @variable/@constant via a #match? predicate); the C
 * tree-sitter API does not evaluate predicates, so without this the renderer
 * would get conflicting colours for identical bytes. */
static int group_priority(edc_syntax_group g)
{
    switch (g) {
    case EDC_SYNTAX_FUNCTION: return 10;
    case EDC_SYNTAX_KEYWORD:  return 9;
    case EDC_SYNTAX_TYPE:     return 8;
    case EDC_SYNTAX_NUMBER:   return 7;
    case EDC_SYNTAX_STRING:   return 6;
    case EDC_SYNTAX_COMMENT:  return 5;
    case EDC_SYNTAX_CONSTANT: return 4;
    case EDC_SYNTAX_OPERATOR: return 3;
    case EDC_SYNTAX_VARIABLE: return 2;
    default:                  return 1;
    }
}

/* Sort, then collapse identical [start,end) ranges to the highest-priority
 * group. Operates in place. */
static size_t dedupe_spans(edc_syntax_span *spans, size_t count)
{
    if (count < 2)
        return count;
    qsort(spans, count, sizeof(spans[0]), span_cmp);
    size_t w = 0;
    for (size_t r = 1; r < count; ++r) {
        if (spans[r].start_byte == spans[w].start_byte &&
            spans[r].end_byte == spans[w].end_byte) {
            if (group_priority(spans[r].group) > group_priority(spans[w].group))
                spans[w].group = spans[r].group;
        } else {
            spans[++w] = spans[r];
        }
    }
    return w + 1;
}

static bool push_span(edc_syntax_result *out, uint32_t start, uint32_t end,
                      edc_syntax_group group)
{
    if (out->count == out->cap) {
        size_t ncap = out->cap ? out->cap * 2 : 16;
        edc_syntax_span *p = realloc(out->spans, ncap * sizeof(*p));
        if (!p)
            return false;
        out->spans = p;
        out->cap = ncap;
    }
    out->spans[out->count].start_byte = start;
    out->spans[out->count].end_byte = end;
    out->spans[out->count].group = group;
    out->count++;
    return true;
}

bool edc_syntax_available(const char *lang)
{
    return find_lang(lang) != NULL;
}

int edc_syntax_highlight(const char *code, size_t len, const char *lang,
                         edc_syntax_result *out)
{
    const syntax_lang *sl = find_lang(lang);
    if (!sl || !out || (!code && len))
        return -1;

    memset(out, 0, sizeof(*out));

    TSQueryError qerr = 0;
    uint32_t qerr_off = 0;
    TSQuery *query = ts_query_new(sl->language(), sl->query,
                                  (uint32_t)sl->query_size, &qerr_off, &qerr);
    if (!query)
        return -1;

    TSParser *parser = ts_parser_new();
    if (!parser) {
        ts_query_delete(query);
        return -1;
    }
    if (!ts_parser_set_language(parser, sl->language())) {
        ts_parser_delete(parser);
        ts_query_delete(query);
        return -1;
    }
    TSTree *tree = ts_parser_parse_string(parser, NULL, code, (uint32_t)len);
    if (!tree) {
        ts_parser_delete(parser);
        ts_query_delete(query);
        return -1;
    }

    TSQueryCursor *cursor = ts_query_cursor_new();
    ts_query_cursor_exec(cursor, query, ts_tree_root_node(tree));

    TSQueryMatch match;
    while (ts_query_cursor_next_match(cursor, &match)) {
        for (uint16_t i = 0; i < match.capture_count; ++i) {
            TSQueryCapture cap = match.captures[i];
            uint32_t nlen = 0;
            const char *cname = ts_query_capture_name_for_id(query, cap.index, &nlen);
            edc_syntax_group group = map_capture(cname, nlen);
            if (group == EDC_SYNTAX_COUNT)
                continue;
            uint32_t start = ts_node_start_byte(cap.node);
            uint32_t end = ts_node_end_byte(cap.node);
            if (end > start && !push_span(out, start, end, group))
                goto fail;
        }
    }

    out->count = dedupe_spans(out->spans, out->count);

    ts_query_cursor_delete(cursor);
    ts_tree_delete(tree);
    ts_parser_delete(parser);
    ts_query_delete(query);
    return 0;

fail:
    edc_syntax_result_free(out);
    ts_query_cursor_delete(cursor);
    ts_tree_delete(tree);
    ts_parser_delete(parser);
    ts_query_delete(query);
    return -1;
}

void edc_syntax_result_free(edc_syntax_result *out)
{
    if (!out)
        return;
    free(out->spans);
    out->spans = NULL;
    out->count = 0;
    out->cap = 0;
}

const char *edc_syntax_group_name(edc_syntax_group group)
{
    static const char *names[] = {
        "comment", "keyword", "string", "number", "function",
        "variable", "type", "operator", "constant", "punctuation",
    };
    if ((size_t)group >= EDC_SYNTAX_COUNT)
        return "unknown";
    return names[group];
}
