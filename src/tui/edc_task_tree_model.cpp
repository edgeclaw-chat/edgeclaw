#include "edc_task_tree_model.hpp"
#include "edc_style.hpp"
#include "edc_text_util.hpp"

#include <utility>

namespace edc::model
{

namespace
{

// Upper bound (in code points) kept per task when importing a snapshot. The exact
// per-row fit is decided at render time by TruncateToWidth(); this cap only stops a
// pathological description from being copied/stored in full.
constexpr size_t kMaxDescCodepoints = 200;

bool StateIsProcessing(const std::string &state)
{
    return style::TaskStateProcessing(state);
}

} // namespace

void TaskTreeModel::Apply(const TaskSnapshot *snap)
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

    // Refresh collapse state: keep user toggles for groups that still exist; a
    // newly-seen group is expanded when it holds the active task. A system group
    // (folding) is *not* special-cased here: it starts folded like any other group
    // that is not active -- it can hold dozens of finished fold tasks, and expanding
    // all of them would squeeze the user's tree out of the bottom-anchored viewport.
    std::map<std::string, bool> next;
    for (const auto &g : groups_)
    {
        const auto it = group_expanded_.find(g.group_id);
        next[g.group_id] = (it != group_expanded_.end()) ? it->second : (g.group_id == active_group_id);
    }
    group_expanded_ = std::move(next);
}

bool TaskTreeModel::HasProcessingTask() const
{
    for (const auto &g : groups_)
        for (const auto &t : g.tasks)
            if (StateIsProcessing(t.state))
                return true;
    return false;
}

} // namespace edc::model
