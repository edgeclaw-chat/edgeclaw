#pragma once

#include "edc_c_bridge.h"
#include "ftxui/component/component_base.hpp"
#include "ftxui/component/event.hpp"
#include "ftxui/screen/box.hpp"
#include <map>
#include <string>
#include <vector>

namespace edc
{
// Right-sidebar widget that renders the active session's task tree (process
// groups and their member tasks). It forms a collapsible sidebar section on
// its own ("▾/▸ Tasks" header toggles the whole tree) and the groups inside
// are individually collapsible too: clicking a group header toggles its task
// list. Only the group containing the active task is expanded by default;
// user toggles are remembered across snapshot refreshes.
class TaskWidget : public ::ftxui::ComponentBase
{
  public:
    TaskWidget();

    // Copy the parsed snapshot into display state. The snapshot is owned by
    // the caller and freed after this call.
    void SetSnapshot(const ::edc_c_task_snapshot *snap);

    ::ftxui::Element OnRender() override;
    bool OnEvent(::ftxui::Event event) override;

    bool Focusable() const override
    {
        return false;
    }

  private:
    struct TaskInfo
    {
        std::string task_id;
        std::string state;
        bool is_leader = false;
        bool is_active = false;
        std::string description;
    };

    struct GroupInfo
    {
        std::string group_id;
        int msg_count = 0; /* live group messages (committed + in-flight) */
        bool orphaned = false;
        bool fg = false;
        std::vector<TaskInfo> tasks;
    };

    std::string session_id_;
    std::string session_name_;
    std::string created_at_;  /* "YYYY-MM-DD HH:MM:SS" or "" */
    int message_count_ = 0;
    std::vector<GroupInfo> groups_;

    // Collapse state for the whole Tasks section; preserved across snapshot
    // refreshes so a user's manual fold is not undone by background updates.
    bool section_collapsed_ = false;

    // Click target of the Tasks section header row, populated during OnRender.
    ::ftxui::Box section_header_box_;

    // group_id → whether its task list is expanded. Persisted across snapshot
    // refreshes so a user's manual toggles survive background updates.
    std::map<std::string, bool> group_expanded_;

    // Per-group header click targets, populated during OnRender.
    std::vector<::ftxui::Box> group_header_boxes_;

    // Content box of this widget's inner vbox (rows), written by reflect()
    // during the previous render pass. OnRender reads it to truncate task
    // descriptions to the real row width (instead of letting flex-shrink
    // hard-clip them flush against the state label). Note the box is measured
    // inside the border, whose EMPTY style still insets its child by one
    // column per side. All-zero means "not measured yet".
    ::ftxui::Box content_box_;
};
} // namespace edc
