#pragma once

// View: the /connect dialog (ConnectDialog).
//
// Its state, rendering, events and submit used to be welded into TUIWindow (6
// members + 3 methods + a branch in OnEvent). It is a self-contained component:
// three fields (API URL / Model / API key) plus Connect/Cancel, owning its form
// state, rendering and event handling.
//
// The window only opens it (Open pre-fills from the current config) and draws it
// as an overlay. Config persistence/hot-apply goes through the Submit callback
// given at construction, so the dialog never sees IAppFacade.

#include "edc_app_facade.hpp" /* port::ConnectConfig */
#include "edc_widget.hpp"     /* ui::Widget (Template Method base) */

#include "ftxui/component/component_base.hpp"

#include <functional>
#include <string>

namespace edc::ui
{

class ConnectDialog : public Widget
{
  public:
    // Submit callback: hand the filled config to the host (persist + hot-apply).
    // true = success, the dialog closes itself.
    using Submit = std::function<bool(const port::ConnectConfig &)>;
    explicit ConnectDialog(Submit submit);

    // Pre-fill the three fields from |initial| and open.
    void Open(const port::ConnectConfig &initial);
    void Close();
    bool IsOpen() const
    {
        return open_;
    }

    // Forwarded by the window only while open: Esc closes, Up/Down move between
    // rows, Enter submits (from a field) or runs the focused button, the rest
    // goes to the fields.
    bool OnEvent(::ftxui::Event event) override;

  protected:
    // Widget template method: draw only (OnRender is fixed to Draw + reflect).
    ::ftxui::Element Draw(const Frame &frame) override;

  private:
    // Rows the Up/Down keys walk: the three text fields plus the button row.
    static constexpr int kRows = 4;

    // Collect the fields and call Submit; close on success.
    bool SubmitNow();

    // Move the row focus by |dir| (-1 up / +1 down), clamped to the rows.
    void MoveRow(int dir);

    bool open_ = false;
    std::string url_;
    std::string model_;
    std::string key_;

    // Where the caret sits in each field. Bound into the inputs' InputOption so
    // Open() can park it at the end of the pre-filled value (typing then appends
    // instead of prepending).
    int url_cursor_ = 0;
    int model_cursor_ = 0;
    int key_cursor_ = 0;

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
