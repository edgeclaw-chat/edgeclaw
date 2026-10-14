#pragma once

#include "edc_message.hpp"
#include "edc_widget.hpp" /* ui::IScrollAnchor */
#include "ftxui/component/component_base.hpp"
#include "ftxui/screen/box.hpp"
#include <cstddef>
#include <memory>
#include <vector>

namespace edc
{

class MessageListNode;

// A vertical list of chat messages that lays out and draws only the items
// intersecting the viewport: without it the per-frame cost grew linearly with the
// log (one child component per message, ~0.5ms per item), so a few hundred
// messages froze the UI. Heights are measured once per width and cached.
//
// A width change (the sidebar drag changes the chat pane's width on every frame)
// does not re-measure the whole log: the item heights are kept as estimates and
// only the window that is on screen is refreshed, so a drag frame costs the same
// as a steady frame no matter how long the log is. The leftovers are re-measured
// when they scroll back into view or by a bounded catch-up once the width has
// settled (see HasPendingWork()).
//
// Keeping estimates is only invisible because of the scroll anchor (see
// ui::IScrollAnchor): repairing a row above the viewport moves every row below it,
// so the offset is moved by the same amount and the rows on screen stay put.
//
// The window comes from the stencil the enclosing ScrollView narrows to the
// viewport, so the list never knows the scroll offset and cannot disagree with it.
class MessageList : public ::ftxui::ComponentBase, public ui::IScrollAnchor
{
  public:
    MessageList() = default;
    MessageList(MessageList const &) = delete;
    MessageList &operator=(MessageList const &) = delete;

    void AddMessage(std::shared_ptr<Message> message);

    // Remove every message (the log was re-pointed at another session), and their
    // cached heights.
    void Clear();
    bool Empty() const
    {
        return items_.empty();
    }

    // True while the list still owes work to finish a resize: some heights are
    // still estimates from an older width, or a height measured while drawing has
    // not been folded back into the row map yet. The window keeps a (throttled)
    // frame coming until this clears -- see TUIWindow::TickAnimations.
    bool HasPendingWork() const
    {
        return stale_count_ > 0 || geometry_settle_;
    }

    // ui::IScrollAnchor: a row re-measured above the viewport moves every row
    // below it, so the offset has to move with it or the rows on screen slide
    // (see the anchor fields).
    void OnFrameBegin() override;
    int TakeOffsetAdjustment() override;

    bool OnEvent(ftxui::Event event) override;
    ftxui::Element OnRender() override;

  private:
    friend class MessageListNode;

    // Marker in heights_ for "not measured yet".
    static constexpr int kUnmeasured = -1;
    // Generous bound handed to the layout while measuring: text height depends on
    // width, not box height, so a huge box is harmless.
    static constexpr int kMeasureHeight = 1 << 20;

    // Where an item was drawn in the last frame, for mouse hit-testing (only
    // visible items, which is exactly what can be clicked).
    struct Hit
    {
        size_t index;
        ::ftxui::Box box;
    };

    std::vector<std::shared_ptr<Message>> items_;
    std::vector<int> heights_; // rows of the item's own body (or an estimate; excludes the blank spacer)
    std::vector<bool> stale_;  // heights_[i] is an estimate measured at an older width
    int stale_count_ = 0;      // number of true entries in stale_
    std::vector<int> prefix_;  // prefix_[i] = rows above item i; size = n + 1
    int total_ = 0;            // total content rows
    int measured_width_ = 0;   // width heights_ were last (re)measured at
    std::vector<Hit> hits_;    // geometry of the last drawn frame

    // The item window drawn on the last frame. A width change refreshes exactly
    // this window instead of the whole log; it is the drag path's bound.
    size_t visible_first_ = 0;
    size_t visible_last_ = 0;
    bool visible_known_ = false;

    // A height was measured while drawing, i.e. after layout: the row map is
    // rebuilt at the end of the frame and one more frame is owed to draw the
    // settled rows. Cleared by the next EnsureMeasured().
    bool geometry_settle_ = false;

    // Scroll anchoring. |anchor_index_| is the item drawn at the top of the
    // viewport and |anchor_prefix_| the prefix it had then. A re-measure above the
    // viewport changes prefix_, and the scroll offset must change by the same
    // amount or the rows on screen slide. Taken in Render (the only place that
    // knows which item is on screen) and handed to the ScrollNode through
    // TakeOffsetAdjustment().
    size_t anchor_index_ = 0;
    int anchor_prefix_ = 0;
    bool anchor_known_ = false;

    // Off-screen estimates re-measured this frame, and whether the width moved in
    // it. Both are per frame -- OnFrameBegin() refills them, because SetBox (which
    // spends the budget) runs several times per frame and a per-SetBox budget
    // would drain the whole log within one.
    int catch_up_budget_ = 0;
    bool width_moved_this_frame_ = false;

    // Items whose cached tree still carries the last Select() highlight. Text
    // clears its highlight only in ComputeRequirement, which a steady frame skips,
    // so the next frame clears these before painting the new selection.
    std::vector<size_t> highlighted_;

    // Lay one item out at |width|, returning its own body height; the blank row
    // between messages is a stride added in Rebuild.
    int MeasureItem(size_t index, int width);
    // Measure |index| at |width| and mark it fresh (the one write path for
    // heights_, so stale_/stale_count_ cannot drift).
    void MeasureAt(size_t index, int width);
    // Height of |index| at |width|, measuring it first if it is missing or an
    // estimate from another width. Used while drawing, so a row never renders a
    // tree wrapped for the wrong width.
    int EnsureItemMeasured(size_t index, int width);
    // Measure every item still unknown at |width|, re-measuring all when the width
    // changed. True when anything was measured.
    bool EnsureMeasured(int width);
    void InvalidateHeight(size_t index);
    void Rebuild(); // refresh prefix_ / total_ (body height + spacer stride)
    size_t FirstVisible(int row) const;
};

} // namespace edc
