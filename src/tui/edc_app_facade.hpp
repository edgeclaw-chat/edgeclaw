#pragma once

// ── 端口层: 应用边界 (IAppFacade) + 通道事件的强类型出口 (IChannelSink) ──────
//
// 现状: TUIWindow 直接 include edc_api.h, 在静态回调 `on_channel_adapter_send`
// 里读 `edc_app_message_*` 分类、取字段, 再在 `screen_->Post` 的闭包里分派 ——
// C 边界、消息类型字符串、线程 marshal 全糊在一个上帝类里。
//
// 这里把它拆成两个角色:
//   * IAppFacade / AppFacade —— 唯一的 C 边界。它拥有 edc_app 生命周期, 读
//     `edc_app_message_*` 把通道消息**翻译**成下面这套强类型事件, 经 IUiDispatcher
//     marshal 到 UI 线程, 交给 IChannelSink。视图从此不见 edc_app_*。
//   * IChannelSink —— 强类型事件的下游 (今天由 TUIWindow 实现; 之后会收进
//     独立的 ChannelDispatcher + 模型, 见 docs/tui-oop-redesign.zh.md P3)。
//
// IUiDispatcher 是"投到 UI 线程"这一动作的端口: 生产实现走 ftxui 屏幕, 没有
// 屏幕时内联执行 —— 后者正是测试能同步喂消息的原因。

#include "edc_api.h" /* struct edc_channel_message + message_* 只读窗口 + EDC_APP_RENDER_* */

#include <cstddef>
#include <ctime>
#include <functional>
#include <memory>
#include <string>

namespace edc::port
{

// UI 线程调度端口。
class IUiDispatcher
{
  public:
    virtual ~IUiDispatcher() = default;
    // 把 |task| 投到 UI 线程; 没有屏幕时同步执行 (测试路径)。
    virtual void Post(std::function<void()> task) = 0;
    // 请求重绘一帧。
    virtual void RequestFrame() = 0;
};

// 通道消息的种类 (对应原 `enum class Kind`)。None 不在此列: 认不出来的消息
// 在适配器里直接丢弃, 不会到达 sink。
enum class ChatKind
{
    User,
    Tool,
    Thought,
    Reply,
    System,
};

// 一条聊天消息的强类型快照: 适配器在 C 消息存活期间拷出全部字段。
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

// 强类型通道出口。适配器在 UI 线程上调用这些方法。
class IChannelSink
{
  public:
    virtual ~IChannelSink() = default;
    virtual void OnChatMessage(const ChatMessageEvent &event) = 0;
    // 任务树快照 (原始 JSON: 解析 + 认领属于前端自身的判断)。
    virtual void OnTaskSnapshot(const std::string &json) = 0;
    // "session X 的树变了, 原因 reason" 控制事件 (不带树)。
    virtual void OnTreeChanged(const std::string &session_id, const std::string &reason) = 0;
    // `/task history` / `/group history` 的结构化载荷。
    virtual void OnTaskHistory(const std::string &payload) = 0;
};

// /connect 弹窗提交的配置。
struct ConnectConfig
{
    std::string api_url;
    std::string model;
    std::string api_key;
};

// 前端需要的 app 操作的投影。视图/控制器依赖它, 而不是 edc_app_* C API。
class IAppFacade
{
  public:
    virtual ~IAppFacade() = default;

    // False when the app failed to start (bad config / key / no tty).
    virtual bool Ready() const = 0;

    // ── 配置读取 ────────────────────────────────────────────────
    virtual bool ConfigModel(std::string *out) const = 0;
    virtual bool ConfigApiUrl(std::string *out) const = 0;
    virtual bool ConfigApiKey(std::string *out) const = 0;
    virtual bool SandboxEnabled() const = 0;
    // Persist + hot-apply; true when it succeeded.
    virtual bool SetLlmConfig(const ConnectConfig &config) = 0;

    // ── 前端要做的几件事 ────────────────────────────────────────
    virtual void SendUserMessage(const std::string &session_id, const std::string &content) = 0;
    virtual void RequestTaskSnapshot(const std::string &session_id) = 0;

    // ── 路径 / 文件 (前端自己拥有的文件) ────────────────────────
    virtual bool HistoryPath(std::string *out) const = 0;
    virtual bool ChatLogPathFor(const std::string &session_id, std::string *out) const = 0;
    virtual bool WriteFile(const std::string &path, const char *data, size_t len, bool append,
                           bool under_workspace) = 0;
    virtual bool EnsureParentDir(const std::string &path) = 0;

    // 适配器入口: 生产路径注册的 C 回调最终进这里。公开是**给测试**用的 ——
    // 可以直接把一条真 `struct edc_channel_message` 喂进来; 没 SetScreen() 时
    // 事件同步执行, 喂完即可渲染断言 (见 tests/test_tui_dispatch.cpp)。
    virtual void HandleChannelMessage(struct edc_channel_message *message) = 0;
};

// edc_app_* 的生产实现: 拥有 app 生命周期, 把 C 回调翻译成强类型事件并经
// IUiDispatcher marshal 到 UI 线程。
class AppFacade : public IAppFacade
{
  public:
    // |dispatcher| / |sink| 都不被拥有, 须比本对象活得久 (今天都指向 TUIWindow)。
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
    // C 回调蹦床: instance 就是本 AppFacade。
    static void ChannelTrampoline(void *instance, struct edc_channel_message *message);
    // 读 C 消息 → 造强类型事件 → Post 到 UI 线程。必须在 C 消息存活期间完成取字段。
    void TranslateAndDispatch(struct edc_channel_message *message);

    struct edc_app *app_ = nullptr;
    IUiDispatcher *dispatcher_ = nullptr;
    IChannelSink *sink_ = nullptr;
};

} // namespace edc::port
