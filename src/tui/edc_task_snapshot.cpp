#include "edc_task_snapshot.hpp"

#include <cJSON.h>

/* Moved from the C bridge's edc_c_task_snapshot_parse(): same JSON, same
 * per-field degradation, but the result is a C++ value (std::string /
 * std::vector) instead of a malloc'd tree that the caller had to free. */

namespace edc
{
namespace
{

const cJSON *Field(const cJSON *object, const char *name)
{
    return cJSON_GetObjectItemCaseSensitive(object, name);
}

/* Non-string (or absent) -> "": a snapshot with a missing key must not make the
 * whole tree unreadable. */
std::string JsonStr(const cJSON *item)
{
    return cJSON_IsString(item) ? std::string(item->valuestring) : std::string();
}

/* cJSON has no integer type: counts/token totals arrive as doubles and are
 * narrowed back here (they stay far below 2^53). */
long long JsonInt64(const cJSON *item)
{
    return cJSON_IsNumber(item) ? static_cast<long long>(item->valuedouble) : 0;
}

int JsonInt(const cJSON *item)
{
    return cJSON_IsNumber(item) ? item->valueint : 0;
}

bool JsonBool(const cJSON *item)
{
    return cJSON_IsTrue(item) != 0;
}

TodoItem ParseTodo(const cJSON *item)
{
    TodoItem todo;
    todo.content = JsonStr(Field(item, "content"));
    todo.status = JsonStr(Field(item, "status"));
    todo.priority = JsonStr(Field(item, "priority"));
    return todo;
}

TaskInfo ParseTask(const cJSON *item)
{
    TaskInfo task;
    task.task_id = JsonStr(Field(item, "id"));
    task.state = JsonStr(Field(item, "state"));
    task.end = JsonStr(Field(item, "end")); /* "" from an older producer = not over */
    task.is_leader = JsonBool(Field(item, "leader"));
    task.is_active = JsonBool(Field(item, "active"));
    task.description = JsonStr(Field(item, "desc"));
    task.input = JsonStr(Field(item, "input"));
    task.tokens_prompt = JsonInt64(Field(item, "tokens_prompt"));
    task.tokens_completion = JsonInt64(Field(item, "tokens_completion"));
    task.tokens_total = JsonInt64(Field(item, "tokens_total"));
    task.tokens_cached = JsonInt64(Field(item, "tokens_cached"));

    if (const cJSON *todos = Field(item, "todos"); cJSON_IsArray(todos))
    {
        task.todos.reserve(static_cast<size_t>(cJSON_GetArraySize(todos)));
        const cJSON *todo = nullptr;
        cJSON_ArrayForEach(todo, todos)
        task.todos.push_back(ParseTodo(todo));
    }
    return task;
}

TaskGroup ParseGroup(const cJSON *item)
{
    TaskGroup group;
    group.group_id = JsonStr(Field(item, "id"));
    /* Only "system" is recognized; anything else (including a missing key from
     * an older producer) degrades to User: the display layer treats an unknown
     * kind as "draw it as usual" and never hides anything. */
    group.kind = JsonStr(Field(item, "kind")) == "system" ? GroupKind::System : GroupKind::User;
    group.member_count = JsonInt(Field(item, "member_count"));
    group.msg_count = JsonInt(Field(item, "msg_count"));
    group.orphaned = JsonBool(Field(item, "orphaned"));
    group.fg = JsonBool(Field(item, "fg"));
    group.tokens_total = JsonInt64(Field(item, "tokens_total"));
    group.tokens_prompt = JsonInt64(Field(item, "tokens_prompt"));
    group.tokens_completion = JsonInt64(Field(item, "tokens_completion"));
    group.tokens_cached = JsonInt64(Field(item, "tokens_cached"));
    group.ctx_tokens = JsonInt64(Field(item, "ctx_tokens"));
    group.ctx_estimated = JsonBool(Field(item, "ctx_estimated"));

    if (const cJSON *tasks = Field(item, "tasks"); cJSON_IsArray(tasks))
    {
        group.tasks.reserve(static_cast<size_t>(cJSON_GetArraySize(tasks)));
        const cJSON *task = nullptr;
        cJSON_ArrayForEach(task, tasks)
        group.tasks.push_back(ParseTask(task));
    }
    return group;
}

} // namespace

bool ParseTaskSnapshot(const std::string &json, TaskSnapshot *out)
{
    if (!out)
        return false;
    *out = TaskSnapshot{};
    if (json.empty())
        return false;

    cJSON *root = cJSON_Parse(json.c_str());
    if (!root)
        return false;

    out->session_id = JsonStr(Field(root, "session_id"));
    out->session_name = JsonStr(Field(root, "session_name"));
    out->workdir = JsonStr(Field(root, "workdir"));
    out->message_count = JsonInt(Field(root, "message_count"));
    out->created_at = JsonStr(Field(root, "created_at"));

    if (const cJSON *groups = Field(root, "groups"); cJSON_IsArray(groups))
    {
        out->groups.reserve(static_cast<size_t>(cJSON_GetArraySize(groups)));
        const cJSON *group = nullptr;
        cJSON_ArrayForEach(group, groups)
        out->groups.push_back(ParseGroup(group));
    }

    cJSON_Delete(root);
    return true;
}

} // namespace edc
