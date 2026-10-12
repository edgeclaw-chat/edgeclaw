#pragma once

// ── 视图: 状态行 / 页脚 (StatusBarView) ─────────────────────────────────────
//
// 从前它是 TUIWindow 里的一个 RenderFooter() 方法: 读会话工作目录、取终端宽度、
// 拿沙箱状态, 再按可用宽度决定右端丢哪些字段。现在独立成一个组件:
//
//   * 依赖注入 (会话模型 / 终端宽度 / 沙箱状态 / 侧栏宽度) —— 不再伸手进窗口;
//   * "按宽度丢字段"的策略提成**纯函数** RightText(), 可以脱离终端单测
//     (窄屏先丢 model, 再丢 sandbox, 永远保留 "/help");
//   * Draw() 只负责把左右两段拼成一行。

#include "edc_ports.hpp"       /* port::ITerminalInfo */
#include "edc_session_model.hpp" /* model::SessionModel */
#include "edc_widget.hpp"      /* ui::Widget */

#include <functional>
#include <memory>
#include <string>

namespace edc::ui
{

class StatusBarView : public Widget
{
  public:
    // |sandbox_enabled| / |sidebar_width| are read per frame (both change at
    // runtime: /sandbox and the draggable split). |session| supplies the bound
    // working directory; |terminal| the pane width.
    StatusBarView(std::shared_ptr<const model::SessionModel> session,
                  std::shared_ptr<const port::ITerminalInfo> terminal, std::function<bool()> sandbox_enabled,
                  std::function<int()> sidebar_width);

    // The model name shown at the right end ("·" separated); "" hides it.
    void SetModel(std::string model);

    // The right-hand text for |pane_width| columns with |left| already drawn:
    // "model · sandbox:on|off · /help", dropping fields from the middle outward
    // as the pane narrows (first the model, then the sandbox state), leaving
    // "/help" alone. Pure, so the drop policy is unit-testable.
    static std::string RightText(int pane_width, const std::string &left, const std::string &model, bool sandbox_on);

  protected:
    ::ftxui::Element Draw(const Frame &frame) override;

  private:
    // The working directory to show: the bound one, else the launch directory,
    // with $HOME shortened to "~".
    std::string CurrentDir() const;

    std::shared_ptr<const model::SessionModel> session_;
    std::shared_ptr<const port::ITerminalInfo> terminal_;
    std::function<bool()> sandbox_enabled_;
    std::function<int()> sidebar_width_;
    std::string model_;
};

} // namespace edc::ui
