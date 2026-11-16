#pragma once

// View: the first-run setup page (SetupDialog).
//
// A wizard whose length depends on the provider: a catalog provider knows its base URL and
// its models, so it takes three steps; anything else takes five, because the wizard has to
// ask for what the catalog would have known.
//
//   catalog:  provider → API key → model
//   custom:   provider → endpoint → API key → model id → context window
//
// Each step is one page: either a "search box over a list" (type to filter; Up/Down move;
// Enter selects) or a single field (Enter submits).
// Esc on any step exits the wizard (and marks it done, so it does not reopen).
//
// The custom path is how an endpoint outside the embedded catalog is configured -- a
// gateway, a self-hosted vLLM, a provider added after the catalog was generated. It is
// reached from the provider step's synthetic "custom" row (see WantsCustomRow), and every
// value it needs is typed: base URL, model id, context window. Nothing here talks to the
// network: a wrong endpoint surfaces on the first message, not as a wizard that hangs.
//
// The catalog (edc_models) and persistence are read/written through IAppFacade; the
// dialog owns only its step state, rendering and events. The custom path needs no facade
// call the catalog path does not already make: IAppFacade::SetSetup takes the whole
// (url, model, key, context_window) quadruple, which is exactly what was typed.

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
    /* One input per step, declared in the SAME order the Tab below is built: step_ is the
     * focused input's index (tab_index_ = step_). Adding a step means adding its input at
     * the same position -- Draw() renders one field while the Tab carries the focus, so a
     * step and its input out of step would leave the caret in a field that is not drawn.
     *
     * A custom endpoint therefore walks all five; the catalog path skips STEP_ENDPOINT and
     * STEP_CONTEXT. The enum holds every step in flow order and the per-path order is what
     * CommitXxx decides, which is why those handlers assign step_ explicitly instead of
     * incrementing it. */
    enum Step
    {
        STEP_PROVIDER = 0,
        STEP_ENDPOINT = 1,
        STEP_KEY = 2,
        STEP_MODEL = 3,
        STEP_CONTEXT = 4,
    };

    /* provider_view_'s slot for the synthetic "custom endpoint" row (it has no catalog id).
     * A sentinel rather than -1 so the vector stays size_t and the two kinds of row share
     * one cursor. */
    static constexpr size_t kCustomRow = static_cast<size_t>(-1);

    /* The context-window default offered for a custom endpoint. Deliberately NOT the
     * config's 1 MiB default (EDC_CONFIG_DEFAULT_CONTEXT_WINDOW): that is the default model
     * family's window, and pre-filling it for an unknown endpoint is how a window bigger than
     * the provider's real one gets saved -- which postpones folding past the provider's own
     * limit and surfaces as an upstream 400 far from the cause. An install that already
     * points outside the catalog keeps whatever window it has (see Open). */
    static constexpr int kDefaultCustomContextWindow = 128000;

    void ReloadProviders();
    void ApplyProviderFilter();
    void ReloadModels();
    void ApplyModelFilter();

    /* Whether the provider list gets the synthetic custom row. True when the user typed a
     * base URL into the search box, typed "custom"/"other" (how you look for something the
     * list does not offer), or when the config already points somewhere the catalog does not
     * know -- the last case so that a custom install's current choice has a row to be ticked
     * in. An empty filter with a catalog-backed config matches none of them, which is what
     * leaves the plain provider list exactly as it was. */
    bool WantsCustomRow() const;

    void CommitProvider(); /* step 0 -> 1 (custom) or -> 2: set the base URL, load the models */
    void CommitEndpoint(); /* custom step 1 -> 2: validate the URL */
    void CommitKey();      /* -> 3 */
    void CommitModel();    /* 3 -> 4 (custom) or finish: persist and close */
    void CommitContext();  /* custom step 4: validate the window, then finish */
    void Finish(const std::string &model_id); /* persist + close; no-op on refusal */
    void Exit();           /* Esc: mark done and close */

    /* 1-based position within the ACTIVE path, and that path's length, for the header. On
     * step 1 the path is not chosen yet, so it counts the catalog's three. */
    int StepOrdinal() const;
    int StepTotal() const;

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
    /* The config points at an endpoint no catalog provider matches: the synthetic row is
     * shown and carries the tick. Recomputed by ApplyProviderFilter. */
    bool config_is_custom_ = false;

    /* The path the user is on. Set by CommitProvider; from then on it decides which steps
     * exist, what the model step collects (a catalog pick vs a typed id) and where the
     * context window comes from (the catalog vs the field on STEP_CONTEXT). */
    bool custom_ = false;

    std::string url_;
    std::string key_;
    int context_window_ = 0;
    /* The typed context window, as text: it is a free field (ftxui's Input is a string), so
     * it is only an int once CommitContext has parsed and validated it. */
    std::string context_text_;

    /* A rejected value keeps the user on its step, with the reason under the field. Cleared
     * by the field's on_change, so typing a correction clears the complaint. */
    bool endpoint_bad_ = false;
    bool context_bad_ = false;

    ::ftxui::Component provider_filter_input_;
    ::ftxui::Component endpoint_input_;
    ::ftxui::Component key_input_;
    ::ftxui::Component model_filter_input_;
    ::ftxui::Component context_input_;
    /* A Tab over the five inputs, in Step order (see the enum): exactly the current step's
     * input is focused (and draws the caret); added as the dialog's child (see the ctor). */
    ::ftxui::Component tabs_;
    int tab_index_ = 0;

    /* Caret position per field, bound into the inputs' InputOption so Open() can park it
     * at the END of a pre-filled value (typing then appends, not prepends). */
    int provider_cursor_ = 0;
    int endpoint_cursor_ = 0;
    int key_cursor_ = 0;
    int model_cursor_ = 0;
    int context_cursor_ = 0;
};

} // namespace edc::ui
