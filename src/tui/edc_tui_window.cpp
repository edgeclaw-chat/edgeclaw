#include "edc_tui_window.hpp"
#include "edc_chat_widget.hpp"
#include "edc_task_widget.hpp"
#include "edc_todo_widget.hpp"
#include "ftxui/component/component.hpp"
#include "ftxui/component/component_options.hpp"
#include "ftxui/component/event.hpp"
#include "ftxui/dom/elements.hpp"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <edc_channel_adapter.h>
#include <memory>
#include <tui/edc_c_bridge.h>

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

// update_todolist 工具的 TOOL 消息头（与 edc_tools_todo.c 注册名保持一致）。
// 该工具的文本回显（Markdown checklist）在 TUI 里不进对话消息列表——TODO
// 状态只由右侧 TodoWidget 展示，其数据源是任务快照里 active task 的 todo
// （快照由 scheduler 在每次工具回显后主动补推），聊天区重复一条 checklist
// 纯属噪音；文本仍随消息传递，供无侧栏的渲染端（CLI）与 tool result（LLM）
// 使用。
constexpr char kTodoToolHeader[] = "update_todolist";

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
            return t->state && (std::strcmp(t->state, "LLM_PROCESSING") == 0 ||
                                std::strcmp(t->state, "TOOL_CALLING") == 0);
        }
        return false; // foreground group exists but has no active task marked
    }
    return false; // no foreground group at all
}

// Todo sidebar content: the foreground group's active task's own todo list.
// The list is part of the task state (update_todolist attaches it to the
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
                item.active_form = t->todo_items[i].active_form ? t->todo_items[i].active_form : "";
                items.push_back(std::move(item));
            }
            return items;
        }
        return items; // foreground group exists but has no active task marked
    }
    return items; // no foreground group at all
}

} // namespace

TUIWindow::TUIWindow() : input_text_("")
{
    chat_widget_ = std::make_shared<ChatWidget>();
    task_widget_ = std::make_shared<TaskWidget>();
    todo_widget_ = std::make_shared<TodoWidget>();

    footer_ = ::ftxui::text(" Enter 发送   Alt+Enter 换行   Tab 切换输入/滚动   滚轮/↑↓ 滚动   Esc 退出 ") |
              ::ftxui::color(::ftxui::Color::RGB(147, 147, 147));
    prompt_ = ::ftxui::text("❯ ") | ::ftxui::color(::ftxui::Color::Blue) | ::ftxui::bold;

    ::ftxui::InputOption option;
    option.multiline = true;

    option.transform = [](::ftxui::InputState state) {
        if (state.is_placeholder)
        {
            state.element |= ::ftxui::dim;
        }
        return state.element;
    };

    input_ = ::ftxui::Input(&input_text_, "输入消息，Enter 发送，Alt+Enter 换行...", option);

    container_ = ::ftxui::Container::Vertical({chat_widget_, input_}, &focused_);

    // Left pane = chat + input + footer (see RenderMain); right pane = two
    // collapsible sidebar sections stacked top to bottom: the task tree
    // (TaskWidget) and, directly below it, the todo list (TodoWidget). Both
    // sections are always present — an empty todo list still shows its
    // collapsible "▾/▸ Todos 0/0" header so the sidebar layout is stable
    // from startup instead of the Todo block popping in after the first
    // update_todolist call. Each section has its own clickable "▾/▸" header
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
    auto right_column = ::ftxui::Container::Vertical({task_widget_, todo_widget_});
    auto right_pane = ::ftxui::Renderer(right_column, [this] {
        // The two sections stack directly on each other with no divider
        // between them: a separator line would visually slice the sidebar
        // into fragments. The task tree simply ends where the Todo header
        // row begins; the identical pane background keeps them one surface.
        ::ftxui::Elements col = {task_widget_->Render(), todo_widget_->Render()};
        // The column itself flexes so the sidebar background covers the whole
        // pane, but its children stay at natural height and are laid out from
        // the top: leftover vertical space remains at the bottom instead of
        // being absorbed by the task tree to push the todo section down.
        return ::ftxui::vbox(std::move(col)) |
               ::ftxui::bgcolor(::ftxui::Color::RGB(30, 30, 30)) | ::ftxui::flex;
    });
    split_ = ::ftxui::ResizableSplit({
        right_pane,                         // main (right pane)
        main_area,                          // back (left pane)
        ::ftxui::Direction::Right,          // direction
        &sidebar_width_,                    // main_size
        [] { return ::ftxui::text("  "); }, // subtle separator, drag grab area
    });
    Add(split_);

    agent = edc_c_agent_new(false, false);
    edc_channel_adapter_init(edc_c_agent_channel_actor(agent), this, on_channel_adapter_send);
    EnableBracketedPaste();

    // Populate the sidebar once the agent is running (its response is routed
    // through the adapter and handled by on_channel_adapter_send below).
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

    chat_widget_->AddMessage(std::make_shared<UserMessage>(message, ::time(nullptr)));
    input_text_.clear();

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

void TUIWindow::RequestTaskSnapshot()
{
    last_snapshot_request_ = std::chrono::steady_clock::now();
    edc_c_agent_request_task_snapshot(agent);
}

void TUIWindow::SyncWorkingFromSnapshot(const ::edc_c_task_snapshot *snap)
{
    const bool busy = ForegroundProcessBusy(snap);
    if (busy == working_)
        return;
    working_ = busy;
    // A transition here can happen while the render loop is otherwise idle
    // (e.g. the first snapshot after a submit starts the animation), so ask
    // for a frame to repaint the footer/spinner row.
    if (screen_)
        screen_->RequestAnimationFrame();
}

::ftxui::Element TUIWindow::RenderMain()
{
    auto input_row = ::ftxui::hbox({prompt_, input_->Render() | ::ftxui::flex});

    ::ftxui::Elements children = {chat_widget_->Render() | ::ftxui::flex, ::ftxui::separator(), input_row,
                                  ::ftxui::separator()};

    if (working_)
    {
        // While the foreground group's active task is running the animated
        // status replaces the footer line (so the bottom row keeps its
        // height); as soon as a task snapshot reports it is no longer busy,
        // the footer hint returns.
        static const char *kFrames[] = {"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"};
        const auto now = std::chrono::steady_clock::now();
        const auto ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
        const size_t frame = (static_cast<size_t>(ms) / 100) % (sizeof(kFrames) / sizeof(kFrames[0]));

        auto spinner = ::ftxui::text(kFrames[frame]) | ::ftxui::color(::ftxui::Color::RGB(94, 151, 247)) |
                       ::ftxui::bold;
        auto label = ::ftxui::text(" 处理中...") | ::ftxui::color(::ftxui::Color::RGB(147, 147, 147));
        children.push_back(::ftxui::hbox({spinner, label}));

        if (screen_)
            screen_->RequestAnimationFrame();
    }
    else
    {
        children.push_back(footer_);
    }

    return ::ftxui::vbox(std::move(children));
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

    return split_->Render() | ::ftxui::bgcolor(::ftxui::Color::RGB(10, 10, 10));
}

bool TUIWindow::OnEvent(::ftxui::Event event)
{
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
                // The snapshot is the single source of truth for the status
                // animation: re-derive it whenever the process tree refreshes.
                tui->SyncWorkingFromSnapshot(snap);
                // And for the todo section: it shows the foreground group's
                // active task's own todo list (part of the task state), so it
                // refreshes on every snapshot — not via a side channel on the
                // update_todolist chat message.
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
    // Sender-declared render preference (PLAIN/MARKDOWN). Read here while
    // the message is still alive; the closure below only needs the int.
    const int render_pref = edc_c_channel_message_render_pref(message);
    const time_t ts = ::time(nullptr);

    auto add_message = [tui, kind, text, header, render_pref, ts]() {
        std::shared_ptr<Message> widget;
        // update_todolist 的 TOOL 消息不进对话区：TODO 状态已由右侧
        // TodoWidget 展示，而它的数据源是任务快照里 active task 的 todo
        // （见 on_channel_adapter_send 的快照分支；工具每次回显都会让
        // scheduler 补推一次快照），聊天里再排一条 checklist 纯属噪音。
        // 消息文本仍随消息传递，供无侧栏的渲染端（CLI）与 tool result
        // （LLM）使用。
        bool skip_chat = false;
        switch (kind)
        {
        case Kind::User:
            widget = std::make_shared<UserMessage>(text, ts);
            break;
        case Kind::Tool:
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
                    widget = std::make_shared<ToolMessage>(header, text, ts, /*markdown=*/true);
                else
                    widget = std::make_shared<Message>(text, ts);
            }
            else
            {
                widget = std::make_shared<ToolMessage>(header, text, ts);
            }
            break;
        case Kind::Thought:
            widget = std::make_shared<ThoughtMessage>(text, ts);
            break;
        case Kind::Reply:
            widget = std::make_shared<Message>(text, ts);
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
    // directly is safe (the agent cannot have produced channel traffic yet).
    if (tui->screen_)
        tui->screen_->Post(std::move(add_message));
    else
        add_message();
}
} // namespace edc
