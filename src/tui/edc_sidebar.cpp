#include "edc_sidebar.hpp"
#include "edc_style.hpp"
#include "ftxui/dom/elements.hpp"

namespace edc
{

namespace
{
// The todo list is allowed to squeeze the task tree -- the step being worked on
// matters as much as the task that owns it, so the list takes the height it needs
// instead of having to share a short sidebar with the tree. The cap is what keeps
// that from swallowing the tree: the list may never take more than this share of
// the column, so the tree always keeps the rest of it. (Anchored to its bottom,
// the tree then still shows the active task's own row -- see ScrollOptions.)
constexpr int kTodosMaxShareNum = 2;
constexpr int kTodosMaxShareDen = 3;
} // namespace

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
    // fits, and scrolls when the cap (below) or a short pane hands it less.
    sidebar.todos = std::make_shared<ScrollView>(todo_body,
                                                 ScrollOptions{ScrollMode::kFitContent, /*show_indicator=*/false,
                                                               /*follow_tail=*/false,
                                                               /*focusable=*/false},
                                                 todo_header);
    auto tasks = sidebar.tasks;
    auto todos = sidebar.todos;
    // The column's own box, published by reflect() during layout and read on the
    // next frame to size the todo list's share of it. Nothing else reads it, so the
    // one-frame lag costs nothing: the next frame has it and a resize is redrawn.
    // y_max < y_min marks "no measurement yet": the first frame has none, and reading
    // the default-constructed box as a one-row pane would cap the list to almost
    // nothing on the very first paint.
    auto pane_box = std::make_shared<::ftxui::Box>();
    pane_box->y_min = 1;
    pane_box->y_max = 0;
    sidebar.container = ::ftxui::Container::Vertical({tasks, todos});
    sidebar.pane = ::ftxui::Renderer(sidebar.container, [tasks, todos, pane_box] {
        // Cap the todo section at kTodosMaxShareNum/kTodosMaxShareDen of the pane.
        // ftxui's size(LESS_THAN) does both halves of the rule at once: it clamps
        // the height this section can claim, and it clears the flex_shrink flag the
        // ScrollView sets, so on a short pane the *tree* absorbs the deficit (and
        // keeps what is left) instead of both sections being squeezed together.
        ::ftxui::Element todo_element = todos->Render() | ::ftxui::borderStyled(::ftxui::BorderStyle::EMPTY);
        const int pane_height = pane_box->y_max - pane_box->y_min + 1;
        if (pane_height > 0)
        {
            const int cap = std::max(1, pane_height * kTodosMaxShareNum / kTodosMaxShareDen);
            todo_element = std::move(todo_element) | ::ftxui::size(::ftxui::HEIGHT, ::ftxui::LESS_THAN, cap);
        }

        // The sections stack with no divider (a separator would slice the sidebar
        // into fragments); one EMPTY border around each keeps every section's pinned
        // header and its rows sharing an inset.
        ::ftxui::Elements col = {tasks->Render() | ::ftxui::borderStyled(::ftxui::BorderStyle::EMPTY),
                                 std::move(todo_element)};
        // The column itself flexes so the sidebar background covers the whole pane,
        // but its children stay at natural height and are laid out from the top:
        // leftover vertical space remains at the bottom instead of being absorbed by
        // the task tree to push the todo section down.
        return ::ftxui::vbox(std::move(col)) | ::ftxui::bgcolor(style::palette().pane_bg) | ::ftxui::flex |
               ::ftxui::reflect(*pane_box);
    });
    return sidebar;
}

} // namespace edc
