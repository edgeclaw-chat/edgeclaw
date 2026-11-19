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

#include "edc_chat_window.hpp"
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
    };
    using Listener = std::function<void(const Change &)>;

    int Subscribe(Listener listener);
    void Unsubscribe(int id);

    // Append one bubble. Recorded whether or not it passes the filter; the view
    // only learns about it when it is visible.
    void AddMessage(std::shared_ptr<Message> message);

    // Drop every message (the log was re-pointed at another session).
    void Clear();

    /* Point the log at a transcript file: build the descriptor index once and
     * fill the log with LazyBubble proxies. This is the ONLY history path. */
    void LoadTranscript(const std::string &path);

    /* Replace the log with one chat_bubbles page (startup / session switch).
     * |payload| is the CHAT_BUBBLE_PAGE reply JSON read on the actor thread. */
    bool LoadFromPage(const std::string &session_id, const std::string &payload);

    /* Prepend an older page (scrolling to the top). Returns false when the
     * payload belongs to another session or there is nothing more to add. */
    bool ApplyOlderPage(const std::string &session_id, const std::string &payload);

    /* A whole-model snapshot, used while the history view temporarily replaces
     * the chat pane. The history view used to be undone by replaying the
     * transcript file; with the DB as the authority that replay would drop the
     * live bubbles that arrived before the view was opened, so the caller now
     * saves/restores the model itself. */
    struct Snapshot
    {
        std::unique_ptr<std::vector<ChatBubbleRef>> refs;
        std::unique_ptr<MessageWindow> window;
        std::vector<std::shared_ptr<Message>> all;
        std::vector<std::shared_ptr<Message>> visible;
        std::string session_id;
        int64_t oldest_seq = 0;
        bool has_more = false;
    };
    Snapshot TakeSnapshot();
    void RestoreSnapshot(Snapshot snap);

    /* The seq of the oldest loaded row, and whether an older page exists. */
    int64_t oldest_seq() const;
    bool has_more() const { return has_more_; }
    const std::string &session_id() const { return session_id_; }

    /* Drive the resident window from the list's visible index range. */
    void SetWindow(size_t first, size_t last, size_t prefetch);

    /* Resident REAL Message count (proxies do not count). */
    size_t LiveCount() const;

    /* How many proxies still pin a cached ftxui element tree. Read-only
     * probe for the regression test / diagnostics. */
    size_t CachedTreeCount() const;
    size_t size() const
    {
        return all_.size();
    }

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

    /* Owns the descriptor vector and the ONLY real Message instances. */
    std::unique_ptr<std::vector<ChatBubbleRef>> refs_holder_;
    std::unique_ptr<MessageWindow> window_;

    std::vector<std::shared_ptr<Message>> all_;   /* LazyBubble proxies */
    std::vector<std::shared_ptr<Message>> visible_;
    std::shared_ptr<const std::map<std::string, std::string>> task_inputs_;
    std::set<std::string> input_shown_;
    std::shared_ptr<const std::set<std::string>> filter_;
    std::string filter_label_;

    /* Paged log state (chat_bubbles). Kept in the model, never in the UI. */
    std::string session_id_;
    int64_t oldest_seq_ = 0;
    bool has_more_ = false;

    std::map<int, Listener> listeners_;
    int next_id_ = 0;
};

} // namespace edc::model
