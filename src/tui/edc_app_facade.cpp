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

bool AppFacade::SetLlmConfig(const ConnectConfig &config)
{
    if (!app_)
        return false;
    return edc_app_set_llm_config(app_, config.api_url.c_str(), config.model.c_str(), config.api_key.c_str());
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
