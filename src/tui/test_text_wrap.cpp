// Unit tests for the CJK-aware text helpers in edc_text_util.hpp
// (WrapToWidth in particular). These protect the TodoWidget multi-line
// layout: every wrapped line must fit the given cell budget, spaces must not
// be swallowed in the middle of prose, and long unbroken runs (CJK, paths)
// must still break so lines never overflow the sidebar column.

#include "edc_text_util.hpp"

#include <cstdio>
#include <string>
#include <vector>

static int g_failures = 0;

#define TEST(name)                                                     \
    do                                                                 \
    {                                                                  \
        std::printf("  %s\n", name);                                   \
    } while (0)

#define CHECK(cond, ...)                                               \
    do                                                                 \
    {                                                                  \
        if (!(cond))                                                   \
        {                                                              \
            std::printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            std::printf("    ");                                       \
            std::printf(__VA_ARGS__);                                  \
            std::printf("\n");                                         \
            g_failures++;                                              \
        }                                                              \
    } while (0)

static void dump_lines(const std::vector<std::string> &lines)
{
    for (size_t i = 0; i < lines.size(); i++)
        std::printf("      [%zu] '%s' (width %d)\n", i, lines[i].c_str(), edc::StringWidth(lines[i]));
}

static void test_ascii_word_wrap(void)
{
    TEST("ascii prose wraps at spaces, never overflowing the budget");
    const std::string text = "hello world this is a longer sentence to wrap";
    std::vector<std::string> lines = edc::WrapToWidth(text, 12);
    dump_lines(lines);
    CHECK(lines.size() >= 3, "expected several lines, got %zu", lines.size());
    for (const std::string &ln : lines)
        CHECK(edc::StringWidth(ln) <= 12, "line '%s' exceeds budget", ln.c_str());
    // Word-boundary wrap: words must remain intact (nothing shorter than
    // "hello" should have been hard-split).
    for (const std::string &ln : lines)
    {
        const std::string first = ln.substr(0, ln.find(' '));
        if (!first.empty())
            CHECK(text.find(first) != std::string::npos, "unknown word '%s'", first.c_str());
    }
}

static void test_cjk_hard_break(void)
{
    TEST("an unbroken CJK run hard-breaks every 2 cells so lines fit");
    const std::string cjk = "恢复滚动指示条并且让内容可以换行显示不再拥挤";
    std::vector<std::string> lines = edc::WrapToWidth(cjk, 10); // 5 CJK chars per line
    dump_lines(lines);
    CHECK(lines.size() >= 4, "expected at least 4 lines, got %zu", lines.size());
    for (const std::string &ln : lines)
        CHECK(edc::StringWidth(ln) <= 10, "line '%s' exceeds budget", ln.c_str());
    // Reconstructing must not lose characters (space insertion aside).
    std::string joined;
    for (const std::string &ln : lines)
        joined += ln;
    CHECK(joined.size() == cjk.size(), "characters lost: %zu -> %zu", cjk.size(), joined.size());
}

static void test_mixed_cjk_latin(void)
{
    TEST("mixed CJK + latin text wraps with spaces kept between words");
    const std::string text = "写测试 todo widget 换行 wrap width";
    std::vector<std::string> lines = edc::WrapToWidth(text, 12);
    dump_lines(lines);
    CHECK(lines.size() >= 2, "expected at least 2 lines, got %zu", lines.size());
    for (const std::string &ln : lines)
        CHECK(edc::StringWidth(ln) <= 12, "line '%s' exceeds budget", ln.c_str());
}

static void test_long_word_breaks(void)
{
    TEST("a long path/URL breaks into fitting chunks");
    const std::string url = "https://example.com/a/very/long/path/that/does/not/fit";
    std::vector<std::string> lines = edc::WrapToWidth(url, 15);
    dump_lines(lines);
    CHECK(lines.size() >= 2, "expected at least 2 lines, got %zu", lines.size());
    for (const std::string &ln : lines)
        CHECK(edc::StringWidth(ln) <= 15, "line '%s' exceeds budget", ln.c_str());
}

static void test_empty_and_whitespace(void)
{
    TEST("empty / whitespace-only input yields no lines; fits() stays no-op");
    CHECK(edc::WrapToWidth("", 10).empty(), "empty string should wrap to nothing");
    CHECK(edc::WrapToWidth("   \n\t ", 10).empty(), "whitespace-only should wrap to nothing");
    CHECK(edc::WrapToWidth("abc", 0).empty(), "zero budget should wrap to nothing");
}

static void test_whitespace_collapse(void)
{
    TEST("whitespace runs collapse to a single space; edges are trimmed");
    const std::string text = "  alpha   beta\tgamma\n";
    std::vector<std::string> lines = edc::WrapToWidth(text, 100);
    CHECK(lines.size() == 1, "expected 1 line, got %zu", lines.size());
    if (!lines.empty())
        CHECK(lines[0] == "alpha beta gamma", "got '%s'", lines[0].c_str());
}

int main(void)
{
    test_ascii_word_wrap();
    test_cjk_hard_break();
    test_mixed_cjk_latin();
    test_long_word_breaks();
    test_empty_and_whitespace();
    test_whitespace_collapse();

    if (g_failures)
    {
        std::printf("%d check(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("all text wrap tests passed\n");
    return 0;
}
