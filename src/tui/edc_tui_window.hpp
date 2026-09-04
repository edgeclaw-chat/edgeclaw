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
    ::ftxui::Component footer_;
    std::string input_text_;

    // Routes keyboard focus between the chat log and the input; the input
    // starts focused so typing works without a Tab first. The selector int must
    // outlive the container that borrows it, so it is declared first.
    int focused_ = 1;
    ::ftxui::Component container_;
};
} // namespace edc
