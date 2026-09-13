#include "edc_sidebar.hpp"
#include "edc_style.hpp"
#include "ftxui/dom/elements.hpp"

namespace edc
{

Sidebar MakeSidebar(std::shared_ptr<TaskWidget> task, std::shared_ptr<TodoWidget> todo)
{
    // The container keeps mouse/key events reaching both widgets: a childless
    // Renderer would swallow them (TaskWidget needs its group-header clicks, the
    // todo section its header).
    auto task_header = ::ftxui::Renderer(task, [task] { return task->RenderHeader(); });
    auto task_body = ::ftxui::Renderer(task, [task] { return task->RenderBody(); });
    auto todo_header = ::ftxui::Renderer(todo, [todo] { return todo->RenderHeader(); });
    auto todo_body = ::ftxui::Renderer(todo, [todo] { return todo->RenderBody(); });

    Sidebar sidebar;
    sidebar.tasks = std::make_shared<ScrollView>(task_body,
                                                 ScrollOptions{ScrollMode::kFitContent, /*show_indicator=*/false,
                                                               /*follow_tail=*/true,
                                                               /*focusable=*/false},
                                                 task_header);
    // kFitContent rather than kFill: the list claims its natural height when it
    // fits, and scrolls when a short pane hands it less.
    sidebar.todos = std::make_shared<ScrollView>(todo_body,
                                                 ScrollOptions{ScrollMode::kFitContent, /*show_indicator=*/false,
                                                               /*follow_tail=*/false,
                                                               /*focusable=*/false},
                                                 todo_header);
    auto tasks = sidebar.tasks;
    auto todos = sidebar.todos;
    sidebar.container = ::ftxui::Container::Vertical({tasks, todos});
    sidebar.pane = ::ftxui::Renderer(sidebar.container, [tasks, todos] {
        // The sections stack with no divider (a separator would slice the sidebar
        // into fragments); one EMPTY border around each keeps every section's pinned
        // header and its rows sharing an inset.
        ::ftxui::Elements col = {tasks->Render() | ::ftxui::borderStyled(::ftxui::BorderStyle::EMPTY),
                                 todos->Render() | ::ftxui::borderStyled(::ftxui::BorderStyle::EMPTY)};
        // The column itself flexes so the sidebar background covers the whole pane,
        // but its children stay at natural height and are laid out from the top:
        // leftover vertical space remains at the bottom instead of being absorbed by
        // the task tree to push the todo section down.
        return ::ftxui::vbox(std::move(col)) | ::ftxui::bgcolor(style::palette().pane_bg) | ::ftxui::flex;
    });
    return sidebar;
}

} // namespace edc
