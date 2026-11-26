#include "edc_syntax.h"

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

int main(void)
{
    /* --- language registry --- */
    assert(edc_syntax_available("c"));
    assert(edc_syntax_available("h")); /* alias */
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
    assert(!edc_syntax_available("ruby"));

    puts("test_syntax: PASS");
    return 0;
}
