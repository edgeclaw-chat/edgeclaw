#pragma once

#include "edc_working_indicator_view.hpp" /* 动画行: WorkingIndicatorView (含旧入口 WorkingIndicatorElement) */
#include "edc_ports.hpp"                  /* 端口: IClock / ITerminalInfo */
#include "edc_app_facade.hpp"             /* 端口: IAppFacade / IChannelSink / IUiDispatcher */
#include "edc_channel_dispatcher.hpp"     /* 控制层: ChannelDispatcher / IChannelHost */
#include "edc_connect_dialog.hpp"         /* 视图: /connect 弹窗 */
#include "edc_transcript_store.hpp"       /* 端口: 转录存储 */
#include "edc_task_snapshot.hpp"
#include "edc_clipboard.hpp"
#include <chrono>
#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <functional>
#include <memory>
#include <string>

namespace edc
{
namespace ui
{
class SidebarView;
} // namespace ui

class ChatWidget;
class EdcInput;
class TaskWidget;
class TodoWidget;

// ── P3 ①b: 快照的**认领规则** ────────────────────────────────
//
// 快照现在是**拉取**的 (内核不再主动把树推给前端), 所以前端有两个纯判断:
// 收到一棵树时它是不是我要的, 收到"某棵树变了"时我要不要拉。两条规则都提成
// 纯函数, 单测直接钉它们 —— 这是"别人的树不许上屏"这条性质的前端那一半。

// A reply belongs to the frontend only when it is the tree it is currently
// displaying. A late reply (the user switched sessions while it was in flight)
// is yesterday's news: drop it. A reply that names no session is nobody's.
//
// |displayed_session| empty = nothing adopted yet: the snapshot claims the slot
// (the caller adopts its session_id). Safe because snapshots are **pulled**,
// never pushed -- the only reply that can arrive before the first adoption is
// the answer to the bootstrap pull, which names the active session.
bool SnapshotIsForSession(const TaskSnapshot *snap, const std::string &displayed_session);

// "session |event_session| changed for |reason|" → should the frontend pull?
//  - reason "active": the user switched tabs — the caller already adopted that
//    session as displayed, so yes, pull it;
//  - anything else: only if it is the tree on screen (a background session's
//    churn is not the displayed tab's business).
bool TreeChangedEventWantsPull(const std::string &event_session, const std::string &reason,
                               const std::string &displayed_session);

// 展示闸门 (P3 ③: 从 core 移到前端) —— 一条**实时**产出该去哪儿?
//   - kShown:        进当前视图 + 当前转录 (显示中的 session 的 active task 的产出);
//   - kSilent:       什么都不做 (同 session 里 **非** active task 的输出静默 ——
//                    以前这条判断在 core 里做, 靠"当前绑定的 session"那份副本);
//   - kOtherSession: 写进**它自己** session 的转录 (切回去能看到, 但不在当前 tab 冒泡)。
// 没有 session 标签的产出 (命令回复等, 发送方不是 task) 一律 kShown。
enum class LiveOutputRoute
{
    kShown,
    kSilent,
    kOtherSession,
};
LiveOutputRoute RouteLiveOutput(const std::string &session_id, bool from_active_task,
                                const std::string &displayed_session);

class TUIWindow : public ::ftxui::ComponentBase,
                  public control::IChannelHost,
                  public port::IUiDispatcher
{
  public:
    // |sandbox_override| mirrors --sandbox/--no-sandbox: -1 keeps the persisted
    // config.json value, 0 forces off, 1 forces on (per-run only). Runtime
    // toggling goes through /sandbox on|off. |terminal| supplies the width the
    // footer trims its fields against (nullptr = the real terminal); a test can
    // pass a fixed-width one.
    explicit TUIWindow(int sandbox_override = -1,
                       std::shared_ptr<const port::ITerminalInfo> terminal = nullptr);
    virtual ~TUIWindow();

    TUIWindow(const TUIWindow &) = delete;
    TUIWindow &operator=(const TUIWindow &) = delete;

    bool OnEvent(ftxui::Event) override;
    ::ftxui::Element OnRender() override;

    // The app's actors run on their own threads; adapter callbacks are marshalled
    // through this screen's task queue, so the tree is only touched by the
    // render thread.
    void SetScreen(::ftxui::ScreenInteractive *screen);

    // False when the app failed to start (bad/missing config, bad master key,
    // no terminal for the passphrase). main() checks this and exits instead of
    // dereferencing a NULL app.
    bool Ready() const
    {
        return app_ && app_->Ready();
    }

    // Pace the window's own animations (the working spinner and the sidebar's
    // breathing: processing dots + the in_progress todo row) from the main-loop
    // iteration; see TickAnimations().
    void TickAnimations();

    // ── 控制层 hook (IChannelHost) ───────────────────────────────
    // ChannelDispatcher 调用这些做"模型之外的视图动作"。事件本身由分派器承接
    // (AppFacade 注册的 sink 是它, 不是本窗口); 生产与测试同一条路径:
    // App()->HandleChannelMessage() → 分派器 → 这里的 hook。
    bool InHistoryView() const override
    {
        return history_view_;
    }
    bool PresentChat(const port::ChatMessageEvent &event, const std::string &kind_str) override;
    void LoadSessionTranscript(const std::string &session_id) override;
    void SetTaskTree(const TaskSnapshot &snap) override;
    void SyncWorking(const TaskSnapshot &snap) override;
    void SetTodos(const TaskSnapshot &snap) override;
    void PresentHistory(const std::string &payload) override;
    void RequestTaskSnapshot() override;
    void RequestFrame() override;

    // IUiDispatcher: 把任务投到 UI 线程。没有屏幕时 Post() 同步执行 (正是在这个
    // 分支上, harness 的喂消息会立刻跑到分派器)。
    void Post(std::function<void()> task) override;

    // 应用端口 (视图与测试用; 生命期与窗口一致)。
    port::IAppFacade *App()
    {
        return app_.get();
    }

    /* 退出"只看历史"模式, 回到实时流 (重放本次对话转录)。纯视图动作: kernel 不知道
     * 谁在看什么, 也不留任何状态。Esc 调它。 */
    void ExitHistoryView();

  private:
    /* 只看模式: 对话区正在显示一份拉回来的历史 (RenderTaskHistory 建的视图), 此时实时
     * 产出不上屏也不写转录 —— 写点在展示之后 (与从前的过滤同一个口径)。 */
    bool history_view_ = false;

    // Append the current input as a user message and clear the box. A bare
    // "/connect" opens the provider dialog instead of sending anything.
    void Submit();
    // Seed + open the /connect dialog (reads the persisted config).
    void OpenConnect();
    // Seed the input history from $EDC_HOME/.tui_history (a missing file is fine).
    void LoadInputHistory();
    void SaveInputHistory();
    /* `/task history <id>` / `/group history <id>` 的应答载荷: 拆成一条条气泡追加到
     * 对话区 (数据来自 scheduler 的 task->turns)。生产路径由 PresentHistory (IChannelHost)
     * 调; 私有 —— dispatch 测试同样经通道喂载荷, 不直接调。 */
    void RenderTaskHistory(const std::string &payload);
    // Show |session_id|'s chat log: the TUI's own transcript
    // (<session>/chat.jsonl, one line per bubble it showed) is replayed into the
    // cleared log. A session without one shows an empty log — nothing is derived
    // (see edc_chat_transcript.hpp for why the view keeps its own record).
    void LoadChatTranscript(const char *session_id);
    // Replay |path| into the (already cleared) log.
    void LoadChatTranscriptFile(const std::string &path);
    // Record one displayed bubble at the tail of the (current session's)
    // transcript, so the next start replays exactly what was on screen.
    void AppendChatTranscript(const std::string &kind, const std::string &header, const std::string &content,
                              const std::string &task_id, int render_pref);
    // Same, into an explicit path: a background session's output never lands in
    // the tab on screen, but it does belong in that session's own transcript.
    void AppendChatTranscriptTo(const std::string &path, const std::string &kind, const std::string &header,
                                const std::string &content, const std::string &task_id, int render_pref);
    // Show "processing" exactly while the foreground group's active task runs.
    void SyncWorkingFromSnapshot(const TaskSnapshot *snap);
    // Switch the log to |session_id|: when the id changes the log is cleared and
    // that session's messages re-derived, so the log always belongs to the
    // The left pane: chat log + input + footer/spinner.
    ::ftxui::Element RenderMain();
    // The working directory on the left; the model, sandbox state and "/help"
    // right-aligned within the chat pane's width (never into the sidebar).
    ::ftxui::Element RenderFooter() const;
    // Put the selection on the clipboard and raise the chip. Runs on the frame
    // after the drag was released, once FTXUI's selection is complete.
    void CopySelectionToClipboard();

    // ── 事件模式 (P4) ────────────────────────────────────────────
    // 从前 OnEvent 的优先级靠一串 if 的先后维持; 现在显式成一个模式: 粘贴 /
    // 只看历史 / 正常 (弹窗单独最先判 —— 它是最内层、且拥有全部事件)。三个
    // 处理器各自只处理本模式的事件。
    enum class InputMode
    {
        Paste,
        History,
        Normal,
    };
    InputMode inputMode() const
    {
        if (in_paste_)
            return InputMode::Paste;
        if (history_view_)
            return InputMode::History;
        return InputMode::Normal;
    }
    bool HandlePasteEvent(::ftxui::Event event);
    // 返回 true = 已消费; false = 放行给正常路径 (鼠标/滚轮/Alt+Enter)。
    bool HandleHistoryEvent(::ftxui::Event event);
    bool HandleNormalEvent(::ftxui::Event event);

    std::shared_ptr<edc::ChatWidget> chat_widget_;
    std::shared_ptr<edc::TaskWidget> task_widget_;
    std::shared_ptr<edc::TodoWidget> todo_widget_;
    // Multi-line input with soft wrapping (see EdcInput). Owned here, also
    // handed to |container_| for focus/events.
    std::shared_ptr<edc::EdcInput> input_;
    ::ftxui::Element prompt_;
    std::string input_text_;
    // Read once from the app's config; shown at the right end of the footer
    // (dropped only when the pane is too narrow).
    std::string model_;

    // Routes keyboard focus between the chat log and the input (1 = input,
    // focused from the start). Declared before |container_| because the
    // container borrows it.
    int focused_ = 1;
    ::ftxui::Component container_;

    // Drag-adjustable split: the task sidebar is on the right, resized by the
    // separator.
    int sidebar_width_ = 36;
    ::ftxui::Component split_;
    // The sidebar column (task tree over todo list); held so it outlives the split
    // it was handed to.
    std::shared_ptr<ui::SidebarView> sidebar_;

    // Set between the bracketed-paste ESC[200~ and ESC[201~, where Return is
    // inserted as a newline instead of submitting.
    bool in_paste_ = false;

    // ── 模型层 (P3) ─────────────────────────────────────────────
    // The chat log's state (messages + filter + task inputs) and the displayed
    // session's identity. Widgets render them; the ChannelDispatcher writes them.
    std::shared_ptr<model::ChatModel> chat_model_;
    std::shared_ptr<model::SessionModel> session_model_;

    // App port: owns the edc_app (assembly + the only C boundary).
    std::unique_ptr<port::AppFacade> app_;
    // Channel dispatcher: the AppFacade sink. Turns typed events into model
    // updates + the IChannelHost hooks below.
    std::unique_ptr<control::ChannelDispatcher> dispatcher_;
    // Chat-log file I/O (path, append, replay) — the transcript port (interface;
    // the concrete TranscriptStore is built in the ctor).
    std::unique_ptr<port::ITranscriptStore> transcript_;
    // Terminal geometry port: the footer trims its fields against this width.
    std::shared_ptr<const port::ITerminalInfo> terminal_;
    // True once bracketed paste was actually enabled, so the destructor only
    // emits the "disable" sequence when the matching "enable" ran.
    bool bracketed_paste_on_ = false;
    ::ftxui::ScreenInteractive *screen_ = nullptr;
    // The "Copied to clipboard" chip's own clock (see edc_clipboard.hpp).
    ClipboardToast clipboard_toast_;
    // Where the left-button drag started, and whether a copy is still due. The
    // copy waits for the next frame: FTXUI rebuilds its selection while that
    // frame is drawn, and a click without movement is not a selection.
    int press_x_ = 0;
    int press_y_ = 0;
    bool copy_pending_ = false;

    // /connect dialog: three fields (API URL / Model / API key) built once in
    // the constructor. While open it owns all keyboard/mouse events and renders
    // from these strings directly.
    // /connect dialog: a self-contained widget (owns its three fields, its
    // rendering and its event handling). The window only opens it and overlays
    // it; config persistence goes through the Submit callback given at build time.
    std::shared_ptr<ui::ConnectDialog> connect_dialog_;

    // True while the foreground group's active task runs. Owned exclusively by
    // SyncWorkingFromSnapshot(), never toggled by channel messages.
    bool working_ = false;
    // The animated "working" row, swapped in for the footer while working_. It
    // owns its own clock/timing (Restart() on the busy transition); the window
    // only paces its repaints (see TickAnimations).
    std::shared_ptr<ui::WorkingIndicatorView> working_indicator_;
    // When the spinner's next repaint is due (see TickAnimations).
    std::chrono::steady_clock::time_point last_spinner_frame_;
    // Whether the sidebar is currently breathing (a processing task's dot or an
    // in_progress todo row), and when that run started: the breath blooms in
    // from its dim end whenever the smooth run begins again after nothing was
    // breathing (see TickAnimations).
    bool dot_breathing_ = false;
    std::chrono::steady_clock::time_point breath_started_;
    // Throttles the periodic snapshot refresh, which is what catches
    // background-task state changes (they send no channel message).
    std::chrono::steady_clock::time_point last_snapshot_request_;
};
} // namespace edc
