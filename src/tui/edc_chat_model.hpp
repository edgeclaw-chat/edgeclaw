#pragma once

// ── 模型层: 聊天记录 + 过滤视图 (ChatModel) ─────────────────────────────────
//
// 从前这些状态和过滤逻辑全是 ChatWidget 的私有字段 (+ ApplyFilter): 视图与
// 状态/规则糊在一个类里, 既没法脱离 ftxui 单测规则, 也没法让别的角色 (如
// ChannelDispatcher) 只往"记录"里写而不碰渲染。
//
// 现在: ChatModel 持有全部消息、当前过滤 (哪些 task id 可见) 和每个 task 的
// 原始输入, 并计算**过滤后的可见序列**; ChatWidget 退化成"把 visible() 画出来
// 的视图", 通过订阅在变化时增量更新。
//
// 兼容: ChatWidget 仍保留 AddMessage / Clear / SetFilter / SetTaskInputs
// (委托给模型), 既有测试不必改。

#include "edc_message.hpp"

#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace edc::model
{

class ChatModel
{
  public:
    // 变化通知: |appended| 非空 = 追加了一条可见消息 (视图增量追加);
    // |appended| 为空 = 整体重建 (过滤 / 任务输入 / 清空 —— 视图从 visible() 重建)。
    struct Change
    {
        std::shared_ptr<Message> appended;
    };
    using Listener = std::function<void(const Change &)>;

    int Subscribe(Listener listener);
    void Unsubscribe(int id);

    // Append one bubble. Recorded whether or not it passes the filter; the view
    // only learns about it when it is visible.
    void AddMessage(std::shared_ptr<Message> message);

    // Drop every message (the log was re-pointed at another session).
    void Clear();

    // Point the log at a group's / task's history: only bubbles whose TaskId()
    // is in |allowed| are visible. nullptr = no filter. |label| names the
    // selection for the "nothing here" hint.
    void SetFilter(std::shared_ptr<const std::set<std::string>> allowed, std::string label = "");

    // The user input each task was created from (task id -> raw input), as the
    // task snapshot reports it. Under a filter, a task's input is prepended to
    // its history (the input's own bubble belongs to no task, so the filter hides it).
    void SetTaskInputs(std::shared_ptr<const std::map<std::string, std::string>> inputs);

    std::shared_ptr<const std::set<std::string>> filter() const
    {
        return filter_;
    }
    const std::string &filter_label() const
    {
        return filter_label_;
    }
    bool has_filter() const
    {
        return filter_ != nullptr;
    }

    // The drawn sequence: every message passing the filter, with each task's
    // input prepended. This is what a view renders.
    const std::vector<std::shared_ptr<Message>> &visible() const
    {
        return visible_;
    }
    bool empty() const
    {
        return visible_.empty();
    }

  private:
    bool Visible(const Message &message) const;
    // Rebuild visible_ from all_ under the current filter (the old ApplyFilter).
    void Rebuild();
    void Notify(const Change &change);

    std::vector<std::shared_ptr<Message>> all_;
    std::vector<std::shared_ptr<Message>> visible_;
    std::shared_ptr<const std::map<std::string, std::string>> task_inputs_;
    std::set<std::string> input_shown_;
    std::shared_ptr<const std::set<std::string>> filter_;
    std::string filter_label_;

    std::map<int, Listener> listeners_;
    int next_id_ = 0;
};

} // namespace edc::model
