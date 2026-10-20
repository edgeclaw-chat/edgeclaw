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

static void test_cursor_and_range_replace(void)
{
    std::printf("  SetCursor / ReplaceRange: clamped, snapped, caret after the insert\n");
    std::string text = "abc def";
    edc::EdcInput input(&text, "");

    input.SetCursor(-5);
    CHECK(input.CursorPosition() == 0, "a negative caret clamps to 0, got %d", input.CursorPosition());
    input.SetCursor(9999);
    CHECK(input.CursorPosition() == static_cast<int>(text.size()), "past the end clamps to the end, got %d",
          input.CursorPosition());

    text = "abc def";
    input.ReplaceRange(0, 3, "XYZ");
    CHECK(text == "XYZ def", "a prefix is replaced in place: '%s'", text.c_str());
    CHECK(input.CursorPosition() == 3, "the caret sits right after the insert, got %d", input.CursorPosition());

    /* What `@` completion does: replace the half-typed mention, keep the rest. */
    text = "@src x";
    input.ReplaceRange(0, 4, "@src/main.c ");
    CHECK(text == "@src/main.c  x", "the mention is completed in place: '%s'", text.c_str());
    CHECK(input.CursorPosition() == 12, "caret after the inserted text, got %d", input.CursorPosition());

    /* Reversed offsets are accepted (one caller passes trigger/cursor in order; another
       may not). */
    text = "abcdef";
    input.ReplaceRange(4, 1, "-");
    CHECK(text == "a-ef", "begin > end is swapped: '%s'", text.c_str());

    /* An offset inside a multi-byte character snaps back to its start, so the
       replacement can never split it. */
    text = "\xe4\xb8\xad\xe6\x96\x87x"; /* 中文x */
    input.ReplaceRange(1, 3, "-");
    CHECK(text == "-\xe6\x96\x87x", "a caret inside a character snaps to its boundary: '%s'", text.c_str());
    CHECK(input.CursorPosition() == 1, "and the caret follows the insert, got %d", input.CursorPosition());
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
    // The draft is a prefix filter, so the recallable entries have to start with it.
    std::string text = "al";
    edc::EdcInput input(&text, "");
    input.AddHistory("alpha");
    input.AddHistory("alpine");
    render_twice(input, 20, 8);
    input.OnEvent(::ftxui::Event::End); // caret at the end (single visual row)

    input.OnEvent(::ftxui::Event::ArrowUp);
    CHECK(text == "alpine", "up recalls the newest match, got '%s'", text.c_str());
    input.OnEvent(::ftxui::Event::ArrowUp);
    CHECK(text == "alpha", "up recalls alpha, got '%s'", text.c_str());
    input.OnEvent(::ftxui::Event::ArrowDown);
    CHECK(text == "alpine", "down -> alpine, got '%s'", text.c_str());
    input.OnEvent(::ftxui::Event::ArrowDown);
    CHECK(text == "al", "down restores the draft, got '%s'", text.c_str());
    CHECK(input.CursorPosition() == 2, "draft caret restored, got %d", input.CursorPosition());
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
    input.AddHistory("/task list");
    input.AddHistory("/task new");
    render_twice(input, 20, 8);

    input.OnEvent(::ftxui::Event::ArrowUp); // recall /task new, caret at its end
    CHECK(text == "/task new", "recalled /task new, got '%s'", text.c_str());
    input.OnEvent(::ftxui::Event::Backspace);
    CHECK(text == "/task ne", "edited recalled entry, got '%s'", text.c_str());
    CHECK(!input.IsBrowsingHistory(), "the edit ends the browse");

    // The edited text becomes the current draft and the prefix filter is taken from
    // it afresh: the next Up browses the entries starting with "/task ne" from the
    // newest one, and Down brings the edited draft back instead of losing it.
    input.OnEvent(::ftxui::Event::ArrowUp);
    CHECK(text == "/task new", "up after edit browses from the newest match, got '%s'", text.c_str());
    input.OnEvent(::ftxui::Event::ArrowDown);
    CHECK(text == "/task ne", "down restores the edited draft, got '%s'", text.c_str());
}

static void test_history_up_from_lower_row_moves_caret(void)
{
    std::printf("  history: up on a lower visual row moves the caret, not history\n");
    const std::string draft = "abcdefghijklmnopqrstuvwxyz"; // soft-wraps at width 10
    std::string text = draft;
    edc::EdcInput input(&text, "");
    // The draft is a prefix filter, so a recallable entry has to extend it.
    input.AddHistory(draft + " tail");
    render_twice(input, 10, 8);

    input.OnEvent(::ftxui::Event::End); // caret on the last visual row
    const int before = input.CursorPosition();
    CHECK(input.OnEvent(::ftxui::Event::ArrowUp), "up handled");
    CHECK(text == draft, "content untouched by caret up");
    CHECK(input.CursorPosition() != before, "caret moved up within the draft");

    // Once the caret reaches the first visual row, Up recalls history and the
    // draft is stashed for ArrowDown to bring back.
    input.OnEvent(::ftxui::Event::Home);
    input.OnEvent(::ftxui::Event::ArrowUp);
    CHECK(text == draft + " tail", "up from the first row recalls history, got '%s'", text.c_str());
    input.OnEvent(::ftxui::Event::ArrowDown);
    CHECK(text == draft, "down restores the stashed draft, got '%s'", text.c_str());
}

/* The owner asks "am I browsing?" to keep the completion popup from opening on a
   recalled `/command` (it would then steal Up/Down from the history). The flag has
   to be up exactly between the recalling Up and the edit that ends the browse. */
static void test_history_browsing_flag(void)
{
    std::printf("  history: IsBrowsingHistory marks a recall from the first Up to the edit\n");
    std::string text;
    edc::EdcInput input(&text, "");
    input.AddHistory("/task");
    render_twice(input, 20, 8);

    CHECK(!input.IsBrowsingHistory(), "a fresh input is not browsing");
    input.OnEvent(::ftxui::Event::ArrowUp);
    CHECK(input.IsBrowsingHistory(), "after a recall the flag is up");
    input.OnEvent(::ftxui::Event::ArrowDown); // past the newest: back to the draft
    CHECK(!input.IsBrowsingHistory(), "down past the newest ends the browse");

    input.OnEvent(::ftxui::Event::ArrowUp);
    input.OnEvent(::ftxui::Event::Character("x")); // an edit ends it too
    CHECK(!input.IsBrowsingHistory(), "an edit ends the browse");
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

/* Recalling only what the user is in the middle of typing: the draft at the moment
   the browse starts is a prefix filter, so `task ` + Up walks the `task ...` lines
   instead of every unrelated message. */
static void test_history_prefix_filters_entries(void)
{
    std::printf("  history: only entries starting with the typed draft are recalled\n");
    std::string text = "task ";
    edc::EdcInput input(&text, "");
    input.AddHistory("hello world");
    input.AddHistory("task list");
    input.AddHistory("task new --foo");
    render_twice(input, 40, 8);
    input.OnEvent(::ftxui::Event::End); // caret at the end of the draft

    CHECK(input.OnEvent(::ftxui::Event::ArrowUp), "up handled");
    CHECK(text == "task new --foo", "newest match recalled, got '%s'", text.c_str());
    input.OnEvent(::ftxui::Event::ArrowUp);
    CHECK(text == "task list", "the non-matching entry is skipped, got '%s'", text.c_str());
    CHECK(input.OnEvent(::ftxui::Event::ArrowUp), "up at the oldest match consumed");
    CHECK(text == "task list", "stays at the oldest match, got '%s'", text.c_str());

    input.OnEvent(::ftxui::Event::ArrowDown);
    CHECK(text == "task new --foo", "down -> the next match, got '%s'", text.c_str());
    input.OnEvent(::ftxui::Event::ArrowDown); // past the newest match
    CHECK(text == "task ", "down past the newest match restores the draft, got '%s'", text.c_str());
    CHECK(input.CursorPosition() == 5, "draft caret restored, got %d", input.CursorPosition());
    CHECK(!input.IsBrowsingHistory(), "the browse is over");
}

/* A draft nothing matches is not a recall: ArrowUp keeps its caret meaning and the
   draft is not stashed as if a browse had started. */
static void test_history_prefix_no_match_keeps_caret(void)
{
    std::printf("  history: a draft nothing matches leaves ArrowUp on the caret\n");
    std::string text = "zzz";
    edc::EdcInput input(&text, "");
    input.AddHistory("task list");
    render_twice(input, 40, 8);
    input.OnEvent(::ftxui::Event::End);

    input.OnEvent(::ftxui::Event::ArrowUp);
    CHECK(text == "zzz", "content untouched by a non-matching up, got '%s'", text.c_str());
    CHECK(!input.IsBrowsingHistory(), "no recall -> not browsing");
    CHECK(input.CursorPosition() == 0, "up still moves the caret to the start, got %d", input.CursorPosition());
}

/* The filter is a real prefix: a shorter entry and a case difference both miss. */
static void test_history_prefix_exact_and_shorter(void)
{
    std::printf("  history: a shorter or differently-cased entry does not match the prefix\n");
    std::string text = "task ";
    edc::EdcInput input(&text, "");
    input.AddHistory("task");      // strictly shorter than the draft
    input.AddHistory("Task list"); // same text, different case
    render_twice(input, 40, 8);
    input.OnEvent(::ftxui::Event::End);

    input.OnEvent(::ftxui::Event::ArrowUp);
    CHECK(text == "task ", "draft untouched, got '%s'", text.c_str());
    CHECK(!input.IsBrowsingHistory(), "nothing matched, so no browse");
}

// The mode label is what gets written into the input box's top rule. A plain message has
// none (the rule stays plain, and the prompt below stays flush left); everything else names
// itself, and the two separators centre it.
static void test_paste_block_folds_and_expands(void)
{
    std::printf("  paste block: one placeholder in the box, the bytes behind it for the message\n");
    std::string text;
    edc::EdcInput input(&text, "");

    input.InsertPasteBlock("[Pasted #1 \xc2\xb7 3 lines]", "line1\nline2\nline3");

    /* The buffer holds ONE line: that is the whole point of folding. */
    CHECK(text == "[Pasted #1 \xc2\xb7 3 lines] ", "'%s'", text.c_str());
    CHECK(text.find('\n') == std::string::npos, "no newline in the buffer");
    CHECK(input.CursorPosition() == static_cast<int>(text.size()), "the caret sits after the chip");

    /* Rendering it stays on one row. */
    const auto screen = render_twice(input, 40, 6);
    CHECK(row_text(screen, 0, 40) == "[Pasted #1 \xc2\xb7 3 lines]", "row 0 shows the chip: '%s'",
          row_text(screen, 0, 40).c_str());
    CHECK(row_text(screen, 1, 40).empty(), "and nothing wraps below it: '%s'", row_text(screen, 1, 40).c_str());

    /* But the line SAYS the pasted bytes: that is what the app receives. */
    CHECK(input.ExpandedDraft() == "line1\nline2\nline3 ", "expanded: '%s'", input.ExpandedDraft().c_str());
    CHECK(edc_payload_count(input.Blocks()) == 1, "one block in the payload");
}

static void test_paste_block_is_one_atom(void)
{
    std::printf("  paste block: Backspace deletes the whole chip, payload and all\n");
    std::string text;
    edc::EdcInput input(&text, "");
    input.InsertPasteBlock("[Pasted #1 \xc2\xb7 2 lines]", "a\nb");

    /* First Backspace eats the trailing space the insert added... */
    input.OnEvent(ftxui::Event::Backspace);
    CHECK(text == "[Pasted #1 \xc2\xb7 2 lines]", "the trailing space goes first: '%s'", text.c_str());
    CHECK(edc_payload_count(input.Blocks()) == 1, "the chip is still there");

    /* ...the second one eats the chip as one atom, and the payload entry with it. */
    input.OnEvent(ftxui::Event::Backspace);
    CHECK(text.empty(), "the whole placeholder is gone: '%s'", text.c_str());
    CHECK(edc_payload_count(input.Blocks()) == 0, "and its parked bytes are gone too");
    CHECK(input.ExpandedDraft().empty(), "nothing left to expand");
}

static void test_paste_block_moves_and_caret(void)
{
    std::printf("  paste block: Left/Right step over the chip, they do not walk into it\n");
    std::string text;
    edc::EdcInput input(&text, "");
    input.InsertPasteBlock("[Pasted #1 \xc2\xb7 2 lines]", "a\nb");

    const int end = static_cast<int>(text.size());
    input.OnEvent(ftxui::Event::ArrowLeft); /* over the trailing space */
    CHECK(input.CursorPosition() == end - 1, "one column into the trailing space, got %d", input.CursorPosition());
    input.OnEvent(ftxui::Event::ArrowLeft);
    CHECK(input.CursorPosition() == 0, "and then straight to the chip's start, got %d", input.CursorPosition());
    input.OnEvent(ftxui::Event::ArrowRight);
    CHECK(input.CursorPosition() == end - 1, "Right jumps over the whole chip again, got %d", input.CursorPosition());

    /* Typing at the chip's edge does not eat it: the placeholder stays intact. */
    input.SetCursor(0);
    input.OnEvent(ftxui::Event::ArrowRight);
    /* The caret is right after the chip (ArrowRight put it there), so the text lands
     * between the chip and its trailing space. */
    input.ReplaceRange(input.CursorPosition(), input.CursorPosition(), "- ");
    CHECK(edc_payload_count(input.Blocks()) == 1, "still one block after typing next to it");
    CHECK(input.ExpandedDraft() == "a\nb-  ", "expanded: '%s'", input.ExpandedDraft().c_str());
}

static void test_paste_block_expansion_tracks_edits(void)
{
    std::printf("  paste block: the expanded draft is cached but never stale\n");
    std::string text;
    edc::EdcInput input(&text, "");
    input.InsertPasteBlock("[Pasted #1 \xc2\xb7 2 lines]", "a\nb");

    CHECK(input.ExpandedDraft() == "a\nb ", "first expansion");
    input.ReplaceRange(0, 0, "看下 ");
    CHECK(input.ExpandedDraft() == "看下 a\nb ", "the cache follows the edit: '%s'", input.ExpandedDraft().c_str());
}

static void test_paste_block_broken_placeholder(void)
{
    std::printf("  paste block: a placeholder the user edited through stops expanding\n");
    std::string text;
    edc::EdcInput input(&text, "");
    input.InsertPasteBlock("[Pasted #1 \xc2\xb7 2 lines]", "a\nb");

    /* Delete the closing bracket: the key no longer matches, so the text is plain. */
    const int close = static_cast<int>(text.find(']'));
    input.ReplaceRange(close, close + 1, "");
    CHECK(input.ExpandedDraft() == text, "no expansion without the key: '%s'", input.ExpandedDraft().c_str());

    /* The bytes stay in the library (another history entry may still point at them), but
     * the draft references nothing -- so a submit carries nothing. */
    CHECK(edc_payload_count(input.Blocks()) == 1, "the block is still known");
    CHECK(edc_payload_empty(input.DraftPayload()), "but nothing is referenced by this draft");
}

static void test_paste_block_library(void)
{
    std::printf("  paste block: a recalled entry's bytes can arrive later (library model)\n");
    std::string text = "[Pasted #7 \xc2\xb7 2 lines] answer this";
    edc::EdcInput input(&text, "");

    /* A line recalled from history: its placeholder is text, its bytes are not here yet. */
    CHECK(input.HasUnresolvedBlocks(), "a placeholder with no bytes behind it is unresolved");
    CHECK(edc_payload_empty(input.DraftPayload()), "nothing to send yet");
    CHECK(input.ExpandedDraft() == text, "and nothing to expand: '%s'", input.ExpandedDraft().c_str());

    /* The store's answer lands: the library learns the block, the line starts expanding. */
    struct edc_payload *restored = edc_payload_new();
    edc_payload_set(restored, "[Pasted #7 \xc2\xb7 2 lines]", "a\nb");
    input.RestoreBlocks(restored);
    edc_payload_free(restored);

    CHECK(!input.HasUnresolvedBlocks(), "the placeholder is resolved now");
    CHECK(input.ExpandedDraft() == "a\nb answer this", "expanded: '%s'", input.ExpandedDraft().c_str());
    CHECK(edc_payload_count(input.DraftPayload()) == 1, "and the block travels with the message");

    /* Typing your own placeholder is text, not a block: there is nothing to fetch. */
    std::string typed = "[Pasted #9 \xc2\xb7 1 line]";
    edc::EdcInput plain(&typed, "");
    CHECK(plain.HasUnresolvedBlocks(), "a hand-typed placeholder looks the same...");
    CHECK(edc_payload_empty(plain.DraftPayload()), "...and carries nothing (it is just text)");
}

static void test_history_entry_removal(void)
{
    std::printf("  history: an entry whose payload is gone can be dropped\n");
    std::string text;
    edc::EdcInput input(&text, "");
    input.AddHistory("first");
    input.AddHistory("[Pasted #1 \xc2\xb7 2 lines]");
    CHECK(input.History().size() == 2, "two entries");

    input.RemoveHistoryEntry("[Pasted #1 \xc2\xb7 2 lines]");
    CHECK(input.History().size() == 1 && input.History()[0] == "first", "the dead one is gone");
}

static void test_mention_atom(void)
{
    std::printf("  mention atom: a completed `@file` is one unit while editing\n");
    std::string text = "看下 @src/main.c 这段";
    edc::EdcInput input(&text, "");
    input.RegisterAtom("@src/main.c");
    CHECK(input.AtomCount() == 1, "the mention is registered");

    /* The buffer is unchanged: `@` keeps its meaning, nothing is expanded. */
    CHECK(text == "看下 @src/main.c 这段", "'%s'", text.c_str());
    CHECK(input.ExpandedDraft() == text, "no payload, no expansion");

    /* Caret just after the mention: Backspace deletes the WHOLE mention, not one glyph. */
    input.SetCursor(static_cast<int>(text.find(" 这段")));
    input.OnEvent(ftxui::Event::Backspace);
    CHECK(text == "看下  这段", "the whole @mention is gone: '%s'", text.c_str());
    CHECK(input.CursorPosition() == static_cast<int>(std::string("看下 ").size()), "caret where it was: %d",
          input.CursorPosition());

    /* Delete at the mention's first glyph removes it too. */
    text = "a @src/main.c b";
    input.SetCursor(2);
    input.OnEvent(ftxui::Event::Delete);
    CHECK(text == "a  b", "Delete takes the whole mention: '%s'", text.c_str());

    /* Left/Right step over it instead of walking inside. */
    text = "see @src/main.c";
    input.SetCursor(static_cast<int>(text.size()));
    input.OnEvent(ftxui::Event::ArrowLeft);
    CHECK(input.CursorPosition() == static_cast<int>(text.find("@src")), "Left lands on the mention's start: %d",
          input.CursorPosition());
    input.OnEvent(ftxui::Event::ArrowRight);
    CHECK(input.CursorPosition() == static_cast<int>(text.size()), "Right jumps past it: %d", input.CursorPosition());

    /* Rendering keeps the text (the underline is a style, not a different string). */
    const auto screen = render_twice(input, 40, 4);
    CHECK(row_text(screen, 0, 40) == "see @src/main.c", "row 0: '%s'", row_text(screen, 0, 40).c_str());
}

static void test_input_mode_label()
{
    CHECK(edc::InputModeLabel(EDC_INPUT_TEXT) == "message", "a plain message says message");
    CHECK(edc::InputModeLabel(EDC_INPUT_COMMAND) == "command", "a / command says command");
    CHECK(edc::InputModeLabel(EDC_INPUT_LOCALSHELL) == "local shell", "a ! line says local shell");
    CHECK(edc::InputModeLabel(EDC_INPUT_SHELL) == "shell", "a shell line says shell");
    CHECK(edc::InputModeLabel(EDC_INPUT_REJECTED) == "syntax error", "a rejected line says syntax error");
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
    test_cursor_and_range_replace();
    test_history_recall_up_down();
    test_history_preserves_draft();
    test_history_dedupes_consecutive();
    test_history_edit_leaves_browsing();
    test_history_up_from_lower_row_moves_caret();
    test_history_browsing_flag();
    test_history_clear();
    test_history_prefix_filters_entries();
    test_history_prefix_no_match_keeps_caret();
    test_history_prefix_exact_and_shorter();
    test_paste_block_folds_and_expands();
    test_paste_block_is_one_atom();
    test_paste_block_moves_and_caret();
    test_paste_block_expansion_tracks_edits();
    test_paste_block_broken_placeholder();
    test_paste_block_library();
    test_history_entry_removal();
    test_mention_atom();
    test_input_mode_label();

    if (g_failures)
    {
        std::printf("%d check(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("all EdcInput tests passed\n");
    return 0;
}
