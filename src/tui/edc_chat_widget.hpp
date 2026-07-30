#include "ftxui/component/component_base.hpp"
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
        Thought,
        Tool,
        System
    };

    Message(const std::string_view content, Role role, time_t ts);
    Message(Message &&rhs);

    Message(const Message &rhs) = delete;
    Message &operator=(const Message &) = delete;

    bool OnEvent(ftxui::Event) override;
    ftxui::Element OnRender() override;

  private:
    const std::string content_;
    Role role_;
    time_t ts_;
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

