#include "edc_history_line.hpp"

namespace edc
{
namespace
{
// Backslash and the characters that would break the one-line format. Tab is escaped too:
// it is the mid/text separator.
std::string Escape(const std::string &text)
{
    std::string out;
    out.reserve(text.size());
    for (char c : text)
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
            break;
        }
    }
    return out;
}

std::string Unescape(const std::string &text)
{
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i)
    {
        if (text[i] != '\\')
        {
            out += text[i];
            continue;
        }
        if (i + 1 >= text.size())
            break; // dangling escape at EOF: drop it
        const char n = text[++i];
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
        case '\\':
            out += '\\';
            break;
        default:
            // Unknown escape: keep both bytes verbatim.
            out += '\\';
            out += n;
            break;
        }
    }
    return out;
}
} // namespace

bool LooksLikeMid(const std::string &candidate)
{
    if (candidate.empty() || candidate.size() > 64 || candidate.find('#') == std::string::npos)
        return false;
    for (char c : candidate)
    {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '#' ||
                        c == '_' || c == '.' || c == ':' || c == '-';
        if (!ok)
            return false;
    }
    return true;
}

std::string EncodeHistoryLine(const std::string &mid, const std::string &text)
{
    if (mid.empty())
        return Escape(text);
    return mid + "\t" + Escape(text);
}

HistoryLine DecodeHistoryLine(const std::string &line)
{
    HistoryLine out;
    const size_t tab = line.find('\t');
    if (tab != std::string::npos && LooksLikeMid(line.substr(0, tab)))
    {
        out.mid = line.substr(0, tab);
        out.text = Unescape(line.substr(tab + 1));
        return out;
    }
    out.text = Unescape(line); // written before mids existed (or a hand-edited file)
    return out;
}

} // namespace edc
