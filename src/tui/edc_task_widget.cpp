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

// The state predicates/colour table live in edc_style.hpp so this file and the
// todo widget stay in sync.
bool StateIsProcessing(const std::string &state)
{
    return style::TaskStateProcessing(state);
}

// A task's marker in the tree: one shape per meaning. The block is the active task, the
// dot is everything else, the star means "finished while you were not looking". The rest
// answer "why is this one over?" -- the colour already says CANCELLED/ERROR, and these
// shapes keep a stop, a `||` skip and a dependency cascade from looking alike.
// style::kBlockGlyph is the same glyph the working bar is made of.
constexpr const char *kStateDot = "●";
constexpr const char *kActiveMarker = style::kBlockGlyph;
constexpr const char *kInterruptedMarker = "□"; /* the process went down underneath it */
constexpr const char *kStoppedMarker = "×";     /* the user stopped it */
constexpr const char *kSkippedMarker = "–";     /* `||`: the branch is not taken */
constexpr const char *kBlockedMarker = "⊘";     /* a dependency ended without producing */
/* Finished while the user was not looking at it. A glyph of its own rather than a
 * colour change: the colour already says which state the task is in, and two signals
 * in one cell would overwrite each other. */
constexpr const char *kUnreadMarker = "*";
constexpr int kMarkerCells = 1; // any of the markers is a single terminal cell

/* The shape for a task that has ENDED, by WHY (nullptr = no reason-specific shape). An
 * older producer sends no `end`: fall back to the one state that already had a shape of
 * its own, so a mixed-version frontend does not lose it. */
const char *EndMarker(const std::string &end, const std::string &state)
{
    if (end == "INTERRUPTED" || (end.empty() && state == "INTERRUPTED"))
        return kInterruptedMarker;
    if (end == "STOPPED")
        return kStoppedMarker;
    if (end == "SKIPPED")
        return kSkippedMarker;
    if (end == "BLOCKED")
        return kBlockedMarker;
    return nullptr;
}

// Blank cells kept at the right edge of every full-width row: the sidebar is the
// window's rightmost element, so this restores the inset the left side has.
constexpr int kRightPad = 2;

// Right-align a count into a fixed-width cell so numeric columns line up across
// rows and live increments (9 -> 10 -> 21) do not reflow the layout.
std::string PadCount(int value, int width)
{
    std::string s = std::to_string(value);
    if (static_cast<int>(s.size()) >= width)
        return s;
    return std::string(width - static_cast<int>(s.size()), ' ') + s;
}

// Compact token count: 942 -> "942", 12345 -> "12.3k", 1250000 -> "1.2M".
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

// The group's usage line: ↑ input, ↓ output, cache hit rate, current context.
// Fields with nothing to say are dropped. The context is marked "~" when it
// carries an estimate (the provider's own count is the anchor, the tail is ours).
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

std::vector<std::string> TaskWidget::TakeFinished()
{
    return tree_->TakeFinished();
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

    // The header row is full width, so a click anywhere on it toggles the section.
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
    // reflect() sits on the vbox *inside* the EMPTY border, which still insets
    // by one column per side. Reads the previous frame's width; before the first
    // measurement, the default sidebar width minus those columns.
    if (content_box_.x_min != 0 || content_box_.x_max != 0 || content_box_.y_min != 0 || content_box_.y_max != 0)
        return content_box_.x_max - content_box_.x_min + 1;
    return 34;
}

// The section heading (fold arrow, "Tasks" label, counts, session meta, rule),
// drawn apart from the group rows so the sidebar can pin it above the scrolling
// tree.
::ftxui::Element TaskWidget::RenderHeader()
{
    using namespace ::ftxui;

    const auto &pal = style::palette();
    const int row_width = RowWidth();
    Elements lines;

    // Collapsible header: the row is full width (reflect records its box), so
    // clicking anywhere on it toggles the whole tree (see OnEvent).
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
            // Truncate the summary to the room left after the label and right pad.
            const int left_cells = 2 /* arrow */ + static_cast<int>(std::string(" Tasks ").size());
            const int summary_budget = row_width - kRightPad - left_cells;
            head.push_back(text(TruncateToWidth(gc + " · " + tc, summary_budget)) | dim | color(pal.text_dim));
        }
        head.push_back(text(std::string(kRightPad, ' ')));
        lines.push_back(hbox(std::move(head)) | reflect(section_header_box_));
    }

    if (tree_->section_collapsed())
    {
        // A collapsed section is its header row alone.
        return vbox(std::move(lines));
    }

    if (!tree_->session_name().empty() || !tree_->session_id().empty())
    {
        // Session meta on one row: the name (id bracketed when distinct) on the
        // left, the creation date flushed right. The date is never truncated;
        // a narrow sidebar ellipsizes the name instead.
        const std::string created = tree_->created_at().size() >= 10 ? "created " + tree_->created_at().substr(0, 10) : "";
        const std::string name = tree_->session_name().empty() ? tree_->session_id() : tree_->session_name();
        // The id is bracketed only when a distinct name exists; with no name the
        // id stands alone and must not repeat itself.
        const bool distinct = !tree_->session_name().empty() && !tree_->session_id().empty() && tree_->session_name() != tree_->session_id();
        const std::string sid = distinct ? name + " [" + tree_->session_id() + "]" : name;

        // Room for the name after the leading gutter, the created block, the
        // right pad and a one-cell spacer.
        const int created_cells = created.empty() ? 0 : StringWidth(created);
        const int gap_cells = created.empty() ? 0 : 1;
        const int name_budget = row_width - 1 - kRightPad - created_cells - gap_cells;

        std::string left = name;
        if (name_budget > 0 && StringWidth(sid) <= name_budget)
            left = sid; /* full name + id fits */
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

// The group rows (everything under the heading's rule); the half the sidebar
// gives to a ScrollView. Carries reflect(content_box_), the row width both
// halves truncate against.
::ftxui::Element TaskWidget::RenderBody()
{
    using namespace ::ftxui;

    const auto &pal = style::palette();
    const int row_width = RowWidth();
    Elements lines;

    if (tree_->section_collapsed())
    {
        // Collapsed: no group headers are drawn, so drop their click boxes to
        // avoid stale hit targets from the expanded frame.
        group_header_boxes_.clear();
    }
    else
    {
        if (tree_->groups().empty())
        {
            lines.push_back(text(" (no tasks)") | dim | color(pal.text_dim));
        }

        group_header_boxes_.resize(tree_->groups().size());

        // The system group is drawn first so it is squeezed out first when the
        // viewport is short (the sidebar tree is anchored to the tail), keeping
        // "the last row is the running task" ahead of "seeing folded tasks".
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

            // A user group is "Group <id>" (its id is its leader's task id); a
            // system group shows "System". "~" marks an orphaned group. Cost is
            // shown only in the footer, never on a folded group's header.
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

            // Two weights only: the foreground group is yellow, everything else is dim. A
            // system/fold group (the agent's own housekeeping) was tried with a hue of its own
            // and it sat in the tree like a foreign object -- the word "System", its place at
            // the top and its folded-by-default state already say it.
            lines.push_back(hbox({
                                text(arrow + visible_core) | flex | bold | color(g.fg ? pal.warn : pal.text_dim),
                                text(std::string(kRightPad, ' ')),
                            }) |
                            reflect(group_header_boxes_[i]));

            if (!expanded)
                continue;

            for (const auto &t : g.tasks)
            {
                // Indent, the marker glyph and the task id; the description is
                // appended separately so its length can be measured against the
                // row width.
                const char *marker = EndMarker(t.end, t.state);
                if (!marker)
                    marker = t.is_active ? kActiveMarker : (t.unread ? kUnreadMarker : kStateDot);
                const std::string head = " " + t.task_id;
                std::string left = head;

                if (!t.description.empty())
                {
                    // Two cells separate the id from the description; kRightPad
                    // keeps the row off the pane's right edge.
                    const int budget =
                        row_width - (2 + kMarkerCells) - StringWidth(head) - 2 - kRightPad;
                    if (budget > 0)
                        left += "  " + TruncateToWidth(t.description, budget);
                }

                // Only the marker is tinted; a processing task breathes.
                const ::ftxui::Color tint = StateIsProcessing(t.state) ? style::breath_accent(breath_ms_)
                                                                       : style::TaskStateColor(pal, t.state);
                lines.push_back(hbox({
                                    text("  " + std::string(marker)) | color(tint),
                                    text(left) | flex,
                                    text(std::string(kRightPad, ' ')),
                                }));
            }

            // The group's footer: what it has spent and how wide its context is now.
            if (const std::string usage =
                    FormatGroupUsage(g.tokens_prompt, g.tokens_completion, g.tokens_cached, g.ctx_tokens,
                                     g.ctx_estimated);
                !usage.empty())
                lines.push_back(text(TruncateToWidth("  " + usage, row_width - kRightPad)) | dim |
                                color(pal.text_dim));
        }
    }

    // No flex here: the enclosing column owns the vertical space, so this
    // section only occupies its own content height and never pushes the todo
    // list to the pane bottom.
    return vbox(std::move(lines)) | reflect(content_box_);
}

// The section as one block: header over body, inside the section's EMPTY border.
// The sidebar builds the same two halves itself so a wheel scrolls only the tree.
::ftxui::Element TaskWidget::OnRender()
{
    return ::ftxui::vbox({RenderHeader(), RenderBody()}) | ::ftxui::borderStyled(::ftxui::BorderStyle::EMPTY) |
           ::ftxui::bgcolor(style::palette().pane_bg);
}

} // namespace edc
