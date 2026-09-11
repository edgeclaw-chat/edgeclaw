#pragma once

#include "edc_message.hpp"
#include "ftxui/component/component_base.hpp"
#include "ftxui/screen/box.hpp"
#include <cstddef>
#include <memory>
#include <vector>

namespace edc
{

class MessageListNode;

// A vertical list of chat messages that lays out and draws only the items
// intersecting the viewport.
//
// Why: ftxui rebuilds and re-lays-out the whole element tree on every frame,
// and the chat log is redrawn on every frame of the working animation (~66fps).
// With one child component per message the per-frame cost grew linearly with
// the log (measured ~0.5ms per item in a fastbuild), so a few hundred messages
// froze the UI. Here each item's height is measured once per width and cached;
// a frame then only touches the handful of items inside the viewport.
//
// The visible window is read from the screen stencil the enclosing ScrollView
// narrows down to the viewport, so the list needs no knowledge of the scroll
// offset and therefore cannot disagree with it.
class MessageList : public ::ftxui::ComponentBase
{
  public:
    MessageList() = default;
    MessageList(MessageList const &) = delete;
    MessageList &operator=(MessageList const &) = delete;

    void AddMessage(std::shared_ptr<Message> message);

    // Remove every message. Used when the chat log is re-pointed at a different
    // session's transcript. Cached heights are dropped with the items.
    void Clear();
    bool Empty() const
    {
        return items_.empty();
    }

    bool OnEvent(ftxui::Event event) override;
    ftxui::Element OnRender() override;

  private:
    friend class MessageListNode;

    // Marker in heights_ for "not measured yet".
    static constexpr int kUnmeasured = -1;
    // Generous bound for one item's natural height, handed to the layout while
    // measuring. The height text wants does not depend on the box height, only
    // on the width, so a huge box is harmless.
    static constexpr int kMeasureHeight = 1 << 20;

    // Where an item was drawn in the last frame, for mouse hit-testing. Only
    // visible items are recorded, which is exactly what can be clicked.
    struct Hit
    {
        size_t index;
        ::ftxui::Box box;
    };

    std::vector<std::shared_ptr<Message>> items_;
    std::vector<int> heights_; // rows of the item's own body (excludes the blank spacer)
    std::vector<int> prefix_;  // prefix_[i] = rows above item i; size = n + 1
    int total_ = 0;            // total content rows
    int measured_width_ = 0;   // width heights_ were measured at
    std::vector<Hit> hits_;    // geometry of the last drawn frame

    // Lay one item out at |width| and return the height of its own body. The
    // blank row separating consecutive messages is added as a stride in
    // Rebuild, not folded into this height.
    int MeasureItem(size_t index, int width);
    // Measure every item still unknown at |width|, re-measuring all of them
    // when the width changed. Returns true when anything was measured.
    bool EnsureMeasured(int width);
    void InvalidateHeight(size_t index);
    void Rebuild(); // refresh prefix_ / total_ (body height + spacer stride)
    size_t FirstVisible(int row) const;
};

} // namespace edc
