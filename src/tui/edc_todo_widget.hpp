#pragma once

#include "ftxui/component/component_base.hpp"
#include "ftxui/component/event.hpp"
#include "ftxui/screen/box.hpp"
#include <string>
#include <vector>

namespace edc
{
// Right-sidebar section under the task tree: the current foreground task's todo
// list (part of the task state, carried by the snapshot JSON -- no side channel).
// Header "▾/▸ Todos" toggles the items; starts collapsed and auto-expands on the
// first non-empty list. The in_progress row is a static highlight that takes the
// failure colour when the owning task failed (see SetOwnerState).
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

    // Replace the whole displayed list. |task_id| names the owning task (shown
    // as "#<id>" while the list has items); "" when unknown. No items clears the
    // rows and resets the summary to 0/0, but the header stays visible.
    void SetItems(const std::vector<Item> &items, const std::string &task_id);

    // The owning task's scheduler state ("" when unattached or unknown). Decides
    // the in_progress row's colour via the same task-state table the tree uses:
    // ERROR -> the failure colour; anything else -> the section's plain accent.
    // Deliberately not animated: render is a pure function of items + this state.
    void SetOwnerState(const std::string &task_state);

    // The section in two halves, like the task tree: the header (title, task id,
    // progress counter) and the body (items). The window pins the header above a
    // ScrollView; Render() stacks both halves and insets them.
    ::ftxui::Element RenderHeader();
    ::ftxui::Element RenderBody();
    ::ftxui::Element OnRender() override;
    bool OnEvent(::ftxui::Event) override;
    bool Focusable() const override
    {
        return false;
    }

  private:
    // Width a section row may use (previous frame's content box, or the default
    // sidebar width before the first measurement). Shared by both halves.
    int RowWidth() const;

    std::vector<Item> items_;
    int completed_ = 0;
    // The task whose list this is, shown as "#<id>"; empty when unattached.
    std::string task_id_;

    // Preserved across SetItems() so a manual fold survives background refreshes;
    // SetItems() auto-expands only on empty -> non-empty.
    bool collapsed_ = true;

    // The owning task's state, as last set by the window; only the in_progress
    // row's colour reads it.
    std::string owner_state_;

    // Click target of the section header row, populated during OnRender.
    ::ftxui::Box header_box_;

    // Cap the rows drawn so a huge list cannot swallow the task tree above;
    // leftovers are summarized with a "+N more" line.
    static constexpr size_t kMaxVisibleRows = 10;

    // Cap the wrapped lines per item, clipped with a trailing "…".
    static constexpr size_t kMaxContentLines = 4;

    // Content box of the inner vbox, written by reflect() during the previous
    // render pass; all-zero means "not measured yet".
    ::ftxui::Box content_box_;
};
} // namespace edc
