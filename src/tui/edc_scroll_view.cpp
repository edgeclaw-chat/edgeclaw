#include "edc_scroll_view.hpp"
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

// Shifts its child up by the view's offset and clips it to the box it was
// given, so the content can be taller than the space available on screen.
//
// Three things have to happen together for this to scroll:
//   ComputeRequirement  stop asking the parent for the content's full height,
//                       or the parent would size us to it and push our siblings
//                       off screen
//   SetBox              lay the child out at that full height, shifted up by
//                       the offset -- the child never learns it was scrolled
//   Render              narrow the screen stencil, or the rows we pushed past
//                       the viewport would still be drawn over our siblings
class ScrollNode : public ::ftxui::Node
{
  public:
    ScrollNode(::ftxui::Element child, std::shared_ptr<ScrollState> state)
        : Node({std::move(child)}), state_(std::move(state))
    {
    }

    void ComputeRequirement() override
    {
        Node::ComputeRequirement();
        requirement_ = children_[0]->requirement();

        // Publish the height the content actually wants, then stop asking the
        // parent for it: we render whatever height we are handed and scroll the
        // remainder.
        state_->content_height = requirement_.min_y;
        requirement_.min_y = 0;
        requirement_.flex_grow_y = 1;
        requirement_.flex_shrink_y = 1;
    }

    void SetBox(::ftxui::Box box) override
    {
        Node::SetBox(box);

        const int viewport = box.y_max - box.y_min + 1;
        state_->viewport_height = viewport;
        state_->ClampForLayout();

        // The child is always laid out at its full height; only the visible
        // window moves.
        const int content = std::max(state_->content_height, viewport);

        ::ftxui::Box child = box;
        child.y_min = box.y_min - state_->offset;
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
    std::shared_ptr<ScrollState> state_;
};

} // namespace

ScrollView::ScrollView(::ftxui::Component content) : content_(std::move(content))
{
    Add(content_);
}

bool ScrollView::Scroll(int delta)
{
    return state_->ScrollBy(delta);
}

bool ScrollView::ScrollToTop()
{
    return state_->ScrollToTop();
}

bool ScrollView::ScrollToBottom()
{
    return state_->ScrollToBottom();
}

bool ScrollView::OnEvent(::ftxui::Event event)
{
    if (event.is_mouse())
    {
        // The content owns its clicks (a collapsible entry toggles on press),
        // so it gets first refusal before the wheel becomes a scroll.
        if (content_->OnEvent(event))
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
        return Scroll(-std::max(1, state_->viewport_height));
    }

    if (event == ::ftxui::Event::PageDown)
    {
        return Scroll(std::max(1, state_->viewport_height));
    }

    if (event == ::ftxui::Event::Home)
    {
        return ScrollToTop();
    }

    if (event == ::ftxui::Event::End)
    {
        return ScrollToBottom();
    }

    return content_->OnEvent(event);
}

::ftxui::Element ScrollView::OnRender()
{
    // vscroll_indicator sits inside the scroll node so that it sees the full
    // content height as its own box and the visible window as the stencil,
    // which is what it measures the thumb against. Outside, the two would be
    // equal and it would draw nothing.
    auto content = content_->Render() | ::ftxui::vscroll_indicator;

    ::ftxui::Element scrollable = std::make_shared<ScrollNode>(std::move(content), state_);

    return std::move(scrollable) | ::ftxui::reflect(box_);
}

} // namespace edc
