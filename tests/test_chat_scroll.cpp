#include "ftxui/component/component.hpp"
#include "ftxui/component/event.hpp"
#include "ftxui/component/mouse.hpp"
#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/screen.hpp"
#include "tui/edc_chat_widget.hpp"
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>

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

constexpr int kViewport = 10;
constexpr int kMessages = 40;

// Each message renders as a distinct single word so we can tell from the
// rasterised screen which slice of the log is visible.
std::shared_ptr<edc::ChatWidget> MakeChat()
{
    auto chat = std::make_shared<edc::ChatWidget>();
    auto now = time(nullptr);
    for (int i = 0; i < kMessages; i++)
    {
        chat->AddMessage(std::make_shared<edc::Message>("msg" + std::to_string(i), now));
    }
    return chat;
}

std::string Draw(const std::shared_ptr<edc::ChatWidget> &chat)
{
    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(40), ftxui::Dimension::Fixed(kViewport));
    auto element = chat->Render() | ftxui::flex;
    ftxui::Render(screen, element);
    return screen.ToString();
}

bool Visible(const std::string &frame, int index)
{
    const std::string needle = "msg" + std::to_string(index);
    const size_t at = frame.find(needle);
    if (at == std::string::npos)
    {
        return false;
    }
    // "msg1" must not match inside "msg10".
    const char next = at + needle.size() < frame.size() ? frame[at + needle.size()] : ' ';
    return next < '0' || next > '9';
}

} // namespace

// Content taller than the viewport must clip, not overflow.
static void test_overflow_is_clipped(void)
{
    TEST("long log does not render every message at once");
    auto chat = MakeChat();
    const std::string frame = Draw(chat);

    int visible = 0;
    for (int i = 0; i < kMessages; i++)
    {
        visible += Visible(frame, i) ? 1 : 0;
    }
    ASSERT(visible > 0, "nothing rendered");
    ASSERT(visible < kMessages, "all %d messages fit in a %d-line viewport", kMessages, kViewport);
    PASS();
}

// New messages should pin the viewport to the tail.
static void test_sticks_to_bottom(void)
{
    TEST("newest message visible by default");
    auto chat = MakeChat();
    const std::string frame = Draw(chat);

    ASSERT(Visible(frame, kMessages - 1), "last message not visible");
    ASSERT(!Visible(frame, 0), "first message should be scrolled off");
    PASS();
}

static void test_scroll_up_changes_view(void)
{
    TEST("ArrowUp scrolls the viewport");
    auto chat = MakeChat();
    const std::string before = Draw(chat);

    bool handled = false;
    for (int i = 0; i < 5; i++)
    {
        handled |= chat->OnEvent(ftxui::Event::ArrowUp);
    }
    const std::string after = Draw(chat);

    ASSERT(handled, "ArrowUp was not handled");
    ASSERT(before != after, "viewport did not move");
    PASS();
}

static void test_home_reaches_top(void)
{
    TEST("Home scrolls to the first message");
    auto chat = MakeChat();
    Draw(chat); // establish geometry

    ASSERT(chat->OnEvent(ftxui::Event::Home), "Home was not handled");
    const std::string frame = Draw(chat);

    ASSERT(Visible(frame, 0), "first message not visible after Home");
    ASSERT(!Visible(frame, kMessages - 1), "last message should be off-screen at top");
    PASS();
}

static void test_end_reaches_bottom(void)
{
    TEST("End returns to the newest message");
    auto chat = MakeChat();
    Draw(chat);
    chat->OnEvent(ftxui::Event::Home);
    Draw(chat);

    ASSERT(chat->OnEvent(ftxui::Event::End), "End was not handled");
    const std::string frame = Draw(chat);

    ASSERT(Visible(frame, kMessages - 1), "last message not visible after End");
    PASS();
}

static void test_page_scrolls_further_than_line(void)
{
    TEST("PageUp moves further than ArrowUp");
    auto line = MakeChat();
    Draw(line);
    line->OnEvent(ftxui::Event::ArrowUp);
    const std::string line_frame = Draw(line);

    auto page = MakeChat();
    Draw(page);
    ASSERT(page->OnEvent(ftxui::Event::PageUp), "PageUp was not handled");
    const std::string page_frame = Draw(page);

    ASSERT(line_frame != page_frame, "PageUp scrolled the same amount as ArrowUp");
    PASS();
}

static void test_cannot_scroll_past_top(void)
{
    TEST("scrolling up past the top is clamped");
    auto chat = MakeChat();
    Draw(chat);

    for (int i = 0; i < kMessages * 4; i++)
    {
        chat->OnEvent(ftxui::Event::ArrowUp);
    }
    const std::string a = Draw(chat);
    chat->OnEvent(ftxui::Event::ArrowUp);
    const std::string b = Draw(chat);

    ASSERT(Visible(a, 0), "first message not visible at top");
    ASSERT(a == b, "view moved even though already at the top");
    PASS();
}

static void test_cannot_scroll_past_bottom(void)
{
    TEST("scrolling down past the bottom is clamped");
    auto chat = MakeChat();
    Draw(chat);

    for (int i = 0; i < kMessages * 4; i++)
    {
        chat->OnEvent(ftxui::Event::ArrowDown);
    }
    const std::string frame = Draw(chat);

    ASSERT(Visible(frame, kMessages - 1), "last message not visible at bottom");
    PASS();
}

// Scrolled-up readers should not be yanked to the tail by an arriving message.
static void test_new_message_does_not_yank_view(void)
{
    TEST("new message does not disturb a scrolled-up reader");
    auto chat = MakeChat();
    Draw(chat);
    chat->OnEvent(ftxui::Event::Home);
    const std::string before = Draw(chat);

    chat->AddMessage(std::make_shared<edc::Message>("brand new arrival", time(nullptr)));
    const std::string after = Draw(chat);

    ASSERT(before == after, "viewport jumped when a message arrived");
    PASS();
}

// ...but a reader at the tail should follow along.
static void test_new_message_follows_when_at_bottom(void)
{
    TEST("new message is revealed when following the tail");
    auto chat = MakeChat();
    Draw(chat);

    chat->AddMessage(std::make_shared<edc::Message>("zzznewest", time(nullptr)));
    const std::string frame = Draw(chat);

    ASSERT(frame.find("zzznewest") != std::string::npos, "new message not shown while following tail");
    PASS();
}

static void test_wheel_scrolls(void)
{
    TEST("mouse wheel scrolls the viewport");
    auto chat = MakeChat();
    const std::string before = Draw(chat);

    ftxui::Event wheel = ftxui::Event::Mouse("", ftxui::Mouse{ftxui::Mouse::WheelUp, ftxui::Mouse::Pressed, false,
                                                             false, false, 2, 2});
    const bool handled = chat->OnEvent(wheel);
    const std::string after = Draw(chat);

    ASSERT(handled, "wheel event not handled");
    ASSERT(before != after, "wheel did not scroll");
    PASS();
}

static void test_scrollbar_drawn(void)
{
    TEST("scroll indicator is drawn when content overflows");
    auto chat = MakeChat();
    const std::string frame = Draw(chat);

    const bool has_bar = frame.find("┃") != std::string::npos || frame.find("╹") != std::string::npos ||
                         frame.find("╻") != std::string::npos;
    ASSERT(has_bar, "no scrollbar glyph found");
    PASS();
}

static void test_short_log_not_scrollable(void)
{
    TEST("short log renders without scrolling");
    auto chat = std::make_shared<edc::ChatWidget>();
    chat->AddMessage(std::make_shared<edc::Message>("only one", time(nullptr)));

    const std::string before = Draw(chat);
    chat->OnEvent(ftxui::Event::ArrowUp);
    const std::string after = Draw(chat);

    ASSERT(before.find("only one") != std::string::npos, "message not rendered");
    ASSERT(before == after, "short log scrolled when it should not");
    PASS();
}

// An exhausted scroll must not swallow the key, or focus gets trapped in the
// chat log when it sits in a container with other components.
static void test_exhausted_scroll_releases_key(void)
{
    TEST("keys are released once the view cannot move");
    auto chat = MakeChat();
    Draw(chat);

    ASSERT(!chat->OnEvent(ftxui::Event::ArrowDown), "ArrowDown swallowed while already at bottom");
    ASSERT(!chat->OnEvent(ftxui::Event::End), "End swallowed while already at bottom");

    chat->OnEvent(ftxui::Event::Home);
    Draw(chat);
    ASSERT(!chat->OnEvent(ftxui::Event::ArrowUp), "ArrowUp swallowed while already at top");
    PASS();
}

int main()
{
    printf("=== ChatWidget Scrolling Tests ===\n\n");

    test_overflow_is_clipped();
    test_sticks_to_bottom();
    test_scroll_up_changes_view();
    test_home_reaches_top();
    test_end_reaches_bottom();
    test_page_scrolls_further_than_line();
    test_cannot_scroll_past_top();
    test_cannot_scroll_past_bottom();
    test_new_message_does_not_yank_view();
    test_new_message_follows_when_at_bottom();
    test_wheel_scrolls();
    test_scrollbar_drawn();
    test_short_log_not_scrollable();
    test_exhausted_scroll_releases_key();

    printf("\n=== Results: %d/%d tests passed ===\n", tests_passed, tests_run);
    return tests_run == tests_passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
