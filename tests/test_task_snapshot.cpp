// Tests for edc::ParseTaskSnapshot -- the TUI's view model of the scheduler's
// task-tree snapshot JSON (edc_task_snapshot.hpp).
//
// This parser used to be C code inside the TUI's C bridge (edc_c_task_snapshot_parse,
// which produced a malloc'd tree the caller had to free). It is now a plain C++
// function in the TUI, so it is unit-testable for the first time.
//
// The JSON below is shaped exactly like the producer's output
// (edc_scheduler.c): session_id / session_name / message_count / created_at /
// groups -> id / kind / member_count / msg_count / orphaned / fg / tokens_total
// / tasks -> id / state / leader / active / desc / tokens_* / todos.
#include "edc_task_snapshot.hpp"
#include "edc_task_widget.hpp"
#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/screen.hpp"
#include <iostream>
#include <string>

namespace
{

int g_failures = 0;

void Check(bool ok, const std::string &what)
{
    if (ok)
    {
        std::cout << "  ok   " << what << "\n";
        return;
    }
    std::cout << "  FAIL " << what << "\n";
    g_failures++;
}

/* Indexing helpers that cannot crash: a mutation in the parser must make the
 * assertions FAIL, not abort the process on an out-of-range element. */
template <typename T> const T &At(const std::vector<T> &v, size_t i)
{
    static const T kEmpty{};
    return i < v.size() ? v[i] : kEmpty;
}

const char *kFull = R"json({
  "session_id": "s7",
  "session_name": "work",
  "message_count": 42,
  "created_at": "2026-09-16 05:00:00",
  "groups": [
    {
      "id": "9", "kind": "system", "member_count": 1, "msg_count": 2,
      "orphaned": false, "fg": false, "tokens_total": 30,
      "tasks": [
        {
          "id": "777", "state": "LLM_PROCESSING", "leader": true, "active": false,
          "desc": "fold group 1", "tokens_prompt": 10, "tokens_completion": 20,
          "tokens_total": 30, "tokens_cached": 5
        }
      ]
    },
    {
      "id": "1", "kind": "user", "member_count": 2, "msg_count": 7,
      "orphaned": true, "fg": true, "tokens_total": 100,
      "tasks": [
        {
          "id": "11", "state": "IDLE", "leader": true, "active": true,
          "desc": "user work", "tokens_prompt": 1, "tokens_completion": 2,
          "tokens_total": 3, "tokens_cached": 4,
          "todos": [
            {"content": "one", "status": "pending", "priority": "high"},
            {"content": "two", "status": "completed", "priority": ""}
          ]
        },
        {
          "id": "12", "state": "COMPLETED", "leader": false, "active": false,
          "desc": "", "tokens_prompt": 0, "tokens_completion": 0,
          "tokens_total": 0, "tokens_cached": 0
        }
      ]
    }
  ]
})json";

void TestParseFullSnapshot()
{
    std::cout << "a full snapshot round-trips\n";

    edc::TaskSnapshot snap;
    Check(edc::ParseTaskSnapshot(kFull, &snap), "parses");
    Check(snap.session_id == "s7", "session_id");
    Check(snap.session_name == "work", "session_name");
    Check(snap.message_count == 42, "message_count");
    Check(snap.created_at == "2026-09-16 05:00:00", "created_at");
    Check(snap.groups.size() == 2, "two groups, in snapshot order");

    const edc::TaskGroup &sys = At(snap.groups, 0);
    Check(sys.kind == edc::GroupKind::System, "the system group is marked as such");
    Check(sys.msg_count == 2 && sys.member_count == 1 && sys.tokens_total == 30, "group counters");
    Check(!sys.fg && !sys.orphaned, "group flags");
    Check(sys.tasks.size() == 1 && At(sys.tasks, 0).todos.empty(), "a task with no todo list");

    const edc::TaskGroup &user = At(snap.groups, 1);
    Check(user.kind == edc::GroupKind::User, "user group kind");
    Check(user.fg && user.orphaned, "fg / orphaned flags");
    Check(user.tasks.size() == 2, "both member tasks");

    const edc::TaskInfo &active = At(user.tasks, 0);
    Check(active.task_id == "11" && active.state == "IDLE", "task id + state");
    Check(active.is_leader && active.is_active, "leader / active flags");
    Check(active.description == "user work", "description");
    Check(active.tokens_prompt == 1 && active.tokens_completion == 2 && active.tokens_total == 3 &&
              active.tokens_cached == 4,
          "token counters");
    Check(active.todos.size() == 2, "todo items");
    Check(At(active.todos, 0).content == "one" && At(active.todos, 0).status == "pending" &&
              At(active.todos, 0).priority == "high",
          "first todo item");
    Check(At(active.todos, 1).status == "completed" && At(active.todos, 1).priority.empty(),
          "second todo item (empty priority stays empty)");

    Check(!At(user.tasks, 1).is_leader && !At(user.tasks, 1).is_active, "the second task is neither leader nor active");
}

void TestMissingFieldsDegrade()
{
    std::cout << "missing / unknown fields degrade per field\n";

    edc::TaskSnapshot snap;
    // An older producer may omit `kind`, `todos`, counters, even everything but
    // the id: the display layer must still draw the tree.
    const char *minimal = R"json({"groups":[{"id":"1"},{"id":"2","kind":"weird"}]})json";
    Check(edc::ParseTaskSnapshot(minimal, &snap), "parses a minimal snapshot");
    Check(snap.session_id.empty() && snap.session_name.empty(), "absent root strings -> empty");
    Check(snap.message_count == 0, "absent message_count -> 0");
    Check(snap.groups.size() == 2, "groups still there");
    Check(At(snap.groups, 0).group_id == "1" && At(snap.groups, 0).kind == edc::GroupKind::User,
          "absent kind -> User");
    Check(At(snap.groups, 1).kind == edc::GroupKind::User, "unknown kind -> User");
    Check(At(snap.groups, 0).tasks.empty() && At(snap.groups, 0).msg_count == 0, "absent arrays/counters -> empty/0");
    Check(!At(snap.groups, 0).fg && !At(snap.groups, 0).orphaned, "absent flags -> false");

    // Right-shaped but ill-typed values (a string where a number belongs) must
    // not poison the rest of the parse.
    const char *illTyped = R"json({"message_count":"many","groups":[{"id":7,"msg_count":"x"}]})json";
    Check(edc::ParseTaskSnapshot(illTyped, &snap), "parses ill-typed values");
    Check(snap.message_count == 0, "ill-typed number -> 0");
    Check(snap.groups.size() == 1 && At(snap.groups, 0).group_id.empty(), "ill-typed string -> empty");
    Check(At(snap.groups, 0).msg_count == 0, "ill-typed group counter -> 0");
}

void TestRejectsGarbage()
{
    std::cout << "garbage is rejected (and never half-applied)\n";

    edc::TaskSnapshot snap;
    snap.groups.resize(1); /* must be cleared when the parse fails */
    Check(!edc::ParseTaskSnapshot("", &snap), "empty input -> false");
    Check(snap.groups.empty(), "a failed parse leaves no groups behind");

    snap.groups.resize(1);
    Check(!edc::ParseTaskSnapshot("{not json", &snap), "malformed JSON -> false");
    Check(snap.groups.empty(), "and none are left behind");

    Check(!edc::ParseTaskSnapshot("null", &snap) || snap.groups.empty(), "a JSON null is not a tree");
}

/* The whole path the sidebar actually runs: JSON -> parse -> widget -> frame.
 * The frame is what the user sees, and A/B never compares frames (see
 * tools/ab/README.md), so this is the only net under this path -- it is why the
 * parse lives in a unit-testable C++ module instead of behind the C bridge. */
void TestParsedSnapshotRenders()
{
    std::cout << "a parsed snapshot reaches the frame\n";

    edc::TaskSnapshot snap;
    Check(edc::ParseTaskSnapshot(kFull, &snap), "parses (render case)");

    edc::TaskWidget widget;
    widget.SetSnapshot(&snap);

    // The widget sizes its rows from the box a previous render measured, so the
    // first pass only primes it (same two-pass scheme as test_task_widget).
    constexpr int kW = 40;
    constexpr int kH = 20;
    auto make_screen = [] {
        return ::ftxui::Screen::Create(::ftxui::Dimension::Fixed(kW), ::ftxui::Dimension::Fixed(kH));
    };
    ::ftxui::Screen screen = make_screen();
    for (int pass = 0; pass < 2; pass++)
    {
        screen = make_screen();
        ::ftxui::Render(screen, widget.Render() | ::ftxui::flex);
    }

    std::string frame;
    for (int y = 0; y < kH; y++)
    {
        for (int x = 0; x < kW; x++)
        {
            const std::string &cell = screen.CellAt(x, y).character;
            frame += cell.empty() ? " " : cell;
        }
        frame += "\n";
    }

    Check(frame.find("Group 1") != std::string::npos, "the user group's header is drawn");
    Check(frame.find("Group 9") != std::string::npos, "the system group's header is drawn");
    Check(frame.find("11") != std::string::npos, "task 11's row is drawn");
    Check(frame.find("777") != std::string::npos, "the system task's row is drawn");
    Check(frame.find("2 groups") != std::string::npos && frame.find("3 tasks") != std::string::npos,
          "the header counts both groups and all three tasks");
}

} // namespace

int main()
{
    TestParseFullSnapshot();
    TestMissingFieldsDegrade();
    TestRejectsGarbage();
    TestParsedSnapshotRenders();

    if (g_failures == 0)
    {
        std::cout << "\nall passed\n";
        return 0;
    }
    std::cout << "\n" << g_failures << " FAILED\n";
    return 1;
}
