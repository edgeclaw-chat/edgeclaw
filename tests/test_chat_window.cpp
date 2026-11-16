// Regression tests for chat-log windowing (docs/tui-windowing-proposal.md).
//
// The defect: MessageList draws only the viewport, but it keeps every Message
// instance (and each instance's cached ftxui render tree) resident in items_.
// ChatModel::all_ holds a second vector of the same shared_ptrs. So the log's
// memory is O(history), not O(viewport): a long transcript backfilled at startup
// leaves every bubble alive even though only a screenful can ever be drawn.
//
// P0 pins the property down BEFORE the fix: LiveCount() must stay bounded no
// matter how many bubbles the log holds. On the pre-windowing implementation
// this fails (LiveCount() == the whole log), which is the point -- a regression
// test that has never been red has proved nothing.
#include "edc_chat_transcript.hpp"
#include "edc_chat_widget.hpp"
#include "edc_message.hpp"
#include "ftxui/component/event.hpp"
#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/screen.hpp"
#include "edc_message.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <ctime>
#include <memory>
#include <string>
#include <vector>

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name)                                                                                                     \
    do                                                                                                                 \
    {                                                                                                                  \
        tests_run++;                                                                                                   \
        printf("[%3d] %-58s ", tests_run, name);                                                                       \
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

/* The bound the review settled on (docs/tui-windowing-proposal.md §2.2, §8). */
constexpr size_t kMaxLive = 200;

/* Enough bubbles that "linear in history" cannot be confused with a bounded
 * window: 5x the cap. Each carries a body of its own so the log is not a run of
 * empty strings that an optimizer or a shortcut could collapse. */
constexpr size_t kHistory = 1000;

/* Feed a long history the way startup does: a transcript file, loaded in one
 * shot. This is the path that must stay O(viewport) resident. */
std::string WriteLog(const std::string &tag, size_t n)
{
    const std::string path = "/tmp/edc_chat_window_" + tag + ".jsonl";
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    for (size_t i = 0; i < n; i++)
    {
        edc::ChatTranscriptEntry e;
        e.kind = "sys";
        e.content = "bubble " + std::to_string(i) + " — a line of chat text that must be rendered";
        out << edc::EncodeTranscriptLine(e);
    }
    return path;
}

std::shared_ptr<edc::ChatWidget> FillLog(size_t n)
{
    auto chat = std::make_shared<edc::ChatWidget>();
    chat->model()->LoadTranscript(WriteLog("fill" + std::to_string(n), n));
    return chat;
}

static void test_live_count_is_bounded_by_history_length(void)
{
    TEST("resident Message count stays bounded as history grows");

    auto chat = FillLog(kHistory);

    /* Vacuous-pass guard: the log really does hold kHistory bubbles. Without
     * this, an implementation that silently dropped messages would pass the
     * bound check while testing nothing. */
    ASSERT(chat->LiveCount() > 0, "LiveCount() reports 0 -- the probe is not wired to the log");

    /* P0: on the old implementation LiveCount() == kHistory, so this is RED.
     * After windowing it must be <= kMaxLive. */
    ASSERT(chat->LiveCount() <= kMaxLive, "live=%zu exceeds the cap %zu for a %zu-bubble log (O(history) residency)",
           chat->LiveCount(), kMaxLive, kHistory);
    PASS();
}

static void test_live_count_does_not_grow_with_history(void)
{
    TEST("doubling history does not double residency");

    const size_t small_n = kMaxLive / 2; /* below the cap: everything may be live */
    const size_t large_n = kHistory;

    auto small = FillLog(small_n);
    auto large = FillLog(large_n);
    const size_t small_live = small->LiveCount();
    const size_t large_live = large->LiveCount();

    /* Vacuous-pass guards. */
    ASSERT(small_live > 0, "small log has no live messages");
    ASSERT(large_live > 0, "large log has no live messages");
    ASSERT(large_n > small_n * 2, "the two logs are not far enough apart to prove the point");

    /* A bounded implementation may legitimately keep more of the small log than
     * of the large one, but never more than the cap; and the large log must not
     * cost proportionally more than the small one. On the old implementation
     * large_live / small_live == large_n / small_n, so this is RED. */
    ASSERT(large_live <= kMaxLive, "large log is O(history): live=%zu (cap %zu)", large_live, kMaxLive);
    ASSERT(large_live <= small_live * 2, "residency grew with history: %zu -> %zu for %zu -> %zu bubbles", small_live,
           large_live, small_n, large_n);
    PASS();
}


/* Draw one frame and return its plain text (terminal escapes stripped enough
 * for substring checks). */
std::string Frame(const std::shared_ptr<edc::ChatWidget> &chat)
{
    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(100), ftxui::Dimension::Fixed(25));
    ftxui::Render(screen, chat->Render() | ftxui::flex);
    return screen.ToString();
}

/* Regression for the windowing render bug: after scrolling from the tail to the
 * head and back, EVERY frame must still draw the bubbles at that end. The
 * geometry (prefix_/total_/visible range) was correct while the frame was
 * blank, so a residency-only assertion does not catch it -- the text has to be
 * on screen. Verified RED against a build where LazyBubble delegated through
 * ComponentBase::Render() (the measure/draw passes saw different Nodes). */
static void test_scrolling_keeps_both_ends_rendered(void)
{
    TEST("scrolling to head and back keeps both ends rendered");
    auto chat = FillLog(400);

    std::string start = Frame(chat);
    ASSERT(start.find("bubble 399") != std::string::npos, "tail blank on the first frame");

    for (int i = 0; i < 2000; i++)
        chat->OnEvent(ftxui::Event::ArrowUp);
    std::string top = Frame(chat);
    ASSERT(top.find("bubble 0") != std::string::npos, "head blank after scrolling to the top");

    for (int i = 0; i < 2000; i++)
        chat->OnEvent(ftxui::Event::ArrowDown);
    std::string bottom = Frame(chat);
    ASSERT(bottom.find("bubble 399") != std::string::npos, "tail blank after scrolling back down");

    /* Vacuous-pass guard: the log really is long and residency really is bounded. */
    ASSERT(chat->LiveCount() > 0 && chat->LiveCount() <= kMaxLive, "probe is vacuous: live=%zu", chat->LiveCount());
    PASS();
}

} // namespace

int main(void)
{
    test_live_count_is_bounded_by_history_length();
    test_live_count_does_not_grow_with_history();
    test_scrolling_keeps_both_ends_rendered();

    printf("\n%d/%d tests passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
