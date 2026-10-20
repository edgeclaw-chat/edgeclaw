#include "edc_token_style.hpp"

#include "edc_payload.h"
#include "edc_style.hpp"
#include <cstring>

namespace edc
{
namespace
{
// The mention pass's notion of whitespace (edc_pass_mentions.c: is_space).
bool IsSpace(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

bool IsDigit(char c)
{
    return c >= '0' && c <= '9';
}

// Trailing ASCII punctuation is not part of a reference: "看下 @a.c." names a.c.
bool IsTrailingPunct(char c)
{
    return c != '\0' && strchr(",.):;!?]}", c) != nullptr;
}

// `[Pasted #<n> · <m> line(s)]` starting at |at| (the '['): returns the byte length, or 0
// when this is not a label. The middle dot is UTF-8 (C2 B7) because that is what the
// editor writes.
size_t PasteLabelAt(const std::string &text, size_t at)
{
    static const char kPrefix[] = "[Pasted #";
    const size_t prefix_len = sizeof(kPrefix) - 1;
    if (text.compare(at, prefix_len, kPrefix) != 0)
        return 0;

    size_t p = at + prefix_len;
    if (p >= text.size() || !IsDigit(text[p]))
        return 0;
    while (p < text.size() && IsDigit(text[p]))
        p++;

    // " · "  (space, U+00B7, space)
    if (text.compare(p, 4, " \xc2\xb7 ") != 0)
        return 0;
    p += 4;

    if (p >= text.size() || !IsDigit(text[p]))
        return 0;
    while (p < text.size() && IsDigit(text[p]))
        p++;

    if (text.compare(p, 5, " line") != 0)
        return 0;
    p += 5;
    if (p < text.size() && text[p] == 's') // 1 line / 2 lines
        p++;
    if (p >= text.size() || text[p] != ']')
        return 0;
    return p + 1 - at;
}

// An `@reference` starting at |at| (the '@'): returns the byte length up to the next
// whitespace, minus trailing punctuation. 0 = not a reference (or nothing after the '@').
size_t MentionAt(const std::string &text, size_t at)
{
    size_t stop = at;
    while (stop < text.size() && !IsSpace(text[stop]))
        stop++;
    size_t end = stop;
    while (end > at + 1 && IsTrailingPunct(text[end - 1]))
        end--;
    // Just "@" is not a reference (the pass requires something after it).
    return end > at + 1 ? end - at : 0;
}
} // namespace

std::vector<TokenSpan> ScanTokens(const std::string &text)
{
    std::vector<TokenSpan> spans;
    const size_t n = text.size();
    size_t i = 0;
    while (i < n)
    {
        // `\@` and friends: the backslash escapes the next byte, which is then plain text.
        if (text[i] == '\\' && i + 1 < n)
        {
            i += 2;
            continue;
        }
        if (text[i] == '[')
        {
            if (const size_t len = PasteLabelAt(text, i))
            {
                spans.push_back({i, len, true});
                i += len;
                continue;
            }
        }
        // A reference only starts a token at the line start or after whitespace, so
        // `foo@bar.com` and `@Override` stay words.
        if (text[i] == '@' && (i == 0 || IsSpace(text[i - 1])))
        {
            if (const size_t len = MentionAt(text, i))
            {
                spans.push_back({i, len, false});
                i += len;
                continue;
            }
        }
        i++;
    }
    return spans;
}

std::string FoldPastedText(std::string text, const std::string &payload_json)
{
    struct edc_payload *payload = edc_payload_from_json(payload_json.c_str());
    if (edc_payload_empty(payload))
    {
        edc_payload_free(payload);
        return text;
    }
    /* Each value is folded once, in order: the same passage pasted twice has two keys, and
     * replacing them in sequence keeps them apart. */
    size_t from = 0;
    for (int i = 0; i < payload->n; ++i)
    {
        const char *key = payload->items[i].key;
        const char *bytes = payload->items[i].text;
        if (!key || !*key || !bytes || !*bytes)
            continue;
        const size_t at = text.find(bytes, from);
        if (at == std::string::npos)
            continue;
        text.replace(at, strlen(bytes), key);
        from = at + strlen(key);
    }
    edc_payload_free(payload);
    return text;
}

::ftxui::Element StyleToken(::ftxui::Element element, bool paste)
{
    if (paste)
        return element | ::ftxui::color(style::input_paste_text()) | ::ftxui::bgcolor(style::input_paste_surface());
    return element | ::ftxui::color(style::input_mention_text());
}

} // namespace edc
