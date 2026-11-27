/* Wire-shape regression tests for the chat-bubble page protocol.
 *
 * A silently lossy codec is the classic failure here: a transcript line with an
 * escaped \n / \t / \\ that comes back decoded would corrupt tool output on
 * replay, and a cursor that drifts drops or duplicates bubbles while scrolling.
 * These tests pin the exact bytes and the cursor echo.
 */
#include "edc_chat_bubble_page.hpp"
#include "edc_chat_transcript.hpp"

#include <cstdio>
#include <cstring>
#include <string>

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
    const std::string tricky = edc::EncodeTranscriptLine([] {
        edc::ChatTranscriptEntry e;
        e.kind = "tool"; e.render_pref = 0; e.task_id = "t1"; e.header = "hdr";
        e.content = "line one\nline two\tand a backslash \\ end";
        return e;
    }());

    printf("-- request round trip --\n");
    {
        edc::ChatBubblePageRequest req;
        req.session_id = "sess-42";
        req.before_seq = 1234567890123LL;
        req.limit = 77;
        std::string json = edc::EncodeChatBubblePageRequest(req);
        edc::ChatBubblePageRequest got;
        CHECK(edc::DecodeChatBubblePageRequest(json.c_str(), &got), "request decodes");
        CHECK(got.session_id == req.session_id && got.before_seq == req.before_seq && got.limit == req.limit,
              "session_id / before_seq / limit survive the round trip");
        CHECK(!edc::DecodeChatBubblePageRequest("{", &got), "malformed request is refused");
        CHECK(!edc::DecodeChatBubblePageRequest("{\"limit\":10}", &got),
              "a request without session_id is refused");
        CHECK(!edc::DecodeChatBubblePageRequest("{\"session_id\":\"s\",\"before_seq\":1,\"limit\":0}", &got),
              "a zero limit is refused");
    }

    printf("-- reply round trip (verbatim lines + cursor) --\n");
    {
        const char *lines[] = {"sys\\t0\\t\\t\\tfirst", tricky.c_str(), "reply\\t0\\t\\t\\tthird"};
        const int64_t seqs[] = {7, 8, 9};
        std::string json = edc::EncodeChatBubblePageReply("sess-42", 7, lines, seqs, 3, true);
        edc::ChatBubblePageReply got;
        CHECK(edc::DecodeChatBubblePageReply("sess-42", json.c_str(), &got), "reply decodes");
        CHECK(got.session_id == "sess-42", "header becomes session_id");
        CHECK(got.before_seq == 7, "the cursor is echoed");
        CHECK(got.has_more, "has_more survives");
        CHECK(got.rows.size() == 3, "3 rows, got %zu", got.rows.size());
        if (got.rows.size() == 3)
        {
            CHECK(got.rows[0].seq == 7 && got.rows[1].seq == 8 && got.rows[2].seq == 9,
                  "seqs preserved in order");
            CHECK(got.rows[1].kind == "tool" && got.rows[1].render_pref == 0 &&
                      got.rows[1].task_id == "t1" && got.rows[1].header == "hdr" &&
                      got.rows[1].content == "line one\nline two\tand a backslash \\ end" &&
                      got.rows[1].highlight_lang.empty(),
                  "the tool line's fields survive the JSON round trip byte-for-byte");
        }
    }

    printf("-- a giant cursor must not survive a double round trip --\n");
    {
        /* A previous version used INT64_MAX as the "newest page" sentinel.
         * cJSON stores numbers as doubles: (double)INT64_MAX rounds up to
         * 2^63, and casting that back to int64_t is UB -- on amd64 it becomes
         * INT64_MIN, so the DB query `seq < before` returns ZERO rows.
         * The wire now uses 0 for "newest" and clamps anything unrepresentable. */
        edc::ChatBubblePageRequest req;
        req.session_id = "s1";
        req.before_seq = INT64_MAX; /* an old caller / malformed input */
        req.limit = 10;
        std::string json = edc::EncodeChatBubblePageRequest(req);
        edc::ChatBubblePageRequest got;
        CHECK(edc::DecodeChatBubblePageRequest(json.c_str(), &got), "the giant cursor still decodes");
        CHECK(got.before_seq == 0, "the giant cursor clamps to 0 (newest page), got %lld",
              (long long)got.before_seq);
        CHECK(got.before_seq >= 0, "no negative cursor can reach the DB query");
    }

    printf("-- empty / degenerate replies --\n");
    {
        std::string json = edc::EncodeChatBubblePageReply("s", INT64_MAX, nullptr, nullptr, 0, false);
        edc::ChatBubblePageReply got;
        CHECK(edc::DecodeChatBubblePageReply("s", json.c_str(), &got), "an empty page decodes");
        CHECK(got.rows.empty() && !got.has_more, "empty page is empty and has_more is false");
        CHECK(!edc::DecodeChatBubblePageReply(nullptr, "{", &got), "malformed reply is refused");
    }

    printf("\n%d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
