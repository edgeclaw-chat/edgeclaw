// Tests for the Todos section (edc_todo_widget.*).
//
// The displayed list belongs to the foreground group's active task, and several
// tasks run in a session, so the header names the one it is showing ("#<id>").
// These tests pin that, and pin that the name never costs the progress summary
// its place: the pane is narrow and the id is the part that gives way.
//
// The list's one bright row is the in_progress item, and its tint follows the
// state of the task that owns the list -- supplied by the window through the
// same style::TaskState* table the task tree paints its markers with. These
// tests pin the three cases: processing breathes (the shared working
// animation), ERROR wears the failure colour of the task's own marker, and
// anything else sits still in the section accent, so an item left in_progress by
// a task that finished or is waiting for input cannot pulse for ever.
#include "edc_style.hpp"
#include "edc_todo_widget.hpp"
#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/screen.hpp"
#include <iostream>
#include <string>
#include <vector>

namespace
{

int g_failures = 0;

void Check(bool ok, const std::string &what)
{
    if (ok)
    {
        std::cout << "  ok   " << what << "\n";
        return;
    }
    std::cout << "  FAIL " << what << "\n";
    g_failures++;
}

std::vector<edc::TodoWidget::Item> TwoItems()
{
    std::vector<edc::TodoWidget::Item> items(2);
    items[0].content = "first";
    items[0].status = "completed";
    items[1].content = "second";
    items[1].status = "pending";
    return items;
}

// One list with a row of each kind; the middle one is the running item whose
// highlight must breathe.
std::vector<edc::TodoWidget::Item> MixedItems()
{
    std::vector<edc::TodoWidget::Item> items(3);
    items[0].content = "done";
    items[0].status = "completed";
    items[1].content = "running";
    items[1].status = "in_progress";
    items[2].content = "later";
    items[2].status = "pending";
    return items;
}

// The widget wraps its content in an EMPTY border, so its first content row is
// the screen's row 1. Two passes: it sizes its rows from the box a previous
// render measured, so the first pass only primes it. The screen height caps how
// many item rows are on screen (the inset costs one row top and bottom).
::ftxui::Screen RenderWidget(edc::TodoWidget &widget, int width, int height = 6)
{
    auto screen = ::ftxui::Screen::Create(::ftxui::Dimension::Fixed(width), ::ftxui::Dimension::Fixed(height));
    for (int pass = 0; pass < 2; pass++)
    {
        screen = ::ftxui::Screen::Create(::ftxui::Dimension::Fixed(width), ::ftxui::Dimension::Fixed(height));
        ::ftxui::Render(screen, widget.Render() | ::ftxui::flex);
    }
    return screen;
}

std::string RowText(const ::ftxui::Screen &screen, int width)
{
    std::string row;
    for (int x = 0; x < width; x++)
    {
        const std::string &cell = screen.CellAt(x, 1).character;
        row += cell.empty() ? ' ' : cell[0];
    }
    return row;
}

void TestHeaderNamesTheTask()
{
    std::cout << "the header names the task whose list it shows\n";
    edc::TodoWidget widget;
    widget.SetItems(TwoItems(), "7");

    auto screen = RenderWidget(widget, 36);
    const std::string row = RowText(screen, 36);
    const size_t id_at = row.find("#7");

    Check(row.find("Todos") != std::string::npos, "the section title is there");
    Check(id_at != std::string::npos, "the task id is named");
    Check(row.find("1/2") != std::string::npos, "the progress summary keeps its place");
    Check(id_at != std::string::npos && id_at < row.find("1/2"), "the id sits by the title, before the summary");

    // The id wears the section title's own accent, so the header reads as one
    // label: no bright white, and no dimming (which would drain the accent).
    const auto &pal = edc::style::palette();
    Check(id_at != std::string::npos && screen.CellAt(static_cast<int>(id_at), 1).foreground_color == pal.accent,
          "drawn in the same accent as the section title");
    Check(id_at != std::string::npos && !screen.CellAt(static_cast<int>(id_at), 1).dim, "and not dimmed");

    // The progress counter is part of that same label -- it used to be green
    // when complete and gray otherwise, which split the header into two voices.
    const size_t sum_at = row.find("1/2");
    Check(sum_at != std::string::npos && screen.CellAt(static_cast<int>(sum_at), 1).foreground_color == pal.accent,
          "the progress counter is drawn in the section accent");
}

void TestEmptyListHasNoId()
{
    std::cout << "a list with no task behind it, or no items, shows no id\n";
    edc::TodoWidget noTask;
    noTask.SetItems({}, "");

    const std::string row = RowText(RenderWidget(noTask, 36), 36);
    Check(row.find("#") == std::string::npos, "no stray id");
    Check(row.find("0/0") != std::string::npos, "the summary is still drawn");

    // An id with nothing to label is not shown either: the header would be
    // carrying a name for an empty section.
    edc::TodoWidget empty;
    empty.SetItems({}, "9");

    const std::string empty_row = RowText(RenderWidget(empty, 36), 36);
    Check(empty_row.find("#") == std::string::npos, "an empty list names no task");
    Check(empty_row.find("0/0") != std::string::npos, "and still shows 0/0");
}

void TestNarrowPaneKeepsTheSummary()
{
    std::cout << "a narrow pane truncates the id, not the summary\n";
    edc::TodoWidget widget;
    std::vector<edc::TodoWidget::Item> items(1);
    items[0].content = "only";
    items[0].status = "pending";
    widget.SetItems(items, "1234567890123456789012345");

    const std::string row = RowText(RenderWidget(widget, 24), 24);
    Check(row.find("Todos") != std::string::npos, "the title survives");
    Check(row.find("0/1") != std::string::npos, "and so does the summary");
    Check(row.find("#") != std::string::npos, "the id is still named, just shortened");
}

// The foreground color of the "•" in the in_progress marker after rendering the
// widget with its owner task in |owner_state| at clock |breath_ms|; false when
// no such row is on screen.
bool InProgressMarkerColor(edc::TodoWidget &widget, const std::string &owner_state, long long breath_ms,
                           ::ftxui::Color *out)
{
    widget.SetOwnerState(owner_state, breath_ms);
    const auto screen = RenderWidget(widget, 36);
    for (int y = 0; y < 6; y++)
        for (int x = 0; x < 36; x++)
            if (screen.CellAt(x, y).character == "•")
            {
                *out = screen.CellAt(x, y).foreground_color;
                return true;
            }
    return false;
}

// Whether two frames draw every cell in the same color.
bool SameColors(const ::ftxui::Screen &a, const ::ftxui::Screen &b)
{
    for (int y = 0; y < 6; y++)
        for (int x = 0; x < 36; x++)
            if (!(a.CellAt(x, y).foreground_color == b.CellAt(x, y).foreground_color))
                return false;
    return true;
}

void TestInProgressRowBreathes()
{
    std::cout << "the in_progress todo row breathes\n";

    edc::TodoWidget widget;
    widget.SetItems(MixedItems(), "7");

    ::ftxui::Color dim, rising, peak, again;
    Check(InProgressMarkerColor(widget, "LLM_PROCESSING", 0, &dim), "the in_progress row is on screen");
    Check(InProgressMarkerColor(widget, "TOOL_CALLING", 450, &rising),
          "a mid-rise phase draws (a tool run breathes too)");
    Check(InProgressMarkerColor(widget, "LLM_PROCESSING", 900, &peak), "the peak phase draws");
    Check(InProgressMarkerColor(widget, "LLM_PROCESSING", 1800, &again), "the next cycle draws");
    Check(dim != peak, "the highlight changes color over a cycle");
    Check(dim != rising && rising != peak, "the rise passes through a mid shade");
    Check(dim == again, "the breath repeats every period");
}

void TestOtherRowsDoNotBreathe()
{
    std::cout << "rows that are not running do not breathe\n";

    edc::TodoWidget widget;
    widget.SetItems(TwoItems(), "7"); // completed + pending, nothing running
    widget.SetOwnerState("COMPLETED", 0);
    const auto dim = RenderWidget(widget, 36);
    widget.SetOwnerState("COMPLETED", 900);
    const auto peak = RenderWidget(widget, 36);

    Check(SameColors(dim, peak), "a list with nothing running is identical at both phases");
}

void TestStaleInProgressItemDoesNotBreathe()
{
    std::cout << "an in_progress item left behind by a stopped task does not breathe\n";

    // The task errored (or finished, or is waiting for input) and left an
    // in_progress item behind. The list still holds it, but nothing is running,
    // so the row must sit on a still colour instead of pulsing for ever -- and the
    // window must have a reason to stop asking for frames.
    edc::TodoWidget widget;
    widget.SetItems(MixedItems(), "7");

    const auto &pal = edc::style::palette();
    ::ftxui::Color finished_dim, finished_peak;
    Check(InProgressMarkerColor(widget, "COMPLETED", 0, &finished_dim), "the in_progress row is on screen");
    Check(InProgressMarkerColor(widget, "COMPLETED", 900, &finished_peak), "and draws at the peak phase too");
    Check(finished_dim == finished_peak, "the row is identical at both phases (the breath is off)");
    Check(finished_dim == pal.accent, "and it wears the plain accent, not a frozen mid-breath shade");

    // The very same list, with the task running, does breathe: the difference is
    // the task's state, not the items.
    ::ftxui::Color running_dim, running_peak;
    Check(InProgressMarkerColor(widget, "LLM_PROCESSING", 0, &running_dim), "running: dim phase");
    Check(InProgressMarkerColor(widget, "LLM_PROCESSING", 900, &running_peak), "running: peak phase");
    Check(running_dim != running_peak, "a running task's row does breathe");
}

// The x of the first cell of |tag| on row |y|, or -1 when it is not there.
int FindTag(const ::ftxui::Screen &screen, int y, int width, const std::string &tag)
{
    for (int x = 0; x + static_cast<int>(tag.size()) <= width; x++)
    {
        bool hit = true;
        for (size_t i = 0; i < tag.size() && hit; i++)
            hit = screen.CellAt(x + static_cast<int>(i), y).character == std::string(1, tag[i]);
        if (hit)
            return x;
    }
    return -1;
}

// One list with a row of each kind, each carrying the same priority tag.
std::vector<edc::TodoWidget::Item> TaggedItems()
{
    std::vector<edc::TodoWidget::Item> items(3);
    items[0].content = "done one";
    items[0].status = "completed";
    items[0].priority = "high";
    items[1].content = "running one";
    items[1].status = "in_progress";
    items[1].priority = "high";
    items[2].content = "later one";
    items[2].status = "pending";
    items[2].priority = "high";
    return items;
}

// The item rows are the three after the header and the separator.
constexpr int kFirstItemRow = 3;
constexpr int kContentColumn = 6; // 2 indent + "[x] " marker

void TestPriorityIsPartOfTheItem()
{
    std::cout << "the priority tag wears the item's own styling\n";

    edc::TodoWidget widget;
    widget.SetItems(TaggedItems(), "7");
    widget.SetOwnerState("COMPLETED", 0); // nothing running: the in_progress row is the plain accent
    const auto screen = RenderWidget(widget, 36, 8); // tall enough for all three item rows
    const auto &pal = edc::style::palette();
    const std::string tag = "[high]";

    // Completed: struck through, tag included, and unbroken across the gap the
    // flexible content leaves before it. A tag that stayed unstruck would read as
    // a separate thing parked on the row.
    {
        const int y = kFirstItemRow;
        const int tag_x = FindTag(screen, y, 36, tag);
        Check(tag_x > 0, "the completed row docks its tag");
        if (tag_x > 0)
        {
            const auto &text_cell = screen.CellAt(kContentColumn, y);
            const auto &tag_cell = screen.CellAt(tag_x, y);
            Check(tag_cell.strikethrough, "and the tag is struck through with the text");
            Check(tag_cell.foreground_color == text_cell.foreground_color, "the tag wears the text's colour");
            Check(tag_cell.dim == text_cell.dim, "and the text's dim flag");

            int last_glyph = tag_x - 1;
            while (last_glyph > kContentColumn && screen.CellAt(last_glyph, y).character == " ")
                last_glyph--;
            bool unbroken = true;
            for (int x = last_glyph; x < tag_x + static_cast<int>(tag.size()); x++)
                if (!screen.CellAt(x, y).strikethrough)
                    unbroken = false;
            Check(unbroken, "the strike runs unbroken from the last word into the tag");
        }
    }

    // In progress: the row's single accent covers the tag too (not the dim gray it
    // used to wear, which split the highlighted row in two).
    {
        const int y = kFirstItemRow + 1;
        const int tag_x = FindTag(screen, y, 36, tag);
        Check(tag_x > 0, "the in_progress row docks its tag");
        if (tag_x > 0)
        {
            const auto &text_cell = screen.CellAt(kContentColumn, y);
            const auto &tag_cell = screen.CellAt(tag_x, y);
            Check(text_cell.foreground_color == pal.accent, "the in_progress row's text is the accent");
            Check(tag_cell.foreground_color == text_cell.foreground_color, "the tag is highlighted with it");
            Check(!tag_cell.dim && tag_cell.dim == text_cell.dim, "and is not dimmed away from the text");
            Check(!tag_cell.strikethrough, "and carries no strike");
        }
    }

    // Pending: still the quiet neutral row, tag included (nothing changed here).
    {
        const int y = kFirstItemRow + 2;
        const int tag_x = FindTag(screen, y, 36, tag);
        Check(tag_x > 0, "the pending row docks its tag");
        if (tag_x > 0)
        {
            const auto &text_cell = screen.CellAt(kContentColumn, y);
            const auto &tag_cell = screen.CellAt(tag_x, y);
            Check(tag_cell.foreground_color == text_cell.foreground_color, "the tag wears the text's colour");
            Check(tag_cell.dim && !tag_cell.strikethrough, "dim, not struck");
        }
    }
}

void TestFailedOwnerTurnsTheStepRed()
{
    std::cout << "a failed task turns its current step the failure colour\n";

    // The semantics the user asked for: the tree marks a failed task in red, and
    // the step the session was on must agree -- a run that died must not leave its
    // last step looking merely "current". The colour is not spelled out here: it
    // is the tree's own ERROR entry in the shared table, so the two panes cannot
    // drift into two different reds.
    edc::TodoWidget widget;
    widget.SetItems(TaggedItems(), "7"); // a tagged row of each kind
    const auto &pal = edc::style::palette();
    const auto failure = edc::style::TaskStateColor(pal, "ERROR");
    Check(failure == pal.error, "the failure colour is the palette's error role");

    ::ftxui::Color mark;
    Check(InProgressMarkerColor(widget, "ERROR", 0, &mark), "the in_progress row is on screen");
    Check(mark == failure, "its marker wears the task tree's ERROR colour");
    Check(mark != pal.accent, "so it no longer reads as just the current step");
    Check(!edc::style::TaskStateProcessing("ERROR"), "and a failed task is not processing");

    // Everything the row owns follows: the text and the priority tag are drawn
    // through the same style_content(), and the whole row is one colour.
    const auto screen = RenderWidget(widget, 36, 8);
    const int y = kFirstItemRow + 1; // the in_progress row
    const int tag_x = FindTag(screen, y, 36, "[high]");
    Check(screen.CellAt(kContentColumn, y).foreground_color == failure, "its text is the failure colour too");
    Check(tag_x > 0 && screen.CellAt(tag_x, y).foreground_color == failure, "and so is its priority tag");

    // The other rows keep their own styling: only the current step is repainted.
    Check(screen.CellAt(kContentColumn, kFirstItemRow).foreground_color == pal.text_dim,
          "the completed row above stays neutral");
    Check(screen.CellAt(kContentColumn, kFirstItemRow + 2).foreground_color == pal.text_dim,
          "the pending row below stays neutral");

    // A failed task is not running, so the row is still: two phases, one frame.
    const auto dim = [&] { widget.SetOwnerState("ERROR", 0); return RenderWidget(widget, 36, 8); }();
    const auto peak = [&] { widget.SetOwnerState("ERROR", 900); return RenderWidget(widget, 36, 8); }();
    Check(SameColors(dim, peak), "and it does not pulse (nothing is running)");
}

void TestPriorityBreathesWithTheRow()
{
    std::cout << "the tag breathes with the row it belongs to\n";

    edc::TodoWidget widget;
    widget.SetItems(TaggedItems(), "7");

    ::ftxui::Color text_dim, tag_dim, text_peak, tag_peak;
    for (int phase = 0; phase < 2; phase++)
    {
        const long long ms = phase == 0 ? 0 : 900;
        widget.SetOwnerState("LLM_PROCESSING", ms);
        const auto screen = RenderWidget(widget, 36, 8);
        const int y = kFirstItemRow + 1; // the running row
        const int tag_x = FindTag(screen, y, 36, "[high]");
        if (tag_x <= 0)
        {
            Check(false, "the tag is on screen");
            return;
        }
        if (phase == 0)
        {
            text_dim = screen.CellAt(kContentColumn, y).foreground_color;
            tag_dim = screen.CellAt(tag_x, y).foreground_color;
        }
        else
        {
            text_peak = screen.CellAt(kContentColumn, y).foreground_color;
            tag_peak = screen.CellAt(tag_x, y).foreground_color;
        }
        Check(screen.CellAt(kContentColumn, y).foreground_color == screen.CellAt(tag_x, y).foreground_color,
              phase == 0 ? "at the dim phase the tag matches the text" : "at the peak phase too");
    }
    // The pair moves: the comparison above would also hold if both were frozen.
    Check(text_dim != text_peak && tag_dim != tag_peak, "and both move over the cycle");
    Check(text_peak == edc::style::palette().accent, "the peak is the plain accent");
    Check(tag_dim == text_dim, "the tag is the text's colour at every phase");
}

} // namespace

int main()
{
    TestHeaderNamesTheTask();
    TestEmptyListHasNoId();
    TestNarrowPaneKeepsTheSummary();
    TestInProgressRowBreathes();
    TestOtherRowsDoNotBreathe();
    TestStaleInProgressItemDoesNotBreathe();
    TestPriorityIsPartOfTheItem();
    TestFailedOwnerTurnsTheStepRed();
    TestPriorityBreathesWithTheRow();

    if (g_failures == 0)
    {
        std::cout << "\nall todo header tests passed\n";
        return 0;
    }
    std::cout << "\n" << g_failures << " todo header check(s) failed\n";
    return 1;
}
