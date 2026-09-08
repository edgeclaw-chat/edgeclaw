// Unit tests for the soft-wrapping EdcInput component: rendering a long
// draft into a fixed-width off-screen must wrap onto following rows instead
// of overflowing (the original ftxui::Input clips the excess), and empty
// content shows the placeholder. Rendering is done twice per case because
// the widget measures its own box from the previous frame's layout (exactly
// like in the live TUI, where the first frame after a resize converges one
// frame later).

#include "edc_input.hpp"

#include <ftxui/dom/node.hpp> // Render
#include <ftxui/screen/screen.hpp>
#include <ftxui/component/event.hpp>
#include <cstdio>
#include <string>

static int g_failures = 0;

#define CHECK(cond, ...)                                                    \
    do                                                                      \
    {                                                                       \
        if (!(cond))                                                        \
        {                                                                   \
            std::printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            std::printf("    ");                                            \
            std::printf(__VA_ARGS__);                                       \
            std::printf("\n");                                              \
            g_failures++;                                                   \
        }                                                                   \
    } while (0)

// Render |input| into a |width|x|height| screen twice (converge the wrap
// width), returning the filled screen.
static ftxui::Screen render_twice(edc::EdcInput &input, int width, int height)
{
    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(width),
                                        ftxui::Dimension::Fixed(height));
    {
        auto doc = input.Render();
        Render(screen, doc);
    }
    screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(width),
                                   ftxui::Dimension::Fixed(height));
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
    CHECK(row_text(screen, 0, 10) == std::string(9, 'a'), "row0 '%s'",
          row_text(screen, 0, 10).c_str());
    CHECK(row_text(screen, 1, 10) == std::string(9, 'a'), "row1 '%s'",
          row_text(screen, 1, 10).c_str());
    CHECK(row_text(screen, 2, 10) == std::string(9, 'a'), "row2 '%s'",
          row_text(screen, 2, 10).c_str());
    CHECK(row_text(screen, 3, 10) == "aaa", "row3 '%s'",
          row_text(screen, 3, 10).c_str());
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
        CHECK(static_cast<int>(r.size()) <= 19, "row %d width %zu > 19", y,
              r.size());
        if (y == 0)
        {
            CHECK(r.rfind("the quick", 0) == 0, "row0 starts with prose, got '%s'",
                  r.c_str());
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
    CHECK(row_text(screen, 0, 20) == "type here", "got '%s'",
          row_text(screen, 0, 20).c_str());
}

static void test_multiline_content(void)
{
    std::printf("  explicit \\n lines stay on their own visual rows\n");
    std::string text = "abc\ndef";
    edc::EdcInput input(&text, "");
    const ftxui::Screen screen = render_twice(input, 20, 2);
    CHECK(row_text(screen, 0, 20) == "abc", "row0 '%s'",
          row_text(screen, 0, 20).c_str());
    CHECK(row_text(screen, 1, 20) == "def", "row1 '%s'",
          row_text(screen, 1, 20).c_str());
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
    CHECK(input.CursorPosition() == 25, "down on last row -> end %d",
          input.CursorPosition());
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
    CHECK(input.CursorPosition() == 3, "cursor after CJK char %d",
          input.CursorPosition());
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

int main(void)
{
    test_long_word_wraps();
    test_words_wrap_and_rows_fit();
    test_empty_shows_placeholder();
    test_multiline_content();
    test_arrow_navigation_across_rows();
    test_typing_and_deletion();
    test_internal_scroll_follows_caret();

    if (g_failures)
    {
        std::printf("%d check(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("all EdcInput tests passed\n");
    return 0;
}
