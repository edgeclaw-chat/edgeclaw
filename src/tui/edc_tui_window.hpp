#include <ftxui/component/component.hpp>
#include <memory>

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
    std::shared_ptr<edc::ChatWidget> chat_widget_;
    ::ftxui::Component input_;
    ::ftxui::Component footer_;
};
} // namespace edc

