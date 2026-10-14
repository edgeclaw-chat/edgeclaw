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

    // The section starts collapsed, so an empty -> non-empty transition expands it
    // automatically. A fold the user made while the list was visible survives
    // refreshes (this only fires when the previous state had no rows to fold).
    if (was_empty && !items_.empty())
        collapsed_ = false;
}

void TodoWidget::SetOwnerState(const std::string &task_state)
{
    owner_state_ = task_state;
}

bool TodoWidget::OnEvent(::ftxui::Event event)
{
    if (!event.is_mouse())
        return false;

    if (event.mouse().button != ::ftxui::Mouse::Left || event.mouse().motion != ::ftxui::Mouse::Pressed)
        return false;

    // The header row is full width (box recorded by reflect() last frame), so a
    // click anywhere on it toggles the list.
    if (header_box_.Contain(event.mouse().x, event.mouse().y))
    {
        collapsed_ = !collapsed_;
        return true;
    }
    return false;
}

int TodoWidget::RowWidth() const
{
    // reflect() sits on each half *inside* the (empty-style) inset; before the
    // first measurement, fall back to the default sidebar width (36) minus the 2
    // inset columns.
    if (content_box_.x_min != 0 || content_box_.x_max != 0 || content_box_.y_min != 0 || content_box_.y_max != 0)
        return content_box_.x_max - content_box_.x_min + 1;
    return 34;
}

::ftxui::Element TodoWidget::RenderHeader()
{
    using namespace ::ftxui;

    const auto &pal = style::palette();

    // Blank cells kept free at the right edge; the sidebar is the window's
    // rightmost element, so rows would otherwise butt against the terminal edge.
    constexpr int kRightPad = 2;
    const size_t total = items_.size();
    const int row_width = RowWidth();

    Elements header_row;
    {
        // Collapsible header: the arrow shows the fold state, the summary always
        // reflects the whole list, and the row is full width so a click anywhere
        // toggles the section (header_box_ is filled by reflect below).
        const std::string summary = std::to_string(completed_) + "/" + std::to_string(total);
        const std::string title = (collapsed_ ? "▸ " : "▾ ") + std::string(" Todos ");

        // The header names the owning task ("#<id>", in the section accent) while
        // there is a list; the id is truncated so it never pushes the summary out.
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
            header.push_back(text(id_text) | color(pal.accent));
        }
        header.push_back(filler());
        header.push_back(text(summary) | color(pal.accent) | bold);
        header.push_back(text(std::string(kRightPad, ' ')));
        header_row.push_back(hbox(std::move(header)) | reflect(header_box_));
    }

    // The rule belongs to the header, not the scrolling body, so a pinned header
    // never drifts with the items.
    if (collapsed_)
        return vbox(std::move(header_row)) | reflect(content_box_);
    header_row.push_back(separator() | color(pal.line_dim));
    return vbox(std::move(header_row)) | reflect(content_box_);
}

::ftxui::Element TodoWidget::RenderBody()
{
    using namespace ::ftxui;

    const auto &pal = style::palette();

    // Only one row is ever highlighted -- the in_progress row; pending/completed
    // rows are neutral gray (completed keeps its strikethrough). Its colour comes
    // from the same task-state table the tree uses, so ERROR here wears the same
    // red as the task's own marker. The row is a static highlight (no clock):
    // render is a pure function of the items and the owner state.
    const ::ftxui::Color doing_tint =
        style::TaskStateFailed(owner_state_) ? style::TaskStateColor(pal, owner_state_) : pal.accent;

    constexpr int kRightPad = 2;
    const size_t total = items_.size();
    const size_t shown = std::min(total, kMaxVisibleRows);
    const int row_width = RowWidth();

    Elements lines;
    if (collapsed_)
        return vbox(std::move(lines)) | reflect(content_box_);
    // An expanded but empty section still draws a dim marker, so the sidebar
    // layout stays stable.
    if (items_.empty())
        lines.push_back(text("  (empty)") | dim | color(pal.text_dim));

    // Fixed cells per row: 2 indent + a 3-cell bracket marker + 1 gap. Continuation
    // lines are indented by the same kFixedCells so text aligns under the content.
    constexpr int kFixedCells = 6;
    const std::string cont_prefix(kFixedCells, ' ');

    for (size_t i = 0; i < shown; i++)
    {
        const Item &item = items_[i];
        const bool done = item.status == "completed";
        const bool doing = item.status == "in_progress";
        const bool cancelled = item.status == "cancelled";

        // Bracket marker: "[ ]" pending, "[•]" in_progress, "[✓]" completed,
        // "[~]" cancelled. Only the in_progress marker wears the row tint.
        Element marker;
        if (doing)
            marker = text("  [•] ") | color(doing_tint);
        else if (done)
            marker = text("  [✓] ") | color(pal.text_dim) | dim;
        else if (cancelled)
            marker = text("  [~] ") | color(pal.text_dim) | dim;
        else
            marker = text("  [ ] ") | color(pal.text_dim) | dim;

        const int budget = row_width - kFixedCells - kRightPad;

        // The priority tag is docked right-aligned on the first content line, so
        // that line narrows to leave room for it; when the pane is too narrow the
        // tag is dropped and the content keeps the full width.
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

        // Only the in_progress row carries the tint; other rows are dim, with
        // completed/cancelled struck through.
        auto style_content = [&](Element e) -> Element {
            if (doing)
                return e | color(doing_tint);
            if (done || cancelled)
                return e | dim | strikethrough | color(pal.text_dim);
            return e | dim | color(pal.text_dim);
        };

        Elements item_lines;
        // First line: marker, content, and the right-docked tag. flex() on the
        // content right-aligns the tag in one shared column across items.
        Elements first_children;
        first_children.push_back(std::move(marker));
        first_children.push_back(style_content(text(wrapped[0])) | flex);
        if (dock_tag)
        {
            // The tag wears the item's own styling, so the line runs unbroken
            // (verified in test_todo_widget.cpp).
            first_children.push_back(style_content(text("[" + item.priority + "]")));
        }
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

    return vbox(std::move(lines)) | reflect(content_box_);
}

::ftxui::Element TodoWidget::OnRender()
{
    using namespace ::ftxui;

    // The section as one block: the window splits it into header/body instead so
    // it can pin the header above a viewport; this is that composition with the
    // section's own inset, for callers and tests that want a single element.
    return vbox({RenderHeader(), RenderBody()}) |
           borderStyled(::ftxui::BorderStyle::EMPTY) | bgcolor(style::palette().pane_bg);
}

} // namespace edc
