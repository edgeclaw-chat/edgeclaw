#pragma once

#include "edc_task_snapshot.hpp"
#include "edc_task_tree_model.hpp"
#include "ftxui/component/component_base.hpp"
#include "ftxui/component/event.hpp"
#include "ftxui/screen/box.hpp"
#include <memory>
#include <string>
#include <vector>

namespace edc
{
// Right-sidebar **view** of the active session's task tree (process groups and
// their member tasks) as a collapsible section: the "▾/▸ Tasks" header folds the
// whole tree and each group header folds its own tasks.
//
// It owns no tree state: the parsed tree and the fold state live in a
// TaskTreeModel (so a background snapshot refresh cannot undo a manual fold, and
// a dispatcher can update the model without touching rendering). The compat
// SetSnapshot() below forwards to the model.
class TaskWidget : public ::ftxui::ComponentBase
{
  public:
    // Self-owned model (tests / standalone use).
    TaskWidget();
    // Shared model (the window injects one it also lets the dispatcher update).
    explicit TaskWidget(std::shared_ptr<model::TaskTreeModel> tree);

    // ── 兼容入口: 委托给模型 ──────────────────────────────────
    // Copy the parsed snapshot into the model. The snapshot stays owned by the
    // caller and is freed after this call.
    void SetSnapshot(const TaskSnapshot *snap);

    // The model backing this view (never null).
    const std::shared_ptr<model::TaskTreeModel> &model() const
    {
        return tree_;
    }

    // Phase the breathing dot of a task in a processing state. |elapsed_ms| is
    // the window's animation clock (see TUIWindow::TickAnimations), so the dot
    // breathes on the same tempo as the working bar. Static states ignore it.
    void SetBreath(long long elapsed_ms);

    // True while any task in the tree is in a processing state (delegates to the
    // model). The window uses it to keep asking for animation frames even when the
    // foreground process itself is idle (a background task still has to breathe).
    bool HasProcessingTask() const;

    // The section in two halves: RenderHeader() is everything above the list (the
    // "Tasks" row, the session meta and the rule), RenderBody() the group rows.
    // The sidebar pins the header above a ScrollView owning the body, so the
    // wheel moves the tree while the heading stays put.
    ::ftxui::Element RenderHeader();
    ::ftxui::Element RenderBody();

    ::ftxui::Element OnRender() override;
    bool OnEvent(::ftxui::Event event) override;

    bool Focusable() const override
    {
        return false;
    }

  private:
    // Row width both halves truncate against, from the box the previous render
    // measured (the default sidebar width before that).
    int RowWidth() const;

    std::shared_ptr<model::TaskTreeModel> tree_;

    // Phase of the processing dots' breathing, in ms on the window's animation
    // clock. Only read for tasks in a processing state (see SetBreath).
    long long breath_ms_ = 0;

    // Click target of the Tasks header row, populated during OnRender.
    ::ftxui::Box section_header_box_;

    // Per-group header click targets, populated during OnRender.
    std::vector<::ftxui::Box> group_header_boxes_;

    // Content box of this widget's inner vbox, written by reflect() during the
    // previous render and read to truncate descriptions to the real row width
    // (instead of letting flex-shrink clip them flush against the state label).
    // The box is measured inside the border, whose EMPTY style still insets its
    // child by one column per side. All-zero means "not measured yet".
    ::ftxui::Box content_box_;
};
} // namespace edc
