#pragma once

// View: the first-run setup page (SetupDialog).
//
// A modal like ConnectDialog, but the guided first-run flow: pick a provider, pick one
// of that provider's chat models, fill the base URL / API key, then Finish or Skip.
// Finish persists through IAppFacade::SetSetup (LLM settings + the model's context
// window + the onboarded flag); Skip marks the wizard done without touching the LLM
// settings. The catalog (edc_models) is read through the facade; the dialog owns only
// its form state, rendering and events.

#include "edc_app_facade.hpp" /* port::IAppFacade, port::SetupConfig */
#include "edc_widget.hpp"     /* ui::Widget (Template Method base) */

#include "ftxui/component/component_base.hpp"

#include <string>
#include <vector>

namespace edc::ui
{

class SetupDialog : public Widget
{
  public:
    // |facade| is borrowed and must outlive this dialog; it is not touched until Open().
    explicit SetupDialog(port::IAppFacade *facade);

    // Seed from |initial| (the current config), (re)load the catalog and open.
    void Open(const port::SetupConfig &initial);
    void Close();
    bool IsOpen() const
    {
        return open_;
    }

    // Forwarded by the window only while open: Esc = Skip, arrows move between
    // providers/models/fields, Enter submits from a field.
    bool OnEvent(::ftxui::Event event) override;

  protected:
    ::ftxui::Element Draw(const Frame &frame) override;

  private:
    void ReloadProviders();
    void OnProviderPicked();
    void ReloadModels();
    void OnFilterChanged();
    void OnModelPicked();
    void RefreshInfo();
    bool SubmitNow();
    void SkipNow();
    void RunTest();

    port::IAppFacade *facade_;

    bool open_ = false;

    std::vector<std::string> provider_ids_;
    std::vector<std::string> provider_labels_;
    int provider_sel_ = 0;

    std::vector<std::string> model_ids_;    /* the provider's chat-capable models */
    std::vector<std::string> model_labels_; /* the filtered view shown in the menu */
    int model_sel_ = 0;
    std::string filter_;

    std::string url_;
    std::string key_;
    std::string info_;   /* "context 200000 · output 8192" for the highlighted model */
    std::string status_; /* the Test button's result line */
    int context_window_ = 0;

    ::ftxui::Component provider_menu_;
    ::ftxui::Component model_menu_;
    ::ftxui::Component filter_input_;
    ::ftxui::Component url_input_;
    ::ftxui::Component key_input_;
    ::ftxui::Component buttons_;
    ::ftxui::Component left_;
    ::ftxui::Component right_;
    ::ftxui::Component root_;
    int focus_ = 0;        /* left vs right column */
    int button_focus_ = 0; /* Finish vs Skip */
};

} // namespace edc::ui
