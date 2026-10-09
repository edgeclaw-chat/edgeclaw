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

// The state predicates and the state -> colour table live in edc_style.hpp
// (style::TaskStateProcessing / TaskStateFailed / TaskStateColor): the todo
// list mirrors the tree's failure colour on its current step, so both read the
// same one. The names below keep this file's call sites short.
bool StateIsProcessing(const std::string &state)
{
    return style::TaskStateProcessing(state);
}

// The glyphs that stand in for a task's state in the tree. One marker column
// carries two facts at once: the shape says whether this is the *active* task
// (the one the chat surface is talking to) -- a block for that one, a filled
// dot for every other -- and the color says which state it is in (the shared
// style::TaskStateColor table). The state used to be spelled out, which cost
// each row as many cells as the longest name ("LLM_PROCESSING") and made the
// right edge ragged; a single fixed-column glyph keeps every row's right edge
// still and hands the freed cells to the description.
//
// A block rather than an arrow: it is style::kBlockGlyph, the very glyph the
// working bar's lit cells are made of, so the row the session is working on and
// the bar that says it is working read as one signal -- a processing active task
// lights up as a lit block of that bar. It stays distinct from the thin fold
// arrows the group headers wear (▾ / ▸) and from the round ● of every other row.
// A processing task's marker breathes through its colour instead of sitting
// static (style::breath_accent).
constexpr const char *kStateDot = "●";
constexpr const char *kActiveMarker = style::kBlockGlyph;
constexpr int kMarkerCells = 1; // either glyph is a single terminal cell

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

// The group's usage line, drawn dim as the group's footer -- under the members
// it sums up: ↑ input, ↓ output, the share of that input the provider served
// from its prompt cache, and the size of the current context. Every field with
// nothing to say is dropped, so a group that has not spent anything yet draws no
// line at all.
//
// The context is marked with `~` when it carries an estimate: the anchor is the
// provider's own count, but the tail appended after it is ours, and an estimate
// must never pass for a measurement.
//
// The hit rate is derived here (cached / prompt) instead of travelling as a
// field: it is a view of the two counters, not a datum of its own. It is shown
// only when something *was* cached -- a provider that never reports cached
// tokens would otherwise read as a flat 0%, which claims more than we know --
// and it keeps one decimal, because "12.2%" and "12%" are not the same claim.
std::string FormatGroupUsage(long long prompt, long long completion, long long cached, long long ctx,
                             bool ctx_estimated)
{
    std::string s;
    // Appends |part| (separated by |sep|) unless there is nothing to say.
    auto add = [&s](const char *sep, const std::string &part) {
        if (part.empty())
            return;
        if (!s.empty())
            s += sep;
        s += part;
    };
    if (prompt > 0)
        add("", std::string("↑") + FormatTokens(prompt));
    if (completion > 0)
        add(" ", std::string("↓") + FormatTokens(completion));
    if (prompt > 0 && cached > 0)
    {
        char rate[32];
        std::snprintf(rate, sizeof(rate), "%.1f%%",
                      100.0 * static_cast<double>(cached) / static_cast<double>(prompt));
        add(" · ", rate);
    }
    if (ctx > 0)
        add(" · ", std::string("ctx ") + (ctx_estimated ? "~" : "") + FormatTokens(ctx));
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

void TaskWidget::SetSnapshot(const TaskSnapshot *snap)
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

    session_id_ = snap->session_id;
    session_name_ = snap->session_name;
    created_at_ = snap->created_at;

    std::string active_group_id;
    for (const edc::TaskGroup &g : snap->groups)
    {
        GroupInfo group;
        group.group_id = g.group_id;
        group.kind = g.kind;
        group.msg_count = g.msg_count;
        group.orphaned = g.orphaned;
        group.fg = g.fg;
        group.tokens_total = g.tokens_total;
        group.tokens_prompt = g.tokens_prompt;
        group.tokens_completion = g.tokens_completion;
        group.tokens_cached = g.tokens_cached;
        group.ctx_tokens = g.ctx_tokens;
        group.ctx_estimated = g.ctx_estimated;

        for (const edc::TaskInfo &t : g.tasks)
        {
            TaskInfo task;
            task.task_id = t.task_id;
            task.state = t.state;
            task.is_leader = t.is_leader;
            task.is_active = t.is_active;
            task.description = TruncateUtf8(t.description, kMaxDescCodepoints);
            task.tokens_prompt = t.tokens_prompt;
            task.tokens_completion = t.tokens_completion;
            task.tokens_total = t.tokens_total;
            task.tokens_cached = t.tokens_cached;
            if (task.is_active)
                active_group_id = group.group_id;
            group.tasks.push_back(std::move(task));
        }

        groups_.push_back(std::move(group));
    }

    // Refresh collapse state: keep user toggles for groups that still exist;
    // a newly-seen group is expanded when it holds the active task -- and also
    // when it is a *system* group: the whole point of that block is showing what
    // is running, and a system task lives for seconds, so folding it away by
    // default would hide it for its entire life. (A user can still fold it; the
    // toggle is remembered like any other.)
    std::map<std::string, bool> next;
    for (const auto &g : groups_)
    {
        const auto it = group_expanded_.find(g.group_id);
        next[g.group_id] = (it != group_expanded_.end())
                               ? it->second
                               : (g.group_id == active_group_id || g.kind == GroupKind::System);
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

        /* 画两块: **系统分组单独一块** (只有存在时才画), 用户分组照旧。
         *
         * 为什么系统块在前: 侧栏的树是锚底的 (edc_sidebar 用 follow_tail), 视口不
         * 够时该被挤出去的正是系统块 —— "最后一行永远是用户正在跑的那个 task"
         * 优先于"看见系统任务"。系统块只装折叠任务 (少而独立), 它出现/消失都不该
         * 动用户那棵树的锚点。
         *
         * 这里读 `kind` 是它**唯一的读者** (展示层)。没有任何东西被隐藏: 系统
         * 任务与用户任务一样是快照里的普通成员, 只是画在另一块 —— 换句话说,
         * "漏一处就冒出 Group system"那类事故在这个形状下不可能发生。 */
        {
            std::vector<size_t> order;
            for (size_t i = 0; i < groups_.size(); i++)
                if (groups_[i].kind == GroupKind::System)
                    order.push_back(i);
            if (!order.empty())
            {
                lines.push_back(text(" System") | bold | color(pal.text_dim));
                for (size_t i = 0; i < groups_.size(); i++)
                    if (groups_[i].kind != GroupKind::System)
                        order.push_back(i);
            }
            else
            {
                for (size_t i = 0; i < groups_.size(); i++)
                    order.push_back(i);
            }

        for (size_t k = 0; k < order.size(); k++)
        {
            const size_t i = order[k];
            const auto &g = groups_[i];
            const bool expanded = group_expanded_[g.group_id];

            // Collapsible group header: the arrow plus "Group <id>" ("~" when
            // orphaned) on the left, the folded group's token cost flushed right.
            // The right block is never truncated; the left ellipsizes into
            // whatever width it leaves.
            std::string core = g.group_id.empty() ? "(group)" : "Group " + g.group_id;
            if (g.orphaned)
                core += " ~";

            const std::string arrow = expanded ? "▾ " : "▸ ";
            // The total only earns the header's right column while the footer is
            // not there to break it down: expanded, the footer's `↑` and `↓` add
            // up to exactly this number one line below, and printing it twice is
            // not "the cost at a glance", it is the same datum rendered twice.
            // Folded, the footer is gone -- then this is the whole story.
            const std::string right = (!expanded && g.tokens_total > 0) ? FormatTokens(g.tokens_total) + " tok" : "";

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
                // Left part: indent, the single marker glyph and the task id.
                // The description is appended separately so its length can be
                // computed against the real row width. The group used to carry a
                // `•` on its leader's row as well, but a group's id *is* its
                // leader's task id (edc_task_group_new), so the group header
                // above already names it -- the cell went to the marker instead.
                const char *marker = t.is_active ? kActiveMarker : kStateDot;
                const std::string head = " " + t.task_id;
                std::string left = head;

                if (!t.description.empty())
                {
                    // Two cells separate the id from the description; kRightPad
                    // keeps the row off the pane's right edge. Per-task tokens are
                    // intentionally not shown: the group's footer carries the
                    // whole group's cost.
                    const int budget =
                        row_width - (2 + kMarkerCells) - StringWidth(head) - 2 - kRightPad;
                    if (budget > 0)
                        left += "  " + TruncateToWidth(t.description, budget);
                }

                // A processing task breathes (its marker pulses with the working
                // bar's tempo, peaking at the plain accent); every other state is
                // a fixed color. Only the marker is tinted -- the id and the
                // description stay in the terminal's default foreground, so the
                // row does not turn into a block of state colour.
                const ::ftxui::Color tint = StateIsProcessing(t.state) ? style::breath_accent(breath_ms_)
                                                                       : style::TaskStateColor(pal, t.state);
                lines.push_back(hbox({
                                    text("  " + std::string(marker)) | color(tint),
                                    text(left) | flex,
                                    text(std::string(kRightPad, ' ')),
                                }));
            }

            // The group's footer: what it has spent and how wide its context is
            // *now*. It closes the group's block, which is why a folded group
            // draws none -- there the header's right column is the only number
            // worth the row ("how much has this cost"), and the breakdown is
            // what the fold is hiding.
            if (const std::string usage =
                    FormatGroupUsage(g.tokens_prompt, g.tokens_completion, g.tokens_cached, g.ctx_tokens,
                                     g.ctx_estimated);
                !usage.empty())
                lines.push_back(text(TruncateToWidth("  " + usage, row_width - kRightPad)) | dim |
                                color(pal.text_dim));
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
