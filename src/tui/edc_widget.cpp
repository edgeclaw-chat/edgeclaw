#include "edc_widget.hpp"

namespace edc::ui
{

::ftxui::Element Widget::OnRender()
{
    on_render_begin();
    ::ftxui::Element content = Draw(frame_);
    on_render_end();
    // Draw 已看到"上一帧"的几何, 现在把本帧标记为已测量: 本帧的 reflect 会在
    // OnRender 返回之后由布局写回 box, 于是下一帧的 Draw 读到的就是本帧的盒子。
    frame_.measured = true;
    return std::move(content) | ::ftxui::reflect(frame_.box);
}

bool Widget::OnEvent(::ftxui::Event)
{
    return false;
}

} // namespace edc::ui
