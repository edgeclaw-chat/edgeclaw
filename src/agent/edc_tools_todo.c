#include "edc_tools_todo.h"
#include "edc_channel_actor.h"
#include "edc_compat.h"
#include "edc_param_types.h"
#include "edc_task.h"
#include "edc_toolbox.h"
#include "edc_tools.h"
#include "sds.h"
#include <stdlib.h>

/* 纯渲染"- [ ]/- [x]" 清单行 (无 "# TODO" 头、无统计尾巴) —— 与 render_md
 * 共用同一行格式源, 供 system prompt 注入方按需拼装。items 已在工具入口
 * 校验过 (status 合法、至多一个 in_progress)。对缺失字段做保守兜底 (缺省
 * 视为 pending / 空文本)，避免 agent 侧读取历史数据时因脏数据崩溃。
 * *out_completed (可为 NULL) 返回已完成条数。返回 sds，调用方负责 sdsfree。 */
sds edc_tools_todo_render_body(cJSON *items, int *out_completed)
{
    int total = items ? cJSON_GetArraySize(items) : 0;
    sds lines = sdsempty();
    int completed = 0;
    for (int i = 0; i < total; ++i)
    {
        cJSON *item = cJSON_GetArrayItem(items, i);
        char *status = cJSON_GetStringValue(cJSON_GetObjectItem(item, "status"));
        char *content = cJSON_GetStringValue(cJSON_GetObjectItem(item, "content"));
        char *activeForm = cJSON_GetStringValue(cJSON_GetObjectItem(item, "activeForm"));
        const char *st = status ? status : "pending";
        const char *ct = content ? content : "";

        if (strcmp(st, "completed") == 0)
        {
            lines = sdscatprintf(lines, "- [x] %s\n", ct);
            completed += 1;
        }
        else if (strcmp(st, "in_progress") == 0)
        {
            if (activeForm && *activeForm)
                lines = sdscatprintf(lines, "- [ ] %s <- %s\n", ct, activeForm);
            else
                lines = sdscatprintf(lines, "- [ ] %s\n", ct);
        }
        else
        {
            lines = sdscatprintf(lines, "- [ ] %s\n", ct);
        }
    }
    if (out_completed)
        *out_completed = completed;
    return lines;
}

/* 完整 checklist markdown: "# TODO\n" + 清单行 + "\nN/M completed"。
 * 工具回显 (update_todolist 的消息 content / tool result) 使用。 */
sds edc_tools_todo_render_md(cJSON *items)
{
    int completed = 0;
    sds body = edc_tools_todo_render_body(items, &completed);
    int total = items ? cJSON_GetArraySize(items) : 0;
    sds lines = sdscatprintf(sdsnew("# TODO\n"), "%s", body);
    lines = sdscatprintf(lines, "\n%d/%d completed", completed, total);
    sdsfree(body);
    return lines;
}

static void edc_tools_todo_exec_async(struct edc_toolbox_call_ctx *ctx, struct edc_tool *tool, cJSON *args,
                                      void (*callback)(struct edc_toolbox_call_ctx *, sds, EdcError *))
{
    cJSON *items = cJSON_GetObjectItem(args, "items");
    int total = items ? cJSON_GetArraySize(items) : 0;

    /* 无论清单是否为空都回显一条带 header 的 md TOOL 消息 (文本 = 渲染后的
     * checklist)。TUI 对 update_todolist 消息不进对话区 —— TODO 展示完全走
     * 任务快照 (见 scheduler: 每次回显推送一次快照, active task 的 todolist
     * 渲染到右侧 TodoWidget)；content 仍作为 tool result 回给 LLM，CLI 等
     * 无侧栏渲染端也照常显示。 */

    if (!total)
    {
        /* 空清单同样回显一次：TUI 借此推送快照把 TodoWidget 清成 0/0
         * (active task 的 todolist 已被摘除)。这里只发展示消息，callback
         * 结果仍是 "No todos"，LLM 看到的 tool result 与原来一致。 */
        sds empty = sdsnew("# TODO\n\n0/0 completed");
        edc_toolbox_emit_tool_message_md(ctx, "update_todolist", empty);
        sdsfree(empty);

        /* 清单被清空 → 该 task 不再持有 todo（后续请求不再向 system prompt 追加） */
        if (ctx->task && ctx->task->todolist)
        {
            cJSON_Delete(ctx->task->todolist);
            ctx->task->todolist = NULL;
        }
        callback(ctx, sdsnew("No todos"), NULL);
        return;
    }
    if (total > 20)
    {
        sds msg = sdscatprintf(sdsempty(), "Error: Maximum 20 items allowed, got %d.", total);
        EdcError *err = edc_error_new(EDC_ERR_INVAL, "%s", msg);
        callback(ctx, msg, err);
        return;
    }

    /* 校验 (渲染函数假设输入已合法) */
    int in_progress_count = 0;
    for (int i = 0; i < total; ++i)
    {
        cJSON *item = cJSON_GetArrayItem(items, i);
        char *status = cJSON_GetStringValue(cJSON_GetObjectItem(item, "status"));
        if (!status || (strcmp(status, "pending") && strcmp(status, "in_progress") && strcmp(status, "completed")))
        {
            sds msg = sdscatprintf(sdsempty(),
                                   "Error: invalid status '%s' at item %d. Must be pending, in_progress, or completed.",
                                   status ? status : "(null)", i + 1);
            EdcError *err = edc_error_new(EDC_ERR_INVAL, "%s", msg);
            callback(ctx, msg, err);
            return;
        }
        if (strcmp(status, "in_progress") == 0)
            in_progress_count += 1;
    }
    if (in_progress_count > 1)
    {
        sds msg =
            sdscatprintf(sdsempty(), "Error: Only ONE item can be in_progress at a time, got %d.", in_progress_count);
        EdcError *err = edc_error_new(EDC_ERR_INVAL, "%s", msg);
        callback(ctx, msg, err);
        return;
    }

    sds lines = edc_tools_todo_render_md(items);

    /* 把这份 todo 清单挂到调用它的 task 上 (整体替换)。todo list 是 task
     * 状态的一部分: task->todolist 已被 state_serializer 持久化；之后每次为
     * 该 task 组装 LLM 请求时 (见 agent send_llm)，若清单非空会把渲染 md
     * 追加到该 task 的 system prompt 尾部，让多轮工具循环中的模型始终能
     * 看到当前清单，而不依赖窗口内残留的 update_todolist tool 消息。 */
    if (ctx->task)
    {
        if (ctx->task->todolist)
            cJSON_Delete(ctx->task->todolist);
        ctx->task->todolist = cJSON_Duplicate(items, 1);
    }

    edc_toolbox_emit_tool_message_md(ctx, "update_todolist", lines);

    callback(ctx, lines, NULL);
}

void edc_tools_todo_register(struct edc_toolbox *tb)
{
    const char *name = "update_todolist";
    const char *desc =
        "Validate and update the todo list.\n\nThe model sends a complete new list each time. We validate it,\nstore "
        "it, and return a rendered view that the model will see.\n\nValidation Rules:\n- Each item must have: content, "
        "status, activeForm\n- Status must be: pending | in_progress | completed\n- Only ONE item can be in_progress "
        "at a time\n- Maximum 20 items allowed\n\nArgs:\n    items (list[Todo]): New todo items\n     - content (str): "
        "Task description\n     - status (str): One of \"pending\", \"in_progress\", \"completed\"\n     - activeForm "
        "(str): Current action being performed (if any)\nReturns:\n    Rendered text view of the todo list";

    struct edc_param_object_property properties[] = {
        {"content", edc_param_str_type("Task description", NULL)},
        {"status", edc_param_str_type("Status of task.", "pending", "in_progress", "completed", NULL)},
        {"activeForm", edc_param_str_type("Current action being performed (if any)", NULL)}};

    struct edc_param_type *item_type = edc_param_object_type("Todo item", properties, G_N_ELEMENTS(properties));
    struct edc_param_type *items_type = edc_param_array_type("New todo items", item_type);

    struct edc_tool_param *param = edc_tool_param_new("items", items_type, true);

    struct edc_tool *tool = edc_tool_new(name, desc, param, edc_tools_todo_exec_async);

    edc_toolbox_register(tb, tool);
}
