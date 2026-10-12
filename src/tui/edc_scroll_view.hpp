#pragma once

#include "edc_widget.hpp" /* ui::IScrollable */
#include "ftxui/component/component_base.hpp"
#include "ftxui/screen/box.hpp"
#include <algorithm>
#include <memory>

namespace edc
{

// Channel between a component (owns it, lives across frames) and the scroll node
// it builds each frame (borrows it, dies with the frame): FTXUI rebuilds the
// whole tree every frame and Node is non-copyable, so a node cannot own the
// scroll position. Layout publishes what it measured here, input reads it back,
// and every move goes through MoveTo so the two can never disagree about where
// the ends are.
struct ScrollState
{
    // Written when handling input, read during layout.
    int offset = 0;
    bool stick_to_bottom = true;
    // Whether reaching the bottom re-arms auto-follow of new content. Chat keeps
    // it on (the log pins to its tail); the sidebar tree keeps it off so a
    // refresh never yanks the reader down.
    bool follow_tail = true;

    // Written during layout, read when handling input -- a frame stale by the
    // time an event arrives, which is why MaxScroll() is zero until the first
    // frame has been laid out.
    int content_height = 0;
    int viewport_height = 0;

    int MaxScroll() const
    {
        return std::max(0, content_height - viewport_height);
    }

    // Called once per frame from ScrollNode::Render, after layout has settled and
    // the content height is final. It must NOT run inside SetBox: ftxui iterates
    // the layout several times per frame and wrapping content reports a different
    // height on each pass, so clamping against a transient one would wipe the
    // reader's offset every frame.
    void ClampForLayout()
    {
        offset = (follow_tail && stick_to_bottom) ? MaxScroll() : std::max(0, std::min(offset, MaxScroll()));
    }

    // Input. Each returns whether the view actually moved, so a scroll already
    // against that end releases the key to an enclosing container.
    bool ScrollBy(int delta)
    {
        return MoveTo(offset + delta);
    }

    bool ScrollToTop()
    {
        return MoveTo(0);
    }

    bool ScrollToBottom()
    {
        return MoveTo(MaxScroll());
    }

  private:
    // Scrolling away from the bottom releases auto-follow; reaching it re-arms it
    // (when the view follows the tail at all).
    bool MoveTo(int target)
    {
        const int before = offset;
        offset = std::max(0, std::min(target, MaxScroll()));
        stick_to_bottom = follow_tail && offset >= MaxScroll();
        return offset != before;
    }
};

// How the scroll node asks its parent for height.
enum class ScrollMode
{
    // Claims the whole height the parent grants and scrolls the overflow: the
    // chat log, the only flexible element in its column.
    kFill,

    // Reports the content's natural height and never grows, but shrinks before
    // the siblings below it: the sidebar task tree, which scrolls its overflow
    // instead of clipping it or squeezing the todo list out.
    kFitContent,
};

struct ScrollOptions
{
    ScrollMode mode = ScrollMode::kFill;
    // Draw the right-edge thumb while the content overflows. Costs one column of
    // width permanently, so narrow panes with two stacked sections turn it off to
    // keep their row widths equal.
    bool show_indicator = true;
    // Pin new content to the tail while at the bottom (chat semantics). The task
    // tree turns it off: a refresh must never move the viewport.
    bool follow_tail = true;
    // Whether the view can take keyboard focus (arrow/page scrolling). Both
    // current users opt out, so arrows never get stolen from the message editor.
    bool focusable = true;
};

// Gives its content as much height as it asks for and shows a viewport-sized
// window onto it, scrolled by the offset this view owns. Deliberately not
// ftxui::yframe: yframe derives the offset by centring whichever element
// reported focus, which cannot express "scrolled to line N".
class ScrollView : public ::ftxui::ComponentBase, public ui::IScrollable
{
  public:
    // |pinned|, when given, is drawn above the viewport and never scrolls (the
    // sidebar's section header). The wheel still moves the content whenever the
    // pointer is anywhere in the view, the pinned part included.
    explicit ScrollView(::ftxui::Component content, ScrollOptions options = {}, ::ftxui::Component pinned = nullptr);
    ScrollView(ScrollView const &) = delete;
    ScrollView &operator=(ScrollView const &) = delete;

    bool OnEvent(::ftxui::Event) override;
    ::ftxui::Element OnRender() override;

    bool Focusable() const override
    {
        return options_.focusable;
    }

    // ui::IScrollable: move the viewport by |delta| lines. Returns false when the
    // view was already against that end, so an enclosing container can use the key
    // to move focus instead.
    bool Scroll(int delta) override;
    bool ScrollToTop() override;
    bool ScrollToBottom() override;

    // True while the viewport follows the tail, so new content reveals itself
    // instead of leaving the reader behind.
    bool StickingToBottom() const
    {
        return state_->stick_to_bottom;
    }

    // Viewport height the last frame laid out (0 before the first frame). Lets a
    // caller -- a test, mainly -- see how much height this section was granted
    // instead of re-deriving the layout's arbitration.
    int ViewportHeight() const
    {
        return state_->viewport_height;
    }

  private:
    ::ftxui::Component content_;
    // Drawn above the viewport, outside the scroll offset (usually empty).
    ::ftxui::Component pinned_;
    ScrollOptions options_;
    std::shared_ptr<ScrollState> state_ = std::make_shared<ScrollState>();
    ::ftxui::Box box_;
};

} // namespace edc
