// ChannelDispatcher 的单测: 不需要真 app、不需要屏幕、不需要真 ftxui 事件循环。
//
// 这正是 P3 抽模型的收益: 分派器只依赖 ChatModel / SessionModel / 策略 / 一个
// 窄接口 IChannelHost。这里用假 host 记录"视图动作", 喂强类型事件, 断言模型与
// 动作调用 —— 判断逻辑 (谁的树能上屏、产出该去哪儿) 因此可以脱离终端钉住。

#include "edc_channel_dispatcher.hpp"
#include "edc_chat_model.hpp"
#include "edc_session_model.hpp"

#include <iostream>
#include <string>
#include <vector>

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

// Fake host: records the view actions, and mirrors the real window's one piece of
// state the dispatcher reads back — the displayed session (SessionModel).
class FakeHost : public edc::control::IChannelHost
{
  public:
    explicit FakeHost(edc::model::SessionModel &session) : session_(session)
    {
    }

    bool InHistoryView() const override
    {
        return in_history;
    }
    bool PresentChat(const edc::port::ChatMessageEvent &event, const std::string &kind_str) override
    {
        presented.push_back(kind_str);
        // Background session: not on the current tab (mirrors the window).
        if (!event.session_id.empty() && event.session_id != session_.displayed_id())
        {
            background_presented++;
            return false;
        }
        return true;
    }
    void LoadSessionTranscript(const std::string &session_id) override
    {
        loads.push_back(session_id);
        session_.Reset(session_id, /*log_path=*/"");
    }
    void SetTaskTree(const edc::TaskSnapshot &) override
    {
        tree_sets++;
    }
    void SyncWorking(const edc::TaskSnapshot &) override
    {
        syncs++;
    }
    void SetTodos(const edc::TaskSnapshot &) override
    {
        todos++;
    }
    void PresentHistory(const std::string &payload) override
    {
        history.push_back(payload);
    }
    void RequestTaskSnapshot() override
    {
        pulls++;
    }
    void RequestFrame() override
    {
        frames++;
    }

    bool in_history = false;
    std::vector<std::string> presented;
    std::vector<std::string> loads;
    std::vector<std::string> history;
    int background_presented = 0;
    int tree_sets = 0;
    int syncs = 0;
    int todos = 0;
    int pulls = 0;
    int frames = 0;

  private:
    edc::model::SessionModel &session_;
};

edc::port::ChatMessageEvent Chat(edc::port::ChatKind kind, const char *session, bool from_active,
                                 const char *header = nullptr)
{
    edc::port::ChatMessageEvent e;
    e.kind = kind;
    e.content = "text";
    e.session_id = session;
    e.from_active_task = from_active;
    e.header = header ? header : "";
    e.render_pref = 0;
    e.ts = 0;
    return e;
}

std::string SnapJson(const char *session_id, const char *task_desc)
{
    std::string json = R"({"session_id":")";
    json += session_id;
    json += R"(","session_name":"s","workdir":"/tmp/wd","message_count":1,"created_at":"2026-09-16 05:00:00",)"
            R"("groups":[{"id":"1","kind":"user","member_count":1,"msg_count":1,"orphaned":false,"fg":true,)"
            R"("tasks":[{"id":"7","state":"IDLE","leader":true,"active":true,"input":"the input","desc":")";
    json += task_desc;
    json += R"("}]}]})";
    return json;
}

void TestRouting()
{
    std::cout << "OnChatMessage 路由\n";
    edc::model::ChatModel chat;
    edc::model::SessionModel session;
    FakeHost host(session);
    edc::control::ChannelDispatcher d(chat, session, host);
    session.Reset("sess-a", "");

    // 显示中的 session 的 active task 产出 → 上屏 + 拉树 + 重绘。
    d.OnChatMessage(Chat(edc::port::ChatKind::Reply, "sess-a", true));
    Check(host.presented.size() == 1 && host.presented.back() == "reply", "active 产出 → 上屏 (kind=reply)");
    Check(host.pulls == 1 && host.frames == 1, "上屏后拉树 + 重绘各一次");

    // 同 session 非 active task → 静默。
    d.OnChatMessage(Chat(edc::port::ChatKind::Reply, "sess-a", false));
    Check(host.presented.size() == 1, "同 session 非 active → 不上屏");

    // 后台 session → 进它自己的转录, 不在当前 tab。
    d.OnChatMessage(Chat(edc::port::ChatKind::Thought, "sess-b", true));
    Check(host.presented.size() == 2 && host.background_presented == 1, "别的 session → PresentChat 返回后台");
    Check(host.pulls == 1 && host.frames == 1, "后台产出不触发拉树/重绘");

    // todowrite 的 TOOL → 不进对话区。
    d.OnChatMessage(Chat(edc::port::ChatKind::Tool, "sess-a", true, "todowrite"));
    Check(host.presented.size() == 2, "todowrite 的 TOOL → 不上屏");
    // 普通 tool → 上屏。
    d.OnChatMessage(Chat(edc::port::ChatKind::Tool, "sess-a", true, "exec_shell"));
    Check(host.presented.size() == 3 && host.presented.back() == "tool", "普通 tool → 上屏 (kind=tool)");
}

void TestHistoryViewGate()
{
    std::cout << "只看历史模式的门\n";
    edc::model::ChatModel chat;
    edc::model::SessionModel session;
    FakeHost host(session);
    edc::control::ChannelDispatcher d(chat, session, host);
    session.Reset("s", "");
    host.in_history = true;

    d.OnChatMessage(Chat(edc::port::ChatKind::Reply, "s", true));
    Check(host.presented.empty(), "只看模式: 实时产出不上屏");
    // 命令回复没有 session 标签 (发送方不是 task) → 路由为 Shown, 在只看模式也上屏。
    d.OnChatMessage(Chat(edc::port::ChatKind::System, "", false));
    Check(host.presented.size() == 1 && host.presented.back() == "sys", "只看模式: 命令回复 (sys) 例外上屏");
}

void TestSnapshot()
{
    std::cout << "OnTaskSnapshot 认领 + 更新模型\n";
    edc::model::ChatModel chat;
    edc::model::SessionModel session;
    FakeHost host(session);
    edc::control::ChannelDispatcher d(chat, session, host);

    // 未采纳: 第一帧快照自证身份并采纳 (LoadSessionTranscript → 假 host 也把
    // SessionModel 立起来, 与真窗口一致)。
    d.OnTaskSnapshot(SnapJson("sess-first", "TASK_A"));
    Check(session.displayed_id() == "sess-first", "第一帧快照被采纳为显示会话");
    Check(session.workdir() == "/tmp/wd", "workdir 写进 SessionModel");
    Check(host.tree_sets == 1 && host.syncs == 1 && host.todos == 1, "侧栏/动画/todo 各刷新一次");
    Check(host.frames == 1, "快照后请求一帧");

    // 别人的树 → 丢弃。
    d.OnTaskSnapshot(SnapJson("sess-other", "TASK_B"));
    Check(host.tree_sets == 1, "别人的树 → 丢弃 (不刷新)");

    // 自己的树 → 收下。
    d.OnTaskSnapshot(SnapJson("sess-first", "TASK_C"));
    Check(host.tree_sets == 2, "自己的树 → 收下");
}

void TestTreeChanged()
{
    std::cout << "OnTreeChanged 决定拉不拉\n";
    edc::model::ChatModel chat;
    edc::model::SessionModel session;
    FakeHost host(session);
    edc::control::ChannelDispatcher d(chat, session, host);
    session.Reset("sess-a", "");

    d.OnTreeChanged("sess-b", "state");
    Check(host.pulls == 0 && host.loads.empty(), "后台 session 变化 → 不拉不换显示");

    d.OnTreeChanged("sess-a", "state");
    Check(host.pulls == 1, "显示中的树变了 → 拉");

    d.OnTreeChanged("sess-b", "active");
    Check(host.loads.size() == 1 && host.loads.back() == "sess-b", "切台事件 → 先换显示会话");
    Check(session.displayed_id() == "sess-b", "切台后 SessionModel 更新");
    Check(host.pulls == 2, "切台后拉新 session 的树");
}

void TestTaskHistory()
{
    std::cout << "OnTaskHistory → 历史视图\n";
    edc::model::ChatModel chat;
    edc::model::SessionModel session;
    FakeHost host(session);
    edc::control::ChannelDispatcher d(chat, session, host);
    d.OnTaskHistory("{\"kind\":\"task\"}");
    Check(host.history.size() == 1 && host.history.back() == "{\"kind\":\"task\"}", "载荷透传给历史视图");
}

} // namespace

int main()
{
    TestRouting();
    TestHistoryViewGate();
    TestSnapshot();
    TestTreeChanged();
    TestTaskHistory();

    if (g_failures == 0)
    {
        std::cout << "\nall passed\n";
        return 0;
    }
    std::cout << "\n" << g_failures << " FAILED\n";
    return 1;
}
