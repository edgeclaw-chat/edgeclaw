#include "edc_setup_dialog.hpp"

#include "edc_style.hpp"

#include "ftxui/component/component.hpp"
#include "ftxui/component/component_options.hpp"
#include "ftxui/component/event.hpp"
#include "ftxui/dom/elements.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace edc::ui
{

namespace
{
/* At most this many list rows; the list grows/shrinks with the matches (the dialog's top
 * stays anchored, so it does not jump). The width is fixed so the centered column does
 * not shift. */
constexpr int kListRows = 12;
/* The number itself lives on the class, because the modal host needs it too (see
 * edc_modal.hpp). */
constexpr int kDialogWidth = SetupDialog::kContentWidth;

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

/* An endpoint the wizard can use: an absolute http(s) URL, nothing more. Guessing further
 * would only reject valid input -- edc_url_chat_completions() appends /chat/completions to a
 * base and passes a full endpoint through, so both shapes are legal. */
bool looks_like_url(const std::string &s)
{
    auto starts_with = [&s](const char *prefix) {
        const size_t n = strlen(prefix);
        if (s.size() < n)
            return false;
        for (size_t i = 0; i < n; i++)
            if (tolower(static_cast<unsigned char>(s[i])) != prefix[i])
                return false;
        return true;
    };
    return starts_with("http://") || starts_with("https://");
}

bool contains_ci(const std::string &hay, const char *needle)
{
    const size_t n = strlen(needle);
    if (n == 0 || hay.size() < n)
        return false;
    for (size_t i = 0; i + n <= hay.size(); i++)
    {
        size_t j = 0;
        while (j < n && tolower(static_cast<unsigned char>(hay[i + j])) ==
                           tolower(static_cast<unsigned char>(needle[j])))
            j++;
        if (j == n)
            return true;
    }
    return false;
}

std::string trimmed(const std::string &s)
{
    static const char *ws = " \t\r\n";
    const size_t first = s.find_first_not_of(ws);
    if (first == std::string::npos)
        return "";
    const size_t last = s.find_last_not_of(ws);
    return s.substr(first, last - first + 1);
}

/* The typed context window as a positive token count, or 0 when it is not one. strtoll rather
 * than std::stoi on purpose: this build passes -fno-exceptions (see the root BUILD), where an
 * escaping throw would abort instead of being caught. */
int parse_context_window(const std::string &text)
{
    const std::string t = trimmed(text);
    if (t.empty())
        return 0;
    errno = 0;
    char *end = nullptr;
    const long long v = strtoll(t.c_str(), &end, 10);
    if (errno != 0 || !end || *end != '\0' || v <= 0 || v > 1000000000LL)
        return 0;
    return static_cast<int>(v);
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

    /* A rejected value is only a complaint about what was there; the next keystroke is the
     * user answering it, so it clears the complaint (see CommitEndpoint / CommitContext). */
    ::ftxui::InputOption eopt = field_option();
    eopt.on_change = [this] { endpoint_bad_ = false; };
    eopt.cursor_position = &endpoint_cursor_;
    endpoint_input_ = ::ftxui::Input(&url_, "https://host/v1", eopt);

    ::ftxui::InputOption mfilter = field_option();
    mfilter.on_change = [this] { ApplyModelFilter(); };
    mfilter.cursor_position = &model_cursor_;
    model_filter_input_ = ::ftxui::Input(&model_filter_, "Search models...", mfilter);

    ::ftxui::InputOption copt = field_option();
    copt.on_change = [this] { context_bad_ = false; };
    copt.cursor_position = &context_cursor_;
    context_input_ = ::ftxui::Input(&context_text_, "tokens, e.g. 128000", copt);

    /* Adopt the inputs as a real component child: without a child the dialog is not
     * Focusable(), so its current input never reports Focused() and ftxui never asks the
     * terminal for a caret. Draw() still renders only the active step's input; this link
     * carries focus. The order IS the Step enum's (see the header): tab_index_ = step_. */
    tabs_ = ::ftxui::Container::Tab(
        {provider_filter_input_, endpoint_input_, key_input_, model_filter_input_, context_input_}, &tab_index_);
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

bool SetupDialog::WantsCustomRow() const
{
    if (looks_like_url(provider_filter_))
        return true; /* a pasted URL IS the custom endpoint; the row commits what was typed */
    /* "custom" / "other": how someone looks up what the catalog does not offer. Only a
     * non-empty filter can ask, or every list would grow a row nobody asked for. */
    if (!provider_filter_.empty() && (contains_ci(provider_filter_, "custom") || contains_ci(provider_filter_, "other")))
        return true;
    /* The config already points outside the catalog: its current choice needs a row. */
    return config_is_custom_;
}

void SetupDialog::ApplyProviderFilter()
{
    /* Which provider the config currently uses (by base URL, else by containing the
     * current model). No match means the config points at an endpoint the catalog does not
     * know -- a CUSTOM install -- which is what gives its current choice a row to be ticked
     * in (see WantsCustomRow). */
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
    config_is_custom_ = (match < 0 && (!current_api_.empty() || !current_model_.empty()));

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
    /* The synthetic row, last so a catalog list keeps its order. When it is there because a
     * URL was typed, the row repeats that URL: it is the value Enter is about to commit, and
     * naming it is what makes "use what I typed" a choice rather than a guess. */
    if (WantsCustomRow())
    {
        provider_rows_.push_back(looks_like_url(provider_filter_) ? "Use this endpoint: " + trimmed(provider_filter_)
                                                                 : "Custom endpoint (OpenAI-compatible)");
        provider_view_.push_back(kCustomRow);
    }
    if (provider_sel_ >= static_cast<int>(provider_view_.size()))
        provider_sel_ = provider_view_.empty() ? 0 : static_cast<int>(provider_view_.size()) - 1;

    provider_current_row_ = -1;
    for (size_t r = 0; r < provider_view_.size(); r++)
        if (static_cast<int>(provider_view_[r]) == match)
        {
            provider_current_row_ = static_cast<int>(r);
            break;
        }
    if (config_is_custom_)
        for (size_t r = 0; r < provider_view_.size(); r++)
            if (provider_view_[r] == kCustomRow)
            {
                provider_current_row_ = static_cast<int>(r);
                break;
            }
}

void SetupDialog::ReloadModels()
{
    model_ids_.clear();
    /* The custom row has no catalog id to load models for (the id is typed on that step). */
    if (facade_ && provider_sel_ >= 0 && static_cast<size_t>(provider_sel_) < provider_view_.size() &&
        provider_view_[provider_sel_] != kCustomRow)
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
    const size_t row = provider_view_[provider_sel_];
    if (row == kCustomRow)
    {
        /* Custom: the base URL is NOT taken from a provider -- there is none. A URL typed
         * into the search box becomes it; otherwise the seeded one stays, so an existing
         * custom install reopens with its own endpoint in the field instead of an empty box
         * that would overwrite the config on the next Enter. */
        custom_ = true;
        if (looks_like_url(provider_filter_))
            url_ = trimmed(provider_filter_);
        endpoint_bad_ = false;
        endpoint_cursor_ = static_cast<int>(url_.size());
        step_ = STEP_ENDPOINT;
        return;
    }

    custom_ = false;
    const std::string &pid = provider_ids_[row];
    std::string api = facade_ ? facade_->ProviderApi(pid) : "";
    if (!api.empty())
        url_ = api; /* the base URL is bound to the provider (never shown) */
    ReloadModels();
    step_ = STEP_KEY;
}

void SetupDialog::CommitEndpoint()
{
    if (!looks_like_url(url_))
    {
        endpoint_bad_ = true; /* stay: nothing was accepted, so nothing may move on */
        return;
    }
    endpoint_bad_ = false;
    step_ = STEP_KEY;
}

void SetupDialog::CommitKey()
{
    step_ = STEP_MODEL;
}

void SetupDialog::CommitModel()
{
    if (!facade_)
        return;
    if (custom_)
    {
        /* The catalog has nothing for a custom endpoint: the typed id IS the model, and the
         * window is asked for on the next step -- there is no catalog fact to take it from. */
        if (trimmed(model_filter_).empty())
            return;
        step_ = STEP_CONTEXT;
        return;
    }
    if (model_view_.empty())
        return;
    const std::string &mid = model_ids_[model_view_[model_sel_]];
    port::ModelFacts f;
    context_window_ = facade_->ModelInfo(mid, &f) ? f.context : 0;
    Finish(mid);
}

void SetupDialog::CommitContext()
{
    const int window = parse_context_window(context_text_);
    if (window <= 0)
    {
        context_bad_ = true; /* stay: an unparsable window is not a window */
        return;
    }
    context_bad_ = false;
    context_window_ = window;
    Finish(trimmed(model_filter_));
}

void SetupDialog::Finish(const std::string &model_id)
{
    if (!facade_)
        return;
    port::SetupConfig cfg;
    cfg.api_url = url_;
    cfg.model = model_id;
    cfg.api_key = key_;
    cfg.context_window = context_window_;
    if (facade_->SetSetup(cfg))
    {
        if (on_done_)
            on_done_(); /* host refreshes the footer's model */
        Close();
    }
}

int SetupDialog::StepOrdinal() const
{
    if (custom_)
        return static_cast<int>(step_) + 1; /* STEP_PROVIDER is 1 for both paths */
    return step_ == STEP_PROVIDER ? 1 : step_ == STEP_KEY ? 2 : 3;
}

int SetupDialog::StepTotal() const
{
    /* Before a provider is committed the path is unknown; the catalog's three is what the
     * user is walking until they pick custom, and the denominator corrects itself then. */
    return custom_ ? 5 : 3;
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
    custom_ = false; /* the path is chosen again on every open */
    endpoint_bad_ = false;
    context_bad_ = false;
    step_ = STEP_PROVIDER;
    /* Also decides config_is_custom_ -- whether the config already points outside the
     * catalog, which is the one case where its context window is a real choice of the
     * user's rather than the default model family's 1 MiB (edc_init_env repairs a missing
     * key to that, and pre-filling 1 MiB for an unknown endpoint is exactly how a window
     * larger than the provider's real one gets saved). */
    ReloadProviders();
    const int configured_window = facade_ ? facade_->ContextWindow() : 0;
    const int offered_window =
        (config_is_custom_ && configured_window > 0) ? configured_window : kDefaultCustomContextWindow;
    context_text_ = std::to_string(offered_window);
    /* Park each caret at the END of its value: the user is editing an existing value and
     * the next keystroke should append (the API key is pre-filled from the config). */
    provider_cursor_ = static_cast<int>(provider_filter_.size());
    endpoint_cursor_ = static_cast<int>(url_.size());
    key_cursor_ = static_cast<int>(key_.size());
    model_cursor_ = static_cast<int>(model_filter_.size());
    context_cursor_ = static_cast<int>(context_text_.size());
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

    case STEP_ENDPOINT:
        if (event == ::ftxui::Event::Return)
        {
            CommitEndpoint();
            return true;
        }
        return endpoint_input_->OnEvent(event);

    case STEP_KEY:
        if (event == ::ftxui::Event::Return)
        {
            CommitKey();
            return true;
        }
        return key_input_->OnEvent(event);

    case STEP_MODEL:
        /* The custom model step is a plain field, so there is no list to move in (and
         * model_view_ is empty, so the arrows below would be no-ops anyway). */
        if (event == ::ftxui::Event::ArrowUp && !custom_)
            return model_sel_ > 0 ? (model_sel_--, true) : true;
        if (event == ::ftxui::Event::ArrowDown && !custom_)
            return model_sel_ + 1 < static_cast<int>(model_view_.size()) ? (model_sel_++, true) : true;
        if (event == ::ftxui::Event::Return)
        {
            CommitModel();
            return true;
        }
        return model_filter_input_->OnEvent(event);

    case STEP_CONTEXT:
        if (event == ::ftxui::Event::Return)
        {
            CommitContext();
            return true;
        }
        return context_input_->OnEvent(event);
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
     * only one that draws a caret). The Tab was built in Step order, so this is the index. */
    tab_index_ = static_cast<int>(step_);

    /* A 2-column left gutter, matching the list's "✓ "/"  " prefix, so the title, the
     * inputs and the list rows all share one text margin. */
    auto field = [&](const Component &input) { return hbox({text("  "), input->Render() | flex}); };
    /* A single-field step that had its value refused says why, under the field. */
    auto complaint = [&](bool bad, const char *why) {
        return bad ? text("  " + std::string(why)) | color(pal.text_dim) : text("");
    };

    Element body;
    if (step_ == STEP_PROVIDER)
    {
        body = vbox({
            field(provider_filter_input_), /* the search box itself: its placeholder reads as the prompt */
            separator() | color(pal.line_dim) | bgcolor(pal.dialog_bg),
            ListElement(provider_rows_, provider_sel_, provider_current_row_, kListRows),
        });
    }
    else if (step_ == STEP_ENDPOINT)
    {
        body = vbox({
            field(endpoint_input_),
            complaint(endpoint_bad_, "the endpoint must start with http:// or https://"),
        });
    }
    else if (step_ == STEP_KEY)
    {
        /* Just the field: the base URL is bound to the provider, and a nav hint would be
         * noise on a single-field step. */
        body = field(key_input_);
    }
    else if (step_ == STEP_MODEL && custom_)
    {
        /* No catalog to pick from: the id is typed, as the endpoint expects it. */
        body = vbox({
            field(model_filter_input_),
            separator() | color(pal.line_dim) | bgcolor(pal.dialog_bg),
            text("  type the model id the endpoint answers to") | color(pal.text_dim),
        });
    }
    else if (step_ == STEP_MODEL)
    {
        body = vbox({
            field(model_filter_input_),
            separator() | color(pal.line_dim) | bgcolor(pal.dialog_bg),
            ListElement(model_rows_, model_sel_, model_current_row_, kListRows),
        });
    }
    else
    {
        body = vbox({
            field(context_input_),
            complaint(context_bad_, "the context window must be a positive number of tokens"),
        });
    }

    const char *title = step_ == STEP_PROVIDER   ? "Choose a provider"
                        : step_ == STEP_ENDPOINT ? "API endpoint"
                        : step_ == STEP_KEY      ? "API key"
                        : step_ == STEP_CONTEXT  ? "Context window"
                        : custom_                ? "Model id"
                                                 : "Choose a model";
    std::string step_no = "Step " + std::to_string(StepOrdinal()) + "/" + std::to_string(StepTotal());

    // The header IS the step's name (not a generic "Setup"); flat and borderless. A
    // right-aligned footer shows the keys that apply here
    // (the list steps move the cursor; the field steps submit).
    const bool list_step = step_ == STEP_PROVIDER || (step_ == STEP_MODEL && !custom_);
    const char *hint = list_step ? "Up/Down move  |  Enter select  |  Esc exit" : "Enter submit  |  Esc exit";

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
