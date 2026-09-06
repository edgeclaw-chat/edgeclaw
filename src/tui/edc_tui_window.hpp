#include "edc_tui_agent_bridge.h"
#include <ftxui/component/component.hpp>
#include <memory>
#include <string>

namespace edc
{
class ChatWidget;
class TUIWindow : public ::ftxui::ComponentBase
{
  public:
    TUIWindow();
    virtual ~TUIWindow();

    TUIWindow(const TUIWindow &) = delete;
    TUIWindow &operator=(const TUIWindow &) = delete;

    bool OnEvent(ftxui::Event) override;
    ::ftxui::Element OnRender() override;

  private:
    // Append the current input as a user message and clear the box.
    void Submit();

    std::shared_ptr<edc::ChatWidget> chat_widget_;
    ::ftxui::Component input_;
    ::ftxui::Element footer_;
    ::ftxui::Element prompt_;
    std::string input_text_;

    // Routes keyboard focus between the chat log and the input; the input
    // starts focused so typing works without a Tab first. The selector int must
    // outlive the container that borrows it, so it is declared first.
    int focused_ = 1;
    ::ftxui::Component container_;

    // True while consuming a bracketed-paste block (between ESC[200~ and
    // ESC[201~). Inside it, Return/Character("\n") are inserted as newlines
    // rather than submitted, so a pasted multi-line block becomes one draft.
    bool in_paste_ = false;

    struct edc_agent *agent = NULL;
};
} // namespace edc
