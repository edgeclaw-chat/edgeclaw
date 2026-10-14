#include "edc_widget.hpp"

namespace edc::ui
{

::ftxui::Element Widget::OnRender()
{
    on_render_begin();
    ::ftxui::Element content = Draw(frame_);
    on_render_end();
    // Draw saw "last frame's" geometry; mark this frame measured now. This
    // frame's reflect is written back by layout after OnRender returns, so next
    // frame's Draw reads this frame's box.
    frame_.measured = true;
    return std::move(content) | ::ftxui::reflect(frame_.box);
}

bool Widget::OnEvent(::ftxui::Event)
{
    return false;
}

} // namespace edc::ui
