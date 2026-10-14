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
#include "edc_status_bar_view.hpp"
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
#include "edc_api.h" /* EDC_APP_RENDER_PLAIN / _MARKDOWN */
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

// Cadence of the frames that drain the chat log's post-resize re-measure (see
// TickAnimations). The work is invisible cleanup (off-screen rows only), so it
// runs on the spinner's tempo: a drag supplies its own frames anyway, and this
// only has to keep them coming until the log's scroll geometry has settled.
constexpr long long kRemeasureFrameMs = 33;

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
    // The status row reads the session model + terminal width; the sandbox state
    // and the sidebar width are pulled per frame (both change at runtime).
    status_bar_ = std::make_shared<ui::StatusBarView>(session_model_, terminal_,
                                                      [this] { return app_ && app_->SandboxEnabled(); },
                                                      [this] { return sidebar_width_; });

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
        status_bar_->SetModel(cfg.model); // refresh the footer's model label
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

    // Read the model once for the footer (the app has just loaded its config);
    // the footer itself is rebuilt per frame so it can fit the pane.
    {
        std::string model;
        if (app_->ConfigModel(&model))
            status_bar_->SetModel(model);
    }

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
    /* Test mode: do not pull the tree. Pulls from the constructor and from
     * callbacks arrive as an async reply that, with no SetScreen(), runs inline on
     * the runner thread (Post()'s else branch) and races the test's own assertions
     * over the same widget. Setting this removes that path entirely
     * (tests/test_tui_dispatch.cpp). */
    if (getenv("EDGECLAW_TUI_NO_PULL"))
        return;
    last_snapshot_request_ = std::chrono::steady_clock::now();
    // Name the tree we are currently displaying; another session's tree is not ours.
    app_->RequestTaskSnapshot(session_model_->displayed_id());
}

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
        // Rendering is entirely decided by the sender's render_pref:
        //  - MARKDOWN: render the content as Markdown; keep ToolMessage's collapse
        //    chrome when there is a header, otherwise use the plain md style;
        //  - default/PLAIN (undeclared = plain text): a plain-text ToolMessage.
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
    // The rule lives in domain::SnapshotClaimPolicy; this is a thin legacy entry.
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
    // The legacy enum (LiveOutputRoute) maps one-to-one onto domain::OutputRoute.
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

    // The chat log is the other thing that finishes work off the render path: a
    // sidebar drag re-measures only the rows on screen and leaves the rest as
    // estimates (see MessageList). Keep a throttled frame coming until that
    // drains, so the scroll geometry settles right after the drag instead of the
    // next time the reader happens to scroll.
    if (chat_widget_ && chat_widget_->NeedsRemeasure())
    {
        const auto since_remeasure =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - last_remeasure_frame_).count();
        if (since_remeasure >= kRemeasureFrameMs)
        {
            last_remeasure_frame_ = now;
            screen_->RequestAnimationFrame();
        }
    }

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

    // History view: the input is not rendered at all (nor its two separators) --
    // the pane is just history + status row. The return hint is a badge in the
    // history pane's top-right corner (same dbox trick as the "Copied" chip).

    ::ftxui::Elements children = {chat | ::ftxui::flex, ::ftxui::separator()};
    if (!history_view_)
    {
        children.push_back(input_row);
        children.push_back(::ftxui::separator());
    }

    // History view: the input is off screen, so the return hint takes the bottom
    // row (the status row's slot); nothing is drawn where the input was.
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
        children.push_back(status_bar_->Render());
    }

    // A blank line under the status row so its text does not sit flush against
    // the terminal's bottom edge (which reads as cramped). Kept in both
    // branches, so swapping the footer for the working bar still never changes
    // the layout height.
    children.push_back(::ftxui::text(""));

    return ::ftxui::vbox(std::move(children));
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

    // The old 500ms poll (a background task emits no message, so it was the only
    // way to catch its state changes) is gone: the scheduler now pushes every
    // task's state change, coalesced once per actor message.

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

/* One message from the history payload -> one bubble. kind/header map the same as
 * the live stream (user | tool | reply | sys); the render field decides Markdown. */
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
            header = role; // reasoning/other: draw as a sys row, naming it
        }

        std::string text = body ? body : "";
        const cJSON *calls = cJSON_GetObjectItem(m, "tool_calls");
        if (text.empty() && calls && cJSON_GetArraySize(calls) > 0)
        {
            /* Empty assistant message (only tool calls): draw "called what". */
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

    /* History view: this replaces the chat pane; live output stops until Esc. */
    chat_widget_->Clear();
    for (const auto &bubble : bubbles)
        chat_widget_->AddMessage(bubble);
    history_view_ = true;
    screen_ ? screen_->RequestAnimationFrame() : void();
}

// IUiDispatcher + IChannelHost implementation.
//
// Events are handled by control::ChannelDispatcher; this window only does two
// things: marshal tasks onto the UI thread (IUiDispatcher), and perform the
// dispatcher's out-of-model view actions (IChannelHost) -- transcript replay and
// writes, sidebar refresh, animation, repaint, tree pull. Everything runs on the
// UI thread: production marshals through Post(); with no screen Post() runs the
// task synchronously.

void TUIWindow::Post(std::function<void()> task)
{
    if (screen_)
        screen_->Post(std::move(task));
    else
        task(); // no screen (construction / harness): run synchronously
}

void TUIWindow::RequestFrame()
{
    if (screen_)
        screen_->RequestAnimationFrame();
}

void TUIWindow::PresentHistory(const std::string &payload)
{
    // Structured history payload: split into bubbles appended to the chat pane.
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
