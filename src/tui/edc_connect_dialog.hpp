#pragma once

// ── 视图: /connect 弹窗 (ConnectDialog) ─────────────────────────────────────
//
// 从前这个弹窗的状态、渲染、事件、提交全糊在 TUIWindow 里 (6 个成员 + 3 个方法 +
// OnEvent 的一段分支)。它是一个自洽的组件: 三个字段 (API URL / Model / API key)
// 加 Connect/Cancel, 自己持有表单状态、自己渲染、自己处理事件。
//
// 窗口只剩两件事: 打开它 (Open 用当前配置预填) 和把它作为 overlay 画出来。配置的
// 持久化/热应用交给构造时传入的 Submit 回调 —— 弹窗因此不认识 IAppFacade。

#include "edc_app_facade.hpp" /* port::ConnectConfig */

#include "ftxui/component/component_base.hpp"

#include <functional>
#include <string>

namespace edc::ui
{

class ConnectDialog : public ::ftxui::ComponentBase
{
  public:
    // 提交回调: 把填好的配置交给宿主 (持久化 + 热应用)。返回 true = 成功, 弹窗自关。
    using Submit = std::function<bool(const port::ConnectConfig &)>;
    explicit ConnectDialog(Submit submit);

    // 用 |initial| 预填三个字段并打开。
    void Open(const port::ConnectConfig &initial);
    void Close();
    bool IsOpen() const
    {
        return open_;
    }

    // 只在打开时由窗口转发事件: Esc 关, Enter 提交, 其余交给字段。
    bool OnEvent(::ftxui::Event event) override;
    ::ftxui::Element OnRender() override;

  private:
    // 收集字段并调用 Submit; 成功则自关。
    bool SubmitNow();
    ::ftxui::Element Draw();

    bool open_ = false;
    std::string url_;
    std::string model_;
    std::string key_;

    ::ftxui::Component url_input_;
    ::ftxui::Component model_input_;
    ::ftxui::Component key_input_;
    ::ftxui::Component buttons_;
    ::ftxui::Component fields_;
    int focus_ = 0;
    int button_focus_ = 0;

    Submit submit_;
};

} // namespace edc::ui
