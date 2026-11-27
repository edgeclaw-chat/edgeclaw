#include "edc_chat_transcript.hpp"
#include "edc_chat_bubble_line.h"  /* the one shared encoder */

#include <cstdlib>
#include <string_view>

namespace edc
{
namespace
{

/* Append the unescaped form of |s| to |out|. */
void UnescapeInto(std::string_view s, std::string &out)
{
    out.clear();
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++)
    {
        if (s[i] != '\\' || i + 1 >= s.size())
        {
            out += s[i];
            continue;
        }
        const char n = s[++i];
        switch (n)
        {
        case 'n':
            out += '\n';
            break;
        case 'r':
            out += '\r';
            break;
        case 't':
            out += '\t';
            break;
        default:
            out += n; /* covers "\\" and any stray escape */
        }
    }
}

/* Integer parse matching std::atoi on the decimal fields we write (no allocation). */
int ParseRenderPref(std::string_view s)
{
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t'))
        i++;
    bool negative = false;
    if (i < s.size() && (s[i] == '+' || s[i] == '-'))
        negative = (s[i++] == '-');
    int value = 0;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9')
        value = value * 10 + (s[i++] - '0');
    return negative ? -value : value;
}

/* Decode one line from a view: no per-line string copy. Same contract as
 * DecodeTranscriptLine (which is a thin wrapper over this). */
bool DecodeLineView(std::string_view line, ChatTranscriptEntry *out)
{
    if (!out)
        return false;

    if (!line.empty() && line.back() == '\n')
        line.remove_suffix(1);
    if (!line.empty() && line.back() == '\r')
        line.remove_suffix(1);
    if (line.empty())
        return false;

    /* Split on up to five raw tabs; escaped tabs are not raw. Four tabs = legacy
     * five-field line (lang empty), five tabs = six-field line with lang. */
    std::string_view parts[6];
    size_t start = 0;
    int field = 0;
    for (size_t i = 0; i < line.size(); i++)
    {
        if (line[i] == '\t' && field < 5)
        {
            parts[field++] = line.substr(start, i - start);
            start = i + 1;
        }
    }
    if (field < 4)
        return false; /* fewer columns than a bubble: a truncated line */
    parts[field] = line.substr(start);
    const std::string_view content_part = parts[4];
    const std::string_view lang_part = (field == 5) ? parts[5] : std::string_view();

    ChatTranscriptEntry entry;
    UnescapeInto(parts[0], entry.kind);
    entry.render_pref = ParseRenderPref(parts[1]);
    UnescapeInto(parts[2], entry.task_id);
    UnescapeInto(parts[3], entry.header);
    UnescapeInto(content_part, entry.content);
    UnescapeInto(lang_part, entry.highlight_lang);
    if (entry.kind.empty())
        return false;

    *out = std::move(entry);
    return true;
}

} // namespace

std::string EncodeTranscriptLine(const ChatTranscriptEntry &entry)
{
    /* The channel actor writes the SAME line into chat_bubbles; using one C
     * encoder on both sides is what guarantees a bubble survives a restart
     * byte-for-byte. */
    char *raw = edc_chat_bubble_encode(entry.kind.c_str(), entry.render_pref, entry.task_id.c_str(),
                                       entry.header.c_str(), entry.content.c_str(),
                                       entry.highlight_lang.c_str());
    if (!raw)
        return {};
    std::string out(raw);
    free(raw);
    out.push_back('\n');
    return out;
}

bool DecodeTranscriptLine(const std::string &raw, ChatTranscriptEntry *out)
{
    return DecodeLineView(std::string_view(raw), out);
}

} // namespace edc
