// P1 验证: 快照认领 / 实时产出路由已从自由函数提成**策略对象**。
//
// 这里钉三件事:
//   1. 默认策略的语义与原自由函数逐条一致;
//   2. 旧入口 (edc::SnapshotIsForSession 等) 只是薄委托, 与策略结果一致;
//   3. 策略是可替换的 —— 测试能塞进一个"全放行"的假策略并按它断言
//      (这正是不再依赖真 app / 真 TUI 的可测性来源)。
//
// 不需要窗口、不需要 app、不需要屏幕: 只构造对象。

#include "edc_domain.hpp"
#include "edc_tui_window.hpp" // 旧入口

#include <iostream>
#include <string>

namespace
{

int g_failures = 0;

void Check(bool ok, const std::string &what)
{
    if (ok)
    {
        std::cout << "  ok   " << what << "\n";
        return;
    }
    std::cout << "  FAIL " << what << "\n";
    g_failures++;
}

edc::TaskSnapshot MakeSnap(const char *session_id)
{
    edc::TaskSnapshot snap;
    snap.session_id = session_id;
    return snap;
}

// 一个"全放行"的假认领策略: 默认策略会拒的输入, 它一律接受。用来证明抽象
// 是**可替换的**, 而不只是把函数换了个写法。
class AllowAllPolicy : public edc::domain::SnapshotClaimPolicy
{
  public:
    bool Claims(const edc::TaskSnapshot &, const std::string &) const override
    {
        return true;
    }
    bool WantsPull(const std::string &, const std::string &, const std::string &) const override
    {
        return true;
    }
};

void TestDefaultClaimPolicy()
{
    std::cout << "SnapshotClaimPolicy::Default\n";
    const auto &p = edc::domain::SnapshotClaimPolicy::Default();

    Check(p.Claims(MakeSnap("a"), "a"), "政策: 显示中的那棵 → 收");
    Check(!p.Claims(MakeSnap("b"), "a"), "政策: 别人的那棵 → 弃");
    Check(!p.Claims(MakeSnap(""), "a"), "政策: 没标 session 的树 → 弃");
    Check(p.Claims(MakeSnap("a"), ""), "政策: 尚未采纳时引导帧 → 收");

    Check(p.WantsPull("a", "state", "a"), "政策: 我的树变了 → 拉");
    Check(!p.WantsPull("b", "state", "a"), "政策: 后台 session 变了 → 不拉");
    Check(p.WantsPull("b", "active", "a"), "政策: 切台事件 → 拉");
    Check(!p.WantsPull("", "state", "a"), "政策: 没标 session 的事件 → 不拉");
}

void TestDefaultRoutingPolicy()
{
    std::cout << "OutputRoutingPolicy::Default\n";
    const auto &p = edc::domain::OutputRoutingPolicy::Default();

    Check(p.Route("a", true, "a") == edc::domain::OutputRoute::Shown, "路由: 显示中 active task → 上屏");
    Check(p.Route("a", false, "a") == edc::domain::OutputRoute::Silent, "路由: 同 session 非 active → 静默");
    Check(p.Route("b", true, "a") == edc::domain::OutputRoute::OtherSession, "路由: 别的 session → 进它自己的转录");
    Check(p.Route("", false, "a") == edc::domain::OutputRoute::Shown, "路由: 无 session 标签 (命令回复) → 上屏");
}

void TestLegacyDelegation()
{
    std::cout << "旧入口 == 策略结果 (薄委托)\n";
    const auto &claim = edc::domain::SnapshotClaimPolicy::Default();
    const edc::TaskSnapshot mine = MakeSnap("sess-a");

    for (const char *displayed : {"sess-a", "sess-b", ""})
    {
        const std::string d = displayed;
        Check(::edc::SnapshotIsForSession(&mine, d) == claim.Claims(mine, d),
              std::string("SnapshotIsForSession 委托一致 (displayed='") + d + "')");
        Check(::edc::TreeChangedEventWantsPull("sess-b", "state", d) == claim.WantsPull("sess-b", "state", d),
              std::string("TreeChangedEventWantsPull 委托一致 (displayed='") + d + "')");
    }

    const auto &route = edc::domain::OutputRoutingPolicy::Default();
    Check(::edc::RouteLiveOutput("a", true, "a") == edc::LiveOutputRoute::kShown &&
              route.Route("a", true, "a") == edc::domain::OutputRoute::Shown,
          "RouteLiveOutput kShown 与策略一致");
    Check(::edc::RouteLiveOutput("a", false, "a") == edc::LiveOutputRoute::kSilent &&
              route.Route("a", false, "a") == edc::domain::OutputRoute::Silent,
          "RouteLiveOutput kSilent 与策略一致");
    Check(::edc::RouteLiveOutput("b", true, "a") == edc::LiveOutputRoute::kOtherSession &&
              route.Route("b", true, "a") == edc::domain::OutputRoute::OtherSession,
          "RouteLiveOutput kOtherSession 与策略一致");
}

void TestPolicyIsSubstitutable()
{
    std::cout << "策略可替换\n";
    const AllowAllPolicy allow;
    // 默认策略会拒的两条, 假策略接受 —— 证明行为确实由对象决定。
    Check(!edc::domain::SnapshotClaimPolicy::Default().Claims(MakeSnap("b"), "a"),
          "默认策略: 别人的树 → 弃 (对照组)");
    Check(allow.Claims(MakeSnap("b"), "a"), "替换策略: 同样的输入 → 收");
    Check(allow.WantsPull("", "state", "a"), "替换策略: 没标 session 也拉");
}

} // namespace

int main()
{
    TestDefaultClaimPolicy();
    TestDefaultRoutingPolicy();
    TestLegacyDelegation();
    TestPolicyIsSubstitutable();

    if (g_failures == 0)
    {
        std::cout << "\nall passed\n";
        return 0;
    }
    std::cout << "\n" << g_failures << " FAILED\n";
    return 1;
}
