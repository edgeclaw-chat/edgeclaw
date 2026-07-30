#include "ftxui/component/component_base.hpp"
#include "ftxui/screen/box.hpp"
#include <memory>
#include <string_view>
#include <vector>

namespace edc
{
class Message : public ::ftxui::ComponentBase
{
  public:
    enum class Role
    {
        User,
        Assistant,
        Tool,
        System
    };

    Message(const std::string_view content, Role role, time_t ts);
    virtual ~Message() = default;

    Message(const Message &rhs) = delete;
    Message &operator=(const Message &) = delete;

    Role role() const;

    bool OnEvent(ftxui::Event) override;
    ftxui::Element OnRender() override;

  protected:
    const std::string content_;
    Role role_;
    time_t ts_;
};

class ThoughtMessage : public Message
{
  public:
    ThoughtMessage(const std::string_view content, time_t ts);

    bool OnEvent(ftxui::Event) override;
    ftxui::Element OnRender() override;

  private:
    bool thought_expanded_ = false;
    ::ftxui::Box box_;
};

class ChatWidget : public ::ftxui::ComponentBase
{
  public:
    ChatWidget();
    ChatWidget(ChatWidget const &) = delete;
    ChatWidget &operator=(ChatWidget const &) = delete;

    void AddMessage(std::shared_ptr<Message> message);

    bool OnEvent(ftxui::Event) override;
    ftxui::Element OnRender() override;

  private:
    std::vector<std::shared_ptr<ftxui::ComponentBase>> messages_;
    ::ftxui::Component container_;
};
} // namespace edc
