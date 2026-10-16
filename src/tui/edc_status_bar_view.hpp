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
    // |sandbox_enabled| / |shell_syntax| / |sidebar_width| / |context_window| are read
    // per frame (the first three change at runtime: /sandbox, /shell and the draggable
    // split; the window is the configured model's context size). |session| supplies the
    // bound working directory; |terminal| the pane width.
    StatusBarView(std::shared_ptr<const model::SessionModel> session,
                  std::shared_ptr<const port::ITerminalInfo> terminal, std::function<bool()> sandbox_enabled,
                  std::function<bool()> shell_syntax, std::function<int()> sidebar_width,
                  std::function<int()> context_window);

    // The model name shown at the right end ("·" separated); "" hides it.
    void SetModel(std::string model);

    // The foreground group's current context size (from the task snapshot); the window
    // is read from the injected |context_window|. Together they render as
    // "ctx 45k/128k" (ctx 45k when the window is unknown).
    void SetContextUsage(long long tokens, bool estimated);

    // A transient hint shown at the LEFT end, after the working directory (e.g.
    // "Ctrl+C again to quit"). Empty hides it.
    void SetHint(std::string hint);

    // The footer's left text: " <dir>" normally, or " <hint>" while a hint is up (the
    // hint REPLACES the directory). Pure, so the composition is unit-testable.
    static std::string LeftText(const std::string &dir, const std::string &hint);

    // The right-hand text for |pane_width| columns with |left| already drawn:
    // "model · ctx · sandbox:on|off · shell:on|off · /help", dropping fields from the
    // front outward as the pane narrows (the model first, then ctx, then sandbox, then
    // shell), leaving "/help" alone. Pure, so the drop policy is unit-testable.
    static std::string RightText(int pane_width, const std::string &left, const std::string &model,
                                 const std::string &ctx, bool sandbox_on, bool shell_on);

    // "ctx 45k/128k" (with "~" when the size is an estimate); "" when |used| is 0. Pure.
    static std::string ContextText(long long used, bool estimated, int window);

  protected:
    ::ftxui::Element Draw(const Frame &frame) override;

  private:
    // The working directory to show: the bound one, else the launch directory,
    // with $HOME shortened to "~".
    std::string CurrentDir() const;

    std::shared_ptr<const model::SessionModel> session_;
    std::shared_ptr<const port::ITerminalInfo> terminal_;
    std::function<bool()> sandbox_enabled_;
    std::function<bool()> shell_syntax_;
    std::function<int()> sidebar_width_;
    std::function<int()> context_window_;
    std::string model_;
    std::string hint_;
    long long ctx_tokens_ = 0;
    bool ctx_estimated_ = false;
};

} // namespace edc::ui
