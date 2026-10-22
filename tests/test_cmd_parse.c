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

/* ── `$` 的语法：这一层没有"字面 $"这条写法 ──────────────────────
 *
 * 这一层（`edc_shell_expand_vars`）的 `$` 是**任务引用**，不是变量插值：
 *   `$$`    → 当前前台任务的 task_id（`edc_llm_request.c` 的 resolver；**不是**转义）
 *   `$N`    → 任务 N 的输出
 *   `$LAST` → 最后一个已完成任务的输出
 *   `$NAME` → 任务 NAME 的输出
 * 引用不到是**硬错误**（返回 NULL + 文案），且 `collect_var_refs` 那时已经把依赖建好了。
 *
 * 为什么钉它：`/prompt` 的模板层也用 `$`（`$1` = 第一个参数），两层的 `$N` 直接撞车。
 * 在决定"模板里怎么写字面 $"之前，必须先有这份事实，否则只是猜。 */
static sds shape_resolver(const char *name, void *user_data)
{
    (void)user_data;
    if (strcmp(name, "SELF") == 0)
        return sdsnew("<front>");
    if (strcmp(name, "1") == 0)
        return sdsnew("<task1-output>");
    if (strcmp(name, "LAST") == 0)
        return sdsnew("<last-output>");
    return NULL;
}

/* Task references are `{{…}}`, not `$…`: `$` belongs to the PROMPT TEMPLATE layer
 * (pi / Claude Code / opencode spell their parameters `$1`, `$@`, `${1:-x}`), so the two
 * layers get one sigil each and can never collide. */
static void test_expand_task_refs(void)
{
    sds err = NULL;
    sds out = NULL;

    out = edc_shell_expand_vars("{{SELF}}", shape_resolver, NULL, &err);
    T_STREQ(out, "<front>", "expand: `{{SELF}}` = 当前前台任务（原 `$$`）");
    sdsfree(out);

    out = edc_shell_expand_vars("{{1}}", shape_resolver, NULL, &err);
    T_STREQ(out, "<task1-output>", "expand: `{{1}}` = 任务 1 的输出");
    sdsfree(out);

    out = edc_shell_expand_vars("{{LAST}}", shape_resolver, NULL, &err);
    T_STREQ(out, "<last-output>", "expand: `{{LAST}}` = 最后完成的任务输出");
    sdsfree(out);

    /* 单引号是**唯一**能挡住这一层的东西；双引号挡不住。 */
    out = edc_shell_expand_vars("'{{1}}'", shape_resolver, NULL, &err);
    T_STREQ(out, "'{{1}}'", "expand: 单引号内不展开——但**引号本身留在文本里**");
    sdsfree(out);

    out = edc_shell_expand_vars("\"{{1}}\"", shape_resolver, NULL, &err);
    T_STREQ(out, "\"<task1-output>\"", "expand: 双引号挡不住 `{{…}}`");
    sdsfree(out);

    /* 不是我们的形式 → **原样保留**，不报错。这是粘贴安全的那条规则：`{{ var }}`
     * （Jinja / Handlebars）、`$HOME`、`$5.00` 都照原样过去。 */
    out = edc_shell_expand_vars("{{ var }}", shape_resolver, NULL, &err);
    T_STREQ(out, "{{ var }}", "expand: 不认识的形式原样保留（不报错）");
    sdsfree(out);

    out = edc_shell_expand_vars("a $ b and $HOME", shape_resolver, NULL, &err);
    T_STREQ(out, "a $ b and $HOME", "expand: `$` 不再是引用 → 原样");
    sdsfree(out);

    /* 未闭合的 `{{` 就是两个普通字节。 */
    out = edc_shell_expand_vars("a {{ b", shape_resolver, NULL, &err);
    T_STREQ(out, "a {{ b", "expand: 未闭合的 `{{` 原样留下");
    sdsfree(out);
}

/* ── 展开的出参契约 ──────────────────────────────────────────
 * `edc_shell_expand_vars` 是"出参一定写定"的: **成功**也要把 *out_error 置 NULL,
 * 这样调用方可以传一个未初始化的 `sds err;` 而不必自己先清零 (同 `edc_shell_spawn_task`)。
 * 用毒值 (绝不解引用的 0x1) 验"是函数在写它", 而不是调用方自己清的。 */
static sds stub_resolver(const char *name, void *user_data)
{
    (void)user_data;
    if (strcmp(name, "LAST") == 0)
        return sdsnew("value");
    return NULL; /* 其余一律"查不到" */
}

static void test_expand_vars_out_error(void)
{
    sds err = (sds)(uintptr_t)1;
    sds out = edc_shell_expand_vars("plain text", stub_resolver, NULL, &err);
    T_STREQ(out, "plain text", "expand: 无引用 → 原样返回");
    T_CHECK(err == NULL, "expand: **成功**也要把 *out_error 置 NULL");
    sdsfree(out);

    /* 认识的形式但查不到 → 报错（打字错要吵）；不认识的形式 → 原样（粘贴要安全）。 */
    err = (sds)(uintptr_t)1;
    out = edc_shell_expand_vars("{{7}}", stub_resolver, NULL, &err);
    T_CHECK(out == NULL, "expand: 认识的形式查不到 → 返回 NULL");
    T_CHECK(err != NULL, "expand: 失败时 *out_error 带文案");
    sdsfree(err);

    err = (sds)(uintptr_t)1;
    out = edc_shell_expand_vars("{{LAST}}", stub_resolver, NULL, &err);
    T_STREQ(out, "value", "expand: 查得到 → 展开");
    T_CHECK(err == NULL, "expand: 成功路径 *out_error 仍被置 NULL");
    sdsfree(out);

    out = edc_shell_expand_vars("x", stub_resolver, NULL, NULL);
    T_STREQ(out, "x", "expand: out_error 传 NULL 也安全");
    sdsfree(out);
}

static void test_desc_needs_quotes(void)
{
    /* No options -> any shape is fine. */
    T_CHECK(edc_cmd_desc_needs_quotes(NULL) == false, "needs_quotes: NULL -> false");
    T_CHECK(edc_cmd_desc_needs_quotes("plain description") == false, "needs_quotes: plain text -> false");
    T_CHECK(edc_cmd_desc_needs_quotes("{\"a\": 1}") == false, "needs_quotes: json-ish text -> false");
    T_CHECK(edc_cmd_desc_needs_quotes("a - b") == false, "needs_quotes: a lone '-' is not an option");

    /* Quote-led -> the option area after it is legal. */
    T_CHECK(edc_cmd_desc_needs_quotes("\"quoted desc\"") == false, "needs_quotes: quoted -> false");
    T_CHECK(edc_cmd_desc_needs_quotes("'quoted desc'") == false, "needs_quotes: sq quoted -> false");
    T_CHECK(edc_cmd_desc_needs_quotes("\"\"\"multi\"\"\" -d d1") == false, "needs_quotes: quoted + opt -> false");
    T_CHECK(edc_cmd_desc_needs_quotes("  \"spaced\" --bg") == false, "needs_quotes: leading spaces trimmed");

    /* Unquoted but carrying an option -> the description must be quoted. */
    T_CHECK(edc_cmd_desc_needs_quotes("desc --bg") == true, "needs_quotes: unquoted --bg -> true");
    T_CHECK(edc_cmd_desc_needs_quotes("desc -d d1") == true, "needs_quotes: unquoted -d -> true");
    T_CHECK(edc_cmd_desc_needs_quotes("  lead --group g1") == true, "needs_quotes: leading spaces, option");
    T_CHECK(edc_cmd_desc_needs_quotes("analyze \"find bugs\" --bg") == true,
             "needs_quotes: option after a bare first word -> true");
}

static void test_escapes(void)
{
    /* bash quoting: the plain quotes carry NO escapes. `\n` inside "…" is two
     * characters, exactly as in bash; the escapes live in the ANSI-C forms --
     * `$'…'`, and its in-tree alias '''…''' (triple single quotes). */
    check("\"a\\nb\"", "a\\nb", 0, 0, "dq: \\n is backslash + n, not a newline");
    check("\"a\\tb\"", "a\\tb", 0, 0, "dq: \\t is backslash + t");
    check("\"a\\\\b\"", "a\\b", 0, 0, "dq: \\\\ is one backslash");
    check("\"a\\\"b\"", "a\"b", 0, 0, "dq: \\\" is a quote, not the closing one");
    check("\"\\x41B\"", "\\x41B", 0, 0, "dq: no \\xHH -- it stays literal");
    check("'a\\b'", "a\\b", 1, 0, "sq: raw, the backslash survives");
    check("'a\\nb'", "a\\nb", 1, 0, "sq: no escapes at all, literal stays 1");
    check("\"a\\db\"", "a\\db", 0, 0, "dq: unknown escape keeps its backslash");
    /* An unterminated region is not one: the whole text stays the input. */
    check("\"abc", "\"abc", 0, 0, "unterminated quote -> whole text verbatim");
    /* ANSI-C is where escapes belong. */
    check("$'a\\nb'", "a\nb", 1, 0, "$'…': \\n is a newline");
    check("$'a\\tb'", "a\tb", 1, 0, "$'…': \\t is a tab");
    check("$'\\x41B'", "AB", 1, 0, "$'…': \\xHH");
    check("$'\\u00e9'", "\xc3\xa9", 1, 0, "$'…': \\uHHHH becomes UTF-8");
    check("$'a\\qb'", "a\\qb", 1, 0, "$'…': unknown escape keeps its backslash");
    check("'''a\\nb'''", "a\nb", 1, 0, "'''…''': the in-tree alias of $'…'");
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
    test_escapes();
    test_desc_needs_quotes();
    test_expand_vars_out_error();
    test_expand_task_refs();

    printf("test_cmd_parse: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
