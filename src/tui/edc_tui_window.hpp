#include "edc_c_bridge.h"
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
    TUIWindow();
    virtual ~TUIWindow();

    TUIWindow(const TUIWindow &) = delete;
    TUIWindow &operator=(const TUIWindow &) = delete;

    bool OnEvent(ftxui::Event) override;
    ::ftxui::Element OnRender() override;

    // The agent channel actor runs on its own thread; adapter callbacks are
    // marshalled to the UI thread through this screen's task queue so the
    // ftxui component tree is only touched from the render thread.
    void SetScreen(::ftxui::ScreenInteractive *screen);

  private:
    // Append the current input as a user message and clear the box.
    void Submit();
    // Seed the input box's recall history from $EDC_HOME/.tui_history (tolerates
    // a missing file).
    void LoadInputHistory();
    // Rewrite $EDC_HOME/.tui_history from the input box's current history.
    void SaveInputHistory();
    // Ask the agent (on its scheduler thread) for a fresh task-tree snapshot.
    void RequestTaskSnapshot();
    // Ask for a snapshot that also carries the foreground group's conversation
    // messages, then refill the chat log from it (once, at startup, so a
    // restored session shows its history instead of an empty log).
    void RequestHistorySnapshot();
    // Seed the chat log from a snapshot's foreground-group history. Each entry
    // carries the id of the task that produced it, so the refilled messages
    // keep their "task N" tags. No-op after the first snapshot that carried a
    // "messages" array (history_seeded_).
    void SeedChatFromSnapshot(const ::edc_c_task_snapshot *snap);
    // Re-evaluate the animated status from a freshly parsed task snapshot:
    // "processing" is shown exactly while the foreground group's active task
    // is running, regardless of which channel messages arrived.
    void SyncWorkingFromSnapshot(const ::edc_c_task_snapshot *snap);
    // Render the left pane (chat log + input + footer/spinner).
    ::ftxui::Element RenderMain();

    std::shared_ptr<edc::ChatWidget> chat_widget_;
    std::shared_ptr<edc::TaskWidget> task_widget_;
    std::shared_ptr<edc::TodoWidget> todo_widget_;
    // Multi-line input with soft (visual) line wrapping: long drafts wrap to
    // the input width instead of overflowing, and the caret navigates the
    // wrapped rows. Owned here; also handed to |container_| for focus/events.
    std::shared_ptr<edc::EdcInput> input_;
    ::ftxui::Element footer_;
    ::ftxui::Element prompt_;
    std::string input_text_;

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

    // True once the chat log has been refilled from the foreground group's
    // history. The history arrives attached to a one-shot snapshot requested
    // in the constructor; polling snapshots carry no messages, so this must
    // latch on the first snapshot that does.
    bool history_seeded_ = false;

    struct edc_agent *agent = NULL;
    ::ftxui::ScreenInteractive *screen_ = nullptr;

    // True while the foreground group's active task is running (the
    // scheduler's task snapshot reports it busy); shows an animated status
    // row in place of the footer. Owned exclusively by
    // SyncWorkingFromSnapshot() — never toggled from channel-message types.
    bool working_ = false;

    // When working_ last flipped to true; the status row turns this into the
    // animation phase, so the run starts at the first cell each time a process
    // goes busy. Only meaningful while working_ is true.
    std::chrono::steady_clock::time_point working_started_;

    // Last time a task-tree snapshot was requested; throttles the periodic
    // refresh that catches background-task state changes (which send no
    // channel message when they are not the active task).
    std::chrono::steady_clock::time_point last_snapshot_request_;

    static void on_channel_adapter_send(void *instance, struct edc_channel_message *message);
};
} // namespace edc
