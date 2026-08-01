#pragma once

#include "edc_message.hpp"
#include "ftxui/component/component_base.hpp"
#include <memory>
#include <vector>

namespace edc
{

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
