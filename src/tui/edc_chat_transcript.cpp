#include "edc_chat_transcript.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string_view>

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

/* Append the unescaped form of |s| to |out|. The view overload lets the replay
 * path decode a field straight out of the file buffer instead of copying every
 * line into a temporary std::string first (that copy plus the substring split
 * made replaying a 33MB transcript take ~2s). */
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

std::string UnescapeField(const std::string &s)
{
    std::string out;
    UnescapeInto(s, out);
    return out;
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

    /* Split on the first four raw tabs; escaped tabs are not raw. Only the last
     * field may contain raw tabs (a bubble written by a build that did not
     * escape them), which keeps such a line readable instead of dropping it. */
    std::string_view parts[5];
    size_t start = 0;
    int field = 0;
    for (size_t i = 0; i < line.size(); i++)
    {
        if (line[i] == '\t' && field < 4)
        {
            parts[field++] = line.substr(start, i - start);
            start = i + 1;
        }
    }
    if (field < 4)
        return false; /* fewer columns than a bubble: a truncated tail */
    parts[4] = line.substr(start);

    ChatTranscriptEntry entry;
    UnescapeInto(parts[0], entry.kind);
    entry.render_pref = ParseRenderPref(parts[1]);
    UnescapeInto(parts[2], entry.task_id);
    UnescapeInto(parts[3], entry.header);
    UnescapeInto(parts[4], entry.content);
    if (entry.kind.empty())
        return false;

    *out = std::move(entry);
    return true;
}

/* Decode the whole lines that end at |last_nl| (a '\n' in [base, base+len)) and
 * start at |from|, appending them to |out| in file order. A line's bytes are
 * [pos, nl); the newline is not part of any field. |cancelled|, when non-null, is
 * polled periodically: true stops the decode early (what was decoded is kept). */
size_t DecodeLines(const char *base, size_t from, size_t last_nl, std::vector<ChatTranscriptEntry> &out,
                   const std::function<bool()> *cancelled)
{
    /* A call per line is needless noise; a huge history only has to yield to a
     * session switch within a batch or two of lines. */
    constexpr size_t kCancelPollLines = 256;

    size_t count = 0;
    size_t pos = from;
    size_t since_poll = 0;
    while (pos <= last_nl)
    {
        if (cancelled && ++since_poll >= kCancelPollLines)
        {
            since_poll = 0;
            if ((*cancelled)())
                break;
        }
        const char *hit = static_cast<const char *>(std::memchr(base + pos, '\n', last_nl - pos + 1));
        if (!hit)
            break;
        const size_t nl = static_cast<size_t>(hit - base);
        ChatTranscriptEntry entry;
        if (DecodeLineView(std::string_view(base + pos, nl - pos), &entry))
        {
            out.push_back(std::move(entry));
            count++;
        }
        pos = nl + 1;
    }
    return count;
}

} // namespace

std::string EncodeTranscriptLine(const ChatTranscriptEntry &entry)
{
    return EscapeField(entry.kind) + "\t" + std::to_string(entry.render_pref) + "\t" +
           EscapeField(entry.task_id) + "\t" + EscapeField(entry.header) + "\t" + EscapeField(entry.content) + "\n";
}

bool DecodeTranscriptLine(const std::string &raw, ChatTranscriptEntry *out)
{
    return DecodeLineView(std::string_view(raw), out);
}

size_t LoadTranscript(const std::string &path, const std::function<void(const ChatTranscriptEntry &)> &emit)
{
    if (path.empty() || !emit)
        return 0;

    std::ifstream file(path, std::ios::binary);
    if (!file)
        return 0;

    /* One bulk read beats std::istreambuf_iterator's per-character pull on a long
     * transcript. Keep whatever a short read produced. */
    file.seekg(0, std::ios::end);
    const std::streamoff end = file.tellg();
    if (end <= 0)
        return 0;
    std::string data(static_cast<size_t>(end), '\0');
    file.seekg(0, std::ios::beg);
    file.read(&data[0], end);
    data.resize(static_cast<size_t>(file.gcount()));

    /* Only newline-terminated lines are whole bubbles; anything after the last
     * newline is the fragment of an append that never completed. */
    const size_t last_nl = data.rfind('\n');
    if (last_nl == std::string::npos)
        return 0;

    const char *base = data.data();
    size_t count = 0;
    size_t pos = 0;
    while (pos <= last_nl)
    {
        const size_t nl = data.find('\n', pos);
        if (nl == std::string::npos || nl > last_nl)
            break;
        ChatTranscriptEntry entry;
        /* Decode straight out of the buffer: line = [pos, nl). */
        if (DecodeLineView(std::string_view(base + pos, nl - pos), &entry))
        {
            emit(entry);
            count++;
        }
        pos = nl + 1;
    }
    return count;
}

size_t LoadTranscriptTail(const std::string &path, size_t max_entries, size_t *prefix_end,
                          std::vector<ChatTranscriptEntry> *out)
{
    if (prefix_end)
        *prefix_end = 0;
    if (out)
        out->clear();
    if (path.empty() || !out || max_entries == 0)
        return 0;

    std::ifstream file(path, std::ios::binary);
    if (!file)
        return 0;
    file.seekg(0, std::ios::end);
    const std::streamoff file_size = file.tellg();
    if (file_size <= 0)
        return 0;

    /* Read backwards in chunks until the buffer holds more than |max_entries|
     * newlines (so the line that opens the tail is inside it), or the start. */
    constexpr size_t kChunk = 256 * 1024;
    std::string buf;
    std::streamoff read_from = file_size;
    size_t newlines = 0;
    while (read_from > 0 && newlines <= max_entries)
    {
        const size_t want = static_cast<size_t>(std::min<std::streamoff>(static_cast<std::streamoff>(kChunk), read_from));
        read_from -= static_cast<std::streamoff>(want);
        std::string chunk(want, '\0');
        file.seekg(read_from, std::ios::beg);
        file.read(&chunk[0], static_cast<std::streamsize>(want));
        chunk.resize(static_cast<size_t>(file.gcount()));
        buf.insert(0, chunk);
        newlines = static_cast<size_t>(std::count(buf.begin(), buf.end(), '\n'));
    }
    if (buf.empty())
        return 0;

    const size_t last_nl = buf.rfind('\n');
    if (last_nl == std::string::npos)
        return 0; /* no terminated line: nothing to replay */

    /* Walk back to the start of the |max_entries|-th line from the end. */
    size_t start = 0;
    size_t nl = last_nl;
    for (size_t i = 0; i < max_entries; i++)
    {
        const size_t prev = (nl == 0) ? std::string::npos : buf.rfind('\n', nl - 1);
        if (prev == std::string::npos)
        {
            start = 0; /* the whole buffer (and file) is the tail */
            break;
        }
        start = prev + 1;
        nl = prev;
    }

    /* Absolute offset of the tail's first line. When the whole file was read
     * (read_from == 0) a start of 0 means there is no older history. Otherwise the
     * extra newline proves a line boundary sits inside the buffer (start > 0). */
    *prefix_end = (read_from > 0) ? static_cast<size_t>(read_from) + start : start;

    return DecodeLines(buf.data(), start, last_nl, *out, nullptr);
}

size_t LoadTranscriptPrefix(const std::string &path, size_t prefix_end, std::vector<ChatTranscriptEntry> *out)
{
    return LoadTranscriptPrefix(path, prefix_end, out, std::function<bool()>{});
}

size_t LoadTranscriptPrefix(const std::string &path, size_t prefix_end, std::vector<ChatTranscriptEntry> *out,
                            const std::function<bool()> &cancelled)
{
    if (out)
        out->clear();
    if (path.empty() || !out || prefix_end == 0)
        return 0;

    std::ifstream file(path, std::ios::binary);
    if (!file)
        return 0;
    std::string data(prefix_end, '\0');
    file.read(&data[0], static_cast<std::streamsize>(prefix_end));
    data.resize(static_cast<size_t>(file.gcount()));

    const size_t last_nl = data.rfind('\n');
    if (last_nl == std::string::npos)
        return 0;
    return DecodeLines(data.data(), 0, last_nl, *out, cancelled ? &cancelled : nullptr);
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
