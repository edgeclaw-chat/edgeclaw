#pragma once

#include <string>
#include <vector>

/* Task-tree snapshot: the *frontend's own view model*, moved here from the C
 * bridge (2026-09-16). Its shape is dictated by the two widgets that render it
 * (sidebar tree, todo list) -- it is a projection of the scheduler's JSON, not
 * a kernel concept. It is C++ because cJSON compiles as C++ (unlike sds.h,
 * whose SDS_HDR_VAR macro does not); the project has no other JSON library.
 *
 * The JSON producer is edc_scheduler.c -- the keys below mirror its field names
 * one for one, so changing either side means changing both. */

namespace edc
{

/* Mirrors the kernel's edc_group_kind: the display layer's only hint that a
 * group is a system one (drawn as its own block in the sidebar). The kernel
 * hides nothing based on it -- system tasks appear in the snapshot like any
 * other. */
enum class GroupKind
{
    User = 0,
    System = 1,
};

struct TodoItem
{
    std::string content;
    std::string status;   /* pending | in_progress | completed | cancelled */
    std::string priority; /* high | medium | low ("" when unset) */
};

struct TaskInfo
{
    std::string task_id;
    std::string state; /* scheduler state name, e.g. "COMPLETED" */
    bool is_leader = false;
    bool is_active = false;
    std::string description;
    std::vector<TodoItem> todos; /* the todo list this task holds (the todowrite items) */
    /* Accumulated LLM token usage for this task (0 when none reported yet). */
    long long tokens_prompt = 0;
    long long tokens_completion = 0;
    long long tokens_total = 0;
    long long tokens_cached = 0;
};

struct TaskGroup
{
    std::string group_id;
    GroupKind kind = GroupKind::User;
    int member_count = 0;
    int msg_count = 0; /* committed messages (no system prompt) + in-flight turns */
    bool orphaned = false;
    bool fg = false; /* this is the session's foreground group */
    long long tokens_total = 0; /* sum of member tasks' total tokens */
    std::vector<TaskInfo> tasks;
};

struct TaskSnapshot
{
    std::string session_id;
    std::string session_name;
    int message_count = 0;  /* messages in the session history */
    std::string created_at; /* "YYYY-MM-DD HH:MM:SS" or "" */
    std::vector<TaskGroup> groups;
};

/* Parse the scheduler's task-tree snapshot JSON into |*out|.
 * Returns false (and leaves |*out| cleared) when |json| is empty or malformed;
 * the caller then drops the snapshot -- a late reply must not repaint anything
 * (see TUIWindow's task-snapshot handler).
 * Missing or ill-typed fields degrade per field (strings -> "", numbers -> 0,
 * bools -> false, an unknown group kind -> User): the display layer shows what
 * it got instead of hiding the tree. */
bool ParseTaskSnapshot(const std::string &json, TaskSnapshot *out);

} // namespace edc
