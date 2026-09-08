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

    printf("\n=== Results: %d/%d tests passed ===\n", tests_passed, tests_run);
    return tests_run == tests_passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
