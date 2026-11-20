/* ChatModel paging rules, pinned without a terminal or an actor thread.
 *
 * A CHAT_BUBBLE_PAGE reply is asynchronous. Under a slow sanitizer build the
 * startup fetch (cursor 0 = "newest page") sometimes timed out; its reply then
 * arrived after the first live bubbles were already on screen. Before the fix
 * the UI treated that reply as an OLDER page and called ApplyOlderPage(), which
 * rebuilt all_/visible_ from the stale DB snapshot -- the live bubbles vanished
 * and whichever assertion read the frame next failed. The failure was flaky and
 * looked like a rendering bug (see tests/test_tui_dispatch.cpp).
 *
 * These cases feed the real ChatModel with the real wire JSON, deterministically.
 */
#include "edc_chat_bubble_page.hpp"
#include "edc_chat_model.hpp"
#include "edc_message.hpp"
#include "test_fs_util.h"

#include <cstdio>
#include <fstream>
#include <cstring>
#include <memory>
#include <string>

static int g_failed = 0;
static int g_passed = 0;

#define CHECK(cond, ...)                                                                                               \
    do                                                                                                               \
    {                                                                                                              \
        if (!(cond))                                                                                               \
        {                                                                                                          \
            fprintf(stderr, "    FAIL: ");                                                                            \
            fprintf(stderr, __VA_ARGS__);                                                                              \
            fprintf(stderr, "\n");                                                                                     \
            g_failed++;                                                                                                \
        }                                                                                                              \
        else                                                                                                           \
        {                                                                                                              \
            printf("    ok: ");                                                                                        \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
            g_passed++;                                                                                                \
        }                                                                                                              \
    } while (0)

namespace
{

std::string Page(const char *sid, int64_t before_seq, const char *const *lines, const int64_t *seqs, int n)
{
    return edc::EncodeChatBubblePageReply(sid, before_seq, lines, seqs, n, false);
}

std::string Line(const char *content)
{
    /* Four real tabs: kind, render_pref, task_id, header, then content. */
    return std::string("reply\t0\t\t\t") + content;
}

/* A model loaded from a startup page with seq 20..21, plus one live bubble. */
struct Fixture
{
    edc::model::ChatModel model;
    std::string a = Line("startup-page-row-A");
    std::string b = Line("startup-page-row-B");

    Fixture()
    {
        const char *lines[] = {a.c_str(), b.c_str()};
        const int64_t seqs[] = {20, 21};
        /* Startup cursor is 0 on the wire; LoadFromPage does not care about it. */
        model.LoadFromPage("s1", Page("s1", 0, lines, seqs, 2));
        model.AddMessage(std::make_shared<edc::Message>("LIVE_BUBBLE_MARKER", 0));
    }
};

void test_older_page_is_applied(void)
{
    printf("-- a legitimate older page is still prepended --\n");
    Fixture f;
    CHECK(f.model.size() == 3, "startup page (2) + one live bubble = 3, got %zu", f.model.size());
    /* Vacuous-pass guard: the cursor really is the oldest loaded seq. */
    CHECK(f.model.oldest_seq() == 20, "oldest_seq is 20, got %lld", (long long)f.model.oldest_seq());

    const std::string older = Line("older-page-row");
    const char *lines[] = {older.c_str()};
    const int64_t seqs[] = {19};
    /* The request echoed the cursor it answered: 20. */
    CHECK(f.model.ApplyOlderPage("s1", Page("s1", 20, lines, seqs, 1)), "the page matching oldest_seq is applied");
    CHECK(f.model.size() == 4, "one older row was prepended, got %zu", f.model.size());
    CHECK(f.model.oldest_seq() == 19, "oldest_seq moved back to 19, got %lld", (long long)f.model.oldest_seq());
    CHECK(f.model.visible().size() == 4, "the live bubble survives a legitimate prepend");
}

void test_late_startup_page_is_refused(void)
{
    printf("-- a late startup page (cursor 0) must not rebuild the model --\n");
    Fixture f;
    const size_t before = f.model.size();
    const int64_t oldest_before = f.model.oldest_seq();

    const char *lines[] = {"reply\t0\t\t\tSTALE_DB_MARKER"};
    const int64_t seqs[] = {18};
    /* This is exactly the reply a timed-out FetchChatBubblePageSync(id, 0, ...)
     * would deliver: cursor 0, rows from the DB, header = session id. */
    CHECK(!f.model.ApplyOlderPage("s1", Page("s1", 0, lines, seqs, 1)),
          "cursor 0 is refused (it answers the startup request, not an older scroll)");
    CHECK(f.model.size() == before, "size unchanged, got %zu (was %zu)", f.model.size(), before);
    CHECK(f.model.oldest_seq() == oldest_before, "oldest_seq unchanged");
    CHECK(f.model.visible().size() == before, "the visible sequence was not rebuilt");

    /* Anti-vacuity: with no guard, a cursor-0 page WOULD be applied when the
     * model has no loaded rows (oldest_seq_ == 0). Assert the precondition that
     * makes this fixture able to tell the difference. */
    CHECK(oldest_before == 20, "the fixture really loaded a startup page (oldest_seq != 0)");
}

void test_wrong_cursor_is_refused(void)
{
    printf("-- a page answering a request the reader has moved past is refused --\n");
    Fixture f;
    const size_t before = f.model.size();

    const char *lines[] = {"reply\t0\t\t\tWRONG_CURSOR_MARKER"};
    const int64_t seqs[] = {15};
    /* A scroll asked for seq < 17 long ago; oldest_seq_ is now 20. */
    CHECK(!f.model.ApplyOlderPage("s1", Page("s1", 17, lines, seqs, 1)), "a mismatched cursor is refused");
    CHECK(f.model.size() == before, "no duplicate rows were prepended");

    /* And the live bubble is still the last visible one. Read its content
     * through the model's own window: LazyBubble proxies keep only a reference,
     * so Message::Content() is empty by design until Get() materializes it. */
    const auto &visible = f.model.visible();
    CHECK(!visible.empty() && visible.back()->HasCachedTree() == false,
          "the live bubble is still the last visible proxy");
    CHECK(visible.size() == 3 && visible.back()->TaskId().empty(),
          "the visible sequence was not rebuilt (3 rows, live bubble last)");
}

void test_foreign_session_is_refused(void)
{
    printf("-- a page for another session is refused --\n");
    Fixture f;
    const size_t before = f.model.size();
    const char *lines[] = {"reply\t0\t\t\tFOREIGN_MARKER"};
    const int64_t seqs[] = {19};
    /* The UI receives (session_id, payload) as a pair: session_id comes from
     * the channel message HEADER, not from the JSON body. Reply for a session
     * the model has since left must be refused even though its cursor happens
     * to match. */
    CHECK(!f.model.ApplyOlderPage("s2", Page("s2", 20, lines, seqs, 1)),
          "a reply for the previous session is refused");
    CHECK(f.model.size() == before, "no row from the old session was prepended");
}

/* A cold/debug restart can make FetchChatBubblePageSync's 3s wait time out.
 * LoadChatTranscript then falls back to an old chat.jsonl that has not grown
 * since chat_bubbles became authoritative. The late cursor-0 DB page must
 * replace that fallback, or the newest history looks lost even though the DB
 * still owns every row. */
void test_late_startup_page_recovers_fallback(void)
{
    printf("-- a late cursor-0 page recovers the legacy fallback --\n");
    const std::string path = test_tmp_file_path("edc_late_startup_fallback.jsonl");
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << "reply\t0\t\t\tOLD_FILE_MARKER\n";
    }

    edc::model::ChatModel model;
    model.LoadTranscript(path);
    CHECK(model.size() == 1, "the legacy file really loaded one row, got %zu", model.size());

    const char *lines[] = {"reply\t0\t\t\tDURABLE_DB_TAIL_MARKER"};
    const int64_t seqs[] = {97};
    CHECK(model.RecoverStartupPage("s1", Page("s1", 0, lines, seqs, 1)),
          "the startup page replaces the legacy fallback");
    CHECK(model.size() == 1, "the DB page has one row, got %zu", model.size());
    CHECK(model.oldest_seq() == 97, "the DB page's seq became the oldest cursor, got %lld",
          (long long)model.oldest_seq());
    CHECK(model.session_id() == "s1", "the DB page's session id was adopted");
    std::remove(path.c_str());
}

/* The same late page must not clobber live bubbles that appeared while the
 * synchronous fetch was waiting. */
void test_late_startup_page_does_not_clobber_live_chat(void)
{
    printf("-- a late cursor-0 page leaves a live bubble alone --\n");
    const std::string path = test_tmp_file_path("edc_late_startup_live.jsonl");
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << "reply\t0\t\t\tOLD_FILE_MARKER\n";
    }

    edc::model::ChatModel model;
    model.LoadTranscript(path);
    model.AddMessage(std::make_shared<edc::Message>("LIVE_BUBBLE_MARKER", 0));
    const size_t before = model.size();

    const char *lines[] = {"reply\t0\t\t\tSTALE_DB_TAIL_MARKER"};
    const int64_t seqs[] = {97};
    CHECK(!model.RecoverStartupPage("s1", Page("s1", 0, lines, seqs, 1)),
          "a late page is refused once live bubbles are present");
    CHECK(model.size() == before, "the live bubble was not clobbered, got %zu (was %zu)", model.size(), before);
    CHECK(model.visible().size() == before, "the visible sequence was not rebuilt");
    std::remove(path.c_str());
}

} // namespace

int main(void)
{
    test_older_page_is_applied();
    test_late_startup_page_is_refused();
    test_wrong_cursor_is_refused();
    test_foreign_session_is_refused();
    test_late_startup_page_recovers_fallback();
    test_late_startup_page_does_not_clobber_live_chat();
    printf("\n%d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
