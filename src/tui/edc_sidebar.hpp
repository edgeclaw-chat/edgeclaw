#pragma once

#include "edc_scroll_view.hpp"
#include "edc_task_widget.hpp"
#include "edc_todo_widget.hpp"
#include "edc_widget.hpp" /* ui::Widget */

#include "ftxui/component/component_base.hpp"

#include <memory>

namespace edc::ui
{

// The sidebar column: the task tree above the todo list.
//
// Both sections are fit-content ScrollViews with their own headers pinned above
// the viewport, so a short sidebar is *shared* instead of one section eating the
// other's rows. The tree is anchored to its bottom (follow_tail): when it does not
// fit, the oldest rows leave the top and the active task (the last row) stays on
// screen. The todo list keeps its tail off (items are read from the top) and is
// capped so it can never swallow the tree.
//
// A real composite component rather than a bare struct: the window (and a test)
// holds one object and renders it; the two ScrollViews stay reachable for
// geometry assertions and wheel events.
class SidebarView : public Widget
{
  public:
    SidebarView(std::shared_ptr<TaskWidget> task, std::shared_ptr<TodoWidget> todo);

    // The two scrollable sections (a test reads their viewport height / sends
    // wheel events; the tree is |tasks|, the todo list is |todos|).
    const std::shared_ptr<ScrollView> &tasks() const
    {
        return tasks_;
    }
    const std::shared_ptr<ScrollView> &todos() const
    {
        return todos_;
    }

    // The rendered column (ResizableSplit borrows this as its right pane).
    ::ftxui::Component pane() const
    {
        return pane_;
    }

    bool OnEvent(::ftxui::Event event) override;

  protected:
    // Widget 模板方法: 只实现绘制。
    ::ftxui::Element Draw(const Frame &frame) override;

  private:
    std::shared_ptr<ScrollView> tasks_;
    std::shared_ptr<ScrollView> todos_;
    // Event routing for the whole column (a click on either section header).
    ::ftxui::Component container_;
    // The rendered column, each section inset by its own empty border.
    ::ftxui::Component pane_;
};

} // namespace edc::ui
