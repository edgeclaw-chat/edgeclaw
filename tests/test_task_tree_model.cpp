// Tests for the sidebar model's finish detection (edc_task_tree_model.*).
//
// "A task finished" is not an event the kernel sends: every state change already
// reaches the frontend as a fresh task tree, so the model compares the new state with
// the one it saw last time. These tests pin what counts as a finish worth announcing
// and what does not:
//
//   - only the transition from a live state into COMPLETED / CANCELLED / ERROR;
//   - never the first sighting of a task (opening a session must not replay history);
//   - never INTERRUPTED (that lands on every unfinished task at shutdown);
//   - never a system group's task (fold / curation are internal);
//   - never the active task (its reply is already in the chat pane);
//   - a finish is reported once, and the unread mark survives later refreshes.
#include "edc_task_snapshot.hpp"
#include "edc_task_tree_model.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace
{

int g_failures = 0;

void Check(bool ok, const std::string &what)
{
    if (!ok)
    {
        std::cerr << "FAIL: " << what << "\n";
        g_failures++;
    }
    else
    {
        std::cout << "  ok: " << what << "\n";
    }
}

edc::TaskInfo Task(const std::string &id, const std::string &state, bool active = false)
{
    edc::TaskInfo t;
    t.task_id = id;
    t.state = state;
    t.is_active = active;
    return t;
}

/* One group holding |tasks|, kind User unless asked otherwise. */
edc::TaskSnapshot Snap(edc::GroupKind kind, const std::vector<edc::TaskInfo> &tasks)
{
    edc::TaskSnapshot s;
    s.session_id = "sess-1";
    s.session_name = "Unit";
    edc::TaskGroup g;
    g.group_id = "1";
    g.kind = kind;
    g.tasks = tasks;
    g.member_count = (int)tasks.size();
    s.groups.push_back(g);
    return s;
}

bool SameList(const std::vector<std::string> &got, const std::vector<std::string> &want)
{
    if (got.size() != want.size())
        return false;
    for (size_t i = 0; i < got.size(); i++)
        if (got[i] != want[i])
            return false;
    return true;
}

} // namespace

int main()
{
    using edc::GroupKind;
    using edc::model::TaskTreeModel;

    {
        std::cout << "live -> COMPLETED is a finish, reported once\n";
        TaskTreeModel m;

        edc::TaskSnapshot running = Snap(GroupKind::User, {Task("3", "LLM_PROCESSING")});
        m.Apply(&running);
        Check(m.TakeFinished().empty(), "nothing finished while it was running");
        Check(!m.IsUnread("3"), "and it is not unread");

        edc::TaskSnapshot done = Snap(GroupKind::User, {Task("3", "COMPLETED")});
        m.Apply(&done);
        Check(SameList(m.TakeFinished(), {"3"}), "the finish is reported");
        Check(m.TakeFinished().empty(), "and only once");
        Check(m.IsUnread("3"), "it is marked unread");

        /* A later refresh with the same state must not repeat it, nor drop the mark. */
        m.Apply(&done);
        Check(m.TakeFinished().empty(), "a refresh with the same state reports nothing");
        Check(m.IsUnread("3"), "the unread mark survives the refresh");

        m.MarkRead("3");
        Check(!m.IsUnread("3"), "MarkRead drops it");
    }

    {
        std::cout << "the first sighting of a finished task is not a finish\n";
        TaskTreeModel m;
        edc::TaskSnapshot already = Snap(GroupKind::User, {Task("7", "COMPLETED")});
        m.Apply(&already);
        Check(m.TakeFinished().empty(), "opening a session does not replay its history");
        Check(!m.IsUnread("7"), "and nothing is marked unread");
    }

    {
        std::cout << "the active task's finish is not announced\n";
        TaskTreeModel m;
        edc::TaskSnapshot running = Snap(GroupKind::User, {Task("5", "TOOL_CALLING", true)});
        m.Apply(&running);
        edc::TaskSnapshot done = Snap(GroupKind::User, {Task("5", "COMPLETED", true)});
        m.Apply(&done);
        Check(m.TakeFinished().empty(), "its reply is already in the chat pane");
        Check(!m.IsUnread("5"), "so it is not unread either");
    }

    {
        std::cout << "a system group's task is not announced\n";
        TaskTreeModel m;
        edc::TaskSnapshot running = Snap(GroupKind::System, {Task("9", "LLM_PROCESSING")});
        m.Apply(&running);
        edc::TaskSnapshot done = Snap(GroupKind::System, {Task("9", "COMPLETED")});
        m.Apply(&done);
        Check(m.TakeFinished().empty(), "fold / curation are internal work");
        Check(!m.IsUnread("9"), "and never marked");
    }

    {
        std::cout << "CANCELLED and ERROR are announced; INTERRUPTED is not\n";
        TaskTreeModel m;
        edc::TaskSnapshot running =
            Snap(GroupKind::User, {Task("a", "LLM_PROCESSING"), Task("b", "LLM_PROCESSING"),
                                   Task("c", "LLM_PROCESSING")});
        m.Apply(&running);
        edc::TaskSnapshot mixed =
            Snap(GroupKind::User, {Task("a", "CANCELLED"), Task("b", "ERROR"), Task("c", "INTERRUPTED")});
        m.Apply(&mixed);
        Check(SameList(m.TakeFinished(), {"a", "b"}), "the stopped and the failed task, in tree order");
        Check(m.IsUnread("a") && m.IsUnread("b"), "both marked unread");
        Check(!m.IsUnread("c"), "the interrupted one is left for the startup notice");
    }

    {
        std::cout << "resume re-arms the task\n";
        TaskTreeModel m;
        edc::TaskSnapshot live = Snap(GroupKind::User, {Task("3", "LLM_PROCESSING")});
        m.Apply(&live);
        edc::TaskSnapshot done = Snap(GroupKind::User, {Task("3", "COMPLETED")});
        m.Apply(&done);
        Check(SameList(m.TakeFinished(), {"3"}), "the first finish is reported");
        m.MarkRead("3");

        m.Apply(&live); /* /task resume: back to live */
        Check(m.TakeFinished().empty(), "going live is not a finish");
        m.Apply(&done); /* and finished again */
        Check(SameList(m.TakeFinished(), {"3"}), "the second finish is reported again");
        Check(m.IsUnread("3"), "and marked unread again");
    }

    if (g_failures == 0)
        std::cout << "\nall model checks passed\n";
    else
        std::cerr << "\n" << g_failures << " failure(s)\n";
    return g_failures == 0 ? 0 : 1;
}
