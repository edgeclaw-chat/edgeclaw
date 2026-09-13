// Tests for the Tasks tree's state indicator (edc_task_widget.*).
//
// A task's state used to be spelled out on its row ("… COMPLETED"), which cost
// every row as many cells as the longest state name and left the right edge
// ragged. It is now a single filled dot in a fixed right-hand column, tinted
// with the state's color: these tests pin the glyph, the per-state tint and
// the shared column.
#include "edc_style.hpp"
#include "edc_task_widget.hpp"
#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/screen.hpp"
#include <iostream>
#include <string>
#include <utility>
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

constexpr int kW = 40;
constexpr int kH = 20;
constexpr const char *kDot = "●";

struct TaskSpec
{
    const char *id;
    const char *state;
    const char *desc;
};

// One group holding `specs.size()` tasks. The states are deliberately given
// both short and long names, so the test can prove the dot's column does not
// depend on how long the state's label used to be.
std::shared_ptr<edc::TaskWidget> MakeWidget(const std::vector<TaskSpec> &specs)
{
    std::vector<edc_c_task_info> infos(specs.size());
    for (size_t i = 0; i < specs.size(); i++)
    {
        infos[i] = edc_c_task_info{};
        infos[i].task_id = specs[i].id;
        infos[i].state = specs[i].state;
        infos[i].description = specs[i].desc;
    }
    // A group only starts expanded when it holds the active task, so mark the
    // first one: the test cares about the task rows, not about folding.
    if (!infos.empty())
        infos[0].is_active = 1;
    edc_c_task_group group = edc_c_task_group{};
    group.group_id = "1";
    group.member_count = static_cast<int>(specs.size());
    group.msg_count = 1;
    group.task_count = static_cast<int>(specs.size());
    group.tasks = infos.data();

    edc_c_task_snapshot snap = edc_c_task_snapshot{};
    snap.session_id = "s";
    snap.session_name = "session";
    snap.group_count = 1;
    snap.groups = &group;

    auto widget = std::make_shared<edc::TaskWidget>();
    widget->SetSnapshot(&snap);
    return widget;
}

// The widget sizes its rows from the box a previous render measured, so the
// first pass only primes it (same two-pass scheme as the todo header tests).
::ftxui::Screen Render(edc::TaskWidget &widget)
{
    auto make_screen = [] {
        return ::ftxui::Screen::Create(::ftxui::Dimension::Fixed(kW), ::ftxui::Dimension::Fixed(kH));
    };
    ::ftxui::Screen screen = make_screen();
    for (int pass = 0; pass < 2; pass++)
    {
        screen = make_screen();
        ::ftxui::Render(screen, widget.Render() | ::ftxui::flex);
    }
    return screen;
}

std::string FrameText(const ::ftxui::Screen &screen)
{
    std::string frame;
    for (int y = 0; y < kH; y++)
    {
        for (int x = 0; x < kW; x++)
        {
            const std::string &cell = screen.CellAt(x, y).character;
            frame += cell.empty() ? " " : cell;
        }
        frame += "\n";
    }
    return frame;
}

// The dot cells, in top-to-bottom, left-to-right order: one per task row.
std::vector<std::pair<int, int>> DotCells(const ::ftxui::Screen &screen)
{
    std::vector<std::pair<int, int>> cells;
    for (int y = 0; y < kH; y++)
        for (int x = 0; x < kW; x++)
            if (screen.CellAt(x, y).character == kDot)
                cells.emplace_back(x, y);
    return cells;
}

void TestDotPerStateInItsColor(void)
{
    std::cout << "each task shows one dot, tinted with its state's color\n";

    const std::vector<TaskSpec> specs = {
        {"1", "IDLE", "idle task"},          {"2", "LLM_PROCESSING", "busy task"},
        {"3", "COMPLETED", "done task"},     {"4", "ERROR", "failed task"},
        {"5", "SUSPENDED", "paused task"},   {"6", "CANCELLED", "dropped task"},
    };
    auto widget = MakeWidget(specs);
    const auto screen = Render(*widget);
    const auto dots = DotCells(screen);

    const auto &pal = edc::style::palette();
    const std::vector<::ftxui::Color> expected = {
        pal.warn,       // IDLE
        pal.accent,     // LLM_PROCESSING
        pal.ok,         // COMPLETED
        pal.error,      // ERROR
        pal.suspended,  // SUSPENDED
        pal.text_dim,   // CANCELLED
    };

    Check(dots.size() == specs.size(), "one dot per task (and nothing else draws one)");
    for (size_t i = 0; i < dots.size() && i < expected.size(); i++)
    {
        const auto &cell = screen.CellAt(dots[i].first, dots[i].second);
        Check(cell.foreground_color == expected[i], std::string("dot for ") + specs[i].state + " wears its state color");
    }
}

void TestDotsShareOneColumn(void)
{
    std::cout << "every dot lands in the same right-hand column\n";

    const std::vector<TaskSpec> specs = {
        {"1", "IDLE", "short"}, {"2", "LLM_PROCESSING", "a much longer description than the first row"},
    };
    auto widget = MakeWidget(specs);
    const auto screen = Render(*widget);
    const auto dots = DotCells(screen);

    Check(dots.size() == 2, "both rows drew a dot");
    if (dots.size() == 2)
        Check(dots[0].first == dots[1].first, "the dot column is fixed, not pushed around by the label length");
}

void TestStateNameIsGone(void)
{
    std::cout << "the state is no longer spelled out\n";

    const std::vector<TaskSpec> specs = {
        {"1", "IDLE", "idle task"}, {"2", "LLM_PROCESSING", "busy task"}, {"3", "COMPLETED", "done task"},
    };
    auto widget = MakeWidget(specs);
    const std::string frame = FrameText(Render(*widget));

    Check(frame.find("IDLE") == std::string::npos, "no IDLE text");
    Check(frame.find("LLM_PROCESSING") == std::string::npos, "no LLM_PROCESSING text");
    Check(frame.find("COMPLETED") == std::string::npos, "no COMPLETED text");
    Check(frame.find(kDot) != std::string::npos, "the dot is the state's only trace");
    Check(frame.find("busy task") != std::string::npos, "descriptions still render");
}

} // namespace

int main()
{
    TestDotPerStateInItsColor();
    TestDotsShareOneColumn();
    TestStateNameIsGone();

    if (g_failures == 0)
    {
        std::cout << "\nall task widget state-dot tests passed\n";
        return 0;
    }
    std::cout << "\n" << g_failures << " task widget check(s) failed\n";
    return 1;
}
