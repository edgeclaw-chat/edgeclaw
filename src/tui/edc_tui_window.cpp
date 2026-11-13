#include "edc_tui_window.hpp"
#include "edc_api.h" /* EDC_APP_RENDER_PLAIN / _MARKDOWN */
#include "edc_chat_transcript.hpp"
#include "edc_chat_widget.hpp"
#include "edc_command_palette.hpp"
#include "edc_domain.hpp"
#include "edc_history_line.hpp"
#include "edc_token_style.hpp"
#include "edc_input.hpp"
#include "edc_scroll_view.hpp"
#include "edc_sidebar.hpp"
#include "edc_status_bar_view.hpp"
#include "edc_style.hpp"
#include "edc_task_widget.hpp"
#include "edc_todo_widget.hpp"
#include "edc_working_indicator_view.hpp"
#include "ftxui/component/component.hpp"
#include "ftxui/component/component_options.hpp"
#include "ftxui/component/event.hpp"
#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/string.hpp"
#include <algorithm>
#include <atomic>
#include <cJSON.h>
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
#include <thread>
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

// Ctrl+C pressed twice within this window quits; a lone press only clears the input box.
constexpr auto kQuitWindow = std::chrono::milliseconds(1500);

// How much of the chat shows through the update notice: 0 is the opaque surface a toast wears,
// 1 paints nothing. A third of the way gives a surface that is clearly there without hiding
// the messages under it -- and since only the background is blended, the label stays crisp at
// any value.
constexpr float kUpdateBannerAlpha = 0.35f;
// The "press Ctrl+C again to quit" hint stays up exactly as long as a second press
// would still quit, so the hint is never shown when the second press would do nothing.
constexpr auto kHintHold = kQuitWindow;
constexpr char kInterruptHint[] = "Ctrl+C again to quit";

/* 粘贴折叠的门槛（3 行 / 150 字节）。
 *
 * 折叠只在屏幕上有意义：一段两行的东西塞进输入框还不至于失控，一段四十行的会。所以
 * 低于门槛就原样插入 —— 行为与手打完全一致，用户看不出区别。 */
constexpr int kPasteFoldLines = 3;
constexpr int kPasteFoldChars = 150;

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

// Startup transcript backfill. The newest kTranscriptInitialEntries bubbles are
// painted at once (a replay of the whole log is what made startup jank once a
// session's transcript grew); the older history is decoded on a worker thread and
// prepended kTranscriptBackfillBatch at a time from TickAnimations. Both bound one
// step's work (a bubble's measure costs ~0.15ms), so neither the first paint nor a
// backfill frame stalls; the rest converges over the next frames. A batch also stops
// at kTranscriptBackfillBytes so a run of huge tool outputs (a measure is linear in
// the rendered rows) cannot land in a single frame.
constexpr size_t kTranscriptInitialEntries = 200;
constexpr size_t kTranscriptBackfillBatch = 100;
constexpr size_t kTranscriptBackfillBytes = 64 * 1024;

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

// $EDC_HOME/.tui_history stores one recalled entry per line: see edc_history_line.hpp for
// the shape (`<mid>\t<text>`, with the text escaped so newlines and tabs survive).

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
    // The status row reads the session model + terminal width; the sandbox state and the
    // sidebar width are pulled per frame (both change at runtime). The shell-like syntax
    // switch is NOT a footer field (it changes once in a blue moon and its effect is
    // shown where it matters -- see the input line).
    status_bar_ = std::make_shared<ui::StatusBarView>(
        session_model_, terminal_, [this] { return app_ && app_->SandboxEnabled(); },
        [this] { return sidebar_width_; }, [this] { return app_ ? app_->ContextWindow() : 0; });

    const auto &pal = style::palette();
    // The prompt glyph is a step darker than the input box's frame (style::input_prompt_color):
    // a one-cell glyph at the badge's brightness reads as a lamp, not as punctuation.
    prompt_ = ::ftxui::text("❯ ") | ::ftxui::color(style::input_prompt_color(pal)) | ::ftxui::bold;

    // Multiline chat draft with soft (visual) wrapping. Long unbroken lines
    // wrap onto following rows instead of overflowing the input width;
    // Alt+Enter / pasted newlines still create real '\n' in the content. The
    // placeholder is dimmed by the widget itself (no InputOption transform).
    input_ = std::make_shared<edc::EdcInput>(
        &input_text_, "Type a message, or /help for commands, Alt+Enter for a newline...");
    /* An `@path` wears its mark only when the reference is REAL. The check goes through the
     * app (session directory + sandbox + existence), which is also where the mention pass
     * resolves it at submit time -- so the colour cannot promise something the pass refuses. */
    edc::SetMentionPathCheck([this](const std::string &path) {
        return app_ && app_->MentionExists(session_model_->workdir(), path);
    });
    // A recalled line may carry folded blocks whose bytes only the store still has: ask
    // for them as soon as the entry lands in the box (they arrive in a few milliseconds).
    input_->SetRecallHook([this](const std::string &entry) { OnRecalledHistory(entry); });

    container_ = ::ftxui::Container::Vertical({chat_widget_, input_}, &focused_);

    // Left pane = chat + input + footer (see RenderMain); right pane = the task tree
    // (TaskWidget) with the todo list (TodoWidget) stacked under it. Both sections
    // are always present (an empty list still shows its "▾/▸ Todos 0/0" header, so
    // the sidebar is stable from startup) and take only their natural height.
    // ResizableSplit keeps "main" on the right at a drag-adjustable width; its
    // separator is style::split_gutter, so the sidebar stays borderless while the
    // seam between the two panes is still findable (and grabbable).
    /* The left column keeps the backdrop, and the message bubbles keep pane_bg --
     * exactly the layout the widgets themselves draw, unchanged. It is deliberately
     * NOT painted pane_bg as a whole: a pane-coloured bubble on a pane-coloured panel
     * is 1:1, so two messages and the space between them became one flat field and no
     * block edge could be found at all. Against the backdrop a bubble is 1.29:1 -- a
     * real step -- and the backdrop is the palette's own colour (theme::palette's
     * window_bg, the same one the split gutter and the frame use) rather than whatever
     * the terminal happens to be themed with, which is what makes the edge measurable.
     * The pane's outer edge is still the gutter's hairline (style::split_gutter). */
    auto main_area = ::ftxui::Renderer(container_, [this] {
        return RenderMain();
    });
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
        style::split_gutter,                // gutter + hairline, and the drag grab area
    });
    Add(split_);

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

    // First-run setup page: a modal like /connect, opened ONCE while config.json is not
    // "onboarded". Finish persists (LLM settings + context window + the flag); Skip (or
    // Esc) just marks it done, leaving the settings as they are.
    setup_dialog_ = std::make_shared<ui::SetupDialog>(App(), [this] {
        /* Finish persisted a new model: refresh the footer with it. */
        std::string model;
        if (app_->ConfigModel(&model))
            status_bar_->SetModel(model);
        if (screen_)
            screen_->RequestAnimationFrame();
    });
    if (!App()->Onboarded())
    {
        port::SetupConfig seed;
        std::string s;
        if (App()->ConfigApiUrl(&s))
            seed.api_url = s;
        if (App()->ConfigApiKey(&s))
            seed.api_key = s;
        if (App()->ConfigModel(&s))
            seed.model = s; /* ticks the config's current provider/model in the lists */
        setup_dialog_->Open(seed);
    }

    /* The update dialog. The two callbacks are the whole of its coupling to the app: it asks
     * for an upgrade and it asks for the manual command. Everything else -- fetching, spawning,
     * timing out -- stays behind the facade. */
    update_dialog_ = std::make_shared<ui::UpdateDialog>(
        [this](const std::string &tag) {
            /* Start the upgrade; the dialog is already in RUNNING, so a failure here means
             * the work never began and has to be reported as such. */
            const bool started = app_->UpdateStart(tag, [this](bool ok, std::string detail) {
                update_dialog_->OnResult(ok, detail);
                if (screen_)
                    screen_->RequestAnimationFrame();
            });
            if (!started)
            {
                update_dialog_->OnResult(false, "could not start the installer");
                if (screen_)
                    screen_->RequestAnimationFrame();
            }
        },
        [this]() { return app_->UpdateManualHint(); },
        /* Esc while the installer runs: kill it. This is the difference between "abandoned"
         * and "hidden but still replacing your binary". A cancel during the download stage
         * waits for that fetch to return (it has no cancellation hook), which is at most its
         * own timeout. */
        [this]() { app_->UpdateCancel(); });

    /* The check reports here, on the UI thread (the facade marshals it). Record the tag and
     * ask for a frame: the screen is idle by the time the check lands, so without the request
     * the banner would only appear at the user's next keystroke. */
    app_->SetUpdateObserver([this](std::string tag) {
        if (tag.empty())
            return;
        update_tag_ = std::move(tag);
        if (screen_)
            screen_->RequestAnimationFrame();
    });

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
    // A recalled block whose bytes have not arrived yet would go out as the literal
    // `[Pasted #1 · 42 lines]`. Ask again and hold the send: folding must never change what
    // is sent. (When there is nothing to ask for, those placeholders were typed by hand --
    // they are text, and sending them is right.)
    if (input_->HasUnresolvedBlocks() && RequestPayloadForDraft())
    {
        RaisePasteHint(" Fetching the pasted block… ");
        if (screen_)
            screen_->RequestAnimationFrame();
        return;
    }

    // What the line SAYS, not what the box shows: a folded block expands here, so the
    // bubble, the transcript, the history check and the message the app receives all see
    // the same bytes the user would have typed. Folding never changes meaning.
    const std::string expanded = input_->ExpandedDraft();
    const auto first = expanded.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
    {
        input_text_.clear();
        input_->Reset(); // the library keeps its blocks: a history entry still needs them
        return;
    }
    const auto last = expanded.find_last_not_of(" \t\r\n");
    std::string message = expanded.substr(first, last - first + 1);

    /* What the user SAW, and what the bubble/transcript record: a folded block stays the
     * placeholder it was in the box (the bytes go to the app, not to the screen). Without
     * a folded block the two strings are the same one. */
    std::string draft_entry = input_text_;
    const auto d_first = draft_entry.find_first_not_of(" \t\r\n");
    const auto d_last = draft_entry.find_last_not_of(" \t\r\n");
    draft_entry = d_first == std::string::npos ? std::string() : draft_entry.substr(d_first, d_last - d_first + 1);

    // "/connect" is handled locally: open the setup wizard and do not send anything to
    // the app or record it as a chat message. It is the same page the first run shows.
    if (message == "/connect")
    {
        input_text_.clear();
        input_->Reset();
        RecomputeCommandPalette();
        OpenConnect();
        return;
    }

    // "/update" is local for the same reason, and for one more: the dialog is the only place
    // that can say what the update costs, so the command must not become a chat message that
    // scrolls away. With nothing on offer it says so instead of opening an empty dialog.
    if (message == "/update")
    {
        input_text_.clear();
        input_->Reset();
        RecomputeCommandPalette();
        if (update_tag_.empty())
            RaisePasteHint(" No update available ");
        else
            OpenUpdate();
        if (screen_)
            screen_->RequestAnimationFrame();
        return;
    }

    // The bubble is immediate and names no task: the input predates its task and
    // nothing here guesses which task it will become. While no selection is
    // active that is all the log needs; under a selection the input is drawn from
    // the task's own raw_input, as the first user message of its history
    // (ChatWidget::SetTaskInputs), so it is hidden here and shown there.
    const std::string &shown = draft_entry.empty() ? message : draft_entry;
    chat_widget_->AddMessage(std::make_shared<UserMessage>(shown, ::time(nullptr)));
    AppendChatTranscript("user", "", shown, "", EDC_APP_RENDER_PLAIN);
    // Only what this draft actually references travels with the message.
    const struct edc_payload *draft_payload = input_->DraftPayload();
    std::string payload_json;
    if (!edc_payload_empty(draft_payload))
    {
        char *json = edc_payload_to_json(draft_payload);
        if (json)
        {
            payload_json.assign(json);
            edc_payload_string_free(json);
        }
    }
    // The mid is what lets a recalled line fetch its bytes back from the store.
    const std::string mid = NewMessageMid();
    input_text_.clear();
    input_->Reset(); // caret/scroll back to the start after the external clear
    RecomputeCommandPalette();
    // Record the submitted line for ArrowUp recall and persist it so history survives a
    // restart. A folded entry is recorded like any other: its bytes live on the message,
    // and the mid (stored next to the entry) is how a recall gets them back.
    if (!draft_entry.empty())
    {
        input_->AddHistory(draft_entry);
        history_mid_[draft_entry] = mid;
        SaveInputHistory();
    }

    /* Naming a task in a command counts as looking at it: drop its unread marker now,
     * without waiting for the next snapshot (the task's own state does not change). */
    MarkReadFromInput(message);

    app_->ResetFileCandidates(); /* a submission may have created files */
    app_->SendUserMessage(session_model_->displayed_id(), message, payload_json, mid);
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

void TUIWindow::StopForegroundTask()
{
    // The same thing a typed "/task stop" does, minus the chat bubble: nothing here was
    // typed, and the command layer's own reply is the feedback. With no id the command
    // stops the foreground group's active task.
    app_->SendUserMessage(session_model_->displayed_id(), "/task stop", "", "");
    RequestTaskSnapshot();
    if (screen_)
        screen_->RequestAnimationFrame();
}

bool TUIWindow::OnInterrupt()
{
    const auto now = std::chrono::steady_clock::now();
    // A second Ctrl+C within the window quits; a lone press just clears the input box
    // and arms the next one. (Ctrl+C never reaches OnEvent: main()'s top-level
    // CatchEvent routes it here.)
    if (now - last_interrupt_ <= kQuitWindow)
    {
        ClearInterruptHint();
        return true;
    }
    last_interrupt_ = now;

    if (!input_text_.empty())
    {
        input_text_.clear();
        input_->Reset();
        RecomputeCommandPalette();
    }

    // Say what the second press would do, in the bottom-left corner (the footer, or the
    // working row while a task runs).
    interrupt_hint_ = kInterruptHint;
    interrupt_hint_until_ = now + kHintHold;
    PushInterruptHint();

    if (screen_)
        screen_->RequestAnimationFrame();
    return false;
}

void TUIWindow::PushInterruptHint()
{
    if (status_bar_)
        status_bar_->SetHint(interrupt_hint_);
    if (working_indicator_)
        working_indicator_->SetHint(interrupt_hint_);
}

void TUIWindow::ClearInterruptHint()
{
    if (interrupt_hint_.empty())
        return;
    interrupt_hint_.clear();
    PushInterruptHint();
}

void TUIWindow::TickInterruptHint(std::chrono::steady_clock::time_point now)
{
    if (interrupt_hint_.empty())
        return;
    if (now < interrupt_hint_until_)
    {
        // Keep frames coming, so the hint goes away on time even while nothing else is
        // repainting.
        if (screen_)
            screen_->RequestAnimationFrame();
        return;
    }
    ClearInterruptHint();
    if (screen_)
        screen_->RequestAnimationFrame();
}

void TUIWindow::OpenConnect()
{
    // Seed the setup wizard from the persisted config (the same seed the first-run
    // auto-open uses); the model ticks the config's current choice in the lists.
    port::SetupConfig seed;
    std::string s;
    if (app_->ConfigApiUrl(&s))
        seed.api_url = s;
    if (app_->ConfigApiKey(&s))
        seed.api_key = s;
    if (app_->ConfigModel(&s))
        seed.model = s;
    setup_dialog_->Open(seed);
    if (screen_)
        screen_->RequestAnimationFrame();
}

std::string TUIWindow::NewMessageMid()
{
    /* <session>#<n>#<ms>: unique per run (the counter) and across runs (the millisecond
     * clock), readable in a log, and narrow enough that LooksLikeMid() can tell it from a
     * pasted line that happens to contain a tab. Opaque to the kernel -- it only stores it. */
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();
    char buf[128];
    snprintf(buf, sizeof(buf), "%s#%lld#%lld", session_model_->displayed_id().c_str(),
             static_cast<long long>(++mid_seq_), static_cast<long long>(ms));
    return buf;
}

void TUIWindow::RaisePasteHint(const std::string &label)
{
    paste_hint_label_ = label;
    paste_hint_.Raise(std::chrono::steady_clock::now());
}

void TUIWindow::OnRecalledHistory(const std::string &entry)
{
    /* A recalled line whose placeholders this widget does not know: the bytes are in the
     * store, under the mid recorded with that entry. Nothing is asked twice. */
    if (!input_->HasUnresolvedBlocks())
        return;
    const auto mid = history_mid_.find(entry);
    if (mid == history_mid_.end() || dead_mids_.count(mid->second))
        return;
    app_->RequestMessagePayload(mid->second);
}

bool TUIWindow::RequestPayloadForDraft()
{
    if (!input_->HasUnresolvedBlocks())
        return false;

    /* The draft was recalled from the history: name the entry and ask for its payload.
     * A draft that matches no entry has placeholders the user typed by hand -- there is
     * nothing to fetch, and they are text. */
    const std::string draft = input_text_;
    bool asked = false;
    for (const auto &entry : history_mid_)
    {
        if (dead_mids_.count(entry.second))
            continue;
        if (draft.find(entry.first) == std::string::npos)
            continue;
        app_->RequestMessagePayload(entry.second);
        asked = true;
    }
    return asked;
}

void TUIWindow::PresentMessagePayload(const std::string &mid, const std::string &payload_json)
{
    struct edc_payload *payload = edc_payload_from_json(payload_json.c_str());
    if (edc_payload_empty(payload))
    {
        /* The message is gone (its session was removed, or the store never had it). Offering
         * that history line again would put a placeholder in the box that can never be
         * expanded, so it is dropped -- "查不到就跳过该条", never a literal placeholder. */
        edc_payload_free(payload);
        dead_mids_.insert(mid);
        for (auto it = history_mid_.begin(); it != history_mid_.end(); ++it)
        {
            if (it->second != mid)
                continue;
            const std::string entry = it->first;
            history_mid_.erase(it);
            input_->RemoveHistoryEntry(entry);
            SaveInputHistory();
            if (input_text_.find(entry) != std::string::npos)
            {
                input_text_.clear();
                input_->Reset();
                RecomputeCommandPalette();
            }
            break;
        }
        RaisePasteHint(" Pasted block is gone ");
        if (screen_)
            screen_->RequestAnimationFrame();
        return;
    }

    /* Its bytes are back in the library: ExpandedDraft() starts working, the badge and the
     * next submit see the real line. */
    input_->RestoreBlocks(payload);
    edc_payload_free(payload);
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
    history_mid_.clear();
    std::string line;
    while (std::getline(file, line))
    {
        if (!line.empty() && line.back() == '\r') // tolerate CRLF files
            line.pop_back();
        if (line.empty())
            continue;
        const HistoryLine decoded = DecodeHistoryLine(line);
        input_->AddHistory(decoded.text);
        if (!decoded.mid.empty())
            history_mid_[decoded.text] = decoded.mid;
    }
}

void TUIWindow::SaveInputHistory()
{
    std::string path;
    if (!app_->HistoryPath(&path))
        return;

    std::string out;
    for (const std::string &entry : input_->History())
    {
        const auto mid = history_mid_.find(entry);
        out += EncodeHistoryLine(mid == history_mid_.end() ? std::string() : mid->second, entry) + "\n";
    }
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
                                               const std::string &content, const std::string &task_id, int render_pref,
                                               time_t ts)
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
    // A new session (or a return from the history view): abandon any backfill still
    // filling the previous log.
    CancelTranscriptBackfill();

    // Paint only the newest bubbles now. Replaying the whole log at startup is what
    // made it jank once a session's transcript grew (decode + measure, both linear
    // in the entry count); the older history is backfilled below.
    size_t prefix_end = 0;
    std::vector<ChatTranscriptEntry> tail;
    transcript_->ReplayTail(path, kTranscriptInitialEntries, &prefix_end,
                            [&tail](const ChatTranscriptEntry &entry) { tail.push_back(entry); });

    for (const ChatTranscriptEntry &entry : tail)
    {
        const std::shared_ptr<Message> widget = MakeChatWidget(entry.kind, entry.header, entry.content, entry.task_id,
                                                               entry.render_pref, ::time(nullptr));
        if (widget)
            chat_widget_->AddMessage(widget);
    }
    if (!tail.empty())
        chat_widget_->ScrollToBottom();

    // Anything older: decode it off the UI thread and prepend it progressively.
    if (prefix_end > 0)
        StartTranscriptBackfill(path, prefix_end);
}

void TUIWindow::StartTranscriptBackfill(const std::string &path, size_t prefix_end)
{
    // Make sure no previous worker is alive before reusing the state (the caller
    // normally reached here through CancelTranscriptBackfill already).
    CancelTranscriptBackfill();

    std::lock_guard<std::mutex> lock(backfill_mutex_);
    backfill_cancel_.store(false, std::memory_order_relaxed);
    backfill_ready_.store(false, std::memory_order_relaxed);
    // The worker captures |this|, but it is always joined before destruction (see
    // CancelTranscriptBackfill) and is the only writer of |backfill_result_|; it
    // publishes that vector with the release store below. The decode polls
    // |backfill_cancel_| so an abandoned run returns within a batch of lines.
    backfill_thread_ = std::thread([this, path, prefix_end]() {
        std::vector<ChatTranscriptEntry> decoded;
        decoded.reserve(1024);
        LoadTranscriptPrefix(path, prefix_end, &decoded,
                             [this]() { return backfill_cancel_.load(std::memory_order_relaxed); });
        if (backfill_cancel_.load(std::memory_order_relaxed))
            return; // abandoned: publish nothing (avoids a stale/partial history)
        backfill_result_ = std::move(decoded);
        backfill_ready_.store(true, std::memory_order_release); // publishes |backfill_result_|
    });
}

void TUIWindow::PumpTranscriptBackfill()
{
    std::vector<ChatTranscriptEntry> batch;
    {
        std::lock_guard<std::mutex> lock(backfill_mutex_);
        // Take the worker's result once. The acquire load pairs with the worker's
        // release store, so reading |backfill_result_| here is safe.
        if (backfill_remaining_ == 0 && backfill_queue_.empty() && backfill_ready_.load(std::memory_order_acquire))
        {
            backfill_queue_ = std::move(backfill_result_);
            backfill_remaining_ = backfill_queue_.size();
            backfill_result_.clear();
            backfill_ready_.store(false, std::memory_order_relaxed);
        }
        if (backfill_remaining_ == 0)
            return;

        const size_t batch_max = std::min(backfill_remaining_, kTranscriptBackfillBatch);
        // Take entries from the newest end of what is left, stopping early on a byte
        // budget so a run of huge tool outputs is spread over several frames (one
        // entry still goes no matter how large -- it has to land somewhere).
        size_t count = 0;
        size_t bytes = 0;
        while (count < batch_max && bytes < kTranscriptBackfillBytes)
        {
            bytes += backfill_queue_[backfill_remaining_ - 1 - count].content.size();
            count++;
        }
        const size_t start = backfill_remaining_ - count;
        batch.reserve(count);
        for (size_t i = start; i < backfill_remaining_; i++)
            batch.push_back(std::move(backfill_queue_[i]));
        backfill_queue_.resize(start);
        backfill_remaining_ = start;
        if (backfill_remaining_ == 0)
            ReleaseBackfillWorkerLocked(); // fully drained: join the worker, release its state
    }

    // Build the widgets and prepend them outside the lock: ftxui/model work that is
    // UI-thread-only anyway.
    std::vector<std::shared_ptr<Message>> messages;
    messages.reserve(batch.size());
    for (const ChatTranscriptEntry &entry : batch)
    {
        std::shared_ptr<Message> widget = MakeChatWidget(entry.kind, entry.header, entry.content, entry.task_id,
                                                         entry.render_pref, ::time(nullptr));
        if (widget)
            messages.push_back(std::move(widget));
    }
    chat_widget_->PrependMessages(std::move(messages));
    if (screen_)
        screen_->RequestAnimationFrame();
}

void TUIWindow::CancelTranscriptBackfill()
{
    std::lock_guard<std::mutex> lock(backfill_mutex_);
    backfill_cancel_.store(true, std::memory_order_relaxed);
    ReleaseBackfillWorkerLocked();
    backfill_cancel_.store(false, std::memory_order_relaxed);
}

bool TUIWindow::TranscriptBackfillActive() const
{
    std::lock_guard<std::mutex> lock(backfill_mutex_);
    // The worker is joinable from Start until it is released (on drain or cancel);
    // |backfill_remaining_| covers the window after it published but before the last
    // batch was prepended.
    return backfill_thread_.joinable() || backfill_remaining_ > 0;
}

void TUIWindow::ReleaseBackfillWorkerLocked()
{    // The worker polls |backfill_cancel_| while decoding, so it bails out within a
    // batch of lines; the join is bounded and prompt, and it is what makes reusing
    // (or destroying) the members below safe.
    if (backfill_thread_.joinable())
        backfill_thread_.join();
    backfill_result_.clear();
    backfill_result_.shrink_to_fit();
    backfill_queue_.clear();
    backfill_queue_.shrink_to_fit();
    backfill_remaining_ = 0;
    backfill_ready_.store(false, std::memory_order_relaxed);
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
    // Expire the "press Ctrl+C again to quit" hint (and keep repainting while it is up).
    TickInterruptHint(now);

    // Fill in the older transcript a bounded batch at a time (startup backfill).
    // Runs every iteration, so it advances whether or not anything else is animating.
    PumpTranscriptBackfill();

    // "Loading earlier history…" reports a load that lands *above what the reader
    // is looking at*, so it belongs to the reader who has scrolled up past the
    // bubbles painted at startup, into the older history the backfill is still
    // filling in. Merely scrolling up a notch (the first screenful is already
    // there) says nothing is missing, and neither does sitting at the tail; the
    // hint stays down and fades out on its own once the reader comes back.
    if (TranscriptBackfillActive() && chat_widget_ && !chat_widget_->StickingToBottom() &&
        chat_widget_->ReadingBackfilledHistory())
        backfill_hint_.Raise(now);

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

::ftxui::Element TUIWindow::InputSeparatorElement()
{
    // The badge answers "how will this line be read", so it reads the EXPANDED draft: a
    // folded block whose text starts with `/` is a command, and the badge must say so
    // before the user presses Enter (m175: 逐行判定可见).
    const edc_input_mode mode = app_ ? app_->InputMode(input_->ExpandedDraft()) : EDC_INPUT_TEXT;
    const std::string label = InputModeLabel(mode);

    // One colour for the whole frame: the rules and the badge's fill are the SAME colour, so
    // the line runs straight into the badge -- the badge is not a chip laid on top of the rule,
    // it is a word written into it. That colour is the accent toned down (style::
    // input_frame_color): at full strength it reads as neon across a hundred columns.
    const auto &pal = style::palette();
    const ::ftxui::Color rule = style::input_frame_color(pal);
    const ::ftxui::Color badge = rule;

    // The empty box is the one state with no label: nothing is being read yet, and an
    // untouched input should not look decorated.
    if (input_text_.empty())
        return ::ftxui::separator() | ::ftxui::color(rule);

    // The exception writes its name INTO the rule, near its left end: a short run of the
    // line, then the label, then the line running to the right edge. (Centred between two
    // equal runs looked decorative and cost the eye a jump; the label belongs to the line
    // the input box starts on, so it sits where reading starts.)
    //
    // `| flex` is load-bearing on the trailing run: a bare separator in an hbox gets one
    // column and draws the VERTICAL glyph (│), while a stretched one sees a line box and
    // draws "─". The leading run is a fixed 3 columns, which is what keeps the label at the
    // left instead of floating to the middle of the row.
    //
    // A FILLED badge, not coloured text: the label sits IN the line as a block, its glyphs
    // punched out in the frame ink, so the line runs into it and the badge is what the eye
    // lands on. Which mode it is, the word says -- every mode wears the same fill; the kind
    // is not a colour signal.
    return ::ftxui::hbox({::ftxui::separator() | ::ftxui::size(::ftxui::WIDTH, ::ftxui::EQUAL, 3) |
                              ::ftxui::color(rule),
                          ::ftxui::text(" " + label + " ") | ::ftxui::bgcolor(badge) |
                              ::ftxui::color(style::input_frame_ink()) | ::ftxui::bold,
                          ::ftxui::separator() | ::ftxui::flex | ::ftxui::color(rule)});
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

    /* The update notice. First, so it is the BOTTOM-most overlay: the toasts below are
     * reactions to something the user just did and belong on top of a standing fact.
     *
     * An overlay rather than a layout row, and translucent (see TranslucentChipElement): a
     * line between the chat and the input used to cost one row of conversation for as long as
     * the release went uninstalled. Hidden while the dialog is open, which says the same thing
     * with more detail. */
    if (!update_tag_.empty() && !update_dialog_->IsOpen())
        chat = ::ftxui::dbox({std::move(chat), UpdateBannerElement()});

    if (clipboard_toast_.Visible(toast_now))
    {
        chat = ::ftxui::dbox({std::move(chat), ClipboardToastElement(clipboard_toast_.Fade(toast_now))});
        if (screen_)
            screen_->RequestAnimationFrame();
    }

    // The "Loading earlier history…" hint reuses the clipboard chip's look and corner
    // while the startup backfill is still filling the older history in.
    if (backfill_hint_.Visible(toast_now))
    {
        chat = ::ftxui::dbox(
            {std::move(chat), ToastChipElement(" Loading earlier history… ", backfill_hint_.Fade(toast_now))});
        if (screen_)
            screen_->RequestAnimationFrame();
    }

    // The "Fetching the pasted block…" / "Pasted block is gone": same chip, same corner.
    if (paste_hint_.Visible(toast_now))
    {
        chat = ::ftxui::dbox({std::move(chat), ToastChipElement(paste_hint_label_, paste_hint_.Fade(toast_now))});
        if (screen_)
            screen_->RequestAnimationFrame();
    }

    // History view: the input is not rendered at all (nor its two separators) --
    // the pane is just history + status row. The return hint is a badge in the
    // history pane's top-right corner (same dbox trick as the "Copied" chip).

    // The chat is the flex child, and the completion popup is pushed right after it -- i.e.
    // ABOVE the input's top rule. The popup therefore grows into the chat's bottom, and the
    // input block (rule / input / rule) keeps its exact place and height: the list never
    // looks like part of the input box.
    ::ftxui::Elements children;
    children.push_back(chat | ::ftxui::flex);
    if (!history_view_ && command_palette_.open() && !setup_dialog_->IsOpen())
        children.push_back(ui::CommandPaletteElement(command_palette_));
    children.push_back(InputSeparatorElement());
    if (!history_view_)
    {
        children.push_back(input_row);
        // The box's bottom rule wears the same colour, so the input reads as one framed block.
        children.push_back(::ftxui::separator() | ::ftxui::color(style::input_frame_color(style::palette())));
    }

    // History view: the input is off screen, so the return hint takes the bottom
    // row (the status row's slot); nothing is drawn where the input was.
    if (history_view_)
    {
        children.push_back(
            ::ftxui::hbox({::ftxui::text(" History · Esc to go back ") | ::ftxui::color(style::palette().accent)}));
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

::ftxui::Element TUIWindow::UpdateBannerElement()
{
    /* The clipboard chip's look, worn by a notice that does not go away: a translucent
     * surface over the top-left of the chat, so it reports without occupying a layout row.
     *
     * Overlaid rather than stacked on purpose -- a standing "an update is available" line
     * between the chat and the input used to cost one row of conversation for as long as the
     * release went uninstalled. The chip borrows the same corner treatment the transient
     * toasts use, but the opposite corner: they own the top-right and would overdraw a
     * permanent neighbour for the length of every copy and paste.
     *
     * The text keeps the banner's shape (name, version, what to type) because that part was
     * never the problem; only where it sits and how solid it is. */
    return TranslucentChipElement(" EdgeClaw " + update_tag_ + " is available  ·  /update to install ",
                                  kUpdateBannerAlpha);
}

void TUIWindow::OpenUpdate()
{
    if (update_tag_.empty())
        return;
    const bool can = app_ && app_->UpdateCanSelfUpgrade();
    update_dialog_->Open(update_tag_, can);
    if (screen_)
        screen_->RequestAnimationFrame();
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
    // The /connect setup wizard is a modal, centered -- its natural place. The backdrop is
    // dimmed while it is open; clear_under is load-bearing: ftxui's `dim` is a per-cell flag
    // the backdrop stamps over its box, which an overlaid element does not clear.
    //
    // The update dialog is checked first: it appears while the app is in use, the wizard only
    // on first run, and if both were somehow open the newer question is the update.
    if (update_dialog_ && update_dialog_->IsOpen())
        return ::ftxui::dbox({base | ::ftxui::dim, update_dialog_->Render() | ::ftxui::clear_under | ::ftxui::center});
    if (!setup_dialog_->IsOpen())
        return base;
    return ::ftxui::dbox({base | ::ftxui::dim, setup_dialog_->Render() | ::ftxui::clear_under | ::ftxui::center});
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

    // The update dialog, like the setup wizard, is a modal: while it is open it owns every
    // event. It comes first because it can appear while the wizard's state is irrelevant, and
    // because Esc on it must abandon the upgrade rather than fall through to the input box.
    if (update_dialog_ && update_dialog_->IsOpen())
        return update_dialog_->OnEvent(event);

    // The /connect setup wizard is a modal: while open it owns every event (Esc exits,
    // Enter advances, arrows move, the rest goes to its current field).
    if (setup_dialog_ && setup_dialog_->IsOpen())
        return setup_dialog_->OnEvent(event);

    // Any other key means the user moved on, so the "press Ctrl+C again" hint goes away.
    if (!interrupt_hint_.empty() &&
        (event.is_character() || event == ::ftxui::Event::Return || event == ::ftxui::Event::Backspace ||
         event == ::ftxui::Event::Delete || event == ::ftxui::Event::Escape))
    {
        ClearInterruptHint();
        if (screen_)
            screen_->RequestAnimationFrame();
    }

    // Bracketed paste markers (ESC[200~ / ESC[201~) toggle the paste mode and are
    // consumed whatever the mode was. FTXUI delivers each marker as a single
    // Event::Special.
    static const auto kPasteStart = ::ftxui::Event::Special("\x1b[200~");
    static const auto kPasteEnd = ::ftxui::Event::Special("\x1b[201~");
    if (event == kPasteStart)
    {
        in_paste_ = true;
        paste_buffer_.clear();
        return true;
    }
    if (event == kPasteEnd)
    {
        in_paste_ = false;
        CommitPasteBuffer();
        // The paste just changed the text without going through the key path, so refresh
        // the completion popup once here (during the paste it stays as it was).
        RecomputeCommandPalette();
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
    // Everything inside a paste is text, and it is collected rather than typed: the
    // whole paste is inserted once, at ESC[201~ (see CommitPasteBuffer). Nothing here
    // submits.
    if (event == ::ftxui::Event::Return)
    {
        paste_buffer_ += '\n';
        return true;
    }
    if (event.is_character())
    {
        paste_buffer_ += event.character();
        return true;
    }
    /* A control byte is text inside a paste. The parser reports TAB -- and the rest of the
     * C0 range -- as Event::Special, and ignoring those welded the words on both sides of
     * every tab together: a pasted `git ls-files --eol patches/x.patch` line came out as
     * `--eolpatches/x.patch`, unrecoverable once sent. Escape sequences (arrows, mouse
     * reports, the paste terminator) are not text and stay ignored. */
    const std::string &paste_input = event.input();
    if (paste_input.size() == 1 && paste_input[0] != '\x1b' &&
        static_cast<unsigned char>(paste_input[0]) < 0x20)
    {
        paste_buffer_ += paste_input[0];
        return true;
    }
    return false; // mouse / wheel keep their normal meaning
}

/* The paste just ended: insert it as one thing.
 *
 * Small pastes are typed in as-is, so a paste that would be one line stays one line. A
 * large one (>= kPasteFoldLines lines, or > kPasteFoldChars bytes) becomes ONE block:
 * the draft holds a placeholder (`[Pasted #1 · 42 lines]`) and the bytes go to the
 * payload. Folding is a display decision only -- ExpandedDraft() is what the badge, the
 * bubble and the message use, so the line still says exactly what it would have said if
 * it had been typed (edc_text_passes.h has the why). */
void TUIWindow::CommitPasteBuffer()
{
    std::string text = std::move(paste_buffer_);
    paste_buffer_.clear();
    if (text.empty())
        return;

    // Terminals disagree on line endings inside a bracketed paste (Windows ConPTY sends
    // bare CR); normalise once, here, so nothing downstream has to care.
    std::string normalised;
    normalised.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i)
    {
        if (text[i] == '\r')
        {
            if (i + 1 < text.size() && text[i + 1] == '\n')
                continue; // CRLF -> the LF is kept by the next iteration
            normalised += '\n';
            continue;
        }
        normalised += text[i];
    }

    // Trim, the way the submit path trims: a pasted file usually carries a trailing
    // newline that the user did not mean to send.
    const auto first = normalised.find_first_not_of(" \t\n");
    if (first == std::string::npos)
        return; // whitespace only: nothing to insert
    const auto last = normalised.find_last_not_of(" \t\n");
    const std::string body = normalised.substr(first, last - first + 1);

    int lines = 1;
    for (char c : body)
        if (c == '\n')
            ++lines;

    if (!input_)
        return;
    if (lines < kPasteFoldLines && static_cast<int>(body.size()) <= kPasteFoldChars)
    {
        input_->ReplaceRange(input_->CursorPosition(), input_->CursorPosition(), body);
        return;
    }

    char label[64];
    snprintf(label, sizeof(label), "[Pasted #%d \xc2\xb7 %d line%s]", ++paste_seq_, lines, lines == 1 ? "" : "s");
    input_->InsertPasteBlock(label, body);
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

    // The completion popup owns Up/Down/Tab/Esc while it is open, so ↑/↓ walk the rows
    // instead of the caret and history. Enter stays a submit -- with ONE exception: once
    // the user has walked the list (Up/Down pressed on it), the bar is what they are
    // pointing at, so Enter picks that row instead of sending. Typing again clears the
    // walk, so plain `/help⏎` still submits exactly as before.
    if (command_palette_.open())
    {
        if (event == ::ftxui::Event::ArrowUp)
        {
            command_palette_.MoveUp();
            return true;
        }
        if (event == ::ftxui::Event::ArrowDown)
        {
            command_palette_.MoveDown();
            return true;
        }
        if (event == ::ftxui::Event::Tab)
        {
            AcceptCompletion();
            return true;
        }
        /* Enter picks instead of sending -- and "picks" means the highlighted ROW, whole.
         *
         * In the `@` popup there is no "send the half-typed path" reading at all: the line
         * under the caret is a reference that is not finished yet, so Enter always takes the
         * highlighted row (Esc closes the popup first if what you want is to send the text
         * as typed). For `/` commands Enter stays "send" until Up/Down says otherwise -- a
         * whole command line can be typed without ever looking at the list. */
        const bool mention = command_palette_.mode() == model::CommandPalette::Mode::Mention;
        if (event == ::ftxui::Event::Return && (mention || command_palette_.navigated()))
        {
            if (!AcceptCompletion(/*take_selected=*/true))
                Submit();
            return true;
        }
        if (event == ::ftxui::Event::Escape)
        {
            command_palette_.Close();
            return true;
        }
    }

    // Esc stops the foreground group's running task. The completion popup owns Esc while
    // it is open (above), and the history view's Esc is handled before this handler is
    // reached; so here it means "stop what is running". When nothing is running there is
    // no task to stop, so it falls through.
    if (event == ::ftxui::Event::Escape)
    {
        if (working_)
        {
            StopForegroundTask();
            return true;
        }
        return false;
    }

    // Plain Enter submits. Intercepting it here means it never reaches the Input,
    // so multiline stays purely for Alt+Enter and typed text.
    if (event == ::ftxui::Event::Return)
    {
        Submit();
        return true;
    }
    const bool handled = split_->OnEvent(event);
    // Any text edit -- or a caret move, which decides which `@` is being typed -- changes
    // the popup's query.
    if (event.is_character() || event == ::ftxui::Event::Backspace || event == ::ftxui::Event::Delete ||
        event == ::ftxui::Event::ArrowLeft || event == ::ftxui::Event::ArrowRight ||
        event == ::ftxui::Event::ArrowUp || event == ::ftxui::Event::ArrowDown ||
        event == ::ftxui::Event::Home || event == ::ftxui::Event::End)
        RecomputeCommandPalette();
    return handled;
}

bool TUIWindow::AcceptCompletion(bool take_selected)
{
    if (command_palette_.mode() == model::CommandPalette::Mode::Mention)
    {
        // Replace "@src" with the selected "@src/main.c " -- or, for a directory,
        // with "@src/" and stay open so the next level can be picked.
        const std::string insert = command_palette_.MentionInsert();
        if (insert.empty())
            return false;
        input_->ReplaceRange(command_palette_.trigger_begin(), input_->CursorPosition(), insert);
        if (insert.back() == '/')
        {
            RecomputeCommandPalette();
            return true;
        }
        // The inserted `@path` is already one unit: a token's identity comes from the text
        // (edc_token_style.hpp), so nothing has to be registered -- and the same holds for a
        // mention typed by hand or recalled from history.
        command_palette_.Close();
        return true;
    }

    // Complete the selected row. When that leaves nothing more to complete, this same
    // press is the "that's it": fold the list now instead of demanding one more press to
    // say so (the extra press carried no new information).
    const std::string completed =
        take_selected ? command_palette_.AcceptSelected() : command_palette_.Accept();
    const bool changed = completed != input_text_;
    if (changed)
    {
        input_->ReplaceAll(completed);
        RecomputeCommandPalette();
    }
    if (!command_palette_.HasMore())
        command_palette_.Close();
    return changed;
}

void TUIWindow::RecomputeCommandPalette()
{
    // A recall is a browse, not typing. If the popup opened on a recalled `/command` it
    // would grab the very Up/Down keys that walk the history -- after landing on e.g.
    // `/task new` the next ArrowUp would move the list instead of recalling the entry
    // above, so the history dead-ends there. Keep it folded for as long as the browse
    // lasts: the first edit (or ArrowDown past the newest entry) ends it inside EdcInput,
    // and every such event lands here again, so the popup returns with the typing.
    if (input_->IsBrowsingHistory())
    {
        command_palette_.Close();
        return;
    }

    // Load the command table once (static data; the facade projects it from the shell).
    if (!palette_loaded_ && app_)
    {
        command_palette_.SetSource(app_->CommandFamilies(), app_->Commands());
        palette_loaded_ = true;
    }
    // The shell line's option vocabulary follows the switch (the facade returns none while
    // it is off), so a stale one can never offer syntax that is not in force.
    if (app_)
        command_palette_.SetShellOptionSource(app_->ShellOptions());
    // `@`: the caret is inside a file reference. The kernel owns the tree, so ask it for
    // the candidates that match what has been typed -- the popup only shows them.
    const int cursor = input_->CursorPosition();
    const model::CommandPalette::MentionHit mention = model::CommandPalette::ParseMention(input_text_, cursor);
    if (app_ && mention.active)
    {
        command_palette_.SetFileSource(app_->FileCandidates(session_model_->workdir(), mention.query));
        command_palette_.Update(input_text_, cursor);
        return;
    }

    // A command whose first argument names a resource (`/prompt <name>`, `/skill <name>`)
    // gets that vocabulary while it is in play, so a file added on disk is offered without a
    // restart. The DISPLAYED session's bound directory goes with it -- the project roots are
    // found by walking up from there (and the frontend already holds it, so no store read on
    // this thread). Any other input clears the vocabulary so a stale one cannot linger.
    if (app_)
    {
        const std::string &wd = session_model_->workdir();
        if (input_text_.compare(0, 7, "/prompt") == 0)
            command_palette_.SetArgSource("/prompt", app_->PromptNames(wd));
        else if (input_text_.compare(0, 6, "/skill") == 0)
            command_palette_.SetArgSource("/skill", app_->SkillNames(wd));
        else
            command_palette_.SetArgSource("", {});
    }
    command_palette_.Update(input_text_, cursor);
}

TUIWindow::~TUIWindow()
{
    // Abandon a backfill still decoding: the worker touches no window state, so this
    // only drops the shared objects (see StartTranscriptBackfill).
    CancelTranscriptBackfill();
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
        const int render_pref =
            (render && strcmp(render, "markdown") == 0) ? EDC_APP_RENDER_MARKDOWN : EDC_APP_RENDER_PLAIN;
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
        if (kind == "user")
        {
            /* The message holds the expanded text; the history view shows the line the
             * user typed, so the bytes go back to their placeholders. */
            const cJSON *payload = cJSON_GetObjectItem(m, "payload");
            if (cJSON_IsObject(payload))
            {
                char *printed = cJSON_PrintUnformatted(payload);
                if (printed)
                {
                    text = FoldPastedText(std::move(text), printed);
                    cJSON_free(printed);
                }
            }
        }
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
            /* Every member's transcript is on screen now, so every member has been read. */
            if (block_id && *block_id)
                task_widget_->model()->MarkRead(block_id);
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
        /* The whole transcript -- the task's output included -- is on screen now. This is
         * the structural place to say so: it holds for ANY way the view was opened (and the
         * payload is parsed here anyway, so there is no line to re-parse). */
        task_widget_->model()->MarkRead(sid);
    }
    cJSON_Delete(root);

    /* History view: this replaces the chat pane; live output stops until Esc. */
    CancelTranscriptBackfill(); // a running backfill would pollute the filtered view
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

/* `/task output|status|fg <id>`, `/task|group history <id>`: the user asked about that task,
 * so it has been read. Parsed from what was typed -- the frontend owns the input box, and the
 * reply text is not something to re-parse.
 *
 * The command list is for the replies that arrive as PLAIN TEXT: `/task output` prints the
 * output, and `--json` prints the raw history payload. The normal history view does not need
 * an entry here -- it marks itself read where the transcript is rendered (RenderTaskHistory),
 * which also covers the group's members. */
void TUIWindow::MarkReadFromInput(const std::string &line)
{
    static const char *const kCommands[] = {"/task output ", "/task status ", "/task fg ",
                                            "/task history ", "/group history "};
    for (const char *cmd : kCommands)
    {
        const size_t len = std::strlen(cmd);
        if (line.compare(0, len, cmd) != 0)
            continue;
        /* Every non-option word, not just the first: `/task history --json 42` puts the
         * option before the id (`--json` may stand anywhere among the words), and an id
         * that is not in the tree makes MarkRead a no-op -- so marking each candidate is
         * both simpler and correct regardless of order. */
        const std::string rest = line.substr(len);
        size_t p = 0;
        while (p < rest.size())
        {
            const size_t start = rest.find_first_not_of(" \t", p);
            if (start == std::string::npos)
                break;
            const size_t end = rest.find_first_of(" \t\r\n", start);
            const std::string word =
                rest.substr(start, end == std::string::npos ? std::string::npos : end - start);
            if (!word.empty() && word[0] != '-')
                task_widget_->model()->MarkRead(word);
            if (end == std::string::npos)
                break;
            p = end;
        }
        return;
    }
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
    /* A new session deserves the startup hint again: whether this one has interrupted
     * tasks is only knowable from its first snapshot. */
    interrupted_hint_done_ = false;
}

void TUIWindow::SetTaskTree(const TaskSnapshot &snap)
{
    task_widget_->SetSnapshot(&snap);

    /* A task that ended between the last two snapshots, while the user was not looking
     * at it: say so once, in the chat pane. The transcript is deliberately NOT written --
     * this is interface feedback, not something the conversation said. */
    /* "Seen" wins over "finished": a task whose output was on screen since the last
     * snapshot is read -- no marker, and nothing announced. */
    task_widget_->model()->DropSeen(presented_tasks_);
    presented_tasks_.clear();

    const std::vector<std::string> just_finished = task_widget_->TakeFinished();
    if (!just_finished.empty())
    {
        std::string line;
        for (size_t i = 0; i < just_finished.size(); i++)
        {
            std::string state;
            for (const TaskGroup &g : snap.groups)
                for (const TaskInfo &t : g.tasks)
                    if (t.task_id == just_finished[i])
                        state = t.state;
            if (i > 0)
                line += ", ";
            line += "task " + just_finished[i];
            if (!state.empty())
                line += " (" + state + ")";
        }
        line += just_finished.size() == 1 ? " finished · /task output " + just_finished.front()
                                          : " finished";
        chat_widget_->AddMessage(std::make_shared<Message>(line, ::time(nullptr)));
    }

    /* Startup hint: tasks the exit interrupted (nothing decided about them). Once per
     * session, and only from a snapshot that actually carries tasks -- the first pull can
     * arrive before the session is loaded, and that one says nothing about the archive.
     * No transcript write, like the finish notice: this is interface feedback. */
    if (!interrupted_hint_done_ && !snap.groups.empty())
    {
        interrupted_hint_done_ = true;
        std::vector<std::string> interrupted;
        for (const TaskGroup &g : snap.groups)
            for (const TaskInfo &t : g.tasks)
                if (t.end == "INTERRUPTED")
                    interrupted.push_back(t.task_id);
        if (interrupted.size() == 1)
            chat_widget_->AddMessage(std::make_shared<Message>(
                "task " + interrupted.front() + " was interrupted — `/task resume " + interrupted.front() +
                    "` to continue it",
                ::time(nullptr)));
        else if (!interrupted.empty())
            chat_widget_->AddMessage(std::make_shared<Message>(
                std::to_string(interrupted.size()) +
                    " tasks were interrupted — `/task resume <id>` brings one back, one at a time",
                ::time(nullptr)));
    }
    // The footer's context gauge: the foreground group's current context, the same
    // number the sidebar shows for that group.
    long long ctx = 0;
    bool estimated = false;
    for (const TaskGroup &g : snap.groups)
    {
        if (!g.fg)
            continue;
        ctx = g.ctx_tokens;
        estimated = g.ctx_estimated;
        break;
    }
    status_bar_->SetContextUsage(ctx, estimated);
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

    /* Its output is on screen, so this task has been read -- whatever the scheduler's
     * active pointer says by the time the next snapshot arrives (that pointer moves on
     * to the next task in the same batch, which used to make a `!cmd` look unread). */
    if (!event.task_id.empty())
    {
        presented_tasks_.insert(event.task_id);
        task_widget_->model()->MarkRead(event.task_id);
    }

    // No explicit ScrollToBottom() here: if the reader is already at the tail the
    // view follows it automatically; if they scrolled up to read history, arriving
    // messages must not yank them back down. The caller (the dispatcher) then
    // refreshes the sidebar and asks for a frame.
    return true;
}
} // namespace edc
