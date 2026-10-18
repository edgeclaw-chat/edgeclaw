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

/* No further transition will happen on its own (the same set edc_task_state_str
 * can produce for a finished task). */
bool IsTerminalState(const std::string &state)
{
    return state == "COMPLETED" || state == "CANCELLED" || state == "ERROR" || state == "INTERRUPTED" ||
           state == "SUSPENDED";
}

/* The endings worth telling the user about. INTERRUPTED is left out on purpose: it
 * lands on every unfinished task when the app exits, so reporting it would fill the
 * screen at shutdown (that story belongs to the startup notice). */
bool AnnouncesFinish(const std::string &state)
{
    return state == "COMPLETED" || state == "CANCELLED" || state == "ERROR";
}

} // namespace

void TaskTreeModel::Apply(const TaskSnapshot *snap)
{
    /* How every task read BEFORE this snapshot -- the old tree is about to be
     * replaced, and "just finished" is a comparison. Copy the rows we need; a task
     * that disappears from the snapshot simply has no entry. */
    std::map<std::string, TaskInfo> before;
    for (const auto &g : groups_)
        for (const auto &t : g.tasks)
            before[t.task_id] = t;

    finished_.clear();

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
            /* Did it end between the two snapshots? A task seen for the first time
             * never counts -- otherwise opening a session would report its whole
             * history at once. */
            const auto prev = before.find(task.task_id);
            if (prev != before.end())
            {
                task.prev_state = prev->second.state;
                task.unread = prev->second.unread;
                if (!IsTerminalState(prev->second.state) && AnnouncesFinish(task.state) &&
                    group.kind != GroupKind::System && !task.is_active)
                {
                    task.unread = true;
                    finished_.push_back(task.task_id);
                }
            }
            if (task.is_active)
                task.unread = false; /* the chat pane is showing it right now: read */
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

std::vector<std::string> TaskTreeModel::TakeFinished()
{
    std::vector<std::string> out;
    out.swap(finished_);
    return out;
}

bool TaskTreeModel::IsUnread(const std::string &task_id) const
{
    for (const auto &g : groups_)
        for (const auto &t : g.tasks)
            if (t.task_id == task_id)
                return t.unread;
    return false;
}

void TaskTreeModel::MarkRead(const std::string &task_id)
{
    for (auto &g : groups_)
        for (auto &t : g.tasks)
            if (t.task_id == task_id)
                t.unread = false;
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
