#pragma once

// ── UI 基础设施: 组件基类 (模板方法) + 能力接口 ────────────────────────────
//
// 现状里每个自绘组件都在重复同一套样板: 用一个 Box 成员接住上一帧 reflect
// 出来的几何, 量宽, 截断/换行, 再画。TaskWidget / TodoWidget / MessageList /
// EdcInput 里各有一份 (content_box_ / RowWidth() / reflect)。
//
// Widget 把这套骨架收进基类的 OnRender(): 先 reflect 记录本帧几何, 再交给子类
// 唯一的渲染入口 Draw()。子类因此不再持有 Box 字段, 也不会忘记 reflect。
//
// 注意: ScrollView / MessageList / Message 这些性能关键、且直接构建自定义 Node
// 的组件**继续**继承 ::ftxui::ComponentBase, 不套这一层 —— 迁移是为了消除重复,
// 不是为了统一。

#include "ftxui/component/component_base.hpp"
#include "ftxui/component/event.hpp"
#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/box.hpp"

namespace edc::ui
{

// 一帧的几何信息: 本组件上一帧被分配到的 box。
//
// 与现有 reflect 语义一致 —— Draw() 看到的是**上一帧**量到的盒子 (本帧的
// reflect 在 OnRender 返回之后才由布局写回)。首次渲染前 valid() 为 false。
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

// 所有自绘组件的根。渲染骨架 (Template Method) 在基类固定成三步:
//   1. on_render_begin() —— 画之前清理上一帧的残留 (如选择高亮), 默认空;
//   2. Draw(frame)       —— 子类唯一的渲染入口;
//   3. on_render_end()   —— 画之后的钩子, 默认空。
// 最后统一挂上 reflect, 把本帧几何写回 frame_。
class Widget : public ::ftxui::ComponentBase
{
  public:
    // 固定渲染骨架: 子类不覆写 OnRender (最终实现), 只实现 Draw。
    ::ftxui::Element OnRender() final;

    // 默认不消费事件; 需要交互的子类覆写。
    bool OnEvent(::ftxui::Event event) override;

  protected:
    // 子类实现: 返回本帧的元素树。|frame| 是上一帧的几何 (同 reflect 语义)。
    virtual ::ftxui::Element Draw(const Frame &frame) = 0;

    // 画之前的钩子 (默认空)。
    virtual void on_render_begin()
    {
    }
    // 画之后的钩子 (默认空)。
    virtual void on_render_end()
    {
    }

    // 上一帧被分配到的几何 (未量过时 valid() == false)。
    const Frame &last_frame() const
    {
        return frame_;
    }

  private:
    Frame frame_;
};

// ── 能力接口 (接口隔离) ─────────────────────────────────────────────────────
// 小的能力接口, 而不是一个万能胖基类: 组件按需实现自己提供的能力。

// 可滚动: 视口可以按行移动。
class IScrollable
{
  public:
    virtual ~IScrollable() = default;
    virtual bool Scroll(int delta) = 0;
    virtual bool ScrollToTop() = 0;
    virtual bool ScrollToBottom() = 0;
};

} // namespace edc::ui
