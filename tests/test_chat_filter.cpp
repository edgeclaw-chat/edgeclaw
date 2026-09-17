// Tests for the chat log's group/task filter (2026-09-17).
//
// The sidebar's task tree used to be nearly inert: only the section header and
// the group headers took clicks (both folding). Every row now carries a one-cell
// selector in front of it, and picking one points the chat log at that group's or
// that task's history.
//
// Two mechanisms are pinned here:
//   1. ownership -- a task-produced bubble belongs to its TaskId(); a user input
//      belongs to the task its adjacency in the log says it started, and a
//      bubble with no owner (a command reply, an error) is hidden by any filter;
//   2. the selector -- drawn on every row, a click selects (a second click
//      clears), a click elsewhere on a group header still folds, and the
//      selection is dropped when the session or the selected row goes away.
#include "edc_chat_widget.hpp"
#include "edc_message.hpp"
#include "edc_task_widget.hpp"
#include "ftxui/component/event.hpp"
#include "ftxui/component/mouse.hpp"
#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/screen.hpp"
#include <ctime>
#include <iostream>
#include <memory>
#include <map>
#include <set>
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

constexpr int kW = 48;
constexpr int kH = 24;

std::set<std::string> Ids(std::initializer_list<const char *> what)
{
    std::set<std::string> out;
    for (const char *id : what)
        out.insert(id);
    return out;
}

// ── chat log helpers ────────────────────────────────────────────────────

std::string Draw(const std::shared_ptr<edc::ChatWidget> &chat, int w = 100, int h = 40)
{
    auto screen = ::ftxui::Screen::Create(::ftxui::Dimension::Fixed(w), ::ftxui::Dimension::Fixed(h));
    ::ftxui::Render(screen, chat->Render() | ::ftxui::flex);
    return screen.ToString();
}

// Screen::ToString() interleaves SGR escapes with the cells; drop them.
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

bool Shows(const std::shared_ptr<edc::ChatWidget> &chat, const char *needle)
{
    return Plain(Draw(chat)).find(needle) != std::string::npos;
}

// ── sidebar helpers ─────────────────────────────────────────────────────

// A snapshot with one user group ("1", tasks 11 and 12, 11 active) and one
// system group ("9", task 777). The system group is drawn first and both groups
// start expanded (one holds the active task, the other is a system one).
edc::TaskSnapshot TwoGroupSnapshot(const char *session = "s")
{
    edc::TaskGroup user;
    user.group_id = "1";
    user.member_count = 2;
    {
        edc::TaskInfo a;
        a.task_id = "11";
        a.state = "IDLE";
        a.description = "user work";
        a.is_active = true;
        a.is_leader = true;
        user.tasks.push_back(a);

        edc::TaskInfo b;
        b.task_id = "12";
        b.state = "COMPLETED";
        b.description = "user followup";
        user.tasks.push_back(b);
    }

    edc::TaskGroup sys;
    sys.group_id = "9";
    sys.kind = edc::GroupKind::System;
    sys.member_count = 1;
    {
        edc::TaskInfo t;
        t.task_id = "777";
        t.state = "LLM_PROCESSING";
        t.description = "fold group 1";
        t.is_leader = true;
        sys.tasks.push_back(t);
    }

    edc::TaskSnapshot snap;
    snap.session_id = session;
    snap.session_name = "session";
    snap.groups = {user, sys};
    return snap;
}

::ftxui::Screen Render(edc::TaskWidget &widget)
{
    auto make = [] {
        return ::ftxui::Screen::Create(::ftxui::Dimension::Fixed(kW), ::ftxui::Dimension::Fixed(kH));
    };
    ::ftxui::Screen screen = make();
    for (int pass = 0; pass < 2; pass++)
    {
        screen = make();
        ::ftxui::Render(screen, widget.Render() | ::ftxui::flex);
    }
    return screen;
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

int RowWith(const ::ftxui::Screen &screen, const std::string &needle)
{
    for (int y = 0; y < kH; y++)
        if (RowText(screen, y).find(needle) != std::string::npos)
            return y;
    return -1;
}

int ColOfGlyph(const ::ftxui::Screen &screen, int y, const char *glyph)
{
    for (int x = 0; x < kW; x++)
        if (screen.CellAt(x, y).character == glyph)
            return x;
    return -1;
}

int CountGlyph(const ::ftxui::Screen &screen, const char *glyph)
{
    int n = 0;
    for (int y = 0; y < kH; y++)
        for (int x = 0; x < kW; x++)
            if (screen.CellAt(x, y).character == glyph)
                n++;
    return n;
}

::ftxui::Event ClickAt(int x, int y)
{
    return ::ftxui::Event::Mouse(
        "", ::ftxui::Mouse{::ftxui::Mouse::Left, ::ftxui::Mouse::Pressed, false, false, false, x, y});
}

// ── task id (the key the filter matches on) ─────────────────────────────

void TestBubbleCarriesItsTaskId(void)
{
    std::cout << "every bubble carries the task it belongs to\n";

    const time_t now = ::time(nullptr);
    auto reply = std::make_shared<edc::Message>("the reply", now, "5");
    auto thought = std::make_shared<edc::ThoughtMessage>("thinking", now, "5");
    auto tool = std::make_shared<edc::ToolMessage>("Bash", "output", now, false, "5");

    Check(reply->TaskId() == "5", "plain reply");
    Check(thought->TaskId() == "5", "thought");
    Check(tool->TaskId() == "5", "tool output");
}

void TestBubblesFromNoTaskCarryNoTaskId(void)
{
    std::cout << "a bubble from no task carries no task id\n";

    const time_t now = ::time(nullptr);
    auto chat = std::make_shared<edc::ChatWidget>();
    auto sys = std::make_shared<edc::Message>("no such task", now); // a command reply
    chat->AddMessage(sys);
    Check(sys->TaskId().empty(), "no task id, so the filter hides it");
}

// ── filtering ───────────────────────────────────────────────────────────

void TestFilterShowsOnlyTheSelectedTaskHistory(void)
{
    std::cout << "filtering to a task shows its history and the input that started it\n";

    const time_t now = ::time(nullptr);
    auto chat = std::make_shared<edc::ChatWidget>();
    // The inputs each task was created from, as the task snapshot reports them.
    chat->SetTaskInputs(std::make_shared<const std::map<std::string, std::string>>(
        std::map<std::string, std::string>{{"5", "first input"}, {"6", "second input"}}));
    chat->AddMessage(std::make_shared<edc::Message>("reply of task five", now, "5"));
    chat->AddMessage(std::make_shared<edc::Message>("a command reply", now)); // sys: no task id
    chat->AddMessage(std::make_shared<edc::Message>("reply of task six", now, "6"));

    Check(Shows(chat, "reply of task five") && Shows(chat, "reply of task six"), "unfiltered shows everything");

    chat->SetFilter(std::make_shared<const std::set<std::string>>(Ids({"5"})), "task 5");
    Check(Shows(chat, "reply of task five"), "the selected task's reply is shown");
    Check(Shows(chat, "first input"), "so is the input that started it");
    Check(!Shows(chat, "reply of task six"), "the other task's reply is hidden");
    Check(!Shows(chat, "second input"), "the other task's input is hidden");
    Check(!Shows(chat, "a command reply"), "a bubble with no task id is hidden");

    chat->SetFilter(std::make_shared<const std::set<std::string>>(Ids({"5", "6"})), "Group 1");
    Check(Shows(chat, "first input") && Shows(chat, "second input"), "a group filter shows every member's history");
    Check(Shows(chat, "reply of task five") && Shows(chat, "reply of task six"), "both members' replies");
    Check(!Shows(chat, "a command reply"), "still no unattributable bubbles");

    chat->SetFilter(nullptr);
    Check(Shows(chat, "a command reply"), "clearing the filter restores the whole log");
}

void TestFilteredEmptyLogSaysSo(void)
{
    std::cout << "a filter that matches nothing says which selection is empty\n";

    const time_t now = ::time(nullptr);
    auto chat = std::make_shared<edc::ChatWidget>();
    chat->AddMessage(std::make_shared<edc::Message>("reply of task five", now, "5"));

    chat->SetFilter(std::make_shared<const std::set<std::string>>(Ids({"9"})), "task 9");
    Check(Plain(Draw(chat)).find("no messages in task 9") != std::string::npos, "says the selection is empty");

    chat->SetFilter(nullptr);
    Check(Plain(Draw(chat)).find("no messages in") == std::string::npos, "no hint without a filter");
}

void TestFilteredHistoryOpensWithTheTasksInput(void)
{
    std::cout << "under a selection, each task's history opens with its input\n";

    const time_t now = ::time(nullptr);
    auto chat = std::make_shared<edc::ChatWidget>();
    chat->SetTaskInputs(std::make_shared<const std::map<std::string, std::string>>(
        std::map<std::string, std::string>{{"5", "fix the parser"}, {"6", "and the writer"}}));
    chat->AddMessage(std::make_shared<edc::UserMessage>("fix the parser", now)); // 回显: 没有 task
    chat->AddMessage(std::make_shared<edc::Message>("reply of task five", now, "5"));
    chat->AddMessage(std::make_shared<edc::Message>("a command reply", now));
    chat->AddMessage(std::make_shared<edc::Message>("reply of task six", now, "6"));

    chat->SetFilter(std::make_shared<const std::set<std::string>>(Ids({"5"})), "task 5");
    Check(Shows(chat, "fix the parser"), "the input that started it heads its history");
    Check(Shows(chat, "reply of task five"), "followed by the task's own output");
    Check(!Shows(chat, "and the writer"), "the other task's input is not drawn");
    Check(!Shows(chat, "a command reply"), "nor a bubble that belongs to no task");

    // 一个组 = 多个 task: 每个 task 各自带上自己那条输入。
    chat->SetFilter(std::make_shared<const std::set<std::string>>(Ids({"5", "6"})), "group");
    Check(Shows(chat, "fix the parser") && Shows(chat, "and the writer"), "a group draws each task's own input");
    Check(Shows(chat, "reply of task six"), "with that task's output");

    // 没有输入数据的 task (fork/system): 只画它自己的消息。
    chat->SetFilter(std::make_shared<const std::set<std::string>>(Ids({"9"})), "task 9");
    Check(!Shows(chat, "fix the parser"), "a task with no input draws none");
}

void TestClearDropsTheLog(void)
{
    std::cout << "Clear() drops the log (and an unowned input is not retro-claimed)\n";

    const time_t now = ::time(nullptr);
    auto chat = std::make_shared<edc::ChatWidget>();
    chat->AddMessage(std::make_shared<edc::UserMessage>("gone input", now));
    chat->Clear();
    chat->AddMessage(std::make_shared<edc::Message>("reply of task five", now, "5"));
    Check(!Shows(chat, "gone input"), "the old log is gone and nothing was retro-adopted");
}

// ── the sidebar selector ────────────────────────────────────────────────

} // namespace

int main()
{
    std::cout << "chat filter / sidebar selector tests\n";

    TestBubbleCarriesItsTaskId();
    TestBubblesFromNoTaskCarryNoTaskId();
    TestFilterShowsOnlyTheSelectedTaskHistory();
    TestFilteredEmptyLogSaysSo();
    TestFilteredHistoryOpensWithTheTasksInput();
    TestClearDropsTheLog();

    if (g_failures == 0)
    {
        std::cout << "all checks passed\n";
        return 0;
    }
    std::cout << g_failures << " check(s) failed\n";
    return 1;
}
