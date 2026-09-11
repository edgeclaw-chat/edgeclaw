// Unit tests for the soft-wrapping EdcInput component: rendering a long
// draft into a fixed-width off-screen must wrap onto following rows instead
// of overflowing (the original ftxui::Input clips the excess), and empty
// content shows the placeholder. Rendering is done twice per case because
// the widget measures its own box from the previous frame's layout (exactly
// like in the live TUI, where the first frame after a resize converges one
// frame later).

#include "edc_input.hpp"

#include <cstdio>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/node.hpp> // Render
#include <ftxui/screen/screen.hpp>
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

// Render |input| into a |width|x|height| screen twice (converge the wrap
// width), returning the filled screen.
static ftxui::Screen render_twice(edc::EdcInput &input, int width, int height)
{
    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(width), ftxui::Dimension::Fixed(height));
    {
        auto doc = input.Render();
        Render(screen, doc);
    }
    screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(width), ftxui::Dimension::Fixed(height));
    {
        auto doc = input.Render();
        Render(screen, doc);
    }
    return screen;
}

static std::string row_text(const ftxui::Screen &screen, int y, int width)
{
    std::string out;
    for (int x = 0; x < width; ++x)
        out += screen.CellAt(x, y).character;
    // Trim the trailing blanks the screen pads with.
    while (!out.empty() && out.back() == ' ')
        out.pop_back();
    return out;
}

static void test_long_word_wraps(void)
{
    std::printf("  long word soft-wraps across rows, nothing overflows\n");
    std::string text(30, 'a');
    edc::EdcInput input(&text, "");
    const ftxui::Screen screen = render_twice(input, 10, 8);
    // Wrap budget is width - 1 (one column reserved for the caret cell), so
    // 30 cells of 'a' become 9 + 9 + 9 + 3.
    CHECK(row_text(screen, 0, 10) == std::string(9, 'a'), "row0 '%s'", row_text(screen, 0, 10).c_str());
    CHECK(row_text(screen, 1, 10) == std::string(9, 'a'), "row1 '%s'", row_text(screen, 1, 10).c_str());
    CHECK(row_text(screen, 2, 10) == std::string(9, 'a'), "row2 '%s'", row_text(screen, 2, 10).c_str());
    CHECK(row_text(screen, 3, 10) == "aaa", "row3 '%s'", row_text(screen, 3, 10).c_str());
    CHECK(row_text(screen, 4, 10).empty(), "row4 should be empty");
    // The reserved column is never painted over by text.
    const auto &blank0 = screen.CellAt(9, 0).character;
    const auto &blank2 = screen.CellAt(9, 2).character;
    CHECK(blank0.empty() || blank0 == " ", "cell(9,0) must stay blank");
    CHECK(blank2.empty() || blank2 == " ", "cell(9,2) must stay blank");
}

static void test_words_wrap_and_rows_fit(void)
{
    std::printf("  prose wraps at word boundaries within the budget\n");
    std::string text = "the quick brown fox jumps over the lazy dog";
    edc::EdcInput input(&text, "");
    const ftxui::Screen screen = render_twice(input, 20, 8);
    bool saw_prefix = false;
    bool saw_tail = false;
    int occupied = 0;
    for (int y = 0; y < 8; ++y)
    {
        const std::string r = row_text(screen, y, 20);
        if (r.empty())
            break;
        ++occupied;
        CHECK(static_cast<int>(r.size()) <= 19, "row %d width %zu > 19", y, r.size());
        if (y == 0)
        {
            CHECK(r.rfind("the quick", 0) == 0, "row0 starts with prose, got '%s'", r.c_str());
            saw_prefix = true;
        }
        if (r.rfind("dog", r.size() - 3) != std::string::npos)
            saw_tail = true;
    }
    CHECK(saw_prefix, "first row missing");
    CHECK(saw_tail, "last word 'dog' never rendered");
    CHECK(occupied >= 3, "expected 3+ wrapped rows, got %d", occupied);
}

static void test_empty_shows_placeholder(void)
{
    std::printf("  empty content shows the dimmed placeholder\n");
    std::string text;
    edc::EdcInput input(&text, "type here");
    const ftxui::Screen screen = render_twice(input, 20, 1);
    CHECK(row_text(screen, 0, 20) == "type here", "got '%s'", row_text(screen, 0, 20).c_str());
}

static void test_multiline_content(void)
{
    std::printf("  explicit \\n lines stay on their own visual rows\n");
    std::string text = "abc\ndef";
    edc::EdcInput input(&text, "");
    const ftxui::Screen screen = render_twice(input, 20, 2);
    CHECK(row_text(screen, 0, 20) == "abc", "row0 '%s'", row_text(screen, 0, 20).c_str());
    CHECK(row_text(screen, 1, 20) == "def", "row1 '%s'", row_text(screen, 1, 20).c_str());
}

static void test_arrow_navigation_across_rows(void)
{
    std::printf("  arrows navigate wrapped visual rows keeping the column\n");
    std::string text(25, 'a'); // 9 + 9 + 7 rows at wrap budget 9
    edc::EdcInput input(&text, "");
    render_twice(input, 10, 8); // measure the widget box -> wrap budget 9
    CHECK(input.CursorPosition() == 0, "start cursor %d", input.CursorPosition());

    for (int i = 0; i < 4; ++i)
        input.OnEvent(::ftxui::Event::ArrowRight);
    CHECK(input.CursorPosition() == 4, "right x4 -> %d", input.CursorPosition());

    CHECK(input.OnEvent(::ftxui::Event::ArrowDown), "down handled");
    CHECK(input.CursorPosition() == 13, "down row1 col4 -> %d", input.CursorPosition());
    CHECK(input.OnEvent(::ftxui::Event::ArrowDown), "down handled");
    CHECK(input.CursorPosition() == 22, "down row2 col4 -> %d", input.CursorPosition());
    CHECK(input.OnEvent(::ftxui::Event::ArrowDown), "down to end handled");
    CHECK(input.CursorPosition() == 25, "down on last row -> end %d", input.CursorPosition());
    CHECK(!input.OnEvent(::ftxui::Event::ArrowDown), "down at end not handled");

    CHECK(input.OnEvent(::ftxui::Event::ArrowUp), "up handled");
    CHECK(input.CursorPosition() == 16, "up row1 col7 -> %d", input.CursorPosition());
    CHECK(input.OnEvent(::ftxui::Event::ArrowUp), "up handled");
    CHECK(input.CursorPosition() == 7, "up row0 col7 -> %d", input.CursorPosition());
    CHECK(input.OnEvent(::ftxui::Event::ArrowUp), "up from first row -> start");
    CHECK(input.CursorPosition() == 0, "up on first row -> %d", input.CursorPosition());
    CHECK(!input.OnEvent(::ftxui::Event::ArrowUp), "up at start not handled");

    // Horizontal movement across a row boundary (over the wrap point).
    input.OnEvent(::ftxui::Event::End);
    CHECK(input.CursorPosition() == 25, "End -> %d", input.CursorPosition());
    for (int i = 0; i < 4; ++i)
        input.OnEvent(::ftxui::Event::ArrowLeft);
    CHECK(input.CursorPosition() == 21, "left x4 -> %d", input.CursorPosition());
    input.OnEvent(::ftxui::Event::Home);
    CHECK(input.CursorPosition() == 0, "Home -> %d", input.CursorPosition());
}

static void test_typing_and_deletion(void)
{
    std::printf("  typing/backspace/delete are glyph and newline aware\n");
    std::string text;
    edc::EdcInput input(&text, "");
    render_twice(input, 10, 8);

    CHECK(!input.OnEvent(::ftxui::Event::Return), "Return must not insert");
    CHECK(text.empty(), "Return changed content");

    input.OnEvent(::ftxui::Event::Character("你"));
    CHECK(text == "你", "CJK char inserted, got '%s'", text.c_str());
    CHECK(input.CursorPosition() == 3, "cursor after CJK char %d", input.CursorPosition());
    input.OnEvent(::ftxui::Event::Character("\n")); // Alt+Enter path
    input.OnEvent(::ftxui::Event::Character('b'));
    CHECK(text == "你\nb", "newline + char, got '%s'", text.c_str());
    CHECK(input.CursorPosition() == 5, "cursor %d", input.CursorPosition());

    input.OnEvent(::ftxui::Event::Backspace); // removes 'b'
    CHECK(text == "你\n", "backspace across char, got '%s'", text.c_str());
    input.OnEvent(::ftxui::Event::Backspace); // removes '\n', merging lines
    CHECK(text == "你", "backspace removed newline, got '%s'", text.c_str());
    input.OnEvent(::ftxui::Event::Backspace); // removes the whole CJK glyph
    CHECK(text.empty(), "backspace removed CJK glyph whole");

    // Delete key removes the glyph under the caret.
    input.OnEvent(::ftxui::Event::Character("ab"));
    CHECK(text == "ab", "typed 'ab', got '%s'", text.c_str());
    input.OnEvent(::ftxui::Event::Home);
    input.OnEvent(::ftxui::Event::Delete);
    CHECK(text == "b", "Delete removed 'a', got '%s'", text.c_str());
    input.OnEvent(::ftxui::Event::Delete);
    input.OnEvent(::ftxui::Event::Delete);
    CHECK(text.empty(), "Delete drained content");
    CHECK(!input.OnEvent(::ftxui::Event::Delete), "Delete on empty not handled");
}

static void test_internal_scroll_follows_caret(void)
{
    std::printf("  tall drafts scroll so the caret row stays visible\n");
    std::string text(100, 'a'); // 11 full rows of 9 + 1 trailing 'a'
    edc::EdcInput input(&text, "");
    // Default max_visible_rows is 8.
    {
        const ftxui::Screen screen = render_twice(input, 10, 8);
        // Caret at the start: the window shows rows 0..7.
        CHECK(row_text(screen, 0, 10) == std::string(9, 'a'), "top window row0");
        CHECK(row_text(screen, 7, 10) == std::string(9, 'a'), "top window row7");
    }
    input.OnEvent(::ftxui::Event::End); // caret on the trailing 'a', row 11
    {
        const ftxui::Screen screen = render_twice(input, 10, 8);
        // Window slides to rows 4..11: every visible row is 9 wide except the
        // last, which is the single trailing 'a'.
        CHECK(row_text(screen, 0, 10) == std::string(9, 'a'), "bottom window row0");
        CHECK(row_text(screen, 6, 10) == std::string(9, 'a'), "bottom window row6");
        CHECK(row_text(screen, 7, 10) == "a", "bottom window row7 shows trailing 'a'");
    }
}

static void test_history_recall_up_down(void)
{
    std::printf("  history: up recalls older, down returns to the draft\n");
    std::string text;
    edc::EdcInput input(&text, "");
    input.AddHistory("first");
    input.AddHistory("second");
    input.AddHistory("third");
    render_twice(input, 20, 8); // measure the box -> wrap budget 19

    // Empty box: the first Up recalls the newest entry (caret at its end).
    CHECK(input.OnEvent(::ftxui::Event::ArrowUp), "up handled");
    CHECK(text == "third", "up -> newest, got '%s'", text.c_str());
    CHECK(input.CursorPosition() == 5, "caret at end of recalled entry, got %d", input.CursorPosition());

    input.OnEvent(::ftxui::Event::ArrowUp);
    CHECK(text == "second", "up -> second, got '%s'", text.c_str());
    input.OnEvent(::ftxui::Event::ArrowUp);
    CHECK(text == "first", "up -> first, got '%s'", text.c_str());
    CHECK(input.OnEvent(::ftxui::Event::ArrowUp), "up at oldest consumed");
    CHECK(text == "first", "up at oldest stays put, got '%s'", text.c_str());

    input.OnEvent(::ftxui::Event::ArrowDown);
    CHECK(text == "second", "down -> second, got '%s'", text.c_str());
    input.OnEvent(::ftxui::Event::ArrowDown);
    CHECK(text == "third", "down -> third, got '%s'", text.c_str());
    input.OnEvent(::ftxui::Event::ArrowDown); // past the newest: back to the draft
    CHECK(text.empty(), "down past newest restores the empty draft, got '%s'", text.c_str());
    // No longer browsing: Down is plain caret movement again.
    CHECK(!input.OnEvent(::ftxui::Event::ArrowDown), "down at end not handled");
}

static void test_history_preserves_draft(void)
{
    std::printf("  history: a typed draft is stashed and restored on the way back\n");
    std::string text = "my draft";
    edc::EdcInput input(&text, "");
    input.AddHistory("alpha");
    input.AddHistory("beta");
    render_twice(input, 20, 8);
    input.OnEvent(::ftxui::Event::End); // caret at the end (single visual row)

    input.OnEvent(::ftxui::Event::ArrowUp);
    CHECK(text == "beta", "up recalls beta, got '%s'", text.c_str());
    input.OnEvent(::ftxui::Event::ArrowUp);
    CHECK(text == "alpha", "up recalls alpha, got '%s'", text.c_str());
    input.OnEvent(::ftxui::Event::ArrowDown);
    CHECK(text == "beta", "down -> beta, got '%s'", text.c_str());
    input.OnEvent(::ftxui::Event::ArrowDown);
    CHECK(text == "my draft", "down restores the draft, got '%s'", text.c_str());
    CHECK(input.CursorPosition() == 8, "draft caret restored, got %d", input.CursorPosition());
}

static void test_history_dedupes_consecutive(void)
{
    std::printf("  history: consecutive duplicates collapse\n");
    std::string text;
    edc::EdcInput input(&text, "");
    input.AddHistory("dup");
    input.AddHistory("dup");
    input.AddHistory("other");
    input.AddHistory("other");
    render_twice(input, 20, 8);

    CHECK(input.History().size() == 2, "history size %zu", input.History().size());
    input.OnEvent(::ftxui::Event::ArrowUp);
    CHECK(text == "other", "newest is 'other', got '%s'", text.c_str());
    input.OnEvent(::ftxui::Event::ArrowUp);
    CHECK(text == "dup", "then 'dup', got '%s'", text.c_str());
}

static void test_history_edit_leaves_browsing(void)
{
    std::printf("  history: editing a recalled entry leaves history mode\n");
    std::string text;
    edc::EdcInput input(&text, "");
    input.AddHistory("old1");
    input.AddHistory("old2");
    render_twice(input, 20, 8);

    input.OnEvent(::ftxui::Event::ArrowUp); // recall old2, caret at its end
    CHECK(text == "old2", "recalled old2, got '%s'", text.c_str());
    input.OnEvent(::ftxui::Event::Character("!"));
    CHECK(text == "old2!", "edited recalled entry, got '%s'", text.c_str());

    // The edit leaves history mode; the edited text becomes the current draft
    // (so the next Up browses afresh from the newest entry) and Down brings the
    // edited draft back instead of losing it.
    input.OnEvent(::ftxui::Event::ArrowUp);
    CHECK(text == "old2", "up after edit browses from newest, got '%s'", text.c_str());
    input.OnEvent(::ftxui::Event::ArrowDown);
    CHECK(text == "old2!", "down restores the edited draft, got '%s'", text.c_str());
}

static void test_history_up_from_lower_row_moves_caret(void)
{
    std::printf("  history: up on a lower visual row moves the caret, not history\n");
    std::string text = "abcdefghijklmnopqrstuvwxyz"; // soft-wraps at width 10
    edc::EdcInput input(&text, "");
    input.AddHistory("hist");
    render_twice(input, 10, 8);

    input.OnEvent(::ftxui::Event::End); // caret on the last visual row
    const int before = input.CursorPosition();
    CHECK(input.OnEvent(::ftxui::Event::ArrowUp), "up handled");
    CHECK(text == "abcdefghijklmnopqrstuvwxyz", "content untouched by caret up");
    CHECK(input.CursorPosition() != before, "caret moved up within the draft");

    // Once the caret reaches the first visual row, Up recalls history and the
    // draft is stashed for ArrowDown to bring back.
    input.OnEvent(::ftxui::Event::Home);
    input.OnEvent(::ftxui::Event::ArrowUp);
    CHECK(text == "hist", "up from the first row recalls history, got '%s'", text.c_str());
    input.OnEvent(::ftxui::Event::ArrowDown);
    CHECK(text == "abcdefghijklmnopqrstuvwxyz", "down restores the stashed draft, got '%s'", text.c_str());
}

static void test_history_clear(void)
{
    std::printf("  history: ClearHistory empties the recall list\n");
    std::string text;
    edc::EdcInput input(&text, "");
    input.AddHistory("a");
    render_twice(input, 20, 4);
    CHECK(!input.History().empty(), "history seeded");
    input.ClearHistory();
    CHECK(input.History().empty(), "history cleared");
    CHECK(!input.OnEvent(::ftxui::Event::ArrowUp), "up with no history not handled");
}

int main(void)
{
    test_long_word_wraps();
    test_words_wrap_and_rows_fit();
    test_empty_shows_placeholder();
    test_multiline_content();
    test_arrow_navigation_across_rows();
    test_typing_and_deletion();
    test_internal_scroll_follows_caret();
    test_history_recall_up_down();
    test_history_preserves_draft();
    test_history_dedupes_consecutive();
    test_history_edit_leaves_browsing();
    test_history_up_from_lower_row_moves_caret();
    test_history_clear();

    if (g_failures)
    {
        std::printf("%d check(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("all EdcInput tests passed\n");
    return 0;
}
