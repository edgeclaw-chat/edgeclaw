#pragma once

// UI infrastructure: the component base class (Template Method) + capability
// interfaces.
//
// Self-drawing components repeat the same boilerplate: a Box member that catches
// the previous frame's reflect geometry, measure, truncate/wrap, draw. Widget
// folds that into the base class's OnRender(): first reflect records this frame's
// geometry, then Draw() is the subclass's single render entry. Subclasses hold no
// Box field and cannot forget to reflect.
//
// ScrollView / MessageList / Message are performance-critical and build custom
// Nodes directly; they keep deriving from ::ftxui::ComponentBase -- the migration
// removes duplication, it is not about uniformity.

#include "ftxui/component/component_base.hpp"
#include "ftxui/component/event.hpp"
#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/box.hpp"

namespace edc::ui
{

// One frame's geometry: the box this component was assigned last frame.
//
// Same semantics as reflect -- Draw() sees the box measured *last* frame (this
// frame's reflect is written back by layout after OnRender returns). valid() is
// false before the first render.
struct Frame
{
    ::ftxui::Box box;
    bool measured = false;

    bool valid() const
    {
        return measured;
    }
    int width() const
    {
        return measured ? box.x_max - box.x_min + 1 : 0;
    }
    int height() const
    {
        return measured ? box.y_max - box.y_min + 1 : 0;
    }
};

// Root of every self-drawing component. The render skeleton (Template Method) is
// fixed in the base: 1) on_render_begin() clears last frame's leftovers; 2)
// Draw(frame), the subclass's only render entry; 3) on_render_end(). reflect is
// attached last, writing this frame's geometry back to frame_.
class Widget : public ::ftxui::ComponentBase
{
  public:
    // Fixed skeleton: subclasses do not override OnRender (final), only Draw.
    ::ftxui::Element OnRender() final;

    // Consumes no event by default; interactive subclasses override.
    bool OnEvent(::ftxui::Event event) override;

  protected:
    // Subclass: return this frame's element tree. |frame| is last frame's
    // geometry (same as reflect).
    virtual ::ftxui::Element Draw(const Frame &frame) = 0;

    // Hooks before/after drawing (empty by default).
    virtual void on_render_begin()
    {
    }
    virtual void on_render_end()
    {
    }

    // The geometry assigned last frame (valid() == false when not measured).
    const Frame &last_frame() const
    {
        return frame_;
    }

  private:
    Frame frame_;
};

// Capability interfaces (interface segregation): small capabilities rather than
// one fat base; components implement the ones they provide.

// Scrollable: the viewport can move by lines.
class IScrollable
{
  public:
    virtual ~IScrollable() = default;
    virtual bool Scroll(int delta) = 0;
    virtual bool ScrollToTop() = 0;
    virtual bool ScrollToBottom() = 0;
};

} // namespace edc::ui
