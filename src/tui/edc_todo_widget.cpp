#include "edc_todo_widget.hpp"
#include "edc_style.hpp"
#include "edc_text_util.hpp"
#include "ftxui/dom/elements.hpp"
#include <algorithm>
#include <string>

namespace edc
{

TodoWidget::TodoWidget()
{
}

void TodoWidget::SetItems(const std::vector<Item> &items, const std::string &task_id)
{
    const bool was_empty = items_.empty();
    items_ = items;
    task_id_ = task_id;
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

void TodoWidget::SetBreath(long long elapsed_ms)
{
    breath_ms_ = elapsed_ms;
}

bool TodoWidget::HasInProgressItem() const
{
    for (const Item &item : items_)
        if (item.status == "in_progress")
            return true;
    return false;
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

    // All colors come from the central style palette (edc_style.hpp); the
    // palette is the single source of truth for chrome, accents and the pane
    // background shared with TaskWidget / the chat surface.
    const auto &pal = style::palette();
    // Scheme A: the todo list no longer re-uses the task-tree palette state
    // by state. Only one item is ever accented — the in_progress row, in
    // blue; pending and completed rows are neutral gray (completed keeps its
    // strikethrough). One quiet block with at most one bright row, instead
    // of a second rainbow beside the Tasks tree.
    //
    // That one accent breathes while an item is running: the same shared
    // working animation as the processing task dot (style::breath_accent), so
    // the sidebar pulses as one thing rather than several.
    const ::ftxui::Color doing_accent = style::breath_accent(breath_ms_);

    // Blank cells kept free at the right edge of every full-width row: the sidebar is
    // the window's rightmost element, so rows would otherwise butt against the
    // terminal edge while the left side kept its gutter.
    constexpr int kRightPad = 2;

    const size_t total = items_.size();
    const size_t shown = std::min(total, kMaxVisibleRows);

    // Width available for a row. reflect() below sits on the vbox *inside* the
    // (empty-style) border; before the first measurement fall back to the
    // default sidebar width (36) minus the 2 border columns.
    int row_width = 34;
    if (content_box_.x_min != 0 || content_box_.x_max != 0 || content_box_.y_min != 0 || content_box_.y_max != 0)
        row_width = content_box_.x_max - content_box_.x_min + 1;

    Elements lines;
    {
        // Collapsible section header: the arrow shows the fold state and the
        // summary always reflects the whole list. The header row is full
        // width so a click anywhere on it toggles the section (header_box_
        // is filled by reflect below; click targets are consumed in OnEvent).
        const std::string summary = std::to_string(completed_) + "/" + std::to_string(total);
        const std::string title = (collapsed_ ? "▸ " : "▾ ") + std::string(" Todos ");

        // The list belongs to the foreground group's active task, so the header names
        // it ("#<id>" after the title, in the section's accent) while there is a list
        // to name. The progress summary keeps its slot on the right: the id is
        // truncated rather than allowed to push the summary out of the pane.
        std::string id_text;
        if (!task_id_.empty() && total > 0)
        {
            const int room = row_width - kRightPad - StringWidth(summary) - StringWidth(title) - 1;
            if (room >= 3) // room for "#1" once the separating space is paid for
                id_text = " " + TruncateToWidth("#" + task_id_, room - 1);
        }

        Elements header;
        header.push_back(text(title) | bold | color(pal.accent));
        if (!id_text.empty())
        {
            // The same accent as the section title, and nothing else: the header
            // reads as one label naming both the section and the task it is
            // following, instead of a bright white number pulling the eye off it.
            // Not dimmed either -- that would take the accent straight back out.
            header.push_back(text(id_text) | color(pal.accent));
        }
        header.push_back(filler());
        // The progress counter wears the section's own accent, like the title
        // and the task id: the whole header reads as one label instead of
        // splitting into a second voice (an "all done" green or a neutral
        // gray). Bold is what keeps it readable as a number rather than chrome.
        header.push_back(text(summary) | color(pal.accent) | bold);
        header.push_back(text(std::string(kRightPad, ' ')));
        lines.push_back(hbox(std::move(header)) | reflect(header_box_));
    }

    // A collapsed section keeps only its header row. An expanded but empty one still
    // draws its separator plus a dim "(empty)" marker, so the fold keeps a visible
    // body and the sidebar layout stays stable.
    if (!collapsed_)
    {
        lines.push_back(separator() | color(pal.line_dim));
        if (items_.empty())
            lines.push_back(text("  (empty)") | dim | color(pal.text_dim));
    }

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
            const bool cancelled = item.status == "cancelled";

            // Per-state marker in a bracket box: "[ ]" pending, "[•]" in_progress,
            // "[✓]" completed. The box is blue only on the in_progress row (matching
            // its text) and dim gray otherwise, so a marker adds no second hue;
            // completed rows are also struck through.
            Element marker;
            if (doing)
                marker = text("  [•] ") | color(doing_accent);
            else if (done)
                marker = text("  [✓] ") | color(pal.text_dim) | dim;
            else if (cancelled)
                marker = text("  [~] ") | color(pal.text_dim) | dim;
            else
                marker = text("  [ ] ") | color(pal.text_dim) | dim;

            // Content wraps to the whole row width minus the marker column
            // and the right margin; single-line ellipsis truncation is only
            // a last resort for a description longer than kMaxContentLines
            // wrapped lines.
            const int budget = row_width - kFixedCells - kRightPad;

            // The priority tag is docked right-aligned on the *first* content line, so
            // that line leaves room for it (re-wrapping at a narrower budget when it
            // would collide). When the pane is too narrow for the tag, it is dropped and
            // the content keeps the full width.
            const bool has_tag = !item.priority.empty();
            bool dock_tag = false;
            int wrap_budget = budget;
            if (has_tag)
            {
                const int tagw = StringWidth("[" + item.priority + "]");
                constexpr int kTagGapCells = 2;
                constexpr int kMinTagLineBudget = 8;
                const int room = budget - tagw - kTagGapCells;
                dock_tag = room >= kMinTagLineBudget;
                if (dock_tag)
                    wrap_budget = room;
            }
            std::vector<std::string> wrapped = WrapToWidth(item.content, budget);
            int used_budget = budget;
            if (dock_tag && !wrapped.empty() && StringWidth(wrapped[0]) > wrap_budget)
            {
                wrapped = WrapToWidth(item.content, wrap_budget);
                used_budget = wrap_budget;
            }
            if (wrapped.empty())
                wrapped.push_back(std::string());
            if (wrapped.size() > kMaxContentLines)
            {
                wrapped.resize(kMaxContentLines);
                std::string &last = wrapped.back();
                if (used_budget - StringWidth(last) >= 1)
                    last += "…";
            }

            // Content styling: only the in_progress row is bright blue — the
            // single accent of the whole list. Pending and completed rows
            // are dim neutral gray; completed adds strikethrough so "done"
            // stays legible without a color of its own. Every wrapped line
            // of the item gets the same treatment.
            auto style_content = [&](Element e) -> Element {
                if (doing)
                    return e | color(doing_accent);
                if (done || cancelled)
                    return e | dim | strikethrough | color(pal.text_dim);
                return e | dim | color(pal.text_dim);
            };

            Elements item_lines;
            // First content line: the marker stays glued to the left, the
            // content takes the middle and the priority tag (when docked)
            // sits at the far right, its right edge held kRightPad off the
            // pane border. flex() on the content pushes the tag across, so
            // every item's tag lands in the same right-aligned column no
            // matter how long the content is.
            Elements first_children;
            first_children.push_back(std::move(marker));
            first_children.push_back(style_content(text(wrapped[0])) | flex);
            if (dock_tag)
                first_children.push_back(text("[" + item.priority + "]") | dim | color(pal.text_dim));
            first_children.push_back(text(std::string(kRightPad, ' ')));
            item_lines.push_back(hbox(std::move(first_children)));
            for (size_t ln = 1; ln < wrapped.size(); ln++)
                item_lines.push_back(hbox({text(cont_prefix), style_content(text(wrapped[ln]))}));

            lines.push_back(vbox(std::move(item_lines)));
        }

        if (total > shown)
        {
            lines.push_back(text("  … +" + std::to_string(total - shown) + " more") | dim | color(pal.text_dim));
        }
    }

    return vbox(std::move(lines)) | reflect(content_box_) | borderStyled(BorderStyle::EMPTY) | bgcolor(pal.pane_bg);
}

} // namespace edc
