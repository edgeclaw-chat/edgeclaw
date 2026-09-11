// Regression tests for the virtualized chat log (edc_message_list.*).
//
// History: the log used to be a plain ftxui component container holding one
// child per message, so every frame rebuilt, laid out and drew the whole log.
// With the working animation asking for a frame every 15ms that made the UI
// freeze once a conversation grew past a few hundred messages (measured ~0.5ms
// per message per frame in a fastbuild). The list now measures each item once
// per width and, per frame, only lays out the items the viewport covers.
//
// These tests pin down the two halves of that contract: the per-frame work must
// stay bounded regardless of log length, and the geometry the reader sees
// (tail pinning, clamping, hit-testing, re-measuring on resize) must stay
// exactly what a fully rendered log produced.
#include "edc_chat_widget.hpp"
#include "edc_message.hpp"
#include "ftxui/component/event.hpp"
#include "ftxui/component/mouse.hpp"
#include "ftxui/dom/elements.hpp"
#include "ftxui/dom/selection.hpp"
#include "ftxui/screen/color.hpp"
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

std::string Draw(const std::shared_ptr<edc::ChatWidget> &chat, int w = kW, int h = kH)
{
    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(w), ftxui::Dimension::Fixed(h));
    ftxui::Render(screen, chat->Render() | ftxui::flex);
    return screen.ToString();
}

// Screen::ToString() interleaves SGR escapes with the cells; drop them so the
// helpers below can look for plain text.
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

bool AnyRowContains(const std::string &frame, const char *needle)
{
    return Plain(frame).find(needle) != std::string::npos;
}

// 0-based row holding |needle|, or -1. Screen rows and frame lines line up
// because the chat widget is the only thing on the fixed-size screen, so this
// doubles as the y coordinate to click on.
int RowOf(const std::string &frame, const char *needle)
{
    const std::string plain = Plain(frame);
    int row = 0;
    size_t pos = 0;
    while (pos <= plain.size())
    {
        const size_t end = plain.find('\n', pos);
        const std::string line = end == std::string::npos ? plain.substr(pos) : plain.substr(pos, end - pos);
        if (line.find(needle) != std::string::npos)
            return row;
        if (end == std::string::npos)
            break;
        row++;
        pos = end + 1;
    }
    return -1;
}

::ftxui::Event ClickAt(int x, int y)
{
    return ::ftxui::Event::Mouse(
        "", ::ftxui::Mouse{::ftxui::Mouse::Left, ::ftxui::Mouse::Pressed, false, false, false, x, y});
}

::ftxui::Event WheelDownAt(int x, int y)
{
    return ::ftxui::Event::Mouse(
        "", ::ftxui::Mouse{::ftxui::Mouse::WheelDown, ::ftxui::Mouse::Pressed, false, false, false, x, y});
}

// Counts how many times its body is asked for, i.e. how many items the list
// actually laid out in a frame.
class CountingMessage : public edc::Message
{
  public:
    CountingMessage(const std::string &text, time_t ts, int *renders) : Message(text, ts), renders_(renders)
    {
    }

    ::ftxui::Element OnRender() override
    {
        (*renders_)++;
        return Message::OnRender();
    }

  private:
    int *renders_;
};

// A log of |count| short single-line messages; every message is 1 row of text
// plus the 1-row spacer the list reserves below it.
std::shared_ptr<edc::ChatWidget> MakeLog(int count, int *renders = nullptr)
{
    auto chat = std::make_shared<edc::ChatWidget>();
    const auto now = ::time(nullptr);
    for (int i = 0; i < count; i++)
    {
        const std::string text = "message number " + std::to_string(i);
        if (renders)
            chat->AddMessage(std::make_shared<CountingMessage>(text, now, renders));
        else
            chat->AddMessage(std::make_shared<edc::Message>(text, now));
    }
    return chat;
}

// A log whose last message is long prose: it wraps into dozens of rows, so its
// measured height depends on the width.
std::shared_ptr<edc::ChatWidget> MakeWrappingChat()
{
    auto chat = std::make_shared<edc::ChatWidget>();
    const auto now = ::time(nullptr);
    for (int i = 0; i < 4; i++)
        chat->AddMessage(std::make_shared<edc::Message>("short message number " + std::to_string(i), now));

    std::string words;
    for (int i = 0; i < 400; i++)
        words += "word" + std::to_string(i) + " ";
    chat->AddMessage(std::make_shared<edc::Message>(words, now));
    return chat;
}

std::string NumberedBody(int lines)
{
    std::string body;
    for (int i = 0; i < lines; i++)
        body += "line-" + std::to_string(i) + "\n";
    return body;
}

void ScrollToTop(const std::shared_ptr<edc::ChatWidget> &chat)
{
    for (int i = 0; i < 2000; i++)
        chat->OnEvent(ftxui::Event::ArrowUp);
    (void)Draw(chat);
}

// The whole point of the rewrite: the per-frame cost must not grow with the
// log. The first frame measures every item once (that is the price of knowing
// the content height); from then on a frame must only touch the viewport.
static void test_only_visible_items_are_laid_out(void)
{
    TEST("a steady-state frame lays out only the visible items");
    int renders = 0;
    auto chat = MakeLog(400, &renders);

    (void)Draw(chat); // measuring pass: every item is laid out exactly once
    ASSERT(renders >= 400, "first frame measured %d items, expected all 400", renders);

    renders = 0;
    const std::string frame = Draw(chat);
    // 25 rows / 2 rows per message => ~13 visible, plus whatever the search
    // overshoots by. Anything close to 400 here means virtualisation is gone.
    ASSERT(renders > 0 && renders < 40, "second frame laid out %d of 400 items", renders);
    ASSERT(AnyRowContains(frame, "message number 399"), "tail not visible on the second frame");
    PASS();
}

static void test_appending_measures_only_the_new_item(void)
{
    TEST("appending a message measures just that message");
    int renders = 0;
    auto chat = MakeLog(400, &renders);
    (void)Draw(chat);

    const auto now = ::time(nullptr);
    chat->AddMessage(std::make_shared<CountingMessage>("zzz newest", now, &renders));

    renders = 0;
    const std::string frame = Draw(chat);
    ASSERT(renders < 40, "frame after an append laid out %d items", renders);
    ASSERT(AnyRowContains(frame, "zzz newest"), "new message not shown while following the tail");
    PASS();
}

static void test_large_log_scrolls_to_both_ends(void)
{
    TEST("a 400-message log still reaches both ends");
    auto chat = MakeLog(400);
    (void)Draw(chat);

    ASSERT(AnyRowContains(Draw(chat), "message number 399"), "tail of the log not visible at start");

    ScrollToTop(chat);
    ASSERT(AnyRowContains(Draw(chat), "message number 0"), "head of the log not reachable by scrolling up");

    for (int i = 0; i < 2000; i++)
        chat->OnEvent(ftxui::Event::ArrowDown);
    ASSERT(AnyRowContains(Draw(chat), "message number 399"), "tail of the log not reachable by scrolling down");
    PASS();
}

static void test_middle_of_log_renders_its_own_messages(void)
{
    TEST("scrolling to the middle shows that slice of the log");
    auto chat = MakeLog(400);
    (void)Draw(chat);

    // Halfway up: the viewport must be a contiguous window from the middle of
    // the log, not a stale tail rendered from cached geometry.
    for (int i = 0; i < 400; i++)
        chat->OnEvent(ftxui::Event::ArrowUp);
    const std::string frame = Draw(chat);

    bool found = false;
    for (int i = 50; i < 350; i++)
        found = found || RowOf(frame, ("message number " + std::to_string(i)).c_str()) >= 0;
    ASSERT(found, "no mid-log message visible after scrolling halfway up");
    ASSERT(!AnyRowContains(frame, "message number 399"), "tail still rendered after scrolling away from it");
    PASS();
}

static void test_wheel_scrolls_the_virtualized_log(void)
{
    TEST("wheel-down still reaches the tail");
    auto chat = MakeLog(400);
    (void)Draw(chat);
    ScrollToTop(chat);

    int handled = 0;
    for (int i = 0; i < 2000; i++)
        handled += chat->OnEvent(WheelDownAt(5, 5)) ? 1 : 0;
    ASSERT(handled > 0, "the wheel never scrolled the log");
    ASSERT(AnyRowContains(Draw(chat), "message number 399"), "wheel-down never reached the tail");
    PASS();
}

static void test_resize_remeasures_wrapped_heights(void)
{
    TEST("narrowing the pane re-wraps and re-pins to the tail");
    auto chat = MakeWrappingChat();
    ASSERT(AnyRowContains(Draw(chat, 100, 25), "word399"), "tail not visible before the resize");

    const std::string narrow = Draw(chat, 40, 25);
    // The wrapped height of every item changed with the width, so the cached
    // measurements must have been dropped and rebuilt.
    ASSERT(AnyRowContains(narrow, "word399"), "tail lost after re-wrapping at a narrower width");
    PASS();
}

static void test_click_reaches_a_visible_item(void)
{
    TEST("clicking an entry's footer still toggles it");
    auto chat = std::make_shared<edc::ChatWidget>();
    const auto now = ::time(nullptr);
    chat->AddMessage(std::make_shared<edc::ToolMessage>("shell", NumberedBody(60), now, false));

    const std::string collapsed = Draw(chat);
    ASSERT(!AnyRowContains(collapsed, "line-40"), "collapsed tool message leaked its full body");
    const int row = RowOf(collapsed, "Click to expand");
    ASSERT(row >= 0, "collapsed tool message has no expand footer to click");

    chat->OnEvent(ClickAt(3, row));
    const std::string expanded = Draw(chat);
    ASSERT(AnyRowContains(expanded, "Collapse"), "the click did not reach the entry's toggle");
    PASS();
}

static void test_click_is_ignored_outside_the_items(void)
{
    TEST("a click outside any item does not toggle anything");
    auto chat = std::make_shared<edc::ChatWidget>();
    const auto now = ::time(nullptr);
    chat->AddMessage(std::make_shared<edc::ToolMessage>("shell", NumberedBody(60), now, false));
    (void)Draw(chat);

    // Last screen row: below the (short) log, inside the viewport but not on
    // any message.
    ASSERT(chat->OnEvent(ClickAt(3, kH - 1)) == false, "a click on empty space was consumed");
    PASS();
}

static void test_expand_grows_the_item(void)
{
    TEST("expanding an entry reveals the rest of its body");
    auto chat = std::make_shared<edc::ChatWidget>();
    const auto now = ::time(nullptr);
    chat->AddMessage(std::make_shared<edc::ToolMessage>("shell", NumberedBody(20), now, false));
    chat->AddMessage(std::make_shared<edc::Message>("zzz after", now));

    const std::string before = Draw(chat);
    const int footer = RowOf(before, "Click to expand");
    ASSERT(footer >= 0, "collapsed tool message has no expand footer");
    ASSERT(RowOf(before, "line-19") < 0, "the collapsed preview already showed the whole body");

    chat->OnEvent(ClickAt(3, footer));
    const std::string after = Draw(chat);

    // The entry is 20 lines tall now instead of 14 + "...", so its tail (and
    // the collapse footer) are reachable — which they are not while collapsed.
    ASSERT(AnyRowContains(after, "Collapse"), "the entry did not expand");
    ASSERT(RowOf(after, "line-19") >= 0, "the expanded body stops short of its last line");
    PASS();
}

static void test_clear_resets_the_log(void)
{
    TEST("clearing the log empties the pane");
    auto chat = MakeLog(50);
    ASSERT(AnyRowContains(Draw(chat), "message number 49"), "log not rendered before the clear");

    chat->Clear();
    const std::string frame = Plain(Draw(chat));
    ASSERT(frame.find_first_not_of(" \r\n") == std::string::npos, "pane not empty after Clear()");

    chat->AddMessage(std::make_shared<edc::Message>("after clear", ::time(nullptr)));
    ASSERT(AnyRowContains(Draw(chat), "after clear"), "message added after Clear() not shown");
    PASS();
}

static void test_spacer_row_is_not_painted(void)
{
    TEST("the spacer row is not painted as part of a message");
    auto chat = std::make_shared<edc::ChatWidget>();
    const auto now = ::time(nullptr);
    chat->AddMessage(std::make_shared<edc::Message>("A", now));
    chat->AddMessage(std::make_shared<edc::Message>("B", now));

    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(30), ftxui::Dimension::Fixed(12));
    ftxui::Render(screen, chat->Render() | ftxui::flex);

    // A one-line message is a 1-row body inside an EMPTY border, so the item's
    // own box is rows 0..2. Row 3 is the list's spacer and must stay unpainted:
    // painting it (by stretching the bordered element into the spacer) is what
    // made every message look one row taller than the pre-virtualized log.
    auto painted = [&](int y) { return screen.PixelAt(0, y).background_color != ftxui::Color(); };
    ASSERT(painted(0) && painted(2), "the message's own box is not fully painted");
    ASSERT(!painted(3), "the spacer row is painted with the message background");
    ASSERT(painted(4), "the next message does not resume right after the spacer");
    PASS();
}

// ftxui's flow containers only break *between* their children, so a run with no
// space in it (CJK text, a long URL or path) is a single unbreakable child that
// used to overflow its box and be clipped by the stencil — the text past the
// right edge simply vanished. Every message type now wraps its own text against
// the width the list measured it at.
static void test_unbroken_run_wraps_in_every_message_type(void)
{
    TEST("an unbroken CJK run wraps instead of being clipped");
    const auto now = ::time(nullptr);
    // No space anywhere, so there is no word boundary to break at; the TAIL
    // marker can only show up if the run was hard-broken between code points.
    const std::string text = "这是一段没有任何空格的中文文本用来测试消息正文在宽度不足时会不会把尾巴裁掉TAIL";

    struct Case
    {
        const char *label;
        std::shared_ptr<edc::Message> message;
    };
    const Case cases[] = {
        {"user", std::make_shared<edc::UserMessage>(text, now)},
        {"assistant", std::make_shared<edc::Message>(text, now)},
        {"tool", std::make_shared<edc::ToolMessage>("shell", text, now, false)},
    };

    for (const Case &c : cases)
    {
        auto chat = std::make_shared<edc::ChatWidget>();
        chat->AddMessage(c.message);
        const std::string frame = Draw(chat, 40, kH);
        ASSERT(AnyRowContains(frame, "TAIL"), "%s message lost the tail of an unbroken run", c.label);
        ASSERT(AnyRowContains(frame, "这是一段"), "%s message lost the head of an unbroken run", c.label);
    }
    PASS();
}

// Wrapping is width-dependent, so narrowing the pane has to re-wrap rather than
// leave the text as it was measured at the old width.
static void test_unbroken_run_rewraps_on_resize(void)
{
    TEST("narrowing the pane re-wraps an unbroken run without losing text");
    const auto now = ::time(nullptr);
    const std::string text = "这是一段没有任何空格的中文文本用来测试收窄面板之后正文能否重新换行并且不丢字TAIL";
    auto chat = std::make_shared<edc::ChatWidget>();
    chat->AddMessage(std::make_shared<edc::UserMessage>(text, now));

    for (int width : {80, 40, 24})
        ASSERT(AnyRowContains(Draw(chat, width, kH), "TAIL"), "tail lost at width %d", width);
    PASS();
}

// FTXUI collects the selected text by walking the node tree and asking every
// node which of its cells the selection covers. The list draws its items itself
// instead of holding them as child components, so it must forward Select() down
// to them; otherwise dragging over the log selects nothing. Regression: the
// virtualization rewrite dropped that forwarding, so message text could no
// longer be selected at all.
static void test_selection_collects_message_text(void)
{
    TEST("dragging over the log collects the message text");
    const auto now = ::time(nullptr);
    auto chat = std::make_shared<edc::ChatWidget>();
    chat->AddMessage(std::make_shared<edc::Message>("ZEBRAFISH", now));

    ftxui::Element element = chat->Render() | ftxui::flex;
    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(kW), ftxui::Dimension::Fixed(kH));
    ftxui::Selection selection(0, 0, kW - 1, kH - 1);
    ftxui::Render(screen, element.get(), selection);

    const std::string selected = selection.GetParts();
    ASSERT(selected.find("ZEBRAFISH") != std::string::npos,
           "selection did not collect the message text (got \"%s\")", selected.c_str());
    PASS();
}

} // namespace

int main(void)
{
    test_only_visible_items_are_laid_out();
    test_appending_measures_only_the_new_item();
    test_large_log_scrolls_to_both_ends();
    test_middle_of_log_renders_its_own_messages();
    test_wheel_scrolls_the_virtualized_log();
    test_resize_remeasures_wrapped_heights();
    test_click_reaches_a_visible_item();
    test_click_is_ignored_outside_the_items();
    test_expand_grows_the_item();
    test_clear_resets_the_log();
    test_spacer_row_is_not_painted();
    test_unbroken_run_wraps_in_every_message_type();
    test_unbroken_run_rewraps_on_resize();
    test_selection_collects_message_text();

    printf("\n%d/%d tests passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
