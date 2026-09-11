#include "edc_c_bridge.h"
#include "edc_clipboard.hpp"
#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <chrono>
#include <memory>
#include <string>

namespace edc
{
class ChatWidget;
class EdcInput;
class TaskWidget;
class TodoWidget;

// The animated "working" row shown in place of the footer while the
// foreground process runs: a bar of blocks that sweeps back and forth, its
// brightness flowing along the lit run. The animation is the whole row — no
// label text. |elapsed_ms| is the time since the process went busy; it drives
// both the sweep position and the flow phase. Declared here (rather than
// staying local to the window) so tests can render the row without a live
// agent. The bar has a fixed number of cells and is one line tall, so showing
// it in place of the footer never moves the layout.
::ftxui::Element WorkingIndicatorElement(long long elapsed_ms);

class TUIWindow : public ::ftxui::ComponentBase
{
  public:
    // |sandbox_override| mirrors the --sandbox/--no-sandbox command-line flag:
    // -1 keeps the persisted config.json value, 0 forces off, 1 forces on. It is
    // passed straight to edc_c_agent_new() (per-run only, never persisted) and
    // can still be toggled at runtime with /sandbox on|off.
    explicit TUIWindow(int sandbox_override = -1);
    virtual ~TUIWindow();

    TUIWindow(const TUIWindow &) = delete;
    TUIWindow &operator=(const TUIWindow &) = delete;

    bool OnEvent(ftxui::Event) override;
    ::ftxui::Element OnRender() override;

    // The agent channel actor runs on its own thread; adapter callbacks are
    // marshalled to the UI thread through this screen's task queue so the
    // ftxui component tree is only touched from the render thread.
    void SetScreen(::ftxui::ScreenInteractive *screen);

    // False when the agent failed to start (missing/corrupt config.json, bad
    // master key, no terminal for the passphrase, ...). The error is logged by
    // edc_agent_new(); main() checks this and exits without entering the loop
    // instead of dereferencing a NULL agent.
    bool Ready() const { return agent != nullptr; }

    // Advance the window's own animations. Called once per main-loop iteration
    // (see main.cpp): the loop returns every animation tick, and this is the
    // only UI-thread moment the window gets that is not a repaint. It asks for
    // a frame only when the working spinner's cadence is due, which is what
    // keeps a running task from repainting the whole TUI at the loop's rate.
    void TickAnimations();

  private:
    // Append the current input as a user message and clear the box. A bare
    // "/connect" opens the provider dialog instead of sending anything.
    void Submit();
    // Open the /connect dialog, prefilled with the current provider settings.
    void OpenConnect();
    // Persist + hot-apply whatever is in the /connect dialog. Returns true on
    // success (the dialog then closes).
    bool SubmitConnect();
    // Render the centered /connect dialog (overlaid on the normal UI).
    ::ftxui::Element RenderConnectModal();
    // Seed the input box's recall history from $EDC_HOME/.tui_history (tolerates
    // a missing file).
    void LoadInputHistory();
    // Rewrite $EDC_HOME/.tui_history from the input box's current history.
    void SaveInputHistory();
    // Ask the agent (on its scheduler thread) for a fresh task-tree snapshot.
    void RequestTaskSnapshot();
    // Replay this session's persisted chat transcript (the exact bubbles shown
    // before the last exit) into the chat log, after clearing it. Called when
    // the active session is (re)identified from a snapshot. Best-effort: a
    // missing/unreadable file just leaves the chat empty.
    void LoadChatTranscript();
    // Append one shown chat bubble to the current session's transcript file so
    // the next start replays it verbatim. |render_pref| is EDC_C_RENDER_*.
    // Best-effort: a failed write must not disturb the UI.
    void AppendChatTranscript(const std::string &kind, const std::string &header, const std::string &content,
                              const std::string &task_id, int render_pref);
    // Re-evaluate the animated status from a freshly parsed task snapshot:
    // "processing" is shown exactly while the foreground group's active task
    // is running, regardless of which channel messages arrived.
    void SyncWorkingFromSnapshot(const ::edc_c_task_snapshot *snap);
    // Point the chat log at |session_id|'s own transcript. When the id changes
    // (first snapshot after start, /session switch or /session new) the log is
    // cleared and that session's persisted bubbles are replayed, so the log
    // always belongs to the active session and survives a restart verbatim.
    void SyncChatSession(const char *session_id);
    // Render the left pane (chat log + input + footer/spinner).
    ::ftxui::Element RenderMain();
    // Build the footer row: the working directory on the left; on the right the
    // active model, the sandbox state and the "/help" pointer, right-aligned
    // within the chat pane's width (never spilling into the sidebar).
    ::ftxui::Element RenderFooter() const;
    // Put whatever is selected on the clipboard and raise the chip. Runs on the
    // frame after the drag was released, once FTXUI's selection is complete.
    void CopySelectionToClipboard();

    std::shared_ptr<edc::ChatWidget> chat_widget_;
    std::shared_ptr<edc::TaskWidget> task_widget_;
    std::shared_ptr<edc::TodoWidget> todo_widget_;
    // Multi-line input with soft (visual) line wrapping: long drafts wrap to
    // the input width instead of overflowing, and the caret navigates the
    // wrapped rows. Owned here; also handed to |container_| for focus/events.
    std::shared_ptr<edc::EdcInput> input_;
    ::ftxui::Element prompt_;
    std::string input_text_;
    // The LLM model in use, read once from the agent's config; shown at the
    // right end of the footer (dropped only when the pane is too narrow).
    std::string model_;

    // Routes keyboard focus between the chat log and the input; the input
    // starts focused so typing works without a Tab first. The selector int must
    // outlive the container that borrows it, so it is declared first.
    int focused_ = 1;
    ::ftxui::Component container_;

    // Drag-adjustable left/right split: the task sidebar is on the right and
    // its width is user-resizable via the separator.
    int sidebar_width_ = 36;
    ::ftxui::Component split_;

    // True while consuming a bracketed-paste block (between ESC[200~ and
    // ESC[201~). Inside it, Return/Character("\n") are inserted as newlines
    // rather than submitted, so a pasted multi-line block becomes one draft.
    bool in_paste_ = false;

    // The active session's chat-transcript path and id, refreshed whenever a
    // snapshot reports a (possibly new) session. The transcript is the exact
    // stream of chat bubbles shown for that session; when the id changes the
    // chat log is cleared and the new session's transcript replayed, so the
    // log always reflects the active session.
    std::string chat_log_path_;
    // Sentinel: no session seen yet, so the first snapshot triggers a load.
    std::string current_session_id_ = "\x01";

    struct edc_agent *agent = NULL;
    // True once bracketed paste has actually been turned on, so the destructor
    // only emits the "disable" sequence when the matching "enable" ran (a
    // failed startup returns from the constructor before enabling it).
    bool bracketed_paste_on_ = false;
    ::ftxui::ScreenInteractive *screen_ = nullptr;
    // The "Copied to clipboard" chip's own clock: a copy raises it and it fades
    // out by itself (see edc_clipboard.hpp).
    ClipboardToast clipboard_toast_;
    // Where the current left-button drag started, and whether it has been let go
    // with a copy still to take. The copy waits for the next frame: FTXUI
    // rebuilds its selection while that frame is drawn, and a click (no
    // movement) is not a selection at all.
    int press_x_ = 0;
    int press_y_ = 0;
    bool copy_pending_ = false;

    // /connect dialog: an overlay with three fields (API URL / Model / API
    // Key), built once in the constructor. While it is open the dialog owns
    // all keyboard/mouse events. The values live in the strings above; the
    // components render from them directly.
    bool connect_open_ = false;
    std::string connect_url_;
    std::string connect_model_;
    std::string connect_key_;
    ::ftxui::Component connect_modal_;
    ::ftxui::Component connect_url_input_;
    ::ftxui::Component connect_model_input_;
    ::ftxui::Component connect_key_input_;
    ::ftxui::Component connect_buttons_;
    int connect_focus_ = 0;
    int connect_button_focus_ = 0;

    // True while the foreground group's active task is running (the
    // scheduler's task snapshot reports it busy); shows an animated status
    // row in place of the footer. Owned exclusively by
    // SyncWorkingFromSnapshot() — never toggled from channel-message types.
    bool working_ = false;

    // When working_ last flipped to true; the status row turns this into the
    // animation phase, so the run starts at the first cell each time a process
    // goes busy. Only meaningful while working_ is true.
    std::chrono::steady_clock::time_point working_started_;

    // The spinner's own repaint cadence (see TickAnimations): when a frame was
    // last requested for it. UI thread only.
    std::chrono::steady_clock::time_point last_spinner_frame_;

    // Last time a task-tree snapshot was requested; throttles the periodic
    // refresh that catches background-task state changes (which send no
    // channel message when they are not the active task).
    std::chrono::steady_clock::time_point last_snapshot_request_;

    static void on_channel_adapter_send(void *instance, struct edc_channel_message *message);
};
} // namespace edc
