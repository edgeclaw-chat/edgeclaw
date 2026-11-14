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
#include "edc_compat.h"
#ifndef _WIN32
#include "fake_release.h"
#endif
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

// 临时 HOME + 最小 config：TUIWindow 的构造会真起一个 app
//
// 注意：本项目用 -fno-exceptions 编译（见 .bazelrc）。所以这里的文件系统调用一律用带
// error_code 的重载 —— 不带的那版失败时抛异常，而异常被关掉后它直接 abort，测试会死在
// 没有任何信息的地方。带 error_code 的版本返回错误码，下面统一用 Fail() 报出来。
bool SetupHome(std::string *out_home)
{
    const char *tmp = getenv("TEST_TMPDIR");
    if (!tmp || !*tmp)
        return false;

    const std::string home = std::string(tmp) + "/home";
    const std::string dir = home + "/.edgeclaw";

    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec)
    {
        std::cout << "  FAIL 建不出 " << dir << "：" << ec.message() << "\n";
        return false;
    }

    std::ofstream conf(dir + "/config.json");
    if (!conf)
        return false;
    // 明文 api_key（无 EDGECLAW_ENC: 前缀 → 不需要解密；EDGECLAW_MASTER_KEY 让
    // secret 模块不必问口令）。api_url 指向一个没人听的端口：本测试不跑 LLM。
    conf << R"({"api_url":"https://127.0.0.1:1/v1/chat/completions",)"
            R"("model":"harness-model","api_key":"harness-plaintext-key","onboarded":true})";
    conf.close();

    // 一个 prompt 模板：给补全弹层的 `/prompt <name>` 词表用（否则那份词表是空的）。
    const std::string prompts = dir + "/workspace/prompts";
    std::filesystem::create_directories(prompts, ec);
    if (ec)
    {
        std::cout << "  FAIL 建不出 " << prompts << "：" << ec.message() << "\n";
        return false;
    }
    std::ofstream pf(prompts + "/review.md");
    if (!pf)
        return false;
    pf << "---\ndescription: review the staged changes\n---\nReview the staged changes.\n";
    pf.close();

    // 一个 skill：给 `/skill <name>` 的词表用（与上面的模板同一套路）。
    const std::string skill = dir + "/workspace/skills/demo";
    std::filesystem::create_directories(skill, ec);
    if (ec)
    {
        std::cout << "  FAIL 建不出 " << skill << "：" << ec.message() << "\n";
        return false;
    }
    std::ofstream sf(skill + "/SKILL.md");
    if (!sf)
        return false;
    sf << "---\ndescription: a harness skill\n---\nbody\n";
    sf.close();

    setenv("HOME", home.c_str(), 1);
    /* libuv uv_os_homedir() prefers USERPROFILE on Windows, so a test that
     * relocates the home directory has to move both or it reads the real one. */
    setenv("USERPROFILE", home.c_str(), 1);
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

// 同 SnapJson，但把前台 active task 的状态设成 |state|（用来驱动工作态：只有
// 处理态才让 working_ 立起来）。
std::string SnapJsonState(const char *session_id, const char *task_desc, const char *state)
{
    std::string json = R"({"session_id":")";
    json += session_id;
    json += R"(","session_name":"harness","message_count":1,"created_at":"2026-09-16 05:00:00","groups":[)"
            R"({"id":"1","kind":"user","member_count":1,"msg_count":1,"orphaned":false,"fg":true,)"
            R"("tokens_total":3,"ctx_tokens":45000,"tasks":[{"id":"4242","state":")";
    json += state;
    json += R"(","leader":true,"active":true,"desc":")";
    json += task_desc;
    json += R"(","tokens_prompt":1,"tokens_completion":2,"tokens_total":3,"tokens_cached":0}]}]})";
    return json;
}

// 同 SnapJsonState，但带上 end —— 侧栏字形与启动提示都靠它。
std::string SnapJsonEnd(const char *session_id, const char *task_id, const char *state, const char *end)
{
    std::string json = R"({"session_id":")";
    json += session_id;
    json += R"(","session_name":"harness","message_count":1,"created_at":"2026-09-16 05:00:00","groups":[)"
            R"({"id":"1","kind":"user","member_count":1,"msg_count":1,"orphaned":false,"fg":true,)"
            R"("tokens_total":3,"ctx_tokens":45000,"tasks":[{"id":")";
    json += task_id;
    json += R"(","state":")";
    json += state;
    json += R"(","end":")";
    json += end;
    json += R"(","leader":true,"active":false,"desc":"hint task","tokens_prompt":1,"tokens_completion":2,)"
            R"("tokens_total":3,"tokens_cached":0}]}]})";
    return json;
}

// 同 SnapJsonEnd，但 active 由调用方给：用来造"在后台跑完"的任务（active=false
// 才会计入 unread，见 TaskTreeModel::Apply）。
std::string SnapJsonTask(const char *session_id, const char *task_id, const char *desc, const char *state, bool active)
{
    std::string json = R"({"session_id":")";
    json += session_id;
    json += R"(","session_name":"harness","message_count":1,"created_at":"2026-09-16 05:00:00","groups":[)"
            R"({"id":"1","kind":"user","member_count":1,"msg_count":1,"orphaned":false,"fg":true,)"
            R"("tokens_total":3,"ctx_tokens":45000,"tasks":[{"id":")";
    json += task_id;
    json += R"(","state":")";
    json += state;
    json += R"(","leader":true,"active":)";
    json += active ? "true" : "false";
    json += R"(,"desc":")";
    json += desc;
    json += R"(","tokens_prompt":1,"tokens_completion":2,"tokens_total":3,"tokens_cached":0}]}]})";
    return json;
}

int CountOf(const std::string &hay, const std::string &needle)
{
    int n = 0;
    for (size_t p = hay.find(needle); p != std::string::npos; p = hay.find(needle, p + needle.size()))
        n++;
    return n;
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

// 帧里包含 |needle| 的那一行的行号（找不到返回 -1）。
int RowOf(const std::string &frame, const std::string &needle)
{
    int row = 0;
    size_t pos = 0;
    while (pos <= frame.size())
    {
        const size_t end = frame.find('\n', pos);
        const std::string line = end == std::string::npos ? frame.substr(pos) : frame.substr(pos, end - pos);
        if (line.find(needle) != std::string::npos)
            return row;
        if (end == std::string::npos)
            break;
        row++;
        pos = end + 1;
    }
    return -1;
}

// |row| 那一行的内容（超界返回空串）。
std::string LineOf(const std::string &frame, int row)
{
    if (row < 0)
        return "";
    int r = 0;
    size_t pos = 0;
    while (pos <= frame.size())
    {
        const size_t end = frame.find('\n', pos);
        if (r == row)
            return end == std::string::npos ? frame.substr(pos) : frame.substr(pos, end - pos);
        if (end == std::string::npos)
            break;
        r++;
        pos = end + 1;
    }
    return "";
}

// ── 输入框的命令补全弹层 ────────────────────────────────────────────────────
// 打 "/" 弹家族、"ta" 收敛、Tab 补全、Esc 关掉；Enter 默认仍是 Submit，只在上下浏览过
// 列表之后变成"选中"（见下面 TestPaletteEnterPicks）。
// ── 未读标记：读过的途径不止 `/task output` ────────────────────────────────
// 后台任务跑完 → 侧栏一枚 `*`（unread）。原来只有 `/task output|status|fg` 会清掉它，
// 可 `/task history` 同样是"读它的产出"的途径 —— 整个转录（含最后那条 output）就摆在
// 眼前，标记却还留着，等于让用户去敲第二条命令才能消掉一个已经看过的提示。
//
// 判据落在屏幕上：`*` 出现在那行 task 里 => 未读；消失 => 已读。
void TestHistoryMarksTaskRead(edc::TUIWindow &win)
{
    std::cout << "未读标记：/task history 也算读过了\n";
    const char *kUnreadTask = "UNREAD_TASK_DESC";

    /* 跑起来 → 后台结束：这是 unread 的产生条件（非 active + 进入终态）。 */
    Feed(win, MakeMsg(HARNESS_MSG_TASK_SNAPSHOT, SnapJsonTask(kShown, "4242", kUnreadTask, "LLM_PROCESSING", false).c_str(),
                      kShown));
    Feed(win, MakeMsg(HARNESS_MSG_TASK_SNAPSHOT, SnapJsonTask(kShown, "4242", kUnreadTask, "COMPLETED", false).c_str(),
                      kShown));
    Check(Has(FrameOf(win), "* 4242"), "后台跑完 → 侧栏出现未读标记");

    /* 读它的 history（走生产路径：结构化载荷 → RenderTaskHistory）。 */
    const char *payload =
        R"({"kind":"task","id":"4242","input":"UNREAD_INPUT","messages":[)"
        R"({"role":"assistant","content":"UNREAD_OUTPUT_MARKER","render":"plain"}]})";
    Feed(win, MakeMsg(HARNESS_MSG_TASK_HISTORY, payload, "", ""));
    const std::string frame = FrameOf(win);
    Check(Has(frame, "UNREAD_OUTPUT_MARKER"), "history 里能看到它的产出");
    Check(!Has(frame, "* 4242"), "看过 history → 未读标记消失（不必再敲 /task output）");

    /* Esc 回到实时流；标记不能复活（终态任务不会再"结束一次"）。 */
    Check(win.OnEvent(::ftxui::Event::Escape), "Esc 被吃掉");
    Check(!Has(FrameOf(win), "* 4242"), "回到实时流后仍未读标记");
}

void TestCommandPalette(edc::TUIWindow &win)
{
    std::cout << "输入框补全弹层\n";
    auto type = [&](const char *s) {
        for (const char *p = s; *p; p++)
            win.OnEvent(::ftxui::Event::Character(std::string(1, *p)));
    };

    // 起点：输入框是空的（上一条输入用例提交后清空了它）。
    type("/");
    {
        const std::string f = FrameOf(win);
        Check(Has(f, "/task") && Has(f, "/session") && Has(f, "/group"), "裸 / 先列三个容器家族");
        Check(Has(f, "Tab complete"), "带了提示行");

        // 面板是输入框**上方**的一块，而且在输入框的**上分割线之上**（中间正好隔着那条
        // 规则线）—— 不然它会看起来像输入框长高了。
        const int input_row = RowOf(f, "❯");
        const int hint_row = RowOf(f, "Tab complete");
        Check(input_row >= 0 && hint_row >= 0 && hint_row < input_row, "补全列表在输入框上方");
        Check(input_row - hint_row == 2 && LineOf(f, input_row - 1).find("─") != std::string::npos,
              "列表在上分割线之上（两者之间隔着规则线）");
    }

    // 唯一命中一个家族（"/ta" -> /task）时直接列出它的子命令 —— 家族那一行只是把名字和
    // 简介再念一遍，没有用处。
    type("ta");
    {
        const std::string f = FrameOf(win);
        Check(Has(f, "/task new"), "唯一命中一个家族 -> 直接列出子命令");
        Check(!Has(f, "create, inspect and control tasks"), "家族行的简介不再单独占一行");
        Check(!Has(f, "/session"), "无关家族被过滤");

        // 同一件事在 10 行子的列表上也成立（面板最多 8 行 + 提示行）。
        const int hint_row = RowOf(f, "Tab complete");
        const int prompt_row = RowOf(f, "❯");
        Check(hint_row >= 0 && prompt_row >= 0 && prompt_row - hint_row == 2 &&
                  LineOf(f, prompt_row - 1).find("─") != std::string::npos,
              "子命令列表在上分割线之上");
    }

    // Tab 一次只补一个词：'/ta' -> '/task '（列表继续列出子命令）。
    win.OnEvent(::ftxui::Event::Tab);
    {
        const std::string f = FrameOf(win);
        Check(Has(f, "❯ /task "), "Tab 补成了一个词：'/task '");
        Check(Has(f, "Tab complete") && Has(f, "/task new"), "列表还在，继续列子命令");
    }
    // 再按一次才落到高亮那一行的下一个词；此时已无可补 -> 同一次 Tab 收起。
    win.OnEvent(::ftxui::Event::Tab);
    {
        const std::string f = FrameOf(win);
        Check(Has(f, "❯ /task new"), "再一次 Tab 取高亮行的下一个词");
        Check(!Has(f, "Tab complete"), "无可再补 -> 同一次 Tab 收起列表");
    }

    // Esc 仍然只是关列表、内容留着：重新开一次验证。
    win.OnEvent(::ftxui::Event::Return); // 提交上一条，输入清空
    type("/task");
    Check(Has(FrameOf(win), "Tab complete"), "重新敲 /task 又开出子命令列表");
    win.OnEvent(::ftxui::Event::Escape);
    {
        const std::string f = FrameOf(win);
        Check(!Has(f, "Tab complete"), "Esc 关掉弹层");
        Check(Has(f, "/task"), "输入内容留着");
    }

    // Tab 补到"最后一个"时，同一次 Tab 就是"就这个了"：列表当即收起，不再多按一次。
    // 先提交上一条把输入清空，再从干净状态走一遍。
    win.OnEvent(::ftxui::Event::Return);
    type("/model");
    win.OnEvent(::ftxui::Event::Tab); // -> "/model "
    win.OnEvent(::ftxui::Event::Tab); // -> "/model search "，已无可补 -> 同一次即收起
    {
        const std::string f = FrameOf(win);
        Check(Has(f, "/model search"), "子命令 search 是独立的表行，能进补全列表");
        Check(Has(f, "❯ /model search "), "Tab 补到了 '/model search '");
        Check(!Has(f, "Tab complete"), "补到最后一个即收起（不再多按一次）");
    }

    // 收尾：把 '/model ' 提交掉，别把脏输入留给后面的用例。
    win.OnEvent(::ftxui::Event::Return);

    // "/session memo" 的子命令是独立的表行，所以它们能进补全列表并被 Tab 补上（以前它们挤
    // 在同一行的参数形状 "[show [id] | search <q> | ...]" 里，按整行前缀永远匹配不到）。
    type("/session memo ");
    {
        const std::string f = FrameOf(win);
        Check(Has(f, "/session memo show") && Has(f, "/session memo search") &&
                  Has(f, "/session memo curate") && Has(f, "/session memo compact"),
              "/session memo 的子命令进入补全列表");
    }
    win.OnEvent(::ftxui::Event::Tab);
    Check(Has(FrameOf(win), "❯ /session memo show"), "Tab 补到 '/session memo show '");
    win.OnEvent(::ftxui::Event::Escape);
    win.OnEvent(::ftxui::Event::Return); // 清空，别把脏输入留给后面的用例

    // `/prompt <name>`：它的参数词表来自 workspace/prompts/*.md 的模板名（harness 预置了
    // review.md）。模板名**从命令词打完那一刻**就在列表里（不必先打空格）。
    type("/prompt");
    Check(Has(FrameOf(win), "/prompt review"), "只打到 /prompt（还没空格）模板名已在列");
    Check(!Has(FrameOf(win), "/prompt <name>"), "占位行（/prompt <name> …）不再是候选");
    win.OnEvent(::ftxui::Event::Tab); // 先把命令词补完 -> "/prompt "
    Check(Has(FrameOf(win), "❯ /prompt "), "Tab 先补完命令词");
    {
        const std::string f = FrameOf(win);
        Check(Has(f, "/prompt review"), "空格之后模板名仍在");
        Check(Has(f, "review the staged changes"), "带上模板自己的 description");
    }
    win.OnEvent(::ftxui::Event::Tab);
    Check(Has(FrameOf(win), "❯ /prompt review "), "Tab 补全到模板名");
    win.OnEvent(::ftxui::Event::Return); // 收尾清空

    // `/skill <name>` 走同一条路：词表来自 workspace/skills/*/SKILL.md 的技能名（harness
    // 预置了 demo）。
    type("/skill");
    Check(Has(FrameOf(win), "/skill demo"), "只打到 /skill（还没空格）技能名已在列");
    Check(!Has(FrameOf(win), "/skill <name>"), "占位行（/skill <name> …）不再是候选");
    win.OnEvent(::ftxui::Event::Tab); // 先把命令词补完 -> "/skill "
    Check(Has(FrameOf(win), "❯ /skill "), "Tab 先补完命令词");
    {
        const std::string f = FrameOf(win);
        Check(Has(f, "/skill demo"), "空格之后技能名仍在");
        Check(Has(f, "a harness skill"), "带上技能自己的 description");
    }
    win.OnEvent(::ftxui::Event::Tab);
    Check(Has(FrameOf(win), "❯ /skill demo "), "Tab 补全到技能名");
    win.OnEvent(::ftxui::Event::Return); // 收尾清空
}
// 上下浏览过补全列表之后，Enter 是"就选这条"，不是发送：光标在列表里指着某一行，按下去
// 显然不是要把半截命令发出去。没浏览过（只是打字）时 Enter 照旧发送，`/help⏎` 那条老路不变。
void TestPaletteEnterPicks(edc::TUIWindow &win)
{
    std::cout << "补全列表里 Enter 是选中而不是发送\n";
    auto type = [&](const char *s) {
        for (const char *p = s; *p; p++)
            win.OnEvent(::ftxui::Event::Character(std::string(1, *p)));
    };
    // 输入行整行文本（行尾填充空格去掉）。
    auto input_line = [&](const std::string &f) {
        std::string line = LineOf(f, RowOf(f, "❯"));
        // 行首是固定的模式槽（"cmd" / "sh" / "!sh" / "err"，普通消息是 3 个空格）。
        // 这里只比从 "❯" 起的部分 —— 槽的可见性由 TestInputModeTag 单独钉。
        const size_t at = line.find("❯");
        if (at != std::string::npos)
            line = line.substr(at);
        while (!line.empty() && line.back() == ' ')
            line.pop_back();
        return line;
    };

    const std::string empty_line = input_line(FrameOf(win));

    // 1) 只打字、没上下浏览：Enter 仍然是发送（输入清空、弹层收起）。
    type("/model");
    Check(Has(FrameOf(win), "Tab complete"), "敲 /model 弹出列表");
    win.OnEvent(::ftxui::Event::Return);
    Check(input_line(FrameOf(win)) == empty_line, "没浏览时 Enter 仍然发送（输入被清空）");

    // 2) 基准：'↓' 走一行后按 Tab，取到高亮那一行的补全结果。
    type("/task ");
    Check(Has(FrameOf(win), "Tab complete"), "敲 '/task ' 弹出子命令列表");
    win.OnEvent(::ftxui::Event::ArrowDown);
    win.OnEvent(::ftxui::Event::Tab);
    const std::string picked_by_tab = input_line(FrameOf(win));
    Check(picked_by_tab.size() > std::string("❯ /task").size(), "Tab 落到了高亮那一行（实际 '" + picked_by_tab + "'）");
    win.OnEvent(::ftxui::Event::Return); // 清空，准备第二轮

    // 3) 同样 '↓' 走一行，这次按 Enter：结果与 Tab 完全相同，而且**没有发送**。
    type("/task ");
    win.OnEvent(::ftxui::Event::ArrowDown);
    win.OnEvent(::ftxui::Event::Return);
    const std::string picked_by_enter = input_line(FrameOf(win));
    Check(picked_by_enter == picked_by_tab,
          "浏览后 Enter 与 Tab 选中同一行（Enter '" + picked_by_enter + "' vs Tab '" + picked_by_tab + "'）");
    Check(picked_by_enter != empty_line, "而且没有发送：输入框没被清空");

    // 4) 选中改变了文本，"走列表"状态清零 —— 再按一次 Enter 才是发送。
    win.OnEvent(::ftxui::Event::Return);
    Check(input_line(FrameOf(win)) == empty_line, "再按一次 Enter 才发送（输入被清空）");

    // 5) 正在打的词还能再长的时候（'/tas' 的词是 '/task'），Enter 不能只补那个词：上下
    //    浏览过之后 Enter 采纳的是高亮那一行整条。这里落到第二行，结果是 '/task ' 加一个词。
    type("/tas");
    Check(Has(FrameOf(win), "Tab complete"), "敲 /tas 弹出 task 的子命令");
    win.OnEvent(::ftxui::Event::ArrowDown);
    win.OnEvent(::ftxui::Event::Return);
    const std::string picked_after_growing = input_line(FrameOf(win));
    Check(picked_after_growing.size() > std::string("❯ /task ").size(),
          "Enter 采纳整行，不是只补一个词（实际 '" + picked_after_growing + "'）");
    win.OnEvent(::ftxui::Event::Return); // 清空
}

// 回翻历史时补全弹层不能抢走上下键。历史里只要有一条以 '/' 开头的条目（`/task new` 这种
// 太常见了），回翻到它时弹层就会弹出来并接管 ArrowUp/ArrowDown —— 于是历史就地卡死在
// 那条上，再往上是"在列表里移动"而不是"回翻更早的一条"。
void TestHistoryRecallKeepsArrows(edc::TUIWindow &win)
{
    std::cout << "历史回翻不被补全弹层抢键\n";
    auto type = [&](const char *s) {
        for (const char *p = s; *p; p++)
            win.OnEvent(::ftxui::Event::Character(std::string(1, *p)));
    };

    // 输入行整行文本（行尾填充空格去掉）。必须整行比：`❯ /task` 是 `❯ /task new` 的前缀,
    // 用子串判定会让"根本没回翻"也通过。
    auto input_line = [&](const std::string &f) {
        std::string line = LineOf(f, RowOf(f, "❯"));
        // 行首是固定的模式槽（"cmd" / "sh" / "!sh" / "err"，普通消息是 3 个空格）。
        // 这里只比从 "❯" 起的部分 —— 槽的可见性由 TestInputModeTag 单独钉。
        const size_t at = line.find("❯");
        if (at != std::string::npos)
            line = line.substr(at);
        while (!line.empty() && line.back() == ' ')
            line.pop_back();
        return line;
    };

    // 两条历史：较新的 `/task new`（本来会开弹层的那种），较旧的 `/model`（两者前缀无关，
    // 回翻了哪一条一眼能分辨）。
    type("/model");
    win.OnEvent(::ftxui::Event::Return);
    type("/task new");
    win.OnEvent(::ftxui::Event::Return);
    Check(!Has(FrameOf(win), "Tab complete"), "提交后输入框清空，弹层收着");
    // 空草稿时输入行长什么样（带 dim placeholder 提示语）：下面"回到草稿"就是回到它。
    const std::string empty_line = input_line(FrameOf(win));

    // 空输入框 -> ArrowUp 回翻到最近一条。
    win.OnEvent(::ftxui::Event::ArrowUp);
    {
        const std::string f = FrameOf(win);
        Check(input_line(f) == "❯ /task new", "上键回翻到最近一条 /task new（实际 '" + input_line(f) + "'）");
        Check(!Has(f, "Tab complete"), "回翻出来的命令不算在打字：弹层不弹");
    }

    // 关键：这一次上键必须继续走历史（以前被弹层吃掉了，历史就停在这儿）。
    win.OnEvent(::ftxui::Event::ArrowUp);
    {
        const std::string f = FrameOf(win);
        Check(input_line(f) == "❯ /model", "上键继续回翻更早的一条（实际 '" + input_line(f) + "'）");
        Check(!Has(f, "Tab complete"), "回翻期间弹层一直收着");
    }
    win.OnEvent(::ftxui::Event::ArrowDown);
    Check(input_line(FrameOf(win)) == "❯ /task new", "下键回到较新的一条");
    win.OnEvent(::ftxui::Event::ArrowDown);
    Check(input_line(FrameOf(win)) == empty_line,
          "再下键越过最新一条：回到空草稿（实际 '" + input_line(FrameOf(win)) + "'）");
    Check(!Has(FrameOf(win), "Tab complete"), "草稿是空的，弹层也不该弹");

    // 一开始编辑就离开历史态：弹层按正常规则回来。
    win.OnEvent(::ftxui::Event::ArrowUp); // 重新回翻 /task new
    win.OnEvent(::ftxui::Event::Backspace);
    {
        const std::string f = FrameOf(win);
        Check(input_line(f) == "❯ /task ne", "退格编辑了回翻出来的那一行（实际 '" + input_line(f) + "'）");
        Check(Has(f, "Tab complete") && Has(f, "/task new"), "一编辑，补全弹层就回来");
    }

    // 收尾：Esc 关弹层，退格清空，别把脏输入留给后面的用例。
    win.OnEvent(::ftxui::Event::Escape);
    for (int i = 0; i < 32; i++)
        win.OnEvent(::ftxui::Event::Backspace);
}

// 输入框上面那条分割线兼做"这行会被怎么读"的标签（"──── shell ────"）：
// 普通消息保持整条线，其它情况把名字写进线里。它读的是派发用的同一个谓词
// （edc_commands_input_kind），所以标签和回车永远一致。
void TestInputModeTag(edc::TUIWindow &win)
{
    std::cout << "输入框分割线上的模式标签\n";
    auto type = [&](const char *s) {
        for (const char *p = s; *p; p++)
            win.OnEvent(::ftxui::Event::Character(std::string(1, *p)));
    };
    auto clear = [&]() {
        for (int i = 0; i < 40; i++)
            win.OnEvent(::ftxui::Event::Backspace);
    };

    // 普通消息：也标 message（空输入框是唯一不标的状态）。
    type("hello");
    Check(Has(FrameOf(win), "─── message "), "普通消息也标 message");
    Check(Has(FrameOf(win), "❯ hello"), "提示符位置不动（❯ 仍贴左）");
    clear();
    Check(!Has(FrameOf(win), "─── message ") && !Has(FrameOf(win), "─── command "), "清空后回到无标签");

    // `/` 命令：名字写进线里，靠左（不是居中 —— 居中看着像个装饰）。
    type("/task");
    Check(Has(FrameOf(win), "─── command "), "命令行：线上靠左标 command");
    Check(Has(FrameOf(win), "❯ /task"), "提示符位置不动（❯ 仍贴左）");
    clear();
}

// 键盘中断：Esc 只在有任务在跑时被吃掉（停下来）；Ctrl+C 第一次清空输入框、连按两次
// 才退出。Ctrl+C 不走 OnEvent（main() 的 CatchEvent 把它路由到 OnInterrupt）。
// 重启后侧栏里那批 INTERRUPTED 任务：启动时在聊天流里说一次，并给出恢复方式。
// 只出一次（同一个 session），而且不写 transcript —— 换会话就看不到。
void TestInterruptedStartupHint(edc::TUIWindow &win)
{
    std::cout << "启动提示：被中断的任务\n";

    // 切到一个别的 session：切会话会重置"只提示一次"的开关（生产路径同一条）。
    AdoptDisplayedSession(win, kOther);
    Feed(win, MakeMsg(HARNESS_MSG_TASK_SNAPSHOT, SnapJsonEnd(kOther, "HARNESS_INT", "INTERRUPTED", "INTERRUPTED").c_str(),
                      kOther));
    {
        const std::string f = FrameOf(win);
        Check(CountOf(f, "was interrupted") == 1, "一行提示说有个任务被中断");
        Check(Has(f, "/task resume"), "并给出恢复方式");
    }

    // 同一会话再来一帧：不再重复。
    Feed(win, MakeMsg(HARNESS_MSG_TASK_SNAPSHOT, SnapJsonEnd(kOther, "HARNESS_INT", "INTERRUPTED", "INTERRUPTED").c_str(),
                      kOther));
    Check(CountOf(FrameOf(win), "was interrupted") == 1, "同一个 session 只提示一次");

    // 换回原会话：提示不在 transcript 里，所以看不到它了。
    AdoptDisplayedSession(win, kShown);
    Feed(win, MakeMsg(HARNESS_MSG_TASK_SNAPSHOT, SnapJsonEnd(kShown, "HARNESS_IDLE", "IDLE", "NONE").c_str(), kShown));
    Check(!Has(FrameOf(win), "was interrupted"), "提示不写 transcript：换个会话就没了");
}

void TestInterruptKeys(edc::TUIWindow &win)
{
    std::cout << "键盘中断：Esc 停任务 / Ctrl+C 两段退出\n";

    // 空闲：前台任务不是处理态 → 没有任务可停，Esc 放行。
    Feed(win, MakeMsg(HARNESS_MSG_TASK_SNAPSHOT, SnapJsonState(kShown, "HARNESS_ESC_IDLE", "IDLE").c_str(), kShown));
    Check(!win.OnEvent(::ftxui::Event::Escape), "空闲时 Esc 放行（没有任务可停）");

    // 前台任务进入处理态 → 工作态立起来，Esc 被吃掉。
    Feed(win, MakeMsg(HARNESS_MSG_TASK_SNAPSHOT, SnapJsonState(kShown, "HARNESS_ESC_BUSY", "LLM_PROCESSING").c_str(),
                      kShown));
    Check(win.OnEvent(::ftxui::Event::Escape), "忙时 Esc 被吃掉（停前台任务）");

    // Ctrl+C：第一次清空输入框（不退出），紧接着第二次才退出。
    const std::string draft = "HARNESS_CTRLC_DRAFT";
    for (char c : draft)
        win.OnEvent(::ftxui::Event::Character(std::string(1, c)));
    Check(Has(FrameOf(win), draft), "打字后草稿在输入框里");
    Check(!win.OnInterrupt(), "第一次 Ctrl+C 不退出");
    Check(!Has(FrameOf(win), draft), "第一次 Ctrl+C 清空输入框");
    Check(Has(FrameOf(win), "Ctrl+C again to quit"), "第一次 Ctrl+C 后提示再按一次退出");
    Check(win.OnInterrupt(), "紧接着第二次 Ctrl+C 退出");
}

} // namespace

/* Set by main() when the local release server came up; the banner case needs it. */
static bool g_fake_release_up = false;

void TestUpdateSurface(edc::TUIWindow &win)
{
    std::cout << "更新入口（横幅 / /update / 升级协调）\n";

    auto *app = win.App();

    /* 这份二进制在 bazel-bin 里，不在 ~/.local/bin —— 正是"不是安装器装的"那种情况。
     * 它必须说自己不能升级，而不是给一个必然失败的按钮。 */
    Check(!app->UpdateCanSelfUpgrade(), "构建树里的二进制报告自己不能自升级");

    /* 不能升级就不该开始：既不取脚本，也不起子进程。 */
    bool done_called = false;
    Check(!app->UpdateStart("v9.9.9", [&](bool, std::string) { done_called = true; }),
          "不能自升级时 update_start 直接拒绝");
    Check(!done_called, "被拒绝时完成回调不会被调用");

    /* 什么都没跑的时候取消是空操作，不是崩溃：Esc 在任何状态都能到达这里。 */
    app->UpdateCancel();
    Check(true, "没有升级在跑时取消是空操作");

    /* 手动命令是不能自升级时的兜底，所以它永远不能是空的。 */
    const std::string hint = app->UpdateManualHint();
    Check(!hint.empty(), "手动升级命令非空");
    Check(hint.find("releases") != std::string::npos || hint.find("install.") != std::string::npos,
          "手动命令指向 release 页或安装器");

    /* 构造之后再注册观察者是允许的（前端就是构造完才注册）。 */
    app->SetUpdateObserver([](std::string) {});
    Check(true, "构造后可以再注册更新观察者");
}

/* 横幅 / `/update` 弹框：只有"检查真的找到了版本"才走得到。
 *
 * 靠 main() 在构造窗口之前起的那个本地假发布服务器 —— 检查在 edc_app_new 里跑一次，
 * 所以这里断言的是它跑过之后的状态：facade 的 UpdateFoundTrampoline 把 tag 送进
 * TUIWindow，update_tag_ 因此非空，横幅出现，`/update` 打开对话框。
 *
 * 这同时是唯一能走到 AppFacade::UpdateFoundTrampoline 的路径 —— 它是 static C 回调，
 * 除了"检查找到版本"没有别的入口。 */
void TestUpdateBanner(edc::TUIWindow &win)
{
    std::cout << "更新横幅（检查找到版本之后）\n";

    if (!g_fake_release_up)
    {
        std::cout << "  (跳过：本地假发布服务器没起来)\n";
        return;
    }

    /* 检查在窗口构造时就跑了，结果经 facade 的 trampoline 投递到 UI 线程，并在那一帧
     * 请求重绘。等它落地再读屏幕。 */
    std::string frame;
    for (int i = 0; i < 200; i++)
    {
        frame = FrameOf(win);
        if (Has(frame, "v9.9.9"))
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }

    Check(Has(frame, "v9.9.9"), "横幅上屏：检查找到的版本号出现了");
    Check(Has(frame, "is available"), "横幅说明了有新版本可用");
    Check(Has(frame, "/update"), "横幅提示用 /update 安装");

    /* `/update` 打开模态对话框。这份二进制在 bazel-bin 里（不是安装器的副本），所以它走的是
     * "无法自升级"那一页 —— 而不是"确认更新"那一页。这正是要断言的分支：能给的手动命令
     * 才是它该说的话，而不是一个必然失败的按钮。
     *
     * （"确认更新"那一页由 test_update_dialog 直接驱动，不需要这里再走一遍。） */
    TypeAndSubmit(win, "/update");
    const std::string modal = FrameOf(win);
    Check(Has(modal, "cannot update itself"), "/update 打开了对话框，并说明这份副本不能自升级");
    Check(Has(modal, "To update by hand"), "并把该手敲的命令给出来");
    Check(Has(modal, "Close"), "对话框有关闭按钮");

    /* Esc 关掉它。 */
    Check(win.OnEvent(::ftxui::Event::Escape), "Esc 被吃掉");
    const std::string after = FrameOf(win);
    Check(!Has(after, "cannot update itself"), "Esc 把对话框关掉了");
    Check(Has(after, "v9.9.9"), "横幅还在（它是个常驻事实，不是一次提示）");
}

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

#ifndef _WIN32
    /* 更新检查在 edc_app_new 里跑一次，所以服务器、endpoint 和 CA 包都必须在构造 TUIWindow
     * 之前就位。CA 包放 <workspace>/ssl/cacert.pem —— app 读的就是那个路径。
     *
     * 这三条环境变量是 edc_app.c 里的注入点（同 EDGECLAW_MODELS_CATALOG 那一类）：URL 指到
     * 本地假服务器、版本给一个可解析的值（dev 构建报的是 commit hash，检查会直接跳过）、
     * 延迟设 0 以免为了等那十秒而拖慢测试。 */
    {
        char url[256] = {0};
        char cert[600] = {0};
        if (fake_release_start_newer("v9.9.9", url, sizeof(url), cert, sizeof(cert)) == 0)
        {
            std::error_code ec;
            std::filesystem::create_directories(home + "/.edgeclaw/workspace/ssl", ec);
            if (!ec)
                std::filesystem::copy_file(cert, home + "/.edgeclaw/workspace/ssl/cacert.pem",
                                          std::filesystem::copy_options::overwrite_existing, ec);
            g_fake_release_up = !ec;
            setenv("EDGECLAW_UPDATE_RELEASES_URL", url, 1);
            setenv("EDGECLAW_UPDATE_CURRENT_VERSION", "1.0.0", 1);
            setenv("EDGECLAW_UPDATE_CHECK_DELAY_MS", "0", 1);
        }
    }

#endif
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
    TestHistoryMarksTaskRead(win);
    TestCommandPalette(win);
    TestPaletteEnterPicks(win);
    TestHistoryRecallKeepsArrows(win);
    TestInputModeTag(win);
    TestInterruptKeys(win);
    TestInterruptedStartupHint(win);
    TestUpdateSurface(win);
    TestUpdateBanner(win);

#ifndef _WIN32
    /* The fake release server served its one connection; release the port and its files. */
    fake_release_stop();
#endif

    if (g_failures)
    {
        std::cout << "\n" << g_failures << " 个断言失败\n";
        return 1;
    }
    std::cout << "\n全部通过\n";
    return 0;
}
