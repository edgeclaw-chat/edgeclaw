/* kDatabase descriptors: a chat_bubbles page row becomes a LazyBubble without
 * ever touching a file. The line is stored verbatim and decoded on demand, so
 * escaped \n / \t / \\ and the per-session seq must survive exactly.
 */
#include "edc_chat_window.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int g_failed = 0;
static int g_passed = 0;

#define CHECK(cond, ...)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            fprintf(stderr, "    FAIL: ");                                                                                \
            fprintf(stderr, __VA_ARGS__);                                                                                 \
            fprintf(stderr, "\n");                                                                                         \
            g_failed++;                                                                                                    \
        }                                                                                                                  \
        else                                                                                                               \
        {                                                                                                                  \
            printf("    ok: ");                                                                                            \
            printf(__VA_ARGS__);                                                                                           \
            printf("\n");                                                                                                  \
            g_passed++;                                                                                                    \
        }                                                                                                                  \
    } while (0)

int main(void)
{
    /* Field separators are REAL tabs; the two-byte \n / \t / \\ sequences
     * below are escapes INSIDE the content field (transcript format). Mixing
     * those up is exactly what this test is here to catch. */
    const std::string tricky =
        "tool\t0\ttask-7\tcmd header\tline one\\nline two\\tand a \\\\ slash";
    const char *lines[] = {
        "user\t0\t\t\thello world",
        tricky.c_str(),
        "sys\t0\t\t\tlast",
        "garbage-without-tabs", /* must be skipped, like the file importer */
    };
    const int64_t seqs[] = {11, 12, 13, 14};

    std::vector<edc::ChatBubbleRef> refs;
    const size_t built = edc::BuildChatBubbleRefsFromPage(seqs, lines, 4, &refs);
    CHECK(built == 3, "3 decodable rows become descriptors, got %zu", built);
    CHECK(refs.size() == 3, "the undecodable row is skipped");

    if (refs.size() == 3)
    {
        CHECK(refs[0].source == edc::ChatBubbleSource::kDatabase &&
                  refs[1].source == edc::ChatBubbleSource::kDatabase &&
                  refs[2].source == edc::ChatBubbleSource::kDatabase,
              "all descriptors are kDatabase");
        CHECK(refs[0].seq == 11 && refs[1].seq == 12 && refs[2].seq == 13, "seqs preserved");
        CHECK(refs[1].line == tricky, "the encoded line is kept verbatim on the descriptor");
        CHECK(refs[0].task_id.empty() && refs[1].task_id == "task-7",
              "task_id decoded once for filtering");

        /* The real consumer: MessageWindow::Fields() must decode the stored line
         * with no file I/O and no second DB read. MessageWindow() owns an EMPTY
         * refs vector, so we hand it ours. */
        edc::MessageWindow window(&refs);
        std::string kind, task_id, header, content;
        int render_pref = -1;
        CHECK(window.Fields(1, &kind, &render_pref, &task_id, &header, &content),
              "Fields() decodes a kDatabase row");
        CHECK(kind == "tool" && render_pref == 0, "kind / render_pref decoded");
        CHECK(task_id == "task-7", "task_id decoded");
        CHECK(header == "cmd header", "header decoded");
        CHECK(content == "line one\nline two\tand a \\ slash",
              "escaped content survives verbatim (got '%s')", content.c_str());

        /* Out-of-range is an honest failure, not a crash. */
        CHECK(!window.Fields(99, &kind, &render_pref, &task_id, &header, &content),
              "an out-of-range index is refused");
    }

    printf("\n%d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
