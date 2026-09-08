#pragma once

#include <ftxui/screen/string.hpp>
#include <string>
#include <utility>
#include <vector>

namespace edc
{

/* UTF-8 / 终端宽度工具，右侧栏各 widget（任务树、todo 列表）共用。 */

// Truncate by UTF-8 code points so a multi-byte character is never cut in
// half (which would otherwise render as a replacement/garbage glyph).
inline std::string TruncateUtf8(const std::string &s, size_t max_codepoints)
{
    if (s.empty())
        return s;

    size_t byte_pos = 0;
    size_t count = 0;
    while (byte_pos < s.size() && count < max_codepoints)
    {
        const unsigned char c = static_cast<unsigned char>(s[byte_pos]);
        size_t len = 1;
        if (c >= 0xF0)
            len = 4;
        else if (c >= 0xE0)
            len = 3;
        else if (c >= 0xC0)
            len = 2;

        // A truncated or overlong sequence ends the scan at the last whole
        // code point.
        if (byte_pos + len > s.size())
            break;

        byte_pos += len;
        count++;
    }

    if (byte_pos >= s.size())
        return s;

    return s.substr(0, byte_pos) + "…";
}

// Number of terminal cells |s| occupies when printed. Wide characters (CJK
// fullwidth, emoji, …) count as two cells — this is what ftxui itself uses
// when laying out and rendering text, so it is the right unit for deciding
// how much of a description fits on a row.
inline int StringWidth(const std::string &s)
{
    return ::ftxui::string_width(s);
}

// Truncate |s| so it occupies at most |max_cells| terminal cells, appending
// "…" when something had to be cut. Truncation happens on UTF-8 code-point
// boundaries and accounts for wide characters, so the result really fits:
// the caller no longer has to rely on flex-shrink, which would hard-clip the
// text flush against whatever follows it.
inline std::string TruncateToWidth(const std::string &s, int max_cells)
{
    if (max_cells <= 0)
        return std::string();
    if (StringWidth(s) <= max_cells)
        return s;

    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    int used = 0;
    while (i < s.size())
    {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        size_t len = 1;
        if (c >= 0xF0)
            len = 4;
        else if (c >= 0xE0)
            len = 3;
        else if (c >= 0xC0)
            len = 2;

        // A truncated or overlong sequence ends the scan at the last whole
        // code point.
        if (i + len > s.size())
            break;

        // Reserve the last cell for the ellipsis.
        const int w = StringWidth(s.substr(i, len));
        if (used + w > max_cells - 1)
            break;

        out.append(s, i, len);
        used += w;
        i += len;
    }
    out += "…";
    return out;
}

// Byte length of the UTF-8 sequence starting at the first byte |c|.
inline size_t Utf8SeqLen(unsigned char c)
{
    if (c >= 0xF0)
        return 4;
    if (c >= 0xE0)
        return 3;
    if (c >= 0xC0)
        return 2;
    return 1;
}

// Greedy word-wrap that is safe for CJK text. Words are split on ASCII
// whitespace, but a word longer than a whole line (typically an unbroken CJK
// run, a long path or a URL) is hard-broken between code points, so every
// returned line occupies at most |max_cells| terminal cells. Whitespace runs
// collapse to a single space between words; leading/trailing whitespace is
// dropped. Empty input, or a non-positive budget, yields no lines — callers
// decide what to draw when nothing fits.
inline std::vector<std::string> WrapToWidth(const std::string &s, int max_cells)
{
    std::vector<std::string> lines;
    if (max_cells <= 0 || s.empty())
        return lines;

    // 1. Split the text on whitespace into words (whole UTF-8 sequences).
    std::vector<std::string> words;
    std::string word;
    size_t i = 0;
    while (i < s.size())
    {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v')
        {
            if (!word.empty())
            {
                words.push_back(std::move(word));
                word.clear();
            }
            ++i;
            continue;
        }
        size_t len = Utf8SeqLen(c);
        if (i + len > s.size())
            len = s.size() - i; // truncated tail: take what remains
        word.append(s, i, len);
        i += len;
    }
    if (!word.empty())
        words.push_back(std::move(word));
    if (words.empty())
        return lines;

    // 2. Pre-split words wider than a line into segments that each fit.
    std::vector<std::string> segs;
    segs.reserve(words.size());
    for (const std::string &w : words)
    {
        if (StringWidth(w) <= max_cells)
        {
            segs.push_back(w);
            continue;
        }
        std::string chunk;
        int used = 0;
        size_t k = 0;
        while (k < w.size())
        {
            const unsigned char c = static_cast<unsigned char>(w[k]);
            size_t len = Utf8SeqLen(c);
            if (k + len > w.size())
                len = w.size() - k;
            const std::string piece = w.substr(k, len);
            const int cw = StringWidth(piece);
            if (cw > max_cells)
            {
                // A single glyph wider than the whole line (degenerate very
                // narrow budgets): emit it alone rather than loop forever.
                if (!chunk.empty())
                {
                    segs.push_back(std::move(chunk));
                    chunk.clear();
                    used = 0;
                }
                segs.push_back(piece);
                k += len;
                continue;
            }
            if (used + cw > max_cells)
            {
                segs.push_back(std::move(chunk));
                chunk.clear();
                used = 0;
            }
            chunk.append(piece);
            used += cw;
            k += len;
        }
        if (!chunk.empty())
            segs.push_back(std::move(chunk));
    }

    // 3. Greedy fill: join segments with single spaces, flush when full.
    std::string line;
    int lw = 0;
    for (const std::string &seg : segs)
    {
        const int sw = StringWidth(seg);
        if (lw == 0)
        {
            line = seg;
            lw = sw;
            continue;
        }
        if (lw + 1 + sw <= max_cells)
        {
            line += ' ';
            line += seg;
            lw += 1 + sw;
        }
        else
        {
            lines.push_back(std::move(line));
            line = seg;
            lw = sw;
        }
    }
    if (!line.empty())
        lines.push_back(std::move(line));
    return lines;
}

// Soft (visual) line wrap for an editable line: returns the byte ranges
// [begin, end) of the rows |s| is split into when displayed at most
// |max_cells| wide. Unlike WrapToWidth this is a *lossless partition* of the
// text for an editor — no whitespace is collapsed or trimmed, and the ranges
// concatenate back to exactly |s| except that the single space sitting on a
// wrap boundary (the separator between two words when the row is exactly
// full) is dropped from both rows. It is CJK/wide-char safe: a run without
// spaces (Chinese, a long URL, …) is hard-broken between code points, never
// in the middle of one. A trailing ASCII space at a word-wrap boundary stays
// at the end of the previous row (invisible, but the byte range stays
// contiguous so a caret sitting on it maps back cleanly). Empty input yields
// no rows — callers wanting to display an empty line add one themselves.
inline std::vector<std::pair<int, int>> VisualWrapRows(const std::string &s, int max_cells)
{
    std::vector<std::pair<int, int>> rows;
    if (max_cells <= 0)
    {
        if (!s.empty())
            rows.push_back({0, static_cast<int>(s.size())});
        return rows;
    }

    const int n = static_cast<int>(s.size());
    int seg = 0;
    while (seg < n)
    {
        int width = 0;
        int last_space = -1; // byte index of the last ' ' that fits this row
        int j = seg;
        int row_end = n;
        bool skip_space = false; // drop the overflowing boundary space
        while (j < n)
        {
            const unsigned char c = static_cast<unsigned char>(s[j]);
            size_t len = Utf8SeqLen(c);
            if (j + static_cast<int>(len) > n)
                len = static_cast<size_t>(n - j); // truncated tail
            const int w = StringWidth(s.substr(j, len));
            if (w > max_cells)
            {
                // A single glyph wider than a whole row (degenerate narrow
                // budget): emit it alone rather than looping forever.
                if (width == 0)
                {
                    row_end = j + static_cast<int>(len);
                }
                else
                {
                    row_end = j;
                }
                break;
            }
            if (width + w > max_cells)
            {
                if (c == ' ' && width == max_cells)
                {
                    // Row exactly full, next char is the word separator:
                    // end here and skip the boundary space.
                    row_end = j;
                    skip_space = true;
                }
                else if (last_space != -1)
                {
                    // Word wrap: end after the last space that fit, so the
                    // next row starts at the following word.
                    row_end = last_space + 1;
                }
                else
                {
                    // No interior space (CJK run / long word): hard break.
                    row_end = j;
                }
                break;
            }
            width += w;
            if (c == ' ')
                last_space = j;
            j += static_cast<int>(len);
        }
        if (row_end > seg)
        {
            rows.push_back({seg, row_end});
            seg = row_end + (skip_space ? 1 : 0);
        }
        else
        {
            // Safety net: always make progress.
            rows.push_back({seg, j});
            seg = j > seg ? j : seg + 1;
        }
    }
    return rows;
}

} // namespace edc
