#pragma once

// ── 领域规则: 快照认领 + 实时产出路由 (策略对象) ─────────────────────────────
//
// 这两个规则原本是匿名命名空间 / 头文件里的**自由函数**:
//   bool SnapshotIsForSession(const TaskSnapshot*, const std::string& displayed);
//   bool TreeChangedEventWantsPull(const std::string& ev, const std::string& reason,
//                                  const std::string& displayed);
//   LiveOutputRoute RouteLiveOutput(const std::string& session, bool from_active,
//                                   const std::string& displayed);
// 行为正确, 但没有"对象"承接: 调用方只能靠传一串参数表达"谁的树能上屏 / 产出该
// 去哪儿", 规则也无法在测试里被替换。
//
// 现在它们是**策略对象**: 默认实现保持不变 (旧的自由函数保留为薄委托, 既有测试
// 不必改), 需要时可按需注入别的策略 (如"全放行"的测试替身)。

#include "edc_task_snapshot.hpp"

#include <string>

namespace edc::domain
{

// 快照认领: 决定前端接不接一棵树、去不去拉一棵树。
class SnapshotClaimPolicy
{
  public:
    virtual ~SnapshotClaimPolicy() = default;

    // 收到的树是不是我正在显示的那棵 (迟到的应答丢弃; 空显示的引导帧自证身份)。
    virtual bool Claims(const TaskSnapshot &snap, const std::string &displayed_session) const = 0;

    // "session |event_session| changed for |reason|" → 要不要拉?
    virtual bool WantsPull(const std::string &event_session, const std::string &reason,
                           const std::string &displayed_session) const = 0;

    // 无状态的默认实现 (拉取语义: 只显示自己那一棵)。进程级共享。
    static const SnapshotClaimPolicy &Default();
};

// 一条实时产出该去哪儿。
enum class OutputRoute
{
    Shown,        // 进当前视图 + 当前转录 (显示中的 session 的 active task 的产出)
    Silent,       // 什么都不做 (同 session 里非 active task 的输出静默)
    OtherSession, // 写进它自己 session 的转录 (切回去能看到, 但不在当前 tab 冒泡)
};

// 实时产出路由策略。
class OutputRoutingPolicy
{
  public:
    virtual ~OutputRoutingPolicy() = default;

    // 没有 session 标签的产出 (命令回复等, 发送方不是 task) 一律 Shown。
    virtual OutputRoute Route(const std::string &session_id, bool from_active_task,
                              const std::string &displayed_session) const = 0;

    static const OutputRoutingPolicy &Default();
};

} // namespace edc::domain
