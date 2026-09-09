#pragma once

#include "edc_message.hpp"
#include "edc_scroll_view.hpp"
#include "ftxui/component/component_base.hpp"
#include <memory>

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

    // The chat log fills whatever height its parent grants and scrolls the
    // overflow with the mouse wheel, but it is deliberately NOT keyboard
    // focusable: ↑/↓ are reserved for the message editor, and no key needs
    // to reach the log. Focusable() is therefore omitted — it inherits
    // false from the (non-focusable) ScrollView the widget wraps.

    // Move the viewport by |delta| lines. Scrolling away from the bottom
    // releases the auto-follow, reaching the bottom again re-arms it.
    // Returns false when the view was already against that end, so an
    // enclosing container can use the key to move focus instead.
    bool Scroll(int delta);
    bool ScrollToTop();
    bool ScrollToBottom();

  private:
    ::ftxui::Component container_;
    std::shared_ptr<ScrollView> scroll_view_;
};

} // namespace edc
