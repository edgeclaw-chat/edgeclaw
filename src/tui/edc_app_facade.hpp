#pragma once

// Port layer: the application boundary (IAppFacade) + the strongly-typed channel
// event sink (IChannelSink).
//
// Before: TUIWindow included edc_api.h directly, classified messages by reading
// edc_app_message_* in a static callback `on_channel_adapter_send`, and then
// dispatched inside a `screen_->Post` closure -- the C boundary, message-type
// strings and thread marshalling all welded into one god class.
//
// Now split into two roles:
//   * IAppFacade / AppFacade -- the only C boundary. It owns the edc_app lifetime,
//     reads edc_app_message_* and translates channel messages into the strongly
//     typed events below, marshals them onto the UI thread through IUiDispatcher,
//     and hands them to IChannelSink. Views never see edc_app_* again.
//   * IChannelSink -- the downstream of strongly typed events (implemented by
//     TUIWindow today; later by ChannelDispatcher + models, see P3).
//
// IUiDispatcher is the "post to the UI thread" port: production posts to the ftxui
// screen, with no screen it runs inline -- which is what lets tests feed messages
// synchronously.

#include "edc_api.h" /* struct edc_channel_message + message_* read-only views + EDC_APP_RENDER_* */

#include <cstddef>
#include <ctime>
#include <functional>
#include <memory>
#include <string>

namespace edc::port
{

// UI-thread dispatch port.
class IUiDispatcher
{
  public:
    virtual ~IUiDispatcher() = default;
    // Post |task| to the UI thread; with no screen, run synchronously (test path).
    virtual void Post(std::function<void()> task) = 0;
    // Request one repaint.
    virtual void RequestFrame() = 0;
};

// The kind of a channel message (the old `enum class Kind`). None is absent:
// unrecognized messages are dropped in the adapter and never reach the sink.
enum class ChatKind
{
    User,
    Tool,
    Thought,
    Reply,
    System,
};

// A strongly typed snapshot of one chat message: the adapter copies all fields
// while the C message is alive.
struct ChatMessageEvent
{
    ChatKind kind = ChatKind::System;
    std::string content;
    std::string header;
    std::string task_id;
    std::string session_id;
    int render_pref = 0; /* EDC_APP_RENDER_PLAIN / _MARKDOWN */
    bool from_active_task = false;
    time_t ts = 0;
};

// Strongly typed channel sink. The adapter calls these on the UI thread.
class IChannelSink
{
  public:
    virtual ~IChannelSink() = default;
    virtual void OnChatMessage(const ChatMessageEvent &event) = 0;
    // Task-tree snapshot (raw JSON: parsing + claiming belong to the frontend).
    virtual void OnTaskSnapshot(const std::string &json) = 0;
    // "session X's tree changed, reason" control event (no tree).
    virtual void OnTreeChanged(const std::string &session_id, const std::string &reason) = 0;
    // Structured payload for `/task history` / `/group history`.
    virtual void OnTaskHistory(const std::string &payload) = 0;
};

// The config submitted by the /connect dialog.
struct ConnectConfig
{
    std::string api_url;
    std::string model;
    std::string api_key;
};

// Projection of the app operations the frontend needs. Views/controllers depend on
// this, not on the edc_app_* C API.
class IAppFacade
{
  public:
    virtual ~IAppFacade() = default;

    // False when the app failed to start (bad config / key / no tty).
    virtual bool Ready() const = 0;

    // ── config reads ─────────────────────────────────────────────
    virtual bool ConfigModel(std::string *out) const = 0;
    virtual bool ConfigApiUrl(std::string *out) const = 0;
    virtual bool ConfigApiKey(std::string *out) const = 0;
    virtual bool SandboxEnabled() const = 0;
    // Persist + hot-apply; true when it succeeded.
    virtual bool SetLlmConfig(const ConnectConfig &config) = 0;

    // ── a few things the frontend does ───────────────────────────
    virtual void SendUserMessage(const std::string &session_id, const std::string &content) = 0;
    virtual void RequestTaskSnapshot(const std::string &session_id) = 0;

    // ── paths / files the frontend owns ──────────────────────────
    virtual bool HistoryPath(std::string *out) const = 0;
    virtual bool ChatLogPathFor(const std::string &session_id, std::string *out) const = 0;
    virtual bool WriteFile(const std::string &path, const char *data, size_t len, bool append,
                           bool under_workspace) = 0;
    virtual bool EnsureParentDir(const std::string &path) = 0;

    // Adapter entry: the C callback registered by production lands here. Public for
    // tests -- they can feed a real `struct edc_channel_message`; with no
    // SetScreen() the event runs synchronously, so the assertion follows at once.
    virtual void HandleChannelMessage(struct edc_channel_message *message) = 0;
};

// Production edc_app_* implementation: owns the app lifetime and translates C
// callbacks into strongly typed events marshalled via IUiDispatcher.
class AppFacade : public IAppFacade
{
  public:
    // |dispatcher| / |sink| are not owned and must outlive this object (both point
    // at TUIWindow today).
    AppFacade(int sandbox_override, IUiDispatcher *dispatcher, IChannelSink *sink);
    ~AppFacade() override;

    AppFacade(const AppFacade &) = delete;
    AppFacade &operator=(const AppFacade &) = delete;

    bool Ready() const override;

    bool ConfigModel(std::string *out) const override;
    bool ConfigApiUrl(std::string *out) const override;
    bool ConfigApiKey(std::string *out) const override;
    bool SandboxEnabled() const override;
    bool SetLlmConfig(const ConnectConfig &config) override;

    void SendUserMessage(const std::string &session_id, const std::string &content) override;
    void RequestTaskSnapshot(const std::string &session_id) override;

    bool HistoryPath(std::string *out) const override;
    bool ChatLogPathFor(const std::string &session_id, std::string *out) const override;
    bool WriteFile(const std::string &path, const char *data, size_t len, bool append,
                   bool under_workspace) override;
    bool EnsureParentDir(const std::string &path) override;

    void HandleChannelMessage(struct edc_channel_message *message) override;

  private:
    // C callback trampoline: instance is this AppFacade.
    static void ChannelTrampoline(void *instance, struct edc_channel_message *message);
    // Read the C message -> build the typed event -> Post to the UI thread. Fields
    // must be read while the C message is alive.
    void TranslateAndDispatch(struct edc_channel_message *message);

    struct edc_app *app_ = nullptr;
    IUiDispatcher *dispatcher_ = nullptr;
    IChannelSink *sink_ = nullptr;
};

} // namespace edc::port
