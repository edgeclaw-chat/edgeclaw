#pragma once

#include "ftxui/component/component_base.hpp"
#include "ftxui/screen/box.hpp"
#include <cstddef>
#include <string_view>

namespace edc
{

class Message : public ::ftxui::ComponentBase
{
  public:
    Message(const std::string_view content, time_t ts);
    virtual ~Message() = default;

    Message(const Message &rhs) = delete;
    Message &operator=(const Message &) = delete;

    bool OnEvent(ftxui::Event) override;
    ftxui::Element OnRender() override;

  protected:
    const std::string content_;
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

class UserMessage : public Message
{
  public:
    UserMessage(const std::string_view content, time_t ts);
    bool OnEvent(ftxui::Event) override;
    ftxui::Element OnRender() override;
};

class ToolMessage : public Message
{
  public:
    ToolMessage(const std::string_view header, const std::string_view content, time_t ts);
    bool OnEvent(ftxui::Event) override;
    ftxui::Element OnRender() override;

  private:
    // Collapsed content shows the first kPreviewLines lines, then a "..."
    // line and a clickable expand label; clicking toggles to the full text.
    static constexpr size_t kPreviewLines = 14;

    std::string header_;
    bool expanded_ = false;
    ::ftxui::Box box_;
};

} // namespace edc

