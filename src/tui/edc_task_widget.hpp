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
// groups and their member tasks). Groups are collapsible: clicking a group
// header toggles its task list. On every snapshot refresh the widget resets
// the collapse state so that only the group containing the active task is
// expanded and all other groups are folded.
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
        int member_count = 0;
        bool orphaned = false;
        bool fg = false;
        std::vector<TaskInfo> tasks;
    };

    std::string session_id_;
    std::string session_name_;
    std::vector<GroupInfo> groups_;

    // group_id → whether its task list is expanded. Reset on every snapshot
    // refresh so only the active task's group remains expanded; a manual
    // toggle holds only until the next snapshot.
    std::map<std::string, bool> group_expanded_;

    // Per-group header click targets, populated during OnRender.
    std::vector<::ftxui::Box> group_header_boxes_;
};
} // namespace edc
