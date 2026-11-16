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

/* The synthetic row's label. |filter|, when it is a URL, is the endpoint the row would
 * commit -- say which one, since the search box above it is already full of it. */
std::string custom_row_label(const std::string &filter)
{
    if (looks_like_url(filter))
        return "Use this endpoint: " + trimmed(filter);
    return "Custom endpoint (OpenAI-compatible)";
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
    key_input_ = ::ftxui::Input(&key_, ::ftxui::StringRef(&key_placeholder_), keyopt);

    /* A rejected value is only a complaint about what was there; the next keystroke is the
     * user answering it, so it clears the complaint (see CommitEndpoint / CommitContext). */
    ::ftxui::InputOption eopt = field_option();
    eopt.on_change = [this] { endpoint_bad_ = false; };
    eopt.cursor_position = &endpoint_cursor_;
    endpoint_input_ = ::ftxui::Input(&endpoint_, "https://host/v1", eopt);

    ::ftxui::InputOption mfilter = field_option();
    mfilter.on_change = [this] { ApplyModelFilter(); };
    mfilter.cursor_position = &model_cursor_;
    /* The placeholder references the member, so the wording can follow the path (see
     * model_placeholder_). */
    model_filter_input_ = ::ftxui::Input(&model_filter_, ::ftxui::StringRef(&model_placeholder_), mfilter);

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
    /* Start the cursor on the config's current choice. With nothing configured there is no
     * ticked row, and the cursor must NOT default to the pinned custom row at the bottom --
     * Enter would then walk the custom path for a user who only pressed Enter. The first
     * CATALOG row is the natural default; the custom row is one Up away, or 0 when the
     * catalog is empty (it is then the only row there is). */
    if (provider_current_row_ >= 0)
        provider_sel_ = provider_current_row_;
    else if (!provider_view_.empty())
        provider_sel_ = 0;
}

void SetupDialog::ApplyProviderFilter()
{
    /* Which provider the config currently uses (by base URL, else by containing the
     * current model). No match means the config points at an endpoint the catalog does not
     * know -- a CUSTOM install -- whose current choice is then ticked on the custom row. */
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
    /* The custom row is ALWAYS the last row: it is the answer to "my provider is not in this
     * list", and a row that only appears once you already know the magic word is not
     * discoverable. It is unaffected by the filter -- a filter narrows the catalog, not the
     * ways out of it. */
    const size_t catalog_rows = provider_view_.size();
    provider_rows_.push_back(custom_row_label(provider_filter_));
    provider_view_.push_back(kCustomRow);

    if (provider_sel_ >= static_cast<int>(provider_view_.size()))
    {
        /* The list shrank under the cursor. Land on the last CATALOG row when one survived:
         * the custom row is not what a filter for a catalog provider is looking for, so a
         * narrowed list must not silently move the cursor onto it (Enter would then commit
         * custom). With no catalog row left, the custom row is the only choice there is. */
        provider_sel_ = catalog_rows > 0 ? static_cast<int>(catalog_rows) - 1
                                         : static_cast<int>(provider_view_.size()) - 1;
    }

    provider_current_row_ = -1;
    /* |match| is a catalog index, and -1 means "none". Comparing it against the view must
     * therefore skip the custom row: kCustomRow is SIZE_MAX, whose int cast is -1, so an
     * unguarded comparison would make "no provider matches" tick the custom row -- the exact
     * opposite of what -1 means. */
    if (match >= 0)
        for (size_t r = 0; r < provider_view_.size(); r++)
            if (provider_view_[r] != kCustomRow && static_cast<int>(provider_view_[r]) == match)
            {
                provider_current_row_ = static_cast<int>(r);
                break;
            }
    if (config_is_custom_)
        provider_current_row_ = static_cast<int>(provider_view_.size()) - 1;
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
        /* A URL typed into the search box IS the endpoint; otherwise the step starts empty
         * unless the config already points at one (see endpoint_ in the header). */
        if (looks_like_url(provider_filter_))
            endpoint_ = trimmed(provider_filter_);
        else
            endpoint_ = config_is_custom_ ? url_ : "";
        endpoint_bad_ = false;
        endpoint_cursor_ = static_cast<int>(endpoint_.size());
        /* Nothing to search on this path: the text IS the id. */
        model_placeholder_ = "Model id";
        step_ = STEP_ENDPOINT;
        return;
    }

    custom_ = false;
    model_placeholder_ = "Search models...";
    const std::string &pid = provider_ids_[row];
    std::string api = facade_ ? facade_->ProviderApi(pid) : "";
    if (!api.empty())
        url_ = api; /* the base URL is bound to the provider (never shown) */
    ReloadModels();
    step_ = STEP_KEY;
}

void SetupDialog::CommitEndpoint()
{
    const std::string typed = trimmed(endpoint_);
    if (!looks_like_url(typed))
    {
        endpoint_bad_ = true; /* stay: nothing was accepted, so nothing may move on */
        return;
    }
    endpoint_bad_ = false;
    url_ = typed; /* the endpoint step is where the URL is decided, for both paths */
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
    /* The key starts EMPTY, always. A stored key is a secret: seeding the field with it prints
     * it on screen for anyone looking at the terminal (and in a screen share, a recording, a
     * scrollback). The wizard only needs to know that one exists, which is what
     * has_stored_key_ is for -- and an empty submission already means "keep it", because both
     * writers skip an empty key. */
    key_.clear();
    has_stored_key_ = initial.has_stored_key;
    key_placeholder_ = has_stored_key_ ? "leave empty to keep the stored key" : "API key";
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
    endpoint_ = ""; /* see endpoint_: the step is seeded when the path is chosen (CommitProvider) */
    /* Park each caret at the END of its value: the user is editing an existing value and the
     * next keystroke should append. (The key field is empty by design -- see Open -- so its
     * caret is simply at 0.) */
    provider_cursor_ = static_cast<int>(provider_filter_.size());
    endpoint_cursor_ = static_cast<int>(endpoint_.size());
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

::ftxui::Element SetupDialog::RowElement(const std::string &label, bool selected, bool ticked) const
{
    using namespace ::ftxui;
    const auto &pal = style::palette();
    auto row = text((ticked ? "\u2713 " : "  ") + label);
    /* The cursor is a background fill (not a font colour); the tick marks the config's
     * current choice. */
    return selected ? (row | bold | bgcolor(pal.accent) | color(pal.dialog_bg)) : (row | color(pal.text));
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

    /* Only the rows that exist (at most max_rows). */
    Elements lines;
    for (int i = first; i < n && i < first + max_rows; i++)
        lines.push_back(RowElement(rows[i], i == sel, i == current_row));
    return vbox(std::move(lines));
}

::ftxui::Element SetupDialog::ProviderListElement() const
{
    using namespace ::ftxui;
    const auto &pal = style::palette();

    /* The custom row is the last entry of provider_view_ / provider_rows_ (see
     * ApplyProviderFilter), so the catalog part is everything before it. */
    const int catalog_n = static_cast<int>(provider_rows_.size()) - 1;
    const int window = kListRows - 1; /* one line is the pinned custom row */

    int first = 0;
    if (provider_sel_ == catalog_n)
    {
        /* The cursor is on the pinned row: show the tail of the catalog above it. */
        first = catalog_n > window ? catalog_n - window : 0;
    }
    else if (catalog_n > window)
    {
        first = provider_sel_ - window / 2;
        if (first < 0)
            first = 0;
        if (first > catalog_n - window)
            first = catalog_n - window;
    }

    Elements lines;
    for (int i = first; i < catalog_n && i < first + window; i++)
        lines.push_back(RowElement(provider_rows_[i], i == provider_sel_, i == provider_current_row_));

    /* The pinned row, always drawn: the catalog can never push it out of view. A separator
     * above it says it is not part of the list above (it is not a provider). */
    const bool custom_selected = (provider_sel_ == catalog_n);
    const bool custom_ticked = (provider_current_row_ == catalog_n);
    return vbox({
        vbox(std::move(lines)),
        separator() | color(pal.line_dim) | bgcolor(pal.dialog_bg),
        RowElement(provider_rows_[catalog_n], custom_selected, custom_ticked),
    });
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
            ProviderListElement(),
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
        /* Just the field, plus -- when a key is already stored -- one line saying how an empty
         * field will be read. The line is the point: the field is empty on purpose, and without
         * it "empty" looks like "the key was lost". */
        if (has_stored_key_)
            body = vbox({
                field(key_input_),
                text("  a key is already stored; leave this empty to keep it") | color(pal.text_dim),
            });
        else
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
