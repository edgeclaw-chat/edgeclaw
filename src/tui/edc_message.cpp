#include "edc_message.hpp"
#include "edc_style.hpp"
#include "edc_text_util.hpp"
#include "edc_token_style.hpp"
#include "ftxui/component/event.hpp"
#include "ftxui/component/mouse.hpp"
#include "ftxui/dom/elements.hpp"
#include "markdown.hpp"

#include <algorithm>

namespace edc
{

namespace
{
// Columns UserMessage draws to the left of its body: the heavy rule plus the
// two columns of the EMPTY border around the body.
constexpr int kUserRuleChrome = 3;

// The body of |text| wrapped to |cols| cells, one text element per display row.
// Messages cannot leave wrapping to ftxui: a flow only breaks *between* its
// children, so an unbroken run (CJK text, a long URL or path) is a single child that
// overflows and gets clipped. VisualWrapLines is the same lossless, wide-char-safe
// wrap the input editor uses. |cols| == 0 (rendered outside a MessageList) falls
// back to ftxui's own wrapping.
::ftxui::Element WrappedText(const std::string &text, int cols)
{
    if (cols <= 0)
        return ::ftxui::paragraph(text);

    ::ftxui::Elements rows;
    const std::vector<std::string> lines = VisualWrapLines(text, cols);
    rows.reserve(lines.size());
    for (const std::string &line : lines)
        rows.push_back(::ftxui::text(line));

    if (rows.size() == 1)
        return std::move(rows.front());
    return ::ftxui::vbox(std::move(rows));
}
// Like WrappedText, but a token keeps its look across the wrap. Each display row is
// scanned on its own, so a token split by wrapping (a very long path) degrades to plain
// text on both halves instead of bleeding colour into the rest of the line.
::ftxui::Element WrappedTokenText(const std::string &text, int cols)
{
    if (cols <= 0)
        return ::ftxui::paragraph(text);

    ::ftxui::Elements rows;
    const std::vector<std::string> lines = VisualWrapLines(text, cols);
    rows.reserve(lines.size());
    for (const std::string &line : lines)
    {
        const std::vector<TokenSpan> spans = ScanTokens(line);
        if (spans.empty())
        {
            rows.push_back(::ftxui::text(line));
            continue;
        }
        ::ftxui::Elements parts;
        size_t at = 0;
        for (const TokenSpan &span : spans)
        {
            if (span.begin > at)
                parts.push_back(::ftxui::text(line.substr(at, span.begin - at)));
            parts.push_back(StyleToken(::ftxui::text(line.substr(span.begin, span.len)), span.paste));
            at = span.begin + span.len;
        }
        if (at < line.size())
            parts.push_back(::ftxui::text(line.substr(at)));
        rows.push_back(::ftxui::hbox(std::move(parts)));
    }

    if (rows.size() == 1)
        return std::move(rows.front());
    return ::ftxui::vbox(std::move(rows));
}
} // namespace

Message::Message(const std::string_view content, time_t ts, const std::string_view task_id)
    : content_(content), ts_(ts), task_id_(task_id)
{
}

void Message::SetRenderWidth(int cols)
{
    if (cols == render_width_)
        return;
    render_width_ = cols;
    OnWidthChanged();
}

void Message::OnWidthChanged()
{
    body_ = nullptr;
}

bool Message::OnEvent(ftxui::Event)
{
    return false;
}

::ftxui::Element Message::OnRender()
{
    // RenderMarkdown is expensive (full md4c parse + element tree build) and
    // the body is rendered again every frame, even when scrolled out of view.
    // Parse once on first render and reuse the cached tree afterwards.
    if (!body_)
    {
        const auto &pal = style::palette();
        body_ = RenderMarkdown(content_, BodyWidth());
        body_ = body_ | ::ftxui::borderStyled(::ftxui::BorderStyle::EMPTY) | ::ftxui::bgcolor(pal.pane_bg);
    }

    return body_;
}

ThoughtMessage::ThoughtMessage(const std::string_view content, time_t ts, const std::string_view task_id)
    : Message(content, ts, task_id)
{
}

bool ThoughtMessage::OnEvent(ftxui::Event event)
{
    if (event.is_mouse())
    {
        if (event.mouse().button == ::ftxui::Mouse::Left && event.mouse().motion == ::ftxui::Mouse::Pressed &&
            box_.Contain(event.mouse().x, event.mouse().y))
        {
            thought_expanded_ = !thought_expanded_;
            body_ = nullptr; // the two shapes have different heights
            return true;
        }
        return false;
    }

    if (event == ftxui::Event::Return)
    {
        thought_expanded_ = !thought_expanded_;
        body_ = nullptr;
        return true;
    }
    return false;
}

::ftxui::Element ThoughtMessage::OnRender()
{
    // Waiting for a frame is what used to make this expensive: the label and
    // the body were rebuilt (and re-measured by ftxui) on every single frame,
    // including the ~66fps of the working animation. content_ is immutable, so
    // the tree only changes when the toggle does.
    if (body_)
        return body_;

    auto arrow = thought_expanded_ ? "-" : "+";
    const auto &pal = style::palette();
    auto applied_color = thought_expanded_ ? pal.thought_open : pal.thought_closed;
    // The thought toggle is the message's only visible header; it carries no
    // task tag, so no "task N" rides on the label.
    std::string label_text = " " + std::string(arrow) + " Thought";
    auto label = ::ftxui::text(label_text) | ::ftxui::color(applied_color) | ::ftxui::bold | ::ftxui::reflect(box_);

    if (!thought_expanded_)
    {
        body_ = std::move(label);
        return body_;
    }

    auto body = WrappedText(content_, BodyWidth()) | ::ftxui::color(pal.text_dim) |
                ::ftxui::borderStyled(::ftxui::BorderStyle::EMPTY);

    body_ = ::ftxui::vbox({std::move(label), std::move(body)});
    return body_;
}

UserMessage::UserMessage(const std::string_view content, time_t ts, const std::string_view task_id)
    : Message(content, ts, task_id)
{
}

::ftxui::Element UserMessage::OnRender()
{
    // Built once: the user turn is created from immutable text and never
    // toggles, so rebuilding the paragraph on every frame only served to make
    // ftxui re-split the whole message into words ~66 times a second.
    if (body_)
        return body_;

    const auto &pal = style::palette();
    /* The same marks the input line painted: `@path` keeps its keyword colour and a folded
     * paste stays a chip. Seeing plain text here is what makes a user doubt the reference
     * was taken at all (edc_token_style.hpp). */
    auto body = WrappedTokenText(content_, BodyWidth(kUserRuleChrome));
    auto header = ::ftxui::separatorHeavy() | ::ftxui::color(pal.user_rule);

    body_ = ::ftxui::hbox({header, body | ::ftxui::borderStyled(::ftxui::BorderStyle::EMPTY)}) |
            ::ftxui::bgcolor(pal.pane_bg);
    return body_;
}

bool UserMessage::OnEvent(ftxui::Event)
{
    return false;
}

namespace
{
// Number of logical lines in text (a trailing '\n' does not count as an extra
// empty line).
size_t CountLines(const std::string &text)
{
    if (text.empty())
        return 0;
    size_t count = 1;
    for (char c : text)
        if (c == '\n')
            count++;
    if (!text.empty() && text.back() == '\n')
        count--;
    return count;
}

// First kPreviewLines lines of text, separated by '\n'.
std::string FirstLines(const std::string &text, size_t max_lines)
{
    size_t end = std::string::npos;
    size_t newlines_seen = 0;
    for (size_t i = 0; i < text.size(); i++)
    {
        if (text[i] == '\n')
        {
            newlines_seen++;
            if (newlines_seen == max_lines)
            {
                end = i;
                break;
            }
        }
    }
    if (end == std::string::npos)
        return text;
    return text.substr(0, end);
}
} // namespace

ToolMessage::ToolMessage(const std::string_view header, const std::string_view content, time_t ts, bool markdown,
                         const std::string_view task_id)
    : Message(content, ts, task_id), header_(header), markdown_(markdown), line_count_(CountLines(content_))
{
}

void ToolMessage::OnWidthChanged()
{
    Message::OnWidthChanged();
    // The full markdown body is parsed for a specific width too; preview_ is
    // plain text and does not depend on it.
    md_body_ = nullptr;
}

bool ToolMessage::OnEvent(::ftxui::Event event)
{
    if (event.is_mouse())
    {
        if (event.mouse().button == ::ftxui::Mouse::Left && event.mouse().motion == ::ftxui::Mouse::Pressed &&
            box_.Contain(event.mouse().x, event.mouse().y))
        {
            expanded_ = !expanded_;
            body_ = nullptr; // collapsed and expanded are different trees
            return true;
        }
        return false;
    }

    if (event == ftxui::Event::Return)
    {
        expanded_ = !expanded_;
        body_ = nullptr;
        return true;
    }
    return false;
}

::ftxui::Element ToolMessage::OnRender()
{
    // Cached like the other messages: the tree is immutable unless the toggle
    // flips. Before this the collapsed view was rebuilt — and the body rescanned
    // line by line, twice — on every frame.
    if (body_)
        return body_;

    const auto &pal = style::palette();

    // Only messages whose content is truncated on the collapsed view carry the
    // toggle; short outputs are shown verbatim without a footer.
    const bool truncated = !expanded_ && line_count_ > kPreviewLines;
    const bool toggleable = expanded_ || line_count_ > kPreviewLines;

    ::ftxui::Elements children;
    if (!header_.empty())
    {
        children.push_back(::ftxui::paragraph(header_) | ::ftxui::color(pal.text_dim));
        children.push_back(::ftxui::separatorEmpty());
    }

    ::ftxui::Element body;
    if (markdown_ && !truncated)
    {
        // Full markdown body: parse once on first render, reuse afterwards.
        // (While collapsed over a long body the raw source preview is shown
        // instead — cheaper and keeps the truncation UX identical to plain.)
        if (!md_body_)
        {
            md_body_ = RenderMarkdown(content_, BodyWidth());
        }
        body = md_body_;
    }
    else if (!truncated)
    {
        body = WrappedText(content_, BodyWidth());
    }
    else
    {
        // First kPreviewLines lines, with the line after them replaced by "...".
        if (!preview_ready_)
        {
            preview_ = FirstLines(content_, kPreviewLines);
            if (!preview_.empty())
                preview_ += "\n";
            preview_ += "...";
            preview_ready_ = true;
        }
        body = WrappedText(preview_, BodyWidth());
    }

    children.push_back(body);
    if (toggleable)
    {
        auto toggle = expanded_ ? ::ftxui::text("- Collapse") : ::ftxui::text("+ Click to expand");
        auto footer = ::ftxui::hbox({toggle | ::ftxui::color(pal.accent) | ::ftxui::reflect(box_)}) | ::ftxui::flex;
        children.push_back(footer);
    }

    body_ =
        ::ftxui::vbox(children) | ::ftxui::borderStyled(::ftxui::BorderStyle::EMPTY) | ::ftxui::bgcolor(pal.pane_bg);
    return body_;
}

} // namespace edc
