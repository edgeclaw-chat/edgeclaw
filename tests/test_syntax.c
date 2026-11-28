#include "edc_syntax.h"
#include "edc_syntax_lang.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Find a span covering the exact byte range [start,end). Returns the group, or
 * EDC_SYNTAX_COUNT when no such span exists. */
static edc_syntax_group span_at(const edc_syntax_result *r, uint32_t start,
                                uint32_t end)
{
    for (size_t i = 0; i < r->count; ++i)
        if (r->spans[i].start_byte == start && r->spans[i].end_byte == end)
            return r->spans[i].group;
    return EDC_SYNTAX_COUNT;
}

static void assert_sorted(const edc_syntax_result *r)
{
    for (size_t i = 1; i < r->count; ++i)
        assert(r->spans[i - 1].start_byte <= r->spans[i].start_byte);
}

static void test_predicates(void)
{
    /* c.scm has two identifier patterns: `(identifier) @variable` (no predicate)
     * and `((identifier) @constant (#match? @constant "^[A-Z][A-Z\\d_]*$"))`.
     * The tree-sitter C API does NOT evaluate #match?, so without our predicate
     * evaluation BOTH patterns match every identifier and dedupe makes even a
     * lowercase name a @constant (purple). Verify the predicate discriminates. */
    const char *code = "int MAX_COUNT = 1; int lower_name = 2;\n";
    edc_syntax_result r;
    assert(edc_syntax_highlight(code, strlen(code), "c", &r) == 0);

    uint32_t caps = (uint32_t)(strstr(code, "MAX_COUNT") - code);
    uint32_t low = (uint32_t)(strstr(code, "lower_name") - code);
    edc_syntax_group gcap = span_at(&r, caps, caps + 9);
    edc_syntax_group glow = span_at(&r, low, low + 10);
    printf("predicate: MAX_COUNT group=%s lower_name group=%s\n",
           edc_syntax_group_name(gcap), edc_syntax_group_name(glow));
    assert(gcap == EDC_SYNTAX_CONSTANT);   /* ALL_CAPS matches the regex */
    assert(glow == EDC_SYNTAX_VARIABLE);   /* lowercase rejected by the predicate */
    edc_syntax_result_free(&r);

    /* #eq? in javascript.scm: only `require(...)` is @function.builtin. */
    const char *js = "require('x'); other('y');\n";
    assert(edc_syntax_highlight(js, strlen(js), "javascript", &r) == 0);
    uint32_t req = (uint32_t)(strstr(js, "require") - js);
    /* require should be coloured as a function group (function/method/function.builtin
     * all map to the FUNCTION group); other() must not be force-coloured. */
    bool req_function = false;
    for (size_t i = 0; i < r.count; ++i)
        if (r.spans[i].start_byte == req && r.spans[i].group == EDC_SYNTAX_FUNCTION)
            req_function = true;
    assert(req_function);
    edc_syntax_result_free(&r);
}

static void test_shared_lang_map(void)
{
    /* The fs tool tags read_file bubbles via edc_syntax_lang_for_path; the
     * highlighter resolves names via the same table. Assert both agree, and
     * that every path-derived tag is actually registered (no drift). */
    struct { const char *path; const char *canonical; } cases[] = {
        {"x.c", "c"}, {"x.h", "c"}, {"x.hpp", "c"},
        {"x.cpp", "cpp"}, {"x.cc", "cpp"}, {"x.cxx", "cpp"}, {"x.c++", "cpp"},
        {"x.py", "python"}, {"x.sh", "bash"}, {"x.bash", "bash"}, {"x.zsh", "bash"},
        {"x.json", "json"}, {"x.go", "go"}, {"x.golang", "go"},
        {"x.js", "javascript"}, {"x.node", "javascript"},
        {"x.ts", "typescript"}, {"x.jsx", "tsx"}, {"x.tsx", "tsx"},
        {"x.rs", "rust"},
        {"x.java", "java"}, {"x.css", "css"}, {"x.rb", "ruby"},
        {"a/b/c/file.py", "python"}, /* full path */
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const char *lang = edc_syntax_lang_for_path(cases[i].path);
        assert(lang && strcmp(lang, cases[i].canonical) == 0);
        assert(edc_syntax_normalize_lang(cases[i].canonical) &&
               strcmp(edc_syntax_normalize_lang(cases[i].canonical), cases[i].canonical) == 0);
        /* The tag the fs tool emits must be one the highlighter accepts. */
        assert(edc_syntax_available(lang));
    }
    assert(edc_syntax_lang_for_path("x.unknown") == NULL);
    assert(edc_syntax_lang_for_path("noext") == NULL);
    assert(edc_syntax_lang_for_path(NULL) == NULL);
    assert(edc_syntax_normalize_lang("py") && strcmp(edc_syntax_normalize_lang("py"), "python") == 0);
    assert(edc_syntax_normalize_lang("hpp") && strcmp(edc_syntax_normalize_lang("hpp"), "c") == 0);
    puts("shared lang map: path/alias -> canonical, all tags registered");
}

int main(void)
{
    test_shared_lang_map();
    /* --- language registry --- */
    assert(edc_syntax_available("c"));
    assert(edc_syntax_available("h")); /* alias */
    assert(edc_syntax_available("java"));
    assert(edc_syntax_available("css"));
    assert(edc_syntax_available("ruby"));
    assert(edc_syntax_available("rb")); /* alias */
    assert(!edc_syntax_available("cobol"));
    assert(!edc_syntax_available(NULL));

    /* --- unknown language degrades cleanly --- */
    edc_syntax_result bad;
    memset(&bad, 0xAA, sizeof(bad));
    assert(edc_syntax_highlight("int x;", 6, "cobol", &bad) == -1);

    /* --- C highlighting --- */
    const char *code =
        "/* hi */\n"
        "int main(void) { return 42; }\n";
    edc_syntax_result r;
    assert(edc_syntax_highlight(code, strlen(code), "c", &r) == 0);
    assert(r.count > 0);
    assert_sorted(&r);

    uint32_t comment_s = 0, comment_e = 8; /* the leading comment is 8 bytes */
    uint32_t kw_start = (uint32_t)(strstr(code, "return") - code);
    uint32_t num_start = (uint32_t)(strstr(code, "42") - code);
    uint32_t fn_start = (uint32_t)(strstr(code, "main") - code);

    assert(span_at(&r, comment_s, comment_e) == EDC_SYNTAX_COMMENT);
    assert(span_at(&r, kw_start, kw_start + 6) == EDC_SYNTAX_KEYWORD);
    assert(span_at(&r, num_start, num_start + 2) == EDC_SYNTAX_NUMBER);
    assert(span_at(&r, fn_start, fn_start + 4) == EDC_SYNTAX_FUNCTION);

    /* stable group names for debugging / themes */
    assert(strcmp(edc_syntax_group_name(EDC_SYNTAX_KEYWORD), "keyword") == 0);
    assert(strcmp(edc_syntax_group_name(EDC_SYNTAX_COUNT), "unknown") == 0);

    edc_syntax_result_free(&r);

    /* --- empty input is a no-op success --- */
    assert(edc_syntax_highlight("", 0, "c", &r) == 0);
    assert(r.count == 0);
    edc_syntax_result_free(&r);

    /* --- all registered languages parse their own scm --- */
    {
        struct {
            const char *lang;
            const char *code;
            edc_syntax_group want; /* at least one span of this group */
        } cases[] = {
            {"c", "int f() { return 1; }", EDC_SYNTAX_KEYWORD},
            {"cpp", "int f() { return 1; }", EDC_SYNTAX_FUNCTION},
            {"python", "def f(x):\n    return \"hi\"\n", EDC_SYNTAX_KEYWORD},
            {"bash", "# a comment\necho hello\n", EDC_SYNTAX_COMMENT},
            {"json", "{\"key\": 42}", EDC_SYNTAX_NUMBER},
            {"go", "package main\nfunc main() {}", EDC_SYNTAX_KEYWORD},
            {"javascript", "const x = 42; // hi", EDC_SYNTAX_NUMBER},
            {"typescript", "let x: number = 1;", EDC_SYNTAX_TYPE},
            {"tsx", "const n = 42;", EDC_SYNTAX_NUMBER},
            {"rust", "fn main() { let x = 1; }", EDC_SYNTAX_KEYWORD},
            {"java", "public class X {}", EDC_SYNTAX_KEYWORD},
            {"css", "a { color: red; }", EDC_SYNTAX_PUNCTUATION},
            {"ruby", "def foo; end", EDC_SYNTAX_KEYWORD},
        };
        for (size_t ci = 0; ci < sizeof(cases) / sizeof(cases[0]); ++ci) {
            assert(edc_syntax_available(cases[ci].lang));
            edc_syntax_result rr;
            int rc = edc_syntax_highlight(cases[ci].code, strlen(cases[ci].code),
                                          cases[ci].lang, &rr);
            assert(rc == 0);
            assert(rr.count > 0);
            assert_sorted(&rr);
            /* no two spans cover the exact same range (predicate dedupe) */
            for (size_t i = 1; i < rr.count; ++i) {
                assert(!(rr.spans[i - 1].start_byte == rr.spans[i].start_byte &&
                         rr.spans[i - 1].end_byte == rr.spans[i].end_byte));
            }
            bool found = false;
            for (size_t i = 0; i < rr.count; ++i)
                if (rr.spans[i].group == cases[ci].want)
                    found = true;
            assert(found);
            edc_syntax_result_free(&rr);
        }
    }

    /* --- language aliases --- */
    assert(edc_syntax_available("py"));
    assert(edc_syntax_available("sh"));
    assert(edc_syntax_available("cc"));
    assert(edc_syntax_available("golang"));
    assert(edc_syntax_available("js"));
    assert(edc_syntax_available("ts"));
    assert(edc_syntax_available("tsx"));
    assert(edc_syntax_available("jsx"));
    assert(edc_syntax_available("rs"));
    assert(!edc_syntax_available("cobol"));

    test_predicates();

    puts("test_syntax: PASS");
    return 0;
}
