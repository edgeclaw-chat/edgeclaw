#include "edc_task_widget.hpp"
#include "edc_text_util.hpp"
#include "ftxui/component/mouse.hpp"
#include "ftxui/dom/elements.hpp"

namespace edc
{

namespace
{

// Map a scheduler task-state name to its sidebar color.
::ftxui::Color StateColor(const std::string &state)
{
    if (state == "COMPLETED")
        return ::ftxui::Color::RGB(64, 168, 96); // green
    if (state == "ERROR")
        return ::ftxui::Color::RGB(224, 108, 117); // red
    if (state == "LLM_PROCESSING" || state == "TOOL_CALLING")
        return ::ftxui::Color::RGB(94, 151, 247); // blue
    if (state == "SUSPENDED")
        return ::ftxui::Color::RGB(198, 120, 221); // magenta
    if (state == "CANCELLED")
        return ::ftxui::Color::RGB(147, 147, 147); // gray
    // IDLE / WAITING_DEPS / WAITING_INPUT
    return ::ftxui::Color::RGB(229, 192, 123); // yellow
}

// Cells guaranteed to stay free between a task's description and its state
// label. Kept as a fixed non-flex element so the state never glues itself to
// a long (truncated) description.
constexpr int kDescStateGapCells = 2;

// Blank cells reserved at the right edge of every full-width row. The
// sidebar pane is the window's rightmost element: rows used to truncate /
// right-align right up to the inner border, so long text and the state
// labels butted against the terminal edge while the left side kept a visible
// gutter (split separator + border inset). Keeping this much of every text
// row free on the right restores a balanced inset.
constexpr int kRightPad = 2;

// Upper bound (in code points) kept per task when importing a snapshot. The
// exact per-row fit is decided at render time by TruncateToWidth(); this cap
// only stops a pathological description from being copied/stored in full.
constexpr size_t kMaxDescCodepoints = 200;

} // namespace

TaskWidget::TaskWidget()
{
}

void TaskWidget::SetSnapshot(const ::edc_c_task_snapshot *snap)
{
    session_id_.clear();
    session_name_.clear();
    created_at_.clear();
    message_count_ = 0;
    groups_.clear();

    if (!snap)
    {
        group_expanded_.clear();
        return;
    }

    session_id_ = snap->session_id ? snap->session_id : "";
    session_name_ = snap->session_name ? snap->session_name : "";
    created_at_ = snap->created_at ? snap->created_at : "";
    message_count_ = snap->message_count;

    std::string active_group_id;
    for (int gi = 0; gi < snap->group_count; gi++)
    {
        const ::edc_c_task_group *g = &snap->groups[gi];

        GroupInfo group;
        group.group_id = g->group_id ? g->group_id : "";
        group.member_count = g->member_count;
        group.orphaned = g->orphaned != 0;
        group.fg = g->fg != 0;

        for (int ti = 0; ti < g->task_count; ti++)
        {
            const ::edc_c_task_info *t = &g->tasks[ti];

            TaskInfo task;
            task.task_id = t->task_id ? t->task_id : "";
            task.state = t->state ? t->state : "";
            task.is_leader = t->is_leader != 0;
            task.is_active = t->is_active != 0;
            task.description = TruncateUtf8(t->description ? t->description : "", kMaxDescCodepoints);
            if (task.is_active)
                active_group_id = group.group_id;
            group.tasks.push_back(std::move(task));
        }

        groups_.push_back(std::move(group));
    }

    // Refresh collapse state: keep user toggles for groups that still exist;
    // a newly-seen group is expanded only when it holds the active task.
    std::map<std::string, bool> next;
    for (const auto &g : groups_)
    {
        const auto it = group_expanded_.find(g.group_id);
        next[g.group_id] = (it != group_expanded_.end()) ? it->second : (g.group_id == active_group_id);
    }
    group_expanded_ = std::move(next);
}

bool TaskWidget::OnEvent(::ftxui::Event event)
{
    if (!event.is_mouse())
        return false;

    if (event.mouse().button != ::ftxui::Mouse::Left || event.mouse().motion != ::ftxui::Mouse::Pressed)
        return false;

    // Clicking the Tasks section header folds/unfolds the whole tree. The
    // header box is recorded by reflect() over the full-width header row, so
    // a click anywhere on that row toggles the section.
    if (section_header_box_.Contain(event.mouse().x, event.mouse().y))
    {
        section_collapsed_ = !section_collapsed_;
        return true;
    }

    for (size_t i = 0; i < group_header_boxes_.size() && i < groups_.size(); i++)
    {
        if (group_header_boxes_[i].Contain(event.mouse().x, event.mouse().y))
        {
            const std::string &gid = groups_[i].group_id;
            group_expanded_[gid] = !group_expanded_[gid];
            return true;
        }
    }
    return false;
}

::ftxui::Element TaskWidget::OnRender()
{
    using namespace ::ftxui;

    Elements lines;

    // Collapsible Tasks section header: the arrow shows the fold state, the
    // counts on the right summarize what a collapsed section hides. The row
    // is full width (reflect below records its box) so clicking anywhere on
    // it toggles the whole tree (see OnEvent).
    {
        int group_count = static_cast<int>(groups_.size());
        int task_count = 0;
        for (const auto &g : groups_)
            task_count += static_cast<int>(g.tasks.size());

        Elements head = {
            text((section_collapsed_ ? "▸ " : "▾ ") + std::string(" Tasks ")) | bold |
                color(Color::RGB(94, 151, 247)),
            filler(),
        };
        if (group_count > 0)
        {
            head.push_back(text(std::to_string(group_count) + "g · " + std::to_string(task_count) + "t") |
                           dim | color(Color::RGB(147, 147, 147)));
        }
        head.push_back(text(std::string(kRightPad, ' ')));
        lines.push_back(hbox(std::move(head)) | reflect(section_header_box_));
    }

    // Width available for a text row. reflect() below sits on the vbox
    // *inside* the (empty-style) border, because ftxui's Border node insets
    // its child by one column on each side even for BorderStyle::EMPTY — the
    // outer box would overestimate the row width by 2 and the extra-wide
    // description would then be flex-shrunk, clipping the trailing "…".
    // reflect() fills content_box_ during the render pass, so this reads the
    // previous frame's width; before the first measurement fall back to the
    // default sidebar width (36) minus the 2 border columns.
    int row_width = 34;
    if (content_box_.x_min != 0 || content_box_.x_max != 0 || content_box_.y_min != 0 || content_box_.y_max != 0)
        row_width = content_box_.x_max - content_box_.x_min + 1;

    if (section_collapsed_)
    {
        // A collapsed section keeps only its header row; the session stats,
        // separator and the group list are skipped entirely. No group
        // headers are drawn, so drop their click boxes to avoid stale hit
        // targets lingering from the expanded frame.
        group_header_boxes_.clear();
    }
    else
    {
        if (!session_name_.empty() || !session_id_.empty())
        {
            // Session line: display name, then the id in brackets when distinct.
            std::string sid = session_name_.empty() ? session_id_ : session_name_;
            if (!session_id_.empty() && session_name_ != session_id_)
                sid += " [" + session_id_ + "]";
            lines.push_back(text(" " + TruncateToWidth(sid, row_width - 1 - kRightPad)) | dim);

            // Session stats, mirroring /session list: message count and the
            // date the session was created ("YYYY-MM-DD HH:MM:SS" → date only).
            std::string stats = std::to_string(message_count_) + " msgs";
            if (created_at_.size() >= 10)
                stats += " · created " + created_at_.substr(0, 10);
            lines.push_back(text(" " + TruncateToWidth(stats, row_width - 1 - kRightPad)) | dim |
                            color(Color::RGB(147, 147, 147)));
        }
        lines.push_back(separator());

        if (groups_.empty())
        {
            lines.push_back(text(" (no tasks)") | dim | color(Color::RGB(147, 147, 147)));
        }

        group_header_boxes_.resize(groups_.size());

        for (size_t i = 0; i < groups_.size(); i++)
        {
            const auto &g = groups_[i];
            const bool expanded = group_expanded_[g.group_id];

            // Collapsible group header: an arrow shows the state; foreground
            // is highlighted with a warm color, orphaned groups are marked.
            std::string header = (expanded ? "▾ " : "▸ ") + g.group_id;
            if (g.orphaned)
                header += " ~";
            header += " (" + std::to_string(g.member_count) + ")";

            lines.push_back(hbox({text(header), filler()}) | bold |
                            color(g.fg ? Color::RGB(229, 192, 123) : Color::RGB(147, 147, 147)) |
                            reflect(group_header_boxes_[i]));

            if (!expanded)
                continue;

            for (const auto &t : g.tasks)
            {
                // Left part: indent, active/leader markers and the task id.
                // The description is appended separately so its length can be
                // computed against the real row width.
                std::string left = "  ";
                left += t.is_active ? "*" : " ";
                left += t.is_leader ? "•" : " ";
                left += " " + t.task_id;

                const int state_cells = StringWidth(t.state);

                if (!t.description.empty())
                {
                    // Two cells separate the id from the description (kept
                    // from the previous layout), then kDescStateGapCells stay
                    // free before the state label; kRightPad keeps the state
                    // label itself off the pane's right edge.
                    const int budget = row_width - StringWidth(left) - 2 - kDescStateGapCells - state_cells -
                                       kRightPad;
                    if (budget > 0)
                        left += "  " + TruncateToWidth(t.description, budget);
                }

                auto row = hbox({
                    text(left) | flex,
                    text(std::string(kDescStateGapCells, ' ')),
                    text(t.state) | color(StateColor(t.state)),
                    text(std::string(kRightPad, ' ')),
                });
                lines.push_back(row);
            }
        }
    }

    // No flex here: the enclosing right-pane column owns the vertical space
    // and stacks this section above the Todo section in natural order. This
    // widget only ever occupies its own content height (plus its own EMPTY
    // border/background), so the todo list below is never pushed to the pane
    // bottom by a greedy task tree.
    return vbox(std::move(lines)) | reflect(content_box_) | borderStyled(BorderStyle::EMPTY) |
           bgcolor(Color::RGB(30, 30, 30));
}

} // namespace edc
