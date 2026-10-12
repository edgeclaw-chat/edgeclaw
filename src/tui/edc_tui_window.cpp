#include <cJSON.h>
#include "edc_tui_window.hpp"
#include "edc_working_indicator_view.hpp"
#include "edc_domain.hpp"
#include "edc_sidebar.hpp"
#include "edc_chat_transcript.hpp"
#include "edc_chat_widget.hpp"
#include "edc_input.hpp"
#include "edc_scroll_view.hpp"
#include "edc_style.hpp"
#include "edc_task_widget.hpp"
#include "edc_todo_widget.hpp"
#include "ftxui/component/component.hpp"
#include "ftxui/component/component_options.hpp"
#include "ftxui/component/event.hpp"
#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/string.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <memory>
#include <set>
#include <string>
#include "edc_api.h" /* EDC_APP_RENDER_PLAIN / _MARKDOWN 常量 */
#include <unistd.h>
#include <vector>

namespace edc
{
namespace
{
static void EnableBracketedPaste()
{
    std::fputs("\x1b[?2004h", stdout);
    std::fflush(stdout);
}
static void DisableBracketedPaste()
{
    std::fputs("\x1b[?2004l", stdout);
    std::fflush(stdout);
}

// The state of the foreground group's active task -- the task whose todo list
// the sidebar shows -- or "" when there is none. Only the foreground group's
// active task counts: a busy sibling in the same group is not the foreground
// process, so it must not light the spinner up or repaint the todo list, which
// belongs to this one task. Both ForegroundProcessBusy below and the todo widget
// read this, so they cannot disagree about which task owns the list.
const char *ForegroundActiveTaskState(const TaskSnapshot *snap)
{
    if (!snap)
        return "";

    for (const TaskGroup &g : snap->groups)
    {
        if (!g.fg)
            continue;
        for (const TaskInfo &t : g.tasks)
        {
            if (!t.is_active)
                continue;
            return t.state.c_str();
        }
        return ""; // foreground group exists but has no active task marked
    }
    return ""; // no foreground group at all
}

// The animated row reflects the scheduler's process state, not the channel
// stream: it spins exactly while the foreground group's active task runs (LLM
// generating or a tool chain in flight). Other members of the group, background
// groups, completed tasks and tasks waiting for input/deps never count as busy.
bool ForegroundProcessBusy(const TaskSnapshot *snap)
{
    return style::TaskStateProcessing(ForegroundActiveTaskState(snap));
}

// How often the working spinner repaints: the bar (ui::WorkingIndicatorView)
// moves one cell per its own step, so a frame per loop tick would redraw the
// whole tree for sub-cell movement nobody can see; see TickAnimations() for why
// the pacing cannot live in the render.
constexpr long long kSpinnerFrameMs = 33;

// Todo sidebar content: the foreground group's active task -- its id and its own
// todo list, which is part of the task state (todowrite attaches it to the task
// that called it). The snapshot already says which task is active, so it is the
// single source of truth here too; the id keeps the section readable once it is
// collapsed.
struct ActiveTaskTodos
{
    std::string task_id;
    std::vector<TodoWidget::Item> items;
};

ActiveTaskTodos ActiveTaskTodosOf(const TaskSnapshot *snap)
{
    ActiveTaskTodos todos;
    if (!snap)
        return todos;

    for (const TaskGroup &g : snap->groups)
    {
        if (!g.fg)
            continue;
        for (const TaskInfo &t : g.tasks)
        {
            if (!t.is_active)
                continue;
            // The id honesty: this is the task the section follows. Whether it is
            // worth showing is the widget's call -- an empty list shows none.
            todos.task_id = t.task_id;
            todos.items.reserve(t.todos.size());
            for (const TodoItem &todo : t.todos)
            {
                TodoWidget::Item item;
                item.content = todo.content;
                item.status = todo.status;
                item.priority = todo.priority;
                todos.items.push_back(std::move(item));
            }
            return todos;
        }
        return todos; // foreground group exists but has no active task marked
    }
    return todos; // no foreground group at all
}

// $EDC_HOME/.tui_history stores one recalled entry per line. Backslash and the
// control characters that would break the one-line format are escaped so a
// multi-line message (Alt+Enter newlines) round-trips as a single line.
std::string EscapeHistoryEntry(const std::string &entry)
{
    std::string out;
    out.reserve(entry.size());
    for (char c : entry)
    {
        switch (c)
        {
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        default:
            out += c;
            break;
        }
    }
    return out;
}

std::string UnescapeHistoryLine(const std::string &line)
{
    std::string out;
    out.reserve(line.size());
    for (size_t i = 0; i < line.size(); ++i)
    {
        if (line[i] != '\\')
        {
            out += line[i];
            continue;
        }
        if (i + 1 >= line.size())
            break; // dangling escape at EOF: drop it
        const char n = line[++i];
        switch (n)
        {
        case 'n':
            out += '\n';
            break;
        case 'r':
            out += '\r';
            break;
        case '\\':
            out += '\\';
            break;
        default:
            // Unknown escape: keep both bytes verbatim.
            out += '\\';
            out += n;
            break;
        }
    }
    return out;
}

// The only hint the footer shows: instead of spelling out every key binding in
// the status line, point users at the slash command that lists them. Placed at
// the right end of the footer, after the model name.
constexpr char kFooterHelp[] = "/help";

// Replace a leading $HOME with "~" so a deep directory stays readable in the
// one-line footer. Falls back to the raw path when HOME is unset or the path
// is not under it.
std::string ShortenHome(const char *path)
{
    if (!path || !*path)
        return "";
    const char *home = std::getenv("HOME");
    if (home && *home)
    {
        const size_t n = std::strlen(home);
        if (std::strncmp(path, home, n) == 0 && (path[n] == '/' || path[n] == '\0'))
            return std::string("~") + (path + n);
    }
    return path;
}

} // namespace

TUIWindow::TUIWindow(int sandbox_override, std::shared_ptr<const port::ITerminalInfo> terminal)
    : input_text_(""), terminal_(terminal ? std::move(terminal) : port::TerminalInfo::Shared())
{
    // Models first: the chat view renders them, the dispatcher writes them.
    chat_model_ = std::make_shared<model::ChatModel>();
    session_model_ = std::make_shared<model::SessionModel>();

    chat_widget_ = std::make_shared<ChatWidget>(chat_model_);
    task_widget_ = std::make_shared<TaskWidget>();
    todo_widget_ = std::make_shared<TodoWidget>();
    working_indicator_ = std::make_shared<ui::WorkingIndicatorView>();

    const auto &pal = style::palette();
    prompt_ = ::ftxui::text("❯ ") | ::ftxui::color(pal.prompt) | ::ftxui::bold;

    // Multiline chat draft with soft (visual) wrapping. Long unbroken lines
    // wrap onto following rows instead of overflowing the input width;
    // Alt+Enter / pasted newlines still create real '\n' in the content. The
    // placeholder is dimmed by the widget itself (no InputOption transform).
    input_ = std::make_shared<edc::EdcInput>(&input_text_, "Type a message, Enter to send, Alt+Enter for a newline...");

    container_ = ::ftxui::Container::Vertical({chat_widget_, input_}, &focused_);

    // Left pane = chat + input + footer (see RenderMain); right pane = the task tree
    // (TaskWidget) with the todo list (TodoWidget) stacked under it. Both sections
    // are always present (an empty list still shows its "▾/▸ Todos 0/0" header, so
    // the sidebar is stable from startup) and take only their natural height.
    // ResizableSplit keeps "main" on the right at a drag-adjustable width; the
    // separator is a blank column, so the sidebar stays borderless but grabbable.
    auto main_area = ::ftxui::Renderer(container_, [this] { return RenderMain(); });
    // The sidebar column is a real component (ui::SidebarView, built in
    // edc_sidebar.cpp) so a test can compose the very same layout: both sections
    // fit-content with their headers pinned, the tree anchored to its bottom --
    // that anchoring keeps the active task's row on screen when the todo list has
    // a lot to say.
    sidebar_ = std::make_shared<ui::SidebarView>(task_widget_, todo_widget_);
    auto right_pane = sidebar_->pane();
    split_ = ::ftxui::ResizableSplit({
        right_pane,                         // main (right pane)
        main_area,                          // back (left pane)
        ::ftxui::Direction::Right,          // direction
        &sidebar_width_,                    // main_size
        [] { return ::ftxui::text("  "); }, // subtle separator, drag grab area
    });
    Add(split_);

    // /connect dialog: a self-contained widget (owns its fields + rendering +
    // events). The window only opens it and overlays it; persistence + hot-apply
    // go through this Submit callback (so the dialog itself never sees the app).
    connect_dialog_ = std::make_shared<ui::ConnectDialog>([this](const port::ConnectConfig &cfg) -> bool {
        if (!app_->SetLlmConfig(cfg))
            return false;
        model_ = cfg.model; // refresh the footer's model label
        if (screen_)
            screen_->RequestAnimationFrame();
        return true;
    });

    // The dispatcher is the AppFacade sink; it updates the models and calls back
    // into this window (IChannelHost) for the view actions. Created before the
    // app so the port can register it as its channel sink.
    dispatcher_ = std::make_unique<control::ChannelDispatcher>(*chat_model_, *session_model_, *this);

    // The app port owns the edc_app: it starts the app, registers the channel
    // adapter (translating C messages into typed events, handed to |dispatcher_|
    // as IChannelSink and marshalled through this window as IUiDispatcher), and
    // frees it in its destructor.
    app_ = std::make_unique<port::AppFacade>(sandbox_override, this, dispatcher_.get());

    // AppFacade is not Ready() (after logging the reason) when the app cannot
    // start: unreadable config, undecryptable API key, no terminal for the
    // passphrase, env init failure. Every call below needs it, so stop here.
    // main() sees Ready() == false and exits cleanly.
    if (!app_->Ready())
        return;

    EnableBracketedPaste();
    bracketed_paste_on_ = true;

    // Read the model once for the footer (the app has just loaded its
    // config); the footer itself is built per frame so it can fit the pane.
    app_->ConfigModel(&model_);

    // The transcript store owns the chat-log file I/O (path, append, replay); it
    // needs the app port for the workspace path and the symlink-safe writes.
    transcript_ = std::make_unique<port::TranscriptStore>(*app_);

    // Restore the recall history saved by previous runs (ArrowUp/ArrowDown in the
    // box). Missing file on first run just leaves the box with no history. Read
    // through the app port, so this must come after it is constructed.
    LoadInputHistory();

    // Populate the sidebar once the app is running (its response is routed
    // through the adapter into the dispatcher), and learn from that
    // first snapshot which session we are showing. Its reply carries the
    // session's own transcript path, which the live path below appends to -- so
    // nothing here reads the session store: it belongs to the actor thread, and
    // the snapshot already names what we need.
    RequestTaskSnapshot();
}

void TUIWindow::Submit()
{
    const auto first = input_text_.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
    {
        input_text_.clear();
        return;
    }
    const auto last = input_text_.find_last_not_of(" \t\r\n");
    std::string message = input_text_.substr(first, last - first + 1);

    // "/connect" is handled locally: open the provider dialog and do not send
    // anything to the app or record it as a chat message.
    if (message == "/connect")
    {
        input_text_.clear();
        input_->Reset();
        OpenConnect();
        return;
    }

    // The bubble is immediate and names no task: the input predates its task and
    // nothing here guesses which task it will become. While no selection is
    // active that is all the log needs; under a selection the input is drawn from
    // the task's own raw_input, as the first user message of its history
    // (ChatWidget::SetTaskInputs), so it is hidden here and shown there.
    chat_widget_->AddMessage(std::make_shared<UserMessage>(message, ::time(nullptr)));
    AppendChatTranscript("user", "", message, "", EDC_APP_RENDER_PLAIN);
    input_text_.clear();
    input_->Reset(); // caret/scroll back to the start after the external clear
    // Record the submitted text for ArrowUp recall and persist it so history
    // survives a restart.
    input_->AddHistory(message);
    SaveInputHistory();

    app_->SendUserMessage(session_model_->displayed_id(), message);
    chat_widget_->ScrollToBottom();

    // A new message may spawn/attach a task, so refresh the sidebar tree. The
    // request is queued behind the user message on the scheduler thread, so
    // its snapshot already reflects the new foreground task's state.
    RequestTaskSnapshot();

    // The animated status is not started here and not tied to message types: the
    // snapshot (this one or the next periodic one) flips the spinner through
    // SyncWorkingFromSnapshot(). Ask for a frame so the submitted message paints now.
    if (screen_)
        screen_->RequestAnimationFrame();
}

void TUIWindow::OpenConnect()
{
    // Seed the dialog from the persisted config; a missing field reads empty
    // (matching the pre-portal behaviour).
    port::ConnectConfig initial;
    if (!app_->ConfigApiUrl(&initial.api_url))
        initial.api_url.clear();
    if (!app_->ConfigModel(&initial.model))
        initial.model.clear();
    if (!app_->ConfigApiKey(&initial.api_key))
        initial.api_key.clear();
    connect_dialog_->Open(initial);
    if (screen_)
        screen_->RequestAnimationFrame();
}

void TUIWindow::LoadInputHistory()
{
    std::string path;
    if (!app_->HistoryPath(&path))
        return;

    std::ifstream file(path, std::ios::binary);
    if (!file)
        return; // first run (or unreadable): start with an empty history

    input_->ClearHistory();
    std::string line;
    while (std::getline(file, line))
    {
        if (!line.empty() && line.back() == '\r') // tolerate CRLF files
            line.pop_back();
        if (line.empty())
            continue;
        input_->AddHistory(UnescapeHistoryLine(line));
    }
}

void TUIWindow::SaveInputHistory()
{
    std::string path;
    if (!app_->HistoryPath(&path))
        return;

    std::string out;
    for (const std::string &entry : input_->History())
        out += EscapeHistoryEntry(entry) + "\n";
    // Symlink-safe (the file sits in $EDC_HOME; still never follow a link).
    app_->WriteFile(path, out.data(), out.size(), /*append=*/false, /*under_workspace=*/false);
}

void TUIWindow::RequestTaskSnapshot()
{
    /* 测试模式: 不向 app 要树。理由 —— 构造期与回调里的拉取都会走一个**异步**
     * 回包, 而没 SetScreen() 时那回包是内联在 runner 线程上跑 (Post() 的 else
     * 分支, 或 Post 之前无屏幕的构造期), 会和测试自己的断言抢同一个 widget。
     * 设了它就完全没有这条路 (tests/test_tui_dispatch.cpp)。
     * 本仓既有先例: EDGECLAW_SNAPSHOT_TRACE 也是 env 开关。 */
    if (getenv("EDGECLAW_TUI_NO_PULL"))
        return;
    last_snapshot_request_ = std::chrono::steady_clock::now();
    // 点名叫自己**正在显示**的那棵树 (P3 ①b) —— 别人的树不上路。
    app_->RequestTaskSnapshot(session_model_->displayed_id());
}

// The sidebar's selection (one group or one task) becomes a filter over the chat
// log: only the bubbles belonging to the selected group's / task's ids are drawn.
// No selection = the whole session log, exactly as before. The task ids come from
// Build one chat bubble. Shared by the live channel path and the restart replay
// so a message looks the same whether it just arrived or was re-derived from
// session.json. |kind| is the bridge's tag: user | tool | thought | reply | sys.
static std::shared_ptr<Message> MakeChatWidget(const std::string &kind, const std::string &header,
                                               const std::string &content, const std::string &task_id,
                                               int render_pref, time_t ts)
{
    if (kind == "user")
        return std::make_shared<UserMessage>(content, ts, task_id);

    if (kind == "tool")
    {
        // 工具消息的渲染完全由发送方声明的 render_pref 决定，渲染端零猜测：
        //  - MARKDOWN: 内容按 Markdown 渲染；有 header 时保留 ToolMessage 的
        //    折叠 chrome，无 header 直接进 md 消息样式
        //  - 默认/PLAIN（未声明即纯文本）: 一律纯文本 ToolMessage
        if (render_pref == EDC_APP_RENDER_MARKDOWN)
        {
            if (!header.empty())
                return std::make_shared<ToolMessage>(header, content, ts, /*markdown=*/true, task_id);
            return std::make_shared<Message>(content, ts, task_id);
        }
        return std::make_shared<ToolMessage>(header, content, ts, /*markdown=*/false, task_id);
    }

    if (kind == "thought")
        return std::make_shared<ThoughtMessage>(content, ts, task_id);

    // "reply" | "sys"
    return std::make_shared<Message>(content, ts, task_id);
}

bool SnapshotIsForSession(const TaskSnapshot *snap, const std::string &displayed_session)
{
    if (!snap)
        return false;
    // 规则本身在 domain::SnapshotClaimPolicy 里; 这里只是保留旧入口 (薄委托)。
    return domain::SnapshotClaimPolicy::Default().Claims(*snap, displayed_session);
}

bool TreeChangedEventWantsPull(const std::string &event_session, const std::string &reason,
                               const std::string &displayed_session)
{
    return domain::SnapshotClaimPolicy::Default().WantsPull(event_session, reason, displayed_session);
}

LiveOutputRoute RouteLiveOutput(const std::string &session_id, bool from_active_task,
                                const std::string &displayed_session)
{
    // 旧枚举 (LiveOutputRoute) 与领域枚举 (domain::OutputRoute) 一对一, 映射即可。
    switch (domain::OutputRoutingPolicy::Default().Route(session_id, from_active_task, displayed_session))
    {
    case domain::OutputRoute::Shown:
        return LiveOutputRoute::kShown;
    case domain::OutputRoute::Silent:
        return LiveOutputRoute::kSilent;
    case domain::OutputRoute::OtherSession:
        return LiveOutputRoute::kOtherSession;
    }
    return LiveOutputRoute::kShown;
}

void TUIWindow::LoadChatTranscript(const char *session_id)
{
    chat_widget_->Clear();
    /* The cached workdir belongs to the session being replaced: SessionModel::Reset
     * drops it rather than show a value we already know is wrong. The next snapshot
     * (pulled right after every switch) refills it; until then the footer falls
     * back to the launch directory. */
    const std::string id = session_id ? session_id : "";
    // The transcript belongs to the session, not to the tab: it records what this
    // frontend displayed for that session, so it survives switches and restarts.
    std::string path;
    if (!id.empty())
        transcript_->PathFor(id, &path);
    session_model_->Reset(id, path);

    LoadChatTranscriptFile(session_model_->chat_log_path());
}

void TUIWindow::LoadChatTranscriptFile(const std::string &path)
{
    const size_t entries = transcript_->Replay(path, [this](const ChatTranscriptEntry &entry) {
        const std::shared_ptr<Message> widget = MakeChatWidget(entry.kind, entry.header, entry.content, entry.task_id,
                                                               entry.render_pref, ::time(nullptr));
        if (widget)
            chat_widget_->AddMessage(widget);
    });
    if (entries > 0)
        chat_widget_->ScrollToBottom();
}

void TUIWindow::AppendChatTranscript(const std::string &kind, const std::string &header, const std::string &content,
                                     const std::string &task_id, int render_pref)
{
    AppendChatTranscriptTo(session_model_->chat_log_path(), kind, header, content, task_id, render_pref);
}

void TUIWindow::AppendChatTranscriptTo(const std::string &path, const std::string &kind, const std::string &header,
                                       const std::string &content, const std::string &task_id, int render_pref)
{
    ChatTranscriptEntry entry;
    entry.kind = kind;
    entry.render_pref = render_pref;
    entry.task_id = task_id;
    entry.header = header;
    entry.content = content;

    // The store owns the file details: parent dir, the half-written-line repair,
    // and the symlink-safe append (see TranscriptStore::Append).
    transcript_->Append(path, entry);
}

void TUIWindow::SyncWorkingFromSnapshot(const TaskSnapshot *snap)
{
    const bool busy = ForegroundProcessBusy(snap);
    if (busy == working_)
        return;
    working_ = busy;
    if (busy)
    {
        // The view owns the timing: restart its clock on the busy transition
        // (the run always starts at the first cell).
        working_indicator_->Restart();
        // Start the spinner's cadence from the frame requested just below.
        last_spinner_frame_ = std::chrono::steady_clock::now();
    }
    // A transition here can happen while the render loop is otherwise idle
    // (e.g. the first snapshot after a submit starts the animation), so ask
    // for a frame to repaint the footer/spinner row.
    if (screen_)
        screen_->RequestAnimationFrame();
}

void TUIWindow::TickAnimations()
{
    // The spinner must not ask for a frame from inside its own render: that pins
    // the whole TUI to the loop's rate for as long as a task runs. Its repaints
    // are paced from this once-per-iteration hook (see main.cpp) instead.
    if (!screen_)
        return;

    const auto now = std::chrono::steady_clock::now();

    // The sidebar's breathing (a processing task's marker) runs on the working
    // bar's tempo. Feed it the clock here, off the render path; a run that starts
    // after nothing was breathing begins at the dim end and blooms in.
    //
    // The task tree answers for itself (its rows carry the state). The todo list
    // is deliberately *not* animated: its in_progress row is a static highlight
    // whose colour comes from the owning task's state, so it is set with the items
    // (see the snapshot handler) rather than fed a clock here.
    const bool processing = task_widget_ && task_widget_->HasProcessingTask();
    if (processing && !dot_breathing_)
        breath_started_ = now;
    dot_breathing_ = processing;
    const auto breath_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - breath_started_).count();

    if (task_widget_)
        task_widget_->SetBreath(breath_ms);

    // A background task can process while the foreground process is idle, so the
    // sidebar's breathing, not just the spinner, keeps requesting frames.
    if (!working_ && !processing)
        return;

    const auto since = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_spinner_frame_).count();
    if (since < kSpinnerFrameMs)
        return;

    last_spinner_frame_ = now;
    screen_->RequestAnimationFrame();
}

::ftxui::Element TUIWindow::RenderMain()
{
    auto input_row = ::ftxui::hbox({prompt_, input_->Render() | ::ftxui::flex});

    // The status row shares this pane's width (the input's), so the right-aligned
    // model + "/help" stop at the input's right edge and never spill into the
    // sidebar. The "Copied to clipboard" chip floats over the chat pane's own
    // top-right corner as an overlay, and asking for a frame while it is up is what
    // keeps the fade moving (an idle screen is not redrawn otherwise).
    auto chat = chat_widget_->Render();
    const auto toast_now = std::chrono::steady_clock::now();
    if (clipboard_toast_.Visible(toast_now))
    {
        chat = ::ftxui::dbox({std::move(chat), ClipboardToastElement(clipboard_toast_.Fade(toast_now))});
        if (screen_)
            screen_->RequestAnimationFrame();
    }

    // 只看模式: 输入组件**整块不渲染** (连它那两行分隔线一起) —— 面板只剩历史 + 状态行。
    // 不拿一行文字去顶输入框的位置: 那样看着像"输入框还在, 只是被禁用", 比禁用它更怪。
    // 返回提示做成历史面板右上角的一枚徽标 (与 "Copied" 芯片同一个 dbox 手法)。

    ::ftxui::Elements children = {chat | ::ftxui::flex, ::ftxui::separator()};
    if (!history_view_)
    {
        children.push_back(input_row);
        children.push_back(::ftxui::separator());
    }

    // 只看模式: 输入组件整块不在屏上, 返回提示就放底部这一行 (与状态行同一个位置)。
    // 输入框原来的位置上**什么都不摆** —— 摆一行字反而像"输入框被禁用了"。
    if (history_view_)
    {
        children.push_back(::ftxui::hbox({::ftxui::text(" History · Esc to go back ") |
                                          ::ftxui::color(style::palette().accent)}));
        children.push_back(::ftxui::text(""));
        return ::ftxui::vbox(std::move(children));
    }

    if (working_)
    {
        // While the foreground group's active task is running the animated bar
        // replaces the footer line (so the bottom row keeps its height); as
        // soon as a snapshot reports it is no longer busy, the footer returns.
        // The view owns the timing (started on the busy transition); repaints
        // are paced by TickAnimations(), not requested here.
        children.push_back(working_indicator_->Render());
    }
    else
    {
        children.push_back(RenderFooter());
    }

    // A blank line under the status row so its text does not sit flush against
    // the terminal's bottom edge (which reads as cramped). Kept in both
    // branches, so swapping the footer for the working bar still never changes
    // the layout height.
    children.push_back(::ftxui::text(""));

    return ::ftxui::vbox(std::move(children));
}

::ftxui::Element TUIWindow::RenderFooter() const
{
    const auto &pal = style::palette();

    char cwd_buf[4096];
    /* The active session's bound working directory (what the model is told via
     * {{cwd}} and where tools run), cached from the last applied snapshot so the
     * footer never reads the store; fall back to the process launch directory
     * when the session is unbound, or before the first snapshot lands. */
    const char *dir_src = !session_model_->workdir().empty()
                              ? session_model_->workdir().c_str()
                              : (::getcwd(cwd_buf, sizeof(cwd_buf)) ? cwd_buf : nullptr);
    const std::string dir = dir_src ? ShortenHome(dir_src) : std::string();
    const std::string left = " " + dir;

    // Columns available to the footer: the chat pane, i.e. the terminal minus
    // the sidebar and the two-cell split separator. The footer never uses more
    // than this, so its right-aligned model/"help" stop at the input's right
    // edge instead of running under the sidebar. The width comes from the
    // injected terminal port (the real tty by default, a fixed one in tests).
    const int term_w = terminal_->width();
    const int pane_w = term_w - sidebar_width_ - 2;

    // Right end: the model in use, the sandbox state and the "/help" pointer,
    // joined by middle dots so they read as one compact unit -- model first,
    // then the sandbox state, then "/help". Flush to the pane's right edge with
    // the filler; when the pane is narrow drop fields from the middle outward:
    // first the model, then the sandbox state, leaving "/help" alone.
    const int budget = pane_w - ::ftxui::string_width(left) - 1;
    const std::string sandbox = std::string("sandbox:") + (app_->SandboxEnabled() ? "on" : "off");

    std::string right = kFooterHelp;
    const std::string sandbox_help = sandbox + " · " + kFooterHelp;
    if (::ftxui::string_width(sandbox_help) <= budget)
        right = sandbox_help;
    if (!model_.empty())
    {
        const std::string full = model_ + " · " + sandbox + " · " + kFooterHelp;
        if (::ftxui::string_width(full) <= budget)
            right = full;
    }

    return ::ftxui::hbox({
        ::ftxui::text(left) | ::ftxui::color(pal.text_dim),
        ::ftxui::filler(),
        ::ftxui::text(right + " ") | ::ftxui::color(pal.text_dim),
    });
}

::ftxui::Element TUIWindow::OnRender()
{
    // The drag that just ended copies on this frame, not on the release itself:
    // FTXUI rebuilds its selection *while* a frame is drawn, so the text under
    // the cursor only settles once the frame the release belongs to has been
    // rendered.
    if (copy_pending_)
    {
        copy_pending_ = false;
        CopySelectionToClipboard();
    }

    // 这里以前有个 500ms 的轮询: 后台任务不是 active task, 状态变化没有任何
    // 消息, 只能靠它兜底。现在调度器对**任何** task 的状态变化都推 (合并到每条
    // actor 消息一次), 所以删掉了 —— 而且它本来也不可靠: 它写在 Render() 里,
    // ftxui 只在有帧请求时才调 Render, 空闲时没有帧就是没有轮询。

    auto base = split_->Render() | ::ftxui::bgcolor(style::palette().window_bg);
    if (!connect_dialog_->IsOpen())
        return base;
    // The dialog is borderless, so the backdrop is dimmed while it is open: the
    // raised surface then reads as the one lit panel on a muted screen.
    // clear_under is load-bearing: ftxui's `dim` is a per-cell flag the backdrop
    // stamps over its box and an overlaid element never clears it, so without it the
    // dialog renders gray whatever its text colour.
    return ::ftxui::dbox({base | ::ftxui::dim, connect_dialog_->Render() | ::ftxui::clear_under |
                                                   ::ftxui::size(::ftxui::WIDTH, ::ftxui::LESS_THAN, 80) |
                                                   ::ftxui::center});
}


bool TUIWindow::OnEvent(::ftxui::Event event)
{
    // A drag is copied when it is let go, not while it is being made: mid-drag
    // the selection is still moving, and a click has to stay a click. This is an
    // observation only -- returning true here would make FTXUI cancel the
    // selection outright (see HandleSelection: a handled mouse event clears it),
    // so the event still has to travel on to the rest of the tree.
    if (event.is_mouse() && event.mouse().button == ::ftxui::Mouse::Left)
    {
        if (event.mouse().motion == ::ftxui::Mouse::Pressed)
        {
            press_x_ = event.mouse().x;
            press_y_ = event.mouse().y;
        }
        else if (event.mouse().motion == ::ftxui::Mouse::Released &&
                 IsSelectionDrag(press_x_, press_y_, event.mouse().x, event.mouse().y))
        {
            copy_pending_ = true;
            if (screen_)
                screen_->RequestAnimationFrame();
        }
    }

    // The /connect dialog is a modal: while open it owns every event (Esc closes,
    // Enter submits, the rest go to its fields). Checked first -- it is the
    // innermost thing on screen, so it takes the event before any input mode.
    if (connect_dialog_->IsOpen())
        return connect_dialog_->OnEvent(event);

    // Bracketed paste markers (ESC[200~ / ESC[201~) toggle the paste mode and are
    // consumed whatever the mode was. FTXUI delivers each marker as a single
    // Event::Special.
    static const auto kPasteStart = ::ftxui::Event::Special("\x1b[200~");
    static const auto kPasteEnd = ::ftxui::Event::Special("\x1b[201~");
    if (event == kPasteStart)
    {
        in_paste_ = true;
        return true;
    }
    if (event == kPasteEnd)
    {
        in_paste_ = false;
        return true;
    }

    // Explicit input modes replace the old implicit if-ordering (see inputMode()).
    switch (inputMode())
    {
    case InputMode::Paste:
        return HandlePasteEvent(event);
    case InputMode::History:
        // Read-only: Esc returns to the live stream; characters and Return are
        // swallowed (the box is off screen). Mouse/wheel still fall through so the
        // history can be scrolled.
        if (HandleHistoryEvent(event))
            return true;
        return HandleNormalEvent(event);
    case InputMode::Normal:
        return HandleNormalEvent(event);
    }
    return false;
}

bool TUIWindow::HandlePasteEvent(::ftxui::Event event)
{
    // Everything inside a paste is text. Return/newline becomes a line break;
    // other characters flow straight through to the Input (cursor movement and
    // UTF-8). Nothing here submits.
    if (event == ::ftxui::Event::Return)
    {
        input_->OnEvent(::ftxui::Event::Character("\n"));
        return true;
    }
    return input_->OnEvent(event);
}

bool TUIWindow::HandleHistoryEvent(::ftxui::Event event)
{
    if (event == ::ftxui::Event::Escape)
    {
        ExitHistoryView();
        return true;
    }
    if (event.is_character() || event == ::ftxui::Event::Return)
        return true; // swallowed: the input box is off screen
    return false;    // mouse / wheel / Alt+Enter -> normal handling
}

bool TUIWindow::HandleNormalEvent(::ftxui::Event event)
{
    // Alt+Enter arrives as ESC-prefixed Return: the terminal (raw mode, ICRNL off)
    // sends CR for Enter, and Alt adds the ESC prefix, so the parser emits a single
    // Special({27, 13}). ESC+LF is accepted too for terminals that send LF. Insert a
    // newline via a synthetic character event, which reuses the Input's cursor and
    // UTF-8 handling without firing on_enter.
    static const auto kAltEnterCR = ::ftxui::Event::Special(std::string({27, 13}));
    static const auto kAltEnterLF = ::ftxui::Event::Special(std::string({27, 10}));
    if (event == kAltEnterCR || event == kAltEnterLF)
    {
        input_->OnEvent(::ftxui::Event::Character("\n"));
        return true;
    }

    // Plain Enter submits. Intercepting it here means it never reaches the Input,
    // so multiline stays purely for Alt+Enter and typed text.
    if (event == ::ftxui::Event::Return)
    {
        Submit();
        return true;
    }

    return split_->OnEvent(event);
}

TUIWindow::~TUIWindow()
{
    // app_ (a member) frees the edc_app: "destroy = persist" lives inside
    // AppFacade's destructor, which runs right after this body.
    if (bracketed_paste_on_)
        DisableBracketedPaste();
}

void TUIWindow::SetScreen(::ftxui::ScreenInteractive *screen)
{
    screen_ = screen;
}

void TUIWindow::CopySelectionToClipboard()
{
    if (!screen_)
        return;

    const std::string selected = screen_->GetSelection();
    if (selected.empty())
        return;

    CopyToClipboard(selected);

    // The write is invisible, so acknowledge it on screen.
    clipboard_toast_.Raise(std::chrono::steady_clock::now());
    screen_->RequestAnimationFrame();
}

/* 历史载荷里的一条消息 → 一个气泡。kind/header 的映射与实时流一致 (user | tool |
 * reply | sys), render 字段决定 Markdown 与否 (与 edc_turn_add_message 写下的偏好同一个)。 */
static void AppendHistoryBubbles(const cJSON *messages, const std::string &task_id,
                                 std::vector<std::shared_ptr<Message>> *out)
{
    const cJSON *m = NULL;
    cJSON_ArrayForEach(m, messages)
    {
        const char *role = cJSON_GetStringValue(cJSON_GetObjectItem(m, "role"));
        const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(m, "name"));
        const char *body = cJSON_GetStringValue(cJSON_GetObjectItem(m, "content"));
        const char *render = cJSON_GetStringValue(cJSON_GetObjectItem(m, "render"));
        const int render_pref = (render && strcmp(render, "markdown") == 0) ? EDC_APP_RENDER_MARKDOWN : EDC_APP_RENDER_PLAIN;
        const cJSON *ts_json = cJSON_GetObjectItem(m, "ts");
        const time_t ts = (ts_json && cJSON_IsNumber(ts_json)) ? (time_t)ts_json->valuedouble : 0;

        std::string kind = "sys";
        std::string header;
        if (role && strcmp(role, "user") == 0)
        {
            kind = "user";
        }
        else if (role && strcmp(role, "assistant") == 0)
        {
            kind = "reply";
        }
        else if (role && strcmp(role, "tool") == 0)
        {
            kind = "tool";
            header = name ? name : "";
        }
        else if (role)
        {
            header = role; // reasoning/其它: 当 sys 行, 报出它是什么
        }

        std::string text = body ? body : "";
        const cJSON *calls = cJSON_GetObjectItem(m, "tool_calls");
        if (text.empty() && calls && cJSON_GetArraySize(calls) > 0)
        {
            /* 空的 assistant 消息 (只有发起工具调用): 画一行"调用了什么", 倒参数没有意义。 */
            kind = "sys";
            header.clear();
            text = "→ ";
            const cJSON *call = NULL;
            cJSON_ArrayForEach(call, calls)
            {
                const cJSON *fn = cJSON_GetObjectItem(call, "function");
                const char *fn_name = fn ? cJSON_GetStringValue(cJSON_GetObjectItem(fn, "name")) : NULL;
                text += (fn_name ? fn_name : "tool");
                text += " ";
            }
        }
        out->push_back(MakeChatWidget(kind, header, text, task_id, render_pref, ts));
    }
}

void TUIWindow::ExitHistoryView()
{
    if (!history_view_)
        return;
    history_view_ = false;
    chat_widget_->Clear();
    const std::string id = session_model_->displayed_id();
    LoadChatTranscript(id.empty() ? nullptr : id.c_str());
    if (screen_)
        screen_->RequestAnimationFrame();
}

void TUIWindow::RenderTaskHistory(const std::string &payload)
{
    cJSON *root = cJSON_Parse(payload.c_str());
    if (!root)
        return;

    std::vector<std::shared_ptr<Message>> bubbles;
    const char *kind = cJSON_GetStringValue(cJSON_GetObjectItem(root, "kind"));
    const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(root, "id"));
    const std::string sid = id ? id : "?";
    const bool group = kind && strcmp(kind, "group") == 0;

    if (group)
    {
        const cJSON *block = NULL;
        cJSON_ArrayForEach(block, cJSON_GetObjectItem(root, "tasks"))
        {
            const char *block_id = cJSON_GetStringValue(cJSON_GetObjectItem(block, "id"));
            const cJSON *msgs = cJSON_GetObjectItem(block, "messages");
            const char *input = cJSON_GetStringValue(cJSON_GetObjectItem(block, "input"));
            std::string head = "— task " + std::string(block_id ? block_id : "?") + " · " +
                               std::to_string(cJSON_GetArraySize(msgs)) + " messages";
            if (input && *input)
                head += " · input: " + std::string(input);
            head += " —";
            bubbles.push_back(MakeChatWidget("sys", "", head, "", EDC_APP_RENDER_PLAIN, 0));
            AppendHistoryBubbles(msgs, block_id ? block_id : "", &bubbles);
        }
    }
    else
    {
        const cJSON *msgs = cJSON_GetObjectItem(root, "messages");
        const char *input = cJSON_GetStringValue(cJSON_GetObjectItem(root, "input"));
        std::string head = "history of task " + sid + " · " + std::to_string(cJSON_GetArraySize(msgs)) + " messages";
        if (input && *input)
            head += " · input: " + std::string(input);
        bubbles.push_back(MakeChatWidget("sys", "", head, "", EDC_APP_RENDER_PLAIN, 0));
        AppendHistoryBubbles(msgs, sid, &bubbles);
    }
    cJSON_Delete(root);

    /* 只看模式: 这份历史**替换**对话区 (不是追加), 之后实时产出不再上屏, 直到 Esc。 */
    chat_widget_->Clear();
    for (const auto &bubble : bubbles)
        chat_widget_->AddMessage(bubble);
    history_view_ = true;
    screen_ ? screen_->RequestAnimationFrame() : void();
}

// ── IUiDispatcher + IChannelHost 实现 ───────────────────────────────────────
//
// 事件本身由 control::ChannelDispatcher 承接 (判断 + 更新模型); 本窗口只做
// 两件事: 把任务 marshal 到 UI 线程 (IUiDispatcher), 以及分派器回调的"模型
// 之外的视图动作" (IChannelHost) —— 转录重放/落盘、侧栏刷新、动画、重绘、拉树。
// 全部运行在 UI 线程: 生产由 AppFacade 经 Post() marshal, 测试无屏幕时 Post()
// 同步执行 (正是在这个分支上, harness 的喂消息会立刻跑到分派器)。

void TUIWindow::Post(std::function<void()> task)
{
    if (screen_)
        screen_->Post(std::move(task));
    else
        task(); // 没有屏幕 (构造期 / harness): 同步执行
}

void TUIWindow::RequestFrame()
{
    if (screen_)
        screen_->RequestAnimationFrame();
}

void TUIWindow::PresentHistory(const std::string &payload)
{
    // 结构化历史载荷: 拆成一条条气泡追加到对话区 (见 RenderTaskHistory)。
    RenderTaskHistory(payload);
}

void TUIWindow::LoadSessionTranscript(const std::string &session_id)
{
    LoadChatTranscript(session_id.c_str());
}

void TUIWindow::SetTaskTree(const TaskSnapshot &snap)
{
    task_widget_->SetSnapshot(&snap);
}

void TUIWindow::SyncWorking(const TaskSnapshot &snap)
{
    // The snapshot is the single source of truth for the status animation.
    SyncWorkingFromSnapshot(&snap);
}

void TUIWindow::SetTodos(const TaskSnapshot &snap)
{
    // The todo section shows the foreground group's active task's own todos (part
    // of the task state): the task id names whose list it is, and the task's state
    // decides the failure colour — read from the very same snapshot so the two
    // cannot disagree.
    const ActiveTaskTodos todos = ActiveTaskTodosOf(&snap);
    todo_widget_->SetItems(todos.items, todos.task_id);
    todo_widget_->SetOwnerState(ForegroundActiveTaskState(&snap));
}

bool TUIWindow::PresentChat(const port::ChatMessageEvent &event, const std::string &kind_str)
{
    // A background session's output must not appear in the tab that is currently
    // displayed — but it is still part of *that* session's screen history, so it
    // goes into that session's own transcript and shows up when the user switches
    // back. The current tab (and its sidebar) is left untouched.
    if (!event.session_id.empty() && event.session_id != session_model_->displayed_id())
    {
        std::string path;
        if (transcript_->PathFor(event.session_id, &path))
            AppendChatTranscriptTo(path, kind_str, event.header, event.content, event.task_id, event.render_pref);
        return false;
    }

    // Persist exactly what is shown, so the next start replays this log.
    AppendChatTranscript(kind_str, event.header, event.content, event.task_id, event.render_pref);

    if (std::shared_ptr<Message> widget =
            MakeChatWidget(kind_str, event.header, event.content, event.task_id, event.render_pref, event.ts))
        chat_widget_->AddMessage(widget);

    // No explicit ScrollToBottom() here: if the reader is already at the tail the
    // view follows it automatically; if they scrolled up to read history, arriving
    // messages must not yank them back down. The caller (the dispatcher) then
    // refreshes the sidebar and asks for a frame.
    return true;
}
} // namespace edc
