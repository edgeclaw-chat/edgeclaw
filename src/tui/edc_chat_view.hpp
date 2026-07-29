#include "ftxui/component/component_base.hpp"
#include <string_view>

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

class ChatView : public ::ftxui::ComponentBase
{
};
} // namespace edc

