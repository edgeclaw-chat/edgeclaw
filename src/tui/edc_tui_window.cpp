#include "edc_tui_window.hpp"
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
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <edc_channel_adapter.h>
#include <fstream>
#include <memory>
#include <string>
#include <sys/ioctl.h>
#include <tui/edc_c_bridge.h>
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

// todowrite 工具的 TOOL 消息头（与 edc_tools_todo.c 注册名保持一致）。
// 该工具的文本回显（Markdown checklist）在 TUI 里不进对话消息列表——TODO
// 状态只由右侧 TodoWidget 展示，其数据源是任务快照里 active task 的 todo
// （快照由 scheduler 在每次工具回显后主动补推），聊天区重复一条 checklist
// 纯属噪音；文本仍随消息传递，供无侧栏的渲染端（CLI）与 tool result（LLM）
// 使用。
constexpr char kTodoToolHeader[] = "todowrite";

// The animated "working" row must reflect the scheduler's process state, not
// the channel-message stream: it spins exactly while the current foreground
// process — the task marked active in the foreground group (fg) of the
// snapshot — is running (LLM generating or a tool chain in flight). Other
// members of the same group, background groups, completed tasks, and tasks
// merely waiting for input/deps never count as busy, so a slash command
// answered only with SYS (no RESP, no foreground task) stops the spinner as
// soon as its snapshot arrives, and a background task's traffic can neither
// start nor stop it.
bool ForegroundProcessBusy(const ::edc_c_task_snapshot *snap)
{
    if (!snap)
        return false;

    for (int gi = 0; gi < snap->group_count; gi++)
    {
        const ::edc_c_task_group *g = &snap->groups[gi];
        if (!g->fg)
            continue;
        // Only the foreground group's active task counts as the foreground
        // process: a busy sibling in the same group must not light the
        // spinner up.
        for (int ti = 0; ti < g->task_count; ti++)
        {
            const ::edc_c_task_info *t = &g->tasks[ti];
            if (!t->is_active)
                continue;
            return t->state &&
                   (std::strcmp(t->state, "LLM_PROCESSING") == 0 || std::strcmp(t->state, "TOOL_CALLING") == 0);
        }
        return false; // foreground group exists but has no active task marked
    }
    return false; // no foreground group at all
}

// The "working" row: a bar of blocks that sweeps back and forth — left to
// right, then right to left — while the brightness inside the lit run flows
// along it, so the row reads as moving water rather than a static progress
// bar. The run does not turn around while it is still on screen: it travels
// until it has left the bar completely, tail and all, and only then comes
// back, so the light enters at one edge, crosses, leaves, and returns.
//
// Constraints kept from every earlier attempt: exactly one text line (the row
// replaces the footer, so it must never change the layout height), a fixed
// number of cells (never change the width), no background fill, and medium
// block glyphs — not the full-height █▓▒░, which tower over the surrounding
// text.
constexpr int kBarCells = 10;      // cells in the bar
constexpr int kBarLit = 5;         // lit cells: the sliding run
constexpr long long kStepMs = 120; // per cell of travel
// The run slides from completely past the left edge to completely past the
// right edge before the sweep reverses, so the ends of the bar are genuine
// turning points (a few frames of empty bar) rather than bumpers.
constexpr int kRunFirst = -kBarLit; // run fully off the left edge
constexpr int kRunLast = kBarCells; // run fully off the right edge
constexpr int kRunPositions = kRunLast - kRunFirst + 1;
// First frame: the run fully on screen at the left, so the row looks busy
// straight away instead of opening on an empty bar.
constexpr int kRunStart = 0;

// Where the lit run sits at |elapsed_ms|, and which way it is travelling.
struct BarState
{
    int pos;      // leftmost lit cell; may be negative or past the right edge
    bool forward; // true while the run sweeps right
};

// The run slides out past one edge — tail included — then comes back in from
// that same edge and crosses to the other side, where the same thing happens
// again (ping-pong): the step index within the cycle maps onto the outward
// leg, then mirrors on the way home, so consecutive frames never jump and the
// turning points are not held for an extra frame. Integer arithmetic on
// purpose: a float phase can land between two cells at the turning points and
// make the run stutter.
BarState BarAt(long long elapsed_ms)
{
    const long long step = (elapsed_ms < 0 ? 0 : elapsed_ms) / kStepMs; // clamp: clock skew
    const long long cycle = 2 * (kRunPositions - 1);
    const long long phase = (step + (kRunStart - kRunFirst)) % cycle;
    if (phase < kRunPositions)
        return {kRunFirst + static_cast<int>(phase), true};
    return {kRunFirst + static_cast<int>(cycle - phase), false};
}

// The depth of the block |i| cells into the lit run. The leading edge — the
// edge the run is travelling towards — is the brightest, and the run fades
// away behind it, so the gradient always points the way the light moves and
// swaps ends when the sweep turns around. Every cell the run passes over
// therefore changes depth as it goes by, which is what makes the bar read as
// water flowing rather than a slab sliding.
int FlowTier(int i, bool forward)
{
    return forward ? (kBarLit - 1 - i) : i;
}

// A lit block, shaded by depth: the terminal cannot fade opacity, so the ramp
// is a handful of discrete steps from a bold accent head down to a barely
// there gray — the closest text-mode equivalent of a brightness keyframe.
// Same glyph at every depth: only the colour intensity changes.
::ftxui::Element LitBlock(int tier, const style::Palette &pal)
{
    switch (tier)
    {
    case 0:
        return ::ftxui::text("■") | ::ftxui::color(pal.accent) | ::ftxui::bold;
    case 1:
        return ::ftxui::text("■") | ::ftxui::color(pal.accent);
    case 2:
        return ::ftxui::text("■") | ::ftxui::color(pal.accent) | ::ftxui::dim;
    case 3:
        return ::ftxui::text("■") | ::ftxui::color(pal.text_dim);
    default:
        return ::ftxui::text("■") | ::ftxui::color(pal.text_dim) | ::ftxui::dim;
    }
}

// An unlit cell: the small square that keeps the bar's full length visible, so
// the eye can see the run move rather than a floating blob.
::ftxui::Element OffBlock(const style::Palette &pal)
{
    return ::ftxui::text("⬝") | ::ftxui::color(pal.text_dim) | ::ftxui::dim;
}

// Todo sidebar content: the foreground group's active task's own todo list.
// The list is part of the task state (todowrite attaches it to the
// task that called it), so the snapshot — which already tells us which task
// is active — is the single source of truth for the TodoWidget, just as it
// is for the task tree and the working spinner. A task without a list (or
// the fg group without an active task) renders an empty TodoWidget.
std::vector<TodoWidget::Item> ActiveTaskTodoItems(const ::edc_c_task_snapshot *snap)
{
    std::vector<TodoWidget::Item> items;
    if (!snap)
        return items;

    for (int gi = 0; gi < snap->group_count; gi++)
    {
        const ::edc_c_task_group *g = &snap->groups[gi];
        if (!g->fg)
            continue;
        for (int ti = 0; ti < g->task_count; ti++)
        {
            const ::edc_c_task_info *t = &g->tasks[ti];
            if (!t->is_active)
                continue;
            items.reserve(static_cast<size_t>(t->todo_count));
            for (int i = 0; i < t->todo_count; i++)
            {
                TodoWidget::Item item;
                item.content = t->todo_items[i].content ? t->todo_items[i].content : "";
                item.status = t->todo_items[i].status ? t->todo_items[i].status : "";
                item.priority = t->todo_items[i].priority ? t->todo_items[i].priority : "";
                items.push_back(std::move(item));
            }
            return items;
        }
        return items; // foreground group exists but has no active task marked
    }
    return items; // no foreground group at all
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
constexpr char kFooterHelp[] = "/help 帮助";

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

// The animated "working" row: the sweeping block bar on its own, no text.
// Defined here (outside the anonymous namespace, declared in the header) so
// tests can render the row for a given elapsed time without a live agent. The
// bar has a fixed number of cells, so showing it in place of the footer never
// changes the row's width, and it is one line tall, so it never changes the
// layout either.
::ftxui::Element WorkingIndicatorElement(long long elapsed_ms)
{
    const auto &pal = style::palette();

    // BarAt clamps a negative elapsed (a clock adjustment between the start
    // stamp and the frame must not rewind the bar).
    const BarState bar = BarAt(elapsed_ms);

    // The bar is the whole row: the animation alone says "working", so the
    // row carries no label and its width is just the bar's.
    ::ftxui::Elements row;
    row.reserve(static_cast<size_t>(kBarCells));
    for (int i = 0; i < kBarCells; i++)
    {
        if (i >= bar.pos && i < bar.pos + kBarLit)
            row.push_back(LitBlock(FlowTier(i - bar.pos, bar.forward), pal));
        else
            row.push_back(OffBlock(pal));
    }
    return ::ftxui::hbox(std::move(row));
}

TUIWindow::TUIWindow(int sandbox_override) : input_text_("")
{
    chat_widget_ = std::make_shared<ChatWidget>();
    task_widget_ = std::make_shared<TaskWidget>();
    todo_widget_ = std::make_shared<TodoWidget>();

    const auto &pal = style::palette();
    prompt_ = ::ftxui::text("❯ ") | ::ftxui::color(pal.prompt) | ::ftxui::bold;

    // Multiline chat draft with soft (visual) wrapping. Long unbroken lines
    // wrap onto following rows instead of overflowing the input width;
    // Alt+Enter / pasted newlines still create real '\n' in the content. The
    // placeholder is dimmed by the widget itself (no InputOption transform).
    input_ = std::make_shared<edc::EdcInput>(&input_text_, "输入消息，Enter 发送，Alt+Enter 换行...");

    // Restore the recall history saved by previous runs (ArrowUp/ArrowDown in
    // the box). Missing file on first run just leaves the box with no history.
    LoadInputHistory();

    container_ = ::ftxui::Container::Vertical({chat_widget_, input_}, &focused_);

    // Left pane = chat + input + footer (see RenderMain); right pane = two
    // collapsible sidebar sections stacked top to bottom: the task tree
    // (TaskWidget) and, directly below it, the todo list (TodoWidget). Both
    // sections are always present — an empty todo list still shows its
    // collapsible "▾/▸ Todos 0/0" header so the sidebar layout is stable
    // from startup instead of the Todo block popping in after the first
    // todowrite call. Each section has its own clickable "▾/▸" header
    // and takes only its natural content height; neither is pinned to the
    // pane bottom, so a short task tree leaves the Todo section sitting
    // right underneath it instead of gluing it to the bottom edge.
    // ResizableSplit puts "main" on the right at a fixed, drag-adjustable
    // width and lets "back" (the chat area) take the rest. The separator is
    // a thin blank column so the sidebar stays borderless (distinguished
    // only by its background color) while keeping a drag grab area to resize
    // it.
    auto main_area = ::ftxui::Renderer(container_, [this] { return RenderMain(); });
    // The container child exists so mouse/key events keep reaching the two
    // widgets (a childless Renderer would swallow them — TaskWidget needs
    // them for its group-header expand/collapse clicks, TodoWidget for its
    // section header).
    //
    // The task tree is the only section that can legitimately outgrow the
    // pane (a long expanded session), so it lives inside a fit-content
    // ScrollView: while the tree is short it occupies exactly its natural
    // height and the Todo section sits right underneath; once the tree would
    // overflow, the view shrinks to the space the Todo section leaves and
    // scrolls the overflow inside it (mouse wheel over the tree). The Todo
    // section is never shrunk, so it stays visible at the bottom instead of
    // being squeezed out of the pane. The view opts out of keyboard focus and
    // of the tail-follow: a snapshot refresh must neither steal Tab/arrows
    // from the editor nor yank the viewport to the bottom of the tree.
    auto task_scroll = std::make_shared<ScrollView>(
        task_widget_, ScrollOptions{ScrollMode::kFitContent, /*show_indicator=*/false, /*follow_tail=*/false,
                                    /*focusable=*/false});
    auto right_column = ::ftxui::Container::Vertical({task_scroll, todo_widget_});
    auto right_pane = ::ftxui::Renderer(right_column, [this, task_scroll] {
        // The two sections stack directly on each other with no divider
        // between them: a separator line would visually slice the sidebar
        // into fragments. The task tree simply ends where the Todo header
        // row begins; the identical pane background keeps them one surface.
        ::ftxui::Elements col = {task_scroll->Render(), todo_widget_->Render()};
        // The column itself flexes so the sidebar background covers the whole
        // pane, but its children stay at natural height and are laid out from
        // the top: leftover vertical space remains at the bottom instead of
        // being absorbed by the task tree to push the todo section down.
        return ::ftxui::vbox(std::move(col)) | ::ftxui::bgcolor(style::palette().pane_bg) | ::ftxui::flex;
    });
    split_ = ::ftxui::ResizableSplit({
        right_pane,                         // main (right pane)
        main_area,                          // back (left pane)
        ::ftxui::Direction::Right,          // direction
        &sidebar_width_,                    // main_size
        [] { return ::ftxui::text("  "); }, // subtle separator, drag grab area
    });
    Add(split_);

    // /connect dialog: three text fields plus Connect/Cancel. Built once; the
    // window overlays it while open and routes all events to it.
    connect_url_input_ = ::ftxui::Input(&connect_url_, "https://api.example.com/v1/chat/completions");
    connect_model_input_ = ::ftxui::Input(&connect_model_, "model name");
    connect_key_input_ = ::ftxui::Input(&connect_key_, "API key");
    // Flat, borderless buttons. ButtonOption::Simple()/Border() draw a boxed
    // frame around the label, which reads as heavy next to the flat dialog;
    // focus is shown as the accent highlight instead, matching the active
    // Tasks/Todos rows.
    ::ftxui::ButtonOption flat_button;
    flat_button.transform = [&pal](const ::ftxui::EntryState &s) {
        auto label = ::ftxui::text(" " + s.label + " ");
        if (s.focused)
            return label | ::ftxui::bold | ::ftxui::color(pal.accent);
        return label | ::ftxui::dim | ::ftxui::color(pal.text_dim);
    };
    auto connect_ok = ::ftxui::Button("Connect", [this] { SubmitConnect(); }, flat_button);
    auto connect_cancel = ::ftxui::Button("Cancel", [this] { connect_open_ = false; }, flat_button);
    connect_buttons_ = ::ftxui::Container::Horizontal({connect_ok, connect_cancel}, &connect_button_focus_);
    auto connect_fields = ::ftxui::Container::Vertical(
        {connect_url_input_, connect_model_input_, connect_key_input_, connect_buttons_}, &connect_focus_);
    connect_modal_ = ::ftxui::Renderer(connect_fields, [this] { return RenderConnectModal(); });

    agent = edc_c_agent_new(false, sandbox_override);
    edc_channel_adapter_init(edc_c_agent_channel_actor(agent), this, on_channel_adapter_send);
    EnableBracketedPaste();

    // Read the model once for the footer (the agent has just loaded its
    // config); the footer itself is built per frame so it can fit the pane.
    {
        char model[256];
        if (edc_c_agent_model(agent, model, sizeof(model)))
            model_ = model;
    }

    // The agent has already restored its active session, so resolve the chat
    // transcript now — before any input can arrive — and replay it. This keys
    // the transcript to the active session without waiting for the first task
    // snapshot, so even the very first typed message is persisted. Later
    // snapshots only reload when /session switch/new changes the session.
    {
        char sid[256];
        if (edc_c_agent_session_id(agent, sid, sizeof(sid)))
        {
            current_session_id_ = sid;
            char path[4096];
            if (edc_c_session_chat_log_path(agent, path, sizeof(path)))
                chat_log_path_ = path;
            LoadChatTranscript();
        }
    }

    // Populate the sidebar once the agent is running (its response is routed
    // through the adapter and handled by on_channel_adapter_send below). The
    // chat log is NOT refilled from any agent state: the TUI persists and
    // replays its own transcript (see LoadChatTranscript).
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
    // anything to the agent or record it as a chat message.
    if (message == "/connect")
    {
        input_text_.clear();
        input_->Reset();
        OpenConnect();
        return;
    }

    chat_widget_->AddMessage(std::make_shared<UserMessage>(message, ::time(nullptr)));
    AppendChatTranscript("user", "", message, "", EDC_C_RENDER_PLAIN);
    input_text_.clear();
    input_->Reset(); // caret/scroll back to the start after the external clear
    // Record the submitted text for ArrowUp recall and persist it so history
    // survives a restart.
    input_->AddHistory(message);
    SaveInputHistory();

    edc_c_agent_send_user_message(agent, message.c_str());
    chat_widget_->ScrollToBottom();

    // A new message may spawn/attach a task, so refresh the sidebar tree. The
    // request is queued behind the user message on the scheduler thread, so
    // its snapshot already reflects the new foreground task's state.
    RequestTaskSnapshot();

    // The animated status is NOT started here and NOT tied to message types:
    // when the snapshot above (or the next periodic one) reports the current
    // foreground process as running, SyncWorkingFromSnapshot() flips the
    // spinner on; it flips off again as soon as the process is no longer
    // busy. Request one extra frame now so the just-submitted message paints
    // immediately.
    if (screen_)
        screen_->RequestAnimationFrame();
}

void TUIWindow::OpenConnect()
{
    char buf[4096];
    connect_url_ = edc_c_agent_api_url(agent, buf, sizeof(buf)) ? buf : "";
    connect_model_ = edc_c_agent_model(agent, buf, sizeof(buf)) ? buf : "";
    connect_key_ = edc_c_agent_api_key(agent, buf, sizeof(buf)) ? buf : "";
    connect_open_ = true;
    connect_focus_ = 0;
    connect_button_focus_ = 0;
    if (screen_)
        screen_->RequestAnimationFrame();
}

bool TUIWindow::SubmitConnect()
{
    if (connect_url_.empty() || connect_model_.empty() || connect_key_.empty())
        return false;
    if (!edc_c_agent_connect(agent, connect_url_.c_str(), connect_model_.c_str(), connect_key_.c_str()))
        return false;
    model_ = connect_model_; // refresh the footer's model label
    connect_open_ = false;
    if (screen_)
        screen_->RequestAnimationFrame();
    return true;
}

void TUIWindow::LoadInputHistory()
{
    char path[4096];
    if (!edc_c_history_path(path, sizeof(path)))
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
    char path[4096];
    if (!edc_c_history_path(path, sizeof(path)))
        return;

    std::string out;
    for (const std::string &entry : input_->History())
        out += EscapeHistoryEntry(entry) + "\n";
    // Symlink-safe (the file sits in $EDC_HOME; still never follow a link).
    edc_c_agent_write_file(agent, path, out.data(), out.size(), /*append=*/false, /*under_workspace=*/false);
}

void TUIWindow::RequestTaskSnapshot()
{
    last_snapshot_request_ = std::chrono::steady_clock::now();
    edc_c_agent_request_task_snapshot(agent);
}

// The chat transcript is a line-oriented file the TUI owns end to end, so its
// format is a private contract: one bubble per line, tab-separated
//   kind \t render_pref \t task_id \t header \t content
// with `\\`, `\n`, `\r` and `\t` backslash-escaped inside every field (so a
// field never contains a raw tab/newline). It records exactly what was shown,
// which is why replay reproduces the pre-exit log verbatim — unlike deriving
// it from the agent's LLM message history, whose entries differ from the
// display stream (tool header text, SYS/command bubbles, suppressed
// background-task traffic).
static std::string EscapeTranscriptField(const std::string &s)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s)
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
        case '\t':
            out += "\\t";
            break;
        default:
            out += c;
        }
    }
    return out;
}

static std::string UnescapeTranscriptField(const std::string &s)
{
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++)
    {
        if (s[i] != '\\' || i + 1 >= s.size())
        {
            out += s[i];
            continue;
        }
        char n = s[++i];
        switch (n)
        {
        case 'n':
            out += '\n';
            break;
        case 'r':
            out += '\r';
            break;
        case 't':
            out += '\t';
            break;
        default:
            out += n; // covers "\\" and any stray escape
        }
    }
    return out;
}

void TUIWindow::AppendChatTranscript(const std::string &kind, const std::string &header, const std::string &content,
                                     const std::string &task_id, int render_pref)
{
    if (chat_log_path_.empty())
        return;
    // Symlink-safe append: the transcript lives under the model-writable
    // workspace, so refuse to follow a planted symlink (see
    // edc_c_agent_write_file / edc_file_fopen_safe).
    std::string line = EscapeTranscriptField(kind) + "\t" + std::to_string(render_pref) + "\t" +
                       EscapeTranscriptField(task_id) + "\t" + EscapeTranscriptField(header) + "\t" +
                       EscapeTranscriptField(content) + "\n";
    edc_c_agent_write_file(agent, chat_log_path_.c_str(), line.data(), line.size(), /*append=*/true,
                           /*under_workspace=*/true);
}

void TUIWindow::LoadChatTranscript()
{
    chat_widget_->Clear();
    if (chat_log_path_.empty())
        return;

    std::ifstream file(chat_log_path_, std::ios::binary);
    if (!file)
        return; // no transcript yet (fresh session)

    const time_t ts = ::time(nullptr);
    std::string line;
    while (std::getline(file, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty())
            continue;

        // Split on the first four raw tabs; fields are escape-free per tab.
        std::string parts[5];
        int field = 0;
        for (char c : line)
        {
            if (c == '\t' && field < 4)
            {
                field++;
                continue;
            }
            parts[field] += c;
        }
        if (field < 4)
            continue; // malformed line

        const std::string kind = UnescapeTranscriptField(parts[0]);
        const int render_pref = std::atoi(parts[1].c_str());
        const std::string task_id = UnescapeTranscriptField(parts[2]);
        const std::string header = UnescapeTranscriptField(parts[3]);
        const std::string content = UnescapeTranscriptField(parts[4]);

        std::shared_ptr<Message> widget;
        if (kind == "user")
        {
            widget = std::make_shared<UserMessage>(content, ts, task_id);
        }
        else if (kind == "tool")
        {
            if (render_pref == EDC_C_RENDER_MARKDOWN)
            {
                if (!header.empty())
                    widget = std::make_shared<ToolMessage>(header, content, ts, /*markdown=*/true, task_id);
                else
                    widget = std::make_shared<Message>(content, ts, task_id);
            }
            else
            {
                widget = std::make_shared<ToolMessage>(header, content, ts, /*markdown=*/false, task_id);
            }
        }
        else if (kind == "thought")
        {
            widget = std::make_shared<ThoughtMessage>(content, ts, task_id);
        }
        else // "reply" | "sys"
        {
            widget = std::make_shared<Message>(content, ts, task_id);
        }
        if (widget)
            chat_widget_->AddMessage(widget);
    }
    chat_widget_->ScrollToBottom();
}

void TUIWindow::SyncChatSession(const char *session_id)
{
    const std::string sid = session_id ? session_id : "";
    if (sid == current_session_id_)
        return;
    current_session_id_ = sid;

    chat_log_path_.clear();
    char path[4096];
    if (edc_c_session_chat_log_path(agent, path, sizeof(path)))
        chat_log_path_ = path;

    LoadChatTranscript();
}

void TUIWindow::SyncWorkingFromSnapshot(const ::edc_c_task_snapshot *snap)
{
    const bool busy = ForegroundProcessBusy(snap);
    if (busy == working_)
        return;
    working_ = busy;
    if (busy)
        working_started_ = std::chrono::steady_clock::now();
    // A transition here can happen while the render loop is otherwise idle
    // (e.g. the first snapshot after a submit starts the animation), so ask
    // for a frame to repaint the footer/spinner row.
    if (screen_)
        screen_->RequestAnimationFrame();
}

::ftxui::Element TUIWindow::RenderMain()
{
    auto input_row = ::ftxui::hbox({prompt_, input_->Render() | ::ftxui::flex});

    // The status row shares this pane's width (the input's width), so the
    // right-aligned model + "/help" stop at the input's right edge and never
    // run into the sidebar: the task/todo pane stays a full-height column.
    ::ftxui::Elements children = {chat_widget_->Render() | ::ftxui::flex, ::ftxui::separator(), input_row,
                                  ::ftxui::separator()};

    if (working_)
    {
        // While the foreground group's active task is running the animated bar
        // replaces the footer line (so the bottom row keeps its height); as
        // soon as a snapshot reports it is no longer busy, the footer returns.
        const auto now = std::chrono::steady_clock::now();
        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - working_started_).count();
        children.push_back(WorkingIndicatorElement(elapsed_ms));

        if (screen_)
            screen_->RequestAnimationFrame();
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

    char cwd[4096];
    const std::string dir = ::getcwd(cwd, sizeof(cwd)) ? ShortenHome(cwd) : std::string();
    const std::string left = " " + dir;

    // Columns available to the footer: the chat pane, i.e. the terminal minus
    // the sidebar and the two-cell split separator. The footer never uses more
    // than this, so its right-aligned model/"help" stop at the input's right
    // edge instead of running under the sidebar.
    int term_w = 80;
    struct winsize ws;
    if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        term_w = ws.ws_col;
    const int pane_w = term_w - sidebar_width_ - 2;

    // Right end: the model in use, the sandbox state and the "/help" pointer,
    // joined by middle dots so they read as one compact unit -- model first,
    // then the sandbox state, then "/help". Flush to the pane's right edge with
    // the filler; when the pane is narrow drop fields from the middle outward:
    // first the model, then the sandbox state, leaving "/help" alone.
    const int budget = pane_w - ::ftxui::string_width(left) - 1;
    const std::string sandbox =
        std::string("sandbox:") + (edc_c_agent_sandbox_enabled(agent) ? "on" : "off");

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
    // Periodic sidebar refresh: background tasks change state without any
    // channel message (they are not the active task), so poll at a low rate.
    {
        const auto now = std::chrono::steady_clock::now();
        if (now - last_snapshot_request_ > std::chrono::milliseconds(500))
            RequestTaskSnapshot();
    }

    auto base = split_->Render() | ::ftxui::bgcolor(style::palette().window_bg);
    if (!connect_open_)
        return base;
    // The /connect dialog is borderless, so the backdrop is dimmed while it is
    // open: the raised dialog surface then reads as the one lit panel on a
    // muted screen instead of relying on a frame to stand out.
    return ::ftxui::dbox(
        {base | ::ftxui::dim,
         connect_modal_->Render() | ::ftxui::size(::ftxui::WIDTH, ::ftxui::LESS_THAN, 80) | ::ftxui::center});
}

::ftxui::Element TUIWindow::RenderConnectModal()
{
    // Flat, borderless dialog. It borrows the sidebar's visual language -- a
    // bold accent heading, dim rules and dim field labels -- but floats on
    // the raised dialog surface instead of drawing a frame. BorderStyle::EMPTY
    // reserves a one-cell inset on every side (so the fields keep their
    // breathing room) while drawing no box lines at all; the window-level
    // overlay dims the backdrop behind it (see OnRender).
    using namespace ::ftxui;
    const auto &pal = style::palette();

    auto field = [&](const char *label, const Component &input) {
        return hbox({
            text(label) | dim | color(pal.text_dim),
            input->Render() | flex,
        });
    };

    return vbox({
               text(" Connect provider ") | bold | color(pal.accent),
               separator() | color(pal.line_dim),
               field("API URL  ", connect_url_input_),
               field("Model    ", connect_model_input_),
               field("API Key  ", connect_key_input_),
               separator() | color(pal.line_dim),
               hbox({
                   text(" ") | dim | color(pal.text_dim),
                   filler(),
                   connect_buttons_->Render(),
               }),
           }) |
           borderStyled(BorderStyle::EMPTY) | bgcolor(pal.dialog_bg);
}

bool TUIWindow::OnEvent(::ftxui::Event event)
{
    // The /connect dialog, when open, owns every event: Esc closes it, Enter
    // submits, everything else goes to its fields.
    if (connect_open_)
    {
        if (event == ::ftxui::Event::Escape)
        {
            connect_open_ = false;
            return true;
        }
        if (event == ::ftxui::Event::Return)
        {
            SubmitConnect();
            return true;
        }
        return connect_modal_->OnEvent(event);
    }

    // Bracketed paste: the terminal wraps pasted content in ESC[200~ ...
    // ESC[201~. FTXUI delivers each marker as a single Event::Special, so the
    // window can track paste state here. Inside a paste, Return and '\n' are
    // inserted as newlines instead of submitted, so a multi-line block becomes
    // one draft rather than one message per line.
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

    if (in_paste_)
    {
        // Everything inside a paste is text. Return/newline becomes a line
        // break; other characters flow straight through to the Input, which
        // handles cursor movement and UTF-8. Nothing here submits.
        if (event == ::ftxui::Event::Return)
        {
            input_->OnEvent(::ftxui::Event::Character("\n"));
            return true;
        }
        return input_->OnEvent(event);
    }

    // Alt+Enter arrives as ESC-prefixed Return: the terminal (raw mode, ICRNL
    // off) sends CR for Enter, and Alt adds the ESC prefix, so the parser emits
    // a single Special({27, 13}). ESC+LF is accepted too for terminals that
    // send LF. Insert a newline via a synthetic character event, which reuses
    // the Input's cursor and UTF-8 handling without firing on_enter.
    static const auto kAltEnterCR = ::ftxui::Event::Special(std::string({27, 13}));
    static const auto kAltEnterLF = ::ftxui::Event::Special(std::string({27, 10}));
    if (event == kAltEnterCR || event == kAltEnterLF)
    {
        input_->OnEvent(::ftxui::Event::Character("\n"));
        return true;
    }

    // Plain Enter submits. Intercepting it here means it never reaches the
    // Input, so multiline stays purely for Alt+Enter and typed text.
    if (event == ::ftxui::Event::Return)
    {
        Submit();
        return true;
    }

    return split_->OnEvent(event);
}

TUIWindow::~TUIWindow()
{
    edc_c_agent_free(agent);
    DisableBracketedPaste();
}

void TUIWindow::SetScreen(::ftxui::ScreenInteractive *screen)
{
    screen_ = screen;
}

void TUIWindow::on_channel_adapter_send(void *instance, struct edc_channel_message *message)
{
    if (!instance || !message)
        return;

    auto *tui = static_cast<TUIWindow *>(instance);
    const auto *content = edc_c_channel_message_content(message);
    if (!content)
        return;

    // A task-tree snapshot is a control message for the sidebar, not chat
    // text. Parse the JSON (via the bridge) and refresh the widget on the UI
    // thread, then stop.
    if (edc_c_channel_message_is_task_snapshot(message))
    {
        const std::string snapshot = content;
        auto update = [tui, snapshot]() {
            ::edc_c_task_snapshot *snap = edc_c_task_snapshot_parse(snapshot.c_str());
            if (snap)
            {
                tui->task_widget_->SetSnapshot(snap);
                // The snapshot identifies the active session; when it changes
                // (first snapshot, /session switch/new) swap the chat log over
                // to that session's own persisted transcript.
                tui->SyncChatSession(snap->session_id);
                // The snapshot is the single source of truth for the status
                // animation: re-derive it whenever the process tree refreshes.
                tui->SyncWorkingFromSnapshot(snap);
                // And for the todo section: it shows the foreground group's
                // active task's own todo list (part of the task state), so it
                // refreshes on every snapshot — not via a side channel on the
                // todowrite chat message.
                tui->todo_widget_->SetItems(ActiveTaskTodoItems(snap));
                edc_c_task_snapshot_free(snap);
            }
            // FTXUI only repaints when the frame was invalidated. Closures
            // posted via screen_->Post() do NOT invalidate it, so while the
            // TUI is idle (no spinner requesting frames) a refreshed sidebar
            // would stay invisible until the next keypress. Ask for exactly
            // one frame so the change paints immediately.
            if (tui->screen_)
                tui->screen_->RequestAnimationFrame();
        };
        if (tui->screen_)
            tui->screen_->Post(std::move(update));
        else
            update();
        return;
    }

    // The channel message is owned by the actor framework and is freed as soon
    // as this callback returns, so classify it and copy the text now. Widget
    // construction and AddMessage() mutate ftxui state shared with the
    // renderer, so they must run on the UI thread via the screen task queue.
    enum class Kind
    {
        None,
        User,
        Tool,
        Thought,
        Reply,
        Sys,
    };
    Kind kind = Kind::None;
    if (edc_c_channel_message_is_user(message))
        kind = Kind::User;
    else if (edc_c_channel_message_is_tool(message))
        kind = Kind::Tool;
    else if (edc_c_channel_message_is_thought(message))
        kind = Kind::Thought;
    else if (edc_c_channel_message_is_resp(message))
        kind = Kind::Reply;
    else if (edc_c_channel_message_is_sys(message))
        kind = Kind::Sys;

    const std::string text = content;
    const char *header_c = edc_c_channel_message_header(message);
    const std::string header = header_c ? header_c : "";
    // Task that produced this message (NULL for user input / snapshots). The
    // chat widgets render it as a "task N" tag.
    const char *task_id_c = edc_c_channel_message_task_id(message);
    const std::string task_id = task_id_c ? task_id_c : "";
    // Sender-declared render preference (PLAIN/MARKDOWN). Read here while
    // the message is still alive; the closure below only needs the int.
    const int render_pref = edc_c_channel_message_render_pref(message);
    const time_t ts = ::time(nullptr);

    auto add_message = [tui, kind, text, header, task_id, render_pref, ts]() {
        std::shared_ptr<Message> widget;
        // todowrite 的 TOOL 消息不进对话区：TODO 状态已由右侧
        // TodoWidget 展示，而它的数据源是任务快照里 active task 的 todo
        // （见 on_channel_adapter_send 的快照分支；工具每次回显都会让
        // scheduler 补推一次快照），聊天里再排一条 checklist 纯属噪音。
        // 消息文本仍随消息传递，供无侧栏的渲染端（CLI）与 tool result
        // （LLM）使用。
        bool skip_chat = false;
        const char *kind_str = "sys";
        switch (kind)
        {
        case Kind::User:
            kind_str = "user";
            widget = std::make_shared<UserMessage>(text, ts);
            break;
        case Kind::Tool:
            kind_str = "tool";
            if (header == kTodoToolHeader)
            {
                skip_chat = true;
                break;
            }

            // 其余工具消息仍渲染进对话区，完全由发送方声明的 render_pref
            // 决定，渲染端零猜测：
            //  - MARKDOWN: 内容按 Markdown 渲染；有 header 时保留
            //    ToolMessage 的折叠 chrome，无 header 直接进 md 消息样式
            //  - 默认/PLAIN（未声明即纯文本）: 一律纯文本 ToolMessage
            if (render_pref == EDC_C_RENDER_MARKDOWN)
            {
                if (!header.empty())
                    widget = std::make_shared<ToolMessage>(header, text, ts, /*markdown=*/true, task_id);
                else
                    widget = std::make_shared<Message>(text, ts, task_id);
            }
            else
            {
                widget = std::make_shared<ToolMessage>(header, text, ts, /*markdown=*/false, task_id);
            }
            break;
        case Kind::Thought:
            kind_str = "thought";
            widget = std::make_shared<ThoughtMessage>(text, ts, task_id);
            break;
        case Kind::Reply:
            kind_str = "reply";
            widget = std::make_shared<Message>(text, ts, task_id);
            // RESP is just another chat message here: it neither starts nor
            // stops the working animation. The animation follows the task
            // snapshot (foreground group's active task running), which is
            // re-requested below and refreshed by the periodic poll in
            // OnRender().
            break;
        case Kind::Sys:
            widget = std::make_shared<Message>(text, ts);
            // SYS (e.g. an error or a command reply) does not touch the
            // status row either; the next snapshot decides it.
            break;
        default:
            return;
        }
        if (!skip_chat && widget)
        {
            // Persist exactly what is shown so the next start replays it.
            tui->AppendChatTranscript(kind_str, header, text, task_id, render_pref);
            tui->chat_widget_->AddMessage(widget);
        }

        // No explicit ScrollToBottom() here: if the reader is already at the
        // tail the view follows it automatically; if they scrolled up to read
        // history, arriving messages must not yank them back down.
        // A message (or a sidebar-only todo refresh) implies task state may
        // have changed; refresh the sidebar.
        tui->RequestTaskSnapshot();

        // The posted closure mutated the chat log but does not invalidate the
        // FTXUI frame by itself; while the TUI is idle (working_ == false and
        // nothing requests frames) the new message would never paint. Ask for
        // one frame so it appears immediately (Submit() already does the same
        // for locally typed messages).
        if (tui->screen_)
            tui->screen_->RequestAnimationFrame();
    };

    // Before Loop() starts there is no render thread, so mutating the tree
    // directly is safe (the agent cannot have produced channel traffic yet).
    if (tui->screen_)
        tui->screen_->Post(std::move(add_message));
    else
        add_message();
}
} // namespace edc
