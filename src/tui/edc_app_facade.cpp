#include "edc_app_facade.hpp"

#include "edc_app.h"

#include <utility>

namespace edc::port
{

AppFacade::AppFacade(int sandbox_override, IUiDispatcher *dispatcher, IChannelSink *sink)
    : dispatcher_(dispatcher), sink_(sink)
{
    app_ = edc_app_new(false, sandbox_override);
    // Register the C callback only when the app started; an app that failed to
    // start leaves Ready() false and every method below is a safe no-op.
    if (app_)
        edc_app_channel_adapter_init(app_, this, &AppFacade::ChannelTrampoline);
}

AppFacade::~AppFacade()
{
    if (app_)
        edc_app_free(app_);
}

bool AppFacade::Ready() const
{
    return app_ != nullptr;
}

bool AppFacade::ConfigModel(std::string *out) const
{
    if (!app_)
        return false;
    char buf[256];
    if (!edc_app_config_model(app_, buf, sizeof(buf)))
        return false;
    if (out)
        *out = buf;
    return true;
}

bool AppFacade::ConfigApiUrl(std::string *out) const
{
    if (!app_)
        return false;
    char buf[4096];
    if (!edc_app_config_api_url(app_, buf, sizeof(buf)))
        return false;
    if (out)
        *out = buf;
    return true;
}

bool AppFacade::ConfigApiKey(std::string *out) const
{
    if (!app_)
        return false;
    char buf[4096];
    if (!edc_app_config_api_key(app_, buf, sizeof(buf)))
        return false;
    if (out)
        *out = buf;
    return true;
}

bool AppFacade::SandboxEnabled() const
{
    return app_ && edc_app_sandbox_enabled(app_);
}

int AppFacade::ContextWindow() const
{
    return app_ ? edc_app_config_context_window(app_) : 0;
}

bool AppFacade::Onboarded() const
{
    return app_ && edc_app_onboarded(app_);
}

bool AppFacade::SetSetup(const SetupConfig &config)
{
    if (!app_)
        return false;
    return edc_app_complete_setup(app_, config.api_url.c_str(), config.model.c_str(), config.api_key.c_str(),
                                  config.context_window);
}

bool AppFacade::MarkOnboarded()
{
    return app_ && edc_app_mark_onboarded(app_);
}

std::string AppFacade::TestEndpoint(const std::string &api_url, const std::string &api_key) const
{
    if (!app_)
        return "app not ready";
    char buf[160] = {0};
    edc_app_test_endpoint(app_, api_url.c_str(), api_key.c_str(), 8000, buf, sizeof(buf));
    return buf;
}

size_t AppFacade::Providers(std::vector<std::string> *ids) const
{
    size_t n = edc_app_models_providers();
    if (ids)
    {
        ids->clear();
        for (size_t i = 0; i < n; i++)
        {
            const char *id = edc_app_models_provider_id(i);
            if (id)
                ids->emplace_back(id);
        }
    }
    return n;
}

std::string AppFacade::ProviderName(const std::string &provider_id) const
{
    const char *s = edc_app_models_provider_name(provider_id.c_str());
    return s ? s : "";
}

std::string AppFacade::ProviderApi(const std::string &provider_id) const
{
    const char *s = edc_app_models_provider_api(provider_id.c_str());
    return s ? s : "";
}

std::string AppFacade::ProviderEnvName(const std::string &provider_id, size_t index) const
{
    const char *s = edc_app_models_provider_env_name(provider_id.c_str(), index);
    return s ? s : "";
}

size_t AppFacade::Models(const std::string &provider_id, std::vector<std::string> *ids) const
{
    size_t n = edc_app_models_count(provider_id.c_str());
    if (ids)
    {
        ids->clear();
        for (size_t i = 0; i < n; i++)
        {
            const char *id = edc_app_models_model_id(provider_id.c_str(), i);
            if (id)
                ids->emplace_back(id);
        }
    }
    return n;
}

bool AppFacade::ModelInfo(const std::string &model_id, ModelFacts *out) const
{
    const char *mid = model_id.c_str();
    const char *name = edc_app_models_model_name(mid);
    if (!name)
        return false;
    if (out)
    {
        out->name = name;
        out->context = edc_app_models_model_context(mid);
        out->output = edc_app_models_model_output(mid);
        out->chat = edc_app_models_model_is_chat(mid);
    }
    return true;
}

std::vector<model::CommandFact> AppFacade::Commands() const
{
    std::vector<model::CommandFact> out;
    const size_t n = edc_app_cmd_count();
    out.reserve(n);
    for (size_t i = 0; i < n; i++)
    {
        const char *t = edc_app_cmd_text(i);
        const char *b = edc_app_cmd_brief(i);
        if (t)
            out.push_back({t, b ? b : ""});
    }
    return out;
}

std::vector<model::CommandFact> AppFacade::CommandFamilies() const
{
    std::vector<model::CommandFact> out;
    const size_t n = edc_app_cmd_family_count();
    out.reserve(n);
    for (size_t i = 0; i < n; i++)
    {
        const char *t = edc_app_cmd_family_name(i);
        const char *b = edc_app_cmd_family_brief(i);
        if (t)
            out.push_back({t, b ? b : ""});
    }
    return out;
}

std::vector<model::CommandFact> AppFacade::PromptNames() const
{
    std::vector<model::CommandFact> out;
    if (!app_)
        return out;

    // "<name>\t<description>\n" per template (see edc_app_prompts).
    char buf[4096] = {0};
    edc_app_prompts(app_, buf, sizeof(buf));
    const std::string all(buf);
    size_t pos = 0;
    while (pos < all.size())
    {
        const size_t nl = all.find('\n', pos);
        const std::string line =
            (nl == std::string::npos) ? all.substr(pos) : all.substr(pos, nl - pos);
        pos = (nl == std::string::npos) ? all.size() : nl + 1;
        if (line.empty())
            continue;
        const size_t tab = line.find('\t');
        const std::string name = (tab == std::string::npos) ? line : line.substr(0, tab);
        const std::string desc = (tab == std::string::npos) ? "" : line.substr(tab + 1);
        if (!name.empty())
            out.push_back({"/prompt " + name, desc});
    }
    return out;
}

void AppFacade::SendUserMessage(const std::string &session_id, const std::string &content)
{
    if (app_)
        edc_app_send_user_message(app_, session_id.c_str(), content.c_str());
}

void AppFacade::RequestTaskSnapshot(const std::string &session_id)
{
    if (app_)
        edc_app_request_task_snapshot(app_, session_id.c_str());
}

bool AppFacade::HistoryPath(std::string *out) const
{
    char buf[4096];
    if (!edc_app_history_path(buf, sizeof(buf)))
        return false;
    if (out)
        *out = buf;
    return true;
}

bool AppFacade::ChatLogPathFor(const std::string &session_id, std::string *out) const
{
    if (!app_)
        return false;
    char buf[4096];
    if (!edc_app_chat_log_path_for(app_, session_id.c_str(), buf, sizeof(buf)))
        return false;
    if (out)
        *out = buf;
    return true;
}

bool AppFacade::WriteFile(const std::string &path, const char *data, size_t len, bool append, bool under_workspace)
{
    if (!app_)
        return false;
    return edc_app_write_file(app_, path.c_str(), data, len, append, under_workspace);
}

bool AppFacade::EnsureParentDir(const std::string &path)
{
    if (!app_)
        return false;
    return edc_app_ensure_parent_dir(app_, path.c_str());
}

void AppFacade::ChannelTrampoline(void *instance, struct edc_channel_message *message)
{
    if (!instance || !message)
        return;
    static_cast<AppFacade *>(instance)->TranslateAndDispatch(message);
}

void AppFacade::HandleChannelMessage(struct edc_channel_message *message)
{
    TranslateAndDispatch(message);
}

// Verbatim from the old TUIWindow::on_channel_adapter_send classification + field
// copying: read the C message (lifetime = this function), build the typed event,
// Post to the UI thread. The view side no longer touches the C API.
void AppFacade::TranslateAndDispatch(struct edc_channel_message *message)
{
    if (!message || !sink_ || !dispatcher_)
        return;

    const auto *content = edc_app_message_content(message);

    // Structured reply for `/task history <id>` / `/group history <id>`.
    if (edc_app_message_is_task_history(message))
    {
        const std::string payload = content ? content : "";
        IChannelSink *sink = sink_;
        dispatcher_->Post([sink, payload]() { sink->OnTaskHistory(payload); });
        return;
    }
    if (!content)
        return;

    // Tree-changed control event (no tree): the frontend decides whether to pull.
    if (edc_app_message_is_task_tree_changed(message))
    {
        const char *sid_c = edc_app_message_session_id(message);
        const std::string sid = sid_c ? sid_c : "";
        const std::string reason = content;
        IChannelSink *sink = sink_;
        dispatcher_->Post([sink, sid, reason]() { sink->OnTreeChanged(sid, reason); });
        return;
    }

    // Task-tree snapshot: raw JSON for the frontend to parse + claim.
    if (edc_app_message_is_task_snapshot(message))
    {
        const std::string snapshot = content;
        IChannelSink *sink = sink_;
        dispatcher_->Post([sink, snapshot]() { sink->OnTaskSnapshot(snapshot); });
        return;
    }

    // Chat message: classify (anything unrecognized is dropped); the result is a
    // typed enum, no longer a string.
    ChatMessageEvent event;
    if (edc_app_message_is_user(message))
        event.kind = ChatKind::User;
    else if (edc_app_message_is_tool(message))
        event.kind = ChatKind::Tool;
    else if (edc_app_message_is_thought(message))
        event.kind = ChatKind::Thought;
    else if (edc_app_message_is_resp(message))
        event.kind = ChatKind::Reply;
    else if (edc_app_message_is_sys(message))
        event.kind = ChatKind::System;
    else
        return; // unrecognized type: the frontend does nothing

    event.content = content;
    const char *header_c = edc_app_message_header(message);
    event.header = header_c ? header_c : "";
    const char *task_id_c = edc_app_message_task_id(message);
    event.task_id = task_id_c ? task_id_c : "";
    const char *session_id_c = edc_app_message_session_id(message);
    event.session_id = session_id_c ? session_id_c : "";
    event.render_pref = edc_app_message_render_pref(message);
    event.from_active_task = edc_app_message_from_active_task(message) != 0;
    event.ts = ::time(nullptr);

    IChannelSink *sink = sink_;
    dispatcher_->Post([sink, event]() { sink->OnChatMessage(event); });
}

} // namespace edc::port
