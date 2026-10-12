#include "edc_channel_dispatcher.hpp"

#include <map>
#include <memory>
#include <utility>

namespace edc::control
{

namespace
{

// todowrite 工具的 TOOL 消息头（与 edc_tools_todo.c 注册名一致）。它的文本回显
// （Markdown checklist）不进对话区：TODO 状态只由右侧 TodoWidget 展示。
constexpr char kTodoToolHeader[] = "todowrite";

// 每个 task 由哪条输入建成 (task id -> 原文)，喂给 ChatModel: 选中该 task 时它被
// 渲染成该 task 的第一条 user message。
std::shared_ptr<const std::map<std::string, std::string>> TaskInputsOf(const TaskSnapshot &snap)
{
    auto inputs = std::make_shared<std::map<std::string, std::string>>();
    for (const TaskGroup &g : snap.groups)
        for (const TaskInfo &t : g.tasks)
            if (!t.input.empty())
                (*inputs)[t.task_id] = t.input;
    return inputs;
}

// 强类型枚举 → 持久化 tag (与从前的字符串契约逐字一致: 转录文件里存的就是这些)。
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

void ChannelDispatcher::OnTaskHistory(const std::string &payload)
{
    // 结构化历史载荷: 拆成一条条气泡追加到对话区 (见 TUIWindow::RenderTaskHistory)。
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
    // 只看模式: 实时产出不上屏、也不写转录。命令回复 (System) 例外: 在只看模式里
    // 敲命令也要有反馈, 不然静默得像卡死了。
    if (host_.InHistoryView() && event.kind != port::ChatKind::System)
        return;

    // 展示闸门 (P3 ③: 从 core 移到前端): 显示中的 session 只显示它 **active task**
    // 的产出 —— 同 session 里非 active task 的输出静默。
    if (route_.Route(event.session_id, event.from_active_task, session_.displayed_id()) ==
        domain::OutputRoute::Silent)
        return;

    // todowrite 的 TOOL 消息不进对话区：TODO 状态已由右侧 TodoWidget 展示。
    if (event.kind == port::ChatKind::Tool && event.header == kTodoToolHeader)
        return;

    const std::string kind_str = TagOf(event.kind);

    // PresentChat 负责落盘 + 上屏; 后台上 session 的产出只落进它自己的转录,
    // 不在当前 tab 冒泡, 也不需要拉树/重绘。
    if (!host_.PresentChat(event, kind_str))
        return;

    // A message implies task state may have changed; refresh the sidebar, and ask
    // for a frame so the new bubble paints immediately while the TUI is idle.
    host_.RequestTaskSnapshot();
    host_.RequestFrame();
}

} // namespace edc::control
