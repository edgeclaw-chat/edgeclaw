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
// Right-sidebar view of the active session's task tree, as a collapsible section:
// the "▾/▸ Tasks" header folds the whole tree and each group header folds its
// tasks. It owns no tree state -- that lives in a TaskTreeModel, so a background
// snapshot refresh cannot undo a manual fold.
class TaskWidget : public ::ftxui::ComponentBase
{
  public:
    // Self-owned model (tests / standalone use).
    TaskWidget();
    // Shared model (the window injects one it also lets the dispatcher update).
    explicit TaskWidget(std::shared_ptr<model::TaskTreeModel> tree);

    // Copy the parsed snapshot into the model; the snapshot is freed by the caller.
    void SetSnapshot(const TaskSnapshot *snap);

    // The model backing this view (never null).
    const std::shared_ptr<model::TaskTreeModel> &model() const
    {
        return tree_;
    }

    // Phase the breathing dot of a processing task; |elapsed_ms| is the window's
    // animation clock, so the dot breathes on the working bar's tempo. Static
    // states ignore it.
    void SetBreath(long long elapsed_ms);

    // True while any task in the tree is processing. The window uses it to keep
    // requesting animation frames even when the foreground process is idle.
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
    // Row width both halves truncate against, from the previous render's box (or
    // the default sidebar width before that).
    int RowWidth() const;

    std::shared_ptr<model::TaskTreeModel> tree_;

    // Phase of the processing dots' breathing, in ms on the window's clock.
    long long breath_ms_ = 0;

    // Click target of the Tasks header row, populated during OnRender.
    ::ftxui::Box section_header_box_;

    // Per-group header click targets, populated during OnRender.
    std::vector<::ftxui::Box> group_header_boxes_;

    // Content box of this widget's inner vbox, measured during the previous
    // render and read to truncate descriptions to the real row width. All-zero
    // means "not measured yet".
    ::ftxui::Box content_box_;
};
} // namespace edc
