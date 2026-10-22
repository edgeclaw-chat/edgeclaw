/* Unit tests for the display side of the input syntax: which substrings a bubble marks,
 * and that it marks them the same way the input line does.
 *
 * The rule belongs to the mention pass (edc_pass_mentions.c) and to the `@` completion;
 * this is the copy that decides what the reader SEES, so it is pinned here case by case.
 * If it ever disagrees with the pass, a coloured `@path` would name a file that was
 * never attached -- that is the failure these cases exist to prevent. */

#include "edc_message.hpp"
#include "edc_style.hpp"
#include "edc_text_util.hpp"
#include "edc_token_style.hpp"

#include <ftxui/dom/node.hpp> // Render
#include <ftxui/screen/screen.hpp>
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

static std::string TokensOf(const std::string &text)
{
    std::string out;
    for (const edc::TokenSpan &span : edc::ScanTokens(text))
        out += (out.empty() ? "" : "|") + std::string(span.paste ? "P:" : "M:") + text.substr(span.begin, span.len);
    return out;
}

static void test_mentions(void)
{
    std::printf("  scan: @ references follow the pass's rule\n");
    CHECK(TokensOf("@a.c 看看") == "M:@a.c", "at the line start: '%s'", TokensOf("@a.c 看看").c_str());
    CHECK(TokensOf("看下 @a.c 有什么") == "M:@a.c", "after whitespace: '%s'", TokensOf("看下 @a.c 有什么").c_str());
    CHECK(TokensOf("first\n@a.c") == "M:@a.c", "after a newline: '%s'", TokensOf("first\n@a.c").c_str());

    CHECK(TokensOf("写信给 foo@a.c 这个人") == "", "inside a word it is not a reference");
    CHECK(TokensOf("\\@a.c 没展开") == "", "a backslash escapes it (the pass leaves it literal)");

    /* Trailing punctuation is not part of the reference: the pass reads `@a.c` there. */
    CHECK(TokensOf("看下 @a.c. 谢谢") == "M:@a.c", "the full stop stays outside: '%s'",
          TokensOf("看下 @a.c. 谢谢").c_str());
    CHECK(TokensOf("看下 @a.c) 谢谢") == "M:@a.c", "nor is a closing paren: '%s'",
          TokensOf("看下 @a.c) 谢谢").c_str());
    /* The pass wants the `@` at the line start or after whitespace; a bracket is neither. */
    CHECK(TokensOf("[@a.c]") == "", "a bracket in front disqualifies it");

    CHECK(TokensOf("@a.c#12-20") == "M:@a.c#12-20", "a line range belongs to the reference");

    /* Quoting: the paint must agree with the pass, which is the point of sharing
     * edc_mention_parse() instead of keeping two scanners. */
    CHECK(TokensOf("@\"my file.txt\" 看下") == "M:@\"my file.txt\"",
          "a quoted path with a space is ONE reference: '%s'", TokensOf("@\"my file.txt\" 看下").c_str());
    CHECK(TokensOf("@'my file.txt'") == "M:@'my file.txt'", "single quotes too: '%s'",
          TokensOf("@'my file.txt'").c_str());
    /* Inside quotes the punctuation is the user's, so it is painted; outside it is not. */
    CHECK(TokensOf("@\"a.txt.\"") == "M:@\"a.txt.\"", "quoted: the full stop is inside the reference");
    /* The range suffix may follow the closing quote. */
    CHECK(TokensOf("@\"my file.txt\"#2-3") == "M:@\"my file.txt\"#2-3", "quoted + range: '%s'",
          TokensOf("@\"my file.txt\"#2-3").c_str());
    /* Unterminated: not a reference, so nothing is painted and the text stays ordinary. */
    CHECK(TokensOf("@\"my file.txt") == "", "an unterminated quote is not a reference");
    CHECK(TokensOf("@ 空空") == "", "a bare '@' is not one");
    CHECK(TokensOf("@a.c 和 @b.c") == "M:@a.c|M:@b.c", "two on one line: '%s'", TokensOf("@a.c 和 @b.c").c_str());
}

static void test_paste_labels(void)
{
    std::printf("  scan: folded paste labels\n");
    CHECK(TokensOf("[Pasted #1 \xc2\xb7 42 lines] 这段") == "P:[Pasted #1 \xc2\xb7 42 lines]",
          "the label the editor writes: '%s'", TokensOf("[Pasted #1 \xc2\xb7 42 lines] 这段").c_str());
    CHECK(TokensOf("[Pasted #12 \xc2\xb7 1 line]") == "P:[Pasted #12 \xc2\xb7 1 line]", "singular");
    CHECK(TokensOf("[Pasted #1 42 lines]") == "", "not the shape the editor writes");
    CHECK(TokensOf("[Pasted #1 \xc2\xb7 42 lines") == "", "unterminated is plain text");
    CHECK(TokensOf("x[Pasted #1 \xc2\xb7 2 lines]") == "P:[Pasted #1 \xc2\xb7 2 lines]",
          "a label anywhere in the line still marks the block");
}

namespace
{
struct FixedPalette : edc::style::IPalette
{
    edc::style::Palette pal;
    const edc::style::Palette &colors() const override
    {
        return pal;
    }
};
} // namespace

static void test_bubble_matches_the_input_line(void)
{
    std::printf("  bubble: the same two marks the input line paints\n");

    /* The bubble takes its colours from the same knobs as the editor (StyleToken), so the
     * only thing left to verify is that it paints them at all. A mention only wears the mark
     * when its path is real, so the harness says "this one exists". */
    edc::SetMentionPathCheck([](const std::string &path) { return path == "src/main.c"; });
    const ftxui::Color mention = edc::style::input_mention_text();
    const ftxui::Color paste_fg = edc::style::input_paste_text();
    const ftxui::Color paste_bg = edc::style::input_paste_surface();

    edc::UserMessage msg("看下 @src/main.c 和 [Pasted #1 \xc2\xb7 42 lines] 这段", 0, "");
    msg.SetRenderWidth(80);

    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(80), ftxui::Dimension::Fixed(4));
    {
        auto doc = msg.Render();
        Render(screen, doc);
    }
    screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(80), ftxui::Dimension::Fixed(4));
    {
        auto doc = msg.Render();
        Render(screen, doc);
    }

    /* The bubble paints its whole surface with pane_bg, so "no surface of its own" means
     * "the same background the surrounding words have". */
    const ftxui::Color plain_bg = screen.CellAt(0, 1).background_color;
    bool saw_at = false, saw_paste = false;
    for (int y = 0; y < 4; ++y)
    {
        for (int x = 0; x < 80; ++x)
        {
            const auto cell = screen.CellAt(x, y);
            if (cell.character == "@")
            {
                saw_at = true;
                CHECK(cell.foreground_color == mention, "the mention keeps its keyword colour in the bubble");
                CHECK(cell.background_color == plain_bg, "and no surface of its own");
            }
            if (cell.character == "[" && x + 1 < 80 && screen.CellAt(x + 1, y).character == "P")
            {
                saw_paste = true;
                CHECK(cell.foreground_color == paste_fg, "the paste block keeps its colour");
                CHECK(cell.background_color == paste_bg, "and its own surface");
            }
        }
    }
    CHECK(saw_at, "the bubble actually drew the mention");
    CHECK(saw_paste, "…and the paste block");
    edc::SetMentionPathCheck(nullptr);
}

static void test_bubble_mention_rules(void)
{
    std::printf("  bubble: no caret, so a mention ending the line is finished\n");

    edc::SetMentionPathCheck([](const std::string &path) { return path == "src/main.c"; });
    const ftxui::Color mention = edc::style::input_mention_text();

    auto mention_is_painted = [&](const std::string &content) {
        edc::UserMessage msg(content, 0, "");
        msg.SetRenderWidth(80);
        ftxui::Screen screen(ftxui::Screen::Create(ftxui::Dimension::Fixed(80), ftxui::Dimension::Fixed(3)));
        for (int pass = 0; pass < 2; ++pass)
        {
            screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(80), ftxui::Dimension::Fixed(3));
            auto doc = msg.Render();
            Render(screen, doc);
        }
        for (int y = 0; y < 3; ++y)
            for (int x = 0; x < 80; ++x)
                if (screen.CellAt(x, y).character == "@")
                    return screen.CellAt(x, y).foreground_color == mention;
        return false;
    };

    CHECK(mention_is_painted("看下 @src/main.c"), "a sent line ending in a real reference IS painted");
    /* A closing quote ends the reference by itself, so it needs no trailing whitespace --
     * that is what edc_mention_parse()'s self_closed reports. */
    CHECK(mention_is_painted("看下 @\"src/main.c\""), "a quoted reference is closed by its own quote");
    CHECK(!mention_is_painted("看下 @ghost.c"), "a path that does not exist is NOT painted");
    edc::SetMentionPathCheck(nullptr);
}

static void test_fold_back(void)
{
    std::printf("  fold: an expanded message goes back to the placeholders it was typed as\n");
    const std::string payload =
        R"({"[Pasted #1 · 2 lines]":"line-1\nline-2","[Pasted #2 · 1 line]":"tail"})";
    const std::string expanded = "看下 line-1\nline-2 和 tail 这段";

    const std::string folded = edc::FoldPastedText(expanded, payload);
    CHECK(folded == "看下 [Pasted #1 · 2 lines] 和 [Pasted #2 · 1 line] 这段", "folded: '%s'", folded.c_str());

    /* No payload (or a broken one): the text is the text. */
    CHECK(edc::FoldPastedText(expanded, "") == expanded, "no payload -> unchanged");
    CHECK(edc::FoldPastedText(expanded, "not json") == expanded, "bad JSON -> unchanged");

    /* The same body pasted twice folds to its own two labels, in order. */
    const std::string twice = "a X b X c";
    const std::string two = R"({"[Pasted #1 · 1 line]":"X","[Pasted #2 · 1 line]":"X"})";
    CHECK(edc::FoldPastedText(twice, two) == "a [Pasted #1 · 1 line] b [Pasted #2 · 1 line] c",
          "each occurrence takes the next label: '%s'", edc::FoldPastedText(twice, two).c_str());
}

int main(void)
{
    test_mentions();
    test_paste_labels();
    test_bubble_matches_the_input_line();
    test_bubble_mention_rules();
    test_fold_back();

    if (g_failures)
    {
        std::printf("%d check(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("all token-style tests passed\n");
    return 0;
}
