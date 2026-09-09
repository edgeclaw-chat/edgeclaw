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
    ScrollNode(::ftxui::Element child, std::shared_ptr<ScrollState> state, ScrollMode mode)
        : Node({std::move(child)}), state_(std::move(state)), mode_(mode)
    {
    }

    void ComputeRequirement() override
    {
        Node::ComputeRequirement();
        requirement_ = children_[0]->requirement();

        if (mode_ == ScrollMode::kFitContent)
        {
            // Report the content's natural height (no zero-minimum, no grow):
            // the view claims exactly as much as its content when the column
            // has room. When the siblings below leave less, the vbox shrinks
            // this (flex_shrink_y = 1) before touching them, and the view
            // scrolls the overflow inside the smaller viewport.
            requirement_.flex_shrink_y = 1;
            return;
        }

        // Stop asking the parent for the content's full height: we render
        // whatever height we are handed and scroll the remainder.
        requirement_.min_y = 0;
        requirement_.flex_grow_y = 1;
        requirement_.flex_shrink_y = 1;
    }

    void SetBox(::ftxui::Box box) override
    {
        Node::SetBox(box);

        const int viewport = box.y_max - box.y_min + 1;
        state_->viewport_height = viewport;

        // Layout the content at the offset we currently own. This is a
        // provisional pass: ftxui iterates the layout several times per frame
        // and the content height is only final once those iterations finish,
        // so the authoritative clamp happens in Render() below.
        const int content = std::max(state_->content_height, viewport);

        ::ftxui::Box child = box;
        child.y_min = box.y_min - state_->offset;
        child.y_max = child.y_min + content - 1;
        children_[0]->SetBox(child);
    }

    void Render(::ftxui::Screen &screen) override
    {
        // Layout has settled (or hit ftxui's iteration cap): children_[0]'s
        // requirement now carries the height the content was really laid out
        // at. Commit it and clamp the viewport once, against that final
        // height. Clamping inside SetBox would lock the reader to whichever
        // transient height a mid-iteration pass reported -- wrapping text
        // reports several different heights while its flexbox converges, and
        // the smallest one would reset the scroll offset every frame, making
        // it impossible to scroll down past the first screenful.
        const int viewport = state_->viewport_height;
        const int content = std::max(children_[0]->requirement().min_y, viewport);
        state_->content_height = content;
        state_->ClampForLayout();

        // The provisional SetBox passes ran against the previous frame's
        // committed height and offset, so re-lay the content out against the
        // committed values. This guarantees the drawing always matches the
        // geometry the clamp decided (growing content while sticky, shrinking
        // content, or the transient heights seen earlier in this frame).
        ::ftxui::Box child = box_;
        child.y_min = box_.y_min - state_->offset;
        child.y_max = child.y_min + content - 1;
        children_[0]->SetBox(child);

        const ::ftxui::AutoReset<::ftxui::Box> stencil(&screen.stencil,
                                                       ::ftxui::Box::Intersection(box_, screen.stencil));
        children_[0]->Render(screen);
    }

  private:
    std::shared_ptr<ScrollState> state_;
    ScrollMode mode_;
};

} // namespace

ScrollView::ScrollView(::ftxui::Component content, ScrollOptions options)
    : content_(std::move(content)), options_(std::move(options))
{
    Add(content_);
    // A chat log opens pinned to its tail; a sidebar section opens at its top
    // (the active task) and never auto-follows refreshes.
    state_->stick_to_bottom = options_.follow_tail;
    state_->follow_tail = options_.follow_tail;
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
    //
    // The pane has no border, and the thumb down the right edge was once
    // dropped because it read as a frame slicing the pane in two. In practice
    // the bar is the only cue that the log continues past the viewport and
    // where in it the reader is, so it is drawn deliberately again.
    //
    // Sidebar sections turn the indicator off: it permanently reserves a
    // column, and two stacked sections would otherwise end up with different
    // row widths (one losing a column to the thumb, the other not).
    auto content = content_->Render();
    if (options_.show_indicator)
        content = std::move(content) | ::ftxui::vscroll_indicator;

    ::ftxui::Element scrollable =
        std::make_shared<ScrollNode>(std::move(content), state_, options_.mode);

    return std::move(scrollable) | ::ftxui::reflect(box_);
}

} // namespace edc
