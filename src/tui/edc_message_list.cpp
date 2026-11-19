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

// Off-screen estimates re-measured per frame once the width has settled. Bounded
// so the catch-up can never grow back into the whole-log stall a resize used to
// be; a drag frame does no catch-up at all (the width is still changing).
constexpr int kCatchUpItems = 64;

// Rows kept cached during the first-frame measurement sweep. The transcript
// loader primes the tail window before the first layout, and a screen holds a
// few dozen rows at most; keeping a generous tail means the visible rows (and
// the ones a small scroll would reveal) already have their tree when drawing
// starts. Everything older is rebuilt on demand when it scrolls into view.
constexpr size_t kFirstFrameKeep = 200;
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

        // Text's selection highlight lives in a member that only
        // ComputeRequirement resets, and a steady frame deliberately never
        // re-requires the items (that is what the cached heights buy). So clear
        // the highlight the last Select() phase painted here, which is the one
        // phase that runs before this frame's Select(): the highlight then
        // disappears the moment the selection does.
        for (size_t index : list_.highlighted_)
        {
            if (index < list_.items_.size())
                list_.items_[index]->Render()->ComputeRequirement();
        }
        list_.highlighted_.clear();
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
        const int width = box_.x_max - box_.x_min + 1;

        const size_t first = list_.FirstVisible(first_row);
        size_t last = first;
        bool any = false;

        for (size_t i = first; i < list_.items_.size(); i++)
        {
            if (list_.prefix_[i] > last_row)
                break;

            any = true;
            last = i;

            // A row whose cached tree was wrapped for another width is
            // re-measured here, so a jump that outruns the previous frame's
            // window (a PageUp) still draws text that fits. The row map is
            // rebuilt below and one more frame is owed to draw the settled rows.
            const int height = list_.EnsureItemMeasured(i, width);

            ::ftxui::Box item = box_;
            item.y_min = box_.y_min + list_.prefix_[i];
            item.y_max = item.y_min + height - 1;

            ::ftxui::Element element = list_.items_[i]->Render();
            element->SetBox(item);
            element->Render(screen);

            const ::ftxui::Box visible = ::ftxui::Box::Intersection(item, stencil);
            if (!visible.IsEmpty())
                list_.hits_.push_back({i, visible});
        }

        // Remember the window just drawn: a width change refreshes exactly this
        // window instead of the whole log (see EnsureMeasured).
        if (any)
        {
            list_.visible_first_ = first;
            /* Ask for an older page exactly once per "arrival at the top". */
            if (first == 0 && !list_.reached_top_fired_ && list_.reached_top_cb_)
            {
                list_.reached_top_fired_ = true;
                list_.reached_top_cb_();
            }
            else if (first > 0)
            {
                list_.reached_top_fired_ = false;
            }
            list_.visible_last_ = last;
            list_.visible_known_ = true;
            // ...and it is the anchor the scroll offset is tied to: item |first| sits
            // at the top of the viewport, and a later re-measure above it has to move
            // the offset by the same amount (see TakeOffsetAdjustment).
            list_.anchor_index_ = first;
            list_.anchor_prefix_ = list_.prefix_[first];
            list_.anchor_known_ = true;
        }
    }

    void Select(::ftxui::Selection &selection) override
    {
        // FTXUI collects the selection by walking the tree and asking every node which
        // cells it covers (Text::Select adds the glyphs). This node draws its items
        // itself instead of holding them as children, so it must forward the call or no
        // message text is selectable. It has to happen here, during Select: the
        // Selection handed down is a stack temporary, valid only for this call. Only the
        // touched items are built, and those painted are remembered so the next frame
        // clears the highlight.
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
            list_.highlighted_.push_back(i);
        }
    }

  private:
    MessageList &list_;
    bool remeasured_ = false;
};

void MessageList::SetReachedTopCallback(std::function<void()> cb)
{
    reached_top_cb_ = std::move(cb);
    reached_top_fired_ = false;
}

void MessageList::AddMessage(std::shared_ptr<Message> message)
{
    if (!message)
        return;
    items_.push_back(std::move(message));
    heights_.push_back(kUnmeasured);
    stale_.push_back(false);
    /* Incremental geometry: an append only extends the row map by one entry.
     * The new item starts at the current total and, being unmeasured, adds no
     * height yet (Rebuild skips kUnmeasured heights) -- byte-for-byte what a
     * full Rebuild would have computed. Rebuilding the whole prefix_ on every
     * append made loading an N-message log O(N^2): a 15k-line transcript spent
     * ~0.8s here alone, and it grows quadratically. */
    if (prefix_.empty())
        prefix_.push_back(0); /* prefix_[0] = row 0 */
    prefix_.push_back(total_); /* start row of the new item == the old total */
}

void MessageList::PrependMessages(std::vector<std::shared_ptr<Message>> messages)
{
    messages.erase(std::remove(messages.begin(), messages.end(), nullptr), messages.end());
    if (messages.empty())
        return;

    const size_t count = messages.size();
    items_.insert(items_.begin(), std::make_move_iterator(messages.begin()), std::make_move_iterator(messages.end()));
    heights_.insert(heights_.begin(), count, kUnmeasured);
    stale_.insert(stale_.begin(), count, false);

    /* Every existing item moved down by |count| indices, so the remembered window
     * and the scroll anchor have to move with them -- otherwise the offset
     * correction (TakeOffsetAdjustment) would point at the wrong row. */
    if (visible_known_)
    {
        visible_first_ += count;
        visible_last_ += count;
    }
    if (anchor_known_)
        anchor_index_ += count;

    /* The mouse hit boxes and the last selection highlight are index-based too.
     * The anchor keeps the visible rows exactly where they were, so each of those
     * items now sits |count| indices higher; shift the indices with them, or a
     * click/selection arriving before the next frame targets the wrong bubble. */
    for (Hit &hit : hits_)
        hit.index += count;
    for (size_t &index : highlighted_)
        index += count;

    /* The prepended items are unmeasured, so they add no height yet and the
     * existing rows keep their positions; the next frame measures the new window
     * and the anchor moves the offset by exactly the height that lands above. */
    prepended_count_ += count;
    Rebuild();
}

bool MessageList::TopIsBackfilled() const
{
    // No window drawn yet (the log was just opened): nothing is being read, so the
    // hint stays down. |visible_first_| and |prepended_count_| move together when a
    // batch lands above (PrependMessages shifts the window, then counts the batch),
    // so a reader parked in the tail never drifts over the boundary.
    return visible_known_ && prepended_count_ > 0 && visible_first_ < prepended_count_;
}

void MessageList::Clear()
{
    items_.clear();
    heights_.clear();
    stale_.clear();
    stale_count_ = 0;
    prefix_.clear();
    total_ = 0;
    hits_.clear();
    visible_first_ = 0;
    visible_last_ = 0;
    visible_known_ = false;
    prepended_count_ = 0;
    geometry_settle_ = false;
    anchor_index_ = 0;
    anchor_prefix_ = 0;
    anchor_known_ = false;
    catch_up_budget_ = 0;
    width_moved_this_frame_ = false;
    reached_top_fired_ = false;
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

void MessageList::MeasureAt(size_t index, int width)
{
    if (stale_[index])
    {
        stale_[index] = false;
        stale_count_--;
    }
    heights_[index] = MeasureItem(index, width);
}

void MessageList::OnFrameBegin()
{
    // Per-frame budgets, refilled where the frame boundary is known: the scroll
    // node is rebuilt every frame and tells us (SetBox alone runs several times
    // inside one frame).
    catch_up_budget_ = kCatchUpItems;
    width_moved_this_frame_ = false;

    /* A LazyBubble caches its real bubble's ftxui tree on ITSELF (see
     * LazyBubble::OnRender), and the proxy is never evicted from
     * ChatModel::all_. Left alone, every row ever measured keeps a tree and
     * the log is O(history) in RAM even though MessageWindow bounds the real
     * Message instances to kMaxLive. DHAT measured ~296 MiB peak after the
     * first windowing merge on an 8.2 MB / 7943-line transcript, barely
     * below the un-windowed 323 MiB.
     *
     * This is the true frame boundary (the ScrollNode is rebuilt every frame),
     * BEFORE any layout/measure/draw pass: drop trees outside last frame's
     * window, with a generous margin so a row about to scroll in is kept.
     * Heights are deliberately NOT touched -- the row map must stay exact.
     * A visible row whose tree was dropped is rebuilt by EnsureItemMeasured
     * before it is drawn. */
    if (visible_known_ && !items_.empty())
    {
        /* Keep kMaxLive/2 rows on each side of the window: the retained set is
         * at most kMaxLive + the handful of rows actually on screen. */
        constexpr size_t kMargin = 100;
        const size_t keep_first = visible_first_ > kMargin ? visible_first_ - kMargin : 0;
        const size_t keep_last = std::min(items_.size(), visible_last_ + kMargin + 1);
        for (size_t i = 0; i < items_.size(); i++)
        {
            if (i >= keep_first && i < keep_last)
                continue;
            items_[i]->ClearCachedTree();
        }
    }
}

int MessageList::TakeOffsetAdjustment()
{
    if (!anchor_known_ || anchor_index_ >= prefix_.size())
        return 0;

    // How far the anchored row moved since it was drawn: prefix_[i] is the row
    // item i starts at, so a re-measure above it changed it by exactly this much,
    // and the offset has to follow for item i to stay on the same screen row.
    const int adjust = prefix_[anchor_index_] - anchor_prefix_;
    anchor_prefix_ = prefix_[anchor_index_]; // consuming: counted as applied
    return adjust;
}

int MessageList::EnsureItemMeasured(size_t index, int width)
{
    /* A row whose cached tree was pruned at the frame boundary still has a
     * valid height, but no Node to draw with. Rebuild it here (this also runs
     * ComputeRequirement) so the draw pass sees a laid-out tree. No Rebuild():
     * the height is unchanged, so the row map and scroll offset stay put. */
    if (!items_[index]->HasCachedTree())
    {
        MeasureItem(index, width);
        return heights_[index];
    }
    if (heights_[index] == kUnmeasured || stale_[index])
    {
        MeasureAt(index, width);
        // Fold it into the row map immediately: the rows below |index| have not
        // been drawn yet this frame, and they must be drawn at their corrected
        // positions. Deferring the rebuild (the natural "one more frame" pattern)
        // would show exactly the shift the scroll anchor exists to prevent.
        Rebuild();
        geometry_settle_ = true;
    }
    return heights_[index];
}

bool MessageList::EnsureMeasured(int width)
{
    if (width <= 0 || items_.empty())
        return false;

    bool measured = false;
    const bool width_changed = (width != measured_width_);

    if (width_changed)
    {
        // Wrapping depends on the width, so every cached height is now wrong --
        // but they are kept as estimates instead of being dropped, and only the
        // window that was on screen is refreshed below. The sidebar drag changes
        // this width on every frame; re-measuring the whole log each time is
        // exactly what made the split stutter.
        measured_width_ = width;
        measured = true;
        width_moved_this_frame_ = true;
        for (size_t i = 0; i < heights_.size(); i++)
        {
            if (heights_[i] != kUnmeasured && !stale_[i])
            {
                stale_[i] = true;
                stale_count_++;
            }
        }
    }

    // The window drawn last frame: keep its geometry exact for the width we are
    // laying out now, so what is on screen never depends on an estimate.
    if (visible_known_)
    {
        for (size_t i = visible_first_; i <= visible_last_ && i < items_.size(); i++)
        {
            if (heights_[i] == kUnmeasured || stale_[i])
            {
                MeasureAt(i, width);
                measured = true;
            }
        }
    }

    // An item with no height at all (first frame, a cleared log, an entry that
    // just expanded) has no estimate to fall back on: measure it now.
    //
    // A windowed transcript arrives as thousands of LazyBubble proxies. Each
    // MeasureAt() makes the proxy cache a real ftxui tree on ITSELF; left in
    // place until the next frame boundary that is one tree per history row
    // alive at the same instant, which is exactly the O(history) peak DHAT
    // measured (~296 MiB on an 8.2 MB / 7943-line transcript). Release the
    // tree immediately after measuring rows that are off screen, so the peak
    // is bounded from the very first frame. Heights stay exact: the row map
    // is independent of the cached Node, and EnsureItemMeasured() rebuilds a
    // tree before any row that actually gets drawn.
    const size_t keep_from = items_.size() > kFirstFrameKeep ? items_.size() - kFirstFrameKeep : 0;
    for (size_t i = 0; i < items_.size(); i++)
    {
        if (heights_[i] == kUnmeasured)
        {
            MeasureAt(i, width);
            measured = true;
            if (i < keep_from)
                items_[i]->ClearCachedTree();
        }
    }

    // Width settled: finish the estimates the last width change left behind, a
    // bounded batch per frame. The budget is refilled once per frame by
    // OnFrameBegin, not here: the layout loop calls this several times per frame,
    // and a per-call budget would drain the whole log within one of them (which is
    // how the drag stutter came back). A frame whose width moved spends nothing --
    // the next drag step would only re-stale the rows it repaired.
    if (!width_moved_this_frame_ && catch_up_budget_ > 0 && stale_count_ > 0)
    {
        for (size_t i = 0; i < items_.size() && catch_up_budget_ > 0; i++)
        {
            if (!stale_[i])
                continue;
            MeasureAt(i, width);
            catch_up_budget_--;
            measured = true;
        }
    }

    // A height measured while drawing last frame is not in the row map yet: fold
    // it in here, and the frame it owes is what HasPendingWork() keeps coming.
    if (geometry_settle_)
    {
        geometry_settle_ = false;
        measured = true;
    }

    if (measured)
        Rebuild();
    return measured;
}

void MessageList::InvalidateHeight(size_t index)
{
    if (index >= heights_.size() || heights_[index] == kUnmeasured)
        return;
    if (stale_[index])
    {
        stale_[index] = false;
        stale_count_--;
    }
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
