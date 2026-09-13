// Regression tests for the chat scroll viewport.
//
// History: scrolling down stopped working once the log contained content that
// wraps (long prose). ftxui runs its layout several times per frame, and
// wrapping flexbox text reports a *different* content height on each pass
// while it converges (observed 20 rows then 52 rows for the same content).
// The scroll offset used to be clamped inside SetBox, i.e. against the first,
// transient measurement of every frame, so each ArrowDown/WheelDown was
// undone before it could be drawn: the viewport never advanced past the top
// screenful and the tail of the last message stayed hidden. The offset is now
// clamped once, from ScrollNode::Render, after layout has settled.
#include "edc_chat_widget.hpp"
#include "edc_scroll_view.hpp"
#include "edc_sidebar.hpp"
#include "edc_task_widget.hpp"
#include "edc_todo_widget.hpp"
#include "ftxui/component/component.hpp"
#include "ftxui/component/event.hpp"
#include "ftxui/component/mouse.hpp"
#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/screen.hpp"
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <vector>

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name)                                                                                                     \
    do                                                                                                                 \
    {                                                                                                                  \
        tests_run++;                                                                                                   \
        printf("[%3d] %-55s ", tests_run, name);                                                                       \
    } while (0)
#define PASS()                                                                                                         \
    do                                                                                                                 \
    {                                                                                                                  \
        tests_passed++;                                                                                                \
        printf("PASS\n");                                                                                              \
    } while (0)
#define FAIL(fmt, ...) printf("FAIL: " fmt "\n", ##__VA_ARGS__)
#define ASSERT(cond, fmt, ...)                                                                                         \
    if (!(cond))                                                                                                       \
    {                                                                                                                  \
        FAIL(fmt, ##__VA_ARGS__);                                                                                      \
        return;                                                                                                        \
    }                                                                                                                  \
    else                                                                                                               \
        do                                                                                                             \
        {                                                                                                              \
        } while (0)

namespace
{

constexpr int kW = 100;
constexpr int kH = 25;

std::string Draw(const std::shared_ptr<edc::ChatWidget> &chat)
{
    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(kW), ftxui::Dimension::Fixed(kH));
    ftxui::Render(screen, chat->Render() | ftxui::flex);
    return screen.ToString();
}

bool AnyRowContains(const std::string &frame, const char *needle)
{
    return frame.find(needle) != std::string::npos;
}

// One screen row of a rendered frame, without its line ending. The sidebar test
// uses it to tell "the header row" from "the rows under it".
std::string LineOf(const std::string &frame, int row)
{
    int current = 0;
    size_t start = 0;
    while (start <= frame.size())
    {
        size_t eol = frame.find('\n', start);
        if (eol == std::string::npos)
            eol = frame.size();
        if (current == row)
        {
            std::string line = frame.substr(start, eol - start);
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            return line;
        }
        current++;
        start = eol + 1;
    }
    return "";
}

// Compare two frames ignoring the scroll-indicator column on the right: the
// thumb legitimately moves when content is added (the log grew, so the bar
// gets shorter), but the reader's slice of the messages must not.
std::string StripIndicator(const std::string &frame)
{
    std::string out;
    int col = 0;
    for (char c : frame)
    {
        if (c == '\n')
        {
            out += c;
            col = 0;
        }
        else
        {
            if (col < kW - 1)
                out += c;
            col++;
        }
    }
    return out;
}

// Chat whose last message is long prose, so the log overflows and the prose
// wraps into dozens of rows. Before the fix this content made the content
// height unstable across layout passes and locked the viewport to the top.
std::shared_ptr<edc::ChatWidget> MakeWrappingChat()
{
    auto chat = std::make_shared<edc::ChatWidget>();
    auto now = ::time(nullptr);
    for (int i = 0; i < 4; i++)
        chat->AddMessage(std::make_shared<edc::Message>("short message number " + std::to_string(i), now));

    std::string words;
    for (int i = 0; i < 400; i++)
        words += "word" + std::to_string(i) + " ";
    chat->AddMessage(std::make_shared<edc::Message>(words, now));
    return chat;
}

// A log of short single-line messages whose last one is a tall fenced block.
std::shared_ptr<edc::ChatWidget> MakeTallChat()
{
    auto chat = std::make_shared<edc::ChatWidget>();
    auto now = ::time(nullptr);
    for (int i = 0; i < 6; i++)
        chat->AddMessage(std::make_shared<edc::Message>("msg " + std::to_string(i), now));

    std::string body;
    for (int i = 0; i < 60; i++)
        body += "TAIL-" + std::to_string(i) + "\n";
    body += "TAIL-BOTTOMLINE";
    chat->AddMessage(std::make_shared<edc::Message>("```\n" + body + "\n```", now));
    return chat;
}

struct DownResult
{
    int handled = 0;
    int first_unhandled = -1;
};

// Press `key` `count` times with a render between presses, as the real event
// loop does, so each press sees the geometry the previous press produced.
DownResult Press(const std::shared_ptr<edc::ChatWidget> &chat, ::ftxui::Event key, int count)
{
    DownResult r;
    for (int i = 0; i < count; i++)
    {
        bool handled = chat->OnEvent(key);
        if (!handled)
        {
            if (r.first_unhandled < 0)
                r.first_unhandled = i;
        }
        else
        {
            r.handled++;
        }
        (void)Draw(chat);
    }
    return r;
}

void ScrollToTop(const std::shared_ptr<edc::ChatWidget> &chat)
{
    for (int i = 0; i < 500; i++)
        chat->OnEvent(ftxui::Event::ArrowUp);
    (void)Draw(chat);
}

// Wrapping content must let the reader reach the tail again after scrolling
// up. (The regression: every ArrowDown was handled but the offset was reset
// to the top by the transient layout pass, so the last line stayed hidden.)
static void test_wrapping_scroll_down_reaches_bottom(void)
{
    TEST("arrow-down reaches the tail of wrapping content");
    auto chat = MakeWrappingChat();
    (void)Draw(chat);

    ScrollToTop(chat);
    DownResult r = Press(chat, ftxui::Event::ArrowDown, 1000);

    const std::string final_frame = Draw(chat);
    ASSERT(r.first_unhandled >= 0, "down-scroll never hit the bottom");
    ASSERT(AnyRowContains(final_frame, "word399"), "last word of the wrapping message never became visible");
    PASS();
}

static void test_wrapping_wheel_reaches_bottom(void)
{
    TEST("wheel-down reaches the tail of wrapping content");
    auto chat = MakeWrappingChat();
    (void)Draw(chat);
    ScrollToTop(chat);

    auto wheel_down = ftxui::Event::Mouse(
        "", ftxui::Mouse{ftxui::Mouse::WheelDown, ftxui::Mouse::Pressed, false, false, false, 5, 5});
    DownResult r = Press(chat, wheel_down, 1000);

    const std::string final_frame = Draw(chat);
    ASSERT(r.first_unhandled >= 0, "wheel never hit the bottom");
    ASSERT(AnyRowContains(final_frame, "word399"), "last word of the wrapping message never became visible");
    PASS();
}

static void test_tail_message_fully_visible_when_pinned(void)
{
    TEST("last message is fully revealed at the bottom");
    auto chat = MakeTallChat();
    const std::string frame = Draw(chat);

    ASSERT(AnyRowContains(frame, "TAIL-BOTTOMLINE"), "last line of the last message not visible at bottom");
    PASS();
}

static void test_cannot_scroll_past_top_with_wrapping(void)
{
    TEST("wrapping content still clamps at the top");
    auto chat = MakeWrappingChat();
    (void)Draw(chat);
    ScrollToTop(chat);

    const std::string a = Draw(chat);
    chat->OnEvent(ftxui::Event::ArrowUp);
    const std::string b = Draw(chat);
    ASSERT(AnyRowContains(a, "short message number 0"), "top of the log not visible at offset 0");
    ASSERT(a == b, "view moved even though already at the top");
    PASS();
}

static void test_new_message_follows_tail_after_fix(void)
{
    TEST("new message is revealed while following the tail");
    auto chat = MakeWrappingChat();
    (void)Draw(chat); // pin to the tail

    chat->AddMessage(std::make_shared<edc::Message>("zzznewest", ::time(nullptr)));
    const std::string frame = Draw(chat);

    ASSERT(AnyRowContains(frame, "zzznewest"), "new message not shown while following the tail");
    PASS();
}

static void test_new_message_does_not_disturb_reader(void)
{
    TEST("new message does not disturb a scrolled-up reader");
    auto chat = MakeWrappingChat();
    (void)Draw(chat);
    ScrollToTop(chat);
    // The thumb at the right edge legitimately changes when the log grows
    // below the viewport, so compare everything but the indicator column: a
    // scrolled-up reader's slice of the messages must stay byte-identical
    // when a message arrives.
    const std::string before = StripIndicator(Draw(chat));

    chat->AddMessage(std::make_shared<edc::Message>("arrival", ::time(nullptr)));
    const std::string after = StripIndicator(Draw(chat));

    ASSERT(before == after, "viewport jumped when a message arrived for a scrolled-up reader");
    PASS();
}

// The chat log scrolls by mouse wheel only: it must stay out of the keyboard
// focus cycle so ↑/↓ always reach the message editor (caret move / history
// recall) and are never consumed to scroll the log.
static void test_chat_is_not_keyboard_focusable(void)
{
    TEST("chat is not keyboard focusable (wheel-only scrolling)");
    auto chat = std::make_shared<edc::ChatWidget>();
    ASSERT(!chat->Focusable(), "ChatWidget must stay out of the keyboard focus cycle");
    PASS();
}

// A TaskWidget holding 24 collapsed groups: taller than any viewport the tests
// below hand it, so it always has something to scroll.
std::shared_ptr<edc::TaskWidget> MakeTallTaskWidget()
{
    std::vector<std::string> group_ids;
    std::vector<std::string> task_ids;
    for (int i = 0; i < 24; i++)
    {
        group_ids.push_back("g" + std::to_string(i));
        task_ids.push_back("t" + std::to_string(i));
    }
    std::vector<edc_c_task_info> infos(24);
    std::vector<edc_c_task_group> groups(24);
    for (int i = 0; i < 24; i++)
    {
        infos[i] = edc_c_task_info{};
        infos[i].task_id = task_ids[i].c_str();
        infos[i].state = "COMPLETED";
        infos[i].description = "";
        groups[i] = edc_c_task_group{};
        groups[i].group_id = group_ids[i].c_str();
        groups[i].member_count = 1;
        groups[i].msg_count = 1;
        groups[i].task_count = 1;
        groups[i].tasks = &infos[i];
    }
    edc_c_task_snapshot snap{};
    snap.session_id = "s";
    snap.session_name = "session";
    snap.group_count = 24;
    snap.groups = groups.data();

    auto task = std::make_shared<edc::TaskWidget>();
    task->SetSnapshot(&snap);
    return task;
}


// The same tall tree, but with the session's active task at the very end: the
// sidebar anchors this widget's viewport to its bottom (follow_tail), so the
// row the window is pointed at is the last one the tree draws.
std::shared_ptr<edc::TaskWidget> MakeDeepActiveTaskWidget()
{
    std::vector<std::string> group_ids;
    std::vector<std::string> task_ids;
    const int kGroups = 24;
    for (int i = 0; i < kGroups; i++)
    {
        group_ids.push_back("g" + std::to_string(i));
        task_ids.push_back("t" + std::to_string(i));
    }
    std::vector<edc_c_task_info> infos(kGroups);
    std::vector<edc_c_task_group> groups(kGroups);
    for (int i = 0; i < kGroups; i++)
    {
        infos[i] = edc_c_task_info{};
        infos[i].task_id = task_ids[i].c_str();
        infos[i].state = "COMPLETED";
        infos[i].description = "";
        infos[i].is_active = (i == kGroups - 1); // the newest task is the one running
        groups[i] = edc_c_task_group{};
        groups[i].group_id = group_ids[i].c_str();
        groups[i].member_count = 1;
        groups[i].msg_count = 1;
        groups[i].task_count = 1;
        groups[i].tasks = &infos[i];
    }
    edc_c_task_snapshot snap{};
    snap.session_id = "s";
    snap.session_name = "session";
    snap.group_count = kGroups;
    snap.groups = groups.data();

    auto task = std::make_shared<edc::TaskWidget>();
    task->SetSnapshot(&snap);
    return task;
}

// The frame without ANSI styling. These assertions are about which rows are on
// screen, not their colours -- and whether ftxui emits colour depends on the
// terminal it thinks it is talking to, which a test process has none of.
std::string Plain(const std::string &frame)
{
    std::string out;
    for (size_t i = 0; i < frame.size(); i++)
    {
        if (frame[i] != 0x1b)
        {
            out += frame[i];
            continue;
        }
        while (i < frame.size() && !isalpha(static_cast<unsigned char>(frame[i])))
            i++;
    }
    return out;
}

// The sidebar as the application builds it: edc::MakeSidebar is the very factory
// TUIWindow uses, so these assertions pin the real layout rather than a copy of it.
struct SidebarHarness
{
    edc::Sidebar sidebar;

    // The ScrollViews commit their offset during the render pass, so the frame a
    // fresh harness draws is provisional (in the TUI the next frame corrects it and
    // nobody sees it). Drawing a few times is what a running window does.
    std::string Draw(int width, int height)
    {
        std::string frame;
        for (int pass = 0; pass < 3; pass++)
        {
            auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(width), ftxui::Dimension::Fixed(height));
            ftxui::Render(screen, sidebar.pane->Render() | ftxui::flex);
            frame = screen.ToString();
        }
        return frame;
    }
};

// The sidebar's Tasks section is split in two: everything above the group rows
// -- the "Tasks" header row, the session meta row and the rule under them -- is
// pinned above a ScrollView that owns the list, so a wheel moves the tree
// without taking the section's heading with it. The pinned part stays part of
// the view, too: a wheel over it scrolls the tree instead of hitting dead space.
void test_pinned_section_header_stays_put()
{
    TEST("pinned Tasks heading does not scroll with the tree");

    auto task = MakeTallTaskWidget();

    auto header = ftxui::Renderer(task, [task] { return task->RenderHeader(); });
    auto body = ftxui::Renderer(task, [task] { return task->RenderBody(); });
    auto view = std::make_shared<edc::ScrollView>(body,
                                                  edc::ScrollOptions{edc::ScrollMode::kFitContent,
                                                                     /*show_indicator=*/false, /*follow_tail=*/false,
                                                                     /*focusable=*/false},
                                                  header);
    // The sidebar wraps the section in its own EMPTY border, which puts the
    // header row on screen row 1 -- where the wheel is aimed below.
    auto pane = ftxui::Renderer(
        view, [view] { return view->Render() | ftxui::borderStyled(ftxui::BorderStyle::EMPTY) | ftxui::flex; });

    const int width = 40;
    const int height = 14;
    auto draw = [&] {
        auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(width), ftxui::Dimension::Fixed(height));
        ftxui::Render(screen, pane->Render() | ftxui::flex);
        return screen.ToString();
    };

    const std::string before = draw();
    // Rows 1..3 are the pinned part -- the section title, the session meta row
    // and the rule under them (row 0 is the section's EMPTY border).
    const std::string title_before = LineOf(before, 1);
    const std::string meta_before = LineOf(before, 2);
    const std::string rule_before = LineOf(before, 3);
    ASSERT(title_before.find("Tasks") != std::string::npos, "no section title to check");
    ASSERT(meta_before.find("session") != std::string::npos, "no session meta row to check");
    ASSERT(rule_before.find("─") != std::string::npos, "no rule to check");
    ASSERT(before.find("Group g0") != std::string::npos, "the tree does not start at the first group");

    // Wheel over the pinned heading itself.
    auto wheel_down = ftxui::Event::Mouse(
        "", ftxui::Mouse{ftxui::Mouse::WheelDown, ftxui::Mouse::Pressed, false, false, false, 5, 1});
    view->OnEvent(wheel_down);
    const std::string after = draw();

    ASSERT(LineOf(after, 1) == title_before, "the section title moved with the tree");
    ASSERT(LineOf(after, 2) == meta_before, "the session meta row moved with the tree");
    ASSERT(LineOf(after, 3) == rule_before, "the rule moved with the tree");
    ASSERT(after.find("Group g0") == std::string::npos, "the tree did not scroll under the pinned heading");
    PASS();
}

// A sidebar too short for both sections used to hand the todo list everything it
// asked for -- ftxui's flex_shrink starts at 0, so the todo section never gave a
// row back and the task tree (which does shrink) was squeezed until its own
// section header was gone. Both sections are fit-content ScrollViews now, so a
// short pane is shared, and the tree is anchored to its bottom: what leaves the
// window is the oldest rows, never the row the session is working on.
void test_squeezed_sidebar_keeps_the_working_row_and_both_headings()
{
    TEST("a squeezed sidebar keeps the active task's row and both section headings");

    auto task = MakeDeepActiveTaskWidget();
    auto todo = std::make_shared<edc::TodoWidget>();
    std::vector<edc::TodoWidget::Item> items(8);
    for (int i = 0; i < 8; i++)
    {
        items[i].content = "TODO-" + std::to_string(i);
        items[i].status = i < 3 ? "completed" : (i == 3 ? "in_progress" : "pending");
    }
    todo->SetItems(items, "t23");
    SidebarHarness sidebar;
    sidebar.sidebar = edc::MakeSidebar(task, todo);

    const int width = 40;
    const int height = 16;
    const std::string plain = Plain(sidebar.Draw(width, height));

    ASSERT(plain.find("■ t23") != std::string::npos, "the active task's row was pushed out of the pane");
    ASSERT(plain.find("Group g0") == std::string::npos, "the oldest rows did not scroll out: the top was kept");
    ASSERT(plain.find("Tasks") != std::string::npos, "the pinned Tasks heading was squeezed out");
    ASSERT(plain.find("Todos") != std::string::npos, "the pinned Todos heading was squeezed out");

    // At a pane this short the list keeps its natural height: the *tree* is the one
    // that gives rows back (the cap is a ceiling, not a target, and the list is not
    // squeezed together with the tree as it would be if both were flexible).
    // With a little more room both sections are readable, and the todo list's own
    // rows are not lost either: it keeps its first rows and scrolls the rest.
    const int roomier = height + 8;
    const std::string roomier_frame = Plain(sidebar.Draw(width, roomier));
    ASSERT(roomier_frame.find("TODO-0") != std::string::npos, "the todo list lost its first row");
    auto wheel_down = ftxui::Event::Mouse(
        "", ftxui::Mouse{ftxui::Mouse::WheelDown, ftxui::Mouse::Pressed, false, false, false, 5, roomier - 3});
    for (int i = 0; i < 8; i++)
        sidebar.sidebar.todos->OnEvent(wheel_down);
    const std::string scrolled = Plain(sidebar.Draw(width, roomier));
    ASSERT(scrolled.find("TODO-7") != std::string::npos || scrolled.find("TODO-6") != std::string::npos,
           "the todo list's hidden rows are not reachable by scrolling");
    ASSERT(scrolled.find("■ t23") != std::string::npos,
           "scrolling the todos disturbed the task tree's bottom anchor");

    // And the tree's own rows above stay reachable too: scrolling up releases the
    // bottom anchor, so the older groups can be read.
    auto wheel_up = ftxui::Event::Mouse(
        "", ftxui::Mouse{ftxui::Mouse::WheelUp, ftxui::Mouse::Pressed, false, false, false, 5, 4});
    for (int i = 0; i < 20; i++)
        sidebar.sidebar.tasks->OnEvent(wheel_up);
    ASSERT(Plain(sidebar.Draw(width, roomier)).find("Group g0") != std::string::npos,
           "scrolling up did not release the bottom anchor");
    PASS();
}

} // namespace

int main()
{
    printf("=== ChatWidget Scroll Regression Tests ===\n\n");

    test_wrapping_scroll_down_reaches_bottom();
    test_wrapping_wheel_reaches_bottom();
    test_tail_message_fully_visible_when_pinned();
    test_cannot_scroll_past_top_with_wrapping();
    test_new_message_follows_tail_after_fix();
    test_new_message_does_not_disturb_reader();
    test_chat_is_not_keyboard_focusable();
    test_pinned_section_header_stays_put();
    test_squeezed_sidebar_keeps_the_working_row_and_both_headings();

    printf("\n=== Results: %d/%d tests passed ===\n", tests_passed, tests_run);
    return tests_run == tests_passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
