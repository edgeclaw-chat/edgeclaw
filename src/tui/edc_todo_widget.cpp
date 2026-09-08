#include "edc_todo_widget.hpp"
#include "edc_text_util.hpp"
#include "ftxui/dom/elements.hpp"
#include <algorithm>
#include <string>

namespace edc
{

TodoWidget::TodoWidget()
{
}

void TodoWidget::SetItems(const std::vector<Item> &items)
{
    const bool was_empty = items_.empty();
    items_ = items;
    completed_ = 0;
    for (const Item &item : items_)
    {
        if (item.status == "completed")
            completed_++;
    }

    // Surface the first real content: the section starts collapsed (empty
    // list → nothing to show), so an empty → non-empty transition expands it
    // automatically. A fold the user made while the list was *visible* still
    // survives refreshes — this only fires when the previous state had no
    // rows to fold anyway.
    if (was_empty && !items_.empty())
        collapsed_ = false;
}

bool TodoWidget::OnEvent(::ftxui::Event event)
{
    if (!event.is_mouse())
        return false;

    if (event.mouse().button != ::ftxui::Mouse::Left || event.mouse().motion != ::ftxui::Mouse::Pressed)
        return false;

    // Clicking the section header toggles the whole item list. The box is
    // recorded by reflect() on the full-width header row of the previous
    // frame, so the whole row is a click target (same scheme as the group
    // headers in TaskWidget).
    if (header_box_.Contain(event.mouse().x, event.mouse().y))
    {
        collapsed_ = !collapsed_;
        return true;
    }
    return false;
}

::ftxui::Element TodoWidget::OnRender()
{
    using namespace ::ftxui;

    // Palette is constructed inline at render time (same style as
    // TaskWidget): ftxui::Color's constructor queries terminal state, which
    // is not ready during C++ static initialization — file-scope const Color
    // objects would run before main() and segfault.
    // Scheme A: the todo list no longer re-uses the task-tree palette state
    // by state. Only one item is ever accented — the in_progress row, in
    // blue; pending and completed rows are neutral gray (completed keeps its
    // strikethrough). One quiet block with at most one bright row, instead
    // of a second rainbow beside the Tasks tree.
    const Color kBlue = Color::RGB(94, 151, 247);   // the single accent: in_progress
    const Color kGray = Color::RGB(147, 147, 147);  // neutral rows + chrome (counts, "(empty)", "+N more")
    const Color kGreen = Color::RGB(64, 168, 96);   // summary n/n, only when the whole list is completed
    const Color kDimLine = Color::RGB(60, 60, 60);
    const Color kTodoBg = Color::RGB(30, 30, 30); // must match TaskWidget's pane background

    const size_t total = items_.size();
    const size_t shown = std::min(total, kMaxVisibleRows);

    Elements lines;
    {
        // Collapsible section header: the arrow shows the fold state and the
        // summary always reflects the whole list. The header row is full
        // width so a click anywhere on it toggles the section (header_box_
        // is filled by reflect below; click targets are consumed in OnEvent).
        // "All done" green is reserved for a non-empty list that completed:
        // an empty list (0/0) stays neutral gray, not triumphant green.
        const std::string summary = std::to_string(completed_) + "/" + std::to_string(total);
        const bool all_done = total > 0 && completed_ == static_cast<int>(total);
        lines.push_back(hbox({
            text((collapsed_ ? "▸ " : "▾ ") + std::string(" Todos ")) | bold | color(kBlue),
            filler(),
            text(summary) | color(all_done ? kGreen : kGray) | bold,
        }) | reflect(header_box_));
    }

    // A collapsed section keeps only its header row; the separator and the
    // item rows below are skipped entirely (the enclosing pane reclaims the
    // space for whatever sits underneath in the sidebar column). An expanded
    // but empty section — before the first update_todolist emission or after
    // the tool cleared the list — still draws its separator plus a dim
    // "(empty)" marker, so the fold keeps a visible body and the sidebar
    // layout stays stable instead of the section collapsing to a lone header.
    if (!collapsed_)
    {
        lines.push_back(separator() | color(kDimLine));
        if (items_.empty())
            lines.push_back(text("  (empty)") | dim | color(kGray));
    }

    // Width available for an item row. reflect() below sits on the vbox
    // *inside* the (empty-style) border; before the first measurement fall
    // back to the default sidebar width (36) minus the 2 border columns.
    int row_width = 34;
    if (content_box_.x_min != 0 || content_box_.x_max != 0 || content_box_.y_min != 0 || content_box_.y_max != 0)
        row_width = content_box_.x_max - content_box_.x_min + 1;

    // Fixed cells per row: 2 indent + a 3-cell bracket marker ("[ ]", "[•]"
    // or "[✓]") + 1 gap after the marker. Content wraps across several lines
    // at the full remaining width; every continuation line is indented by the
    // same kFixedCells so text aligns under the first line's content column.
    constexpr int kFixedCells = 6;
    const std::string cont_prefix(kFixedCells, ' ');

    if (!collapsed_)
    {
        for (size_t i = 0; i < shown; i++)
        {
            const Item &item = items_[i];
            const bool done = item.status == "completed";
            const bool doing = item.status == "in_progress";

            // Leading per-state marker inside a bracket box: "[ ]" for a
            // pending item, "[•]" for the in_progress one, "[✓]" once the
            // item is completed. Scheme A's palette still governs the glyph
            // colors: the box is blue only on the in_progress row (matching
            // its text) and neutral dim gray for pending/completed, so a
            // marker never adds a second hue of its own to a row; completed
            // rows are additionally told apart by the strikethrough on their
            // text.
            Element marker;
            if (doing)
                marker = text("  [•] ") | color(kBlue);
            else if (done)
                marker = text("  [✓] ") | color(kGray) | dim;
            else
                marker = text("  [ ] ") | color(kGray) | dim;

            // Content wraps to the whole row width minus the marker column;
            // single-line ellipsis truncation is only a last resort for a
            // description longer than kMaxContentLines wrapped lines.
            const int budget = row_width - kFixedCells;
            std::vector<std::string> wrapped = WrapToWidth(item.content, budget);
            if (wrapped.empty())
                wrapped.push_back(std::string());
            if (wrapped.size() > kMaxContentLines)
            {
                wrapped.resize(kMaxContentLines);
                std::string &last = wrapped.back();
                if (budget - StringWidth(last) >= 1)
                    last += "…";
            }

            // Content styling: only the in_progress row is bright blue — the
            // single accent of the whole list. Pending and completed rows
            // are dim neutral gray; completed adds strikethrough so "done"
            // stays legible without a color of its own. Every wrapped line
            // of the item gets the same treatment.
            auto style_content = [&](Element e) -> Element {
                if (doing)
                    return e | color(kBlue);
                if (done)
                    return e | dim | strikethrough | color(kGray);
                return e | dim | color(kGray);
            };

            Elements item_lines;
            item_lines.push_back(hbox({std::move(marker), style_content(text(wrapped[0]))}));
            for (size_t ln = 1; ln < wrapped.size(); ln++)
                item_lines.push_back(hbox({text(cont_prefix), style_content(text(wrapped[ln]))}));

            // The single in_progress item may additionally report what it is
            // doing right now. It gets its own indented sub-line ("↳ <form>")
            // instead of a " <- <activeForm>" tail squeezed next to the
            // content, so it never steals width from the item text.
            if (doing && !item.active_form.empty())
            {
                const std::string sub = TruncateToWidth("↳ " + item.active_form, budget);
                item_lines.push_back(hbox({text(cont_prefix), text(sub) | dim | color(kBlue)}));
            }

            lines.push_back(vbox(std::move(item_lines)));
        }

        if (total > shown)
        {
            lines.push_back(text("  … +" + std::to_string(total - shown) + " more") | dim | color(kGray));
        }
    }

    return vbox(std::move(lines)) | reflect(content_box_) | borderStyled(BorderStyle::EMPTY) | bgcolor(kTodoBg);
}

} // namespace edc
