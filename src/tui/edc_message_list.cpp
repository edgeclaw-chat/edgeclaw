#include "edc_message_list.hpp"

#include "ftxui/component/event.hpp"
#include "ftxui/dom/elements.hpp"
#include "ftxui/dom/node.hpp"
#include "ftxui/dom/requirement.hpp"
#include "ftxui/dom/selection.hpp"
#include "ftxui/screen/screen.hpp"

#include <algorithm>

namespace edc
{

namespace
{
// ftxui's own layout loop gives up after 20 passes; measuring an item walks the
// same convergence (wrapping text reports a different height on each pass until
// the flexboxes settle).
constexpr int kLayoutIterationCap = 20;
} // namespace

// Draws the items of a MessageList that overlap the viewport the enclosing
// ScrollView narrowed the screen stencil down to.
//
// The node owns no children: ftxui would lay out and draw every one of them
// again on every pass. Instead it borrows the list's cached heights to map rows
// to items, and only touches the items the stencil actually covers.
class MessageListNode : public ::ftxui::Node
{
  public:
    explicit MessageListNode(MessageList &list) : list_(list)
    {
    }

    void ComputeRequirement() override
    {
        // The only thing scroll geometry needs from us: the full content
        // height, which is known from the cached per-item heights even for the
        // items that will never be laid out this frame.
        requirement_.min_x = 0;
        requirement_.min_y = list_.total_;
        requirement_.flex_grow_y = 1;
        requirement_.flex_shrink_y = 1;
        remeasured_ = false;
    }

    void SetBox(::ftxui::Box box) override
    {
        Node::SetBox(box);

        const int width = box.x_max - box.x_min + 1;
        if (width <= 0)
            return;

        remeasured_ = list_.EnsureMeasured(width);

        // The enclosing ScrollNode reads our height back (through the scroll
        // indicator) to size the content and clamp the scroll offset. Its
        // ComputeRequirement for this pass copied our requirement *before*
        // EnsureMeasured ran, so publish the settled height here too.
        requirement_.min_y = list_.total_;
    }

    void Check(Status *status) override
    {
        // A measurement in SetBox invalidated the height reported earlier in
        // this same pass; ask for one more iteration so the parent reads back
        // the settled value.
        status->need_iteration |= remeasured_;
    }

    void Render(::ftxui::Screen &screen) override
    {
        list_.hits_.clear();

        const ::ftxui::Box &stencil = screen.stencil;
        if (list_.items_.empty() || stencil.IsEmpty())
            return;

        // The ScrollNode hands us a box whose top row is content row 0, and the
        // stencil is the part of that box still on screen, so the stencil maps
        // straight onto content coordinates.
        const int first_row = stencil.y_min - box_.y_min;
        const int last_row = stencil.y_max - box_.y_min;

        for (size_t i = list_.FirstVisible(first_row); i < list_.items_.size(); i++)
        {
            if (list_.prefix_[i] > last_row)
                break;

            ::ftxui::Box item = box_;
            item.y_min = box_.y_min + list_.prefix_[i];
            item.y_max = item.y_min + list_.heights_[i] - 1;

            ::ftxui::Element element = list_.items_[i]->Render();
            element->SetBox(item);
            element->Render(screen);

            const ::ftxui::Box visible = ::ftxui::Box::Intersection(item, stencil);
            if (!visible.IsEmpty())
                list_.hits_.push_back({i, visible});
        }
    }

    void Select(::ftxui::Selection &selection) override
    {
        // FTXUI collects the selected text by walking the node tree and asking
        // every node which of its cells the selection covers (Text::Select adds
        // the glyphs). This node draws the items itself instead of holding them
        // as children, so it must forward the call: without this no message text
        // is selectable at all.
        //
        // Only the items the selection touches are built. A drag spans a few
        // rows, so this stays cheap, and it is the only way to get the items'
        // boxes right -- Select runs before Render, so the frame's elements do
        // not exist yet and are rebuilt here the same way Render builds them.
        if (list_.items_.empty())
            return;

        const ::ftxui::Box &sel = selection.GetBox();
        if (::ftxui::Box::Intersection(sel, box_).IsEmpty())
            return;

        const int first_row = sel.y_min - box_.y_min;
        const int last_row = sel.y_max - box_.y_min;

        for (size_t i = list_.FirstVisible(first_row); i < list_.items_.size(); i++)
        {
            if (list_.prefix_[i] > last_row)
                break;

            ::ftxui::Box item = box_;
            item.y_min = box_.y_min + list_.prefix_[i];
            item.y_max = item.y_min + list_.heights_[i] - 1;

            ::ftxui::Element element = list_.items_[i]->Render();
            element->SetBox(item);
            element->Select(selection);
        }
    }

  private:
    MessageList &list_;
    bool remeasured_ = false;
};

void MessageList::AddMessage(std::shared_ptr<Message> message)
{
    if (!message)
        return;
    items_.push_back(std::move(message));
    heights_.push_back(kUnmeasured);
    Rebuild();
}

void MessageList::Clear()
{
    items_.clear();
    heights_.clear();
    prefix_.clear();
    total_ = 0;
    hits_.clear();
}

int MessageList::MeasureItem(size_t index, int width)
{
    // A message whose text cannot be wrapped by ftxui alone (an unbroken CJK
    // run, a long URL) needs the width to wrap itself, and it must know it
    // before it builds its element tree.
    items_[index]->SetRenderWidth(width);

    ::ftxui::Element element = items_[index]->Render();

    ::ftxui::Box box;
    box.x_min = 0;
    box.x_max = std::max(0, width - 1);
    box.y_min = 0;
    box.y_max = kMeasureHeight;

    ::ftxui::Node::Status status;
    element->Check(&status);
    while (status.need_iteration && status.iteration < kLayoutIterationCap)
    {
        element->ComputeRequirement();
        element->SetBox(box);
        status.need_iteration = false;
        status.iteration++;
        element->Check(&status);
    }

    // The item's own height only. The blank separator between messages is a
    // stride the list adds around the item (see Rebuild), never part of the
    // item's box: the message tree ends in `borderStyled(EMPTY) | bgcolor`, so
    // handing it a taller box would stretch the border and paint the spacer
    // with the message background, making every message look one row taller.
    return std::max(1, element->requirement().min_y);
}

bool MessageList::EnsureMeasured(int width)
{
    if (width <= 0 || items_.empty())
        return false;

    bool measured = false;
    if (width != measured_width_)
    {
        // Wrapping depends on the width, so every cached height is stale.
        std::fill(heights_.begin(), heights_.end(), kUnmeasured);
        measured_width_ = width;
        measured = true;
    }

    for (size_t i = 0; i < items_.size(); i++)
    {
        if (heights_[i] == kUnmeasured)
        {
            heights_[i] = MeasureItem(i, width);
            measured = true;
        }
    }

    if (measured)
        Rebuild();
    return measured;
}

void MessageList::InvalidateHeight(size_t index)
{
    if (index >= heights_.size() || heights_[index] == kUnmeasured)
        return;
    heights_[index] = kUnmeasured;
    Rebuild();
}

void MessageList::Rebuild()
{
    prefix_.assign(items_.size() + 1, 0);

    int total = 0;
    for (size_t i = 0; i < items_.size(); i++)
    {
        prefix_[i] = total;
        if (heights_[i] > 0)
            total += heights_[i] + Message::kSpacingRows;
    }
    prefix_[items_.size()] = total;
    total_ = total;
}

size_t MessageList::FirstVisible(int row) const
{
    // First item whose rows reach past |row|: items are laid out back to back,
    // so prefix_ is monotonically increasing and this is a plain binary search.
    size_t lo = 0;
    size_t hi = items_.size();
    while (lo < hi)
    {
        const size_t mid = lo + (hi - lo) / 2;
        // An item reaches from prefix_[mid] through prefix_[mid] + height +
        // spacer - 1; the spacer belongs to it for visibility purposes even
        // though the item's box stops short of it.
        if (prefix_[mid] + std::max(0, heights_[mid]) + Message::kSpacingRows <= row)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

bool MessageList::OnEvent(ftxui::Event event)
{
    // Keyboard events belong to the message editor; only the mouse can target
    // an entry (the list is not focusable).
    if (!event.is_mouse() || hits_.empty())
        return false;

    for (const Hit &hit : hits_)
    {
        if (!hit.box.Contain(event.mouse().x, event.mouse().y))
            continue;

        if (items_[hit.index]->OnEvent(event))
        {
            // Expanding or collapsing an entry changes its height. Drop the
            // cached measurement instead of re-measuring here: the item keeps
            // its correct on-screen box until the next frame lays it out again,
            // so a click arriving in between cannot be hit-tested against the
            // throwaway box a measurement would leave behind.
            InvalidateHeight(hit.index);
            return true;
        }
        return false;
    }
    return false;
}

::ftxui::Element MessageList::OnRender()
{
    return std::make_shared<MessageListNode>(*this);
}

} // namespace edc
