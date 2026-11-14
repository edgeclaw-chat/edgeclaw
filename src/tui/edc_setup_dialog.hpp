#pragma once

// View: the first-run setup page (SetupDialog).
//
// A three-step wizard, each step a "search box over a list" page:
//   1. choose a provider   (type to filter; Up/Down move; Enter selects)
//   2. enter the API key    (the base URL is shown/editable; Enter continues)
//   3. choose a model       (same search+list style; Enter finishes)
// Esc on any step exits the wizard (and marks it done, so it does not reopen).
//
// The catalog (edc_models) and persistence are read/written through IAppFacade; the
// dialog owns only its step state, rendering and events.

#include "edc_app_facade.hpp" /* port::IAppFacade, port::SetupConfig */
#include "edc_widget.hpp"     /* ui::Widget (Template Method base) */

#include "ftxui/component/component_base.hpp"

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace edc::ui
{

class SetupDialog : public Widget
{
  public:
    // |facade| is borrowed and must outlive this dialog; it is not touched until Open().
    // |on_done| runs after a successful Finish (the host refreshes the footer's model).
    explicit SetupDialog(port::IAppFacade *facade, std::function<void()> on_done = nullptr);

    // Seed from |initial| (the current config), (re)load the catalog and open at step 1.
    void Open(const port::SetupConfig &initial);
    void Close();
    bool IsOpen() const
    {
        return open_;
    }

    /* The content width, and the rendered width including the two cells BorderStyle::EMPTY
     * reserves. The host needs PanelWidth() to place the panel -- see edc_modal.hpp. */
    static constexpr int kContentWidth = 76;
    static constexpr int PanelWidth()
    {
        return kContentWidth + 2;
    }

    // The window forwards events here while open (Esc exits, Up/Down move, Enter selects,
    // the rest is typed into the current step's field).
    bool OnEvent(::ftxui::Event event) override;

  protected:
    ::ftxui::Element Draw(const Frame &frame) override;

  private:
    enum Step
    {
        STEP_PROVIDER = 0,
        STEP_KEY = 1,
        STEP_MODEL = 2,
    };

    void ReloadProviders();
    void ApplyProviderFilter();
    void ReloadModels();
    void ApplyModelFilter();

    void CommitProvider(); /* step 0 -> 1: set the base URL + load the models */
    void CommitKey();      /* step 1 -> 2 */
    void CommitModel();    /* step 2: finish (persist) and close */
    void Exit();           /* Esc: mark done and close */

    /* A scrolling list: |sel| is the keyboard cursor (highlighted), |current_row| is the
     * item the config currently uses (marked with a tick), -1 for none. */
    ::ftxui::Element ListElement(const std::vector<std::string> &rows, int sel, int current_row, int max_rows) const;

    port::IAppFacade *facade_;
    std::function<void()> on_done_; /* host refresh after a successful Finish */

    bool open_ = false;
    Step step_ = STEP_PROVIDER;

    std::vector<std::string> provider_ids_; /* all provider ids, sorted */
    std::string provider_filter_;
    std::vector<std::string> provider_rows_; /* filtered labels (view) */
    std::vector<size_t> provider_view_;      /* index into provider_ids_ per row */
    int provider_sel_ = 0;

    std::vector<std::string> model_ids_; /* the provider's chat-capable models */
    std::string model_filter_;
    std::vector<std::string> model_rows_;
    std::vector<size_t> model_view_;
    int model_sel_ = 0;

    /* The config's current choice, ticked in the lists (distinct from the cursor). */
    std::string current_api_;
    std::string current_model_;
    int provider_current_row_ = -1;
    int model_current_row_ = -1;

    std::string url_;
    std::string key_;
    int context_window_ = 0;

    ::ftxui::Component provider_filter_input_;
    ::ftxui::Component key_input_;
    ::ftxui::Component model_filter_input_;
    /* A Tab over the three inputs, so exactly the current step's input is focused (and
     * draws the caret); added as the dialog's child (see the ctor). */
    ::ftxui::Component tabs_;
    int tab_index_ = 0;

    /* Caret position per field, bound into the inputs' InputOption so Open() can park it
     * at the END of a pre-filled value (typing then appends, not prepends). */
    int provider_cursor_ = 0;
    int key_cursor_ = 0;
    int model_cursor_ = 0;
};

} // namespace edc::ui
