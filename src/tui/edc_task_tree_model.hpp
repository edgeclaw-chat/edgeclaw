#pragma once

// Model layer: the sidebar task tree (TaskTreeModel).
//
// These states (session meta + parsed groups/tasks + fold state) used to be
// TaskWidget's private fields, rebuilt in place by SetSnapshot(). They now live
// in a model:
//   * TaskTreeModel holds the whole "this tree + who folded what" state and
//     projects the snapshot (DTO) into display GroupInfo/TaskInfo (with
//     description truncation) and fold-state continuity;
//   * TaskWidget degrades to "draw this tree"; a fold click just changes the model.
//
// Fold state belongs to the model, not the view: it must survive snapshot
// refreshes (a background update must not re-expand a group the user folded).

#include "edc_task_snapshot.hpp"

#include <map>
#include <set>
#include <string>
#include <vector>

namespace edc::model
{

class TaskTreeModel
{
  public:
    struct TaskInfo
    {
        std::string task_id;
        std::string state;
        /* How this task read in the PREVIOUS snapshot ("" = never seen). The pair
         * (prev_state, state) is what says "it has just finished". */
        std::string prev_state;
        /* Finished while the user was not looking at it, and nothing has shown it
         * since: drawn as an unread marker in the sidebar. */
        bool unread = false;
        bool is_leader = false;
        bool is_active = false;
        std::string description;
        long long tokens_prompt = 0;
        long long tokens_completion = 0;
        long long tokens_total = 0;
        long long tokens_cached = 0;
    };

    struct GroupInfo
    {
        std::string group_id;
        GroupKind kind = GroupKind::User;
        int msg_count = 0;
        bool orphaned = false;
        bool fg = false;
        long long tokens_total = 0;
        long long tokens_prompt = 0;
        long long tokens_completion = 0;
        long long tokens_cached = 0;
        long long ctx_tokens = 0;
        bool ctx_estimated = false;
        std::vector<TaskInfo> tasks;
    };

    // Replace the current tree with a snapshot (nullptr = clear). A group the user
    // folded keeps its state if it still exists; a new group expands when it is the
    // active group.
    void Apply(const TaskSnapshot *snap);

    const std::string &session_id() const
    {
        return session_id_;
    }
    const std::string &session_name() const
    {
        return session_name_;
    }
    const std::string &created_at() const
    {
        return created_at_;
    }
    const std::vector<GroupInfo> &groups() const
    {
        return groups_;
    }

    bool section_collapsed() const
    {
        return section_collapsed_;
    }
    void toggle_section()
    {
        section_collapsed_ = !section_collapsed_;
    }

    // A group with no record is treated as collapsed (matching the old
    // map::operator[] default).
    bool group_expanded(const std::string &group_id) const
    {
        const auto it = group_expanded_.find(group_id);
        return it != group_expanded_.end() && it->second;
    }
    void toggle_group(const std::string &group_id)
    {
        bool &v = group_expanded_[group_id];
        v = !v;
    }

    // Whether any task in the tree is processing (the window uses it to decide
    // whether to keep requesting animation frames).
    bool HasProcessingTask() const;

    // Tasks that finished in the LAST Apply() and were worth saying out loud
    // (COMPLETED / CANCELLED / ERROR, outside a system group, not the active task).
    // Taking the list clears it, so one finish is reported exactly once.
    std::vector<std::string> TakeFinished();
    bool IsUnread(const std::string &task_id) const;
    // The user has seen this task: drop its unread marker.
    void MarkRead(const std::string &task_id);
    // The same, for "its output was on screen" rather than "it is the active one":
    // those tasks are read, and a finish already recorded for them is dropped too
    // (no marker, nothing announced).
    void DropSeen(const std::set<std::string> &seen);

  private:
    std::string session_id_;
    std::string session_name_;
    std::string created_at_;
    std::vector<GroupInfo> groups_;

    // Filled by Apply(), drained by TakeFinished().
    std::vector<std::string> finished_;

    // Fold state, preserved across snapshot refreshes.
    bool section_collapsed_ = false;
    std::map<std::string, bool> group_expanded_;
};

} // namespace edc::model
