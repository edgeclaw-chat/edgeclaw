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
#include "ftxui/component/mouse.hpp"
#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/screen.hpp"
#include "test_fs_util.h"
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
    const std::string path = test_tmp_file_path(("edc_chat_window_" + tag + ".jsonl").c_str());
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

/* P7: the FIRST frame is what DHAT's "At t-gmax" measures. The existing
 * pruning runs at the next frame boundary, so a long transcript can still
 * build one cached ftxui tree per proxy during the first layout pass and
 * peak at O(history) bytes before anything is released -- exactly the
 * ~296 MiB the profiler saw. Assert the peak directly: after ONE frame the
 * number of retained trees must already be O(viewport), not O(history). */
static void test_first_frame_cached_trees_are_bounded(void)
{
    TEST("first frame does not cache one render tree per history bubble");
    auto chat = FillLog(kHistory);

    ASSERT(chat->model()->size() == kHistory, "log holds %zu bubbles, expected %zu", chat->model()->size(), kHistory);

    const std::string frame = Frame(chat);
    /* Vacuous-pass guards: the frame really rendered the tail, and the log
     * really is long enough that an O(history) result would be unmistakable. */
    ASSERT(frame.find("bubble 999") != std::string::npos, "tail blank -- first frame did not render");
    ASSERT(kHistory > kMaxLive * 2, "fixture too short to distinguish O(history) from a bounded window");

    const size_t trees = chat->model()->CachedTreeCount();
    ASSERT(trees <= kMaxLive + 32, "first frame cached %zu trees for %zu bubbles (peak is O(history))",
           trees, kHistory);
    PASS();
}

/* P6: the residency test above only counts REAL Message instances in
 * MessageWindow::live_. That is not the whole story: LazyBubble::OnRender()
 * caches the real bubble's element tree on the PROXY (its inherited body_),
 * and the proxy is never evicted. MessageList::EnsureMeasured used to measure
 * every proxy on the first frame, so a 1000-bubble log ended up with 1000
 * cached ftxui trees -- DHAT measured ~296 MiB at the global peak after the
 * first windowing merge, barely below the un-windowed 323 MiB.
 *
 * Count the trees, not the instances. This assertion is RED before the fix
 * (and stays RED for any "fix" that merely spreads the full measurement over
 * several frames via the catch-up budget). */
static void test_cached_trees_stay_bounded(void)
{
    TEST("cached render trees stay bounded, not one per history bubble");
    auto chat = FillLog(kHistory);

    /* Vacuous-pass guards: the log really is long and the frame really drew
     * something (so a no-op implementation cannot pass by rendering nothing). */
    ASSERT(chat->model()->LiveCount() > 0, "no real bubbles are resident -- probe is vacuous");
    ASSERT(chat->model()->size() == kHistory, "log holds %zu bubbles, expected %zu", chat->model()->size(), kHistory);

    /* Several frames: the catch-up work (64 stale rows/frame) must not creep
     * back into a full-log measurement either. */
    for (int frame = 0; frame < 30; frame++)
        (void)Frame(chat);

    /* Then scroll through the WHOLE log in page steps, drawing every frame so
     * each slice really gets measured. A proxy that once entered the viewport
     * keeps its body_ unless something evicts it, so a long read session is
     * the case most likely to regress back to O(history). Guard against a
     * vacuous pass: the head must actually have been reached. */
    std::string head;
    for (int i = 0; i < 2000; i++)
    {
        chat->OnEvent(ftxui::Event::PageUp);
        head = Frame(chat);
        if (head.find("bubble 0") != std::string::npos)
            break;
    }
    ASSERT(head.find("bubble 0") != std::string::npos, "never reached the head -- scroll guard is vacuous");
    for (int i = 0; i < 2000; i++)
    {
        chat->OnEvent(ftxui::Event::PageDown);
        (void)Frame(chat);
        if (chat->StickingToBottom())
            break;
    }

    /* The retained set is the resident window (kMaxLive) plus the few rows
     * actually on screen, not the whole kHistory log. 32 is a generous
     * viewport bound for the fixed 25-row test screen. */
    const size_t trees = chat->model()->CachedTreeCount();
    const size_t bound = kMaxLive + 32;
    ASSERT(trees <= bound, "cached trees=%zu exceeds the bound %zu for a %zu-bubble log (O(history) RSS)",
           trees, bound, kHistory);
    PASS();
}

/* A LazyBubble caches the ftxui tree built by a REAL bubble. ToolMessage and
 * ThoughtMessage put reflect(box_) in that tree: reflect holds a reference to
 * the real bubble's own member. MessageWindow::Get() may evict the real bubble
 * (the startup window primes only the tail; a resize's catch-up / a scroll can
 * materialise another range), but the proxy keeps rendering its cached tree.
 * Reflect::Render then writes the new box into freed memory -- a 16-byte
 * heap-use-after-free that later shows up as corruption in an unrelated vector
 * (the chat list's hit boxes were where the user's gdb backtrace landed). */
static void test_cached_tree_survives_real_bubble_eviction(void)
{
    TEST("a cached tooltip tree does not reference an evicted real bubble");
    constexpr size_t kRows = 260;
    const std::string path = test_tmp_file_path("edc_chat_window_evict.jsonl");
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        for (size_t i = 0; i < kRows; i++)
        {
            edc::ChatTranscriptEntry e;
            /* Plain sys rows cannot reproduce it: their tree has no reflect.
             * Alternate the two message types whose trees capture box_. */
            e.kind = (i % 2 == 0) ? "thought" : "tool";
            e.header = "shell";
            /* >14 lines makes the tool bubble collapsible, so its footer also
             * carries a reflect(box_) in the cached tree. */
            std::string body;
            for (size_t line = 0; line < 20; line++)
                body += "line " + std::to_string(i) + "-" + std::to_string(line) + "\n";
            e.content = body;
            out << edc::EncodeTranscriptLine(e);
        }
    }

    auto chat = std::make_shared<edc::ChatWidget>();
    chat->model()->LoadTranscript(path);
    /* First frame: the tail proxies materialize their real bubbles and cache
     * their trees. Assert the fixture really took that path. */
    (void)Frame(chat);
    ASSERT(chat->model()->CachedTreeCount() > 0, "no proxy cached a render tree -- probe is vacuous");

    /* Drive the resident window away from the tail: all those real bubbles are
     * released. The proxies and their cached trees stay alive. */
    chat->model()->SetWindow(0, 1, /*prefetch=*/0);
    ASSERT(chat->model()->LiveCount() <= 2, "old tail bubbles were not evicted -- probe is vacuous");

    /* Re-render the proxies whose real bubble died. Without owning the real
     * bubble (or dropping the tree), ftxui's reflect writes through a dangling
     * Box&. ASan reports it here; without ASan it corrupts the heap. */
    for (size_t i = kRows - 20; i < kRows; i++)
    {
        auto proxy = chat->model()->visible()[i];
        auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(80), ftxui::Dimension::Fixed(25));
        ftxui::Render(screen, proxy->Render() | ftxui::flex);
    }

    /* The rebound hit box is not merely safe to write: clicking the cached
     * tree's collapsible footer must still reach the newly materialised real
     * bubble and persist the toggle on the descriptor. */
    auto click_screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(100), ftxui::Dimension::Fixed(25));
    ftxui::Render(click_screen, chat->Render() | ftxui::flex);
    int footer_row = -1;
    for (int y = 0; y < 25 && footer_row < 0; y++)
    {
        std::string row;
        for (int x = 0; x < 100; x++)
        {
            const std::string &cell = click_screen.CellAt(x, y).character;
            row += cell.empty() ? " " : cell;
        }
        if (row.find("Click to expand") != std::string::npos)
            footer_row = y;
    }
    ASSERT(footer_row >= 0, "no collapsible footer is on screen -- click probe is vacuous");
    const auto click = ::ftxui::Event::Mouse(
        "", ::ftxui::Mouse{::ftxui::Mouse::Left, ::ftxui::Mouse::Pressed, false, false, false, 4, footer_row});
    ASSERT(chat->OnEvent(click), "click on the cached footer did not reach the bubble");
    ASSERT(Frame(chat).find("Collapse") != std::string::npos, "the cached footer did not expand the bubble");

    std::remove(path.c_str());
    PASS();
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
    test_first_frame_cached_trees_are_bounded();
    test_cached_trees_stay_bounded();
    test_cached_tree_survives_real_bubble_eviction();
    test_scrolling_keeps_both_ends_rendered();

    printf("\n%d/%d tests passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
