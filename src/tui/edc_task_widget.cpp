#include "edc_task_widget.hpp"
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
        return ::ftxui::Color::RGB(64, 168, 96);   // green
    if (state == "ERROR")
        return ::ftxui::Color::RGB(224, 108, 117); // red
    if (state == "LLM_PROCESSING" || state == "TOOL_CALLING")
        return ::ftxui::Color::RGB(94, 151, 247);  // blue
    if (state == "SUSPENDED")
        return ::ftxui::Color::RGB(198, 120, 221); // magenta
    if (state == "CANCELLED")
        return ::ftxui::Color::RGB(147, 147, 147); // gray
    // IDLE / WAITING_DEPS / WAITING_INPUT
    return ::ftxui::Color::RGB(229, 192, 123); // yellow
}

// Truncate by UTF-8 code points so a multi-byte character is never cut in
// half (which would otherwise render as a replacement/garbage glyph).
std::string TruncateUtf8(const std::string &s, size_t max_codepoints)
{
    if (s.empty())
        return s;

    size_t byte_pos = 0;
    size_t count = 0;
    while (byte_pos < s.size() && count < max_codepoints)
    {
        const unsigned char c = static_cast<unsigned char>(s[byte_pos]);
        size_t len = 1;
        if (c >= 0xF0)
            len = 4;
        else if (c >= 0xE0)
            len = 3;
        else if (c >= 0xC0)
            len = 2;

        // A truncated or overlong sequence ends the scan at the last whole
        // code point.
        if (byte_pos + len > s.size())
            break;

        byte_pos += len;
        count++;
    }

    if (byte_pos >= s.size())
        return s;

    return s.substr(0, byte_pos) + "…";
}

} // namespace

TaskWidget::TaskWidget()
{
}

void TaskWidget::SetSnapshot(const ::edc_c_task_snapshot *snap)
{
    session_id_.clear();
    session_name_.clear();
    groups_.clear();

    if (!snap)
    {
        group_expanded_.clear();
        return;
    }

    session_id_ = snap->session_id ? snap->session_id : "";
    session_name_ = snap->session_name ? snap->session_name : "";

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
            task.description = TruncateUtf8(t->description ? t->description : "", 24);
            if (task.is_active)
                active_group_id = group.group_id;
            group.tasks.push_back(std::move(task));
        }

        groups_.push_back(std::move(group));
    }

    // Every snapshot refresh resets collapse state: only the group holding the
    // active task stays expanded; all other groups collapse. A user's manual
    // toggle is therefore in effect only until the next snapshot arrives.
    std::map<std::string, bool> next;
    for (const auto &g : groups_)
    {
        next[g.group_id] = (g.group_id == active_group_id);
    }
    group_expanded_ = std::move(next);
}

bool TaskWidget::OnEvent(::ftxui::Event event)
{
    if (!event.is_mouse())
        return false;

    if (event.mouse().button != ::ftxui::Mouse::Left || event.mouse().motion != ::ftxui::Mouse::Pressed)
        return false;

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

    lines.push_back(text(" Tasks ") | bold | color(Color::RGB(94, 151, 247)));

    if (!session_name_.empty() || !session_id_.empty())
    {
        std::string sid = session_name_.empty() ? session_id_ : session_name_;
        if (!session_id_.empty() && session_name_ != session_id_)
            sid += " [" + session_id_ + "]";
        lines.push_back(text(" " + sid) | dim);
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

        // Collapsible group header: an arrow shows the state; foreground is
        // highlighted with a warm color, orphaned groups are marked.
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
            std::string left = "  ";
            left += t.is_active ? "*" : " ";
            left += t.is_leader ? "●" : " ";
            left += " " + t.task_id;
            if (!t.description.empty())
                left += "  " + t.description;

            auto row = hbox({
                text(left) | flex,
                text(t.state) | color(StateColor(t.state)),
            });
            lines.push_back(row);
        }
    }

    return vbox(std::move(lines)) | borderStyled(BorderStyle::EMPTY) | bgcolor(Color::RGB(30, 30, 30)) | flex;
}

} // namespace edc
