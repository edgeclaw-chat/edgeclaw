#include "edc_syntax.h"
#include "edc_syntax_lang.h"

#include "edc_embedded_queries.h"

#include <tree_sitter/api.h>

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

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

typedef struct {
    const char *name; /* canonical language tag (see edc_syntax_lang.h) */
    const TSLanguage *(*language)(void);
    const char *query;
    size_t query_size;
} syntax_lang;

static const syntax_lang LANGS[] = {
    {"c", tree_sitter_c, edc_query_c, EDC_QUERY_C_SIZE},
    {"cpp", tree_sitter_cpp, edc_query_cpp, EDC_QUERY_CPP_SIZE},
    {"python", tree_sitter_python, edc_query_python, EDC_QUERY_PYTHON_SIZE},
    {"bash", tree_sitter_bash, edc_query_bash, EDC_QUERY_BASH_SIZE},
    {"json", tree_sitter_json, edc_query_json, EDC_QUERY_JSON_SIZE},
    {"go", tree_sitter_go, edc_query_go, EDC_QUERY_GO_SIZE},
    {"javascript", tree_sitter_javascript, edc_query_javascript, EDC_QUERY_JAVASCRIPT_SIZE},
    {"typescript", tree_sitter_typescript, edc_query_typescript, EDC_QUERY_TYPESCRIPT_SIZE},
    {"tsx", tree_sitter_tsx, edc_query_tsx, EDC_QUERY_TSX_SIZE},
    {"rust", tree_sitter_rust, edc_query_rust, EDC_QUERY_RUST_SIZE},
};

static const size_t NLANGS = sizeof(LANGS) / sizeof(LANGS[0]);

static const syntax_lang *find_lang(const char *lang)
{
    /* Aliases/extension names are resolved by the shared mapping so the
     * highlighter and the fs tool's path tagging can never drift apart. */
    const char *canonical = edc_syntax_normalize_lang(lang);
    if (!canonical)
        return NULL;
    for (size_t i = 0; i < NLANGS; ++i)
        if (strcmp(canonical, LANGS[i].name) == 0)
            return &LANGS[i];
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

/* tree-sitter's C API records query predicates (#match?, #eq?, ...) but does
 * NOT evaluate them: ts_query_cursor_next_match returns matches even when a
 * predicate would reject them. nvim-treesitter queries rely on predicates to
 * distinguish e.g. an ALL_CAPS constant from a plain variable, so ignoring them
 * over-captures. Evaluate the two predicates our shipped .scm files actually
 * use; anything else (including nvim's "#is-not? local") is left as a no-op. */

/* Text of the capture with |capture_id| in |match|, or NULL when that capture
 * is not part of this match. */
static const char *capture_text(const TSQueryMatch *match, uint32_t capture_id,
                                const char *code, size_t *out_len)
{
    for (uint16_t i = 0; i < match->capture_count; i++)
    {
        if (match->captures[i].index != capture_id)
            continue;
        TSNode node = match->captures[i].node;
        uint32_t s = ts_node_start_byte(node), e = ts_node_end_byte(node);
        *out_len = (size_t)(e - s);
        return code + s;
    }
    return NULL;
}

/* One predicate = steps between two Done sentinels. Returns false only when a
 * recognised predicate actively rejects the match. */
static bool eval_predicate(const TSQueryPredicateStep *steps, uint32_t n,
                           const TSQuery *query, const TSQueryMatch *match,
                           const char *code)
{
    if (n < 3 || steps[0].type != TSQueryPredicateStepTypeString)
        return true; /* malformed/unknown shape: do not reject */
    uint32_t name_len = 0;
    const char *name = ts_query_string_value_for_id(query, steps[0].value_id, &name_len);
    /* tree-sitter stores predicate names WITHOUT the leading '#' that the .scm
     * syntax uses: the string value here is "match?", "eq?", "is-not?", etc. */
    bool is_match = name && strcmp(name, "match?") == 0;
    bool is_eq = name && strcmp(name, "eq?") == 0;
    if (!is_match && !is_eq)
        return true; /* #is-not? and friends: not a colour decision */

    uint32_t cap_id = 0;
    const char *arg = NULL;
    uint32_t arg_len = 0;
    bool have_cap = false;
    for (uint32_t i = 1; i < n; i++)
    {
        if (steps[i].type == TSQueryPredicateStepTypeCapture && !have_cap)
        {
            cap_id = steps[i].value_id;
            have_cap = true;
        }
        else if (steps[i].type == TSQueryPredicateStepTypeString && !arg)
        {
            arg = ts_query_string_value_for_id(query, steps[i].value_id, &arg_len);
        }
    }
    if (!have_cap || !arg)
        return true;

    size_t text_len = 0;
    const char *text = capture_text(match, cap_id, code, &text_len);
    if (!text)
        return true; /* capture absent from this match: nothing to reject */

    if (is_eq)
    {
        size_t arg_len = strlen(arg);
        return text_len == arg_len && memcmp(text, arg, arg_len) == 0;
    }
    /* #match?: compile the query's PCRE (the .scm patterns use \d, which the
     * POSIX ERE wrapper would not understand), match against the node text. */
    int err = 0;
    PCRE2_SIZE off = 0;
    pcre2_code *re = pcre2_compile((PCRE2_SPTR)arg, PCRE2_ZERO_TERMINATED, 0,
                                   &err, &off, NULL);
    if (!re)
        return true; /* bad pattern in a shipped query: do not reject */
    pcre2_match_data *md = pcre2_match_data_create_from_pattern(re, NULL);
    int rc = pcre2_match(re, (PCRE2_SPTR)text, text_len, 0, 0, md, NULL);
    pcre2_match_data_free(md);
    pcre2_code_free(re);
    return rc >= 0; /* PCRE2_ERROR_NOMATCH => reject */
}

/* True when all recognised predicates of this match hold. */
static bool match_predicates_hold(TSQuery *query, const TSQueryMatch *match, const char *code)
{
    uint32_t nsteps = 0;
    const TSQueryPredicateStep *steps =
        ts_query_predicates_for_pattern(query, match->pattern_index, &nsteps);
    if (!steps || nsteps == 0)
        return true;
    uint32_t start = 0;
    for (uint32_t i = 0; i < nsteps; i++)
    {
        if (steps[i].type != TSQueryPredicateStepTypeDone)
            continue;
        if (!eval_predicate(steps + start, i - start, query, match, code))
            return false;
        start = i + 1;
    }
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
        /* Honour #match?/#eq? predicates that the tree-sitter C API does not
         * evaluate itself (e.g. only ALL_CAPS identifiers are @constant). */
        if (!match_predicates_hold(query, &match, code))
            continue;
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
