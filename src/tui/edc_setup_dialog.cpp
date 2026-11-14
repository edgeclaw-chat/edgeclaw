#include "edc_setup_dialog.hpp"

#include "edc_style.hpp"

#include "ftxui/component/component.hpp"
#include "ftxui/component/component_options.hpp"
#include "ftxui/component/event.hpp"
#include "ftxui/dom/elements.hpp"

#include <algorithm>
#include <utility>

namespace edc::ui
{

namespace
{
/* At most this many list rows; the list grows/shrinks with the matches (the dialog's top
 * stays anchored, so it does not jump). The width is fixed so the centered column does
 * not shift. */
constexpr int kListRows = 12;
constexpr int kDialogWidth = 76;

/* Same base URL: the config may hold a base or a full endpoint, with or without a
 * trailing slash. */
bool same_url(const std::string &a, const std::string &b)
{
    auto norm = [](std::string s) {
        static const std::string suffix = "/chat/completions";
        if (s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0)
            s.erase(s.size() - suffix.size());
        while (!s.empty() && s.back() == '/')
            s.pop_back();
        return s;
    };
    return !a.empty() && !b.empty() && norm(a) == norm(b);
}

/* Field look: the palette's own foreground, placeholder dimmed. (ftxui's default
 * `inverted` fill paints a full-width slab that hides the terminal caret.) */
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

SetupDialog::SetupDialog(port::IAppFacade *facade, std::function<void()> on_done)
    : facade_(facade), on_done_(std::move(on_done))
{
    ::ftxui::InputOption pfilter = field_option();
    pfilter.on_change = [this] { ApplyProviderFilter(); };
    pfilter.cursor_position = &provider_cursor_;
    provider_filter_input_ = ::ftxui::Input(&provider_filter_, "Search providers...", pfilter);

    ::ftxui::InputOption keyopt = field_option();
    keyopt.cursor_position = &key_cursor_;
    key_input_ = ::ftxui::Input(&key_, "API key", keyopt);

    ::ftxui::InputOption mfilter = field_option();
    mfilter.on_change = [this] { ApplyModelFilter(); };
    mfilter.cursor_position = &model_cursor_;
    model_filter_input_ = ::ftxui::Input(&model_filter_, "Search models...", mfilter);

    /* Adopt the inputs as a real component child: without a child the dialog is not
     * Focusable(), so its current input never reports Focused() and ftxui never asks the
     * terminal for a caret. Draw() still renders only the active step's input; this link
     * carries focus. */
    tabs_ = ::ftxui::Container::Tab({provider_filter_input_, key_input_, model_filter_input_}, &tab_index_);
    Add(tabs_);
}

void SetupDialog::ReloadProviders()
{
    provider_ids_.clear();
    if (facade_)
        facade_->Providers(&provider_ids_);
    ApplyProviderFilter();
    if (provider_current_row_ >= 0)
        provider_sel_ = provider_current_row_; /* start the cursor on the current choice */
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

    /* Which provider the config currently uses (by base URL, else by containing the
     * current model) -- shown with a tick. */
    provider_current_row_ = -1;
    int match = -1;
    if (facade_ && !current_api_.empty())
    {
        for (size_t i = 0; i < provider_ids_.size(); i++)
            if (same_url(facade_->ProviderApi(provider_ids_[i]), current_api_))
            {
                match = static_cast<int>(i);
                break;
            }
    }
    if (match < 0 && facade_ && !current_model_.empty())
    {
        for (size_t i = 0; i < provider_ids_.size() && match < 0; i++)
        {
            std::vector<std::string> ms;
            facade_->Models(provider_ids_[i], &ms);
            for (const std::string &m : ms)
                if (m == current_model_)
                {
                    match = static_cast<int>(i);
                    break;
                }
        }
    }
    for (size_t r = 0; r < provider_view_.size(); r++)
        if (static_cast<int>(provider_view_[r]) == match)
        {
            provider_current_row_ = static_cast<int>(r);
            break;
        }
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
    ApplyModelFilter();
    if (model_current_row_ >= 0)
        model_sel_ = model_current_row_; /* start the cursor on the current model */
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
            /* Row text = the catalog's display name plus the wire id, the same shape the
             * provider step uses. Id-only rows hide the newest model on a provider that
             * renames it (DeepSeek's V4.1 lane answers to `deepseek-flash`), and the
             * filter already matches on the name, so the list must show what matched. */
            model_rows_.push_back(name.empty() || name == model_ids_[i]
                                     ? model_ids_[i]
                                     : name + "  (" + model_ids_[i] + ")");
            model_view_.push_back(i);
        }
    }
    if (model_sel_ >= static_cast<int>(model_view_.size()))
        model_sel_ = model_view_.empty() ? 0 : static_cast<int>(model_view_.size()) - 1;

    model_current_row_ = -1;
    for (size_t r = 0; r < model_view_.size(); r++)
        if (model_ids_[model_view_[r]] == current_model_)
        {
            model_current_row_ = static_cast<int>(r);
            break;
        }
}

void SetupDialog::CommitProvider()
{
    if (provider_view_.empty())
        return;
    const std::string &pid = provider_ids_[provider_view_[provider_sel_]];
    std::string api = facade_ ? facade_->ProviderApi(pid) : "";
    if (!api.empty())
        url_ = api; /* the base URL is bound to the provider (never shown) */
    ReloadModels();
    step_ = STEP_KEY;
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
    {
        if (on_done_)
            on_done_(); /* host refreshes the footer's model */
        Close();
    }
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
    current_api_ = initial.api_url; /* ticked in the provider list */
    current_model_ = initial.model; /* ticked in the model list */
    provider_filter_.clear();
    model_filter_.clear();
    provider_sel_ = 0;
    model_sel_ = 0;
    /* Park each caret at the END of its value: the user is editing an existing value and
     * the next keystroke should append (the API key is pre-filled from the config). */
    provider_cursor_ = static_cast<int>(provider_filter_.size());
    key_cursor_ = static_cast<int>(key_.size());
    model_cursor_ = static_cast<int>(model_filter_.size());
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
        if (event == ::ftxui::Event::Return)
        {
            CommitKey();
            return true;
        }
        return key_input_->OnEvent(event);

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

::ftxui::Element SetupDialog::ListElement(const std::vector<std::string> &rows, int sel, int current_row,
                                          int max_rows) const
{
    using namespace ::ftxui;
    const auto &pal = style::palette();

    int n = static_cast<int>(rows.size());
    if (n == 0)
        return text("(no match)") | color(pal.text_dim);

    int first = 0;
    if (n > max_rows)
    {
        first = sel - max_rows / 2;
        if (first < 0)
            first = 0;
        if (first > n - max_rows)
            first = n - max_rows;
    }

    /* Only the rows that exist (at most max_rows). The keyboard cursor is the highlight
     * colour; the config's current choice carries a tick. */
    Elements lines;
    for (int i = first; i < n && i < first + max_rows; i++)
    {
        bool cur = (i == sel);
        std::string prefix = (i == current_row) ? "\u2713 " : "  ";
        auto row = text(prefix + rows[i]);
        /* The cursor is a background fill (not a font colour); the tick marks the config's
         * current choice. */
        lines.push_back(cur ? (row | bold | bgcolor(pal.accent) | color(pal.dialog_bg)) : (row | color(pal.text)));
    }
    return vbox(std::move(lines));
}

::ftxui::Element SetupDialog::Draw(const Frame &)
{
    using namespace ::ftxui;
    const auto &pal = style::palette();

    /* The active Tab child is the current step's input, so it is the focused one (and the
     * only one that draws a caret). */
    tab_index_ = static_cast<int>(step_);

    /* A 2-column left gutter, matching the list's "✓ "/"  " prefix, so the title, the
     * inputs and the list rows all share one text margin. */
    auto field = [&](const Component &input) { return hbox({text("  "), input->Render() | flex}); };

    Element body;
    if (step_ == STEP_PROVIDER)
    {
        body = vbox({
            field(provider_filter_input_), /* the search box itself: its placeholder reads as the prompt */
            separator() | color(pal.line_dim) | bgcolor(pal.dialog_bg),
            ListElement(provider_rows_, provider_sel_, provider_current_row_, kListRows),
        });
    }
    else if (step_ == STEP_KEY)
    {
        /* Just the field: the base URL is bound to the provider, and a nav hint would be
         * noise on a single-field step. */
        body = field(key_input_);
    }
    else
    {
        body = vbox({
            field(model_filter_input_),
            separator() | color(pal.line_dim) | bgcolor(pal.dialog_bg),
            ListElement(model_rows_, model_sel_, model_current_row_, kListRows),
        });
    }

    const char *title = step_ == STEP_PROVIDER ? "Choose a provider" : step_ == STEP_KEY ? "API key" : "Choose a model";
    std::string step_no = "Step " + std::to_string(static_cast<int>(step_) + 1) + "/3";

    // The header IS the step's name (not a generic "Setup"); flat and borderless. A
    // right-aligned footer shows the keys that apply here
    // (the list steps move the cursor; the key step submits).
    const char *hint = step_ == STEP_KEY ? "Enter submit  |  Esc exit" : "Up/Down move  |  Enter select  |  Esc exit";

    Elements rows;
    rows.push_back(hbox({text("  " + std::string(title)) | bold | color(pal.accent), filler(),
                         text(step_no + " ") | color(pal.text_dim)}));
    rows.push_back(separator() | color(pal.line_dim) | bgcolor(pal.dialog_bg));
    rows.push_back(body);
    rows.push_back(separator() | color(pal.line_dim) | bgcolor(pal.dialog_bg));
    rows.push_back(hbox({filler(), text(hint) | color(pal.text_dim)}));
    return vbox(std::move(rows)) | size(WIDTH, EQUAL, kDialogWidth) | borderStyled(BorderStyle::EMPTY) |
           bgcolor(pal.dialog_bg);
}

} // namespace edc::ui
