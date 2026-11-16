// Tests for the windowed chat log's data layer (edc_chat_window.*).
//
// These pin down the part that makes windowing correct at all: the transcript
// index must describe EXACTLY the lines the existing decoder produces, and the
// resident window must stay bounded while sliding anywhere in the log. The file
// is the authority; a descriptor is only an (offset, length) plus small metadata.
#include "edc_chat_transcript.hpp"
#include "edc_chat_window.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
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

std::string TmpPath(const char *tag)
{
    return std::string("/tmp/edc_chat_refs_") + tag + ".jsonl";
}

/* Write `n` encoded lines, with escaped newlines/tabs inside content and a
 * deliberate 300 KB run so at least one line crosses a scan chunk boundary. */
size_t WriteTranscript(const std::string &path, size_t n, size_t huge_at)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
        return 0;
    for (size_t i = 0; i < n; i++)
    {
        edc::ChatTranscriptEntry e;
        e.kind = (i % 5 == 0) ? "tool" : (i % 5 == 1) ? "user" : (i % 5 == 2) ? "thought" : (i % 5 == 3) ? "reply" : "sys";
        e.render_pref = (i % 2) ? 1 : 0;
        e.task_id = "task-" + std::to_string(i % 7);
        e.header = (e.kind == "tool") ? "exec_shell\twith a tab" : "";
        e.content = "bubble " + std::to_string(i) + " line1\\nline2\\twith tab";
        if (i == huge_at)
            e.content += std::string(300 * 1024, 'x');
        out << edc::EncodeTranscriptLine(e);
    }
    out.close();
    /* A trailing fragment, like a process that died mid-append: must be dropped. */
    std::ofstream append(path, std::ios::binary | std::ios::app);
    append << "half\twritten\tline\twithout\tnewline";
    return n;
}

static void test_index_matches_decoder(void)
{
    TEST("index describes exactly the lines the decoder produces");
    const std::string path = TmpPath("decode");
    const size_t n = 500;
    ASSERT(WriteTranscript(path, n, /*huge_at=*/137) == n, "fixture write failed");

    std::vector<edc::ChatBubbleRef> refs;
    const size_t built = edc::BuildChatBubbleRefs(path, &refs);
    ASSERT(built == n, "built %zu refs for %zu lines", built, n);

    std::vector<edc::ChatTranscriptEntry> all;
    const size_t decoded = edc::LoadTranscript(path, [&](const edc::ChatTranscriptEntry &e) { all.push_back(e); });
    ASSERT(decoded == n, "reference decoder got %zu lines", decoded);
    ASSERT(all.size() == refs.size(), "decoder/index count disagree");

    size_t checked = 0;
    for (size_t i = 0; i < n; i++)
    {
        std::string kind, task_id, header, content;
        int pref = -1;
        ASSERT(edc::DecodeChatBubbleAt(path, refs[i], &kind, &pref, &task_id, &header, &content),
               "line %zu did not decode from its offset", i);
        ASSERT(kind == all[i].kind && pref == all[i].render_pref && task_id == all[i].task_id &&
                   header == all[i].header && content == all[i].content,
               "line %zu differs from the reference decode", i);
        checked++;
    }
    ASSERT(checked == n, "only %zu/%zu checked", checked, n);
    std::remove(path.c_str());
    PASS();
}

static void test_index_offsets_are_exact(void)
{
    TEST("each descriptor addresses exactly one whole line");
    const std::string path = TmpPath("offsets");
    const size_t n = 64;
    ASSERT(WriteTranscript(path, n, /*huge_at=*/n + 1) == n, "fixture write failed");

    std::vector<edc::ChatBubbleRef> refs;
    ASSERT(edc::BuildChatBubbleRefs(path, &refs) == n, "index count");
    std::ifstream file(path, std::ios::binary);
    ASSERT(file.good(), "reopen failed");
    for (size_t i = 0; i < n; i++)
    {
        file.seekg(static_cast<std::streamoff>(refs[i].offset), std::ios::beg);
        std::string raw(refs[i].length, '\0');
        file.read(&raw[0], static_cast<std::streamsize>(refs[i].length));
        ASSERT(static_cast<size_t>(file.gcount()) == refs[i].length, "short read at %zu", i);
        ASSERT(!raw.empty() && raw.back() == '\n', "line %zu does not end at a newline", i);
        if (i > 0)
            ASSERT(refs[i].offset == refs[i - 1].offset + refs[i - 1].length, "line %zu is not adjacent", i);
    }
    std::remove(path.c_str());
    PASS();
}

static void test_window_is_bounded(void)
{
    TEST("resident instances never exceed the cap, whatever the history");
    const std::string path = TmpPath("bounded");
    const size_t n = 40000;
    ASSERT(WriteTranscript(path, n, /*huge_at=*/n + 1) == n, "fixture write failed");

    std::vector<edc::ChatBubbleRef> refs;
    ASSERT(edc::BuildChatBubbleRefs(path, &refs) == n, "index count");
    edc::MessageWindow window(path, &refs);

    /* Vacuous-pass guard: the index really is long. */
    ASSERT(window.RefCount() == n, "index is not the full log");

    window.SetWindow(n - 20, n, 20);
    ASSERT(window.LiveCount() > 0, "window loaded nothing -- the test would pass vacuously");
    ASSERT(window.LiveCount() <= edc::MessageWindow::kMaxLive, "live=%zu exceeds cap %zu", window.LiveCount(),
           edc::MessageWindow::kMaxLive);

    /* A huge requested window is still clamped by the cap. */
    window.SetWindow(0, n, n);
    ASSERT(window.LiveCount() <= edc::MessageWindow::kMaxLive, "full-log window kept %zu instances",
           window.LiveCount());
    std::remove(path.c_str());
    PASS();
}

static void test_window_slides_and_evicts(void)
{
    TEST("sliding evicts the old window and reloads it on return");
    const std::string path = TmpPath("slide");
    const size_t n = 2000;
    ASSERT(WriteTranscript(path, n, /*huge_at=*/n + 1) == n, "fixture write failed");

    std::vector<edc::ChatBubbleRef> refs;
    ASSERT(edc::BuildChatBubbleRefs(path, &refs) == n, "index count");
    edc::MessageWindow window(path, &refs);

    window.SetWindow(0, 30, 10);
    ASSERT(window.Has(5), "head window did not load index 5");
    ASSERT(!window.Has(n - 5), "tail was resident before being requested");

    window.SetWindow(n - 30, n, 10);
    ASSERT(window.Has(n - 5), "tail window did not load its own index");
    ASSERT(!window.Has(5), "head instance survived eviction (O(history) residency)");

    window.SetWindow(0, 30, 10);
    ASSERT(window.Has(5), "returning to the head did not reload it");
    ASSERT(!window.Has(n - 5), "tail instance survived the second eviction");
    ASSERT(window.LiveCount() <= edc::MessageWindow::kMaxLive, "live=%zu exceeds cap", window.LiveCount());
    std::remove(path.c_str());
    PASS();
}

static void test_empty_and_missing_files(void)
{
    TEST("missing/empty transcript produces an empty, safe index");
    std::vector<edc::ChatBubbleRef> refs;
    ASSERT(edc::BuildChatBubbleRefs("/tmp/edc_chat_refs_no_such_file.jsonl", &refs) == 0, "missing file built refs");
    ASSERT(refs.empty(), "missing file left refs behind");

    const std::string path = TmpPath("empty");
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
    }
    ASSERT(edc::BuildChatBubbleRefs(path, &refs) == 0, "empty file built refs");
    edc::MessageWindow window(path, &refs);
    ASSERT(window.Empty(), "window over empty index is not empty");
    ASSERT(window.SetWindow(0, 10, 5) == 0, "empty window loaded something");
    ASSERT(window.Get(0) == nullptr, "Get() past the end returned an object");
    std::remove(path.c_str());
    PASS();
}

static void test_state_lives_on_the_ref(void)
{
    TEST("expanded/measured state is on the descriptor, not the instance");
    const std::string path = TmpPath("state");
    const size_t n = 100;
    ASSERT(WriteTranscript(path, n, /*huge_at=*/n + 1) == n, "fixture write failed");
    std::vector<edc::ChatBubbleRef> refs;
    ASSERT(edc::BuildChatBubbleRefs(path, &refs) == n, "index count");
    edc::MessageWindow window(path, &refs);

    window.SetWindow(0, 20, 5);
    edc::ChatBubbleRef *r = window.Ref(7);
    ASSERT(r != nullptr, "no descriptor for index 7");
    r->expanded = true;
    r->measured_h = 42;
    window.SetWindow(n - 20, n, 5);
    ASSERT(!window.Has(7), "index 7 stayed resident");
    window.SetWindow(0, 20, 5);
    ASSERT(window.Ref(7)->expanded, "expanded state was lost when the instance was evicted");
    ASSERT(window.Ref(7)->measured_h == 42, "measured height was lost with the instance");
    std::remove(path.c_str());
    PASS();
}

} // namespace

int main(void)
{
    test_index_matches_decoder();
    test_index_offsets_are_exact();
    test_window_is_bounded();
    test_window_slides_and_evicts();
    test_empty_and_missing_files();
    test_state_lives_on_the_ref();

    printf("\n%d/%d tests passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
