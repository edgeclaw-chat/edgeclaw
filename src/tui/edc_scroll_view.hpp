#pragma once

#include "edc_widget.hpp" /* ui::IScrollable */
#include "ftxui/component/component_base.hpp"
#include "ftxui/screen/box.hpp"
#include <algorithm>
#include <memory>

namespace edc
{

// Shared scroll position between the component (owns it, lives across frames)
// and the scroll node it builds each frame (borrows it, dies with the frame):
// FTXUI rebuilds the tree every frame and Node is non-copyable, so a node cannot
// own it. Layout publishes what it measured here, input reads it back, and every
// move goes through MoveTo so the two cannot disagree about where the ends are.
struct ScrollState
{
    // Written when handling input, read during layout.
    int offset = 0;
    bool stick_to_bottom = true;
    // Whether reaching the bottom re-arms auto-follow. Chat keeps it on; the
    // sidebar tree keeps it off so a refresh never yanks the reader down.
    bool follow_tail = true;

    // Written during layout, read when handling input (a frame stale by the time
    // an event arrives); zero until the first frame has been laid out.
    int content_height = 0;
    int viewport_height = 0;

    int MaxScroll() const
    {
        return std::max(0, content_height - viewport_height);
    }

    // Called once per frame from ScrollNode::Render, after layout has settled.
    // Must NOT run inside SetBox: ftxui iterates layout several times per frame
    // and wrapping content reports a different height each pass, so clamping
    // against a transient one would wipe the offset every frame.
    void ClampForLayout()
    {
        offset = (follow_tail && stick_to_bottom) ? MaxScroll() : std::max(0, std::min(offset, MaxScroll()));
    }

    // Each returns whether the view actually moved, so a scroll already against
    // that end releases the key to an enclosing container.
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
    // Scrolling away from the bottom releases auto-follow; reaching it re-arms it.
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
    // Claims the height the parent grants and scrolls the overflow (the chat log).
    kFill,

    // Reports the content's natural height, never grows, but shrinks before its
    // siblings (the sidebar task tree).
    kFitContent,
};

struct ScrollOptions
{
    ScrollMode mode = ScrollMode::kFill;
    // Draw the right-edge thumb while content overflows; costs one column.
    bool show_indicator = true;
    // Pin new content to the tail while at the bottom (the task tree turns it off).
    bool follow_tail = true;
    // Whether the view can take keyboard focus (arrow/page scrolling).
    bool focusable = true;
};

// Gives its content as much height as it asks for and shows a viewport-sized
// window onto it. Deliberately not ftxui::yframe: yframe derives the offset by
// centring whichever element reported focus, which cannot express "scrolled to
// line N".
class ScrollView : public ::ftxui::ComponentBase, public ui::IScrollable
{
  public:
    // |pinned|, when given, is drawn above the viewport and never scrolls (the
    // sidebar's section header); the wheel still moves the content from anywhere
    // in the view, the pinned part included.
    explicit ScrollView(::ftxui::Component content, ScrollOptions options = {}, ::ftxui::Component pinned = nullptr);
    ScrollView(ScrollView const &) = delete;
    ScrollView &operator=(ScrollView const &) = delete;

    bool OnEvent(::ftxui::Event) override;
    ::ftxui::Element OnRender() override;

    bool Focusable() const override
    {
        return options_.focusable;
    }

    // ui::IScrollable: move by |delta| lines. Returns false when already against
    // that end, so an enclosing container can use the key to move focus instead.
    bool Scroll(int delta) override;
    bool ScrollToTop() override;
    bool ScrollToBottom() override;

    // True while the viewport follows the tail.
    bool StickingToBottom() const
    {
        return state_->stick_to_bottom;
    }

    // Viewport height the last frame laid out (0 before the first frame).
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
    // The content's scroll-anchoring hook, when it provides one (borrowed: the
    // content outlives this view in every use).
    ui::IScrollAnchor *anchor_ = nullptr;
    ::ftxui::Box box_;
};

} // namespace edc
