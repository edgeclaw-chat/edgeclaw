#include "edc_chat_transcript.hpp"

#include <cstdlib>
#include <fstream>
#include <iterator>

namespace edc
{
namespace
{

/* A field must not contain a raw tab (it separates the columns) nor a newline
 * (it would split one bubble into two lines), so both are escaped. */
std::string EscapeField(const std::string &s)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s)
    {
        switch (c)
        {
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            out += c;
        }
    }
    return out;
}

std::string UnescapeField(const std::string &s)
{
    std::string out;
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
    return out;
}

} // namespace

std::string EncodeTranscriptLine(const ChatTranscriptEntry &entry)
{
    return EscapeField(entry.kind) + "\t" + std::to_string(entry.render_pref) + "\t" +
           EscapeField(entry.task_id) + "\t" + EscapeField(entry.header) + "\t" + EscapeField(entry.content) + "\n";
}

bool DecodeTranscriptLine(const std::string &raw, ChatTranscriptEntry *out)
{
    if (!out)
        return false;

    std::string line = raw;
    if (!line.empty() && line.back() == '\n')
        line.pop_back();
    if (!line.empty() && line.back() == '\r')
        line.pop_back();
    if (line.empty())
        return false;

    /* Split on the first four raw tabs; escaped tabs are not raw. Only the last
     * field may contain raw tabs (a bubble written by a build that did not
     * escape them), which keeps such a line readable instead of dropping it. */
    std::string parts[5];
    int field = 0;
    for (char c : line)
    {
        if (c == '\t' && field < 4)
        {
            field++;
            continue;
        }
        parts[field] += c;
    }
    if (field < 4)
        return false; /* fewer columns than a bubble: a truncated tail */

    ChatTranscriptEntry entry;
    entry.kind = UnescapeField(parts[0]);
    entry.render_pref = std::atoi(parts[1].c_str());
    entry.task_id = UnescapeField(parts[2]);
    entry.header = UnescapeField(parts[3]);
    entry.content = UnescapeField(parts[4]);
    if (entry.kind.empty())
        return false;

    *out = entry;
    return true;
}

size_t LoadTranscript(const std::string &path, const std::function<void(const ChatTranscriptEntry &)> &emit)
{
    if (path.empty() || !emit)
        return 0;

    std::ifstream file(path, std::ios::binary);
    if (!file)
        return 0;

    const std::string data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

    /* Only newline-terminated lines are whole bubbles; anything after the last
     * newline is the fragment of an append that never completed. */
    const size_t last_nl = data.rfind('\n');
    if (last_nl == std::string::npos)
        return 0;

    size_t count = 0;
    size_t pos = 0;
    while (pos <= last_nl)
    {
        const size_t nl = data.find('\n', pos);
        if (nl == std::string::npos || nl > last_nl)
            break;
        ChatTranscriptEntry entry;
        if (DecodeTranscriptLine(data.substr(pos, nl - pos), &entry))
        {
            emit(entry);
            count++;
        }
        pos = nl + 1;
    }
    return count;
}

bool TranscriptEndsMidLine(const std::string &path)
{
    if (path.empty())
        return false;

    std::ifstream file(path, std::ios::binary);
    if (!file)
        return false;

    file.seekg(0, std::ios::end);
    const std::streamoff size = file.tellg();
    if (size <= 0)
        return false;
    file.seekg(size - 1, std::ios::beg);
    char last = '\0';
    if (!file.get(last))
        return false;
    return last != '\n';
}

} // namespace edc
