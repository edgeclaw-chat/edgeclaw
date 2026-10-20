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

// One slot per process: the frontend installs it once (TUIWindow), tests install their own,
// and nothing has to be threaded through the widget tree to reach it.
std::function<bool(const std::string &)> &mention_path_check_slot()
{
    static std::function<bool(const std::string &)> check;
    return check;
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

// A mention is finished once nothing that could still belong to it follows: whitespace, the
// stripped punctuation (the pass ends the reference before it), or -- in a display context
// with no caret -- the end of the text.
static bool ClosedAfter(const std::string &text, size_t end, bool end_closes)
{
    if (end >= text.size())
        return end_closes;
    return IsSpace(text[end]) || IsTrailingPunct(text[end]);
}

std::vector<TokenSpan> ScanTokens(const std::string &text, bool end_closes)
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
                spans.push_back({i, len, true, ClosedAfter(text, i + len, end_closes)});
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
                spans.push_back({i, len, false, ClosedAfter(text, i + len, end_closes)});
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

std::string MentionPath(const std::string &token_text)
{
    /* "@a.c#12-20" -> "a.c". Same rule as the pass's split_range(): the '#' only opens a
     * range when what follows is digits or digits-digits, so a name that merely contains a
     * '#' keeps it. */
    std::string path = token_text.empty() || token_text[0] != '@' ? token_text : token_text.substr(1);
    const size_t hash = path.rfind('#');
    if (hash == std::string::npos || hash + 1 >= path.size())
        return path;

    size_t i = hash + 1;
    if (!IsDigit(path[i]))
        return path;
    while (i < path.size() && IsDigit(path[i]))
        i++;
    if (i < path.size() && path[i] == '-')
    {
        i++;
        while (i < path.size() && IsDigit(path[i]))
            i++;
    }
    if (i != path.size())
        return path; // not a range: the '#' belongs to the name
    return path.substr(0, hash);
}

void SetMentionPathCheck(MentionPathCheck check)
{
    mention_path_check_slot() = std::move(check);
}

bool MentionIsMarked(const TokenSpan &span, const std::string &token_text)
{
    if (span.paste)
        return true; // a folded block is a block whether or not its bytes are back yet
    if (!span.closed)
        return false; // still being typed: not a reference yet
    const MentionPathCheck &check = mention_path_check_slot();
    return check && check(MentionPath(token_text));
}

::ftxui::Element StyleToken(::ftxui::Element element, bool paste)
{
    if (paste)
        return element | ::ftxui::color(style::input_paste_text()) | ::ftxui::bgcolor(style::input_paste_surface());
    return element | ::ftxui::color(style::input_mention_text());
}

} // namespace edc
