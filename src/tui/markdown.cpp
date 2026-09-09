#include "edc_style.hpp"
#include "markdown.hpp"

#include <ftxui/dom/table.hpp>
#include <md4c.h>

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
    void AddInline(Element e) override
    {
        words.push_back(std::move(e));
    }
    Element Finish() override
    {
        if (words.empty())
            return text("");
        return hflow(std::move(words));
    }
};

struct HeadingB : ParaB
{
    int level = 1;
    Element Finish() override
    {
        // Heading colors come from the central style palette (edc_style.hpp).
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
        default:
            c = pal.md_h4;
            break;
        }
        Element head = ParaB::Finish() | bold | color(c);
        if (level <= 2)
            return vbox({head, separator() | color(pal.md_rule)});
        return head;
    }
};

struct CodeB : Builder
{
    std::string lang, buf;
    void AddText(std::string_view s)
    {
        buf.append(s);
    }
    Element Finish() override
    {
        Elements lines;
        size_t start = 0;
        for (size_t i = 0; i <= buf.size(); ++i)
        {
            if (i == buf.size() || buf[i] == '\n')
            {
                lines.push_back(text(buf.substr(start, i - start)));
                start = i + 1;
            }
        }
        const auto &pal = style::palette();
        Element body = vbox(std::move(lines)) | color(pal.md_code);
        Element title = text(lang.empty() ? "code" : lang) | color(pal.md_code);
        // Use the same LIGHT border style and rule color as horizontal
        // separators and table lines, so code frames look consistent.
        return window(std::move(title), std::move(body), LIGHT) |
               color(pal.md_rule);
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
            t.SelectRow(0).Decorate(bold);
            t.SelectRow(0).SeparatorHorizontal(LIGHT, color(pal.md_rule));
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
    Builder &top()
    {
        return *stack.back();
    }

    Element StyledWord(std::string s)
    {
        const auto &pal = style::palette();
        Element e = text(std::move(s));
        if (style.code)
            e = e | color(pal.md_code_inline) | bgcolor(pal.md_code_bg);
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
};

int on_enter_block(MD_BLOCKTYPE type, void *det, void *ud)
{
    auto *s = static_cast<State *>(ud);
    switch (type)
    {
    case MD_BLOCK_H: {
        auto h = std::make_unique<HeadingB>();
        h->level = static_cast<MD_BLOCK_H_DETAIL *>(det)->level;
        s->stack.push_back(std::move(h));
        break;
    }
    case MD_BLOCK_P:
        s->stack.push_back(std::make_unique<ParaB>());
        break;
    case MD_BLOCK_CODE: {
        auto c = std::make_unique<CodeB>();
        auto *d = static_cast<MD_BLOCK_CODE_DETAIL *>(det);
        if (d->lang.text && d->lang.size > 0)
            c->lang.assign(d->lang.text, d->lang.size);
        s->stack.push_back(std::move(c));
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
    // code (e.g. `bazel build`) renders as a single continuous glow-style
    // chip instead of one highlight block per word.
    if (s->style.code)
    {
        s->top().AddInline(s->StyledWord(std::string(sv)));
        return 0;
    }

    if (type == MD_TEXT_SOFTBR || type == MD_TEXT_BR)
    {
        s->top().AddInline(text(" "));
        return 0;
    }

    size_t start = 0;
    for (size_t i = 0; i <= sv.size(); ++i)
    {
        if (i == sv.size() || sv[i] == ' ')
        {
            if (i > start)
                s->top().AddInline(s->StyledWord(std::string(sv.substr(start, i - start))));
            if (i < sv.size())
                s->top().AddInline(text(" "));
            start = i + 1;
        }
    }
    return 0;
}

} // namespace

Element RenderMarkdown(std::string_view md)
{
    State st;
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

    int rc = md_parse(md.data(), static_cast<MD_SIZE>(md.size()), &parser, &st);
    if (rc != 0)
        return text(std::string(md));
    return st.stack.front()->Finish();
}

} // namespace edc
