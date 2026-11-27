#include "markdown.hpp"
#include "edc_style.hpp"
#include "edc_syntax.h"
#include "edc_text_util.hpp"

#include <ftxui/dom/table.hpp>
#include <md4c.h>

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace ftxui;

namespace edc
{
namespace
{

struct Style
{
    bool bold = false;
    bool italic = false;
    bool code = false;
    bool strike = false;
    std::string href;
};

struct Builder
{
    virtual ~Builder() = default;
    virtual void AddInline(Element /*e*/)
    {
    }
    virtual void AddBlock(Element /*e*/)
    {
    }
    // A hard line break in inline content: markdown's two trailing spaces, or a backslash
    // before the newline. Only a block that lays inline content out as a flow has anything
    // to do here; a block that draws one line regardless (a table cell) leaves it empty.
    virtual void BreakInline()
    {
    }
    virtual Element Finish() = 0;
};

struct RootB : Builder
{
    Elements blocks;
    void AddBlock(Element e) override
    {
        blocks.push_back(std::move(e));
    }
    Element Finish() override
    {
        if (blocks.empty())
            return text("");
        return vbox(std::move(blocks));
    }
};

struct ParaB : Builder
{
    Elements words;
    /* Runs already ended by a hard break. Empty for the common single-line paragraph, so
     * Finish() returns exactly what it always did and nothing about ordinary rendering
     * changes -- only a paragraph that actually contains a hard break takes the vbox path. */
    std::vector<Elements> lines;

    void AddInline(Element e) override
    {
        words.push_back(std::move(e));
    }
    void BreakInline() override
    {
        lines.push_back(std::move(words));
        words.clear();
    }
    Element Finish() override
    {
        if (lines.empty())
        {
            if (words.empty())
                return text("");
            return hflow(std::move(words));
        }
        lines.push_back(std::move(words));
        words.clear();
        /* A hard break at the very END of a paragraph ends a line, it does not open a new
         * one: without this the vbox would draw a trailing blank row. */
        while (!lines.empty() && lines.back().empty())
            lines.pop_back();
        if (lines.empty())
            return text("");
        Elements rows;
        for (auto &line : lines)
            rows.push_back(hflow(std::move(line)));
        return vbox(std::move(rows));
    }
};

struct HeadingB : ParaB
{
    int level = 1;
    Element Finish() override
    {
        /* A heading is one banner line. md4c does not emit a break inside an ATX heading, but
         * if one ever arrived the text before it must not vanish into `lines`, which this
         * Finish() never looks at: fold the runs back into one line, in order. */
        if (!lines.empty())
        {
            Elements all;
            for (auto &line : lines)
                for (auto &w : line)
                    all.push_back(std::move(w));
            for (auto &w : words)
                all.push_back(std::move(w));
            words = std::move(all);
            lines.clear();
        }
        // Heading colors come from the central palette (edc_style.hpp), which an
        // imported vim colorscheme can replace wholesale: every ATX level keeps its
        // markdown prefix ("## ", "### ", ...) and h1 is a banner whose background
        // only hugs the text. Each level 1..6 has its own role, so a theme that defines
        // markdownH1..markdownH6 shows six distinct heading colours.
        const auto &pal = style::palette();
        Color c;
        switch (level)
        {
        case 1:
            c = pal.md_h1;
            break;
        case 2:
            c = pal.md_h2;
            break;
        case 3:
            c = pal.md_h3;
            break;
        case 4:
            c = pal.md_h4;
            break;
        case 5:
            c = pal.md_h5;
            break;
        default: // 6 and deeper
            c = pal.md_h6;
            break;
        }
        Elements els;
        if (level == 1)
        {
            // Band padding: words were given the banner background while the
            // inline text was built (State::h1); pad the two ends here.
            els.push_back(text(" ") | bgcolor(pal.md_h1_bg));
            for (auto &w : words)
                els.push_back(std::move(w));
            els.push_back(text(" ") | bgcolor(pal.md_h1_bg));
        }
        else
        {
            els.push_back(text(std::string(level, '#') + " "));
            for (auto &w : words)
                els.push_back(std::move(w));
        }
        words.clear();
        Element head = hflow(std::move(els)) | bold | color(c);
        // Blank line above and below, so the banner breathes in the flow.
        return vbox({text(""), std::move(head), text("")});
    }
};

struct CodeB : Builder
{
    std::string buf;
    std::string lang; // fenced-code info string; empty for indented/unlabelled blocks

    void AddText(std::string_view s)
    {
        buf.append(s);
    }

    // Paint one line by cutting it at the byte boundaries of the spans that
    // cover it. tree-sitter's byte offsets always land on UTF-8 code-point
    // boundaries, so substr() is safe; ftxui computes each fragment's display
    // width itself, so no manual byte->cell conversion is needed. Spans from
    // the engine are sorted and non-overlapping at identical ranges, but a
    // nested span (e.g. an identifier inside a call) may still cover a byte;
    // the innermost such span wins.
    static Element PaintLine(std::string_view line, size_t line_byte_base,
                             const edc_syntax_result &hl, const style::Palette &pal)
    {
        if (hl.count == 0)
            return text("  " + std::string(line)) | color(pal.md_code);

        // Collect cut points: line start/end plus each span boundary clipped
        // to this line.
        std::vector<size_t> cuts;
        cuts.push_back(0);
        cuts.push_back(line.size());
        struct Hit
        {
            size_t lo, hi;
            const char *group;
        };
        std::vector<Hit> hits;
        for (size_t i = 0; i < hl.count; ++i)
        {
            const edc_syntax_span &sp = hl.spans[i];
            if (sp.end_byte <= line_byte_base || sp.start_byte >= line_byte_base + line.size())
                continue;
            size_t lo = sp.start_byte > line_byte_base ? sp.start_byte - line_byte_base : 0;
            size_t hi = sp.end_byte < line_byte_base + line.size()
                            ? sp.end_byte - line_byte_base
                            : line.size();
            if (lo >= hi)
                continue;
            hits.push_back({lo, hi, edc_syntax_group_name(sp.group)});
            cuts.push_back(lo);
            cuts.push_back(hi);
        }
        if (hits.empty())
            return text("  " + std::string(line)) | color(pal.md_code);

        std::sort(cuts.begin(), cuts.end());
        cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());

        Elements parts;
        for (size_t k = 0; k + 1 < cuts.size(); ++k)
        {
            size_t lo = cuts[k], hi = cuts[k + 1];
            if (lo >= hi)
                continue;
            // The innermost span containing [lo,hi) wins: the one with the
            // largest lo (or, on a tie, the smallest hi).
            const char *group = nullptr;
            size_t best_lo = 0, best_hi = line.size() + 1;
            for (const Hit &h : hits)
            {
                if (h.lo <= lo && h.hi >= hi &&
                    (h.lo > best_lo || (h.lo == best_lo && h.hi < best_hi)))
                {
                    group = h.group;
                    best_lo = h.lo;
                    best_hi = h.hi;
                }
            }
            std::string seg = std::string(line.substr(lo, hi - lo));
            if (parts.empty())
                seg.insert(0, "  "); // the borderless block's two-space indent
            parts.push_back(text(std::move(seg)) | color(style::SyntaxColor(pal, group)));
        }
        if (parts.empty())
            return text("  " + std::string(line)) | color(pal.md_code);
        return hbox(std::move(parts));
    }

    Element Finish() override
    {
        const auto &pal = style::palette();

        // Try the syntax highlighter only when the fence declared a language
        // that is registered. Any failure falls back to the old single-colour
        // rendering, so highlighting can never break a code block.
        edc_syntax_result hl;
        bool highlighted = !lang.empty() && edc_syntax_available(lang.c_str()) &&
                           edc_syntax_highlight(buf.data(), buf.size(), lang.c_str(), &hl) == 0;

        Elements lines;
        size_t start = 0;
        for (size_t i = 0; i <= buf.size(); ++i)
        {
            if (i == buf.size() || buf[i] == '\n')
            {
                std::string_view line(buf.data() + start, i - start);
                lines.push_back(highlighted ? PaintLine(line, start, hl, pal)
                                            : text("  " + std::string(line)) | color(pal.md_code));
                start = i + 1;
            }
        }
        if (highlighted)
            edc_syntax_result_free(&hl);

        Element body = vbox(std::move(lines));
        // Blank lines above and below keep the borderless block readable.
        return vbox({text(""), std::move(body), text("")});
    }
};

struct QuoteB : Builder
{
    Elements children;
    void AddBlock(Element e) override
    {
        children.push_back(std::move(e));
    }
    Element Finish() override
    {
        const auto &pal = style::palette();
        return hbox({
            text("│ ") | color(pal.md_quote),
            vbox(std::move(children)) | flex,
        });
    }
};

struct ListB;

struct LiB : Builder
{
    Elements body;
    std::unique_ptr<ParaB> current_para;
    void AddInline(Element e) override
    {
        if (!current_para)
            current_para = std::make_unique<ParaB>();
        current_para->AddInline(std::move(e));
    }
    void AddBlock(Element e) override
    {
        FlushPara();
        body.push_back(std::move(e));
    }
    void BreakInline() override
    {
        // The item's inline content lives in a lazily created ParaB, so the break belongs to
        // that paragraph: it splits the item's own text, it does not end the item.
        if (!current_para)
            current_para = std::make_unique<ParaB>();
        current_para->BreakInline();
    }
    void FlushPara()
    {
        if (current_para)
        {
            body.push_back(current_para->Finish());
            current_para.reset();
        }
    }
    Element Finish() override
    {
        FlushPara();
        if (body.empty())
            return text("");
        if (body.size() == 1)
            return std::move(body.front());
        return vbox(std::move(body));
    }
};

struct ListB : Builder
{
    bool ordered = false;
    int index = 1;
    Elements items;
    void AddBlock(Element e) override
    {
        std::string bullet = ordered ? (std::to_string(index++) + ". ") : "• ";
        const auto &pal = style::palette();
        items.push_back(hbox({
            text(bullet) | color(pal.md_list),
            std::move(e) | flex,
        }));
    }
    Element Finish() override
    {
        if (items.empty())
            return text("");
        return vbox(std::move(items));
    }
};

// Pad a table cell with one space on each side so adjacent columns don't run
// into each other.
Element PadTableCell(Element cell)
{
    auto gap_x = emptyElement() | size(WIDTH, EQUAL, 1);
    return hbox({gap_x, std::move(cell), gap_x});
}

struct TableB : Builder
{
    std::vector<std::vector<Element>> rows;
    std::vector<Element> current_row;
    bool in_head = false;
    int head_end = 0;
    Element Finish() override
    {
        if (rows.empty())
            return text("");
        const auto &pal = style::palette();
        Table t(std::move(rows));
        t.SelectAll().Border(LIGHT, color(pal.md_rule));
        t.SelectAll().DecorateCells(PadTableCell);
        if (head_end > 0)
        {
            // The header row wears its own role, so it reads as a label rather than
            // just another bold row. Imported themes pick a bright attention tint here.
            t.SelectRow(0).DecorateCells(color(pal.md_table_head));
            t.SelectRow(0).Decorate(bold);
            t.SelectRow(0).SeparatorHorizontal(LIGHT, color(pal.md_table_head));
        }
        return t.Render();
    }
};

struct CellB : ParaB
{
};

struct State
{
    std::vector<std::unique_ptr<Builder>> stack;
    Style style;
    bool h1 = false; // currently inside an ATX level-1 heading (banner).
    // Width the element tree will be laid out at, or 0 when unknown. See
    // RenderMarkdown() and AddWord().
    int wrap_cols = 0;
    Builder &top()
    {
        return *stack.back();
    }

    // Inter-word space: uncolored normally, but inside an h1 banner the
    // space cell must keep the banner background so the band is continuous.
    Element GapSpace()
    {
        if (h1)
            return text(" ") | bgcolor(style::palette().md_h1_bg);
        return text(" ");
    }

    Element StyledWord(std::string s)
    {
        const auto &pal = style::palette();
        // A code span whose whole text IS a scheduler state name ("`COMPLETED`") is a state,
        // not code: colour it by the state and drop the chip background. That is how
        // `/task list` stays scannable by colour without a single new piece of markup -- the
        // span is ordinary inline code, the renderer just reads what it says. Computed before
        // the string is moved into the text element.
        const bool state_span = style.code && style::IsTaskStateName(s);
        const ::ftxui::Color state_color = state_span ? style::TaskStateColor(pal, s) : pal.md_code_inline;
        Element e = text(std::move(s));
        if (h1)
            e = e | bgcolor(pal.md_h1_bg);
        else if (style.code)
            e = state_span ? (e | color(state_color) | bold)
                           : (e | color(pal.md_code_inline) | bgcolor(pal.md_code_bg));
        if (style.bold)
            e = e | bold;
        if (style.italic)
            e = e | italic;
        if (style.strike)
            e = e | strikethrough;
        if (!style.href.empty())
            e = e | color(pal.md_link) | underlined | hyperlink(style.href);
        return e;
    }

    // Hand one word to the current block, normally as a single flow item. A word that cannot
    // fit on a line of its own -- an unbroken CJK run, a long URL or path -- is instead broken
    // between code points, one item each, so the flow can wrap there: a flow only breaks
    // *between* its children, so otherwise the word overflows the box and is clipped.
    void AddWord(const std::string &word)
    {
        if (wrap_cols > 0 && StringWidth(word) > wrap_cols)
        {
            size_t i = 0;
            while (i < word.size())
            {
                size_t len = Utf8SeqLen(static_cast<unsigned char>(word[i]));
                if (i + len > word.size())
                    len = word.size() - i;
                top().AddInline(StyledWord(word.substr(i, len)));
                i += len;
            }
            return;
        }
        top().AddInline(StyledWord(word));
    }
};

int on_enter_block(MD_BLOCKTYPE type, void *det, void *ud)
{
    auto *s = static_cast<State *>(ud);
    switch (type)
    {
    case MD_BLOCK_H: {
        auto h = std::make_unique<HeadingB>();
        h->level = static_cast<MD_BLOCK_H_DETAIL *>(det)->level;
        s->h1 = (h->level == 1);
        s->stack.push_back(std::move(h));
        break;
    }
    case MD_BLOCK_P:
        s->stack.push_back(std::make_unique<ParaB>());
        break;
    case MD_BLOCK_CODE: {
        auto code = std::make_unique<CodeB>();
        // The fence's info string is the language tag the highlighter keys on.
        // md4c does not NUL-terminate it, so copy by explicit length.
        const auto *cd = static_cast<const MD_BLOCK_CODE_DETAIL *>(det);
        if (cd && cd->lang.text && cd->lang.size)
            code->lang.assign(cd->lang.text, cd->lang.size);
        s->stack.push_back(std::move(code));
        break;
    }
    case MD_BLOCK_QUOTE:
        s->stack.push_back(std::make_unique<QuoteB>());
        break;
    case MD_BLOCK_UL:
        s->stack.push_back(std::make_unique<ListB>());
        break;
    case MD_BLOCK_OL: {
        auto l = std::make_unique<ListB>();
        l->ordered = true;
        s->stack.push_back(std::move(l));
        break;
    }
    case MD_BLOCK_LI:
        s->stack.push_back(std::make_unique<LiB>());
        break;
    case MD_BLOCK_TABLE:
        s->stack.push_back(std::make_unique<TableB>());
        break;
    case MD_BLOCK_THEAD:
    case MD_BLOCK_TBODY:
        if (auto *t = dynamic_cast<TableB *>(&s->top()))
            t->in_head = true;
        break;
    case MD_BLOCK_TR:
        break;
    case MD_BLOCK_TH:
    case MD_BLOCK_TD:
        s->stack.push_back(std::make_unique<CellB>());
        break;
    case MD_BLOCK_DOC:
        break;
    case MD_BLOCK_HR:
        break;
    default:
        s->stack.push_back(std::make_unique<ParaB>());
        break;
    }
    return 0;
}

int on_leave_block(MD_BLOCKTYPE type, void * /*det*/, void *ud)
{
    auto *s = static_cast<State *>(ud);
    switch (type)
    {
    case MD_BLOCK_H:
        s->h1 = false;
        break;
    case MD_BLOCK_HR:
        s->top().AddBlock(separator() | color(style::palette().md_rule));
        return 0;
    case MD_BLOCK_TR: {
        auto *t = dynamic_cast<TableB *>(&s->top());
        if (t)
        {
            t->rows.push_back(std::move(t->current_row));
            t->current_row.clear();
        }
        return 0;
    }
    case MD_BLOCK_THEAD:
    case MD_BLOCK_TBODY: {
        auto *t = dynamic_cast<TableB *>(&s->top());
        if (t)
        {
            t->in_head = false;
            t->head_end = static_cast<int>(t->rows.size());
        }
        return 0;
    }
    case MD_BLOCK_TH:
    case MD_BLOCK_TD: {
        auto cell = s->stack.back()->Finish();
        s->stack.pop_back();
        if (auto *t = dynamic_cast<TableB *>(&s->top()))
            t->current_row.push_back(std::move(cell));
        return 0;
    }
    default:
        break;
    }
    if (s->stack.size() <= 1)
        return 0;
    auto e = s->stack.back()->Finish();
    s->stack.pop_back();
    s->top().AddBlock(std::move(e));
    return 0;
}

int on_enter_span(MD_SPANTYPE type, void *det, void *ud)
{
    auto *s = static_cast<State *>(ud);
    switch (type)
    {
    case MD_SPAN_STRONG:
        s->style.bold = true;
        break;
    case MD_SPAN_EM:
        s->style.italic = true;
        break;
    case MD_SPAN_CODE:
        s->style.code = true;
        break;
    case MD_SPAN_DEL:
        s->style.strike = true;
        break;
    case MD_SPAN_A: {
        auto *a = static_cast<MD_SPAN_A_DETAIL *>(det);
        if (a->href.text)
            s->style.href.assign(a->href.text, a->href.size);
        break;
    }
    default:
        break;
    }
    return 0;
}

int on_leave_span(MD_SPANTYPE type, void * /*det*/, void *ud)
{
    auto *s = static_cast<State *>(ud);
    switch (type)
    {
    case MD_SPAN_STRONG:
        s->style.bold = false;
        break;
    case MD_SPAN_EM:
        s->style.italic = false;
        break;
    case MD_SPAN_CODE:
        s->style.code = false;
        break;
    case MD_SPAN_DEL:
        s->style.strike = false;
        break;
    case MD_SPAN_A:
        s->style.href.clear();
        break;
    default:
        break;
    }
    return 0;
}

int on_text(MD_TEXTTYPE type, const MD_CHAR *txt, MD_SIZE sz, void *ud)
{
    auto *s = static_cast<State *>(ud);
    std::string_view sv(txt, sz);

    if (auto *cb = dynamic_cast<CodeB *>(&s->top()))
    {
        cb->AddText(sv);
        return 0;
    }

    // Inline code: keep the whole span in ONE text element so multi-word
    // code (e.g. `bazel build`) renders as a single continuous Monokai chip
    // instead of one highlight block per word. (An over-wide span is still
    // broken up by AddWord, but the pieces stay adjacent so the chip reads as
    // one block.)
    if (s->style.code)
    {
        s->AddWord(std::string(sv));
        return 0;
    }

    if (type == MD_TEXT_BR)
    {
        /* A hard break is a real line break: two trailing spaces (or a backslash) mean "end
         * this line", and rendering it as a space collapsed poems, addresses and any
         * hand-aligned text into one run. */
        s->top().BreakInline();
        return 0;
    }
    if (type == MD_TEXT_SOFTBR)
    {
        /* A soft break is only a newline in the SOURCE; markdown says it is a space. */
        s->top().AddInline(s->GapSpace());
        return 0;
    }

    size_t start = 0;
    for (size_t i = 0; i <= sv.size(); ++i)
    {
        if (i == sv.size() || sv[i] == ' ')
        {
            if (i > start)
                s->AddWord(std::string(sv.substr(start, i - start)));
            if (i < sv.size())
                s->top().AddInline(s->GapSpace());
            start = i + 1;
        }
    }
    return 0;
}

} // namespace

/* md4c consumes a line-start `[label]:` as a LINK REFERENCE DEFINITION and emits no text
 * for it. Our own generated material collides with that syntax: JOIN dep output is spliced
 * into the task input as `[subtask X result]:` blocks, and in `/task status` only the FIRST
 * header survives -- the first trails `- **input**: ` on the same line (inline text), while
 * the 2nd/3rd start their own line and vanish along with their output.
 *
 * Escape the opening `[` of any line that looks like a reference definition (up to 3 leading
 * spaces, `[label]:`), so md4c draws the label literally. This text is display DATA, not a
 * real link definition; a chat bubble has nothing to gain from silently dropping it. Done on
 * a copy so the caller's buffer is untouched. */
static std::string EscapeLeadingRefDefinitions(std::string_view md)
{
    std::string out;
    out.reserve(md.size() + 16);
    size_t pos = 0;
    while (pos < md.size())
    {
        const size_t nl = md.find('\n', pos);
        const size_t end = (nl == std::string_view::npos) ? md.size() : nl;
        const std::string_view line = md.substr(pos, end - pos);

        size_t indent = 0;
        while (indent < line.size() && indent < 3 && line[indent] == ' ')
            ++indent;
        bool escape = false;
        if (indent < line.size() && line[indent] == '[')
        {
            const size_t close = line.find("]:", indent + 1);
            if (close != std::string_view::npos && close > indent + 1)
            {
                const std::string_view label = line.substr(indent + 1, close - indent - 1);
                /* A reference label cannot contain brackets. Anything else after `]:` is
                 * left alone -- escaping the `[` already stops md4c treating the line as a
                 * definition, and the rest renders as ordinary text. */
                if (label.find('[') == std::string_view::npos && label.find(']') == std::string_view::npos)
                    escape = true;
            }
        }

        if (escape)
        {
            out.append(line.substr(0, indent));
            out += "\\[";
            out.append(line.substr(indent + 1));
        }
        else
        {
            out.append(line);
        }
        if (end < md.size())
            out += '\n';
        pos = end + 1;
    }
    return out;
}

Element RenderMarkdown(std::string_view md, int wrap_cols)
{
    State st;
    st.wrap_cols = wrap_cols;
    st.stack.push_back(std::make_unique<RootB>());

    MD_PARSER parser{};
    parser.abi_version = 0;
    parser.flags = MD_FLAG_TABLES | MD_FLAG_STRIKETHROUGH | MD_FLAG_TASKLISTS | MD_FLAG_PERMISSIVEURLAUTOLINKS |
                   MD_FLAG_PERMISSIVEEMAILAUTOLINKS;
    parser.enter_block = on_enter_block;
    parser.leave_block = on_leave_block;
    parser.enter_span = on_enter_span;
    parser.leave_span = on_leave_span;
    parser.text = on_text;

    const std::string safe = EscapeLeadingRefDefinitions(md);
    int rc = md_parse(safe.data(), static_cast<MD_SIZE>(safe.size()), &parser, &st);
    Element body = (rc != 0) ? text(safe) : st.stack.front()->Finish();

    /* Give every plain-text cell an explicit foreground from the active palette.
     *
     * Raw markdown words are emitted as bare text() with no colour, so they used to
     * inherit the terminal's default foreground. The message bubble behind them,
     * however, IS painted -- with style::palette().pane_bg. On a light terminal the
     * terminal's dark default foreground landed on a dark pane (or vice versa), and
     * body text became nearly invisible while only the explicitly-coloured tokens
     * (headings, code, links) stayed readable. Painting the default here, as the
     * OUTERMOST colour, is safe: ftxui's color() decorator sets the cell colour before
     * rendering its child, so the first/inner colour wins -- every inner highlight
     * (code, link, heading, table head) still overrides this default. Cells with no
     * inner colour get pal.text, which the palette guarantees contrasts with pane_bg. */
    return body | color(style::palette().text);
}

Element HighlightedCode(std::string_view code, const char *lang)
{
    const auto &pal = style::palette();
    edc_syntax_result hl;
    const bool ok = lang && edc_syntax_available(lang) &&
                    edc_syntax_highlight(code.data(), code.size(), lang, &hl) == 0 && hl.count > 0;

    Elements lines;
    size_t start = 0;
    for (size_t i = 0; i <= code.size(); ++i)
    {
        if (i != code.size() && code[i] != '\n')
            continue;
        std::string_view line = code.substr(start, i - start);
        if (!ok)
        {
            lines.push_back(text(std::string(line)) | color(pal.md_code));
        }
        else
        {
            // Cut at span byte boundaries within this line; ftxui lays out each
            // fragment by display width, so no manual byte->cell maths is needed.
            std::vector<size_t> cuts{0, line.size()};
            struct H { size_t lo, hi; const char *group; };
            std::vector<H> hits;
            for (size_t s = 0; s < hl.count; ++s)
            {
                const edc_syntax_span &sp = hl.spans[s];
                if (sp.end_byte <= start || sp.start_byte >= start + line.size())
                    continue;
                size_t lo = sp.start_byte > start ? sp.start_byte - start : 0;
                size_t hi = sp.end_byte < start + line.size() ? sp.end_byte - start : line.size();
                if (lo >= hi) continue;
                hits.push_back({lo, hi, edc_syntax_group_name(sp.group)});
                cuts.push_back(lo); cuts.push_back(hi);
            }
            std::sort(cuts.begin(), cuts.end());
            cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
            Elements parts;
            for (size_t k = 0; k + 1 < cuts.size(); ++k)
            {
                size_t lo = cuts[k], hi = cuts[k + 1];
                if (lo >= hi) continue;
                const char *group = nullptr;
                size_t blo = 0, bhi = line.size() + 1;
                for (const H &h : hits)
                    if (h.lo <= lo && h.hi >= hi &&
                        (h.lo > blo || (h.lo == blo && h.hi < bhi)))
                    { group = h.group; blo = h.lo; bhi = h.hi; }
                parts.push_back(text(std::string(line.substr(lo, hi - lo))) |
                                color(style::SyntaxColor(pal, group)));
            }
            lines.push_back(parts.empty() ? text(std::string(line)) | color(pal.md_code)
                                          : hbox(std::move(parts)));
        }
        start = i + 1;
    }
    if (ok)
        edc_syntax_result_free(&hl);
    return vbox(std::move(lines));
}

} // namespace edc
