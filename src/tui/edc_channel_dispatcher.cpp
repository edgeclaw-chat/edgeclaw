#include "edc_channel_dispatcher.hpp"

#include "edc_token_style.hpp"

#include <map>
#include <memory>
#include <utility>

namespace edc::control
{

namespace
{

// TOOL message header of the todowrite tool (matches edc_tools_todo.c). Its
// Markdown-checklist echo stays out of the chat: TODO state is shown only by the
// sidebar TodoWidget.
constexpr char kTodoToolHeader[] = "todowrite";

// Which input built each task (task id -> raw input), handed to ChatModel: when the
// task is selected it renders as the task's first user message.
std::shared_ptr<const std::map<std::string, std::string>> TaskInputsOf(const TaskSnapshot &snap)
{
    auto inputs = std::make_shared<std::map<std::string, std::string>>();
    for (const TaskGroup &g : snap.groups)
        for (const TaskInfo &t : g.tasks)
            if (!t.input.empty())
            {
                /* `input` is the expanded text (what the model saw); the bubble shows the
                 * line the user typed, so a folded paste goes back to its placeholder. */
                (*inputs)[t.task_id] = FoldPastedText(t.input, t.payload);
            }
    return inputs;
}

// Typed enum -> persisted tag (identical to the old string contract: these are what
// the transcript file stores).
const char *TagOf(port::ChatKind kind)
{
    switch (kind)
    {
    case port::ChatKind::User:
        return "user";
    case port::ChatKind::Tool:
        return "tool";
    case port::ChatKind::Thought:
        return "thought";
    case port::ChatKind::Reply:
        return "reply";
    case port::ChatKind::System:
        break;
    }
    return "sys";
}

} // namespace

ChannelDispatcher::ChannelDispatcher(model::ChatModel &chat, model::SessionModel &session, IChannelHost &host,
                                     const domain::SnapshotClaimPolicy &claim,
                                     const domain::OutputRoutingPolicy &route)
    : chat_(chat), session_(session), host_(host), claim_(claim), route_(route)
{
}

void ChannelDispatcher::OnMessagePayload(const std::string &mid, const std::string &payload_json)
{
    host_.PresentMessagePayload(mid, payload_json);
}

void ChannelDispatcher::OnTaskHistory(const std::string &payload)
{
    // Structured history payload: split into bubbles appended to the chat pane.
    host_.PresentHistory(payload);
}

void ChannelDispatcher::OnTreeChanged(const std::string &session_id, const std::string &reason)
{
    // A tree-changed event (P3 ①b) is a control message: "session X's tree
    // changed", no tree attached. The frontend decides what to do:
    //   - reason "active"  → the user switched tabs: adopt X as displayed (its
    //     transcript), then pull X's tree;
    //   - anything else    → pull **only** if X is the tree on screen.
    if (!claim_.WantsPull(session_id, reason, session_.displayed_id()))
        return;
    if (reason == "active" && session_id != session_.displayed_id())
        host_.LoadSessionTranscript(session_id);
    host_.RequestTaskSnapshot();
}

void ChannelDispatcher::OnTaskSnapshot(const std::string &json)
{
    TaskSnapshot snap;
    // A late reply must claim its own request: if the session changed while it
    // was in flight (the user switched tabs), this tree is yesterday's news —
    // drop it. The displayed session is the frontend's own state (SessionModel);
    // the kernel never tells it what to display.
    if (!ParseTaskSnapshot(json, &snap) || !claim_.Claims(snap, session_.displayed_id()))
        return;

    /* First reply after startup: adopt the session it names (and that session's
     * transcript) as the one we are showing. This is how the frontend learns
     * which session it displays -- no synchronous read of the store at bootstrap. */
    if (!session_.adopted())
        host_.LoadSessionTranscript(snap.session_id);
    /* The footer's working directory comes from here, so it never reaches into the
     * store (which the actor thread mutates). */
    session_.set_workdir(snap.workdir);

    // Give the chat log what each task was created from (drawn as the first user
    // message of a selected task's history).
    chat_.SetTaskInputs(TaskInputsOf(snap));

    host_.SetTaskTree(snap);
    // The snapshot is the single source of truth for the status animation.
    host_.SyncWorking(snap);
    // ... and for the todo section (the foreground group's active task's list).
    host_.SetTodos(snap);

    // FTXUI only repaints when the frame was invalidated; a posted task does not
    // invalidate it, so ask for exactly one frame.
    host_.RequestFrame();
}

void ChannelDispatcher::OnChatMessage(const port::ChatMessageEvent &event)
{
    // History view: live output is neither shown nor written to the transcript.
    // Command replies (System) are the exception -- typing a command must give
    // feedback.
    if (host_.InHistoryView() && event.kind != port::ChatKind::System)
        return;

    // Display gate (moved from core to the frontend): the displayed session shows
    // only its active task's output -- a non-active task's output is silent.
    if (route_.Route(event.session_id, event.from_active_task, session_.displayed_id()) ==
        domain::OutputRoute::Silent)
        return;

    // The todowrite TOOL message stays out of the chat: TODO state is shown by the
    // sidebar TodoWidget.
    if (event.kind == port::ChatKind::Tool && event.header == kTodoToolHeader)
        return;

    const std::string kind_str = TagOf(event.kind);

    // PresentChat persists + shows; a background session's output is written only
    // into its own transcript, does not bubble into the current tab, and needs no
    // tree pull / repaint.
    if (!host_.PresentChat(event, kind_str))
        return;

    // A message implies task state may have changed; refresh the sidebar, and ask
    // for a frame so the new bubble paints immediately while the TUI is idle.
    host_.RequestTaskSnapshot();
    host_.RequestFrame();
}

} // namespace edc::control
