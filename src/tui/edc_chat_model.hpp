#pragma once

// Model layer: the chat log + filtered view (ChatModel).
//
// These states and the filter logic used to be ChatWidget's private fields (+
// ApplyFilter): view and state/rules were welded into one class, so the rules
// could not be unit-tested without ftxui and no other role (e.g.
// ChannelDispatcher) could write to the log without touching rendering.
//
// Now ChatModel holds all messages, the current filter (which task ids are
// visible) and each task's raw input, and computes the filtered visible sequence;
// ChatWidget degrades to a view that renders visible() and updates incrementally
// via a subscription.
//
// Compat: ChatWidget still exposes AddMessage / Clear / SetFilter / SetTaskInputs
// (delegating to the model).

#include "edc_message.hpp"

#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace edc::model
{

class ChatModel
{
  public:
    // Change notification: a non-null |appended| = one visible message appended
    // (the view appends incrementally); a null |appended| = full rebuild (filter /
    // task inputs / clear -- the view rebuilds from visible()).
    struct Change
    {
        std::shared_ptr<Message> appended;               // one visible bubble appended (view appends it)
        std::vector<std::shared_ptr<Message>> prepended; // older bubbles prepended, file order (view prepends them)
    };
    using Listener = std::function<void(const Change &)>;

    int Subscribe(Listener listener);
    void Unsubscribe(int id);

    // Append one bubble. Recorded whether or not it passes the filter; the view
    // only learns about it when it is visible.
    void AddMessage(std::shared_ptr<Message> message);

    // Insert older bubbles (file order: oldest first) above the current head. The
    // startup backfill uses it to fill the history in without a full blocking
    // replay.
    void PrependMessages(std::vector<std::shared_ptr<Message>> messages);

    // Drop every message (the log was re-pointed at another session).
    void Clear();

    // Point the log at a group's / task's history: only bubbles whose TaskId() is
    // in |allowed| are visible. nullptr = no filter. |label| names the selection
    // for the "nothing here" hint.
    void SetFilter(std::shared_ptr<const std::set<std::string>> allowed, std::string label = "");

    // The user input each task was created from (task id -> raw input), as the task
    // snapshot reports it. Under a filter, a task's input is prepended to its
    // history (the input's own bubble belongs to no task, so the filter hides it).
    void SetTaskInputs(std::shared_ptr<const std::map<std::string, std::string>> inputs);

    std::shared_ptr<const std::set<std::string>> filter() const
    {
        return filter_;
    }
    const std::string &filter_label() const
    {
        return filter_label_;
    }
    bool has_filter() const
    {
        return filter_ != nullptr;
    }

    // The drawn sequence: every message passing the filter, with each task's input
    // prepended. This is what a view renders.
    const std::vector<std::shared_ptr<Message>> &visible() const
    {
        return visible_;
    }
    bool empty() const
    {
        return visible_.empty();
    }

  private:
    bool Visible(const Message &message) const;
    // Rebuild visible_ from all_ under the current filter (the old ApplyFilter).
    void Rebuild();
    void Notify(const Change &change);

    std::vector<std::shared_ptr<Message>> all_;
    std::vector<std::shared_ptr<Message>> visible_;
    std::shared_ptr<const std::map<std::string, std::string>> task_inputs_;
    std::set<std::string> input_shown_;
    std::shared_ptr<const std::set<std::string>> filter_;
    std::string filter_label_;

    std::map<int, Listener> listeners_;
    int next_id_ = 0;
};

} // namespace edc::model
