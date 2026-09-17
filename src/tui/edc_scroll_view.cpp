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

// Shifts its child up by the view's offset and clips it to the box it was given, so
// the content can be taller than the space on screen. Three hooks have to move
// together: ComputeRequirement stops asking for the content's full height (or the
// parent sizes us to it and pushes our siblings off screen), SetBox lays the child
// out at that full height shifted by the offset (the child never learns it was
// scrolled), and Render narrows the stencil (or the rows pushed past the viewport
// would still be drawn over our siblings).
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
        // Layout has settled: children_[0]'s requirement now carries the height the
        // content was really laid out at, so commit it and clamp once against that
        // final height. Clamping inside SetBox would lock the reader to whichever
        // transient height a mid-iteration pass reported -- wrapping text reports
        // several while its flexbox converges, and the smallest would reset the
        // offset every frame.
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

ScrollView::ScrollView(::ftxui::Component content, ScrollOptions options, ::ftxui::Component pinned)
    : content_(std::move(content)), pinned_(std::move(pinned)), options_(std::move(options))
{
    Add(content_);
    if (pinned_)
        Add(pinned_);
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
        // The pinned header owns its own clicks (it folds the section) and the
        // content owns its (a collapsible entry toggles on press), so both get
        // first refusal before the wheel becomes a scroll.
        if (pinned_ && pinned_->OnEvent(event))
        {
            return true;
        }

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
            // At an end Scroll() reports unhandled, the contract the arrow keys
            // below rely on. Claiming it would not save a repaint: ftxui
            // invalidates the frame before OnEvent is called.
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
    // vscroll_indicator sits inside the scroll node so it sees the full content
    // height as its box and the visible window as the stencil, which is what it
    // measures the thumb against; outside, the two would be equal and it would draw
    // nothing. Sidebar sections turn it off: it permanently reserves a column, and
    // two stacked sections would end up with different row widths.
    auto content = content_->Render();
    if (options_.show_indicator)
        content = std::move(content) | ::ftxui::vscroll_indicator;

    ::ftxui::Element scrollable = std::make_shared<ScrollNode>(std::move(content), state_, options_.mode);

    if (!pinned_)
        return std::move(scrollable) | ::ftxui::reflect(box_);

    // The pinned part is outside the viewport: it keeps its own height and the scroll
    // node takes what is left. reflect() stays on the whole block, so a wheel over the
    // heading is still a wheel over this view.
    //
    // yflex_shrink is restated deliberately: a vbox does not propagate its children's
    // flex flags (see VBox::ComputeRequirement), so without it the section would hold on
    // to its full height and push the Todo section out of the pane.
    return ::ftxui::vbox({
               pinned_->Render(),
               std::move(scrollable),
           }) |
           ::ftxui::yflex_shrink | ::ftxui::reflect(box_);
}

} // namespace edc
