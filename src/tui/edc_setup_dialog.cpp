#include "edc_setup_dialog.hpp"

#include "edc_style.hpp"

#include "ftxui/component/component.hpp"
#include "ftxui/component/component_options.hpp"
#include "ftxui/component/event.hpp"
#include "ftxui/dom/elements.hpp"

#include <algorithm>

namespace edc::ui
{

namespace
{
/* The steps render a scrolling list; show this many rows at once. */
constexpr int kListRows = 12;

/* Field look: the palette's own foreground, placeholder dimmed (same as ConnectDialog:
 * ftxui's default `inverted` fill hides the terminal caret). */
::ftxui::InputOption field_option()
{
    ::ftxui::InputOption opt;
    opt.multiline = false;
    opt.transform = [](const ::ftxui::InputState &state) {
        const auto &pal = style::palette();
        ::ftxui::Element element = state.element | ::ftxui::color(pal.text);
        if (state.is_placeholder)
            element |= ::ftxui::dim;
        return element;
    };
    return opt;
}
} // namespace

SetupDialog::SetupDialog(port::IAppFacade *facade) : facade_(facade)
{
    ::ftxui::InputOption pfilter = field_option();
    pfilter.on_change = [this] { ApplyProviderFilter(); };
    provider_filter_input_ = ::ftxui::Input(&provider_filter_, "type to filter", pfilter);

    url_input_ = ::ftxui::Input(&url_, "https://api.example.com/v1", field_option());
    key_input_ = ::ftxui::Input(&key_, "API key", field_option());

    ::ftxui::InputOption mfilter = field_option();
    mfilter.on_change = [this] { ApplyModelFilter(); };
    model_filter_input_ = ::ftxui::Input(&model_filter_, "type to filter", mfilter);
}

void SetupDialog::ReloadProviders()
{
    provider_ids_.clear();
    if (facade_)
        facade_->Providers(&provider_ids_);
    ApplyProviderFilter();
}

void SetupDialog::ApplyProviderFilter()
{
    provider_rows_.clear();
    provider_view_.clear();
    for (size_t i = 0; i < provider_ids_.size(); i++)
    {
        const std::string &id = provider_ids_[i];
        std::string name = facade_ ? facade_->ProviderName(id) : "";
        if (provider_filter_.empty() || id.find(provider_filter_) != std::string::npos ||
            name.find(provider_filter_) != std::string::npos)
        {
            provider_rows_.push_back(name.empty() ? id : name + "  (" + id + ")");
            provider_view_.push_back(i);
        }
    }
    if (provider_sel_ >= static_cast<int>(provider_view_.size()))
        provider_sel_ = provider_view_.empty() ? 0 : static_cast<int>(provider_view_.size()) - 1;
}

void SetupDialog::ReloadModels()
{
    model_ids_.clear();
    if (facade_ && provider_sel_ >= 0 && static_cast<size_t>(provider_sel_) < provider_view_.size())
    {
        const std::string &pid = provider_ids_[provider_view_[provider_sel_]];
        std::vector<std::string> all;
        facade_->Models(pid, &all);
        for (const std::string &mid : all)
        {
            port::ModelFacts f;
            if (facade_->ModelInfo(mid, &f) && f.chat)
                model_ids_.push_back(mid);
        }
    }
    model_sel_ = 0;
    ApplyModelFilter();
}

void SetupDialog::ApplyModelFilter()
{
    model_rows_.clear();
    model_view_.clear();
    for (size_t i = 0; i < model_ids_.size(); i++)
    {
        port::ModelFacts f;
        std::string name = facade_ && facade_->ModelInfo(model_ids_[i], &f) ? f.name : "";
        if (model_filter_.empty() || model_ids_[i].find(model_filter_) != std::string::npos ||
            name.find(model_filter_) != std::string::npos)
        {
            model_rows_.push_back(model_ids_[i]);
            model_view_.push_back(i);
        }
    }
    if (model_sel_ >= static_cast<int>(model_view_.size()))
        model_sel_ = model_view_.empty() ? 0 : static_cast<int>(model_view_.size()) - 1;
}

void SetupDialog::CommitProvider()
{
    if (provider_view_.empty())
        return;
    const std::string &pid = provider_ids_[provider_view_[provider_sel_]];
    std::string api = facade_ ? facade_->ProviderApi(pid) : "";
    if (!api.empty())
        url_ = api;
    ReloadModels();
    step_ = STEP_KEY;
    key_focus_ = 0;
}

void SetupDialog::CommitKey()
{
    step_ = STEP_MODEL;
}

void SetupDialog::CommitModel()
{
    if (!facade_ || model_view_.empty())
        return;
    const std::string &mid = model_ids_[model_view_[model_sel_]];
    port::ModelFacts f;
    context_window_ = facade_->ModelInfo(mid, &f) ? f.context : 0;
    port::SetupConfig cfg;
    cfg.api_url = url_;
    cfg.model = mid;
    cfg.api_key = key_;
    cfg.context_window = context_window_;
    if (facade_->SetSetup(cfg))
        Close();
}

void SetupDialog::Exit()
{
    if (facade_)
        facade_->MarkOnboarded();
    Close();
}

void SetupDialog::Open(const port::SetupConfig &initial)
{
    url_ = initial.api_url;
    key_ = initial.api_key;
    provider_filter_.clear();
    model_filter_.clear();
    provider_sel_ = 0;
    model_sel_ = 0;
    key_focus_ = 0;
    step_ = STEP_PROVIDER;
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
    if (event == ::ftxui::Event::Escape)
    {
        Exit();
        return true;
    }

    switch (step_)
    {
    case STEP_PROVIDER:
        if (event == ::ftxui::Event::ArrowUp)
            return provider_sel_ > 0 ? (provider_sel_--, true) : true;
        if (event == ::ftxui::Event::ArrowDown)
            return provider_sel_ + 1 < static_cast<int>(provider_view_.size()) ? (provider_sel_++, true) : true;
        if (event == ::ftxui::Event::Return)
        {
            CommitProvider();
            return true;
        }
        return provider_filter_input_->OnEvent(event);

    case STEP_KEY:
        if (event == ::ftxui::Event::ArrowUp || event == ::ftxui::Event::ArrowDown)
        {
            key_focus_ = (event == ::ftxui::Event::ArrowDown) ? 1 : 0;
            return true;
        }
        if (event == ::ftxui::Event::Return)
        {
            CommitKey();
            return true;
        }
        return (key_focus_ == 0 ? url_input_ : key_input_)->OnEvent(event);

    case STEP_MODEL:
        if (event == ::ftxui::Event::ArrowUp)
            return model_sel_ > 0 ? (model_sel_--, true) : true;
        if (event == ::ftxui::Event::ArrowDown)
            return model_sel_ + 1 < static_cast<int>(model_view_.size()) ? (model_sel_++, true) : true;
        if (event == ::ftxui::Event::Return)
        {
            CommitModel();
            return true;
        }
        return model_filter_input_->OnEvent(event);
    }
    return false;
}

::ftxui::Element SetupDialog::ListElement(const std::vector<std::string> &rows, int sel, int max_rows) const
{
    using namespace ::ftxui;
    const auto &pal = style::palette();
    if (rows.empty())
        return text("  (no match)") | color(pal.text_dim);

    int n = static_cast<int>(rows.size());
    int first = sel - max_rows / 2;
    if (first < 0)
        first = 0;
    if (first > n - max_rows)
        first = std::max(0, n - max_rows);

    Elements lines;
    for (int i = first; i < n && i < first + max_rows; i++)
    {
        bool cur = (i == sel);
        auto row = text((cur ? "> " : "  ") + rows[i]);
        lines.push_back(cur ? (row | bold | color(pal.accent)) : (row | color(pal.text)));
    }
    return vbox(std::move(lines));
}

::ftxui::Element SetupDialog::Draw(const Frame &)
{
    using namespace ::ftxui;
    const auto &pal = style::palette();

    Element body;
    if (step_ == STEP_PROVIDER)
    {
        body = vbox({
            hbox({text("Search ") | color(pal.text), provider_filter_input_->Render() | flex}),
            separator() | color(pal.line_dim),
            ListElement(provider_rows_, provider_sel_, kListRows),
        });
    }
    else if (step_ == STEP_KEY)
    {
        body = vbox({
            hbox({text((key_focus_ == 0 ? "> " : "  ") + std::string("Base URL  ")) | color(pal.text),
                  url_input_->Render() | flex}),
            hbox({text((key_focus_ == 1 ? "> " : "  ") + std::string("API key   ")) | color(pal.text),
                  key_input_->Render() | flex}),
            text(" "),
            text("Leave the key empty for a local endpoint.") | color(pal.text_dim),
        });
    }
    else
    {
        body = vbox({
            hbox({text("Search ") | color(pal.text), model_filter_input_->Render() | flex}),
            separator() | color(pal.line_dim),
            ListElement(model_rows_, model_sel_, kListRows),
        });
    }

    const char *title = step_ == STEP_PROVIDER ? "Choose a provider" : step_ == STEP_KEY ? "API key" : "Choose a model";
    std::string step_no = "Step " + std::to_string(static_cast<int>(step_) + 1) + "/3";

    return vbox({
               hbox({text(" Setup ") | bold | color(pal.accent), filler(), text(step_no + " ") | color(pal.text_dim)}),
               separator() | color(pal.line_dim),
               text(title) | bold | color(pal.accent),
               body,
               separator() | color(pal.line_dim),
               text("up/down move · Enter select · Esc cancel") | color(pal.text_dim),
           }) |
           borderStyled(BorderStyle::ROUNDED) | bgcolor(pal.dialog_bg);
}

} // namespace edc::ui
