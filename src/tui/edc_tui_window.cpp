#include "edc_tui_window.hpp"
#include "edc_chat_widget.hpp"
#include "edc_task_widget.hpp"
#include "ftxui/component/component.hpp"
#include "ftxui/component/component_options.hpp"
#include "ftxui/component/event.hpp"
#include "ftxui/dom/elements.hpp"
#include <chrono>
#include <cstdio>
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

} // namespace

TUIWindow::TUIWindow() : input_text_("")
{
    chat_widget_ = std::make_shared<ChatWidget>();
    task_widget_ = std::make_shared<TaskWidget>();

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

    // Left pane = chat + input + footer (see RenderMain); right pane = task
    // sidebar. ResizableSplit puts "main" on the right at a fixed,
    // drag-adjustable width and lets "back" (the chat area) take the rest.
    // The separator is a thin blank column so the sidebar stays borderless
    // (distinguished only by its background color) while keeping a drag grab
    // area to resize it.
    auto main_area = ::ftxui::Renderer(container_, [this] { return RenderMain(); });
    split_ = ::ftxui::ResizableSplit({
        task_widget_,                       // main (right pane)
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

    // A new message may spawn/attach a task, so refresh the sidebar tree.
    RequestTaskSnapshot();

    // Start the "working" animation until the turn's final reply shows up.
    working_ = true;
    if (screen_)
        screen_->RequestAnimationFrame();
}

void TUIWindow::RequestTaskSnapshot()
{
    last_snapshot_request_ = std::chrono::steady_clock::now();
    edc_c_agent_request_task_snapshot(agent);
}

::ftxui::Element TUIWindow::RenderMain()
{
    auto input_row = ::ftxui::hbox({prompt_, input_->Render() | ::ftxui::flex});

    ::ftxui::Elements children = {chat_widget_->Render() | ::ftxui::flex, ::ftxui::separator(), input_row,
                                  ::ftxui::separator()};

    if (working_)
    {
        // While a turn is in flight the animated status replaces the footer
        // line (so the bottom row keeps its height); when the final RESP
        // message arrives the footer hint returns.
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
                edc_c_task_snapshot_free(snap);
            }
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
    const time_t ts = ::time(nullptr);

    auto add_message = [tui, kind, text, header, ts]() {
        std::shared_ptr<Message> widget;
        switch (kind)
        {
        case Kind::User:
            widget = std::make_shared<UserMessage>(text, ts);
            break;
        case Kind::Tool:
            widget = header.empty() ? std::make_shared<Message>(text, ts)
                                    : std::make_shared<ToolMessage>(header, text, ts);
            break;
        case Kind::Thought:
            widget = std::make_shared<ThoughtMessage>(text, ts);
            break;
        case Kind::Reply:
            widget = std::make_shared<Message>(text, ts);
            // Only a RESP marks the end of the turn, so this is the single
            // place that stops the "working" animation.
            tui->working_ = false;
            break;
        case Kind::Sys:
            widget = std::make_shared<Message>(text, ts);
            // SYS (e.g. an error or command reply) does NOT end the turn:
            // keep the working animation running until a RESP arrives.
            break;
        default:
            return;
        }
        tui->chat_widget_->AddMessage(widget);
        tui->chat_widget_->ScrollToBottom();

        // A message implies task state may have changed; refresh the sidebar.
        tui->RequestTaskSnapshot();
    };

    // Before Loop() starts there is no render thread, so mutating the tree
    // directly is safe (the agent cannot have produced channel traffic yet).
    if (tui->screen_)
        tui->screen_->Post(std::move(add_message));
    else
        add_message();
}
} // namespace edc
