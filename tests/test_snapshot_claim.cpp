// P3 ①b 的前端那一半: 快照的**认领规则**。
//
// 背景: 老做法是内核把"绑定 session"的整棵树推给前端, 前端收到就套用 —— 那次
// 推送顺带决定了**用户在看哪个 session**(快照里带 session_id, 不匹配就把用户的
// 对话记录换掉)。新做法把两件事分开:
//   - 内核只发"session X 的树变了"的**事件**(极小, 不带树);
//   - 树只在前端点名要的时候才序列化回来, 前端**只拉自己正在显示的那一棵**。
//
// 于是"别人的树根本不序列化、不上路、不到达"。这里钉住前端剩余的两个判断:
//   1. 收到的树是不是我要的(迟到的应答丢弃);
//   2. 收到事件要不要拉(别人的 session 的变化不关我的事)。
// 两条都是纯函数, 所以可以直接断言 —— 不需要起一个真 TUI。

#include "edc_tui_window.hpp"
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

::edc_c_task_snapshot MakeSnap(const char *session_id)
{
    ::edc_c_task_snapshot snap{};
    snap.session_id = session_id;
    return snap;
}

void TestSnapshotClaim()
{
    std::cout << "SnapshotIsForSession\n";

    const ::edc_c_task_snapshot mine = MakeSnap("sess-a");
    const ::edc_c_task_snapshot theirs = MakeSnap("sess-b");
    const ::edc_c_task_snapshot unlabelled = MakeSnap("");

    Check(::edc::SnapshotIsForSession(&mine, "sess-a"), "我要的那棵 → 收下");
    Check(!::edc::SnapshotIsForSession(&theirs, "sess-a"), "别人的那棵 → 丢弃");
    Check(!::edc::SnapshotIsForSession(&unlabelled, "sess-a"), "没标 session 的树 → 丢弃 (没人认领)");
    Check(!::edc::SnapshotIsForSession(nullptr, "sess-a"), "空快照 → 丢弃");

    // 换台后才到的应答: 显示的是 B 了, A 的树在途 ⇒ 丢弃 (不把用户拉回 A)。
    Check(!::edc::SnapshotIsForSession(&mine, "sess-b"), "迟到的 A 的应答 (现在显示 B) → 丢弃");
    Check(::edc::SnapshotIsForSession(&theirs, "sess-b"), "紧接着 B 的应答 → 收下");
}

void TestTreeChangedEvent()
{
    std::cout << "TreeChangedEventWantsPull\n";

    // 正常变化: 只有"我正在显示的那棵"值得拉。
    Check(::edc::TreeChangedEventWantsPull("sess-a", "state", "sess-a"), "我的树变了 → 去拉");
    Check(!::edc::TreeChangedEventWantsPull("sess-b", "state", "sess-a"), "后台 session 变了 → 不拉");
    Check(!::edc::TreeChangedEventWantsPull("sess-b", "reap", "sess-a"), "后台的回收 → 不拉");
    Check(!::edc::TreeChangedEventWantsPull("sess-b", "todo", "sess-a"), "后台的 todo → 不拉");

    // 用户切台: 那是给我换显示的事件, 必须拉 (调用方已先把显示换过去)。
    Check(::edc::TreeChangedEventWantsPull("sess-b", "active", "sess-a"), "切台事件 → 拉 (换显示后)");
    Check(::edc::TreeChangedEventWantsPull("sess-a", "active", "sess-a"), "切到自己 → 也拉 (刷新)");

    // 没有 session 标签的事件不指名道姓 ⇒ 不拉。
    Check(!::edc::TreeChangedEventWantsPull("", "state", "sess-a"), "没标 session 的事件 → 不拉");
}

void TestLiveOutputRoute()
{
    std::cout << "RouteLiveOutput\n";

    // 显示中的 session, 它自己的 active task → 上屏。
    Check(::edc::RouteLiveOutput("sess-a", true, "sess-a") == ::edc::LiveOutputRoute::kShown,
          "显示中的 active task 的产出 → 上屏");
    // 同 session 里非 active task 的输出 → 静默 (以前这条判断在 core: "只放行 active task")。
    Check(::edc::RouteLiveOutput("sess-a", false, "sess-a") == ::edc::LiveOutputRoute::kSilent,
          "同 session 的非 active task → 静默 (不上屏、不进转录)");
    // 别的 session 的产出 (前台或后台 task 都一样) → 进它自己的转录。
    Check(::edc::RouteLiveOutput("sess-b", true, "sess-a") == ::edc::LiveOutputRoute::kOtherSession,
          "别的 session (active task) → 进它自己的转录");
    Check(::edc::RouteLiveOutput("sess-b", false, "sess-a") == ::edc::LiveOutputRoute::kOtherSession,
          "别的 session (非 active task) → 也进它自己的转录 (今天就是照发)");
    // 没有 session 标签 = 不是 task 的产出 (命令回复/错误) → 直接上屏。
    Check(::edc::RouteLiveOutput("", false, "sess-a") == ::edc::LiveOutputRoute::kShown,
          "没有 session 标签的产出 (命令回复) → 上屏");
    Check(::edc::RouteLiveOutput("", false, "") == ::edc::LiveOutputRoute::kShown,
          "还没有 session 时 (启动早期) 也上屏");
}

} // namespace

int main()
{
    TestSnapshotClaim();
    TestTreeChangedEvent();
    TestLiveOutputRoute();

    if (g_failures == 0)
    {
        std::cout << "\nall passed\n";
        return 0;
    }
    std::cout << "\n" << g_failures << " FAILED\n";
    return 1;
}
