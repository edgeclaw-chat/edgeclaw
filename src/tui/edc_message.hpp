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
    // task_id: id of the task that produced this message ("1", "2", ...), or
    // empty when the sender is not a task (e.g. user input). Kept for callers;
    // messages no longer render a "task N" tag.
    Message(const std::string_view content, time_t ts, const std::string_view task_id = "");
    virtual ~Message() = default;

    // The chat list knows a message's width before it renders (layout hands it
    // the box) and reports it here, because some content cannot be laid out
    // without it: ftxui's flow containers only break *between* their children,
    // so an unbroken run (CJK text, a long URL or path) is a single
    // unbreakable item that overflows its box and is clipped by the stencil.
    // Subclasses wrap their own text against BodyWidth().
    //
    // 0 means "width unknown"; the widget then leaves wrapping to ftxui, which
    // is what happens when a message is rendered outside a MessageList (unit
    // tests, previews).
    void SetRenderWidth(int cols);

    Message(const Message &rhs) = delete;
    Message &operator=(const Message &) = delete;

    bool OnEvent(ftxui::Event) override;
    ftxui::Element OnRender() override;

    // Blank rows the list reserves below each item, outside the item's own
    // box. The chat log used to append a separator element after every message;
    // the virtualized list (edc_message_list.*) adds this as a layout stride
    // instead of rendering a child for it, so the spacing costs no DOM node and
    // never gets painted with the message background.
    static constexpr int kSpacingRows = 1;

  protected:
    const std::string content_;
    time_t ts_;
    const std::string task_id_;
    int render_width_ = 0; // columns the item's box will have; 0 = unknown

    // Cached rendered element. content_ never changes after construction, so a
    // subclass builds its tree once and hands the same element back on every
    // later frame — but the tree *does* depend on the render width now (see
    // SetRenderWidth), so OnWidthChanged() drops it when that changes. The
    // subclasses that toggle between two shapes (Thought/Tool) drop the cache
    // when they switch.
    ::ftxui::Element body_;

    // Columns a subclass draws around its body (borders, rules, bullets); the
    // rest is what the body text may use. The default is the EMPTY border.
    static constexpr int kChromeCols = 2;

    // Width the body text may use at |chrome| columns of surrounding chrome,
    // or 0 when the render width is not known yet.
    int BodyWidth(int chrome = kChromeCols) const
    {
        return render_width_ > 0 ? std::max(1, render_width_ - chrome) : 0;
    }

    // Wrapping depends on the render width, so every cached element tree is
    // dropped when it changes. Subclasses with extra width-dependent caches
    // (ToolMessage's markdown body) extend this.
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
    // Collapsed content shows the first kPreviewLines lines, then a "..."
    // line and a clickable expand label; clicking toggles to the full text.
    // Markdown bodies are collapsed on the raw source (cheap, no parse) and
    // rendered only when shown in full.
    static constexpr size_t kPreviewLines = 14;

    std::string header_;
    bool expanded_ = false;
    bool markdown_ = false;
    // Line count of content_, computed once at construction: the collapsed
    // view needs it on every frame, and rescanning the whole body per frame was
    // O(content) work repeated for every tool message on screen.
    size_t line_count_ = 0;
    // First kPreviewLines lines plus the trailing "...", built once on demand.
    std::string preview_;
    bool preview_ready_ = false;
    ::ftxui::Element md_body_; /* lazy RenderMarkdown cache, only when markdown_ */
    ::ftxui::Box box_;
};

} // namespace edc
