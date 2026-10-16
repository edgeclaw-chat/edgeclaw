// TUIWindow 的"喂通道消息"harness。
//
// 为什么需要它：通道消息进 TUI 的唯一入口是 `AppFacade::HandleChannelMessage`
// （P2 之前是 `TUIWindow::on_channel_adapter_send`）。适配器先**分类**
// （`edc_app_message_is_task_tree_changed` / `..._is_task_snapshot` / 其余按
// `port::ChatKind::{User,Tool,Thought,Reply,System}`）、再**取字段**
// （`edc_app_message_*`），翻成强类型事件后交给 `IChannelSink`（TUIWindow 实现）。
// 谓词本身有 `test_snapshot_claim.cpp` / `test_domain_policies.cpp` 钉住，但
// **分类 + 取字段这一段谁都没测过**：把 `edc_app_message_is_task_snapshot` 改成
// 恒 0，谓词测试不红、A/B 也不红（A/B 比的是状态机产物，不比屏幕）—— 于是
// "某类消息根本到不了它该去的 widget"这类故障没有任何判据。
//
// 这个 harness 把**真的** `struct edc_channel_message` 喂进**真的** TUIWindow
// （经它自己的 AppFacade），然后读屏幕（`OnRender()` 的帧）。它盖住的正是 A/B
// 看不到的那一层。
//
// 两个测试接缝：
//   1. `IAppFacade::HandleChannelMessage` 是公开的（生产路径注册的 C 回调最终
//      进这里），测试直接 `win.App()->HandleChannelMessage(m)` 喂一条真消息；
//   2. `EDGECLAW_TUI_NO_PULL=1` 让 `RequestTaskSnapshot()` 变 no-op —— 否则构造期
//      与回调里的拉取会经 app **异步**回包，而没 SetScreen() 时那回包内联在
//      runner 线程上（Post() 的 else 分支），会和本测试自己的断言抢同一个 widget。

#include "edc_tui_window.hpp"
#include "tui_dispatch_msgs.h"
#include "ftxui/component/mouse.hpp"
#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/screen.hpp"
#include "ftxui/screen/terminal.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <sys/stat.h>
#include <utility>

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

constexpr int kW = 100;
constexpr int kH = 40;

// 显示中的那个 session（测试自己点名要它，见 AdoptDisplayedSession()）。
constexpr const char *kShown = "harness-shown";
// 另一个 session：它的树/产出都不该出现在屏上。
constexpr const char *kOther = "harness-other";
// 首帧采纳那条用例用的 session：窗口启动时还没身份，由第一帧快照占位。
constexpr const char *kFirst = "harness-first";

// ── 临时 HOME + 最小 config：TUIWindow 的构造会真起一个 app ──────────────
bool SetupHome(std::string *out_home)
{
    const char *tmp = getenv("TEST_TMPDIR");
    if (!tmp || !*tmp)
        return false;

    const std::string home = std::string(tmp) + "/home";
    const std::string dir = home + "/.edgeclaw";
    std::filesystem::create_directories(dir);

    std::ofstream conf(dir + "/config.json");
    if (!conf)
        return false;
    // 明文 api_key（无 EDGECLAW_ENC: 前缀 → 不需要解密；EDGECLAW_MASTER_KEY 让
    // secret 模块不必问口令）。api_url 指向一个没人听的端口：本测试不跑 LLM。
    conf << R"({"api_url":"https://127.0.0.1:1/v1/chat/completions",)"
            R"("model":"harness-model","api_key":"harness-plaintext-key","onboarded":true})";
    conf.close();

    setenv("HOME", home.c_str(), 1);
    setenv("EDGECLAW_MASTER_KEY", "harness-master-key", 1);
    setenv("EDGECLAW_TUI_NO_PULL", "1", 1); /* 接缝 2：构造期/回调里都不拉树 */
    *out_home = home;
    return true;
}

// ── 造一条真消息（生产路径用的是同一个构造器；那两个 sds 字段归 C 侧填）──────
// |type| 是 HARNESS_MSG_*（C++ 侧引不到 edc_config.h：它拉 sds）。
struct edc_channel_message *MakeMsg(int type, const char *content, const char *session_id, const char *header = nullptr,
                                    int from_active_task = 0)
{
    return harness_msg_new(type, content, session_id, header, from_active_task);
}

void Feed(edc::TUIWindow &win, struct edc_channel_message *m)
{
    /* P2: C 消息的分类/取字段已搬进 AppFacade, 窗口实现 IChannelSink。喂消息的
     * 缝因此从 static on_channel_adapter_send 换成端口的适配器入口
     * (App()->HandleChannelMessage) —— 与生产路径注册的是同一段翻译逻辑。 */
    win.App()->HandleChannelMessage(m);
    /* 生产路径由通道在回调之后释放（edc_channel_actor_handle）；测试里消息归调用方。 */
    harness_msg_free(m);
}

// ── 读屏幕：帧 ────────────────────────────────────────────────────────────
// 先渲染两遍再取第三遍：ftxui 的测量在 render 阶段提交，首帧是暂态
// （见 tests/test_task_widget.cpp 的同一个套路）。
std::string FrameOf(edc::TUIWindow &win)
{
    auto make_screen = [] {
        return ::ftxui::Screen::Create(::ftxui::Dimension::Fixed(kW), ::ftxui::Dimension::Fixed(kH));
    };
    ::ftxui::Screen screen = make_screen();
    for (int pass = 0; pass < 3; pass++)
    {
        screen = make_screen();
        ::ftxui::Render(screen, win.OnRender());
    }

    std::string frame;
    for (int y = 0; y < kH; y++)
    {
        for (int x = 0; x < kW; x++)
        {
            const std::string &cell = screen.CellAt(x, y).character;
            frame += cell.empty() ? " " : cell;
        }
        frame += "\n";
    }
    return frame;
}

bool Has(const std::string &frame, const std::string &needle)
{
    return frame.find(needle) != std::string::npos;
}

/* 界面 chrome 不许夹中文。只查 CJK 区间 (CJK 统一表意 / 全角标点 / 中日韩符号),
 * 箱子与箭头 (● ▸ ▾ … ❯ ·) 不在其中, 所以这个谓词能直接用在整帧上。 */
bool HasCJK(const std::string &frame)
{
    for (size_t i = 0; i + 2 < frame.size(); i++)
    {
        const unsigned char *p = reinterpret_cast<const unsigned char *>(frame.data()) + i;
        if (p[0] < 0xE0)
            continue;
        const unsigned cp = ((p[0] & 0x0Fu) << 12) | ((p[1] & 0x3Fu) << 6) | (p[2] & 0x3Fu);
        if ((cp >= 0x3000 && cp <= 0x303F) || (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0xFF00 && cp <= 0xFFEF))
            return true;
    }
    return false;
}

std::string SnapJson(const char *session_id, const char *task_desc)
{
    std::string json = R"({"session_id":")";
    json += session_id;
    json += R"(","session_name":"harness","message_count":1,"created_at":"2026-09-16 05:00:00","groups":[)"
            R"({"id":"1","kind":"user","member_count":1,"msg_count":1,"orphaned":false,"fg":true,)"
            R"("tokens_total":3,"tasks":[{"id":"4242","state":"IDLE","leader":true,"active":true,)"
            R"("desc":")";
    json += task_desc;
    json += R"(","tokens_prompt":1,"tokens_completion":2,"tokens_total":3,"tokens_cached":0}]}]})";
    return json;
}

// 让窗口把 |session_id| 当成"正在显示的那一棵" —— 走的是生产路径的同一条：
// tree-changed(reason=active) ⇒ LoadChatTranscript() ⇒ current_session_id_ = sid。
void AdoptDisplayedSession(edc::TUIWindow &win, const char *session_id)
{
    Feed(win, MakeMsg(HARNESS_MSG_TASK_TREE_CHANGED, "active", session_id));
}

// ── 各条断言 ──────────────────────────────────────────────────────────────

// 首帧采纳：窗口还没采纳任何 session（current_session_id_ 为空）时，第一帧快照
// 自证身份并占位 —— 这条盖住 TUIWindow 里那段采纳接缝，是"不再同步读 session 仓库"
// 的判据。**必须先于 AdoptDisplayedSession() 跑**（那时才真的没身份）。
//
// 第二条断言是这条测试的要害：采纳一旦没发生（身份一直空着），下面的 kOther 也会被
// 当成"第一帧"收下，于是它上屏 —— 所以"别人的树没出现"正是"身份真的立住了"的证据。
void TestFirstSnapshotIsAdopted(edc::TUIWindow &win)
{
    std::cout << "首帧采纳（未采纳时第一帧自证身份）\n";

    Feed(win, MakeMsg(HARNESS_MSG_TASK_SNAPSHOT, SnapJson(kFirst, "HARNESS_FIRST_TASK").c_str(), kFirst));
    Check(Has(FrameOf(win), "HARNESS_FIRST_TASK"), "未采纳时第一帧 → 采纳并上屏");

    Feed(win, MakeMsg(HARNESS_MSG_TASK_SNAPSHOT, SnapJson(kOther, "HARNESS_XTRA_TASK").c_str(), kOther));
    const std::string frame = FrameOf(win);
    Check(!Has(frame, "HARNESS_XTRA_TASK"), "采纳之后别人的树 → 丢弃（身份立住了）");
    Check(Has(frame, "HARNESS_FIRST_TASK"), "第一帧的树还在");
}

// 快照 → 侧栏。**这条是 task 558 那个 M1 缺口的判据**：分类器坏掉时它必须红。
void TestSnapshotReachesSidebar(edc::TUIWindow &win)
{
    std::cout << "快照 → 侧栏\n";
    Feed(win, MakeMsg(HARNESS_MSG_TASK_SNAPSHOT, SnapJson(kShown, "HARNESS_ALPHA_TASK").c_str(), kShown));
    const std::string frame = FrameOf(win);
    Check(Has(frame, "HARNESS_ALPHA_TASK"), "本 session 的树进了侧栏");
    Check(Has(frame, "4242"), "任务 id 也在");
}

// 别人的树 → 丢弃（SnapshotIsForSession 这条线不能绕过去）。
void TestForeignSnapshotDropped(edc::TUIWindow &win)
{
    std::cout << "快照（别人的 session）→ 丢弃\n";
    Feed(win, MakeMsg(HARNESS_MSG_TASK_SNAPSHOT, SnapJson(kOther, "HARNESS_BETA_TASK").c_str(), kOther));
    const std::string frame = FrameOf(win);
    Check(!Has(frame, "HARNESS_BETA_TASK"), "别人的树没上屏");
    Check(Has(frame, "HARNESS_ALPHA_TASK"), "我自己的树还在");
}

// 活产出：显示中的 session 的 active task 才上屏；同 session 的非 active 静默；
// 后台 session 的产出也不上屏（RouteLiveOutput + 转录分流）。
void TestLiveOutputRouting(edc::TUIWindow &win)
{
    std::cout << "活产出路由\n";

    {
        Feed(win, MakeMsg(HARNESS_MSG_RESP, "HARNESS_SHOWN_LINE", kShown, nullptr, /*from_active_task=*/1));
        Check(Has(FrameOf(win), "HARNESS_SHOWN_LINE"), "本 session 的 active task 产出 → 上屏");
    }
    {
        Feed(win, MakeMsg(HARNESS_MSG_RESP, "HARNESS_INACTIVE_LINE", kShown, nullptr, /*from_active_task=*/0));
        Check(!Has(FrameOf(win), "HARNESS_INACTIVE_LINE"), "同 session 的非 active task 产出 → 静默");
    }
    {
        Feed(win, MakeMsg(HARNESS_MSG_RESP, "HARNESS_BG_LINE", kOther, nullptr, /*from_active_task=*/1));
        Check(!Has(FrameOf(win), "HARNESS_BG_LINE"), "后台 session 的产出 → 不上屏");
    }
}

// 工具/思考/系统消息各归各位（kind 映射），todowrite 的 TOOL 不进对话区。
void TestKindRouting(edc::TUIWindow &win)
{
    std::cout << "kind 映射\n";
    {
        Feed(win, MakeMsg(HARNESS_MSG_TOOL, "HARNESS_TOOL_LINE", kShown, "exec_shell", /*from_active_task=*/1));
        Check(Has(FrameOf(win), "HARNESS_TOOL_LINE"), "工具输出 → 上屏");
    }
    {
        Feed(win, MakeMsg(HARNESS_MSG_TOOL, "HARNESS_TODO_LINE", kShown, "todowrite", /*from_active_task=*/1));
        Check(!Has(FrameOf(win), "HARNESS_TODO_LINE"), "todowrite 的 TOOL → 不进对话区（只看侧栏）");
    }
    {
        Feed(win, MakeMsg(HARNESS_MSG_THOUGHT, "HARNESS_THOUGHT_LINE", kShown, nullptr, /*from_active_task=*/1));
        const std::string frame = FrameOf(win);
        // 思考是一枚**折叠**开关（edc_message.cpp:123 的 "Thought" label）：上屏的证据
        // 是那枚开关出现在对话区，内容要用户展开才渲染 —— 所以这里断言标签，不断言正文。
        Check(Has(frame, "Thought"), "思考 → 上屏（折叠态，只露出开关标签）");
        Check(!Has(frame, "HARNESS_THOUGHT_LINE"), "折叠态不显示正文（展开才显示）");
    }
    {
        Feed(win, MakeMsg(HARNESS_MSG_SYS, "HARNESS_SYS_LINE", kShown, nullptr, /*from_active_task=*/1));
        Check(Has(FrameOf(win), "HARNESS_SYS_LINE"), "系统消息 → 上屏");
    }
}

// 兜底：认不出来的消息什么都不做（Kind::None → `default: return`）。
void TestUnknownMessageIgnored(edc::TUIWindow &win)
{
    std::cout << "未知消息 → 不动屏\n";
    const std::string before = FrameOf(win);
    Feed(win, MakeMsg(HARNESS_MSG_GARBAGE, "HARNESS_UNKNOWN_LINE", kShown));
    const std::string after = FrameOf(win);
    Check(before == after, "帧逐字节不变");
    Check(!Has(after, "HARNESS_UNKNOWN_LINE"), "内容没上屏");
}

// ── "任务建成" 事件：用户输入由 kernel 画上屏 ────────────────────────────────
// 用户敲的那条在 Submit 时立刻回显（前端自己画 —— 不建 task 的输入没有事件，
// 否则它什么都显示不出来）；它属于哪个 task 则由 kernel 说的算：任务建成那一刻
// 发"任务建成"事件，带 task_id + 用户原文，前端拿原文认出屏幕上那条气泡、把
// task_id 打上去。前端从前按转录顺序推断归属，那既依赖展示闸门、又会被"前端写
// 输入 / scheduler 收尾上一个任务"的跨线程顺序骗到。
//
// 这一条把整条链走通：侧栏选中一个 task（过滤生效）→ 敲一条输入（回显但没有归属，
// 回显立刻上屏；归属来自快照（task 自己的 raw_input，见 test_chat_filter），不经过
// 任何事件。会话用 harness 自己造的那个：scheduler 解析不到它，于是**不会有真的
// 任务被建**，测试是确定性的（不跟 app 的线程赛跑）。

// 用键盘事件把 |text| 敲进输入框并回车（Submit 走的是生产路径上的同一入口）。
void TypeAndSubmit(edc::TUIWindow &win, const std::string &text)
{
    for (char c : text)
        win.OnEvent(::ftxui::Event::Character(std::string(1, c)));
    win.OnEvent(::ftxui::Event::Return);
}

// 帧里包含 |needle| 的那一行，以及该行上 |glyph| 所在的列（找不到返回 {-1,-1}）。
std::pair<int, int> FindCell(const std::string &frame, const std::string &needle, const std::string &glyph)
{
    int row = 0;
    size_t pos = 0;
    while (pos <= frame.size())
    {
        const size_t end = frame.find('\n', pos);
        const std::string line = end == std::string::npos ? frame.substr(pos) : frame.substr(pos, end - pos);
        if (line.find(needle) != std::string::npos)
        {
            const size_t col = line.find(glyph);
            if (col != std::string::npos)
                return {static_cast<int>(col), row};
        }
        if (end == std::string::npos)
            break;
        row++;
        pos = end + 1;
    }
    return {-1, -1};
}

void TestUserInputIsEchoedImmediately(edc::TUIWindow &win)
{
    std::cout << "用户输入立刻回显（归属不由前端管）\n";

    TypeAndSubmit(win, "HARNESS_ECHO_LINE");
    Check(Has(FrameOf(win), "HARNESS_ECHO_LINE"), "敲下就上屏");

    // 归属不发往任何一个 task: 输入那条气泡不带 task id。
    const std::string frame = FrameOf(win);
    Check(Has(frame, "4242"), "侧栏里 task 行照旧");

    // 界面的固定文案是英文的 (与仓库注释的语言无关): 输入框占位符、footer 的
    // `/help` 提示符。footer 在窄帧里会被裁掉, 所以除了点名那两句, 再加一条整帧
    // 无 CJK 的守卫 —— 它同时钉住"以后新加的 chrome 也不许夹中文"。
    Check(Has(frame, "Type a message"), "输入框占位符是英文的");
    Check(!HasCJK(frame), "整帧无中文 (chrome 全英文)");
}

// ── `/task history` / `/group history` 的载荷 → 气泡 ──────────────────────
// 载荷是 kernel 给的 (scheduler 的 task->turns), 前端只做"拆成气泡 + 用 render 字段
// 决定渲染方式"。这里喂真载荷, 断言那几条消息真的落到了对话区。
void TestTaskHistoryPayloadBecomesBubbles(edc::TUIWindow &win)
{
    const char *task_payload =
        "{\"kind\":\"task\",\"id\":\"77\",\"input\":\"HIST_INPUT_MARKER\",\"messages\":["
        "{\"role\":\"user\",\"content\":\"HIST_INPUT_MARKER\",\"render\":\"plain\"},"
        "{\"role\":\"assistant\",\"content\":\"HIST_REPLY_MARKER\",\"render\":\"plain\"},"
        "{\"role\":\"assistant\",\"content\":\"\",\"tool_calls\":[{\"function\":{\"name\":\"read_file\"}}]},"
        "{\"role\":\"tool\",\"name\":\"HIST_TOOL_MARKER\",\"content\":\"int main(){}\",\"render\":\"plain\"}]}";
    Feed(win, MakeMsg(HARNESS_MSG_TASK_HISTORY, task_payload, "", ""));
    std::string frame = FrameOf(win);
    Check(Has(frame, "HIST_INPUT_MARKER"), "task 载荷: 第一条 user 消息上屏");
    Check(Has(frame, "HIST_REPLY_MARKER"), "task 载荷: assistant 消息上屏");
    Check(Has(frame, "read_file"), "task 载荷: 只有 tool_calls 的消息画成一行调用");
    Check(Has(frame, "HIST_TOOL_MARKER"), "task 载荷: tool 消息的工具名上屏");

    /* 组载荷: 每个成员一块, 块首是它那条输入。 */
    const char *group_payload =
        "{\"kind\":\"group\",\"id\":\"1\",\"tasks\":["
        "{\"id\":\"5\",\"input\":\"GRP_A_INPUT\",\"messages\":[{\"role\":\"user\",\"content\":\"GRP_A_MSG\"}]},"
        "{\"id\":\"6\",\"input\":\"GRP_B_INPUT\",\"messages\":[{\"role\":\"user\",\"content\":\"GRP_B_MSG\"}]}]}";
    Feed(win, MakeMsg(HARNESS_MSG_TASK_HISTORY, group_payload, "", ""));
    frame = FrameOf(win);
    Check(Has(frame, "GRP_A_INPUT") && Has(frame, "GRP_A_MSG"), "组载荷: 第一个成员一块");
    Check(Has(frame, "GRP_B_INPUT") && Has(frame, "GRP_B_MSG"), "组载荷: 第二个成员一块");

    /* 坏载荷 (不是 JSON) 不能把前端打崩, 也不能画出半个人。 */
    Feed(win, MakeMsg(HARNESS_MSG_TASK_HISTORY, "not json at all", "", ""));
    frame = FrameOf(win);
    Check(!Has(frame, "not json at all"), "坏载荷: 什么都不画");

    /* 只看模式: 历史替换对话区, 实时产出不再上屏, Esc 回到实时流 (重放转录)。 */
    Feed(win, MakeMsg(HARNESS_MSG_TASK_HISTORY, task_payload, "", ""));
    frame = FrameOf(win);
    Check(Has(frame, "HIST_REPLY_MARKER"), "只看模式: 历史替换了对话区");
    Check(win.InHistoryView(), "只看模式: 视图状态位立起来了");
    Check(!Has(frame, "Type a message"), "只看模式: 输入框消失");
    Check(Has(frame, "History · Esc to go back"), "只看模式: 历史面板徽标是英文的");
    Check(!HasCJK(frame), "只看模式: 整帧无中文");
    Check(win.OnEvent(::ftxui::Event::Character("x")) && win.OnEvent(::ftxui::Event::Return),
          "只看模式: 字符与回车都被吞掉 (不可输入)");
    Feed(win, MakeMsg(HARNESS_MSG_RESP, "LIVE_WHILE_HISTORY_MARKER", "", ""));
    frame = FrameOf(win);
    Check(!Has(frame, "LIVE_WHILE_HISTORY_MARKER"), "只看模式: 实时产出不上屏");
    Check(win.OnEvent(::ftxui::Event::Escape), "Esc 被吃掉");
    frame = FrameOf(win);
    Check(!Has(frame, "HIST_REPLY_MARKER"), "Esc 之后历史内容消失");
    /* "回到实时流"的可观测判据 = 实时产出又能上屏 (转录重放发生在 harness 里看不到文件那一侧)。 */
    Feed(win, MakeMsg(HARNESS_MSG_RESP, "LIVE_AFTER_ESC_MARKER", "", ""));
    frame = FrameOf(win);
    Check(!win.InHistoryView(), "Esc 之后视图状态位落下");
    Check(Has(frame, "LIVE_AFTER_ESC_MARKER"), "Esc 之后实时产出又上屏");
}

} // namespace

int main()
{
    // ftxui 的测试进程没有 tty：先声明颜色支持，断言前再看文本（见 test_theme.cpp）。
    ::ftxui::Terminal::SetColorSupport(::ftxui::Terminal::Color::TrueColor);

    std::string home;
    if (!SetupHome(&home))
    {
        std::cout << "  FAIL 准备临时 HOME 失败（没有 TEST_TMPDIR？）\n";
        return 1;
    }

    edc::TUIWindow win(/*sandbox_override=*/0);
    if (!win.Ready())
    {
        std::cout << "  FAIL 窗口没起来（app 起不来 —— 临时 HOME 里的 config.json 检查一下）\n";
        return 1;
    }
    Check(true, "窗口起来了（临时 HOME + 最小 config）");

    // 先跑首帧采纳（此刻窗口还没采纳任何 session），它顺带把身份立成 kFirst；
    // 再切到 kShown 供其余用例使用 —— 这条切换同时压住"TreeChanged(active) ⇒
    // LoadChatTranscript"那条生产路径。
    TestFirstSnapshotIsAdopted(win);
    AdoptDisplayedSession(win, kShown);

    TestSnapshotReachesSidebar(win);
    TestForeignSnapshotDropped(win);
    TestLiveOutputRouting(win);
    TestKindRouting(win);
    TestUnknownMessageIgnored(win);
    TestUserInputIsEchoedImmediately(win);
    TestTaskHistoryPayloadBecomesBubbles(win);

    if (g_failures)
    {
        std::cout << "\n" << g_failures << " 个断言失败\n";
        return 1;
    }
    std::cout << "\n全部通过\n";
    return 0;
}
