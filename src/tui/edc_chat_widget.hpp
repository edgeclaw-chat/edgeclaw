#pragma once

#include "edc_message.hpp"
#include "ftxui/component/component_base.hpp"
#include "ftxui/screen/box.hpp"
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

    // The chat log fills whatever height its parent grants and scrolls the
    // overflow, so it must be reachable by keyboard on its own.
    bool Focusable() const override
    {
        return true;
    }

    // Move the viewport by |delta| lines. Scrolling away from the bottom
    // releases the auto-follow, reaching the bottom again re-arms it.
    // Returns false when the view was already against that end, so an
    // enclosing container can use the key to move focus instead.
    bool Scroll(int delta);
    bool ScrollToTop();
    bool ScrollToBottom();

  private:
    int MaxScroll() const;

    std::vector<std::shared_ptr<ftxui::ComponentBase>> messages_;
    ::ftxui::Component container_;

    // Written by the scroll node during layout, read back when handling events.
    int scroll_offset_ = 0;
    int content_height_ = 0;
    int viewport_height_ = 0;
    bool stick_to_bottom_ = true;
    ::ftxui::Box box_;
};

} // namespace edc
