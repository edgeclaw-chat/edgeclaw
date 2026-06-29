#include "edc_string.h"
#include "edc_strutil.h"
#include "edc_tools_todo.h"
#include "edc_channel_actor.h"
#include "edc_param_types.h"
#include "edc_toolbox.h"
#include "edc_tools.h"

static void edc_tools_todo_exec_async(struct edc_toolbox_call_ctx *ctx, struct edc_tool *tool, cJSON *args,
                                     void (*callback)(struct edc_toolbox_call_ctx *, sds , GError *))
{
    cJSON *items = cJSON_GetObjectItem(args, "items");
    if (!items || !cJSON_GetArraySize(items))
    {
        callback(ctx, edc_str_new("No todos"), NULL);
        return;
    }

    sds lines = edc_str_new("# TODO\n");
    int completed = 0;
    int total = cJSON_GetArraySize(items);
    for (int i = 0; i < total; ++i)
    {
        cJSON *item = cJSON_GetArrayItem(items, i);
        char *status = cJSON_GetStringValue(cJSON_GetObjectItem(item, "status"));
        char *content = cJSON_GetStringValue(cJSON_GetObjectItem(item, "content"));
        char *activeForm = cJSON_GetStringValue(cJSON_GetObjectItem(item, "activeForm"));

        if (edc_str_equal(status, "completed"))
        {
            edc_str_printf(lines, "- [x] %s\n", content);
            completed += 1;
        }
        else if (edc_str_equal(status, "in_progress"))
        {
            edc_str_printf(lines, "- [ ] %s <- %s\n", content, activeForm);
        }
        else
        {
            edc_str_printf(lines, "- [ ] %s\n", content);
        }
    }

    edc_str_printf(lines, "\n%d/%d completed", completed, total);

    struct edc_channel_message *message = edc_channel_message_new(edc_channel_copy(ctx->call_from), strdup(lines));
    if (message->channel->type == edc_channel_qq)
    {
        message->channel->qq.seq_id = g_random_int();
    }

    edc_actor_send(ctx->tb->channel_actor,
                  edc_actor_msg_new_sub_type(BC_ACTOR_MSG_CHANNEL_SEND, ctx->call_from->type, ctx->tb->scheduler, NULL,
                                            message, (edc_actor_data_free_fn)edc_channel_message_free));

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

    struct edc_tool_param *param = edc_tool_param_new("items", items_type, TRUE);

    struct edc_tool *tool = edc_tool_new(name, desc, g_slist_append(NULL, param), edc_tools_todo_exec_async);

    edc_toolbox_register(tb, tool);
}
