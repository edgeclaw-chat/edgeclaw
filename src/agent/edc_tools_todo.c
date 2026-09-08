#include "edc_tools_todo.h"
#include "edc_channel_actor.h"
#include "edc_compat.h"
#include "edc_param_types.h"
#include "edc_toolbox.h"
#include "edc_tools.h"
#include "sds.h"
#include <stdlib.h>

static void edc_tools_todo_exec_async(struct edc_toolbox_call_ctx *ctx, struct edc_tool *tool, cJSON *args,
                                      void (*callback)(struct edc_toolbox_call_ctx *, sds, EdcError *))
{
    cJSON *items = cJSON_GetObjectItem(args, "items");
    int total = items ? cJSON_GetArraySize(items) : 0;

    /* 无论清单是否为空，回显都附带原始 args JSON ({"items":[...]})。TUI 对
     * update_todolist 消息只消费 payload 更新右侧 TodoWidget，不再渲染进对话
     * 消息列表；content 仍作为 tool result 回给 LLM，CLI 等无侧栏渲染端也
     * 照常显示。 */

    if (!total)
    {
        /* 空清单同样回显一次 ({"items":[]} payload)：TUI 借此把 TodoWidget
         * 清成 0/0。这里只发展示消息，callback 结果仍是 "No todos"，LLM
         * 看到的 tool result 与原来一致。 */
        char *payload = cJSON_PrintUnformatted(args);
        sds empty = sdsnew("# TODO\n\n0/0 completed");
        edc_toolbox_emit_tool_message_md_payload(ctx, "update_todolist", empty, payload);
        sdsfree(empty);
        cJSON_free(payload);
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

    sds lines = sdsnew("# TODO\n");
    int completed = 0;
    int in_progress_count = 0;
    for (int i = 0; i < total; ++i)
    {
        cJSON *item = cJSON_GetArrayItem(items, i);
        char *status = cJSON_GetStringValue(cJSON_GetObjectItem(item, "status"));
        char *content = cJSON_GetStringValue(cJSON_GetObjectItem(item, "content"));
        char *activeForm = cJSON_GetStringValue(cJSON_GetObjectItem(item, "activeForm"));

        if (!status || (strcmp(status, "pending") && strcmp(status, "in_progress") && strcmp(status, "completed")))
        {
            sdsfree(lines);
            sds msg = sdscatprintf(sdsempty(),
                                   "Error: invalid status '%s' at item %d. Must be pending, in_progress, or completed.",
                                   status ? status : "(null)", i + 1);
            EdcError *err = edc_error_new(EDC_ERR_INVAL, "%s", msg);
            callback(ctx, msg, err);
            return;
        }

        if (strcmp(status, "completed") == 0)
        {
            lines = sdscatprintf(lines, "- [x] %s\n", content);
            completed += 1;
        }
        else if (strcmp(status, "in_progress") == 0)
        {
            in_progress_count++;
            lines = sdscatprintf(lines, "- [ ] %s <- %s\n", content, activeForm);
        }
        else
        {
            lines = sdscatprintf(lines, "- [ ] %s\n", content);
        }
    }

    if (in_progress_count > 1)
    {
        sdsfree(lines);
        sds msg =
            sdscatprintf(sdsempty(), "Error: Only ONE item can be in_progress at a time, got %d.", in_progress_count);
        EdcError *err = edc_error_new(EDC_ERR_INVAL, "%s", msg);
        callback(ctx, msg, err);
        return;
    }

    lines = sdscatprintf(lines, "\n%d/%d completed", completed, total);

    char *payload = cJSON_PrintUnformatted(args);
    edc_toolbox_emit_tool_message_md_payload(ctx, "update_todolist", lines, payload);
    cJSON_free(payload);

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
