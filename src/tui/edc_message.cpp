#include "edc_message.hpp"
#include "edc_style.hpp"
#include "ftxui/component/event.hpp"
#include "ftxui/component/mouse.hpp"
#include "ftxui/dom/elements.hpp"
#include "markdown.hpp"

namespace edc
{

Message::Message(const std::string_view content, time_t ts, const std::string_view task_id)
    : content_(content), ts_(ts), task_id_(task_id)
{
}

bool Message::OnEvent(ftxui::Event)
{
    return false;
}

::ftxui::Element Message::TaskTag() const
{
    const auto &pal = style::palette();
    // "task N" cross-references the sidebar task tree, so it keeps its own hue
    // (violet): gray was too faint and the accent blue now means the user turn.
    return ::ftxui::text("task " + task_id_) | ::ftxui::color(pal.task_tag);
}

::ftxui::Element Message::OnRender()
{
    // RenderMarkdown is expensive (full md4c parse + element tree build) and
    // the body is rendered again every frame, even when scrolled out of view.
    // Parse once on first render and reuse the cached tree afterwards.
    if (!body_)
    {
        const auto &pal = style::palette();
        body_ = RenderMarkdown(content_);
        body_ = body_ | ::ftxui::borderStyled(::ftxui::BorderStyle::EMPTY) | ::ftxui::bgcolor(pal.pane_bg);
    }

    if (task_id_.empty())
        return body_;

    const auto &pal = style::palette();
    return ::ftxui::vbox({TaskTag(), body_}) | ::ftxui::bgcolor(pal.pane_bg);
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
            return true;
        }
        return false;
    }

    if (event == ftxui::Event::Return)
    {
        thought_expanded_ = !thought_expanded_;
        return true;
    }
    return false;
}

::ftxui::Element ThoughtMessage::OnRender()
{
    auto arrow = thought_expanded_ ? "-" : "+";
    const auto &pal = style::palette();
    auto applied_color = thought_expanded_ ? pal.thought_open : pal.thought_closed;
    // The thought toggle doubles as the message's task tag: "task N" rides on
    // the same clickable label instead of becoming a second line.
    std::string label_text = " " + std::string(arrow) + " Thought";
    if (!task_id_.empty())
        label_text += " · task " + task_id_;
    auto label = ::ftxui::text(label_text) | ::ftxui::color(applied_color) | ::ftxui::bold | ::ftxui::reflect(box_);

    if (!thought_expanded_)
    {
        return label;
    }

    auto body = ::ftxui::paragraph(content_) | ::ftxui::color(pal.text_dim) |
                ::ftxui::borderStyled(::ftxui::BorderStyle::EMPTY);

    return ::ftxui::vbox({label, body});
}

UserMessage::UserMessage(const std::string_view content, time_t ts, const std::string_view task_id)
    : Message(content, ts, task_id)
{
}

::ftxui::Element UserMessage::OnRender()
{
    const auto &pal = style::palette();
    auto body = ::ftxui::paragraph(content_);
    auto header = ::ftxui::separatorHeavy() | ::ftxui::color(pal.user_rule);

    body = ::ftxui::hbox({header, body | ::ftxui::borderStyled(::ftxui::BorderStyle::EMPTY)}) |
           ::ftxui::bgcolor(pal.pane_bg);
    return body;
}

bool UserMessage::OnEvent(ftxui::Event)
{
    return false;
}

ToolMessage::ToolMessage(const std::string_view header, const std::string_view content, time_t ts, bool markdown,
                         const std::string_view task_id)
    : Message(content, ts, task_id), header_(header), markdown_(markdown)
{
}

bool ToolMessage::OnEvent(::ftxui::Event event)
{
    if (event.is_mouse())
    {
        if (event.mouse().button == ::ftxui::Mouse::Left && event.mouse().motion == ::ftxui::Mouse::Pressed &&
            box_.Contain(event.mouse().x, event.mouse().y))
        {
            expanded_ = !expanded_;
            return true;
        }
        return false;
    }

    if (event == ftxui::Event::Return)
    {
        expanded_ = !expanded_;
        return true;
    }
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

::ftxui::Element ToolMessage::OnRender()
{
    const auto &pal = style::palette();

    // Only messages whose content is truncated on the collapsed view carry the
    // toggle; short outputs are shown verbatim without a footer.
    const bool truncated = !expanded_ && CountLines(content_) > ToolMessage::kPreviewLines;
    const bool toggleable = expanded_ || CountLines(content_) > ToolMessage::kPreviewLines;

    ::ftxui::Elements children;
    if (!task_id_.empty())
        children.push_back(TaskTag());
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
            md_body_ = RenderMarkdown(content_);
        }
        body = md_body_;
    }
    else if (!truncated)
    {
        body = ::ftxui::paragraph(content_);
    }
    else
    {
        // First kPreviewLines lines, with the line after them replaced by "...".
        std::string preview = FirstLines(content_, ToolMessage::kPreviewLines);
        if (!preview.empty())
            preview += "\n";
        preview += "...";
        body = ::ftxui::paragraph(preview);
    }

    children.push_back(body);
    if (toggleable)
    {
        auto toggle = expanded_ ? ::ftxui::text("- Collapse") : ::ftxui::text("+ Click to expand");
        auto footer = ::ftxui::hbox({toggle | ::ftxui::color(pal.accent) | ::ftxui::reflect(box_)}) | ::ftxui::flex;
        children.push_back(footer);
    }

    return ::ftxui::vbox(children) | ::ftxui::borderStyled(::ftxui::BorderStyle::EMPTY) |
           ::ftxui::bgcolor(pal.pane_bg);
}

} // namespace edc
