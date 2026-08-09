#include "edc_string_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#define T_PASS()  do { passed++; } while (0)
#define T_FAIL(msg) do { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg); failed++; } while (0)
#define T_CHECK(cond, msg) do { if (cond) T_PASS(); else T_FAIL(msg); } while (0)
#define T_STREQ(a, b, msg) do { if (a && b && strcmp(a, b) == 0) T_PASS(); else T_FAIL(msg); } while (0)
#define T_STREQ_OR_NULL(a, b, msg) do { \
    if ((a == NULL && b == NULL) || (a && b && strcmp(a, b) == 0)) T_PASS(); else T_FAIL(msg); \
} while (0)

static int passed = 0;
static int failed = 0;

/* ---- edc_strdup_printf ---- */
static void test_strdup_printf(void)
{
    char *s;

    s = edc_strdup_printf("hello");
    T_STREQ(s, "hello", "strdup_printf literal");
    free(s);

    s = edc_strdup_printf("int=%d str=%s", 42, "world");
    T_STREQ(s, "int=42 str=world", "strdup_printf formatted");
    free(s);

    s = edc_strdup_printf("%s%s%s", "a", "b", "c");
    T_STREQ(s, "abc", "strdup_printf concat");
    free(s);

    s = edc_strdup_printf("");
    T_STREQ(s, "", "strdup_printf empty");
    free(s);

    s = edc_strdup_printf("zero: %d", 0);
    T_STREQ(s, "zero: 0", "strdup_printf zero");
    free(s);

    s = edc_strdup_printf("negative: %d", -123);
    T_STREQ(s, "negative: -123", "strdup_printf negative");
    free(s);

    s = edc_strdup_printf("unsigned: %u", 9999999U);
    T_STREQ(s, "unsigned: 9999999", "strdup_printf unsigned");
    free(s);

    s = edc_strdup_printf("large int64: %" PRId64, (int64_t)9223372036854775807LL);
    T_STREQ(s, "large int64: 9223372036854775807", "strdup_printf int64");
    free(s);

    s = edc_strdup_printf("ptr: %p", (void *)0x1234);
    T_CHECK(s != NULL, "strdup_printf pointer not null");
    free(s);
}

/* ---- edc_strcmp0 ---- */
static void test_strcmp0(void)
{
    T_CHECK(edc_strcmp0(NULL, NULL) == 0, "strcmp0 NULL==NULL");
    T_CHECK(edc_strcmp0(NULL, "a") < 0, "strcmp0 NULL < a");
    T_CHECK(edc_strcmp0("a", NULL) > 0, "strcmp0 a > NULL");
    T_CHECK(edc_strcmp0("abc", "abc") == 0, "strcmp0 equal");
    T_CHECK(edc_strcmp0("abc", "abd") < 0, "strcmp0 less");
    T_CHECK(edc_strcmp0("abd", "abc") > 0, "strcmp0 greater");
    T_CHECK(edc_strcmp0("", "") == 0, "strcmp0 empty-equal");
    T_CHECK(edc_strcmp0("", "a") < 0, "strcmp0 empty < a");
    T_CHECK(edc_strcmp0("a", "") > 0, "strcmp0 a > empty");
    T_CHECK(edc_strcmp0("abc", "ab") > 0, "strcmp0 longer");
    T_CHECK(edc_strcmp0("ab", "abc") < 0, "strcmp0 shorter");
}

/* ---- edc_str_equal ---- */
static void test_str_equal(void)
{
    T_CHECK(edc_str_equal("abc", "abc") != 0, "str_equal same");
    T_CHECK(edc_str_equal("abc", "abd") == 0, "str_equal different");
    T_CHECK(edc_str_equal("", "") != 0, "str_equal empty same");
    T_CHECK(edc_str_equal("a", "") == 0, "str_equal a vs empty");

    /* NULL safety: NULL == NULL should be true */
    T_CHECK(edc_str_equal(NULL, NULL) != 0, "str_equal NULL==NULL");
    T_CHECK(edc_str_equal(NULL, "a") == 0, "str_equal NULL vs a");
    T_CHECK(edc_str_equal("a", NULL) == 0, "str_equal a vs NULL");
}

/* ---- edc_str_hash ---- */
static void test_str_hash(void)
{
    unsigned int h1, h2;

    h1 = edc_str_hash("hello");
    h2 = edc_str_hash("hello");
    T_CHECK(h1 == h2, "str_hash deterministic");

    h2 = edc_str_hash("hellp");
    T_CHECK(h1 != h2, "str_hash different strings differ");

    h1 = edc_str_hash("");
    h2 = edc_str_hash("");
    T_CHECK(h1 == h2, "str_hash empty string deterministic");

    T_CHECK(edc_str_hash("a") != edc_str_hash("b"), "str_hash a!=b");
}

/* ---- edc_direct_hash ---- */
static void test_direct_hash(void)
{
    int x = 42;
    T_CHECK(edc_direct_hash(&x) == edc_direct_hash(&x), "direct_hash same ptr");
}

/* ---- edc_direct_equal ---- */
static void test_direct_equal(void)
{
    int x, y;
    T_CHECK(edc_direct_equal(&x, &x) != 0, "direct_equal same ptr");
    T_CHECK(edc_direct_equal(&x, &y) == 0, "direct_equal diff ptr");
}

/* ---- edc_str_has_prefix ---- */
static void test_str_has_prefix(void)
{
    T_CHECK(edc_str_has_prefix("hello world", "hello"), "prefix normal");
    T_CHECK(!edc_str_has_prefix("hello world", "world"), "prefix not at start");
    T_CHECK(edc_str_has_prefix("abc", ""), "prefix empty string");
    T_CHECK(edc_str_has_prefix("abc", "abc"), "prefix exact match");
    T_CHECK(!edc_str_has_prefix("ab", "abc"), "prefix longer than string");
    T_CHECK(!edc_str_has_prefix(NULL, "a"), "prefix NULL str");
    T_CHECK(!edc_str_has_prefix("a", NULL), "prefix NULL prefix");
    T_CHECK(!edc_str_has_prefix(NULL, NULL), "prefix both NULL");
    T_CHECK(edc_str_has_prefix("", ""), "prefix both empty");
    T_CHECK(edc_str_has_prefix("[思考]: something", "[思考]:"), "prefix CJK brackets");
}

/* ---- edc_strstrip ---- */
static void test_strstrip(void)
{
    char buf[256];

    strcpy(buf, "  hello  ");
    T_STREQ(edc_strstrip(buf), "hello", "strstrip both sides");

    strcpy(buf, "hello  ");
    T_STREQ(edc_strstrip(buf), "hello", "strstrip trailing");

    strcpy(buf, "  hello");
    T_STREQ(edc_strstrip(buf), "hello", "strstrip leading");

    strcpy(buf, "hello");
    T_STREQ(edc_strstrip(buf), "hello", "strstrip none");

    strcpy(buf, "   ");
    T_STREQ(edc_strstrip(buf), "", "strstrip all spaces");

    strcpy(buf, "");
    T_STREQ(edc_strstrip(buf), "", "strstrip empty");

    T_CHECK(edc_strstrip(NULL) == NULL, "strstrip NULL");

    strcpy(buf, "\t\n hello \t\n");
    T_STREQ(edc_strstrip(buf), "hello", "strstrip tabs and newlines");

    /* In-place test: verify returned pointer is within original buffer */
    strcpy(buf, "  a  ");
    char *p = edc_strstrip(buf);
    T_CHECK(p >= buf && p <= buf + strlen("  a  "), "strstrip returns pointer in original buffer");
}

/* ---- edc_strv_length ---- */
static void test_strv_length(void)
{
    char *v1[] = {"a", "b", "c", NULL};
    T_CHECK(edc_strv_length(v1) == 3, "strv_length 3");

    char *v2[] = {NULL};
    T_CHECK(edc_strv_length(v2) == 0, "strv_length null-only");

    T_CHECK(edc_strv_length(NULL) == 0, "strv_length NULL array");
}

/* ---- edc_strsplit ---- */
static void test_strsplit(void)
{
    char **r;

    r = edc_strsplit("a,b,c", ",", -1);
    T_CHECK(r != NULL, "strsplit basic");
    T_STREQ(r[0], "a", "strsplit [0]");
    T_STREQ(r[1], "b", "strsplit [1]");
    T_STREQ(r[2], "c", "strsplit [2]");
    T_CHECK(r[3] == NULL, "strsplit NULL terminator");
    T_CHECK(edc_strv_length(r) == 3, "strsplit count 3");
    edc_strfreev(r);

    r = edc_strsplit("a,,c", ",", -1);
    T_CHECK(r != NULL, "strsplit empty middle");
    T_STREQ(r[0], "a", "strsplit empty-middle [0]");
    T_STREQ(r[1], "", "strsplit empty-middle [1]");
    T_STREQ(r[2], "c", "strsplit empty-middle [2]");
    T_CHECK(r[3] == NULL, "strsplit empty-middle term");
    edc_strfreev(r);

    r = edc_strsplit(",a,c", ",", -1);
    T_CHECK(r != NULL, "strsplit leading delimiter");
    T_STREQ(r[0], "", "strsplit leading [0]");
    T_STREQ(r[1], "a", "strsplit leading [1]");
    T_STREQ(r[2], "c", "strsplit leading [2]");
    edc_strfreev(r);

    r = edc_strsplit("a,c,", ",", -1);
    T_CHECK(r != NULL, "strsplit trailing delimiter");
    T_STREQ(r[0], "a", "strsplit trailing [0]");
    T_STREQ(r[1], "c", "strsplit trailing [1]");
    T_STREQ(r[2], "", "strsplit trailing [2]");
    edc_strfreev(r);

    r = edc_strsplit("no-delimiter", ",", -1);
    T_CHECK(r != NULL, "strsplit no delimiter");
    T_STREQ(r[0], "no-delimiter", "strsplit no-delim [0]");
    T_CHECK(r[1] == NULL, "strsplit no-delim NULL terminator");
    edc_strfreev(r);

    r = edc_strsplit("a:b:c", ":", 2);
    T_CHECK(r != NULL, "strsplit max_tokens");
    T_STREQ(r[0], "a", "strsplit max [0]");
    T_STREQ(r[1], "b:c", "strsplit max [1]");
    T_CHECK(r[2] == NULL, "strsplit max term");
    edc_strfreev(r);

    r = edc_strsplit("", ",", -1);
    T_CHECK(r != NULL, "strsplit empty input");
    T_STREQ(r[0], "", "strsplit empty [0]");
    T_CHECK(r[1] == NULL, "strsplit empty term");
    edc_strfreev(r);

    r = edc_strsplit("k1:v1\nk2:v2", "\n", -1);
    T_CHECK(r != NULL, "strsplit multiline");
    T_STREQ(r[0], "k1:v1", "strsplit multi [0]");
    T_STREQ(r[1], "k2:v2", "strsplit multi [1]");
    T_CHECK(r[2] == NULL, "strsplit multi term");
    edc_strfreev(r);

    r = edc_strsplit("name: value", ": ", 2);
    T_CHECK(r != NULL, "strsplit multi-char delim");
    T_STREQ(r[0], "name", "strsplit multi-delim [0]");
    T_STREQ(r[1], "value", "strsplit multi-delim [1]");
    edc_strfreev(r);

    r = edc_strsplit(NULL, ",", -1);
    T_CHECK(r == NULL, "strsplit NULL string");

    r = edc_strsplit("a,b", NULL, -1);
    T_CHECK(r == NULL, "strsplit NULL delimiter");
}

/* ---- edc_strfreev ---- */
static void test_strfreev(void)
{
    /* Build a strv and free it; shouldn't crash */
    char **v = malloc(3 * sizeof(char *));
    v[0] = strdup("a");
    v[1] = strdup("b");
    v[2] = NULL;
    edc_strfreev(v);
    T_PASS();

    /* NULL input shouldn't crash */
    edc_strfreev(NULL);
    T_PASS();
}

/* ---- edge cases: interop ---- */
static void test_edge_cases(void)
{
    /* str_has_prefix + strstrip together */
    char buf[64];
    strcpy(buf, "  test: value");
    char *s = edc_strstrip(buf);
    T_CHECK(edc_str_has_prefix(s, "test:"), "strip + prefix interop");

    /* strdup_printf with NULL is already tested; strdup is not needed */
}

int main(void)
{
    test_strdup_printf();
    test_strcmp0();
    test_str_equal();
    test_str_hash();
    test_direct_hash();
    test_direct_equal();
    test_str_has_prefix();
    test_strstrip();
    test_strv_length();
    test_strsplit();
    test_strfreev();
    test_edge_cases();

    printf("passed=%d failed=%d\n", passed, failed);
    return failed > 0 ? 1 : 0;
}
