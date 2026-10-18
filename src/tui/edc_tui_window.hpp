#pragma once

#include "edc_app_facade.hpp"
#include "edc_channel_dispatcher.hpp"
#include "edc_clipboard.hpp"
#include "edc_ports.hpp"
#include "edc_setup_dialog.hpp"
#include "edc_task_snapshot.hpp"
#include "edc_transcript_store.hpp"
#include "edc_working_indicator_view.hpp"
#include <atomic>
#include <chrono>
#include <cstddef>
#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace edc
{
namespace ui
{
class SidebarView;
class StatusBarView;
} // namespace ui

class ChatWidget;
class EdcInput;
class TaskWidget;
class TodoWidget;

// Snapshot ownership: a reply belongs to the frontend only when it is the tree
// it is currently displaying. An empty |displayed_session| means nothing is
// adopted yet: the snapshot claims the slot (snapshots are pulled, never
// pushed, so the first reply can only be the bootstrap pull's).
bool SnapshotIsForSession(const TaskSnapshot *snap, const std::string &displayed_session);

// "session |event_session| changed for |reason|" -> should the frontend pull?
// "active" (the user switched tabs) always pulls; anything else only if it is
// the tree on screen.
bool TreeChangedEventWantsPull(const std::string &event_session, const std::string &reason,
                               const std::string &displayed_session);

// Where one live output goes: on screen, silent (a non-active task of the same
// session), or into its own session's transcript. Output with no session tag
// (command replies) is kShown.
enum class LiveOutputRoute
{
    kShown,
    kSilent,
    kOtherSession,
};
LiveOutputRoute RouteLiveOutput(const std::string &session_id, bool from_active_task,
                                const std::string &displayed_session);

class TUIWindow : public ::ftxui::ComponentBase, public control::IChannelHost, public port::IUiDispatcher
{
  public:
    // |sandbox_override| mirrors --sandbox/--no-sandbox: -1 keeps the persisted
    // config.json value, 0 forces off, 1 forces on (per-run only). |terminal|
    // supplies the width the footer trims against (nullptr = the real terminal).
    explicit TUIWindow(int sandbox_override = -1, std::shared_ptr<const port::ITerminalInfo> terminal = nullptr);
    virtual ~TUIWindow();

    TUIWindow(const TUIWindow &) = delete;
    TUIWindow &operator=(const TUIWindow &) = delete;

    bool OnEvent(ftxui::Event) override;
    ::ftxui::Element OnRender() override;

    // The app's actors run on their own threads; adapter callbacks are
    // marshalled through this screen's task queue, so the tree is only touched
    // by the render thread.
    void SetScreen(::ftxui::ScreenInteractive *screen);

    // False when the app failed to start (bad/missing config, bad master key,
    // no terminal for the passphrase); main() then exits instead of
    // dereferencing a NULL app.
    bool Ready() const
    {
        return app_ && app_->Ready();
    }

    // Pace the window's own animations from the main-loop iteration.
    void TickAnimations();

    // Ctrl+C: the first press clears the input box, a second one in a row (within a
    // short window) quits. Returns true when the caller should exit the screen. Called
    // by main()'s top-level CatchEvent -- Ctrl+C never reaches OnEvent.
    bool OnInterrupt();

    // ── IChannelHost hooks ───────────────────────────────────────
    // Called by ChannelDispatcher for the view actions that live outside the
    // models; events themselves are handled by the dispatcher (the AppFacade
    // sink), so production and tests share one path.
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

    // IUiDispatcher: post a task to the UI thread. With no screen, Post() runs
    // synchronously.
    void Post(std::function<void()> task) override;

    // Application port (lifetime tied to the window).
    port::IAppFacade *App()
    {
        return app_.get();
    }

    // Leave "history-only" mode and return to the live stream (replaying this
    // conversation's transcript). Esc calls it.
    void ExitHistoryView();

  private:
    // True while the chat pane shows a pulled task history; live output then
    // neither renders nor is written to the transcript.
    bool history_view_ = false;

    // Append the current input as a user message and clear the box. A bare
    // "/connect" opens the setup wizard instead of sending anything.
    void Submit();
    // Clear the sidebar's unread marker when a command names that task.
    void MarkReadFromInput(const std::string &line);
    // Esc: stop the foreground group's running task. Sends the same "/task stop" a user
    // would type (no chat bubble: the command layer's reply is the feedback).
    void StopForegroundTask();
    // While the "press Ctrl+C again to quit" hint is up: push its text to whichever
    // bottom row is showing (the footer, or the working row that replaces it).
    void PushInterruptHint();
    void ClearInterruptHint();
    // Expire the hint (called once per main-loop iteration from TickAnimations).
    void TickInterruptHint(std::chrono::steady_clock::time_point now);
    // Seed + open the setup wizard (reads the persisted config).
    void OpenConnect();
    // Seed the input history from $EDC_HOME/.tui_history (a missing file is fine).
    void LoadInputHistory();
    void SaveInputHistory();
    // `/task history <id>` / `/group history <id>` payload: split into bubbles
    // and append to the chat pane.
    void RenderTaskHistory(const std::string &payload);
    // Show |session_id|'s chat log by replaying its own transcript
    // (<session>/chat.jsonl); a session without one shows an empty log.
    void LoadChatTranscript(const char *session_id);
    // Replay |path| into the (already cleared) log.
    void LoadChatTranscriptFile(const std::string &path);
    // Progressive transcript backfill (see LoadChatTranscriptFile): the newest
    // bubbles are shown at once; the older history is decoded on a worker thread and
    // prepended a bounded batch per TickAnimations, so no single step blocks a frame.
    void StartTranscriptBackfill(const std::string &path, size_t prefix_end);
    void PumpTranscriptBackfill();
    void CancelTranscriptBackfill();
    // Join the backfill worker and clear its state. Caller must hold |backfill_mutex_|.
    void ReleaseBackfillWorkerLocked();
    // True while older history is still being decoded or prepended -- drives the
    // "Loading earlier history…" hint.
    bool TranscriptBackfillActive() const;
    // Record one displayed bubble at the tail of the current session's transcript.
    void AppendChatTranscript(const std::string &kind, const std::string &header, const std::string &content,
                              const std::string &task_id, int render_pref);
    // Same, into an explicit path: a background session's output belongs in that
    // session's own transcript, never in the tab on screen.
    void AppendChatTranscriptTo(const std::string &path, const std::string &kind, const std::string &header,
                                const std::string &content, const std::string &task_id, int render_pref);
    // Show "processing" exactly while the foreground group's active task runs.
    void SyncWorkingFromSnapshot(const TaskSnapshot *snap);
    // Switch the log to |session_id|: the log is cleared and that session's
    // messages re-derived, so it always belongs to the displayed session.
    ::ftxui::Element RenderMain();
    // Put the selection on the clipboard and raise the chip; runs on the frame
    // after the drag was released, once FTXUI's selection is complete.
    void CopySelectionToClipboard();

    // Input mode, replacing the old if-ordering in OnEvent: paste / history /
    // normal (the dialog is judged first -- it is innermost and owns every
    // event). Each handler only handles its own mode.
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
    // true = consumed; false = pass through to the normal path (mouse/wheel/Alt+Enter).
    bool HandleHistoryEvent(::ftxui::Event event);
    bool HandleNormalEvent(::ftxui::Event event);
    // Refresh the completion popup from the current input text (and load its source
    // from the command table on first use).
    void RecomputeCommandPalette();

    std::shared_ptr<edc::ChatWidget> chat_widget_;
    std::shared_ptr<edc::TaskWidget> task_widget_;
    std::shared_ptr<edc::TodoWidget> todo_widget_;
    // Multi-line input with soft wrapping; also handed to |container_| for focus.
    std::shared_ptr<edc::EdcInput> input_;
    ::ftxui::Element prompt_;
    std::string input_text_;

    // The completion popup, shown while the input starts with '/'. Hangs just below the
    // input. Its source (the shell's command table, via the facade) is loaded once, lazily.
    model::CommandPalette command_palette_;
    bool palette_loaded_ = false;

    // Routes keyboard focus between the chat log and the input (1 = input).
    // Declared before |container_| because the container borrows it.
    int focused_ = 1;
    ::ftxui::Component container_;

    // Drag-adjustable split: the task sidebar is on the right, resized by the
    // separator.
    int sidebar_width_ = 36;
    ::ftxui::Component split_;
    // The sidebar column; held so it outlives the split it was handed to.
    std::shared_ptr<ui::SidebarView> sidebar_;

    // Set between the bracketed-paste ESC[200~ and ESC[201~, where Return is
    // inserted as a newline instead of submitting.
    bool in_paste_ = false;

    // Chat log state (messages + filter + task inputs) and the displayed
    // session's identity. Widgets render them; the dispatcher writes them.
    std::shared_ptr<model::ChatModel> chat_model_;
    std::shared_ptr<model::SessionModel> session_model_;

    // App port: owns the edc_app (assembly + the only C boundary).
    std::unique_ptr<port::AppFacade> app_;
    // The AppFacade sink: turns typed events into model updates + IChannelHost hooks.
    std::unique_ptr<control::ChannelDispatcher> dispatcher_;
    // Chat-log file I/O (transcript port; the concrete store is built in the ctor).
    std::unique_ptr<port::ITranscriptStore> transcript_;
    // Terminal geometry port: the footer trims its fields against this width.
    std::shared_ptr<const port::ITerminalInfo> terminal_;
    // True once bracketed paste was enabled, so the destructor only emits the
    // "disable" sequence when the matching "enable" ran.
    bool bracketed_paste_on_ = false;
    ::ftxui::ScreenInteractive *screen_ = nullptr;
    ClipboardToast clipboard_toast_;
    // The "Loading earlier history…" chip (same look as the clipboard toast): raised
    // only while the reader is scrolled up into a backfill that is still running, then
    // left to fade out once it finishes or they return to the tail.
    Toast backfill_hint_;
    // Where the left-button drag started, and whether a copy is still due. The
    // copy waits for the next frame: FTXUI rebuilds its selection while that
    // frame is drawn, and a click without movement is not a selection.
    int press_x_ = 0;
    int press_y_ = 0;
    bool copy_pending_ = false;

    /* The /connect setup wizard: a modal shown on first run (and by /connect). */
    std::shared_ptr<ui::SetupDialog> setup_dialog_;

    // True while the foreground group's active task runs. Owned exclusively by
    // SyncWorkingFromSnapshot(), never toggled by channel messages.
    bool working_ = false;
    // The animated "working" row, swapped in for the footer while working_.
    std::shared_ptr<ui::WorkingIndicatorView> working_indicator_;
    // Footer: working directory on the left, model/sandbox//help on the right.
    std::shared_ptr<ui::StatusBarView> status_bar_;
    // When the spinner's next repaint is due.
    std::chrono::steady_clock::time_point last_spinner_frame_;
    // When Ctrl+C was last pressed (the second press within a short window quits).
    std::chrono::steady_clock::time_point last_interrupt_{};
    // The "press Ctrl+C again to quit" hint: its text ("" = hidden) and when it expires.
    std::string interrupt_hint_;
    std::chrono::steady_clock::time_point interrupt_hint_until_{};
    // When the chat log's next post-resize re-measure frame is due (see
    // TickAnimations).
    std::chrono::steady_clock::time_point last_remeasure_frame_;
    // Whether the sidebar is currently breathing, and when that run started.
    bool dot_breathing_ = false;
    std::chrono::steady_clock::time_point breath_started_;
    // Throttles the periodic snapshot refresh (which is what catches background
    // task-state changes, since they send no channel message).
    std::chrono::steady_clock::time_point last_snapshot_request_;

    /* Startup transcript backfill.
     *
     * Threading: only the decode runs off the UI thread (|backfill_thread_|); its
     * result is published through |backfill_ready_| with a release store, and read
     * only after an acquire load returns true -- so the decoded vector crosses
     * threads without sharing a lock with the decode itself. Every other access to
     * the fields below (Start/Pump/Cancel, reached from UI-thread callbacks and
     * TickAnimations) is serialized by |backfill_mutex_|. The worker is always
     * joined before its state is reused or destroyed (see CancelTranscriptBackfill),
     * so capturing |this| in the lambda is safe. Note the widget/model calls that
     * Start leads to still require the UI thread (all IChannelHost callbacks are
     * marshalled there by AppFacade) -- the mutex only guards this handoff state. */
    mutable std::mutex backfill_mutex_;
    std::thread backfill_thread_;
    std::atomic<bool> backfill_cancel_{false};
    std::atomic<bool> backfill_ready_{false};
    std::vector<ChatTranscriptEntry> backfill_result_; // worker writes; UI reads after |backfill_ready_|
    std::vector<ChatTranscriptEntry> backfill_queue_;  // file order; consumed from the back
    size_t backfill_remaining_ = 0;
};
} // namespace edc
