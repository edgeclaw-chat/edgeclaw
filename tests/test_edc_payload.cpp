/* Unit tests for the folded-paste payload and the text-layer pass chain.
 *
 * These two pieces are the whole reason a folded block never changes what a line means:
 *   - edc_payload_expand() is a pure, single-pass, non-recursive replacement;
 *   - edc_text_passes_run() is what the app runs on every user input before classifying
 *     it, so the bytes the lexer sees are the bytes a typed line would have produced.
 * The interesting cases here are the ones where a naive implementation would rewrite the
 * pasted text itself (a paste that looks like a placeholder, or like an `@file`). */

#include "edc_payload.h"
#include "edc_text_passes.h"

#include <cstring>
#include <string>

static int g_failures = 0;

#define CHECK(cond, ...)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            std::printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                            \
            std::printf("    ");                                                                                       \
            std::printf(__VA_ARGS__);                                                                                  \
            std::printf("\n");                                                                                         \
            g_failures++;                                                                                              \
        }                                                                                                              \
    } while (0)

/* Owned string helper: expanded text comes back as a char* freed by
 * edc_payload_string_free. */
static std::string expand(const std::string &text, const struct edc_payload *payload)
{
    char *out = edc_payload_expand(text.c_str(), payload);
    std::string result = out ? out : "";
    edc_payload_string_free(out);
    return result;
}

static std::string finalize(const std::string &text, const struct edc_payload *payload)
{
    char *out = edc_text_passes_run(text.c_str(), payload);
    std::string result = out ? out : "";
    edc_payload_string_free(out);
    return result;
}

static void test_payload_map(void)
{
    std::printf("  payload: set / get / remove keep the insertion order\n");
    struct edc_payload *payload = edc_payload_new();
    CHECK(edc_payload_empty(payload), "a new payload is empty");

    edc_payload_set(payload, "[Pasted #1 \xc2\xb7 2 lines]", "a\nb");
    edc_payload_set(payload, "[Pasted #2 \xc2\xb7 4 lines]", "c");
    CHECK(edc_payload_count(payload) == 2, "two entries, got %d", edc_payload_count(payload));
    CHECK(strcmp(edc_payload_get(payload, "[Pasted #2 \xc2\xb7 4 lines]"), "c") == 0, "get returns the text");

    /* Overwriting keeps the position, so blocks never reorder under the user. */
    edc_payload_set(payload, "[Pasted #1 \xc2\xb7 2 lines]", "a\nB");
    CHECK(edc_payload_count(payload) == 2, "overwrite does not add an entry");
    CHECK(strcmp(payload->items[0].key, "[Pasted #1 \xc2\xb7 2 lines]") == 0, "the first key stayed first");

    CHECK(edc_payload_remove(payload, "[Pasted #1 \xc2\xb7 2 lines]"), "remove reports success");
    CHECK(edc_payload_count(payload) == 1, "and drops the entry");
    CHECK(!edc_payload_remove(payload, "nope"), "removing an absent key is false");

    edc_payload_free(payload);
}

static void test_expand_is_single_pass(void)
{
    std::printf("  expand: replaces the key, never re-scans what it inserted\n");
    struct edc_payload *payload = edc_payload_new();
    edc_payload_set(payload, "[Pasted #1 \xc2\xb7 3 lines]", "code @src/main.c and {{LAST}}");

    CHECK(expand("看下 [Pasted #1 \xc2\xb7 3 lines] 什么意思", payload) ==
              "看下 code @src/main.c and {{LAST}} 什么意思",
          "the key is replaced, the body is not touched");

    /* Two occurrences: both go (the same key can be typed/pasted twice). */
    CHECK(expand("[Pasted #1 \xc2\xb7 3 lines]+[Pasted #1 \xc2\xb7 3 lines]", payload) ==
              "code @src/main.c and {{LAST}}+code @src/main.c and {{LAST}}",
          "every occurrence is replaced");

    /* Idempotent: nothing is left to match, so a second pass is a copy. */
    const std::string once = expand("[Pasted #1 \xc2\xb7 3 lines]", payload);
    CHECK(expand(once, payload) == once, "a second expansion is a no-op");

    /* No payload: the text is copied as-is. */
    CHECK(expand("plain", NULL) == "plain", "a NULL payload copies the text");
    edc_payload_free(payload);
}

static void test_expand_longest_key_first(void)
{
    std::printf("  expand: a key that is a prefix of another does not steal the match\n");
    struct edc_payload *payload = edc_payload_new();
    edc_payload_set(payload, "[Pasted #1 \xc2\xb7 2 lines]", "SHORT");
    edc_payload_set(payload, "[Pasted #1 \xc2\xb7 2 lines] (whole file)", "LONG");

    CHECK(expand("[Pasted #1 \xc2\xb7 2 lines] (whole file)", payload) == "LONG", "the longer key wins");
    CHECK(expand("[Pasted #1 \xc2\xb7 2 lines] tail", payload) == "SHORT tail", "the shorter one still matches alone");
    edc_payload_free(payload);
}

static void test_json_round_trip(void)
{
    std::printf("  payload: JSON round trip (the on-disk shape)\n");
    CHECK(edc_payload_to_json(NULL) == NULL, "an empty payload writes nothing (no \"{}\" on every message)");

    struct edc_payload *payload = edc_payload_new();
    edc_payload_set(payload, "[Pasted #1 \xc2\xb7 2 lines]", "line1\nline2 \"quoted\"\\");
    char *json = edc_payload_to_json(payload);
    CHECK(json != NULL, "a non-empty payload prints");

    struct edc_payload *back = edc_payload_from_json(json);
    CHECK(back && edc_payload_count(back) == 1, "one entry comes back");
    CHECK(back && strcmp(edc_payload_get(back, "[Pasted #1 \xc2\xb7 2 lines]"), "line1\nline2 \"quoted\"\\") == 0,
          "the bytes survive the trip (newline and quotes included)");

    edc_payload_free(back);
    edc_payload_string_free(json);
    edc_payload_free(payload);

    CHECK(edc_payload_from_json("not json") == NULL, "bad JSON reads as 'no payload'");
    CHECK(edc_payload_from_json(NULL) == NULL, "NULL reads as 'no payload'");
    CHECK(edc_payload_from_json("{}") == NULL, "an empty object is the same as none");
}

static void test_text_passes_run(void)
{
    std::printf("  text passes: the final line is what the lexer sees\n");

    /* No payload: the fast path must return the input unchanged. */
    CHECK(finalize("+do \"x\"", NULL) == "+do \"x\"", "no payload -> unchanged");

    struct edc_payload *payload = edc_payload_new();
    edc_payload_set(payload, "[Pasted #1 \xc2\xb7 3 lines]", "+汇总 \"{{LAST}} 和这段\"");
    /* A folded block inside a shell line expands BEFORE classification, so the line is a
     * shell line and the reference inside the pasted bytes is a real dependency. */
    CHECK(finalize("+do \"看下 [Pasted #1 \xc2\xb7 3 lines]\"", payload) ==
              "+do \"看下 +汇总 \"{{LAST}} 和这段\"\"",
          "the paste is expanded inside the quoted word");
    edc_payload_free(payload);
}

static void test_unreferenced_keys(void)
{
    std::printf("  payload: placeholders the draft no longer contains are reported\n");
    struct edc_payload *payload = edc_payload_new();
    edc_payload_set(payload, "[Pasted #1 \xc2\xb7 2 lines]", "A");
    edc_payload_set(payload, "[Pasted #2 \xc2\xb7 9 lines]", "B");

    const char *gone[4] = {0};
    /* The draft still shows block #1; block #2's placeholder is gone (edited away). */
    CHECK(edc_payload_unreferenced("[Pasted #1 \xc2\xb7 2 lines] text", payload, gone, 4) == 1,
          "only the unreferenced entry is reported");
    CHECK(gone[0] && strcmp(gone[0], "[Pasted #2 \xc2\xb7 9 lines]") == 0, "and it is the right one");

    const char *none[4] = {0};
    CHECK(edc_payload_unreferenced("[Pasted #1 \xc2\xb7 2 lines]+[Pasted #2 \xc2\xb7 9 lines]", payload, none,
                                   4) == 0,
          "both placeholders present -> nothing to report");
    edc_payload_free(payload);
}

int main(void)
{
    test_payload_map();
    test_expand_is_single_pass();
    test_expand_longest_key_first();
    test_json_round_trip();
    test_text_passes_run();
    test_unreferenced_keys();

    if (g_failures)
    {
        std::printf("%d check(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("all payload/text-pass tests passed\n");
    return 0;
}
