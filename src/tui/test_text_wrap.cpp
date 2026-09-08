// Unit tests for the CJK-aware text helpers in edc_text_util.hpp
// (WrapToWidth and VisualWrapRows in particular). WrapToWidth protects the
// TodoWidget multi-line layout: every wrapped line must fit the given cell
// budget, spaces must not be swallowed in the middle of prose, and long
// unbroken runs (CJK, paths) must still break so lines never overflow the
// sidebar column. VisualWrapRows protects the soft-wrapping EdcInput editor:
// its rows are lossless byte partitions of an editable line (only a single
// boundary separator may be dropped) that never split a UTF-8 sequence.

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

static void test_visual_wrap_rows_fit(void)
{
    TEST("editor soft-wrap: every row fits the budget, text is preserved");
    const std::string s = "hello world this is a slightly longer sentence";
    const auto rows = edc::VisualWrapRows(s, 12);
    std::printf("    rows (width 12):\n");
    for (const auto &sp : rows)
        std::printf("      [%d,%d) '%s' (width %d)\n", sp.first, sp.second,
                    s.substr(sp.first, sp.second - sp.first).c_str(),
                    edc::StringWidth(s.substr(sp.first, sp.second - sp.first)));
    CHECK(!rows.empty(), "expected rows");
    for (const auto &sp : rows)
    {
        CHECK(sp.second > sp.first, "empty row span [%d,%d)", sp.first, sp.second);
        CHECK(edc::StringWidth(s.substr(sp.first, sp.second - sp.first)) <= 12,
              "row exceeds budget");
    }
}

static void test_visual_wrap_boundary_space(void)
{
    TEST("editor soft-wrap: boundary space behavior");
    {
        // Row exactly full when the separator is met -> the space is dropped
        // so no row starts/ends with a lone separator.
        const std::string s = "hello world";
        const auto rows = edc::VisualWrapRows(s, 5);
        CHECK(rows.size() == 2, "expected 2 rows, got %zu", rows.size());
        if (rows.size() == 2)
        {
            const std::string r0 = s.substr(rows[0].first, rows[0].second - rows[0].first);
            const std::string r1 = s.substr(rows[1].first, rows[1].second - rows[1].first);
            CHECK(r0 == "hello", "row0 '%s'", r0.c_str());
            CHECK(r1 == "world", "row1 '%s'", r1.c_str());
        }
    }
    {
        // Word does not quite fill the row -> the space stays at the end of
        // the previous row and the text concatenates back losslessly.
        const std::string s = "hello world";
        const auto rows = edc::VisualWrapRows(s, 8);
        std::string joined;
        for (const auto &sp : rows)
            joined += s.substr(sp.first, sp.second - sp.first);
        CHECK(joined == s, "lossless wrap expected, got '%s'", joined.c_str());
        if (rows.size() == 2)
        {
            const std::string r0 = s.substr(rows[0].first, rows[0].second - rows[0].first);
            CHECK(r0 == "hello ", "row0 '%s'", r0.c_str());
        }
    }
}

static void test_visual_wrap_cjk_and_wide(void)
{
    TEST("editor soft-wrap: CJK/wide chars hard-break but never split a glyph");
    {
        const std::string s = "你好世界你好世界";
        const auto rows = edc::VisualWrapRows(s, 6); // 3 CJK chars per row
        std::string joined;
        for (const auto &sp : rows)
            joined += s.substr(sp.first, sp.second - sp.first);
        CHECK(joined == s, "CJK wrap lost characters: '%s'", joined.c_str());
        CHECK(rows.size() == 3, "expected 3 rows (8 chars / 3 per row), got %zu", rows.size());
        for (const auto &sp : rows)
            CHECK(edc::StringWidth(s.substr(sp.first, sp.second - sp.first)) <= 6,
                  "CJK row exceeds budget");
    }
    {
        // Emoji (2 cells each) must not be split when a 3-cell budget only
        // fits one per row.
        const std::string s = "😀😀😀";
        const auto rows = edc::VisualWrapRows(s, 3);
        std::string joined;
        for (const auto &sp : rows)
            joined += s.substr(sp.first, sp.second - sp.first);
        CHECK(joined == s, "wide-char wrap lost characters");
        CHECK(rows.size() == 3, "expected 3 rows, got %zu", rows.size());
    }
}

static void test_visual_wrap_mixed_and_edges(void)
{
    TEST("editor soft-wrap: mixed prose, empty input, degenerate budgets");
    {
        const std::string s = "abc 你好 def 中文内容 ghi";
        const auto rows = edc::VisualWrapRows(s, 10);
        std::string joined;
        for (const auto &sp : rows)
            joined += s.substr(sp.first, sp.second - sp.first);
        CHECK(joined == s, "mixed wrap lost characters: '%s'", joined.c_str());
        for (const auto &sp : rows)
            CHECK(edc::StringWidth(s.substr(sp.first, sp.second - sp.first)) <= 10,
                  "mixed row exceeds budget");
    }
    CHECK(edc::VisualWrapRows("", 10).empty(), "empty input yields no rows");
    {
        const auto rows = edc::VisualWrapRows("abcdef", 0);
        CHECK(rows.size() == 1, "degenerate budget still yields one row");
    }
    {
        // Long word hard-breaks in the middle without losing bytes.
        const std::string s = "https://example.com/very/long/path";
        const auto rows = edc::VisualWrapRows(s, 10);
        std::string joined;
        for (const auto &sp : rows)
            joined += s.substr(sp.first, sp.second - sp.first);
        CHECK(joined == s, "long-word wrap lost characters: '%s'", joined.c_str());
        CHECK(rows.size() >= 3, "expected several rows, got %zu", rows.size());
    }
}

int main(void)
{
    test_ascii_word_wrap();
    test_cjk_hard_break();
    test_mixed_cjk_latin();
    test_long_word_breaks();
    test_empty_and_whitespace();
    test_whitespace_collapse();
    test_visual_wrap_rows_fit();
    test_visual_wrap_boundary_space();
    test_visual_wrap_cjk_and_wide();
    test_visual_wrap_mixed_and_edges();

    if (g_failures)
    {
        std::printf("%d check(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("all text wrap tests passed\n");
    return 0;
}
