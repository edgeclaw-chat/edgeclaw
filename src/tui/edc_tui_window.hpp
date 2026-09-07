#include "edc_c_bridge.h"
#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <chrono>
#include <memory>
#include <string>

namespace edc
{
class ChatWidget;
class TaskWidget;
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
    // Ask the agent (on its scheduler thread) for a fresh task-tree snapshot.
    void RequestTaskSnapshot();
    // Render the left pane (chat log + input + footer/spinner).
    ::ftxui::Element RenderMain();

    std::shared_ptr<edc::ChatWidget> chat_widget_;
    std::shared_ptr<edc::TaskWidget> task_widget_;
    ::ftxui::Component input_;
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

    struct edc_agent *agent = NULL;
    ::ftxui::ScreenInteractive *screen_ = nullptr;

    // True while an agent turn is in flight; shows an animated status row
    // between the input and the footer. Cleared only when the turn's final
    // RESP message arrives.
    bool working_ = false;

    // Last time a task-tree snapshot was requested; throttles the periodic
    // refresh that catches background-task state changes (which send no
    // channel message when they are not the active task).
    std::chrono::steady_clock::time_point last_snapshot_request_;

    static void on_channel_adapter_send(void *instance, struct edc_channel_message *message);
};
} // namespace edc
