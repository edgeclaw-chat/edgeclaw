#pragma once

#include "ftxui/component/component_base.hpp"
#include "ftxui/screen/box.hpp"
#include <algorithm>
#include <cstddef>
#include <string_view>

namespace edc
{

class Message : public ::ftxui::ComponentBase
{
  public:
    // |task_id| is the task that produced the message, or empty when the sender is
    // not a task (an echoed user input before its task exists, a command reply, an
    // error).
    Message(const std::string_view content, time_t ts, const std::string_view task_id = "");
    virtual ~Message() = default;

    // The task this bubble belongs to -- the key the chat filter matches on. Empty
    // means "no task": the filter hides such bubbles (command replies, errors, and
    // the user's own echo, drawn per-task from raw_input).
    const std::string &TaskId() const
    {
        return task_id_;
    }

    // The list knows the width before rendering and reports it here, because
    // ftxui's flow containers only break between children: an unbroken run (CJK
    // text, a long URL or path) is one unbreakable item that overflows and gets
    // clipped by the stencil. Subclasses wrap against BodyWidth().
    // 0 = "width unknown" (e.g. rendered outside a MessageList).
    void SetRenderWidth(int cols);

    Message(const Message &rhs) = delete;
    Message &operator=(const Message &rhs) = delete;

    bool OnEvent(ftxui::Event) override;
    ftxui::Element OnRender() override;

    // Blank rows reserved below each item, outside its own box: the list adds this
    // as a layout stride, so the spacing costs no DOM node and is never painted
    // with the message background.
    static constexpr int kSpacingRows = 1;

  protected:
    const std::string content_;
    time_t ts_;
    // Task this bubble belongs to; empty = not a task's message. A user input's
    // bubble is empty here: the filter draws it from the task's own raw_input.
    const std::string task_id_;
    int render_width_ = 0; // columns the item's box will have; 0 = unknown

    // Cached rendered element: content_ never changes, but the tree depends on the
    // render width, so OnWidthChanged() drops the cache (and subclasses toggle
    // their two shapes).
    ::ftxui::Element body_;

    // Columns a subclass draws around its body; the rest is for the body text.
    static constexpr int kChromeCols = 2;

    // Width the body text may use, or 0 when the render width is unknown.
    int BodyWidth(int chrome = kChromeCols) const
    {
        return render_width_ > 0 ? std::max(1, render_width_ - chrome) : 0;
    }

    // Wrapping depends on the render width, so every cached tree is dropped when it
    // changes. Subclasses with extra width-dependent caches extend this.
    virtual void OnWidthChanged();
};

class ThoughtMessage : public Message
{
  public:
    ThoughtMessage(const std::string_view content, time_t ts, const std::string_view task_id = "");

    bool OnEvent(ftxui::Event) override;
    ftxui::Element OnRender() override;

  private:
    bool thought_expanded_ = false;
    ::ftxui::Box box_;
};

class UserMessage : public Message
{
  public:
    UserMessage(const std::string_view content, time_t ts, const std::string_view task_id = "");

    bool OnEvent(ftxui::Event) override;
    ftxui::Element OnRender() override;
};

class ToolMessage : public Message
{
  public:
    ToolMessage(const std::string_view header, const std::string_view content, time_t ts, bool markdown = false,
                const std::string_view task_id = "");
    bool OnEvent(ftxui::Event) override;
    ftxui::Element OnRender() override;

  protected:
    // The cached markdown body depends on the render width as well.
    void OnWidthChanged() override;

  private:
    // Collapsed, the content shows the first kPreviewLines lines plus a "..."
    // line and a clickable expand label; clicking shows the full text. Markdown
    // bodies collapse on the raw source and render only when expanded.
    static constexpr size_t kPreviewLines = 14;

    std::string header_;
    bool expanded_ = false;
    bool markdown_ = false;
    // Line count of content_, computed once (the collapsed view needs it every
    // frame; rescanning per frame was O(content) per tool message).
    size_t line_count_ = 0;
    // First kPreviewLines lines plus the trailing "...", built once on demand.
    std::string preview_;
    bool preview_ready_ = false;
    ::ftxui::Element md_body_; /* lazy RenderMarkdown cache, only when markdown_ */
    ::ftxui::Box box_;
};

} // namespace edc
