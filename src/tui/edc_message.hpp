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
    // |task_id| is the task that produced the message ("1", "2", ...), or empty
    // when the sender is not a task (a user input echoed before its task exists,
    // a command reply, an error). Messages no longer render a "task N" tag.
    Message(const std::string_view content, time_t ts, const std::string_view task_id = "");
    virtual ~Message() = default;

    // The task this bubble belongs to -- the key the chat filter matches on.
    // Empty means "no task": the filter hides such bubbles (command replies,
    // errors, and the user's own echo, which is drawn per-task from raw_input).
    const std::string &TaskId() const
    {
        return task_id_;
    }

    // The list knows the width before rendering (layout hands it the box) and
    // reports it here, because some content cannot be laid out without it:
    // ftxui's flow containers only break *between* children, so an unbroken run
    // (CJK text, a long URL or path) is one unbreakable item that overflows and
    // gets clipped by the stencil. Subclasses wrap against BodyWidth().
    // 0 = "width unknown": wrapping is left to ftxui, as when a message is
    // rendered outside a MessageList (tests, previews).
    void SetRenderWidth(int cols);

    Message(const Message &rhs) = delete;
    Message &operator=(const Message &rhs) = delete;

    bool OnEvent(ftxui::Event) override;
    ftxui::Element OnRender() override;

    // Blank rows reserved below each item, outside its own box. The log used to
    // append a separator element per message; the list adds this as a layout
    // stride instead, so the spacing costs no DOM node and is never painted with
    // the message background.
    static constexpr int kSpacingRows = 1;

  protected:
    const std::string content_;
    time_t ts_;
    // Task this bubble belongs to; empty = not a task's message. A user input's
    // bubble is empty here: the input predates its task, and the filter draws it
    // from the task's own raw_input instead (see ChatWidget::SetTaskInputs).
    const std::string task_id_;
    int render_width_ = 0; // columns the item's box will have; 0 = unknown

    // Cached rendered element: content_ never changes after construction, but the
    // tree does depend on the render width (see SetRenderWidth), so
    // OnWidthChanged() drops the cache when that changes, as do the subclasses
    // that toggle between two shapes (Thought/Tool).
    ::ftxui::Element body_;

    // Columns a subclass draws around its body (borders, rules, bullets); the
    // rest is what the body text may use. Default is the EMPTY border.
    static constexpr int kChromeCols = 2;

    // Width the body text may use, or 0 when the render width is unknown.
    int BodyWidth(int chrome = kChromeCols) const
    {
        return render_width_ > 0 ? std::max(1, render_width_ - chrome) : 0;
    }

    // Wrapping depends on the render width, so every cached tree is dropped when
    // it changes. Subclasses with extra width-dependent caches (ToolMessage's
    // markdown body) extend this.
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
    // bodies collapse on the raw source (no parse) and render only when expanded.
    static constexpr size_t kPreviewLines = 14;

    std::string header_;
    bool expanded_ = false;
    bool markdown_ = false;
    // Line count of content_, computed once: the collapsed view needs it every
    // frame, and rescanning the body per frame was O(content) per tool message.
    size_t line_count_ = 0;
    // First kPreviewLines lines plus the trailing "...", built once on demand.
    std::string preview_;
    bool preview_ready_ = false;
    ::ftxui::Element md_body_; /* lazy RenderMarkdown cache, only when markdown_ */
    ::ftxui::Box box_;
};

} // namespace edc
