/* Unit tests for the input-history line format: `<mid>\t<text>`.
 *
 * Two things must not break: the text is arbitrary user input (newlines, tabs, backslashes)
 * and the file predates the mid field, so old lines have to keep loading. */

#include "edc_history_line.hpp"

#include <cstdio>
#include <string>

static int g_failures = 0;

#define CHECK(cond, ...)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            std::printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                            \
            std::printf("    ");                                                                                       \
            std::printf(__VA_ARGS__);                                                                                  \
            std::printf("\n");                                                                                         \
            g_failures++;                                                                                              \
        }                                                                                                              \
    } while (0)

static void test_round_trip(void)
{
    std::printf("  history line: mid + text survive the trip\n");
    const std::string mid = "s3#12#1760000000000";
    const std::string text = "line one\nline two\twith a tab \\ and a backslash";
    const std::string line = edc::EncodeHistoryLine(mid, text);

    CHECK(line.find('\n') == std::string::npos, "an entry is always ONE line on disk");
    CHECK(line.compare(0, mid.size(), mid) == 0, "the mid is the first field");

    const edc::HistoryLine back = edc::DecodeHistoryLine(line);
    CHECK(back.mid == mid, "the mid comes back: '%s'", back.mid.c_str());
    CHECK(back.text == text, "the text comes back byte for byte: '%s'", back.text.c_str());
}

static void test_old_format(void)
{
    std::printf("  history line: a line written before mids existed still loads\n");
    const edc::HistoryLine back = edc::DecodeHistoryLine("old entry with a \\n newline");
    CHECK(back.mid.empty(), "no mid");
    CHECK(back.text == "old entry with a \n newline", "the escapes are decoded: '%s'", back.text.c_str());

    /* And writing one without a mid keeps the old shape. */
    CHECK(edc::EncodeHistoryLine("", "plain") == "plain", "no mid -> no separator");
}

static void test_tab_in_text_is_not_a_mid(void)
{
    std::printf("  history line: a pasted line that starts with a tab is not a mid\n");
    /* A TSV line the user pasted: "a\tb". The first field is "a", which is not a mid, so the
     * whole line is text. */
    const edc::HistoryLine back = edc::DecodeHistoryLine("a\tb");
    CHECK(back.mid.empty(), "no mid is invented");
    CHECK(back.text == "a\tb", "the tab stays part of the text: '%s'", back.text.c_str());

    CHECK(!edc::LooksLikeMid("a"), "a single letter is not a mid");
    CHECK(!edc::LooksLikeMid(""), "nor is an empty string");
    CHECK(!edc::LooksLikeMid("has space#1"), "nor is one with a space");
    CHECK(edc::LooksLikeMid("s3#12#1760000000000"), "the real shape is");
}

int main(void)
{
    test_round_trip();
    test_old_format();
    test_tab_in_text_is_not_a_mid();

    if (g_failures)
    {
        std::printf("%d check(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("all history-line tests passed\n");
    return 0;
}
