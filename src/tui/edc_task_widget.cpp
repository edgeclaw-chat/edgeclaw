#include "edc_task_widget.hpp"
#include "edc_style.hpp"
#include "edc_text_util.hpp"
#include "ftxui/component/mouse.hpp"
#include "ftxui/dom/elements.hpp"
#include <cstdio>

namespace edc
{

namespace
{

// A task is "processing" (working right now) in exactly these two states; they
// are the ones the dot animates for, and the ones `edc_task_is_running` marks
// in the scheduler.
bool StateIsProcessing(const std::string &state)
{
    return state == "LLM_PROCESSING" || state == "TOOL_CALLING";
}

// Map a scheduler task-state name to the color of its sidebar dot.
::ftxui::Color StateColor(const std::string &state)
{
    // Task states map onto the central style palette (edc_style.hpp).
    const auto &pal = style::palette();
    if (state == "COMPLETED")
        return pal.ok;
    if (state == "ERROR")
        return pal.error;
    if (StateIsProcessing(state))
        return pal.accent;
    if (state == "SUSPENDED")
        return pal.suspended;
    if (state == "CANCELLED")
        return pal.text_dim;
    // IDLE / WAITING_DEPS / WAITING_INPUT
    return pal.warn;
}

// The breathing dot of a processing task: the shared working accent, pulsed by
// the window's animation clock (style::breath_accent in edc_style.hpp -- the
// same colour and tempo as the in_progress todo row and the working bar).

// The glyph that stands in for a task's state in the tree: one filled dot,
// tinted with that state's color (StateColor above). The state used to be
// spelled out, which cost each row as many cells as the longest name
// ("LLM_PROCESSING") and made the right edge ragged. A single dot in a fixed
// column keeps every row's right edge still and hands the freed cells to the
// description; the color alone carries the state. A processing task's dot
// breathes through that color instead of sitting static (BreathColor above).
constexpr char kStateDot[] = "●";
constexpr int kStateDotCells = 1; // a single terminal cell

// Cells guaranteed to stay free between a task's description and its state
// dot. Kept as a fixed non-flex element so the dot never glues itself to a
// long (truncated) description.
constexpr int kDescStateGapCells = 2;

// Blank cells reserved at the right edge of every full-width row. The
// sidebar pane is the window's rightmost element: rows used to truncate /
// right-align right up to the inner border, so long text and the state
// labels butted against the terminal edge while the left side kept a visible
// gutter (split separator + border inset). Keeping this much of every text
// row free on the right restores a balanced inset.
constexpr int kRightPad = 2;

// Right-align a count into a fixed-width cell so numeric columns line up
// across rows and live increments (9 → 10 → 21) do not reflow the layout.
// Beyond the width the string simply grows one cell per extra digit (rare)
// and the row's filler absorbs it instead of clipping the number.
std::string PadCount(int value, int width)
{
    std::string s = std::to_string(value);
    if (static_cast<int>(s.size()) >= width)
        return s;
    return std::string(width - static_cast<int>(s.size()), ' ') + s;
}

// Compact token count for the sidebar: 942 -> "942", 12345 -> "12.3k",
// 1_250_000 -> "1.2M". One decimal reads the magnitude at a glance; below
// 100 units a whole number is used so the block does not wobble in width,
// and a trailing ".0" is dropped so round values stay short.
std::string FormatTokens(long long n)
{
    if (n <= 0)
        return "";
    if (n < 1000)
        return std::to_string(n);

    const double v = n < 1000000 ? static_cast<double>(n) / 1000.0 : static_cast<double>(n) / 1000000.0;
    const char *suffix = n < 1000000 ? "k" : "M";

    char buf[32];
    if (v < 100.0)
        std::snprintf(buf, sizeof(buf), "%.1f%s", v, suffix);
    else
        std::snprintf(buf, sizeof(buf), "%.0f%s", v, suffix);

    std::string s(buf);
    const std::string trailing = std::string(".0") + suffix;
    if (s.size() > trailing.size() && s.compare(s.size() - trailing.size(), trailing.size(), trailing) == 0)
        s.erase(s.size() - trailing.size(), 2); // drop the ".0", keep the suffix
    return s;
}

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
    groups_.clear();

    if (!snap)
    {
        group_expanded_.clear();
        return;
    }

    session_id_ = snap->session_id ? snap->session_id : "";
    session_name_ = snap->session_name ? snap->session_name : "";
    created_at_ = snap->created_at ? snap->created_at : "";

    std::string active_group_id;
    for (int gi = 0; gi < snap->group_count; gi++)
    {
        const ::edc_c_task_group *g = &snap->groups[gi];

        GroupInfo group;
        group.group_id = g->group_id ? g->group_id : "";
        group.msg_count = g->msg_count;
        group.orphaned = g->orphaned != 0;
        group.fg = g->fg != 0;
        group.tokens_total = g->tokens_total;

        for (int ti = 0; ti < g->task_count; ti++)
        {
            const ::edc_c_task_info *t = &g->tasks[ti];

            TaskInfo task;
            task.task_id = t->task_id ? t->task_id : "";
            task.state = t->state ? t->state : "";
            task.is_leader = t->is_leader != 0;
            task.is_active = t->is_active != 0;
            task.description = TruncateUtf8(t->description ? t->description : "", kMaxDescCodepoints);
            task.tokens_prompt = t->tokens_prompt;
            task.tokens_completion = t->tokens_completion;
            task.tokens_total = t->tokens_total;
            task.tokens_cached = t->tokens_cached;
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

void TaskWidget::SetBreath(long long elapsed_ms)
{
    breath_ms_ = elapsed_ms;
}

bool TaskWidget::HasProcessingTask() const
{
    for (const auto &g : groups_)
        for (const auto &t : g.tasks)
            if (StateIsProcessing(t.state))
                return true;
    return false;
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

int TaskWidget::RowWidth() const
{
    // Width available for a text row. reflect() sits on the vbox *inside* the
    // EMPTY-style border, which still insets its child by one column per side (the
    // outer box would overestimate by 2 and a wide description would be flex-shrunk,
    // clipping the trailing "…"). Reads the previous frame's width; before the first
    // measurement, the default sidebar width minus those columns.
    if (content_box_.x_min != 0 || content_box_.x_max != 0 || content_box_.y_min != 0 || content_box_.y_max != 0)
        return content_box_.x_max - content_box_.x_min + 1;
    return 34;
}

// The section's heading: the fold arrow, the "Tasks" label and the counts, then
// the session meta row and the rule that closes it off. Drawn apart from the
// group rows so the sidebar can pin all of it above the scrolling tree -- the
// wheel has to move the tree without taking the heading with it.
::ftxui::Element TaskWidget::RenderHeader()
{
    using namespace ::ftxui;

    // All colors come from the central style palette (edc_style.hpp); the
    // pane background must stay identical to TodoWidget and the chat surface.
    const auto &pal = style::palette();
    const int row_width = RowWidth();
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
            text((section_collapsed_ ? "▸ " : "▾ ") + std::string(" Tasks ")) | bold | color(pal.accent),
            filler(),
        };
        if (group_count > 0)
        {
            const std::string gc = PadCount(group_count, 2) + (group_count == 1 ? " group" : " groups");
            const std::string tc = PadCount(task_count, 2) + (task_count == 1 ? " task" : " tasks");
            // Truncate the summary to the room left after the fixed header
            // label and the right pad, instead of letting it clip at the
            // pane edge on a narrow sidebar.
            const int left_cells = 2 /* arrow */ + static_cast<int>(std::string(" Tasks ").size());
            const int summary_budget = row_width - kRightPad - left_cells;
            head.push_back(text(TruncateToWidth(gc + " · " + tc, summary_budget)) | dim | color(pal.text_dim));
        }
        head.push_back(text(std::string(kRightPad, ' ')));
        lines.push_back(hbox(std::move(head)) | reflect(section_header_box_));
    }

    if (section_collapsed_)
    {
        // A collapsed section is its header row alone: the session meta and
        // the rule under it introduce the list, so they fold away with it.
        return vbox(std::move(lines));
    }

    if (!session_name_.empty() || !session_id_.empty())
    {
        // Session meta on one row: the name (with its id in brackets when distinct) on
        // the left, the creation date flushed right into the same edge column as the
        // group rows. The date is never truncated: a narrow sidebar ellipsizes the name
        // instead, dropping the bracketed id first.
        const std::string created = created_at_.size() >= 10 ? "created " + created_at_.substr(0, 10) : "";
        const std::string name = session_name_.empty() ? session_id_ : session_name_;
        // The id is bracketed only when a distinct session *name* exists;
        // with no name the id stands alone and must not repeat itself.
        const bool distinct = !session_name_.empty() && !session_id_.empty() && session_name_ != session_id_;
        const std::string sid = distinct ? name + " [" + session_id_ + "]" : name;

        // Room for the name after the leading gutter, the fixed created
        // block, the reserved right pad and a one-cell spacer that keeps
        // the two sides apart even when the flex label gets no leftover.
        const int created_cells = created.empty() ? 0 : StringWidth(created);
        const int gap_cells = created.empty() ? 0 : 1;
        const int name_budget = row_width - 1 - kRightPad - created_cells - gap_cells;

        std::string left = name;
        if (name_budget > 0 && StringWidth(sid) <= name_budget)
            left = sid; /* full name + id fits: keep the id too */
        else if (name_budget > 0)
            left = TruncateToWidth(name, name_budget);
        else
            left.clear();

        lines.push_back(hbox({
            text(" " + left) | flex | dim,
            text(created.empty() ? "" : " ") | dim | color(pal.text_dim),
            text(created) | dim | color(pal.text_dim),
            text(std::string(kRightPad, ' ')),
        }));
    }
    lines.push_back(separator());

    return vbox(std::move(lines));
}

// The group rows: everything under the heading's rule, and the half the sidebar
// hands to a ScrollView. It carries the body's reflect(content_box_) -- the row
// width both halves truncate against.
::ftxui::Element TaskWidget::RenderBody()
{
    using namespace ::ftxui;

    const auto &pal = style::palette();
    const int row_width = RowWidth();
    Elements lines;

    if (section_collapsed_)
    {
        // A collapsed section keeps only its header row: the session meta and
        // the rule that introduce the list fold away with it (they are part of
        // RenderHeader). No group headers are drawn either, so drop their click
        // boxes to avoid stale hit targets lingering from the expanded frame.
        group_header_boxes_.clear();
    }
    else
    {
        if (groups_.empty())
        {
            lines.push_back(text(" (no tasks)") | dim | color(pal.text_dim));
        }

        group_header_boxes_.resize(groups_.size());

        for (size_t i = 0; i < groups_.size(); i++)
        {
            const auto &g = groups_[i];
            const bool expanded = group_expanded_[g.group_id];

            // Collapsible group header: the arrow plus "Group <id>" ("~" when orphaned)
            // on the left, the group's token cost flushed right. The right block is
            // never truncated; the left ellipsizes into whatever width it leaves.
            std::string core = g.group_id.empty() ? "(group)" : "Group " + g.group_id;
            if (g.orphaned)
                core += " ~";

            const std::string arrow = expanded ? "▾ " : "▸ ";
            // Only the token total is shown now; a group that has not spent
            // anything yet keeps an empty right column (no "0 tok" padding).
            const std::string right = g.tokens_total > 0 ? FormatTokens(g.tokens_total) + " tok" : "";

            const int core_budget = row_width - kRightPad - StringWidth(arrow) - StringWidth(right) - 1;
            const std::string visible_core = core_budget > 0 ? TruncateToWidth(core, core_budget) : "";

            lines.push_back(hbox({
                                text(arrow + visible_core) | flex | bold | color(g.fg ? pal.warn : pal.text_dim),
                                text(right) | dim | color(pal.text_dim),
                                text(std::string(kRightPad, ' ')),
                            }) |
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

                if (!t.description.empty())
                {
                    // Two cells separate the id from the description (kept
                    // from the previous layout), then a gap before the state
                    // dot; kRightPad keeps the dot off the pane's right edge.
                    // Per-task tokens are intentionally not shown: the group
                    // header carries the whole group's cost.
                    const int budget = row_width - StringWidth(left) - 2 - kDescStateGapCells - kStateDotCells - kRightPad;
                    if (budget > 0)
                        left += "  " + TruncateToWidth(t.description, budget);
                }

                Elements cells;
                cells.push_back(text(left) | flex);
                cells.push_back(text(std::string(kDescStateGapCells, ' ')));
                // A processing task breathes (its dot pulses with the working
                // bar's tempo); every other state is a fixed color.
                const ::ftxui::Color dot = StateIsProcessing(t.state) ? style::breath_accent(breath_ms_)
                                                                      : StateColor(t.state);
                cells.push_back(text(kStateDot) | color(dot));
                cells.push_back(text(std::string(kRightPad, ' ')));
                lines.push_back(hbox(std::move(cells)));
            }
        }
    }

    // No flex here: the enclosing right-pane column owns the vertical space
    // and stacks this section above the Todo section in natural order. This
    // widget only ever occupies its own content height (plus its own EMPTY
    // border/background), so the todo list below is never pushed to the pane
    // bottom by a greedy task tree.
    return vbox(std::move(lines)) | reflect(content_box_);
}

// The section as one block: header over body, inside the section's single EMPTY
// border. The sidebar builds the same two halves itself -- pinning the header
// above a ScrollView that owns the body -- so a wheel scrolls only the tree;
// this keeps the widget whole for standalone use and for its tests.
::ftxui::Element TaskWidget::OnRender()
{
    return ::ftxui::vbox({RenderHeader(), RenderBody()}) | ::ftxui::borderStyled(::ftxui::BorderStyle::EMPTY) |
           ::ftxui::bgcolor(style::palette().pane_bg);
}

} // namespace edc
