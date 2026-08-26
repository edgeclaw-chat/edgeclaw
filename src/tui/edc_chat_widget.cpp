#include "edc_chat_widget.hpp"
#include "ftxui/component/component.hpp"
#include "ftxui/component/event.hpp"
#include "ftxui/component/mouse.hpp"
#include "ftxui/dom/elements.hpp"
#include "ftxui/dom/node.hpp"
#include "ftxui/dom/requirement.hpp"
#include "ftxui/screen/screen.hpp"
#include "ftxui/util/autoreset.hpp"
#include <algorithm>

namespace edc
{

namespace
{

constexpr int kWheelStep = 3;

// Shifts its child up by |scroll| lines and clips it to the box it was given,
// so the chat log can be taller than the space available on screen.
//
// This is deliberately not ftxui::yframe: yframe derives the offset by centring
// whichever element reported focus, which cannot express "scrolled to line N".
// Here the offset is the state we own, and layout only clamps it.
class ScrollNode : public ::ftxui::Node
{
  public:
    ScrollNode(::ftxui::Element child, int *scroll, int *content_height, int *viewport_height,
               bool *stick_to_bottom)
        : Node({std::move(child)}), scroll_(scroll), content_height_(content_height),
          viewport_height_(viewport_height), stick_to_bottom_(stick_to_bottom)
    {
    }

    void ComputeRequirement() override
    {
        Node::ComputeRequirement();
        requirement_ = children_[0]->requirement();

        // Publish the height the content actually wants, then stop asking the
        // parent for it: we render whatever height we are handed and scroll the
        // remainder.
        *content_height_ = requirement_.min_y;
        requirement_.min_y = 0;
        requirement_.flex_grow_y = 1;
        requirement_.flex_shrink_y = 1;
    }

    void SetBox(::ftxui::Box box) override
    {
        Node::SetBox(box);

        const int viewport = box.y_max - box.y_min + 1;
        *viewport_height_ = viewport;

        // The child is always laid out at its full height; only the visible
        // window moves.
        const int content = std::max(*content_height_, viewport);
        const int max_scroll = std::max(0, content - viewport);
        *scroll_ = *stick_to_bottom_ ? max_scroll : std::max(0, std::min(*scroll_, max_scroll));

        ::ftxui::Box child = box;
        child.y_min = box.y_min - *scroll_;
        child.y_max = child.y_min + content - 1;
        children_[0]->SetBox(child);
    }

    void Render(::ftxui::Screen &screen) override
    {
        const ::ftxui::AutoReset<::ftxui::Box> stencil(&screen.stencil,
                                                       ::ftxui::Box::Intersection(box_, screen.stencil));
        children_[0]->Render(screen);
    }

  private:
    int *scroll_;
    int *content_height_;
    int *viewport_height_;
    bool *stick_to_bottom_;
};

} // namespace

ChatWidget::ChatWidget()
{
    container_ = ::ftxui::Container::Vertical({});
}

void ChatWidget::AddMessage(std::shared_ptr<Message> message)
{
    container_->Add(message);
    container_->Add(::ftxui::Renderer([] { return ::ftxui::separatorEmpty(); }));

    // Leave scroll_offset_ alone: if we are following the tail, layout pins it
    // to the new bottom, otherwise the reader stays where they were.
}

int ChatWidget::MaxScroll() const
{
    return std::max(0, content_height_ - viewport_height_);
}

bool ChatWidget::Scroll(int delta)
{
    const int max_scroll = MaxScroll();
    const int before = scroll_offset_;
    scroll_offset_ = std::max(0, std::min(scroll_offset_ + delta, max_scroll));
    stick_to_bottom_ = scroll_offset_ >= max_scroll;
    return scroll_offset_ != before;
}

bool ChatWidget::ScrollToTop()
{
    const int before = scroll_offset_;
    scroll_offset_ = 0;
    stick_to_bottom_ = MaxScroll() == 0;
    return scroll_offset_ != before;
}

bool ChatWidget::ScrollToBottom()
{
    const int before = scroll_offset_;
    scroll_offset_ = MaxScroll();
    stick_to_bottom_ = true;
    return scroll_offset_ != before;
}

bool ChatWidget::OnEvent(::ftxui::Event event)
{
    if (event.is_mouse())
    {
        // Messages own their clicks (ThoughtMessage toggles on press), so they
        // get first refusal before the wheel becomes a scroll.
        if (container_->OnEvent(event))
        {
            return true;
        }

        if (!box_.Contain(event.mouse().x, event.mouse().y))
        {
            return false;
        }

        if (event.mouse().button == ::ftxui::Mouse::WheelUp)
        {
            return Scroll(-kWheelStep);
        }

        if (event.mouse().button == ::ftxui::Mouse::WheelDown)
        {
            return Scroll(kWheelStep);
        }

        return false;
    }

    // Report unhandled when the view cannot move any further, so arrow keys
    // still let an enclosing container hand focus to a sibling.
    if (event == ::ftxui::Event::ArrowUp)
    {
        return Scroll(-1);
    }

    if (event == ::ftxui::Event::ArrowDown)
    {
        return Scroll(1);
    }

    if (event == ::ftxui::Event::PageUp)
    {
        return Scroll(-std::max(1, viewport_height_));
    }

    if (event == ::ftxui::Event::PageDown)
    {
        return Scroll(std::max(1, viewport_height_));
    }

    if (event == ::ftxui::Event::Home)
    {
        return ScrollToTop();
    }

    if (event == ::ftxui::Event::End)
    {
        return ScrollToBottom();
    }

    return container_->OnEvent(event);
}

::ftxui::Element ChatWidget::OnRender()
{
    // vscroll_indicator sits inside the scroll node so that it sees the full
    // content height as its own box and the visible window as the stencil,
    // which is what it measures the thumb against.
    auto content = container_->Render() | ::ftxui::vscroll_indicator;

    ::ftxui::Element scrollable = std::make_shared<ScrollNode>(std::move(content), &scroll_offset_, &content_height_,
                                                              &viewport_height_, &stick_to_bottom_);

    return std::move(scrollable) | ::ftxui::reflect(box_) | ::ftxui::bgcolor(::ftxui::Color::RGB(10, 10, 10));
}

} // namespace edc
