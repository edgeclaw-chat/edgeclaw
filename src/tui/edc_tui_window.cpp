#include <cJSON.h>
#include "edc_tui_window.hpp"
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
#include <sys/ioctl.h>
#include "edc_api.h"
#include "edc_app.h"
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

// todowrite 工具的 TOOL 消息头（与 edc_tools_todo.c 注册名一致）。它的文本回显
// （Markdown checklist）不进对话区：TODO 状态只由右侧 TodoWidget 展示，数据源是
// 任务快照里 active task 的 todo；文本仍随消息传递，供 CLI 与 tool result（LLM）用。
constexpr char kTodoToolHeader[] = "todowrite";

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

// The "working" row: a bar of blocks that sweeps back and forth while the
// brightness inside the lit run flows along it. Exactly one text line (it
// replaces the footer, so the layout height must not change), a fixed cell count
// (never the width), no background fill, and medium block glyphs -- not the
// full-height █▓▒░, which tower over the surrounding text.
constexpr int kBarCells = 10;     // cells in the bar
constexpr int kBarLit = 5;        // lit cells: the sliding run
constexpr long long kStepMs = 60; // per cell of travel
// The run slides from completely past the left edge to completely past the
// right edge before the sweep reverses, so the ends of the bar are genuine
// turning points (a few frames of empty bar) rather than bumpers.
constexpr int kRunFirst = -kBarLit; // run fully off the left edge
constexpr int kRunLast = kBarCells; // run fully off the right edge
constexpr int kRunPositions = kRunLast - kRunFirst + 1;
// First frame: the run fully on screen at the left, so the row looks busy
// straight away instead of opening on an empty bar.
constexpr int kRunStart = 0;

// How often the working spinner repaints. The bar moves one cell per kStepMs, so
// a frame per loop tick redraws the whole tree for sub-cell movement nobody can
// see; see TickAnimations() for why the pacing cannot live in the render.
constexpr long long kSpinnerFrameMs = 33;

// Where the lit run sits at |elapsed_ms|, and which way it is travelling.
// The position is fractional so the run can slide *between* cells instead of
// snapping a whole cell per step; at whole-cell times it lands exactly on the
// integer the old discrete sweep used, so the sampled frames are unchanged.
struct BarState
{
    float pos;    // leftmost lit cell; may be negative or past the right edge
    bool forward; // true while the run sweeps right
};

// The run slides out past one edge -- tail included -- then re-enters from it and
// crosses to the other, ping-ponging, so the motion never jumps and no turning
// point is held for an extra frame. Float, so the position can sit between two
// cells and the edge cells can fade (see WorkingIndicatorElement).
BarState BarAt(long long elapsed_ms)
{
    const float step = static_cast<float>(elapsed_ms < 0 ? 0 : elapsed_ms) / static_cast<float>(kStepMs);
    const float cycle = static_cast<float>(2 * (kRunPositions - 1));
    // Negative elapsed is clamped above, so the phase is never negative.
    const float phase = std::fmod(static_cast<float>(kRunStart - kRunFirst) + step, cycle);
    if (phase < static_cast<float>(kRunPositions))
        return {static_cast<float>(kRunFirst) + phase, true};
    return {static_cast<float>(kRunFirst) + (cycle - phase), false};
}

// The shade an unlit cell uses; the run's edge cells fade towards it.
::ftxui::Color UnlitColor(const style::Palette &pal)
{
    return pal.text_dim;
}

// A lit block, shaded by depth: the terminal cannot fade opacity, so the ramp
// is a handful of discrete steps from a bold accent head down to a barely
// there gray. Same glyph at every depth: only the colour intensity changes.
// |tier| 0 is the brightest (bold accent) and 4 the darkest lit shade.
::ftxui::Element LitBlock(int tier, const style::Palette &pal)
{
    if (tier <= 2)
    {
        auto e = ::ftxui::text(style::kBlockGlyph) | ::ftxui::color(pal.accent);
        if (tier == 0)
            e = e | ::ftxui::bold;
        else if (tier == 2)
            e = e | ::ftxui::dim;
        return e;
    }
    auto e = ::ftxui::text(style::kBlockGlyph) | ::ftxui::color(pal.text_dim);
    if (tier >= 4)
        e = e | ::ftxui::dim;
    return e;
}

// A cell the run's edge is only part-way across: same lit glyph, but its shade
// is faded towards the unlit colour by how much of the cell is covered, so the
// head and tail of the run dissolve in and out instead of popping one whole
// cell at a time. |cover| is in (0, 1); whole cells never take this path.
::ftxui::Element PartialLitBlock(int tier, float cover, const style::Palette &pal)
{
    const ::ftxui::Color lit = (tier <= 2) ? pal.accent : pal.text_dim;
    auto e = ::ftxui::text(style::kBlockGlyph) | ::ftxui::color(::ftxui::Color::Interpolate(cover, UnlitColor(pal), lit));
    if (tier == 0)
        e = e | ::ftxui::bold;
    else if (tier == 2 || tier == 4)
        e = e | ::ftxui::dim;
    return e;
}

// An unlit cell: the small square that keeps the bar's full length visible, so
// the eye can see the run move rather than a floating blob.
::ftxui::Element OffBlock(const style::Palette &pal)
{
    return ::ftxui::text("⬝") | ::ftxui::color(pal.text_dim) | ::ftxui::dim;
}

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

// The animated "working" row: the sweeping bar on its own, no text. Defined here
// (declared in the header) so tests can render it for a given elapsed time without
// a live app. Fixed cell count and one line tall, so it never changes the layout.
::ftxui::Element WorkingIndicatorElement(long long elapsed_ms)
{
    const auto &pal = style::palette();

    // BarAt clamps a negative elapsed (a clock adjustment between the start
    // stamp and the frame must not rewind the bar).
    const BarState bar = BarAt(elapsed_ms);
    const float run_begin = bar.pos;
    const float run_end = bar.pos + static_cast<float>(kBarLit);

    // The bar is the whole row: the animation alone says "working", so the
    // row carries no label and its width is just the bar's.
    ::ftxui::Elements row;
    row.reserve(static_cast<size_t>(kBarCells));
    for (int i = 0; i < kBarCells; i++)
    {
        // How much of cell |i| (the span [i, i+1]) the run currently covers.
        // At a whole-cell position every cell is either fully covered or not at
        // all, so the frame matches the old discrete sweep exactly; between
        // positions the edge cells are partially covered.
        const float cover = std::min(static_cast<float>(i + 1), run_end) - std::max(static_cast<float>(i), run_begin);
        if (cover <= 0.0f)
        {
            row.push_back(OffBlock(pal));
            continue;
        }

        // Depth from the leading edge: the edge the run travels towards is the
        // brightest and the shade falls off behind it, so the gradient points
        // the way the light moves and swaps ends when the sweep turns around.
        const float into_run = static_cast<float>(i) - bar.pos;
        float depth = bar.forward ? (static_cast<float>(kBarLit - 1) - into_run) : into_run;
        depth = std::min(std::max(depth, 0.0f), static_cast<float>(kBarLit - 1));
        const int tier = static_cast<int>(std::lround(depth));

        // A whole cell draws its exact depth shade; a part-covered edge cell
        // fades that shade towards the unlit colour by its coverage, so the run
        // slips between cells instead of snapping one whole cell per step.
        if (cover >= 1.0f)
            row.push_back(LitBlock(tier, pal));
        else
            row.push_back(PartialLitBlock(tier, cover, pal));
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

    // Left pane = chat + input + footer (see RenderMain); right pane = the task tree
    // (TaskWidget) with the todo list (TodoWidget) stacked under it. Both sections
    // are always present (an empty list still shows its "▾/▸ Todos 0/0" header, so
    // the sidebar is stable from startup) and take only their natural height.
    // ResizableSplit keeps "main" on the right at a drag-adjustable width; the
    // separator is a blank column, so the sidebar stays borderless but grabbable.
    auto main_area = ::ftxui::Renderer(container_, [this] { return RenderMain(); });
    // The sidebar column is built in edc_sidebar.cpp so a test can compose the
    // very same layout (see MakeSidebar: both sections fit-content with their
    // headers pinned, the tree anchored to its bottom -- that anchoring is what
    // keeps the active task's row on screen when the todo list has a lot to say).
    const Sidebar sidebar = MakeSidebar(task_widget_, todo_widget_);
    auto right_pane = sidebar.pane;
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
        return label | ::ftxui::color(pal.text);
    };
    auto connect_ok = ::ftxui::Button("Connect", [this] { SubmitConnect(); }, flat_button);
    auto connect_cancel = ::ftxui::Button("Cancel", [this] { connect_open_ = false; }, flat_button);
    connect_buttons_ = ::ftxui::Container::Horizontal({connect_ok, connect_cancel}, &connect_button_focus_);
    auto connect_fields = ::ftxui::Container::Vertical(
        {connect_url_input_, connect_model_input_, connect_key_input_, connect_buttons_}, &connect_focus_);
    connect_modal_ = ::ftxui::Renderer(connect_fields, [this] { return RenderConnectModal(); });

    app = edc_app_new(false, sandbox_override);

    // edc_app_new() returns NULL (after logging the reason) when it cannot
    // start: unreadable config, undecryptable API key, no terminal for the
    // passphrase, env init failure. Every call below dereferences the app, so
    // stop here. main() sees Ready() == false and exits cleanly.
    if (!app)
        return;

    edc_app_channel_adapter_init(app, this, on_channel_adapter_send);
    EnableBracketedPaste();
    bracketed_paste_on_ = true;

    // Read the model once for the footer (the app has just loaded its
    // config); the footer itself is built per frame so it can fit the pane.
    {
        char model[256];
        if (edc_app_config_model(app, model, sizeof(model)))
            model_ = model;
    }

    // Populate the sidebar once the app is running (its response is routed
    // through the adapter and handled by on_channel_adapter_send below), and
    // learn from that first snapshot which session we are showing. Its reply
    // carries the session's own transcript path, which the live path below
    // appends to -- so nothing here reads the session store: it belongs to the
    // actor thread, and the snapshot already names what we need.
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

    edc_app_send_user_message(app, current_session_id_.c_str(), message.c_str());
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
    char buf[4096];
    connect_url_ = edc_app_config_api_url(app, buf, sizeof(buf)) ? buf : "";
    connect_model_ = edc_app_config_model(app, buf, sizeof(buf)) ? buf : "";
    connect_key_ = edc_app_config_api_key(app, buf, sizeof(buf)) ? buf : "";
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
    if (!edc_app_set_llm_config(app, connect_url_.c_str(), connect_model_.c_str(), connect_key_.c_str()))
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
    if (!edc_app_history_path(path, sizeof(path)))
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
    if (!edc_app_history_path(path, sizeof(path)))
        return;

    std::string out;
    for (const std::string &entry : input_->History())
        out += EscapeHistoryEntry(entry) + "\n";
    // Symlink-safe (the file sits in $EDC_HOME; still never follow a link).
    edc_app_write_file(app, path, out.data(), out.size(), /*append=*/false, /*under_workspace=*/false);
}

void TUIWindow::RequestTaskSnapshot()
{
    /* 测试模式: 不向 app 要树。理由 —— 构造期 (:464) 与回调里 (:1077) 的拉取
     * 都会走一个**异步**回包, 而没 SetScreen() 时那回包是内联在 runner 线程上
     * 跑进本函数的调用方 (on_channel_adapter_send 的 else 分支), 会和测试自己的
     * 断言抢同一个 widget。设了它就完全没有这条路 (tests/test_tui_dispatch.cpp)。
     * 本仓既有先例: EDGECLAW_SNAPSHOT_TRACE 也是 env 开关。 */
    if (getenv("EDGECLAW_TUI_NO_PULL"))
        return;
    last_snapshot_request_ = std::chrono::steady_clock::now();
    // 点名叫自己**正在显示**的那棵树 (P3 ①b) —— 别人的树不上路。
    edc_app_request_task_snapshot(app, current_session_id_.c_str());
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
    if (!snap || snap->session_id.empty())
        return false;
    /* Nothing adopted yet: let the first reply claim the slot (the caller adopts
     * its session_id). Snapshots are only ever replies to a pull, so the one
     * that can arrive here is the answer to the bootstrap pull -- it names the
     * active session, not someone else's. */
    if (displayed_session.empty())
        return true;
    return displayed_session == snap->session_id;
}

bool TreeChangedEventWantsPull(const std::string &event_session, const std::string &reason,
                               const std::string &displayed_session)
{
    if (event_session.empty())
        return false;
    if (reason == "active")
        return true;
    return event_session == displayed_session;
}

LiveOutputRoute RouteLiveOutput(const std::string &session_id, bool from_active_task,
                                const std::string &displayed_session)
{
    if (session_id.empty())
        return LiveOutputRoute::kShown; // 不是某个 task 的产出 (命令回复等)
    if (session_id != displayed_session)
        return LiveOutputRoute::kOtherSession;
    return from_active_task ? LiveOutputRoute::kShown : LiveOutputRoute::kSilent;
}

void TUIWindow::LoadChatTranscript(const char *session_id)
{
    chat_widget_->Clear();
    chat_log_path_.clear();
    /* The cached workdir belongs to the session being replaced: drop it rather
     * than show a value we already know is wrong. The next snapshot (pulled right
     * after every switch) refills it; until then the footer falls back to the
     * launch directory. */
    session_workdir_.clear();
    current_session_id_ = session_id ? session_id : "";

    // The transcript belongs to the session, not to the tab: it records what this
    // frontend displayed for that session, so it survives switches and restarts.
    char path[4096];
    if (!current_session_id_.empty() &&
        edc_app_chat_log_path_for(app, current_session_id_.c_str(), path, sizeof(path)))
        chat_log_path_ = path;

    LoadChatTranscriptFile(chat_log_path_);
}

void TUIWindow::LoadChatTranscriptFile(const std::string &path)
{
    const size_t entries = LoadTranscript(path, [this](const ChatTranscriptEntry &entry) {
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
    AppendChatTranscriptTo(chat_log_path_, kind, header, content, task_id, render_pref);
}

void TUIWindow::AppendChatTranscriptTo(const std::string &path, const std::string &kind, const std::string &header,
                                       const std::string &content, const std::string &task_id, int render_pref)
{
    if (path.empty())
        return;

    ChatTranscriptEntry entry;
    entry.kind = kind;
    entry.render_pref = render_pref;
    entry.task_id = task_id;
    entry.header = header;
    entry.content = content;

    // A session's directory only exists once the session has been saved, and a
    // command-only session (e.g. /help) may never be saved before its first line
    // is displayed — make sure the parent is there before appending.
    edc_app_ensure_parent_dir(app, path.c_str());

    // A previous process may have died mid-append, leaving a fragment without
    // its newline. Re-terminate it first: otherwise this line lands on top of it
    // and the two bubbles merge into one undecodable line.
    std::string line = EncodeTranscriptLine(entry);
    if (TranscriptEndsMidLine(path))
        line.insert(line.begin(), '\n');

    // Symlink-safe append: the transcript lives under the model-writable
    // workspace, so refuse to follow a planted symlink (see
    // edc_file_fopen_safe / edc_app_write_file).
    edc_app_write_file(app, path.c_str(), line.data(), line.size(), /*append=*/true, /*under_workspace=*/true);
}

void TUIWindow::SyncWorkingFromSnapshot(const TaskSnapshot *snap)
{
    const bool busy = ForegroundProcessBusy(snap);
    if (busy == working_)
        return;
    working_ = busy;
    if (busy)
    {
        working_started_ = std::chrono::steady_clock::now();
        // Start the spinner's cadence from the frame requested just below.
        last_spinner_frame_ = working_started_;
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
        children.push_back(::ftxui::hbox({::ftxui::text(" 历史 · Esc 返回 ") |
                                          ::ftxui::color(style::palette().accent)}));
        children.push_back(::ftxui::text(""));
        return ::ftxui::vbox(std::move(children));
    }

    if (working_)
    {
        // While the foreground group's active task is running the animated bar
        // replaces the footer line (so the bottom row keeps its height); as
        // soon as a snapshot reports it is no longer busy, the footer returns.
        const auto now = std::chrono::steady_clock::now();
        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - working_started_).count();
        children.push_back(WorkingIndicatorElement(elapsed_ms));

        // Repaints are paced by TickAnimations(), not requested here.
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
    const char *dir_src = !session_workdir_.empty()
                              ? session_workdir_.c_str()
                              : (::getcwd(cwd_buf, sizeof(cwd_buf)) ? cwd_buf : nullptr);
    const std::string dir = dir_src ? ShortenHome(dir_src) : std::string();
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
    const std::string sandbox = std::string("sandbox:") + (edc_app_sandbox_enabled(app) ? "on" : "off");

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
    if (!connect_open_)
        return base;
    // The dialog is borderless, so the backdrop is dimmed while it is open: the
    // raised surface then reads as the one lit panel on a muted screen.
    // clear_under is load-bearing: ftxui's `dim` is a per-cell flag the backdrop
    // stamps over its box and an overlaid element never clears it, so without it the
    // dialog renders gray whatever its text colour.
    return ::ftxui::dbox({base | ::ftxui::dim, connect_modal_->Render() | ::ftxui::clear_under |
                                                   ::ftxui::size(::ftxui::WIDTH, ::ftxui::LESS_THAN, 80) |
                                                   ::ftxui::center});
}

::ftxui::Element TUIWindow::RenderConnectModal()
{
    // Flat, borderless dialog borrowing the sidebar's visual language (accent
    // heading, dim rules, flat labels) but floating on dialog_bg. Labels use the
    // bright foreground, not text_dim, which washes out on that surface.
    // BorderStyle::EMPTY reserves the inset without drawing box lines.
    using namespace ::ftxui;
    const auto &pal = style::palette();

    auto field = [&](const char *label, const Component &input) {
        return hbox({
            text(label) | color(pal.text),
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

    // 只看模式: 整块左侧是"读 history"的视图。它**不接受输入** —— 输入框已经不在屏上,
    // 字符与回车一律吞掉 (鼠标与滚动照旧放行, 历史要能滚)。唯一能做的事是 Esc: 回实时流
    // (重放转录)。这一块放在 /connect 弹窗之后: 弹窗开着时 Esc 归它 (先关弹窗),
    // 这也是"最内层先拿事件"的老规矩。
    if (history_view_)
    {
        if (event == ::ftxui::Event::Escape)
        {
            ExitHistoryView();
            return true;
        }
        if (event.is_character() || event == ::ftxui::Event::Return)
            return true;
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
    edc_app_free(app);
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
    LoadChatTranscript(current_session_id_.empty() ? nullptr : current_session_id_.c_str());
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

void TUIWindow::on_channel_adapter_send(void *instance, struct edc_channel_message *message)
{
    if (!instance || !message)
        return;

    auto *tui = static_cast<TUIWindow *>(instance);
    const auto *content = edc_app_message_content(message);
    // `/task history <id>` / `/group history <id>` 的应答: 结构化载荷, 拆成一条条气泡。
    // 与别的消息走同一条 UI 线程规则 (有屏幕就 Post, 无屏幕的 harness 里内联)。
    if (edc_app_message_is_task_history(message))
    {
        const std::string payload = content ? content : "";
        if (tui->screen_)
            tui->screen_->Post([tui, payload]() { tui->RenderTaskHistory(payload); });
        else
            tui->RenderTaskHistory(payload);
        return;
    }
    if (!content)
        return;

    // A tree-changed event (P3 ①b) is a control message: "session X's tree
    // changed", no tree attached. The frontend decides what to do:
    //   - reason "active"  → the user switched tabs: adopt X as displayed (its
    //     transcript), then pull X's tree;
    //   - anything else    → pull **only** if X is the tree on screen. A
    //     background session's churn is not our business: it never touches the
    //     view here (that is the whole point of separating the event from the
    //     payload — before, every change pushed the bound session's whole tree
    //     and that push decided what the user was looking at).
    if (edc_app_message_is_task_tree_changed(message))
    {
        const char *sid_c = edc_app_message_session_id(message);
        const std::string sid = sid_c ? sid_c : "";
        const std::string reason = content;
        auto on_change = [tui, sid, reason]() {
            if (!TreeChangedEventWantsPull(sid, reason, tui->current_session_id_))
                return;
            if (reason == "active" && sid != tui->current_session_id_)
                tui->LoadChatTranscript(sid.c_str());
            tui->RequestTaskSnapshot();
        };
        if (tui->screen_)
            tui->screen_->Post(on_change);
        else
            on_change();
        return;
    }

    // A task-tree snapshot is a control message for the sidebar, not chat
    // text. Parse the JSON (via the bridge) and refresh the widget on the UI
    // thread, then stop.
    if (edc_app_message_is_task_snapshot(message))
    {
        const std::string snapshot = content;
        auto update = [tui, snapshot]() {
            TaskSnapshot snap;
            // A late reply must claim its own request: if the session changed
            // while it was in flight (the user switched tabs), this tree is
            // yesterday's news — drop it. The displayed session is the TUI's
            // own state (current_session_id_); the kernel never tells it what
            // to display, and a snapshot certainly doesn't.
            if (!ParseTaskSnapshot(snapshot, &snap) || !SnapshotIsForSession(&snap, tui->current_session_id_))
                return;
            /* First reply after startup: adopt the session it names (and that
             * session's transcript) as the one we are showing. This is how the
             * frontend learns which session it displays -- no synchronous read of
             * the store at bootstrap. */
            if (tui->current_session_id_.empty())
                tui->LoadChatTranscript(snap.session_id.c_str());
            /* The footer's working directory comes from here, so it never reaches
             * into the store (which the actor thread mutates). */
            tui->session_workdir_ = snap.workdir;
            {
                // Give the chat log what each task was created from: while a
                // selection is active, that input is drawn as the first user
                // message of the task's history (the input's own bubble belongs
                // to no task, so the filter hides it). Straight from the task by
                // id -- no matching of any kind, and it works the same on replay
                // after a restart.
                auto inputs = std::make_shared<std::map<std::string, std::string>>();
                for (const TaskGroup &g : snap.groups)
                    for (const TaskInfo &t : g.tasks)
                        if (!t.input.empty())
                            (*inputs)[t.task_id] = t.input;
                tui->chat_widget_->SetTaskInputs(inputs);
                tui->task_widget_->SetSnapshot(&snap);
                // The snapshot is the single source of truth for the status
                // animation: re-derive it whenever the process tree refreshes.
                tui->SyncWorkingFromSnapshot(&snap);
                // And for the todo section: the list shows the foreground group's
                // active task's own todos (part of the task state), so it
                // refreshes on every snapshot — not via a side channel on the
                // todowrite chat message. The task id is passed along so the
                // section can name whose list it is, and the task's state so its
                // current step wears the failure colour if the task failed (read
                // from the very same snapshot, so items and state cannot
                // disagree).
                const ActiveTaskTodos todos = ActiveTaskTodosOf(&snap);
                tui->todo_widget_->SetItems(todos.items, todos.task_id);
                tui->todo_widget_->SetOwnerState(ForegroundActiveTaskState(&snap));
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
    if (edc_app_message_is_user(message))
        kind = Kind::User;
    else if (edc_app_message_is_tool(message))
        kind = Kind::Tool;
    else if (edc_app_message_is_thought(message))
        kind = Kind::Thought;
    else if (edc_app_message_is_resp(message))
        kind = Kind::Reply;
    else if (edc_app_message_is_sys(message))
        kind = Kind::Sys;

    const std::string text = content;
    const char *header_c = edc_app_message_header(message);
    const std::string header = header_c ? header_c : "";
    // Task that produced this message (NULL for user input / snapshots). Kept
    // for callers; the chat widgets no longer render a "task N" tag.
    const char *task_id_c = edc_app_message_task_id(message);
    const std::string task_id = task_id_c ? task_id_c : "";
    // Session that owns the producing task (NULL for user input / snapshots).
    // Lets output of a background session be kept out of the visible tab (the
    // app already persisted it into that session's own messages).
    const char *session_id_c = edc_app_message_session_id(message);
    const std::string session_id = session_id_c ? session_id_c : "";
    // Sender-declared render preference (PLAIN/MARKDOWN). Read here while
    // the message is still alive; the closure below only needs the int.
    const int render_pref = edc_app_message_render_pref(message);
    // 发送方声明的事实: 这条产出是不是它自己 session 的 active task 产生的。
    const bool from_active_task = edc_app_message_from_active_task(message) != 0;
    const time_t ts = ::time(nullptr);

    auto add_message = [tui, kind, text, header, task_id, session_id, render_pref, ts, from_active_task]() {
        // 只看模式: 实时产出不上屏、也不写转录 (写点在展示之后 —— 与从前的过滤同口径)。
        // 命令回复 (SYS) 例外: 在只看模式里敲命令也要有反馈, 不然静默得像卡死了。
        if (tui->history_view_ && kind != Kind::Sys)
            return;

        // 展示闸门 (P3 ③: 从 core 移到前端): 显示中的 session 只显示它 **active
        // task** 的产出 —— 同 session 里非 active task 的输出静默。以前这条判断在
        // scheduler/app 里做 (靠"当前绑定的 session"这份副本); 现在 core 不知道
        // 谁在看, 只声明事实 (from_active_task), 由这里决定 (见 RouteLiveOutput)。
        if (RouteLiveOutput(session_id, from_active_task, tui->current_session_id_) ==
            LiveOutputRoute::kSilent)
            return;

        // todowrite 的 TOOL 消息不进对话区：TODO 状态已由右侧 TodoWidget 展示，
        // 数据源是任务快照里 active task 的 todo（工具每次回显都会补推快照）。
        // 文本仍随消息传递，供无侧栏的渲染端（CLI）与 tool result（LLM）使用。
        const char *kind_str = "sys";
        switch (kind)
        {
        case Kind::User:
            kind_str = "user";
            break;
        case Kind::Tool:
            if (header == kTodoToolHeader)
                return;
            kind_str = "tool";
            break;
        case Kind::Thought:
            kind_str = "thought";
            break;
        case Kind::Reply:
            kind_str = "reply";
            // RESP is just another chat message here: it neither starts nor
            // stops the working animation. The animation follows the task
            // snapshot (foreground group's active task running), which is
            // re-requested below and refreshed by the periodic poll in
            // OnRender().
            break;
        case Kind::Sys:
            // SYS (e.g. an error or a command reply) does not touch the
            // status row either; the next snapshot decides it.
            break;
        default:
            return;
        }

        // A background session's output must not appear in the tab that is
        // currently displayed — but it is still part of *that* session's screen
        // history, so it goes into that session's own transcript and shows up
        // when the user switches back. The current tab (and its sidebar) is left
        // untouched.
        if (!session_id.empty() && session_id != tui->current_session_id_)
        {
            char path[4096];
            if (edc_app_chat_log_path_for(tui->app, session_id.c_str(), path, sizeof(path)))
                tui->AppendChatTranscriptTo(path, kind_str, header, text, task_id, render_pref);
            return;
        }

        // Persist exactly what is shown, so the next start replays this log.
        tui->AppendChatTranscript(kind_str, header, text, task_id, render_pref);

        if (std::shared_ptr<Message> widget = MakeChatWidget(kind_str, header, text, task_id, render_pref, ts))
            tui->chat_widget_->AddMessage(widget);

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
    // directly is safe (the app cannot have produced channel traffic yet).
    if (tui->screen_)
        tui->screen_->Post(std::move(add_message));
    else
        add_message();
}
} // namespace edc
