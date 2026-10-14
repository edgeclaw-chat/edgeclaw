#pragma once

// Control layer: the channel event dispatcher (ChannelDispatcher).
//
// P2 moved the C boundary (classify + copy fields + marshalling) into AppFacade,
// but "what to decide and update once a typed event arrives" still lived in
// TUIWindow::On{ChatMessage,TaskSnapshot,TreeChanged,TaskHistory}. P3 moves it
// here:
//   * the dispatcher only does three things -- DECIDE with policies (whose tree may
//     be shown / where output goes), UPDATE the models (ChatModel / SessionModel),
//     and ask the host for VIEW ACTIONS (transcript replay/writes, sidebar refresh,
//     animation, repaint, tree pull);
//   * the host is a narrow interface (IChannelHost), implemented by TUIWindow. The
//     dispatcher therefore does not know ftxui or the concrete window and can be
//     unit-tested without a terminal (feed events -> assert the models).

#include "edc_app_facade.hpp" /* port::IChannelSink / ChatMessageEvent */
#include "edc_chat_model.hpp"
#include "edc_domain.hpp"
#include "edc_session_model.hpp"
#include "edc_task_snapshot.hpp"

#include <string>

namespace edc::control
{

// View actions the dispatcher asks the window to do (outside the models).
// Implemented by TUIWindow.
class IChannelHost
{
  public:
    virtual ~IChannelHost() = default;

    // In history-only mode live output is not shown (command replies excepted).
    virtual bool InHistoryView() const = 0;

    // Persist + show one live output (the displayed session), or write only into
    // its own session's transcript (a background session). true = it appeared on
    // the current tab (the caller then decides to pull the tree / repaint).
    virtual bool PresentChat(const port::ChatMessageEvent &event, const std::string &kind_str) = 0;

    // Switch the displayed session and replay its transcript (updates SessionModel
    // + ChatModel).
    virtual void LoadSessionTranscript(const std::string &session_id) = 0;

    // Write a tree into the sidebar (TaskWidget).
    virtual void SetTaskTree(const TaskSnapshot &snap) = 0;
    // Let the tree decide whether the foreground task is running (working animation).
    virtual void SyncWorking(const TaskSnapshot &snap) = 0;
    // Let the tree decide the foreground active task's todo list (TodoWidget).
    virtual void SetTodos(const TaskSnapshot &snap) = 0;
    // Structured `/task history` payload -> history view.
    virtual void PresentHistory(const std::string &payload) = 0;

    virtual void RequestTaskSnapshot() = 0;
    virtual void RequestFrame() = 0;
};

// Channel event dispatcher: AppFacade's sink.
class ChannelDispatcher : public port::IChannelSink
{
  public:
    // |claim| / |route| are not owned; the defaults are the process-wide stateless
    // implementations.
    ChannelDispatcher(model::ChatModel &chat, model::SessionModel &session, IChannelHost &host,
                      const domain::SnapshotClaimPolicy &claim = domain::SnapshotClaimPolicy::Default(),
                      const domain::OutputRoutingPolicy &route = domain::OutputRoutingPolicy::Default());

    void OnChatMessage(const port::ChatMessageEvent &event) override;
    void OnTaskSnapshot(const std::string &json) override;
    void OnTreeChanged(const std::string &session_id, const std::string &reason) override;
    void OnTaskHistory(const std::string &payload) override;

  private:
    model::ChatModel &chat_;
    model::SessionModel &session_;
    IChannelHost &host_;
    const domain::SnapshotClaimPolicy &claim_;
    const domain::OutputRoutingPolicy &route_;
};

} // namespace edc::control
