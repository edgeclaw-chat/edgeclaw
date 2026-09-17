#include "edc_cmd_parse.h"
#include "edc_shell_vars.h"
#include <stdint.h>
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
    /* edc_cmd_parse_task_opts borrows its argument (it makes its own copy), so
     * the caller-side sds has to be released. */
    sds a = sdsnew(arg);
    edc_cmd_parse_task_opts(a, &o);
    sdsfree(a);
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
    sds a = sdsnew("\"\"\"line1\nline2\"\"\" --bg");
    edc_cmd_parse_task_opts(a, &o);
    sdsfree(a);
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
    sds a = sdsnew("\"\"\"work\"\"\" -d dep1 --group g1");
    edc_cmd_parse_task_opts(a, &o);
    sdsfree(a);
    T_STREQ(o.input, "work", "\"\"\" flags input");
    T_CHECK(o.dep_count == 1, "\"\"\" flags dep_count");
    T_CHECK(o.dep_ids != NULL && strcmp(o.dep_ids[0], "dep1") == 0, "\"\"\" flags dep id");
    T_STREQ(o.group_id, "g1", "\"\"\" flags group id");
    edc_cmd_task_opts_free(&o);
}

/* ── $VAR 展开的出参契约 ──────────────────────────────────────
 * `edc_shell_expand_vars` 是"出参一定写定"的: **成功**也要把 *out_error 置 NULL,
 * 这样调用方可以传一个未初始化的 `sds err;` 而不必自己先清零 (同 `edc_shell_spawn_task`)。
 * 用毒值 (绝不解引用的 0x1) 验"是函数在写它", 而不是调用方自己清的。 */
static sds stub_resolver(const char *name, void *user_data)
{
    (void)user_data;
    if (strcmp(name, "GOOD") == 0)
        return sdsnew("value");
    return NULL; /* 其余一律"查不到" */
}

static void test_expand_vars_out_error(void)
{
    sds err = (sds)(uintptr_t)1;
    sds out = edc_shell_expand_vars("plain text", stub_resolver, NULL, &err);
    T_STREQ(out, "plain text", "expand: 无变量 → 原样返回");
    T_CHECK(err == NULL, "expand: **成功**也要把 *out_error 置 NULL");
    sdsfree(out);

    err = (sds)(uintptr_t)1;
    out = edc_shell_expand_vars("$MISSING", stub_resolver, NULL, &err);
    T_CHECK(out == NULL, "expand: 查不到变量 → 返回 NULL");
    T_CHECK(err != NULL, "expand: 失败时 *out_error 带文案");
    sdsfree(err);

    err = (sds)(uintptr_t)1;
    out = edc_shell_expand_vars("$GOOD", stub_resolver, NULL, &err);
    T_STREQ(out, "value", "expand: 查得到 → 展开");
    T_CHECK(err == NULL, "expand: 成功路径 *out_error 仍被置 NULL");
    sdsfree(out);

    out = edc_shell_expand_vars("x", stub_resolver, NULL, NULL);
    T_STREQ(out, "x", "expand: out_error 传 NULL 也安全");
    sdsfree(out);
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
    test_expand_vars_out_error();

    printf("test_cmd_parse: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
