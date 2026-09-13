// Tests for the Todos section (edc_todo_widget.*).
//
// The displayed list belongs to the foreground group's active task, and several
// tasks run in a session, so the header names the one it is showing ("#<id>").
// These tests pin that, and pin that the name never costs the progress summary
// its place: the pane is narrow and the id is the part that gives way.
//
// The list's one accent is the in_progress row, and while an item is running it
// breathes (the shared working animation): the tests pin that highlight moving
// over a cycle and that no other row moves with it.
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
// render measured, so the first pass only primes it.
::ftxui::Screen RenderWidget(edc::TodoWidget &widget, int width)
{
    auto screen = ::ftxui::Screen::Create(::ftxui::Dimension::Fixed(width), ::ftxui::Dimension::Fixed(6));
    for (int pass = 0; pass < 2; pass++)
    {
        screen = ::ftxui::Screen::Create(::ftxui::Dimension::Fixed(width), ::ftxui::Dimension::Fixed(6));
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
// widget at |breath_ms|; false when no such row is on screen.
bool InProgressMarkerColor(edc::TodoWidget &widget, long long breath_ms, ::ftxui::Color *out)
{
    widget.SetBreath(breath_ms);
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
    Check(InProgressMarkerColor(widget, 0, &dim), "the in_progress row is on screen");
    Check(InProgressMarkerColor(widget, 450, &rising), "a mid-rise phase draws");
    Check(InProgressMarkerColor(widget, 900, &peak), "the peak phase draws");
    Check(InProgressMarkerColor(widget, 1800, &again), "the next cycle draws");
    Check(dim != peak, "the highlight changes color over a cycle");
    Check(dim != rising && rising != peak, "the rise passes through a mid shade");
    Check(dim == again, "the breath repeats every period");
}

void TestOtherRowsDoNotBreathe()
{
    std::cout << "rows that are not running do not breathe\n";

    edc::TodoWidget widget;
    widget.SetItems(TwoItems(), "7"); // completed + pending, nothing running
    widget.SetBreath(0);
    const auto dim = RenderWidget(widget, 36);
    widget.SetBreath(900);
    const auto peak = RenderWidget(widget, 36);

    Check(SameColors(dim, peak), "a list with nothing running is identical at both phases");
    Check(!widget.HasInProgressItem(), "and reports nothing in progress");
}

void TestHasInProgressItem()
{
    std::cout << "the widget reports whether an item is running\n";

    edc::TodoWidget busy;
    busy.SetItems(MixedItems(), "7");
    Check(busy.HasInProgressItem(), "an in_progress item is reported");
    edc::TodoWidget idle;
    idle.SetItems(TwoItems(), "7");
    Check(!idle.HasInProgressItem(), "completed/pending only is not");
}

} // namespace

int main()
{
    TestHeaderNamesTheTask();
    TestEmptyListHasNoId();
    TestNarrowPaneKeepsTheSummary();
    TestInProgressRowBreathes();
    TestOtherRowsDoNotBreathe();
    TestHasInProgressItem();

    if (g_failures == 0)
    {
        std::cout << "\nall todo header tests passed\n";
        return 0;
    }
    std::cout << "\n" << g_failures << " todo header check(s) failed\n";
    return 1;
}
