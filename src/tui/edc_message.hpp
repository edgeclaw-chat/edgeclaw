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

  private:
    // Markdown parse result. content_ never changes after construction, so the
    // parse happens once, lazily, on the first render; later frames reuse the
    // same element tree (ftxui re-runs layout every Render pass, so a cached
    // tree is safe to draw repeatedly).
    ::ftxui::Element body_;
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
    ToolMessage(const std::string_view header, const std::string_view content, time_t ts, bool markdown = false);
    bool OnEvent(ftxui::Event) override;
    ftxui::Element OnRender() override;

  private:
    // Collapsed content shows the first kPreviewLines lines, then a "..."
    // line and a clickable expand label; clicking toggles to the full text.
    // Markdown bodies are collapsed on the raw source (cheap, no parse) and
    // rendered only when shown in full.
    static constexpr size_t kPreviewLines = 14;

    std::string header_;
    bool expanded_ = false;
    bool markdown_ = false;
    ::ftxui::Element md_body_; /* lazy RenderMarkdown cache, only when markdown_ */
    ::ftxui::Box box_;
};

} // namespace edc

