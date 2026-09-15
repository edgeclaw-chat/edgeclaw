// Tests for the Tasks tree's state/active indicator (edc_task_widget.*).
//
// A task's state used to be spelled out on its row ("… COMPLETED"), which cost
// every row as many cells as the longest state name and left the right edge
// ragged. It is now a single marker glyph next to the task id, in one fixed
// column: the shape says whether the task is the *active* one (a block for
// that one, a filled dot for every other) and the color says which state it is
// in. Both used to be separate: the active task wore a `*`, its group's leader
// a `•` (which the group header already implies) and the state a dot far out on
// the right. These tests pin the glyphs, the per-state tint, the active row,
// the shared column and the breath.
#include "edc_style.hpp"
#include "edc_task_widget.hpp"
#include "edc_tui_window.hpp"
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
constexpr const char *kBlock = edc::style::kBlockGlyph; // the working bar's own block

bool IsMarker(const std::string &s)
{
    return s == kDot || s == kBlock;
}

struct TaskSpec
{
    const char *id;
    const char *state;
    const char *desc;
};

// One group holding `specs.size()` tasks, with `active` marked as the active
// task. The states are deliberately given both short and long names, so the
// test can prove the marker's column does not depend on how long the state's
// label used to be.
std::shared_ptr<edc::TaskWidget> MakeWidget(const std::vector<TaskSpec> &specs, size_t active = 0)
{
    edc::TaskGroup group;
    group.group_id = "1";
    group.member_count = static_cast<int>(specs.size());
    group.msg_count = 1;
    for (size_t i = 0; i < specs.size(); i++)
    {
        edc::TaskInfo task;
        task.task_id = specs[i].id;
        task.state = specs[i].state;
        task.description = specs[i].desc;
        // A group only starts expanded when it holds the active task, so there is
        // always one: the test cares about the task rows, not about folding.
        task.is_active = (i == active);
        group.tasks.push_back(std::move(task));
    }

    edc::TaskSnapshot snap;
    snap.session_id = "s";
    snap.session_name = "session";
    snap.groups.push_back(std::move(group));

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

std::string RowText(const ::ftxui::Screen &screen, int y)
{
    std::string row;
    for (int x = 0; x < kW; x++)
    {
        const std::string &cell = screen.CellAt(x, y).character;
        row += cell.empty() ? " " : cell;
    }
    return row;
}

// The marker cells (a block or a dot), in top-to-bottom, left-to-right order:
// one per task row.
std::vector<std::pair<int, int>> MarkerCells(const ::ftxui::Screen &screen)
{
    std::vector<std::pair<int, int>> cells;
    for (int y = 0; y < kH; y++)
        for (int x = 0; x < kW; x++)
            if (IsMarker(screen.CellAt(x, y).character))
                cells.emplace_back(x, y);
    return cells;
}

// The first row (top to bottom) whose text contains `needle`, or -1.
int RowWith(const ::ftxui::Screen &screen, const std::string &needle)
{
    for (int y = 0; y < kH; y++)
        if (RowText(screen, y).find(needle) != std::string::npos)
            return y;
    return -1;
}

// Render `widget` at a given breath phase and return the color of its first
// marker.
::ftxui::Color FirstMarkerColor(edc::TaskWidget &widget, long long breath_ms)
{
    widget.SetBreath(breath_ms);
    const auto screen = Render(widget);
    const auto markers = MarkerCells(screen);
    if (markers.empty())
        return ::ftxui::Color();
    return screen.CellAt(markers[0].first, markers[0].second).foreground_color;
}

void TestMarkerPerStateInItsColor(void)
{
    std::cout << "each task shows one marker, tinted with its state's color\n";

    // Processing states are excluded here: they breathe, so their color is not
    // a fixed value (see TestProcessingMarkerBreathes).
    const std::vector<TaskSpec> specs = {
        {"1", "IDLE", "idle task"},        {"3", "COMPLETED", "done task"},
        {"4", "ERROR", "failed task"},     {"5", "SUSPENDED", "paused task"},
        {"6", "CANCELLED", "dropped task"},
    };
    auto widget = MakeWidget(specs);
    const auto screen = Render(*widget);
    const auto markers = MarkerCells(screen);

    const auto &pal = edc::style::palette();
    const std::vector<::ftxui::Color> expected = {
        pal.warn,       // IDLE (and the active task)
        pal.ok,         // COMPLETED
        pal.error,      // ERROR
        pal.suspended,  // SUSPENDED
        pal.text_dim,   // CANCELLED
    };

    Check(markers.size() == specs.size(), "one marker per task (and nothing else draws one)");
    for (size_t i = 0; i < markers.size() && i < expected.size(); i++)
    {
        const auto &cell = screen.CellAt(markers[i].first, markers[i].second);
        Check(cell.foreground_color == expected[i], std::string("marker for ") + specs[i].state + " wears its state color");
    }
}

void TestSystemGroupGetsItsOwnBlock(void)
{
    std::cout << "system groups draw in their own block (and are never hidden)\n";

    // Two groups, snapshot order = user first (the usual case: the user's group
    // is older). The widget must reorder them: system block on top, user tree
    // under it (the tree is bottom-anchored, so the squeeze hits the system
    // block first and the user's active row keeps its place).
    edc::TaskGroup g_user;
    g_user.group_id = "1";
    g_user.kind = edc::GroupKind::User;
    g_user.member_count = 1;
    {
        edc::TaskInfo task;
        task.task_id = "11";
        task.state = "IDLE";
        task.description = "user work";
        task.is_active = true;
        task.is_leader = true;
        g_user.tasks.push_back(std::move(task));
    }

    edc::TaskGroup g_sys;
    g_sys.group_id = "9";
    g_sys.kind = edc::GroupKind::System;
    g_sys.member_count = 1;
    {
        edc::TaskInfo task;
        task.task_id = "777";
        task.state = "LLM_PROCESSING";
        task.description = "fold group 1";
        task.is_leader = true;
        g_sys.tasks.push_back(std::move(task));
    }

    edc::TaskSnapshot snap;
    snap.session_id = "s";
    snap.session_name = "session";
    snap.groups = {g_user, g_sys};

    auto widget = std::make_shared<edc::TaskWidget>();
    widget->SetSnapshot(&snap);
    const auto screen = Render(*widget);

    const int heading = RowWith(screen, " System");
    const int sys_row = RowWith(screen, "Group 9");
    const int user_row = RowWith(screen, "Group 1");
    const std::string frame = FrameText(screen);

    Check(heading >= 0, "the System heading is drawn");
    Check(sys_row > heading, "the system group sits under its heading");
    Check(user_row > sys_row, "the user tree stays below the system block");
    Check(RowWith(screen, "777") >= 0 && RowWith(screen, "fold group 1") >= 0, "its task row is drawn (nothing is hidden)");
    Check(RowWith(screen, "fold group 1") > 0, "the system task's description is visible");
    Check(frame.find("2 groups") != std::string::npos, "the header counts the system group too");

    // Control 1: the same two groups, both USER -> no System heading and the
    // snapshot's own order (user "1" above "9").
    snap.groups[1].kind = edc::GroupKind::User;
    auto plain = std::make_shared<edc::TaskWidget>();
    plain->SetSnapshot(&snap);
    const auto plain_screen = Render(*plain);
    Check(RowWith(plain_screen, " System") < 0, "no System heading when no group is a system group");
    Check(RowWith(plain_screen, "Group 1") < RowWith(plain_screen, "Group 9"), "user groups keep snapshot order");

    // Control 2: a snapshot with no `kind` (zero) renders exactly like an
    // explicit USER one -- old snapshots are unaffected.
    edc::TaskSnapshot snap_one;
    snap_one.session_id = "s";
    snap_one.session_name = "session";
    snap_one.groups.push_back(g_user);
    edc::TaskSnapshot snap_nokind = snap_one; /* kind 缺省 = User（老快照没有这个键） */

    auto a = std::make_shared<edc::TaskWidget>();
    a->SetSnapshot(&snap_one);
    auto b = std::make_shared<edc::TaskWidget>();
    b->SetSnapshot(&snap_nokind);
    Check(FrameText(Render(*a)) == FrameText(Render(*b)), "an absent kind renders like USER (old snapshots unchanged)");
}

void TestActiveTaskWearsTheBlock(void)
{
    std::cout << "the active task's marker is a block, every other one a dot\n";

    const std::vector<TaskSpec> specs = {
        {"1", "IDLE", "first"}, {"2", "IDLE", "second"}, {"3", "IDLE", "third"},
    };
    // The active task is deliberately not the first row: the block has to follow
    // the flag, not the row order.
    auto widget = MakeWidget(specs, 1);
    const auto screen = Render(*widget);
    const auto markers = MarkerCells(screen);

    Check(markers.size() == 3, "every row still draws a marker");
    if (markers.size() == 3)
    {
        Check(screen.CellAt(markers[0].first, markers[0].second).character == kDot, "an inactive row keeps the dot");
        Check(screen.CellAt(markers[1].first, markers[1].second).character == kBlock, "the active row wears the block");
        Check(screen.CellAt(markers[2].first, markers[2].second).character == kDot, "and the next row is a dot again");
        // The shape is the only difference: the column must not move, or the id
        // column would shift by one cell whenever the active task changes.
        Check(markers[0].first == markers[1].first && markers[1].first == markers[2].first,
              "the block and the dots share one column");
    }
    Check(FrameText(screen).find(kBlock) != std::string::npos, "exactly the one block is drawn");

    // The block is not a second, similar-looking glyph: it is the very one the
    // working bar is built from (WorkingIndicatorElement), so the row the session
    // works on and the bar that says it is working speak the same language.
    {
        auto bar = ::ftxui::Screen::Create(::ftxui::Dimension::Fixed(kW), ::ftxui::Dimension::Fixed(1));
        ::ftxui::Render(bar, edc::WorkingIndicatorElement(0));
        bool bar_uses_block = false;
        for (int x = 0; x < kW; x++)
            if (bar.CellAt(x, 0).character == kBlock)
                bar_uses_block = true;
        Check(bar_uses_block, "and it is the working bar's own block glyph");
    }
}

void TestLeaderMarkerIsGone(void)
{
    std::cout << "the group leader no longer gets a bullet of its own\n";

    // A group's id *is* its leader's task id, so the group header above the tree
    // already names the leader: the bullet was a second, redundant marker.
    edc::TaskGroup group;
    group.group_id = "7";
    group.member_count = 2;
    {
        edc::TaskInfo leader;
        leader.task_id = "7";
        leader.state = "IDLE";
        leader.is_leader = true;
        leader.is_active = true;
        group.tasks.push_back(std::move(leader));

        edc::TaskInfo member;
        member.task_id = "8";
        member.state = "IDLE";
        group.tasks.push_back(std::move(member));
    }
    edc::TaskSnapshot snap;
    snap.session_id = "s";
    snap.groups.push_back(std::move(group));

    edc::TaskWidget widget;
    widget.SetSnapshot(&snap);
    const std::string frame = FrameText(Render(widget));

    Check(frame.find("•") == std::string::npos, "no leader bullet anywhere in the tree");
    Check(frame.find(kBlock) != std::string::npos, "the leader row is marked as active instead");
}

void TestProcessingMarkerBreathes(void)
{
    std::cout << "a processing marker breathes instead of holding one color\n";

    const std::vector<const char *> states = {"LLM_PROCESSING", "TOOL_CALLING"};
    for (const char *state : states)
    {
        auto widget = MakeWidget({{"1", state, "busy"}});
        const auto dim = FirstMarkerColor(*widget, 0);
        const auto rising = FirstMarkerColor(*widget, 450);
        const auto peak = FirstMarkerColor(*widget, 900);
        const auto again = FirstMarkerColor(*widget, 1800);
        Check(dim != peak, std::string(state) + ": the marker changes color over a cycle");
        Check(dim != rising && rising != peak, std::string(state) + ": the rise passes a mid shade");
        Check(dim == again, std::string(state) + ": the breath repeats every period");
        // The top of the cycle is the theme's own accent -- the same blue the
        // section titles and the input prompt wear -- and never anything paler.
        Check(peak == edc::style::palette().accent, std::string(state) + ": the peak is the plain accent");
    }

    // A clock that is momentarily negative (a wall-clock adjustment) must wrap
    // into the same phase, not clamp to a different shade.
    auto widget = MakeWidget({{"1", "LLM_PROCESSING", "busy"}});
    Check(FirstMarkerColor(*widget, -100) == FirstMarkerColor(*widget, 1700), "a negative clock wraps to the same phase");
}

void TestStaticStatesIgnoreBreath(void)
{
    std::cout << "states that are not processing do not breathe\n";

    const auto &pal = edc::style::palette();
    auto widget = MakeWidget({{"1", "COMPLETED", "done"}});
    const auto at_dim = FirstMarkerColor(*widget, 0);
    const auto at_peak = FirstMarkerColor(*widget, 900);
    Check(at_dim == at_peak, "the color is the same at both phases");
    Check(at_dim == pal.ok, "and it is the state's own color");
}

void TestHasProcessingTask(void)
{
    std::cout << "the widget reports whether anything is processing\n";

    auto busy = MakeWidget({{"1", "IDLE", "a"}, {"2", "TOOL_CALLING", "b"}});
    Check(busy->HasProcessingTask(), "a processing task is reported");
    auto idle = MakeWidget({{"1", "IDLE", "a"}, {"2", "COMPLETED", "b"}});
    Check(!idle->HasProcessingTask(), "idle/completed only is not");
}

void TestMarkersSitLeftOfTheId(void)
{
    std::cout << "the marker sits on the left, right before the task id\n";

    const std::vector<TaskSpec> specs = {
        {"1", "IDLE", "short"}, {"2", "LLM_PROCESSING", "a much longer description than the first row"},
    };
    auto widget = MakeWidget(specs, 1);
    const auto screen = Render(*widget);
    const auto markers = MarkerCells(screen);

    Check(markers.size() == 2, "both rows drew a marker");
    if (markers.size() == 2)
        Check(markers[0].first == markers[1].first, "the marker column is fixed, not pushed around by the label length");

    // The whole point of the move: the marker is next to the id, not out on the
    // ragged right edge of the description.
    for (size_t i = 0; i < markers.size() && i < specs.size(); i++)
    {
        const std::string row = RowText(screen, markers[i].second);
        const size_t id_at = row.find(specs[i].id);
        Check(id_at != std::string::npos && static_cast<size_t>(markers[i].first) < id_at,
              std::string("the marker comes before the id on row ") + specs[i].id);
    }
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
    Check(frame.find(kBlock) != std::string::npos && frame.find(kDot) != std::string::npos,
          "the marker is the state's only trace");
    Check(frame.find("busy task") != std::string::npos, "descriptions still render");
}

} // namespace

int main()
{
    TestMarkerPerStateInItsColor();
    TestActiveTaskWearsTheBlock();
    TestLeaderMarkerIsGone();
    TestProcessingMarkerBreathes();
    TestStaticStatesIgnoreBreath();
    TestHasProcessingTask();
    TestMarkersSitLeftOfTheId();
    TestStateNameIsGone();
    TestSystemGroupGetsItsOwnBlock();

    if (g_failures == 0)
    {
        std::cout << "\nall task widget marker tests passed\n";
        return 0;
    }
    std::cout << "\n" << g_failures << " task widget check(s) failed\n";
    return 1;
}
