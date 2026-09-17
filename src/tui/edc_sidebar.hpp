#pragma once

#include "edc_scroll_view.hpp"
#include "edc_task_widget.hpp"
#include "edc_todo_widget.hpp"
#include "ftxui/component/component.hpp"
#include <memory>

namespace edc
{

// The sidebar column: the task tree above the todo list.
//
// Both sections are fit-content ScrollViews with their own headers pinned above
// the viewport, so a short sidebar is *shared* instead of one section eating the
// other's rows. That matters because ftxui's flex_shrink starts at 0: a section
// that does not opt in keeps every row it asks for, and the section that opts in
// (the tree) is squeezed until its own heading is gone.
//
// The tree is anchored to its *bottom* (follow_tail): when the tree does not fit,
// the rows that leave the pane are the oldest ones at the top, and the row the
// session is working on -- the active task, the last row of the tree -- stays on
// screen. Wheel scrolling up releases the anchor and scrolling back to the bottom
// re-arms it (see ScrollState), so the viewport follows work only while the reader
// is already watching the tail. The todo list keeps its tail off: its items are
// read from the top, where the current step sits. Neither view takes keyboard
// focus, so a refresh never steals Tab/arrows from the editor.
//
// Built here rather than inside TUIWindow so the layout can be pinned by a test:
// test_scroll_regression composes this exact column, and these decisions are what
// it asserts (the active row survives a squeeze, the oldest rows leave the window
// first, both headings stay pinned).
struct Sidebar
{
    std::shared_ptr<ScrollView> tasks;
    std::shared_ptr<ScrollView> todos;
    // Event routing for the whole column: a click on either section header (folding
    // a group, folding the list) still reaches the widget that owns it.
    ::ftxui::Component container;
    // The rendered column, each section inset by its own empty border.
    ::ftxui::Component pane;
};

Sidebar MakeSidebar(std::shared_ptr<TaskWidget> task, std::shared_ptr<TodoWidget> todo);

} // namespace edc
