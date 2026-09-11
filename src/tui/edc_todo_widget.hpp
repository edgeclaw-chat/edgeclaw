#pragma once

#include "ftxui/component/component_base.hpp"
#include "ftxui/component/event.hpp"
#include "ftxui/screen/box.hpp"
#include <string>
#include <vector>

namespace edc
{
// Right-sidebar section under the task tree: the todo list of the current foreground task.
// The list is part of the task state (todowrite attaches the items to the calling task, the
// snapshot JSON carries them), so there is no side channel from the tool's chat message.
//
// Its "▾/▸ Todos" header toggles the items and always stays visible with the progress
// summary. It starts collapsed and auto-expands on the first non-empty list. Item text is
// word-wrapped, each item carries a priority tag, cancelled items are struck through.
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

    // Replace the whole displayed list. |task_id| names the task the items belong
    // to and is shown as "#<id>" in the header while the list has items; pass ""
    // when the list belongs to no known task. No items clears the rows and
    // resets the summary to 0/0, but the header stays visible.
    void SetItems(const std::vector<Item> &items, const std::string &task_id);

    ::ftxui::Element OnRender() override;
    bool OnEvent(::ftxui::Event) override;
    bool Focusable() const override
    {
        return false;
    }

  private:
    std::vector<Item> items_;
    int completed_ = 0;
    // The task whose list this is, shown as "#<id>" so the reader can tell at a
    // glance; empty when unattached.
    std::string task_id_;

    // Collapse state, preserved across SetItems() so a manual fold survives
    // background refreshes; SetItems() auto-expands only on empty -> non-empty.
    bool collapsed_ = true;

    // Click target of the section header row, populated during OnRender.
    ::ftxui::Box header_box_;

    // Cap the rows drawn so a huge list cannot swallow the task tree above;
    // leftovers are summarized with a "+N more" line.
    static constexpr size_t kMaxVisibleRows = 10;

    // Cap the wrapped lines per item, clipped with a trailing "…" so one item
    // cannot dominate the sidebar.
    static constexpr size_t kMaxContentLines = 4;

    // Content box of the inner vbox, written by reflect() during the previous
    // render pass and read to truncate contents to the real row width. All-zero
    // means "not measured yet".
    ::ftxui::Box content_box_;
};
} // namespace edc
