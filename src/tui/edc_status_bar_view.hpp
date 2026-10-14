#pragma once

// View: the status row / footer (StatusBarView).
//
// It used to be a RenderFooter() method in TUIWindow: read the session workdir,
// take the terminal width, get the sandbox state, then decide which right-end
// fields to drop for the available width. Now a component:
//   * dependencies injected (session model / terminal width / sandbox state /
//     sidebar width) -- it no longer reaches into the window;
//   * the "drop fields by width" policy is a pure function RightText(),
//     unit-testable without a terminal (a narrow pane drops the model first, then
//     the sandbox state, and keeps "/help");
//   * Draw() just joins the two parts into one row.

#include "edc_ports.hpp"         /* port::ITerminalInfo */
#include "edc_session_model.hpp" /* model::SessionModel */
#include "edc_widget.hpp"        /* ui::Widget */

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
    // "model · sandbox:on|off · /help", dropping fields from the middle outward as
    // the pane narrows (first the model, then the sandbox state), leaving "/help"
    // alone. Pure, so the drop policy is unit-testable.
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
