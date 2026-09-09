#pragma once

#include "ftxui/component/component_base.hpp"
#include "ftxui/screen/box.hpp"
#include <algorithm>
#include <memory>

namespace edc
{

// The channel between a component (owns it, lives across frames) and the
// scroll node it builds each frame (borrows it, dies with the frame).
//
// FTXUI rebuilds the whole element tree every frame and Node is non-copyable,
// so a node cannot hold the scroll position itself. Layout publishes what it
// measured here; input reads it back to decide where the viewport may go.
//
// Every move goes through MoveTo, so layout and input can never disagree about
// where the ends are.
struct ScrollState
{
    // Written when handling input, read during layout.
    int offset = 0;
    bool stick_to_bottom = true;
    // Whether scrolling to the bottom re-arms the auto-follow of new
    // content. Chat keeps this on (the log pins to its tail); the sidebar
    // task tree keeps it off so a refresh never yanks the reader to the
    // bottom — the viewport stays where the user left it.
    bool follow_tail = true;

    // Written during layout, read when handling input. Both are a frame stale
    // by the time an event arrives, which is why MaxScroll() reads zero until
    // the first frame has been laid out.
    int content_height = 0;
    int viewport_height = 0;

    int MaxScroll() const
    {
        return std::max(0, content_height - viewport_height);
    }

    // Called once per frame from ScrollNode::Render, i.e. after layout has
    // finished iterating and the height reported by the content is final.
    // Pin to the tail while following it, otherwise just keep the reader's
    // position in range as the content grows or shrinks.
    //
    // It must NOT run inside SetBox: ftxui runs the layout several times per
    // frame and wrapping content reports a different height on each pass while
    // converging. Clamping against a transient mid-iteration height would wipe
    // the reader's offset every frame.
    void ClampForLayout()
    {
        offset = (follow_tail && stick_to_bottom) ? MaxScroll() : std::max(0, std::min(offset, MaxScroll()));
    }

    // Input. Each returns whether the view actually moved, so a scroll that is
    // already against that end releases the key to an enclosing container.
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
    // Scrolling away from the bottom releases the auto-follow; reaching the
    // bottom again re-arms it (when the view follows the tail at all).
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
    // The view claims the whole height its parent grants (reports a zero
    // minimum and grows/shrinks freely) and scrolls whatever overflows. This
    // is the chat log: it is the only flexible element in its column, so it
    // ends up filling the leftover space above the input/footer.
    kFill,

    // The view reports its content's natural height and never grows, but
    // shrinks first when the column is too short for the siblings below it.
    // This is the sidebar task tree: it occupies exactly its content height
    // while everything fits, and once the tree overflows it shrinks to the
    // space the todo section leaves, scrolling the overflow inside that
    // smaller viewport instead of clipping it (or squeezing the todo out).
    kFitContent,
};

struct ScrollOptions
{
    ScrollMode mode = ScrollMode::kFill;
    // Draw the right-edge thumb while the content overflows. Costs one column
    // of width permanently (the indicator reserves it), so narrow panes that
    // align two stacked sections turn it off to keep their row widths equal.
    bool show_indicator = true;
    // Pin new content to the tail while at the bottom (chat semantics). The
    // task tree turns it off: refreshes must never yank the viewport.
    bool follow_tail = true;
    // Whether the view can take keyboard focus (arrow scrolling). The chat
    // log wants it (Tab cycles into it); the mouse-driven sidebar must stay
    // out of the Tab/arrow cycle so keys never get stolen from the editor.
    bool focusable = true;
};

// Gives its content as much height as the content asks for and shows a
// viewport-sized window onto it, scrolled by the amount this view owns.
//
// This is deliberately not ftxui::yframe: yframe derives the offset by centring
// whichever element reported focus, which cannot express "scrolled to line N".
// Here the offset is state the view owns, and layout only clamps it.
class ScrollView : public ::ftxui::ComponentBase
{
  public:
    explicit ScrollView(::ftxui::Component content, ScrollOptions options = {});
    ScrollView(ScrollView const &) = delete;
    ScrollView &operator=(ScrollView const &) = delete;

    bool OnEvent(::ftxui::Event) override;
    ::ftxui::Element OnRender() override;

    // Whether the view is reachable by keyboard. kFill chat scrolls with the
    // arrows once focused (Focusable by default); a sidebar section that is
    // interacted with by mouse only opts out so it never hijacks Tab/arrows.
    bool Focusable() const override
    {
        return options_.focusable;
    }

    // Move the viewport by |delta| lines. Returns false when the view was
    // already against that end, so an enclosing container can use the key to
    // move focus instead.
    bool Scroll(int delta);
    bool ScrollToTop();
    bool ScrollToBottom();

    // True while the viewport is following the tail, so new content reveals
    // itself instead of leaving the reader behind.
    bool StickingToBottom() const
    {
        return state_->stick_to_bottom;
    }

  private:
    ::ftxui::Component content_;
    ScrollOptions options_;
    std::shared_ptr<ScrollState> state_ = std::make_shared<ScrollState>();
    ::ftxui::Box box_;
};

} // namespace edc
