#pragma once

#include "edc_task_snapshot.hpp"
#include "ftxui/component/component_base.hpp"
#include "ftxui/component/event.hpp"
#include "ftxui/screen/box.hpp"
#include <map>
#include <string>
#include <vector>

namespace edc
{
// Right-sidebar widget rendering the active session's task tree (process groups
// and their member tasks) as a collapsible section: the "▾/▸ Tasks" header folds
// the whole tree and each group header folds its own tasks. Only the group
// holding the active task starts expanded, and user toggles survive snapshot
// refreshes.
class TaskWidget : public ::ftxui::ComponentBase
{
  public:
    TaskWidget();

    // Copy the parsed snapshot into display state. The snapshot stays owned by
    // the caller and is freed after this call.
    void SetSnapshot(const TaskSnapshot *snap);

    // Phase the breathing dot of a task in a processing state. |elapsed_ms| is
    // the window's animation clock (see TUIWindow::TickAnimations), so the dot
    // breathes on the same tempo as the working bar. Static states ignore it.
    void SetBreath(long long elapsed_ms);

    // True while any task in the tree is in a processing state. The window uses
    // it to keep asking for animation frames even when the foreground process
    // itself is idle (a background task still has to breathe).
    bool HasProcessingTask() const;

    // The section in two halves: RenderHeader() is everything above the list (the
    // "Tasks" row, the session meta and the rule), RenderBody() the group rows.
    // The sidebar pins the header above a ScrollView owning the body, so the
    // wheel moves the tree while the heading stays put.
    ::ftxui::Element RenderHeader();
    ::ftxui::Element RenderBody();

    ::ftxui::Element OnRender() override;
    bool OnEvent(::ftxui::Event event) override;

    bool Focusable() const override
    {
        return false;
    }

  private:
    // Row width both halves truncate against, from the box the previous render
    // measured (the default sidebar width before that).
    int RowWidth() const;

    struct TaskInfo
    {
        std::string task_id;
        std::string state;
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
        /* GroupKind: 展示层唯一的依据 —— 系统分组画到侧栏单独一块。 */
        GroupKind kind = GroupKind::User;
        int msg_count = 0; /* live group messages (committed + in-flight) */
        bool orphaned = false;
        bool fg = false;
        /* Usage rolled up from the members (the sidebar shows the group's, and
         * only the group's, cost). */
        long long tokens_total = 0;      /* sum of member tasks' total tokens */
        long long tokens_prompt = 0;     /* sum of member tasks' prompt tokens */
        long long tokens_completion = 0; /* sum of member tasks' completion tokens */
        long long tokens_cached = 0;     /* sum of member tasks' cached prompt tokens */
        long long ctx_tokens = 0;        /* current context (biggest member conversation) */
        bool ctx_estimated = false;      /* ... and whether it carries an estimate */
        std::vector<TaskInfo> tasks;
    };

    std::string session_id_;
    std::string session_name_;
    std::string created_at_; /* "YYYY-MM-DD HH:MM:SS" or "" */
    std::vector<GroupInfo> groups_;

    // Phase of the processing dots' breathing, in ms on the window's animation
    // clock. Only read for tasks in a processing state (see SetBreath).
    long long breath_ms_ = 0;

    // Fold state of the whole Tasks section, preserved across snapshot refreshes
    // so a manual fold is not undone by a background update.
    bool section_collapsed_ = false;

    // Click target of the Tasks header row, populated during OnRender.
    ::ftxui::Box section_header_box_;

    // group_id -> whether its task list is expanded, also preserved across
    // refreshes.
    std::map<std::string, bool> group_expanded_;

    // Per-group header click targets, populated during OnRender.
    std::vector<::ftxui::Box> group_header_boxes_;

    // Content box of this widget's inner vbox, written by reflect() during the
    // previous render and read to truncate descriptions to the real row width
    // (instead of letting flex-shrink clip them flush against the state label).
    // The box is measured inside the border, whose EMPTY style still insets its
    // child by one column per side. All-zero means "not measured yet".
    ::ftxui::Box content_box_;
};
} // namespace edc
