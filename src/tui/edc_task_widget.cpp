#include "edc_task_widget.hpp"
#include "edc_style.hpp"
#include "edc_text_util.hpp"
#include "ftxui/component/mouse.hpp"
#include "ftxui/dom/elements.hpp"
#include <cstdio>
#include <memory>
#include <utility>

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

} // namespace

TaskWidget::TaskWidget() : TaskWidget(std::make_shared<model::TaskTreeModel>())
{
}

TaskWidget::TaskWidget(std::shared_ptr<model::TaskTreeModel> tree)
    : tree_(tree ? std::move(tree) : std::make_shared<model::TaskTreeModel>())
{
}

void TaskWidget::SetSnapshot(const TaskSnapshot *snap)
{
    tree_->Apply(snap);
}

void TaskWidget::SetBreath(long long elapsed_ms)
{
    breath_ms_ = elapsed_ms;
}

bool TaskWidget::HasProcessingTask() const
{
    return tree_->HasProcessingTask();
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
        tree_->toggle_section();
        return true;
    }

    for (size_t i = 0; i < group_header_boxes_.size() && i < tree_->groups().size(); i++)
    {
        if (group_header_boxes_[i].Contain(event.mouse().x, event.mouse().y))
        {
            tree_->toggle_group(tree_->groups()[i].group_id);
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
        int group_count = static_cast<int>(tree_->groups().size());
        int task_count = 0;
        for (const auto &g : tree_->groups())
            task_count += static_cast<int>(g.tasks.size());

        Elements head = {
            text((tree_->section_collapsed() ? "▸ " : "▾ ") + std::string(" Tasks ")) | bold | color(pal.accent),
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

    if (tree_->section_collapsed())
    {
        // A collapsed section is its header row alone: the session meta and
        // the rule under it introduce the list, so they fold away with it.
        return vbox(std::move(lines));
    }

    if (!tree_->session_name().empty() || !tree_->session_id().empty())
    {
        // Session meta on one row: the name (with its id in brackets when distinct) on
        // the left, the creation date flushed right into the same edge column as the
        // group rows. The date is never truncated: a narrow sidebar ellipsizes the name
        // instead, dropping the bracketed id first.
        const std::string created = tree_->created_at().size() >= 10 ? "created " + tree_->created_at().substr(0, 10) : "";
        const std::string name = tree_->session_name().empty() ? tree_->session_id() : tree_->session_name();
        // The id is bracketed only when a distinct session *name* exists;
        // with no name the id stands alone and must not repeat itself.
        const bool distinct = !tree_->session_name().empty() && !tree_->session_id().empty() && tree_->session_name() != tree_->session_id();
        const std::string sid = distinct ? name + " [" + tree_->session_id() + "]" : name;

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

    if (tree_->section_collapsed())
    {
        // A collapsed section keeps only its header row: the session meta and
        // the rule that introduce the list fold away with it (they are part of
        // RenderHeader). No group headers are drawn either, so drop their click
        // boxes to avoid stale hit targets lingering from the expanded frame.
        group_header_boxes_.clear();
    }
    else
    {
        if (tree_->groups().empty())
        {
            lines.push_back(text(" (no tasks)") | dim | color(pal.text_dim));
        }

        group_header_boxes_.resize(tree_->groups().size());

        /* 系统组画在最前, 用户分组照旧 (顺序固定, 不靠标题行)。
         *
         * 为什么系统组在前: 侧栏的树是锚底的 (edc_sidebar 用 follow_tail), 视口不
         * 够时该被挤出去的正是它 —— "最后一行永远是用户正在跑的那个 task" 优先
         * 于"看见折叠任务"。它的组头写着 "System" (见下面), 点名靠那一行本身,
         * 所以不再单画一条 ` System` 标题。
         *
         * 这里读 `kind` 是它**唯一的读者** (展示层)。没有任何东西被隐藏: 系统
         * 任务与用户任务一样是快照里的普通成员, 展开就看得见。 */
        std::vector<size_t> order;
        for (size_t i = 0; i < tree_->groups().size(); i++)
            if (tree_->groups()[i].kind == GroupKind::System)
                order.push_back(i);
        for (size_t i = 0; i < tree_->groups().size(); i++)
            if (tree_->groups()[i].kind != GroupKind::System)
                order.push_back(i);

        for (size_t k = 0; k < order.size(); k++)
        {
            const size_t i = order[k];
            const auto &g = tree_->groups()[i];
            const bool expanded = tree_->group_expanded(g.group_id);

            // Collapsible group header: the arrow plus the group's name. A user
            // group shows "Group <id>" (its id *is* its leader's task id); a
            // system group -- the container of the folding tasks, one per
            // session -- shows "System": it has no user-facing id worth
            // reading, and without this it read as one more ordinary group.
            // "~" marks an orphaned group. The left part ellipsizes into
            // whatever width is left.
            // Cost lives in the footer, under the members it is made of, and
            // nowhere else: the header used to repeat the group's total while the
            // group was folded, which read as "this group costs 9.2k" without
            // saying of what. A folded group now shows no number at all -- the
            // row is still the handle that unfolds it.
            std::string core;
            if (g.kind == GroupKind::System)
                core = "System";
            else
                core = g.group_id.empty() ? "(group)" : "Group " + g.group_id;
            if (g.orphaned)
                core += " ~";

            const std::string arrow = expanded ? "▾ " : "▸ ";
            const int core_budget = row_width - kRightPad - StringWidth(arrow) - 1;
            const std::string visible_core = core_budget > 0 ? TruncateToWidth(core, core_budget) : "";

            lines.push_back(hbox({
                                text(arrow + visible_core) | flex | bold | color(g.fg ? pal.warn : pal.text_dim),
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
