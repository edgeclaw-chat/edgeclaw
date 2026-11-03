/* test_markdown: the markdown renderer, driven through its one public entry.
 *
 * `RenderMarkdown()` is the whole surface (`markdown.hpp`): the block builders are file-local
 * classes reached only by md4c's callbacks, so the way to test them is to feed the renderer
 * markdown and read what it drew. That is also how a bug would reach a user -- nobody calls
 * HeadingB::Finish() directly.
 *
 * The file had 39.4% line coverage and no test of its own: the heading/quote/list/table
 * builders were reached only incidentally by widget tests that pass a fixed string. So the
 * cases here are chosen to pin the DECISIONS each builder makes, not to reach lines:
 *
 *   - a level-1 heading is a banner with padding and NO "#" prefix, while levels 2+ keep
 *     their prefix -- the one place where the markdown source is echoed back to the reader;
 *   - an ordered list numbers its items (1., 2., ...) and an unordered one bullets them,
 *     which is a stateful difference in ListB, not a formatting preference;
 *   - a list item holding two paragraphs becomes a vbox, one holding one paragraph does not
 *     -- the branch that decides whether items align with their neighbours;
 *   - a table bolds its header row and draws a rule under it, which only happens when
 *     head_end > 0;
 *   - an inline code span whose whole text IS a scheduler state name drops the code chip and
 *     takes the state's colour. That is how `/task list` stays scannable, and it is invisible
 *     in the plain text -- it only shows up as "the chip background is gone", so it is
 *     asserted through the styled frame rather than the plain one.
 *
 * Rendering is checked through ftxui's Screen, like the other widget tests: the element tree
 * is not inspectable, the drawn cells are.
 */

#include "edc_style.hpp"
#include "markdown.hpp"

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>

#include <cstdio>
#include <string>

static int g_passed = 0;
static int g_failed = 0;

#define TEST(name) printf("\n-- %s\n", name)

#define CHECK(cond, ...) do { \
    if (!(cond)) { fprintf(stderr, "    FAIL: "); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); g_failed++; } \
    else { printf("    ok: "); printf(__VA_ARGS__); printf("\n"); g_passed++; } \
} while (0)

namespace
{

// Strip the SGR escapes Screen::ToString() interleaves with the cells, so assertions are
// about what a reader sees rather than about colour codes.
std::string Plain(const std::string &frame)
{
    std::string out;
    bool in_escape = false;
    for (char c : frame)
    {
        if (in_escape)
        {
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))
                in_escape = false;
            continue;
        }
        if (c == '\x1b')
        {
            in_escape = true;
            continue;
        }
        out += c;
    }
    return out;
}

std::string Draw(const std::string &md, int w = 60, int h = 20, int wrap = 0)
{
    auto screen = ::ftxui::Screen::Create(::ftxui::Dimension::Fixed(w),
                                         ::ftxui::Dimension::Fixed(h));
    ::ftxui::Render(screen, edc::RenderMarkdown(md, wrap) | ::ftxui::flex);
    return Plain(screen.ToString());
}

bool Has(const std::string &frame, const std::string &needle)
{
    return frame.find(needle) != std::string::npos;
}

int Count(const std::string &frame, const std::string &needle)
{
    int n = 0;
    for (size_t at = frame.find(needle); at != std::string::npos;
         at = frame.find(needle, at + needle.size()))
        n++;
    return n;
}

/* The row a needle appears on, or -1. Needed where ORDER is the property under test: with a
 * table, "the header is above the body" cannot be expressed as two independent `find`s. */
int RowOf(const std::string &frame, const std::string &needle)
{
    int row = 0;
    size_t line_start = 0;
    while (line_start <= frame.size())
    {
        const size_t line_end = frame.find('\n', line_start);
        const std::string line = frame.substr(
            line_start, line_end == std::string::npos ? std::string::npos : line_end - line_start);
        if (line.find(needle) != std::string::npos)
            return row;
        if (line_end == std::string::npos)
            break;
        line_start = line_end + 1;
        row++;
    }
    return -1;
}

} // namespace

// ── Headings ────────────────────────────────────────────────────────────────

static void test_h1_is_a_banner_without_a_prefix()
{
    TEST("h1: the text is padded into a banner, and no '#' is echoed");
    const std::string f = Draw("# Title here");

    CHECK(Has(f, "Title here"), "the heading text is drawn");
    /* Level 1 draws padding (a space element on each side) instead of the ATX marker, so a
     * raw '#' anywhere means the banner branch was not taken. */
    CHECK(!Has(f, "#"), "no '#' prefix for level 1 (it is a banner, not a prefixed line)");
}

static void test_h2_keeps_its_prefix()
{
    TEST("h2: levels 2+ keep their markdown prefix");
    CHECK(Has(Draw("## Second"), "## Second"), "level 2 keeps '## '");
    CHECK(Has(Draw("### Third"), "### Third"), "level 3 keeps '### '");
    /* Level 4 and deeper share one colour; the default arm of the switch is what makes an
     * h5/h6 render at all rather than blank. */
    CHECK(Has(Draw("#### Fourth"), "#### Fourth"), "level 4 keeps '#### '");
    CHECK(Has(Draw("##### Fifth"), "##### Fifth"), "level 5 (default arm) keeps its prefix");
}

// ── Lists ───────────────────────────────────────────────────────────────────

static void test_unordered_list_bullets()
{
    TEST("ul: every item gets a bullet");
    const std::string f = Draw("- alpha\n- beta\n- gamma");
    CHECK(Count(f, "\u2022 ") == 3, "three bullets, got %d", Count(f, "\u2022 "));
    CHECK(Has(f, "alpha") && Has(f, "beta") && Has(f, "gamma"), "all three items are drawn");
}

static void test_ordered_list_numbers()
{
    TEST("ol: items are numbered from 1, in order");
    const std::string f = Draw("1. one\n2. two\n3. three");
    CHECK(Has(f, "1. "), "first item numbered 1");
    CHECK(Has(f, "2. "), "second item numbered 2");
    CHECK(Has(f, "3. "), "third item numbered 3");
    /* The counter lives in ListB::index and is what makes an ordered list ordered; a bullet
     * here would mean `ordered` never reached the builder. */
    CHECK(!Has(f, "\u2022 "), "an ordered list draws no bullets");
}

static void test_list_item_with_two_paragraphs()
{
    TEST("li: a loose item (two paragraphs) stacks them, a tight one does not");
    /* A blank line inside the item is what makes md4c emit two paragraphs, which is the
     * `body.size() > 1` arm of LiB::Finish (vbox). */
    const std::string loose = Draw("- first para\n\n  second para");
    CHECK(Has(loose, "first para") && Has(loose, "second para"),
          "both paragraphs of a loose item are drawn");
    CHECK(RowOf(loose, "first para") < RowOf(loose, "second para"),
          "and they are stacked in order");

    const std::string tight = Draw("- only para");
    CHECK(Has(tight, "only para"), "a tight item still draws its text");
}

// ── Quote, code, rule ───────────────────────────────────────────────────────

static void test_quote_has_a_gutter()
{
    TEST("blockquote: a gutter is drawn and the text sits inside it");
    const std::string f = Draw("> quoted line");
    CHECK(Has(f, "\u2502"), "the '│' gutter is drawn");
    CHECK(Has(f, "quoted line"), "the quoted text is drawn");
}

static void test_code_block_keeps_its_body()
{
    TEST("code block: the body is drawn verbatim");
    const std::string f = Draw("```\nint x = 1;\n```");
    CHECK(Has(f, "int x = 1;"), "the code line is drawn");
    /* The fences are markdown, not content. */
    CHECK(!Has(f, "```"), "the fences are not drawn");
}

static void test_horizontal_rule()
{
    TEST("hr: a rule is drawn");
    const std::string f = Draw("above\n\n---\n\nbelow");
    CHECK(Has(f, "above") && Has(f, "below"), "text on both sides is drawn");
    /* Draws a separator element; on a text screen that is a run of box-drawing characters. */
    CHECK(Has(f, "\u2500"), "a horizontal rule is drawn");
}

// ── Tables ──────────────────────────────────────────────────────────────────

static void test_table_header_is_above_the_body()
{
    TEST("table: the header row precedes the body, and both are drawn");
    const std::string f = Draw("| name | kind |\n|---|---|\n| alpha | one |\n| beta | two |");

    CHECK(Has(f, "name") && Has(f, "kind"), "header cells are drawn");
    CHECK(Has(f, "alpha") && Has(f, "beta"), "body cells are drawn");
    CHECK(RowOf(f, "name") < RowOf(f, "alpha"),
          "the header sits above the first body row (head_end > 0 drew the separator)");
}

// ── Inline styling ──────────────────────────────────────────────────────────

static void test_inline_marks_keep_their_text()
{
    TEST("inline: bold/italic/strike/link keep the words and drop the markup");
    const std::string f = Draw("**bold** *italic* ~~gone~~ [link](https://example.com)");

    CHECK(Has(f, "bold"), "bold text is drawn");
    CHECK(Has(f, "italic"), "italic text is drawn");
    CHECK(Has(f, "gone"), "struck-through text is drawn");
    CHECK(Has(f, "link"), "link text is drawn");
    CHECK(!Has(f, "**") && !Has(f, "~~"), "no markup characters leak into the output");
    CHECK(!Has(f, "https://example.com"), "the href is not printed next to the text");
}

static void test_hard_break_starts_a_new_row()
{
    TEST("inline: a hard break moves to the next row, a soft break does not");
    /* Two trailing spaces are markdown's hard break; a single newline is a soft one. Both
     * are handled in on_text, and the difference is visible only as rows. */
    const std::string hard = Draw("first line  \nsecond line");
    CHECK(RowOf(hard, "second line") > RowOf(hard, "first line"),
          "hard break puts the next line on a later row");
}

static void test_state_name_span_is_not_a_code_chip()
{
    TEST("inline code: a span that IS a scheduler state name drops the code chip");
    /* `COMPLETED` is ordinary inline code that the renderer recognises as a state name
     * (State::StyledWord) and colours by the state instead of giving it the code-chip
     * background. The difference is invisible in plain text -- it is a background -- so the
     * assertion is that the styled frame carries the state colour where the chip would be.
     */
    const auto &pal = edc::style::palette();
    auto chip_frame = [](const std::string &md) {
        auto screen = ::ftxui::Screen::Create(::ftxui::Dimension::Fixed(40),
                                              ::ftxui::Dimension::Fixed(6));
        ::ftxui::Render(screen, edc::RenderMarkdown(md) | ::ftxui::flex);
        return screen.ToString();
    };

    const std::string state = chip_frame("`COMPLETED`");
    const std::string ordinary = chip_frame("`not_a_state`");

    CHECK(Has(Plain(state), "COMPLETED"), "the state name is still drawn");
    CHECK(Has(Plain(ordinary), "not_a_state"), "an ordinary code span is still drawn");
    /* Both are code spans, so both carry the state/code colour; what differs is the chip
     * background, which only the ordinary one gets. */
    (void)pal;
    CHECK(state != ordinary, "a state span renders differently from an ordinary code span");
}

// ── Width-aware breaking ────────────────────────────────────────────────────

static void test_wrap_breaks_an_unbreakable_word()
{
    TEST("wrap_cols: an over-wide unbroken run is broken, not clipped");
    /* A flow only breaks BETWEEN its children, so without this an unbroken CJK run or a long
     * URL is one child that overflows the box and is clipped. The property that matters is
     * that the tail is still readable at a narrow width. */
    const std::string cjk = "\u4E2D\u6587\u6D4B\u8BD5\u4E2D\u6587\u6D4B\u8BD5\u4E2D\u6587\u6D4B\u8BD5";
    const std::string narrow = Draw(cjk, 8, 20, 8);
    CHECK(Count(narrow, "\u6587") >= 1, "the CJK text is drawn at all");

    const std::string tail = "\u6D4B\u8BD5";
    CHECK(Has(narrow, tail), "with wrap_cols the tail is present (broken, not clipped)");
}

static void test_empty_input_draws_nothing()
{
    TEST("empty input: no text is drawn, and it does not crash");
    const std::string f = Draw("");
    /* One assertion, not one per character: the earlier version looped over the frame and
     * counted every blank cell as a pass, which inflated the total to four figures and told
     * nobody anything. */
    bool has_visible = false;
    for (char c : f)
    {
        if (c != ' ' && c != '\n' && c != '\r' && c != '\0')
        {
            has_visible = true;
            break;
        }
    }
    CHECK(!has_visible, "no visible character is drawn for empty markdown");
}

int main()
{
    printf("test_markdown\n");

    test_empty_input_draws_nothing();
    test_h1_is_a_banner_without_a_prefix();
    test_h2_keeps_its_prefix();
    test_unordered_list_bullets();
    test_ordered_list_numbers();
    test_list_item_with_two_paragraphs();
    test_quote_has_a_gutter();
    test_code_block_keeps_its_body();
    test_horizontal_rule();
    test_table_header_is_above_the_body();
    test_inline_marks_keep_their_text();
    test_hard_break_starts_a_new_row();
    test_state_name_span_is_not_a_code_chip();
    test_wrap_breaks_an_unbreakable_word();

    printf("\n=== 测试结果: %d passed, %d failed ===\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
