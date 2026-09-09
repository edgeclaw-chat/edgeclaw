#include "edc_cmd_parse.h"
#include <stdio.h>
#include <string.h>

static int passed = 0;
static int failed = 0;

#define T_PASS()                                                                                                       \
    do                                                                                                                 \
    {                                                                                                                  \
        passed++;                                                                                                      \
    } while (0)
#define T_FAIL(msg)                                                                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);                                                  \
        failed++;                                                                                                      \
    } while (0)
#define T_CHECK(cond, msg)                                                                                             \
    do                                                                                                                 \
    {                                                                                                                  \
        if (cond)                                                                                                      \
            T_PASS();                                                                                                  \
        else                                                                                                           \
            T_FAIL(msg);                                                                                               \
    } while (0)
#define T_STREQ(a, b, msg)                                                                                             \
    do                                                                                                                 \
    {                                                                                                                  \
        if ((a) && (b) && strcmp((a), (b)) == 0)                                                                       \
            T_PASS();                                                                                                  \
        else                                                                                                           \
            T_FAIL(msg);                                                                                               \
    } while (0)

static void check(const char *arg, const char *want_input, int want_literal, int want_bg, const char *label)
{
    struct edc_cmd_task_opts o;
    edc_cmd_parse_task_opts(sdsnew(arg), &o);
    T_STREQ(o.input, want_input, label);
    T_CHECK(o.literal == want_literal, label);
    T_CHECK(o.bg == want_bg, label);
    edc_cmd_task_opts_free(&o);
}

static void test_triple_single_raw(void)
{
    /* ''' wraps raw multi-line text: no $ expansion (literal=1) */
    check("'''raw $VAR text'''", "raw $VAR text", 1, 0, "''' raw block literal=1");
}

static void test_triple_double_expand(void)
{
    /* """ expands $VAR (literal=0) */
    check("\"\"\"dq text\"\"\"", "dq text", 0, 0, "\"\"\" dq block literal=0");
}

static void test_single_quote_raw(void)
{
    check("'sq text'", "sq text", 1, 0, "'sq' raw literal=1");
}

static void test_double_quote_expand(void)
{
    check("\"dq text\"", "dq text", 0, 0, "\"dq\" expands literal=0");
}

static void test_triple_multiline_bg(void)
{
    /* multi-line """ block with trailing --bg flag */
    struct edc_cmd_task_opts o;
    edc_cmd_parse_task_opts(sdsnew("\"\"\"line1\nline2\"\"\" --bg"), &o);
    T_STREQ(o.input, "line1\nline2", "\"\"\" multiline input");
    T_CHECK(o.bg == 1, "\"\"\" multiline --bg");
    T_CHECK(o.literal == 0, "\"\"\" multiline literal=0");
    edc_cmd_task_opts_free(&o);
}

static void test_plain_unquoted(void)
{
    /* unquoted text is taken verbatim (contains quotes but isn't truncated) */
    check("{\"a\": 1}", "{\"a\": 1}", 0, 0, "plain JSON verbatim");
}

static void test_flags_after_quotes(void)
{
    /* flags are parsed from text after the closing quote */
    struct edc_cmd_task_opts o;
    edc_cmd_parse_task_opts(sdsnew("\"\"\"work\"\"\" -d dep1 --group g1"), &o);
    T_STREQ(o.input, "work", "\"\"\" flags input");
    T_CHECK(o.dep_count == 1, "\"\"\" flags dep_count");
    T_CHECK(o.dep_ids != NULL && strcmp(o.dep_ids[0], "dep1") == 0, "\"\"\" flags dep id");
    T_STREQ(o.group_id, "g1", "\"\"\" flags group id");
    edc_cmd_task_opts_free(&o);
}

int main(void)
{
    test_triple_single_raw();
    test_triple_double_expand();
    test_single_quote_raw();
    test_double_quote_expand();
    test_triple_multiline_bg();
    test_plain_unquoted();
    test_flags_after_quotes();

    printf("test_cmd_parse: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
