#pragma once

#include "edc_chat_model.hpp"
#include "edc_message.hpp"
#include "edc_message_list.hpp"
#include "edc_scroll_view.hpp"
#include "ftxui/component/component_base.hpp"
#include <map>
#include <memory>
#include <set>
#include <string>

namespace edc
{

// The chat log **view**: renders a ChatModel's visible() sequence inside a
// scrolling, virtualized list. It owns no message state of its own —— all of it
// (the messages, the filter, each task's input) lives in the model, so the log
// can be driven by anyone holding the model (the window / a dispatcher) without
// touching rendering.
//
// The compat methods below (AddMessage / Clear / SetFilter / SetTaskInputs)
// forward to the model, so existing callers and tests are unchanged. The view
// subscribes to the model and updates incrementally: an appended bubble is
// appended to the list (keeping the O(1) append that the virtualization relies
// on), a filter/task-input/clear change rebuilds the list from visible().
class ChatWidget : public ::ftxui::ComponentBase
{
  public:
    // Self-owned model (tests / standalone use).
    ChatWidget();
    // Shared model (the window injects one it also hands to the dispatcher).
    explicit ChatWidget(std::shared_ptr<model::ChatModel> model);
    ~ChatWidget() override;

    ChatWidget(ChatWidget const &) = delete;
    ChatWidget &operator=(ChatWidget const &) = delete;

    // ── 兼容入口: 委托给模型 ──────────────────────────────────
    void AddMessage(std::shared_ptr<Message> message);
    void Clear();
    void SetFilter(std::shared_ptr<const std::set<std::string>> allowed, std::string label = "");
    void SetTaskInputs(std::shared_ptr<const std::map<std::string, std::string>> inputs);

    // The model backing this view (never null).
    const std::shared_ptr<model::ChatModel> &model() const
    {
        return model_;
    }

    bool OnEvent(ftxui::Event) override;
    ftxui::Element OnRender() override;

    // The chat log is deliberately NOT keyboard focusable (↑/↓ belong to the
    // message editor); it still scrolls with the wheel. Focusable() is inherited
    // false from the ScrollView it wraps.

    bool Scroll(int delta);
    bool ScrollToTop();
    bool ScrollToBottom();

  private:
    // One model change → one list update (append or rebuild).
    void OnModelChanged(const model::ChatModel::Change &change);

    std::shared_ptr<model::ChatModel> model_;
    int subscription_ = -1;

    // Holds the messages and draws only the ones inside the viewport; see
    // edc_message_list.hpp for why the log is not a plain component container.
    std::shared_ptr<MessageList> list_;
    std::shared_ptr<ScrollView> scroll_view_;
};

} // namespace edc
