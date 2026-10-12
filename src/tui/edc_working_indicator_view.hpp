#pragma once

// ── 动画行: WorkingIndicatorView ─────────────────────────────────────────────
//
// 前台任务运行时取代 footer 的那一行: 一条来回扫动的方块条, 亮段内部的亮度
// 沿运动方向流动。原本是一个自由函数 WorkingIndicatorElement(elapsed_ms) +
// 匿名命名空间里的一堆常量/辅助函数 —— 行为正确, 但没有"对象"承接:
//   * 谁在计时、计时从哪开始, 由 TUIWindow 的 working_started_ 散着管;
//   * 时钟从环境里现读, 测试只能靠传 elapsed_ms 模拟。
//
// 现在它是个组件: 持有注入的时钟, 自己从 Restart() 起计时, Draw() 把当前
// 时长画出来。旧的自由函数保留为薄委托, 既有测试不必改。
//
// 注意: 它取代 footer, 所以高度恒为一行、格数固定 (不随宽度变), 切换时布局不跳。

#include "edc_ports.hpp"
#include "edc_widget.hpp"

#include <ftxui/dom/elements.hpp>

#include <memory>

namespace edc::ui
{

// The animated "working" row. |clock| is the injected time source; nullptr
// falls back to SystemClock::Shared().
class WorkingIndicatorView : public Widget
{
  public:
    explicit WorkingIndicatorView(std::shared_ptr<const port::IClock> clock = nullptr);

    // Start counting from "now" (the window calls this when the foreground
    // process turns busy).
    void Restart();

    // Stop counting (the window calls this when the process is no longer busy).
    void Stop();

    // True while counting from the last Restart().
    bool Running() const
    {
        return running_;
    }

    // Milliseconds since the last Restart(); 0 when not running.
    long long ElapsedMs() const;

    // Pure render of the frame at |elapsed_ms|: stateless, so tests and
    // stateless callers can draw a given moment without an instance.
    static ::ftxui::Element ElementAt(long long elapsed_ms);

  protected:
    ::ftxui::Element Draw(const Frame &frame) override;

  private:
    std::shared_ptr<const port::IClock> clock_;
    port::IClock::TimePoint started_{};
    bool running_ = false;
};

} // namespace edc::ui

namespace edc
{
// 旧入口 (保留): 等价于 WorkingIndicatorView::ElementAt。声明放这里, 由
// edc_tui_window.hpp 传递包含, 既有测试因此无需改动。
::ftxui::Element WorkingIndicatorElement(long long elapsed_ms);
} // namespace edc
