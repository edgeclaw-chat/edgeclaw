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
// intersecting the viewport: without it the per-frame cost grew linearly with the
// log (one child component per message, ~0.5ms per item), so a few hundred
// messages froze the UI. Heights are measured once per width and cached.
//
// The window comes from the stencil the enclosing ScrollView narrows to the
// viewport, so the list never knows the scroll offset and cannot disagree with it.
class MessageList : public ::ftxui::ComponentBase
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
    std::vector<int> heights_; // rows of the item's own body (excludes the blank spacer)
    std::vector<int> prefix_;  // prefix_[i] = rows above item i; size = n + 1
    int total_ = 0;            // total content rows
    int measured_width_ = 0;   // width heights_ were measured at
    std::vector<Hit> hits_;    // geometry of the last drawn frame
    // Items whose cached tree still carries the last Select() highlight. Text
    // clears its highlight only in ComputeRequirement, which a steady frame skips,
    // so the next frame clears these before painting the new selection.
    std::vector<size_t> highlighted_;

    // Lay one item out at |width|, returning its own body height; the blank row
    // between messages is a stride added in Rebuild.
    int MeasureItem(size_t index, int width);
    // Measure every item still unknown at |width|, re-measuring all when the width
    // changed. True when anything was measured.
    bool EnsureMeasured(int width);
    void InvalidateHeight(size_t index);
    void Rebuild(); // refresh prefix_ / total_ (body height + spacer stride)
    size_t FirstVisible(int row) const;
};

} // namespace edc
