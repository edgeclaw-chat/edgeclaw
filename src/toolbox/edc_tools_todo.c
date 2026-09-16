#include "edc_tools_todo.h"
#include "edc_context.h"
#include "edc_channel_actor.h"
#include "edc_compat.h"
#include "edc_param_types.h"
#include "edc_task.h"
#include "edc_toolbox.h"
#include "edc_tools.h"
#include "sds.h"
#include <stdlib.h>

static void edc_tools_todo_exec_async(struct edc_toolbox_call_ctx *ctx, struct edc_tool *tool, cJSON *args,
                                      void (*callback)(struct edc_toolbox_call_ctx *, sds, EdcError *))
{
    cJSON *items = cJSON_GetObjectItem(args, "items");
    int total = items ? cJSON_GetArraySize(items) : 0;

    /* 无论清单是否为空都回显一条带 header 的 TOOL 消息。非空时文本 = todo
     * reminder (与 LLM 请求队尾的 reminder 逐字节一致, 见
     * edc_context_render_todo_reminder), 它就是工具返回的 result 本身; 空清单
     * 时这里只发展示用的空 checklist (callback 结果仍是 "No todos")。TUI 对
     * todowrite 消息不进对话区 —— TODO 展示完全走任务快照 (见 scheduler:
     * 每次回显推送一次快照, active task 的 todos 渲染到右侧 TodoWidget)；
     * content 同时作为 tool result 回给 LLM，CLI 等无侧栏渲染端也照常显示。 */

    if (!total)
    {
        /* 空清单同样回显一次：TUI 借此推送快照把 TodoWidget 清成 0/0
         * (active task 的 todos 已被摘除)。这里只发展示消息，callback
         * 结果仍是 "No todos"，LLM 看到的 tool result 与原来一致。 */
        sds empty = sdsnew("# TODO\n\n0/0 completed");
        edc_toolbox_emit_tool_message_md(ctx, "todowrite", empty);
        sdsfree(empty);

        /* 清单被清空 → 该 task 不再持有 todo（后续请求的队尾 reminder 自动
         * 退回"尚无清单"的建计划引导） */
        if (ctx->task && ctx->task->todos)
        {
            cJSON_Delete(ctx->task->todos);
            ctx->task->todos = NULL;
        }
        /* 一次成功的 todowrite 调用 (含清空): 重置 todo 同步节流计数,
         * 后续 LLM 请求重新从"隔了多少条对话"计起 */
        if (ctx->task)
            edc_task_note_todo_sync(ctx->task);
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
        if (!status || (strcmp(status, "pending") && strcmp(status, "in_progress") && strcmp(status, "completed") &&
                        strcmp(status, "cancelled")))
        {
            sds msg = sdscatprintf(
                sdsempty(),
                "Error: invalid status '%s' at item %d. Must be pending, in_progress, completed, or cancelled.",
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

    sds lines = edc_context_render_todo_reminder(items);

    /* 把这份 todo 清单挂到调用它的 task 上 (整体替换)。todo list 是 task
     * 状态的一部分: task->todos 已被 state_serializer 持久化；之后为该
     * task 组装 LLM 请求时 (见 app send_llm), app 按节流规则 (隔了
     * EDC_TODO_SYNC_REMIND_GAP 条对话都没再调用 todowrite) 把当前
     * 清单重新计算成队尾 todo reminder (edc_context_make_todo_reminder)
     * 注入请求 —— 与本次工具返回的文本同源 (edc_context_render_todo_reminder),
     * 让多轮工具循环中的模型始终能看到与工具结果逐字节一致的当前清单, 而
     * 不依赖窗口内残留的 todowrite tool 消息。 */
    if (ctx->task)
    {
        if (ctx->task->todos)
            cJSON_Delete(ctx->task->todos);
        ctx->task->todos = cJSON_Duplicate(items, 1);
        /* todowrite 成功: 重置 todo 同步节流计数 (每次工具调用本身
         * 就是一次清单同步) */
        edc_task_note_todo_sync(ctx->task);
    }

    edc_toolbox_emit_tool_message_md(ctx, "todowrite", lines);

    callback(ctx, lines, NULL);
}

void edc_tools_todo_register(struct edc_toolbox *tb)
{
    const char *name = "todowrite";
    const char *desc =
        "Create and maintain a structured task list for the current coding session. Tracks progress, organizes "
        "multi-step work, and surfaces status to the user.\n"
        "\n"
        "## When to use\n"
        "Use proactively when:\n"
        "- The task requires 3+ distinct steps or actions (not just 3 tool calls for a single conceptual step)\n"
        "- The work is non-trivial and benefits from planning\n"
        "- The user provides multiple tasks (numbered or comma-separated) or explicitly asks for a todo list\n"
        "- New instructions arrive - capture them as todos\n"
        "- You start a task - mark it `in_progress` (only one at a time) before working\n"
        "- You finish a task - mark it `completed` and add any follow-ups discovered during the work\n"
        "\n"
        "## When NOT to use\n"
        "Skip when:\n"
        "- The work is a single, straightforward task (or <3 trivial steps)\n"
        "- The request is purely informational or conversational\n"
        "- Tracking adds no organizational value\n"
        "\n"
        "## States\n"
        "- `pending` - not started\n"
        "- `in_progress` - actively working (exactly ONE at a time)\n"
        "- `completed` - finished successfully\n"
        "- `cancelled` - no longer needed\n"
        "\n"
        "## Rules\n"
        "- Update status in real time; don't batch completions\n"
        "- Mark `completed` only after the required work is actually done, including any required verification. "
        "Never based on intent.\n"
        "- Keep exactly one `in_progress` while work remains\n"
        "- If blocked or partial, keep it `in_progress` and add a follow-up todo describing the blocker\n"
        "- Preserve user-provided commands verbatim (flags, args, order)\n"
        "- Items should be specific and actionable; break large work into smaller steps\n"
        "\n"
        "## Examples\n"
        "\n"
        "Use it:\n"
        "- \"Add a dark mode toggle and run the tests\" -> multi-step feature + explicit verification\n"
        "- \"Rename getCwd -> getCurrentWorkingDirectory across the repo\" -> grep reveals 15 occurrences in 8 files\n"
        "- \"Implement registration, catalog, cart, checkout\" -> multiple complex features\n"
        "\n"
        "Skip it:\n"
        "- \"How do I print Hello World in Python?\" -> informational\n"
        "- \"Add a comment to calculateTotal\" -> single edit\n"
        "- \"Run npm install and tell me what happened\" -> one command\n"
        "\n"
        "When in doubt, use it.";

    struct edc_param_object_property properties[] = {
        {"content", edc_param_str_type("Task description", NULL)},
        {"status", edc_param_str_type("Status of task.", "pending", "in_progress", "completed", "cancelled", NULL)},
        {"priority", edc_param_str_type("Priority of task.", "high", "medium", "low", NULL)}};

    struct edc_param_type *item_type = edc_param_object_type("Todo item", properties, G_N_ELEMENTS(properties));
    struct edc_param_type *items_type = edc_param_array_type("New todo items", item_type);

    struct edc_tool_param *param = edc_tool_param_new("items", items_type, true);

    struct edc_tool *tool = edc_tool_new(name, desc, param, edc_tools_todo_exec_async);

    edc_toolbox_register(tb, tool);
}
