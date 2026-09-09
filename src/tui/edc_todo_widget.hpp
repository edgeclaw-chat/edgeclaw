#pragma once

#include "ftxui/component/component_base.hpp"
#include "ftxui/component/event.hpp"
#include "ftxui/screen/box.hpp"
#include <string>
#include <vector>

namespace edc
{
// Right-sidebar widget below the task tree: renders the todo list of the
// current foreground task — the task marked active in the foreground group of
// the task-tree snapshot. The list is part of the task state (todowrite
// attaches the items it validated to the calling task; the snapshot JSON
// carries every task's "todolist" and the C bridge parses it back). The TUI
// window therefore feeds this widget from each snapshot it renders
// (ActiveTaskTodoItems), never from a side channel on the tool's chat
// message: one source of truth for the whole sidebar.
//
// Like the task tree it forms an independent, collapsible sidebar section:
// the "▾/▸ Todos" header row is clickable and toggles the item list while the
// header (with its progress summary) always stays visible. The section is
// always present in the pane: when no active task holds a list yet — or its
// list is empty — the header renders a "0/0" summary. The section
// starts collapsed at startup (an empty body has nothing worth showing); if
// the user expands an empty section it shows a dim "(empty)" row. The first
// non-empty list that arrives auto-expands the section, after which the fold
// state follows the user's clicks across refreshes.
//
// Item text is word-wrapped over several lines (CJK-safe, edc_text_util) so
// long descriptions use the full sidebar width instead of one truncated row;
// each item carries a compact priority tag ("[high]" / "[medium]" / "[low]"),
// and cancelled items are struck through like completed ones.
class TodoWidget : public ::ftxui::ComponentBase
{
  public:
    struct Item
    {
        std::string content;
        std::string status;   /* pending | in_progress | completed | cancelled */
        std::string priority; /* high | medium | low ("" when unset) */
    };

    TodoWidget();

    // Replace the whole displayed list. A call with no items clears the rows
    // (and resets the summary to 0/0) but the section header stays visible.
    void SetItems(const std::vector<Item> &items);

    ::ftxui::Element OnRender() override;
    bool OnEvent(::ftxui::Event event) override;
    bool Focusable() const override
    {
        return false;
    }

  private:
    std::vector<Item> items_;
    int completed_ = 0;

    // Collapse state for the whole section. Starts collapsed: at startup (and
    // after the tool cleared the list) there is nothing to show, so the
    // section keeps only its "Todos c/n" header row instead of an expanded
    // "(empty)" body. Once a real list is visible the state is preserved
    // across SetItems() so a user's manual fold survives background refreshes;
    // SetItems() auto-expands only on an empty → non-empty transition.
    bool collapsed_ = true;

    // Click target of the section header row, populated during OnRender.
    ::ftxui::Box header_box_;

    // Cap the rows drawn so a huge list cannot swallow the task tree above;
    // leftovers are summarized with a "+N more" line.
    static constexpr size_t kMaxVisibleRows = 10;

    // Cap the wrapped lines drawn for one item's content; an over-long
    // description is clipped with a trailing "…" so a single item cannot
    // dominate the sidebar.
    static constexpr size_t kMaxContentLines = 4;

    // Content box of this widget's inner vbox (rows), written by reflect()
    // during the previous render pass. OnRender reads it to truncate item
    // contents to the real row width. All-zero means "not measured yet".
    ::ftxui::Box content_box_;
};
} // namespace edc
