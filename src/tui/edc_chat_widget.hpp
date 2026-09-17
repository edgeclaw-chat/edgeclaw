#pragma once

#include "edc_message.hpp"
#include "edc_message_list.hpp"
#include "edc_scroll_view.hpp"
#include "ftxui/component/component_base.hpp"
#include <memory>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace edc
{

class ChatWidget : public ::ftxui::ComponentBase
{
  public:
    ChatWidget();
    ChatWidget(ChatWidget const &) = delete;
    ChatWidget &operator=(ChatWidget const &) = delete;

    void AddMessage(std::shared_ptr<Message> message);

    // Remove every message (used when the chat log is re-pointed at a different
    // session's transcript). The scroll state survives; the next render shows
    // an empty log.
    void Clear();

    // Point the log at one group's or one task's history (the sidebar's
    // selection): |allowed| is the set of task ids the selection resolves to,
    // and only bubbles belonging to one of them are drawn. nullptr = no filter.
    // |label| names the selection for the "nothing here" hint. A bubble with no
    // task is hidden while a filter is set -- that is what keeps command replies
    // and errors out of a filtered log (see Message::TaskId).
    void SetFilter(std::shared_ptr<const std::set<std::string>> allowed, std::string label = "");

    // The user input each task was created from (task id -> raw input), as the
    // task snapshot reports it. While a filter is set, a task's input is rendered
    // as the first user message of that task's history -- the input's own bubble
    // belongs to no task (it is echoed before the task exists), so the filter
    // hides it and this is what stands in for it. Looked up by id: no matching,
    // no inference, and it works the same on replay after a restart.
    void SetTaskInputs(std::shared_ptr<const std::map<std::string, std::string>> inputs);

    bool OnEvent(ftxui::Event) override;
    ftxui::Element OnRender() override;

    // The chat log fills whatever height its parent grants and scrolls the
    // overflow with the mouse wheel, but it is deliberately NOT keyboard
    // focusable: ↑/↓ are reserved for the message editor, and no key needs
    // to reach the log. Focusable() is therefore omitted — it inherits
    // false from the (non-focusable) ScrollView the widget wraps.

    // Move the viewport by |delta| lines. Scrolling away from the bottom
    // releases the auto-follow, reaching the bottom again re-arms it.
    // Returns false when the view was already against that end, so an
    // enclosing container can use the key to move focus instead.
    bool Scroll(int delta);
    bool ScrollToTop();
    bool ScrollToBottom();

  private:
    // Does |message| pass the current filter? Everything passes when none is set.
    bool Visible(const Message &message) const;
    // Rebuild the drawn list from all_ under the current filter.
    void ApplyFilter();

    // Every bubble the log ever received, filter or not: the filter is a *view*
    // over the log, so switching back to "all" loses nothing. list_ below holds
    // only the visible subset actually drawn.
    std::vector<std::shared_ptr<Message>> all_;
    std::shared_ptr<const std::map<std::string, std::string>> task_inputs_;
    // The tasks whose input line has already been emitted into list_ by the
    // current ApplyFilter() pass: each task's input comes once, before its first
    // visible message.
    std::set<std::string> input_shown_;
    std::shared_ptr<const std::set<std::string>> filter_;
    std::string filter_label_;

    // Holds the messages and draws only the ones inside the viewport; see
    // edc_message_list.hpp for why the log is not a plain component container.
    std::shared_ptr<MessageList> list_;
    std::shared_ptr<ScrollView> scroll_view_;
};

} // namespace edc
