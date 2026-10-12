#pragma once

// ── 控制层: 通道事件分派器 (ChannelDispatcher) ──────────────────────────────
//
// P2 把 C 边界 (分类 + 取字段 + marshalling) 搬进了 AppFacade, 但"收到强类型
// 事件之后怎么判断、更新什么"仍留在 TUIWindow::On{ChatMessage,TaskSnapshot,
// TreeChanged,TaskHistory} 里。P3 把它们移到这里:
//
//   * 分派器只做三件事 —— 用策略**判断** (谁的树能上屏 / 产出该去哪),
//     **更新模型** (ChatModel / SessionModel), 叫 host 去做**视图动作**
//     (转录重放/落盘、侧栏刷新、动画、重绘、拉树);
//   * host 是一个窄接口 (IChannelHost), 由 TUIWindow 实现。分派器因此不认识
//     ftxui、不认识具体窗口, 可以脱离终端单测 (喂事件 → 断言模型)。

#include "edc_app_facade.hpp" /* port::IChannelSink / ChatMessageEvent */
#include "edc_chat_model.hpp"
#include "edc_domain.hpp"
#include "edc_session_model.hpp"
#include "edc_task_snapshot.hpp"

#include <string>

namespace edc::control
{

// 分派器要窗口去做的"视图动作" (模型之外的事)。TUIWindow 实现它。
class IChannelHost
{
  public:
    virtual ~IChannelHost() = default;

    // 只看历史模式里, 实时产出不上屏 (命令回复除外, 见分派器)。
    virtual bool InHistoryView() const = 0;

    // 把一条实时产出落盘并上屏 (显示中的会话) / 只落进它自己会话的转录 (后台会话)。
    // 返回 true = 它出现在当前 tab 上 (调用方据此决定要不要拉树 / 重绘)。
    virtual bool PresentChat(const port::ChatMessageEvent &event, const std::string &kind_str) = 0;

    // 切换显示会话并重放它的转录 (同时更新 SessionModel + ChatModel)。
    virtual void LoadSessionTranscript(const std::string &session_id) = 0;

    // 把一棵树写进侧栏 (TaskWidget)。
    virtual void SetTaskTree(const TaskSnapshot &snap) = 0;
    // 由树决定前台任务是否在跑 (工作动画)。
    virtual void SyncWorking(const TaskSnapshot &snap) = 0;
    // 由树决定前台 active task 的 todo 列表 (TodoWidget)。
    virtual void SetTodos(const TaskSnapshot &snap) = 0;
    // `/task history` 的结构化载荷 → 历史视图。
    virtual void PresentHistory(const std::string &payload) = 0;

    virtual void RequestTaskSnapshot() = 0;
    virtual void RequestFrame() = 0;
};

// 通道事件分派器: AppFacade 的 sink。
class ChannelDispatcher : public port::IChannelSink
{
  public:
    // |claim| / |route| 不拥有, 默认取进程级的无状态实现。
    ChannelDispatcher(model::ChatModel &chat, model::SessionModel &session, IChannelHost &host,
                      const domain::SnapshotClaimPolicy &claim = domain::SnapshotClaimPolicy::Default(),
                      const domain::OutputRoutingPolicy &route = domain::OutputRoutingPolicy::Default());

    void OnChatMessage(const port::ChatMessageEvent &event) override;
    void OnTaskSnapshot(const std::string &json) override;
    void OnTreeChanged(const std::string &session_id, const std::string &reason) override;
    void OnTaskHistory(const std::string &payload) override;

  private:
    model::ChatModel &chat_;
    model::SessionModel &session_;
    IChannelHost &host_;
    const domain::SnapshotClaimPolicy &claim_;
    const domain::OutputRoutingPolicy &route_;
};

} // namespace edc::control
