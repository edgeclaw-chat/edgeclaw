/* test_setup_dialog: the first-run wizard, driven through its public surface.
 *
 * Why this file exists: `SetupDialog` is the page every new user meets before they can type
 * anything, and it had 8.8% line coverage with 19 of its 21 functions never executed. The
 * only thing standing in the way was its single construction dependency -- `port::IAppFacade`
 * -- so the fixture here is a fake facade that answers from an in-memory catalog. The dialog
 * owns only step state, rendering and events (see its header), which is exactly what these
 * cases pin.
 *
 * The three steps are driven the way a user drives them: `Open(seed)`, then events. Rendering
 * is read back through ftxui's Screen (the element tree is not inspectable, the drawn cells
 * are), with SGR escapes stripped so assertions are about what a reader sees.
 *
 * Cases are chosen to pin DECISIONS, not to reach lines:
 *
 *   - the config's current provider and model are TICKED, which is distinct from the
 *     keyboard cursor -- and the cursor is a background fill that plain text cannot show, so
 *     it is asserted through what Enter ends up selecting instead;
 *   - the provider step falls back to "which provider lists the configured model" when no
 *     provider's base URL matches, because a config may hold an endpoint or a bare base;
 *   - a rejected `SetSetup` leaves the wizard OPEN with the user's input intact -- the app
 *     refused to persist it, so silently closing would look like success;
 *   - the model list hides non-chat models: offering an embedding model here produces a
 *     config that cannot answer anything;
 *   - a long list scrolls and clamps at both ends, and a filter that shrinks the list clamps
 *     the cursor rather than leaving it pointing past the end.
 */

#include "edc_setup_dialog.hpp"
#include "edc_style.hpp"

#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>

#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>

static int g_passed = 0;
static int g_failed = 0;

#define TEST(name) printf("\n-- %s\n", name)

#define CHECK(cond, ...) do { \
    if (!(cond)) { fprintf(stderr, "    FAIL: "); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); g_failed++; } \
    else { printf("    ok: "); printf(__VA_ARGS__); printf("\n"); g_passed++; } \
} while (0)

namespace
{

// ── Fake catalog: everything the wizard reads, and nothing else ──────────────

struct FakeModel
{
    std::string id;
    std::string name;
    int context = 0;
    bool chat = true;
};

class FakeFacade : public edc::port::IAppFacade
{
  public:
    // The catalog, in the order the provider list should receive it.
    std::vector<std::string> provider_order;
    std::map<std::string, std::string> provider_name;
    std::map<std::string, std::string> provider_api;
    std::map<std::string, std::vector<FakeModel>> provider_models;

    // The config the wizard seeds from (only the three reads SetupDialog makes).
    std::string config_model;
    std::string config_api_url;
    std::string config_api_key;
    // The configured context window (SetupDialog offers it back for a custom endpoint).
    int config_context_window = 0;

    // Outcomes, and what the dialog asked for.
    bool set_setup_ok = true;
    int set_setup_calls = 0;
    int mark_onboarded_calls = 0;
    edc::port::SetupConfig last_setup;

    bool Ready() const override
    {
        return true;
    }

    bool ConfigModel(std::string *out) const override
    {
        if (config_model.empty())
            return false;
        *out = config_model;
        return true;
    }

    bool ConfigApiUrl(std::string *out) const override
    {
        if (config_api_url.empty())
            return false;
        *out = config_api_url;
        return true;
    }

    bool ConfigApiKey(std::string *out) const override
    {
        if (config_api_key.empty())
            return false;
        *out = config_api_key;
        return true;
    }

    bool SandboxEnabled() const override
    {
        return true;
    }
    bool ShellSyntax() const override
    {
        return false;
    }
    edc_input_mode InputMode(const std::string &) const override
    {
        return EDC_INPUT_TEXT;
    }
    int ContextWindow() const override
    {
        return config_context_window;
    }

    bool Onboarded() const override
    {
        return false;
    }

    bool SetSetup(const edc::port::SetupConfig &config) override
    {
        set_setup_calls++;
        last_setup = config;
        return set_setup_ok;
    }

    bool MarkOnboarded() override
    {
        mark_onboarded_calls++;
        return true;
    }

    std::string TestEndpoint(const std::string &, const std::string &) const override
    {
        return "";
    }

    size_t Providers(std::vector<std::string> *ids) const override
    {
        if (ids)
            *ids = provider_order;
        return provider_order.size();
    }

    std::string ProviderName(const std::string &provider_id) const override
    {
        auto it = provider_name.find(provider_id);
        return it == provider_name.end() ? std::string() : it->second;
    }

    std::string ProviderApi(const std::string &provider_id) const override
    {
        auto it = provider_api.find(provider_id);
        return it == provider_api.end() ? std::string() : it->second;
    }

    std::string ProviderEnvName(const std::string &, size_t) const override
    {
        return "";
    }

    size_t Models(const std::string &provider_id, std::vector<std::string> *ids) const override
    {
        auto it = provider_models.find(provider_id);
        if (ids)
            ids->clear();
        if (it == provider_models.end())
            return 0;
        for (const FakeModel &m : it->second)
            if (ids)
                ids->push_back(m.id);
        return it->second.size();
    }

    bool ModelInfo(const std::string &model_id, edc::port::ModelFacts *out) const override
    {
        for (const auto &entry : provider_models)
            for (const FakeModel &m : entry.second)
                if (m.id == model_id)
                {
                    if (out)
                    {
                        out->name = m.name;
                        out->context = m.context;
                        out->output = m.context / 4;
                        out->chat = m.chat;
                    }
                    return true;
                }
        return false;
    }

    // ── everything below is outside the wizard's surface ────────────────────

    std::vector<edc::model::CommandFact> Commands() const override
    {
        return {};
    }
    std::vector<edc::model::CommandFact> CommandFamilies() const override
    {
        return {};
    }
    std::vector<edc::model::CommandFact> ShellOptions() const override
    {
        return {};
    }
    std::vector<edc::model::CommandFact> PromptNames(const std::string &) const override
    {
        return {};
    }
    std::vector<edc::model::CommandFact> SkillNames(const std::string &) const override
    {
        return {};
    }
    std::vector<edc::model::CommandFact> FileCandidates(const std::string &,
                                                        const std::string &) const override
    {
        return {};
    }
    bool MentionExists(const std::string &, const std::string &) const override
    {
        return false;
    }
    void ResetFileCandidates() const override
    {
    }
    void SendUserMessage(const std::string &, const std::string &, const std::string &,
                         const std::string &) override
    {
    }
    void RequestTaskSnapshot(const std::string &) override
    {
    }
    void RequestMessagePayload(const std::string &) override
    {
    }
    bool HistoryPath(std::string *) const override
    {
        return false;
    }
    bool ChatLogPathFor(const std::string &, std::string *) const override
    {
        return false;
    }
    bool WriteFile(const std::string &, const char *, size_t, bool, bool) override
    {
        return false;
    }
    bool EnsureParentDir(const std::string &) override
    {
        return false;
    }
    void HandleChannelMessage(struct edc_channel_message *) override
    {
    }

    /* The update port. SetupDialog never asks, but the interface is one piece -- a fake that
     * implements only the methods its subject calls would stop compiling the moment the
     * subject grew a call, and the failure would look unrelated. */
    void SetUpdateObserver(std::function<void(std::string)>) override
    {
    }
    bool UpdateCanSelfUpgrade() const override
    {
        return false;
    }
    bool UpdateStart(const std::string &, std::function<void(bool, std::string)>) override
    {
        return false;
    }
    void UpdateCancel() override
    {
    }
    std::string UpdateManualHint() const override
    {
        return "";
    }
};

void AddProvider(FakeFacade &f, const std::string &id, const std::string &name, const std::string &api)
{
    f.provider_order.push_back(id);
    f.provider_name[id] = name;
    f.provider_api[id] = api;
}

void AddModel(FakeFacade &f, const std::string &provider_id, const std::string &id, const std::string &name,
              int context, bool chat = true)
{
    f.provider_models[provider_id].push_back(FakeModel{id, name, context, chat});
}

// Drop the standard two-provider catalog: the scroll cases need a list whose length is the
// whole story, and the fixture's own providers would shift every row index.
//
// The seeded config goes with it. A config whose URL and model match no provider is a CUSTOM
// install (see WantsCustomRow), and the provider step then grows the synthetic custom row --
// which would move the row indices these cases are about. A case that wants that row seeds a
// config that belongs to its own catalog, or none at all.
void ClearCatalog(FakeFacade &f)
{
    f.provider_order.clear();
    f.provider_name.clear();
    f.provider_api.clear();
    f.provider_models.clear();
    f.config_api_url.clear();
    f.config_model.clear();
}

// ── Reading a frame ─────────────────────────────────────────────────────────

// Strip the SGR escapes Screen::ToString() interleaves with the cells, so assertions are
// about what a reader sees rather than about colour codes.
std::string Plain(const std::string &frame)
{
    std::string out;
    bool in_escape = false;
    for (char c : frame)
    {
        if (in_escape)
        {
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))
                in_escape = false;
            continue;
        }
        if (c == '\x1b')
        {
            in_escape = true;
            continue;
        }
        out += c;
    }
    return out;
}

std::string Frame(edc::ui::SetupDialog &dialog, int w = 80, int h = 26)
{
    auto screen =
        ::ftxui::Screen::Create(::ftxui::Dimension::Fixed(w), ::ftxui::Dimension::Fixed(h));
    ::ftxui::Render(screen, dialog.Render());
    return Plain(screen.ToString());
}

bool Has(const std::string &frame, const std::string &needle)
{
    return frame.find(needle) != std::string::npos;
}

// The line a needle appears on, or "" when it is not drawn. Needed because the tick is a
// prefix: "✓ Anthropic" and "Anthropic" cannot be told apart by a bare `find`.
std::string LineOf(const std::string &frame, const std::string &needle)
{
    size_t start = 0;
    while (start <= frame.size())
    {
        size_t end = frame.find('\n', start);
        const std::string line =
            frame.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (line.find(needle) != std::string::npos)
            return line;
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    return "";
}

// A row carries the config's current choice when ListElement prefixed it with U+2713.
bool Ticked(const std::string &frame, const std::string &needle)
{
    const std::string line = LineOf(frame, needle);
    return !line.empty() && line.find("\u2713") != std::string::npos;
}

// ── Driving the wizard ──────────────────────────────────────────────────────

void Text(edc::ui::SetupDialog &dialog, const std::string &text)
{
    for (char c : text)
        dialog.OnEvent(::ftxui::Event::Character(c));
}

void Down(edc::ui::SetupDialog &dialog, int times = 1)
{
    for (int i = 0; i < times; i++)
        dialog.OnEvent(::ftxui::Event::ArrowDown);
}

void Up(edc::ui::SetupDialog &dialog, int times = 1)
{
    for (int i = 0; i < times; i++)
        dialog.OnEvent(::ftxui::Event::ArrowUp);
}

bool Enter(edc::ui::SetupDialog &dialog)
{
    return dialog.OnEvent(::ftxui::Event::Return);
}

// A two-provider catalog with one chat model each, plus a seeded config that points at the
// first one through a full endpoint URL (the shape a real config.json holds).
struct Fixture
{
    FakeFacade facade;
    std::unique_ptr<edc::ui::SetupDialog> dialog;
    int done_calls = 0;

    Fixture()
    {
        AddProvider(facade, "anthropic", "Anthropic", "https://api.anthropic.com/v1");
        AddModel(facade, "anthropic", "claude-sonnet", "Claude Sonnet", 200000);
        AddProvider(facade, "openai", "OpenAI", "https://api.openai.com/v1");
        AddModel(facade, "openai", "gpt-4o", "GPT-4o", 128000);

        facade.config_api_url = "https://api.anthropic.com/v1/chat/completions";
        facade.config_model = "claude-sonnet";
        facade.config_api_key = "";

        dialog = std::make_unique<edc::ui::SetupDialog>(&facade, [this] { done_calls++; });
    }

    edc::port::SetupConfig Seed() const
    {
        edc::port::SetupConfig seed;
        seed.api_url = facade.config_api_url;
        seed.model = facade.config_model;
        seed.api_key = facade.config_api_key;
        return seed;
    }

    void Open()
    {
        dialog->Open(Seed());
    }

    // Provider step -> key step -> model step, selecting the first row of each list.
    void WalkToModelStep()
    {
        Open();
        Enter(*dialog); // commit provider
        Enter(*dialog); // commit key
    }
};

// ── Step 1: choose a provider ───────────────────────────────────────────────

static void test_open_lands_on_step_1_and_ticks_the_configured_provider()
{
    TEST("open: step 1 lists providers and ticks the one the config already uses");
    Fixture f;
    f.Open();
    const std::string frame = Frame(*f.dialog);

    CHECK(f.dialog->IsOpen(), "the dialog is open after Open()");
    CHECK(Has(frame, "Choose a provider"), "the header names the step");
    CHECK(Has(frame, "Step 1/3"), "the header counts the step");
    CHECK(Has(frame, "Anthropic  (anthropic)"), "a provider row shows the display name and the id");
    CHECK(Has(frame, "OpenAI  (openai)"), "the second provider is listed too");
    CHECK(Ticked(frame, "Anthropic"), "the configured provider carries the tick");
    CHECK(!Ticked(frame, "OpenAI"), "the other provider does not");
    CHECK(Has(frame, "Up/Down move  |  Enter select  |  Esc exit"), "the footer names the keys");
}

static void test_provider_filter_matches_both_the_name_and_the_id()
{
    TEST("the provider filter matches the display name and the id");
    /* The assertions name the full row ("name  (id)"), not just "OpenAI": the custom row's
     * label carries "OpenAI-compatible", so a bare substring cannot tell the two apart. */
    {
        Fixture f;
        f.Open();
        Text(*f.dialog, "anth");
        const std::string frame = Frame(*f.dialog);
        CHECK(Has(frame, "Anthropic  (anthropic)"), "typing part of the NAME keeps that provider");
        CHECK(!Has(frame, "OpenAI  (openai)"), "and drops the ones that do not match");
    }
    {
        Fixture f;
        f.Open();
        Text(*f.dialog, "opena"); // matches the id, not the display name "OpenAI"
        const std::string frame = Frame(*f.dialog);
        CHECK(Has(frame, "OpenAI  (openai)"), "typing part of the ID keeps that provider");
        CHECK(!Has(frame, "Anthropic  (anthropic)"), "and drops the rest");
    }
}

static void test_a_provider_filter_with_no_match_leaves_only_the_custom_row()
{
    TEST("no catalog provider matches: the custom row is all that is left, and Enter takes it");
    Fixture f;
    f.Open();
    Text(*f.dialog, "zzzz");

    const std::string filtered = Frame(*f.dialog);
    CHECK(!Has(filtered, "Anthropic  (anthropic)"), "no catalog provider survives the filter");
    /* The list is never empty, so it never needs "(no match)": the way out of a list that
     * does not contain your provider is always on offer. */
    CHECK(!Has(filtered, "(no match)"), "the provider list never reads as empty");
    CHECK(Has(filtered, "Custom endpoint (OpenAI-compatible)"), "the custom row is still there");

    CHECK(Enter(*f.dialog), "Enter is consumed by the step");
    CHECK(Has(Frame(*f.dialog), "API endpoint"), "and it takes the custom row");
    CHECK(Has(Frame(*f.dialog), "Step 2/5"), "which is the longer path");
}

static void test_the_cursor_moves_and_clamps_at_both_ends()
{
    TEST("the provider cursor moves, and clamps at both ends");
    Fixture f;
    f.Open();

    Up(*f.dialog, 3); // already at row 0: stays
    Enter(*f.dialog); // commit -> anthropic
    Enter(*f.dialog); // key step
    CHECK(Has(Frame(*f.dialog), "claude-sonnet"), "Up past the top keeps the first provider");

    Fixture g;
    g.Open();
    Down(*g.dialog, 1); // -> openai
    Enter(*g.dialog);
    Enter(*g.dialog);
    const std::string frame = Frame(*g.dialog);
    CHECK(Has(frame, "gpt-4o"), "Down selects the second provider");
    CHECK(!Has(frame, "claude-sonnet"), "and the first provider's models are gone");
    CHECK(Has(frame, "Choose a model"), "which means we reached step 3");

    /* The bottom of the list IS the custom row, so that is where an overshoot lands. */
    Fixture h;
    h.Open();
    Down(*h.dialog, 20); // far past the end: clamps
    const std::string last = Frame(*h.dialog);
    Enter(*h.dialog);
    CHECK(Has(Frame(*h.dialog), "API endpoint"), "Down past the bottom clamps at the custom row");
    (void)last;
}

static void test_enter_on_the_provider_step_moves_to_the_key_step()
{
    TEST("Enter on the provider step loads its models and moves to the key step");
    Fixture f;
    f.Open();
    CHECK(Enter(*f.dialog), "Enter is consumed");

    const std::string frame = Frame(*f.dialog);
    CHECK(Has(frame, "API key"), "the header names the key step");
    CHECK(Has(frame, "Step 2/3"), "the step counter advanced");
    CHECK(Has(frame, "Enter submit  |  Esc exit"), "the footer switches to the single-field hint");
    CHECK(!Has(frame, "Choose a provider"), "the provider page is gone");
}

// ── Step 2: the API key ─────────────────────────────────────────────────────

static void test_the_key_step_types_where_the_caret_was_left()
{
    TEST("the key field appends to the seeded value instead of overwriting it");
    Fixture f;
    f.facade.config_api_key = "sk-old";
    f.Open();
    Enter(*f.dialog); // -> key step, caret parked at the end of "sk-old"

    Text(*f.dialog, "X");
    Enter(*f.dialog); // -> model step
    Enter(*f.dialog); // finish

    CHECK(f.facade.set_setup_calls == 1, "the wizard finished");
    CHECK(f.facade.last_setup.api_key == "sk-oldX", "one keystroke appends: got '%s'",
          f.facade.last_setup.api_key.c_str());
}

// ── Step 3: choose a model ──────────────────────────────────────────────────

static void test_the_model_list_hides_models_that_are_not_chat_capable()
{
    TEST("the model list only offers chat-capable models");
    Fixture f;
    AddModel(f.facade, "anthropic", "voyage-embed", "Voyage Embed", 8192, /*chat=*/false);
    f.Open();
    Enter(*f.dialog);
    Enter(*f.dialog);

    const std::string frame = Frame(*f.dialog);
    CHECK(Has(frame, "claude-sonnet"), "the chat model is listed");
    CHECK(!Has(frame, "voyage-embed"), "the embedding model is not: it cannot answer a prompt");
}

static void test_a_model_row_shows_the_display_name_and_the_wire_id()
{
    TEST("a model row shows name and id, and an id-only entry shows just the id");
    Fixture f;
    AddModel(f.facade, "anthropic", "claude-haiku", "claude-haiku", 200000); // name == id
    f.Open();
    Enter(*f.dialog);
    Enter(*f.dialog);

    const std::string frame = Frame(*f.dialog);
    CHECK(Has(frame, "Claude Sonnet  (claude-sonnet)"), "a renamed model shows both");
    CHECK(Has(frame, "claude-haiku"), "an unnamed model shows its id");
    CHECK(!Has(frame, "claude-haiku  (claude-haiku)"), "and is not printed twice");
}

static void test_a_model_filter_narrows_by_id_and_by_name()
{
    TEST("the model filter matches the id and the display name");
    Fixture f;
    f.Open();
    Enter(*f.dialog);
    Enter(*f.dialog);

    Text(*f.dialog, "sonnet");
    const std::string by_id = Frame(*f.dialog);
    CHECK(Has(by_id, "claude-sonnet"), "typing part of the id keeps the model");

    {
        Fixture g;
        AddModel(g.facade, "anthropic", "claude-haiku", "Claude Haiku", 200000);
        g.Open();
        Enter(*g.dialog);
        Enter(*g.dialog);
        Text(*g.dialog, "Haiku");
        const std::string by_name = Frame(*g.dialog);
        CHECK(Has(by_name, "claude-haiku"), "typing part of the display name keeps the model");
        CHECK(!Has(by_name, "Claude Sonnet"), "and drops the rest");
    }
}

static void test_finishing_persists_the_whole_setup_and_closes()
{
    TEST("finishing persists provider, key, model and the context window, then closes");
    Fixture f;
    f.facade.config_api_key = "sk-seed";
    f.Open();
    Enter(*f.dialog); // provider: anthropic
    Enter(*f.dialog); // key
    Enter(*f.dialog); // model: claude-sonnet

    CHECK(!f.dialog->IsOpen(), "the dialog closed");
    CHECK(f.facade.set_setup_calls == 1, "SetSetup ran exactly once");
    CHECK(f.facade.last_setup.api_url == "https://api.anthropic.com/v1",
          "the provider's base URL is what gets persisted: got '%s'", f.facade.last_setup.api_url.c_str());
    CHECK(f.facade.last_setup.model == "claude-sonnet", "the chosen model: got '%s'",
          f.facade.last_setup.model.c_str());
    CHECK(f.facade.last_setup.api_key == "sk-seed", "the key as typed: got '%s'",
          f.facade.last_setup.api_key.c_str());
    CHECK(f.facade.last_setup.context_window == 200000,
          "the catalog's context window travels with it: got %d", f.facade.last_setup.context_window);
    CHECK(f.done_calls == 1, "the host was told to refresh (the footer's model changed)");
}

static void test_a_rejected_setup_leaves_the_wizard_open()
{
    TEST("a rejected SetSetup leaves the wizard open and does not notify the host");
    Fixture f;
    f.facade.set_setup_ok = false;
    f.Open();
    Enter(*f.dialog);
    Enter(*f.dialog);
    Enter(*f.dialog);

    CHECK(f.facade.set_setup_calls == 1, "the attempt was made");
    CHECK(f.dialog->IsOpen(), "the wizard stays open: the config was not written");
    CHECK(f.done_calls == 0, "the host is not told a model changed");
    CHECK(Has(Frame(*f.dialog), "Choose a model"), "and the user is still on the model step");
}

static void test_enter_with_no_model_does_not_finish()
{
    TEST("Enter with no selectable model does not finish");
    Fixture f;
    AddProvider(f.facade, "local", "Local", "http://127.0.0.1:8080/v1");
    AddModel(f.facade, "local", "only-embed", "Only Embed", 8192, /*chat=*/false);
    f.Open();
    Text(*f.dialog, "local"); // filter down to the provider with no chat model
    Enter(*f.dialog);
    Enter(*f.dialog);

    const std::string frame = Frame(*f.dialog);
    CHECK(Has(frame, "(no match)"), "the model list is empty and says so");
    CHECK(Enter(*f.dialog), "Enter is consumed");
    CHECK(f.facade.set_setup_calls == 0, "nothing was persisted");
    CHECK(f.dialog->IsOpen(), "and the wizard is still open");
}

static void test_provider_without_an_api_url_keeps_the_seeded_one()
{
    TEST("a provider with no base URL leaves the seeded URL alone");
    Fixture f;
    AddProvider(f.facade, "custom", "Custom", ""); // no base URL known
    AddModel(f.facade, "custom", "custom-model", "Custom Model", 32000);
    f.Open();
    Down(*f.dialog, 2); // -> custom
    Enter(*f.dialog);
    Enter(*f.dialog);
    Enter(*f.dialog);

    CHECK(f.facade.set_setup_calls == 1, "the wizard finished");
    CHECK(f.facade.last_setup.api_url == "https://api.anthropic.com/v1/chat/completions",
          "the seed's URL is kept verbatim: got '%s'", f.facade.last_setup.api_url.c_str());
    CHECK(f.facade.last_setup.model == "custom-model", "the new provider's model: got '%s'",
          f.facade.last_setup.model.c_str());
}

// ── The custom endpoint path (an endpoint the catalog does not know) ──────────
//
// A catalog provider knows its base URL and its models, so the wizard only asks for a key
// and a pick. Anything else has to be typed: base URL, model id, context window. These
// cases pin that the typed values are what gets persisted, that a refused value keeps the
// user on its step, and that the extra row only exists when something asked for it.

// Type a URL into the provider search box and take the synthetic row it reveals.
static void PickCustomByUrl(Fixture &f, const std::string &url)
{
    Text(*f.dialog, url);
    Enter(*f.dialog); // the "Use this endpoint" row
}

static void test_a_pasted_url_becomes_the_custom_endpoint_and_every_value_is_typed()
{
    TEST("a pasted URL turns into the custom row, and url / model / window are typed and persisted");
    Fixture f;
    f.facade.config_api_key = "sk-new";
    f.Open();

    const std::string url = "https://gw.example.com/v1";
    Text(*f.dialog, url);
    const std::string listed = Frame(*f.dialog);
    CHECK(Has(listed, "Use this endpoint: " + url), "the typed URL is offered as the endpoint");
    CHECK(!Has(listed, "Anthropic"), "and the catalog rows, which cannot match a URL, are gone");

    CHECK(Enter(*f.dialog), "Enter takes the row");
    const std::string endpoint_step = Frame(*f.dialog);
    CHECK(Has(endpoint_step, "API endpoint"), "the endpoint step is next");
    CHECK(Has(endpoint_step, "Step 2/5"), "a custom endpoint walks five steps");
    CHECK(Has(endpoint_step, url), "the pasted URL is pre-filled, ready to edit");
    CHECK(Has(endpoint_step, "Enter submit  |  Esc exit"), "a field step submits");

    Enter(*f.dialog); // endpoint
    CHECK(Has(Frame(*f.dialog), "Step 3/5"), "then the API key");
    Enter(*f.dialog); // key
    const std::string model_step = Frame(*f.dialog);
    CHECK(Has(model_step, "Model id"), "then the model id, typed rather than picked");
    CHECK(!Has(model_step, "Search models..."),
          "and its placeholder follows the path: a typed id, not a search over a list that is not there");
    CHECK(Has(model_step, "Step 4/5"), "which is step four");

    Text(*f.dialog, "gw-large-1");
    Enter(*f.dialog); // model id
    const std::string window_step = Frame(*f.dialog);
    CHECK(Has(window_step, "Context window"), "then the window the catalog would have supplied");
    CHECK(Has(window_step, "Step 5/5"), "which is the last step");
    CHECK(Has(window_step, "128000"), "and it opens on a usable default, not the 1 MiB config one");
    Enter(*f.dialog); // context window

    CHECK(!f.dialog->IsOpen(), "the wizard closed");
    CHECK(f.facade.set_setup_calls == 1, "SetSetup ran once");
    CHECK(f.facade.last_setup.api_url == url, "the typed URL is what was persisted: got '%s'",
          f.facade.last_setup.api_url.c_str());
    CHECK(f.facade.last_setup.model == "gw-large-1", "the typed model id: got '%s'",
          f.facade.last_setup.model.c_str());
    CHECK(f.facade.last_setup.api_key == "sk-new", "the seeded key: got '%s'",
          f.facade.last_setup.api_key.c_str());
    CHECK(f.facade.last_setup.context_window == 128000, "the typed window: got %d",
          f.facade.last_setup.context_window);
    CHECK(f.done_calls == 1, "the host was told to refresh");
}

static void test_typing_custom_reveals_the_row_and_keeps_the_seeded_endpoint()
{
    TEST("typing 'custom' reveals the row, and it keeps the endpoint the config already holds");
    Fixture f;
    f.facade.config_api_url = "https://my-gw.example.com/v1"; // no catalog provider matches
    f.Open();

    Text(*f.dialog, "custom");
    CHECK(Has(Frame(*f.dialog), "Custom endpoint (OpenAI-compatible)"), "the row is reachable by name");

    Enter(*f.dialog);
    const std::string endpoint_step = Frame(*f.dialog);
    CHECK(Has(endpoint_step, "API endpoint"), "Enter takes the row");
    // The seeded URL, not an empty box: an existing custom install must not lose its
    // endpoint by pressing Enter on a pre-filled step.
    CHECK(Has(endpoint_step, "https://my-gw.example.com/v1"), "the config's own endpoint is pre-filled");

    Enter(*f.dialog); // accept the endpoint
    Enter(*f.dialog); // key
    Text(*f.dialog, "gw-model");
    Enter(*f.dialog); // model id
    Enter(*f.dialog); // window

    CHECK(f.facade.last_setup.api_url == "https://my-gw.example.com/v1",
          "the pre-filled endpoint survived: got '%s'", f.facade.last_setup.api_url.c_str());
    CHECK(f.facade.last_setup.model == "gw-model", "and the typed model reached SetSetup: got '%s'",
          f.facade.last_setup.model.c_str());
}

static void test_a_custom_endpoint_without_a_scheme_is_refused()
{
    TEST("an endpoint without http(s):// does not advance, and typing clears the complaint");
    Fixture f;
    f.facade.config_api_url.clear(); // the field starts empty, so what is typed is all there is
    f.facade.config_model.clear();
    f.Open();

    Text(*f.dialog, "custom");
    Enter(*f.dialog); // the custom row
    Text(*f.dialog, "gw.example.com/v1"); // no scheme
    Enter(*f.dialog);

    const std::string refused = Frame(*f.dialog);
    CHECK(Has(refused, "API endpoint"), "still on the endpoint step");
    CHECK(Has(refused, "Step 2/5"), "and the counter did not move");
    CHECK(Has(refused, "must start with http"), "the reason is shown under the field");

    Text(*f.dialog, "x"); // any keystroke is the user answering it
    CHECK(!Has(Frame(*f.dialog), "must start with http"), "typing clears the complaint");
}

static void test_a_custom_model_id_must_not_be_empty()
{
    TEST("the custom model step refuses an empty id");
    Fixture f;
    f.Open();
    PickCustomByUrl(f, "https://gw.example.com/v1");
    Enter(*f.dialog); // endpoint
    Enter(*f.dialog); // key

    CHECK(Has(Frame(*f.dialog), "Model id"), "on the model step");
    CHECK(Enter(*f.dialog), "Enter is consumed");
    CHECK(Has(Frame(*f.dialog), "Model id"), "but an empty id does not advance");
    CHECK(f.facade.set_setup_calls == 0, "and nothing was persisted");
}

static void test_a_custom_context_window_must_be_a_positive_number()
{
    TEST("the window step refuses a non-number, and a correction finishes with the typed value");
    Fixture f;
    f.Open();
    PickCustomByUrl(f, "https://gw.example.com/v1");
    Enter(*f.dialog); // endpoint
    Enter(*f.dialog); // key
    Text(*f.dialog, "gw-model");
    Enter(*f.dialog); // model id
    CHECK(Has(Frame(*f.dialog), "Context window"), "on the window step");

    Text(*f.dialog, "abc"); // "128000abc" is not a number
    Enter(*f.dialog);
    const std::string refused = Frame(*f.dialog);
    CHECK(Has(refused, "Context window"), "still on the window step");
    CHECK(Has(refused, "positive number of tokens"), "the reason is shown");
    CHECK(f.facade.set_setup_calls == 0, "nothing was persisted");

    for (int i = 0; i < 3; i++)
        f.dialog->OnEvent(::ftxui::Event::Backspace); // back to "128000"
    Enter(*f.dialog);
    CHECK(f.facade.set_setup_calls == 1, "the correction finished the wizard");
    CHECK(f.facade.last_setup.context_window == 128000, "with the surviving value: got %d",
          f.facade.last_setup.context_window);
}

static void test_the_custom_row_is_always_the_last_row()
{
    TEST("the custom row is always offered, last, whatever the catalog and filter say");
    {
        Fixture f;
        f.Open();
        const std::string frame = Frame(*f.dialog);
        CHECK(Has(frame, "Custom endpoint (OpenAI-compatible)"), "a plain open offers it");
        /* Last means below every catalog row: the tick on the configured provider is what
         * tells the two apart, so compare the drawn order instead of the text. */
        const std::string rows = LineOf(frame, "Anthropic") + "\n" + LineOf(frame, "OpenAI") + "\n" +
                                 LineOf(frame, "Custom endpoint");
        CHECK(rows.find("Anthropic") < rows.find("Custom endpoint") &&
                  rows.find("OpenAI") < rows.find("Custom endpoint"),
              "it is drawn after the catalog rows");
    }
    {
        Fixture f;
        ClearCatalog(f.facade); // an empty catalog: it is the ONLY row
        f.Open();
        const std::string frame = Frame(*f.dialog);
        CHECK(Has(frame, "Custom endpoint (OpenAI-compatible)"), "an empty catalog still offers it");
        CHECK(!Has(frame, "(no match)"), "and the list never reads as empty");
        Enter(*f.dialog);
        CHECK(Has(Frame(*f.dialog), "API endpoint"), "Enter on it starts the custom path");
    }
}

static void test_a_custom_install_ticks_the_custom_row()
{
    TEST("a config that points outside the catalog is ticked on the custom row, and reuses its window");
    Fixture f;
    f.facade.config_api_url = "https://my-gw.example.com/v1";
    f.facade.config_model = "gw-model";      // in no provider's model list
    f.facade.config_context_window = 200000; // a real choice, so it is offered back
    f.Open();

    const std::string frame = Frame(*f.dialog);
    CHECK(Has(frame, "Custom endpoint (OpenAI-compatible)"), "the custom row is listed for a custom install");
    CHECK(Ticked(frame, "Custom endpoint"), "and it carries the tick, so the current choice is visible");
    CHECK(Has(frame, "Anthropic  (anthropic)"), "the catalog rows are still there to switch to");

    Enter(*f.dialog); // the cursor starts on the ticked row
    CHECK(Has(Frame(*f.dialog), "https://my-gw.example.com/v1"), "Enter opens its endpoint");
    Enter(*f.dialog);
    Enter(*f.dialog); // key
    Text(*f.dialog, "gw-model-2");
    Enter(*f.dialog); // model id
    CHECK(Has(Frame(*f.dialog), "200000"), "the configured window is offered, not the fallback default");

    Enter(*f.dialog);
    CHECK(f.facade.last_setup.context_window == 200000, "and it is what gets persisted: got %d",
          f.facade.last_setup.context_window);
}

static void test_an_unconfigured_install_ticks_nothing()
{
    TEST("with no configured provider or model, no row is ticked -- the custom row included");
    /* The custom row's slot in the view is a sentinel (SIZE_MAX), and SIZE_MAX cast to int is
     * -1 -- which is also "no catalog provider matched". An unguarded comparison therefore
     * ticked the custom row for every unconfigured install and parked the cursor on it. */
    Fixture f;
    f.facade.config_api_url.clear();
    f.facade.config_model.clear();
    f.Open();

    const std::string frame = Frame(*f.dialog);
    CHECK(!Ticked(frame, "Anthropic"), "the first provider is not ticked");
    CHECK(!Ticked(frame, "OpenAI"), "nor the second");
    CHECK(!Ticked(frame, "Custom endpoint"), "nor the custom row");
    CHECK(frame.find("\u2713") == std::string::npos, "nothing on the page carries a tick at all");
}

// ── Escape, close, reopen ───────────────────────────────────────────────────

static void test_escape_marks_the_wizard_done()
{
    TEST("Esc marks the wizard done and closes it");
    Fixture f;
    f.Open();
    CHECK(f.dialog->OnEvent(::ftxui::Event::Escape), "Esc is consumed");
    CHECK(!f.dialog->IsOpen(), "the dialog closed");
    CHECK(f.facade.mark_onboarded_calls == 1, "the 'skip' flag was persisted");
    CHECK(f.facade.set_setup_calls == 0, "and the LLM settings were left alone");
}

static void test_a_closed_dialog_ignores_events()
{
    TEST("a closed dialog consumes no events");
    Fixture f;
    f.Open();
    f.dialog->Close();
    CHECK(!f.dialog->OnEvent(::ftxui::Event::Escape), "Esc falls through when closed");
    CHECK(!f.dialog->OnEvent(::ftxui::Event::Return), "Enter falls through when closed");
    CHECK(f.facade.mark_onboarded_calls == 0, "nothing was persisted");
}

static void test_reopening_starts_over_and_ticks_by_model_when_the_url_differs()
{
    TEST("reopening restarts at step 1, and the tick falls back to 'which provider lists the model'");
    Fixture f;
    // The config holds a bare base that matches no provider verbatim; the model still says
    // which provider it belongs to.
    edc::port::SetupConfig seed;
    seed.api_url = "https://gateway.example.com/v1";
    seed.model = "gpt-4o";
    f.dialog->Open(seed);
    CHECK(Has(Frame(*f.dialog), "Step 1/3"), "a reopen starts at step 1 again");
    CHECK(Ticked(Frame(*f.dialog), "OpenAI"), "the provider that lists the configured model is ticked");
    CHECK(!Ticked(Frame(*f.dialog), "Anthropic"), "the other one is not");

    // The tick also parks the cursor there, so Enter selects it without any Down.
    Enter(*f.dialog);
    Enter(*f.dialog);
    CHECK(Has(Frame(*f.dialog), "gpt-4o"), "Enter on an open dialog selects the ticked provider");
}

static void test_the_list_scrolls_to_keep_the_cursor_visible()
{
    TEST("the catalog part scrolls in 11 lines, and the custom row stays pinned below it");
    Fixture f;
    ClearCatalog(f.facade); // 20 catalog rows + the pinned custom row: the arithmetic is the assertion
    for (int i = 0; i < 20; i++)
        AddProvider(f.facade, "p" + std::to_string(i), "Prov " + std::to_string(i), "");

    f.Open();
    const std::string top = Frame(*f.dialog);
    CHECK(Has(top, "Prov 0"), "at the top the first row is drawn");
    CHECK(Has(top, "Prov 10"), "and the catalog window is full (11 lines: 12 minus the pinned row)");
    CHECK(!Has(top, "Prov 11"), "but not beyond it");
    CHECK(Has(top, "Custom endpoint"), "the custom row is visible without scrolling -- the point of pinning it");

    Down(*f.dialog, 9); // -> row 9, catalog window starts at 4
    const std::string middle = Frame(*f.dialog);
    CHECK(!Has(middle, "Prov 0"), "scrolling drops the rows above the window");
    CHECK(Has(middle, "Prov 4"), "the window is re-anchored");
    CHECK(Has(middle, "Prov 14"), "and still holds 11 catalog rows");
    CHECK(Has(middle, "Custom endpoint"), "while the custom row does not scroll away");

    Down(*f.dialog, 20); // clamp at the last row, which is the pinned custom row
    const std::string bottom = Frame(*f.dialog);
    CHECK(Has(bottom, "Prov 19"), "the last catalog row is reachable");
    CHECK(Has(bottom, "Custom endpoint"), "and the custom row is still there");
    CHECK(!Has(bottom, "Prov 7"), "and the window clamped to the end");
    CHECK(Has(bottom, "Prov 9"), "the catalog window's first row is n-11: got the row above it");
    CHECK(!Has(bottom, "Prov 8"), "and nothing above it");
}

static void test_the_model_cursor_moves_and_clamps_at_both_ends()
{
    TEST("the model cursor moves, and clamps at both ends");
    Fixture f;
    ClearCatalog(f.facade);
    AddProvider(f.facade, "p", "P", "https://api.example.com/v1");
    AddModel(f.facade, "p", "m-a", "Model A", 1000);
    AddModel(f.facade, "p", "m-b", "Model B", 2000);
    AddModel(f.facade, "p", "m-c", "Model C", 3000);

    edc::port::SetupConfig seed;
    seed.api_url = "https://api.example.com/v1"; // no configured model: the cursor starts at 0
    f.dialog->Open(seed);
    Enter(*f.dialog);
    Enter(*f.dialog);

    Up(*f.dialog, 2); // already at the first row
    CHECK(Enter(*f.dialog), "Enter is consumed by the model step");
    CHECK(f.facade.last_setup.model == "m-a", "Up past the top keeps the first model: got '%s'",
          f.facade.last_setup.model.c_str());

    Fixture g;
    ClearCatalog(g.facade);
    AddProvider(g.facade, "p", "P", "https://api.example.com/v1");
    AddModel(g.facade, "p", "m-a", "Model A", 1000);
    AddModel(g.facade, "p", "m-b", "Model B", 2000);
    AddModel(g.facade, "p", "m-c", "Model C", 3000);
    g.dialog->Open(seed);
    Enter(*g.dialog);
    Enter(*g.dialog);

    Down(*g.dialog, 1); // -> m-b
    Down(*g.dialog, 9); // past the last row: clamps to m-c
    Enter(*g.dialog);
    CHECK(g.facade.last_setup.model == "m-c", "Down clamps at the last model: got '%s'",
          g.facade.last_setup.model.c_str());
    CHECK(g.facade.last_setup.context_window == 3000, "and the clamped row's context window travels: got %d",
          g.facade.last_setup.context_window);
}

static void test_a_provider_url_with_a_trailing_slash_still_ticks()
{
    TEST("the tick compares base URLs, so a trailing slash does not hide the current provider");
    Fixture f;
    ClearCatalog(f.facade);
    AddProvider(f.facade, "p", "P", "https://api.example.com/v1/"); // with the slash
    AddModel(f.facade, "p", "m-a", "Model A", 1000);

    edc::port::SetupConfig seed;
    seed.api_url = "https://api.example.com/v1"; // without it
    f.dialog->Open(seed);

    CHECK(Ticked(Frame(*f.dialog), "P"), "the provider is still recognised as the current one");
}

static void test_a_filter_that_shrinks_the_list_clamps_the_cursor()
{
    TEST("filtering the list back to one row clamps the cursor instead of pointing past the end");
    Fixture f;
    ClearCatalog(f.facade);
    for (int i = 0; i < 5; i++)
        AddProvider(f.facade, "p" + std::to_string(i), "Prov " + std::to_string(i), "");
    AddModel(f.facade, "p0", "zero-model", "Zero Model", 1000);

    f.Open();
    Down(*f.dialog, 4); // cursor on the last provider
    Text(*f.dialog, "p0"); // shrink the list to one row
    const std::string frame = Frame(*f.dialog);
    CHECK(Has(frame, "Prov 0"), "the surviving row is drawn");
    CHECK(!Has(frame, "Prov 4"), "the rows are gone");

    Enter(*f.dialog); // commit: must pick p0, not an out-of-range row
    Enter(*f.dialog);
    CHECK(Has(Frame(*f.dialog), "zero-model"), "Enter picked the surviving provider");
}

static void test_a_dialog_without_a_facade_still_opens_and_exits()
{
    TEST("a dialog with no facade still opens, draws and exits");
    edc::ui::SetupDialog dialog(nullptr);
    edc::port::SetupConfig seed;
    dialog.Open(seed);
    CHECK(dialog.IsOpen(), "it opened");
    CHECK(Has(Frame(dialog), "Custom endpoint"), "with no catalog, the custom row is all there is");
    CHECK(dialog.OnEvent(::ftxui::Event::Escape), "Esc is consumed");
    CHECK(!dialog.IsOpen(), "and closes it");
}

} // namespace

int main()
{
    test_open_lands_on_step_1_and_ticks_the_configured_provider();
    test_provider_filter_matches_both_the_name_and_the_id();
    test_a_provider_filter_with_no_match_leaves_only_the_custom_row();
    test_the_cursor_moves_and_clamps_at_both_ends();
    test_enter_on_the_provider_step_moves_to_the_key_step();
    test_the_key_step_types_where_the_caret_was_left();
    test_the_model_list_hides_models_that_are_not_chat_capable();
    test_a_model_row_shows_the_display_name_and_the_wire_id();
    test_a_model_filter_narrows_by_id_and_by_name();
    test_finishing_persists_the_whole_setup_and_closes();
    test_a_rejected_setup_leaves_the_wizard_open();
    test_enter_with_no_model_does_not_finish();
    test_provider_without_an_api_url_keeps_the_seeded_one();
    test_a_pasted_url_becomes_the_custom_endpoint_and_every_value_is_typed();
    test_typing_custom_reveals_the_row_and_keeps_the_seeded_endpoint();
    test_a_custom_endpoint_without_a_scheme_is_refused();
    test_a_custom_model_id_must_not_be_empty();
    test_a_custom_context_window_must_be_a_positive_number();
    test_the_custom_row_is_always_the_last_row();
    test_a_custom_install_ticks_the_custom_row();
    test_an_unconfigured_install_ticks_nothing();
    test_escape_marks_the_wizard_done();
    test_a_closed_dialog_ignores_events();
    test_reopening_starts_over_and_ticks_by_model_when_the_url_differs();
    test_the_list_scrolls_to_keep_the_cursor_visible();
    test_the_model_cursor_moves_and_clamps_at_both_ends();
    test_a_provider_url_with_a_trailing_slash_still_ticks();
    test_a_filter_that_shrinks_the_list_clamps_the_cursor();
    test_a_dialog_without_a_facade_still_opens_and_exits();

    printf("\n%d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
