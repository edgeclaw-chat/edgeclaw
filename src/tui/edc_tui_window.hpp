#include "edc_c_bridge.h"
#include "edc_clipboard.hpp"
#include <chrono>
#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <memory>
#include <string>

namespace edc
{
class ChatWidget;
class EdcInput;
class TaskWidget;
class TodoWidget;

// The animated "working" row shown in place of the footer while the foreground
// process runs: a bar of blocks that sweeps back and forth, brightness flowing
// along the lit run. |elapsed_ms| drives position and phase. Fixed cell count
// and one line tall, so swapping it for the footer never moves the layout.
// Declared here so tests can render it without a live agent.
::ftxui::Element WorkingIndicatorElement(long long elapsed_ms);

class TUIWindow : public ::ftxui::ComponentBase
{
  public:
    // |sandbox_override| mirrors --sandbox/--no-sandbox: -1 keeps the persisted
    // config.json value, 0 forces off, 1 forces on (per-run only). Runtime
    // toggling goes through /sandbox on|off.
    explicit TUIWindow(int sandbox_override = -1);
    virtual ~TUIWindow();

    TUIWindow(const TUIWindow &) = delete;
    TUIWindow &operator=(const TUIWindow &) = delete;

    bool OnEvent(ftxui::Event) override;
    ::ftxui::Element OnRender() override;

    // The agent actor runs on its own thread; adapter callbacks are marshalled
    // through this screen's task queue, so the tree is only touched by the
    // render thread.
    void SetScreen(::ftxui::ScreenInteractive *screen);

    // False when the agent failed to start (bad/missing config, bad master key,
    // no terminal for the passphrase). main() checks this and exits instead of
    // dereferencing a NULL agent.
    bool Ready() const
    {
        return agent != nullptr;
    }

    // Pace the window's own animations (the working spinner and the sidebar's
    // breathing: processing dots + the in_progress todo row) from the main-loop
    // iteration; see TickAnimations().
    void TickAnimations();

  private:
    // Append the current input as a user message and clear the box. A bare
    // "/connect" opens the provider dialog instead of sending anything.
    void Submit();
    void OpenConnect();
    // Persist + hot-apply the /connect dialog; true when it succeeded (the
    // dialog then closes).
    bool SubmitConnect();
    ::ftxui::Element RenderConnectModal();
    // Seed the input history from $EDC_HOME/.tui_history (a missing file is fine).
    void LoadInputHistory();
    void SaveInputHistory();
    // Ask the agent (on its scheduler thread) for a fresh task-tree snapshot.
    void RequestTaskSnapshot();
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
    void SyncWorkingFromSnapshot(const ::edc_c_task_snapshot *snap);
    // Switch the log to |session_id|: when the id changes the log is cleared and
    // that session's messages re-derived, so the log always belongs to the
    // active session. See SyncWorkingFromSnapshot for the snapshot source.
    void SyncChatSession(const char *session_id);
    // The left pane: chat log + input + footer/spinner.
    ::ftxui::Element RenderMain();
    // The working directory on the left; the model, sandbox state and "/help"
    // right-aligned within the chat pane's width (never into the sidebar).
    ::ftxui::Element RenderFooter() const;
    // Put the selection on the clipboard and raise the chip. Runs on the frame
    // after the drag was released, once FTXUI's selection is complete.
    void CopySelectionToClipboard();

    std::shared_ptr<edc::ChatWidget> chat_widget_;
    std::shared_ptr<edc::TaskWidget> task_widget_;
    std::shared_ptr<edc::TodoWidget> todo_widget_;
    // Multi-line input with soft wrapping (see EdcInput). Owned here, also
    // handed to |container_| for focus/events.
    std::shared_ptr<edc::EdcInput> input_;
    ::ftxui::Element prompt_;
    std::string input_text_;
    // Read once from the agent's config; shown at the right end of the footer
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

    // Set between the bracketed-paste ESC[200~ and ESC[201~, where Return is
    // inserted as a newline instead of submitting.
    bool in_paste_ = false;

    // The active session's id, used to detect /session switches. Starts at a
    // sentinel so the first snapshot triggers a rebuild.
    std::string current_session_id_ = "\x01";
    // That session's chat-transcript path (<workspace>/<session_id>/chat.jsonl),
    // resolved with the log. Empty when the session is unknown.
    std::string chat_log_path_;

    struct edc_agent *agent = NULL;
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

    // True while the foreground group's active task runs. Owned exclusively by
    // SyncWorkingFromSnapshot(), never toggled by channel messages.
    bool working_ = false;
    // The foreground active task's scheduler state as of the last snapshot ("" when
    // there is none): the todo list's current step follows it, so a step whose task
    // failed turns red with the task's own marker instead of staying "current".
    std::string fg_active_state_;
    // When working_ last flipped to true: the animation phase, so the run starts
    // at the first cell each time.
    std::chrono::steady_clock::time_point working_started_;
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

    static void on_channel_adapter_send(void *instance, struct edc_channel_message *message);
};
} // namespace edc
