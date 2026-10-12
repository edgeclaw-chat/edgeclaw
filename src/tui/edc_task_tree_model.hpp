#pragma once

// ── 模型层: 侧栏任务树 (TaskTreeModel) ──────────────────────────────────────
//
// 从前这些状态 (会话元信息 + 解析后的分组/任务 + 折叠状态) 全是 TaskWidget 的
// 私有字段, 由它的 SetSnapshot() 就地重建。现在归到一个模型里:
//
//   * TaskTreeModel 持有"这一棵树 + 谁折叠了"的全部状态, 并负责把快照 (DTO)
//     投影成展示用的 GroupInfo/TaskInfo (含描述截断) 与折叠状态的延续;
//   * TaskWidget 退化成"把这棵树画出来"的视图, 折叠点击也只是改模型。
//
// 折叠状态属于模型而不属于视图: 它必须跨快照刷新存活 (后台更新不许把用户手动
// 折起的组重新展开), 这正是模型该管的事。

#include "edc_task_snapshot.hpp"

#include <map>
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

    // 用一棵快照替换当前树 (nullptr = 清空)。用户折叠的组若仍存在则保留其状态,
    // 新出现的组在它是 active 组时展开。
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

    // 未记录的组视作未展开 (与旧版 map::operator[] 的默认值一致)。
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

    // 树里有没有处在 processing 状态的任务 (窗口据此决定要不要继续要动画帧)。
    bool HasProcessingTask() const;

  private:
    std::string session_id_;
    std::string session_name_;
    std::string created_at_;
    std::vector<GroupInfo> groups_;

    // Fold state, preserved across snapshot refreshes.
    bool section_collapsed_ = false;
    std::map<std::string, bool> group_expanded_;
};

} // namespace edc::model
