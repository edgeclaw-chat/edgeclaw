#include "edc_setup_dialog.hpp"

#include "edc_style.hpp"

#include "ftxui/component/component.hpp"
#include "ftxui/component/component_options.hpp"
#include "ftxui/component/event.hpp"
#include "ftxui/dom/elements.hpp"

#include <algorithm>

namespace edc::ui
{

SetupDialog::SetupDialog(port::IAppFacade *facade) : facade_(facade)
{
    // Flat, borderless buttons + fields: same look as ConnectDialog (see it for why the
    // default Input transform hides the caret and why buttons are not boxed).
    ::ftxui::InputOption field_option;
    field_option.multiline = false;
    field_option.transform = [](const ::ftxui::InputState &state) {
        const auto &pal = style::palette();
        ::ftxui::Element element = state.element | ::ftxui::color(pal.text);
        if (state.is_placeholder)
            element |= ::ftxui::dim;
        return element;
    };
    url_input_ = ::ftxui::Input(&url_, "https://api.example.com/v1", field_option);
    key_input_ = ::ftxui::Input(&key_, "API key", field_option);

    ::ftxui::InputOption filter_option = field_option;
    filter_option.on_change = [this] { OnFilterChanged(); };
    filter_input_ = ::ftxui::Input(&filter_, "filter models", filter_option);

    ::ftxui::MenuOption menu_option = ::ftxui::MenuOption::Vertical();
    menu_option.entries_option.transform = [](const ::ftxui::EntryState &s) {
        const auto &pal = style::palette();
        auto label = ::ftxui::text(s.label);
        if (s.focused)
            return label | ::ftxui::bold | ::ftxui::color(pal.accent);
        return label | ::ftxui::color(s.active ? pal.text : pal.text_dim);
    };
    menu_option.on_change = [this] { OnProviderPicked(); };
    provider_menu_ = ::ftxui::Menu(&provider_labels_, &provider_sel_, menu_option);

    ::ftxui::MenuOption model_option = ::ftxui::MenuOption::Vertical();
    model_option.entries_option.transform = [](const ::ftxui::EntryState &s) {
        const auto &pal = style::palette();
        auto label = ::ftxui::text(s.label);
        if (s.focused)
            return label | ::ftxui::bold | ::ftxui::color(pal.accent);
        return label | ::ftxui::color(s.active ? pal.text : pal.text_dim);
    };
    model_option.on_change = [this] { OnModelPicked(); };
    model_menu_ = ::ftxui::Menu(&model_labels_, &model_sel_, model_option);

    ::ftxui::ButtonOption flat_button;
    flat_button.transform = [](const ::ftxui::EntryState &s) {
        const auto &pal = style::palette();
        auto label = ::ftxui::text(" " + s.label + " ");
        if (s.focused)
            return label | ::ftxui::bold | ::ftxui::color(pal.accent);
        return label | ::ftxui::color(pal.text);
    };
    auto test = ::ftxui::Button("Test", [this] { RunTest(); }, flat_button);
    auto finish = ::ftxui::Button("Finish", [this] { SubmitNow(); }, flat_button);
    auto skip = ::ftxui::Button("Skip", [this] { SkipNow(); }, flat_button);
    buttons_ = ::ftxui::Container::Horizontal({test, finish, skip}, &button_focus_);

    // One vertical focus order: the menus own Up/Down for their own selection, Tab walks
    // between them / the fields / the buttons.
    root_ =
        ::ftxui::Container::Vertical({provider_menu_, filter_input_, model_menu_, url_input_, key_input_, buttons_});
    Add(root_);
}

void SetupDialog::ReloadProviders()
{
    provider_ids_.clear();
    provider_labels_.clear();
    if (facade_)
        facade_->Providers(&provider_ids_);
    for (const std::string &id : provider_ids_)
    {
        std::string name = facade_ ? facade_->ProviderName(id) : "";
        provider_labels_.push_back(name.empty() ? id : name + "  (" + id + ")");
    }
    if (provider_labels_.empty())
        provider_labels_.push_back("(no providers in the catalog)");
    provider_sel_ = 0;
    OnProviderPicked();
}

void SetupDialog::OnProviderPicked()
{
    if (!facade_ || provider_sel_ < 0 || static_cast<size_t>(provider_sel_) >= provider_ids_.size())
        return;
    const std::string &id = provider_ids_[provider_sel_];
    /* Prefill the base URL from the catalog (still editable). */
    std::string api = facade_->ProviderApi(id);
    if (!api.empty())
        url_ = api;
    ReloadModels();
}

void SetupDialog::ReloadModels()
{
    model_ids_.clear();
    if (facade_ && provider_sel_ >= 0 && static_cast<size_t>(provider_sel_) < provider_ids_.size())
    {
        std::vector<std::string> all;
        facade_->Models(provider_ids_[provider_sel_], &all);
        for (const std::string &mid : all)
        {
            port::ModelFacts f;
            if (facade_->ModelInfo(mid, &f) && f.chat)
                model_ids_.push_back(mid);
        }
    }
    OnFilterChanged();
}

void SetupDialog::OnFilterChanged()
{
    model_labels_.clear();
    for (const std::string &mid : model_ids_)
    {
        port::ModelFacts f;
        std::string name = facade_ && facade_->ModelInfo(mid, &f) ? f.name : "";
        if (filter_.empty() || mid.find(filter_) != std::string::npos || name.find(filter_) != std::string::npos)
            model_labels_.push_back(mid);
    }
    if (model_labels_.empty())
        model_labels_.push_back(model_ids_.empty() ? "(no chat models)" : "(no match)");
    model_sel_ = 0;
    OnModelPicked();
}

void SetupDialog::OnModelPicked()
{
    RefreshInfo();
}

void SetupDialog::RefreshInfo()
{
    context_window_ = 0;
    info_.clear();
    if (!facade_ || model_sel_ < 0 || static_cast<size_t>(model_sel_) >= model_labels_.size())
        return;
    const std::string &mid = model_labels_[model_sel_];
    port::ModelFacts f;
    if (!facade_->ModelInfo(mid, &f))
        return;
    context_window_ = f.context;
    info_ = "context " + std::to_string(f.context) + "  ·  output " + std::to_string(f.output);
}

bool SetupDialog::SubmitNow()
{
    if (!facade_ || url_.empty() || key_.empty() || model_sel_ < 0 ||
        static_cast<size_t>(model_sel_) >= model_labels_.size())
        return false;
    const std::string &mid = model_labels_[model_sel_];
    if (mid.rfind("(", 0) == 0) /* the placeholder rows */
        return false;
    port::SetupConfig cfg;
    cfg.api_url = url_;
    cfg.model = mid;
    cfg.api_key = key_;
    cfg.context_window = context_window_;
    if (!facade_->SetSetup(cfg))
        return false;
    Close();
    return true;
}

void SetupDialog::SkipNow()
{
    if (facade_)
        facade_->MarkOnboarded();
    Close();
}

void SetupDialog::RunTest()
{
    if (!facade_)
    {
        status_ = "app not ready";
        return;
    }
    /* BLOCKING (a few seconds): acceptable on a one-time setup page. Runs on the UI
     * thread, so the window cannot repaint until it returns -- deliberate, to avoid a
     * worker thread for one button. */
    status_ = facade_->TestEndpoint(url_, key_);
}

void SetupDialog::Open(const port::SetupConfig &initial)
{
    url_ = initial.api_url;
    key_ = initial.api_key;
    filter_.clear();
    ReloadProviders();
    open_ = true;
}

void SetupDialog::Close()
{
    open_ = false;
}

bool SetupDialog::OnEvent(::ftxui::Event event)
{
    if (!open_)
        return false;
    // Esc = Skip (accept the default config): the one-time page must never trap the user.
    if (event == ::ftxui::Event::Escape)
    {
        SkipNow();
        return true;
    }
    return root_->OnEvent(event);
}

::ftxui::Element SetupDialog::Draw(const Frame &)
{
    using namespace ::ftxui;
    const auto &pal = style::palette();

    auto section = [&](const char *title) { return text(title) | bold | color(pal.accent); };

    auto left = vbox({
                    section("Provider"),
                    provider_menu_->Render() | vscroll_indicator | frame | flex,
                }) |
                flex;
    auto right = vbox({
                     section("Model"),
                     filter_input_->Render(),
                     model_menu_->Render() | vscroll_indicator | frame | flex,
                     text(info_.empty() ? " " : info_) | color(pal.text_dim),
                 }) |
                 flex;
    auto form = vbox({
        section("Base URL"),
        url_input_->Render(),
        section("API key"),
        key_input_->Render(),
        text(status_.empty() ? " " : status_) | color(pal.text_dim),
        separator() | color(pal.line_dim),
        hbox({filler(), buttons_->Render()}),
    });

    return vbox({
               text(" Welcome to EdgeClaw — pick a provider and a model ") | bold | color(pal.accent),
               text("Enter an API key, then Finish. Skip leaves the settings as they are.") | color(pal.text_dim),
               separator() | color(pal.line_dim),
               hbox({left, separator() | color(pal.line_dim), right, separator() | color(pal.line_dim), form}) | flex,
           }) |
           borderStyled(BorderStyle::ROUNDED) | bgcolor(pal.dialog_bg);
}

} // namespace edc::ui
