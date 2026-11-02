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

    /* Echo one header-carrying TOOL message whether the list is empty or not. When
     * non-empty the text IS the todo reminder (byte-identical to the request-tail
     * reminder, see edc_context_render_todo_reminder) and is the tool's result itself;
     * when empty this sends only a display checklist (the callback result is still "No
     * todos"). The TUI keeps todowrite messages out of the chat -- TODO display goes
     * entirely through task snapshots (the scheduler pushes one per echo, rendering the
     * active task's todos into the sidebar TodoWidget); content is also returned to the
     * LLM as the tool result, so CLI and other sidebar-less renderers show it normally. */

    if (!total)
    {
        /* An empty list is echoed too: the TUI uses it to push a snapshot clearing the
         * TodoWidget to 0/0 (the active task's todos were removed). Only the display
         * message is sent; the callback result is still "No todos" and the LLM sees the
         * same tool result as before. */
        sds empty = sdsnew("# TODO\n\n0/0 completed");
        edc_toolbox_emit_tool_message_md(ctx, "todowrite", empty);
        sdsfree(empty);

        /* The list was cleared -> the task holds no todos (later requests' tail reminder
         * falls back to the "no list yet" planning guide). */
        if (ctx->task && ctx->task->todos)
        {
            cJSON_Delete(ctx->task->todos);
            ctx->task->todos = NULL;
        }
        /* A successful todowrite call (including a clear): reset the todo-sync throttle,
         * so later LLM requests count the gap afresh. */
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

    /* Validate (the render functions assume legal input) */
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

    /* Attach this todo list to the calling task (wholesale replacement). The todo list
     * is part of task state: task->todos is persisted by the state_serializer. When an
     * LLM request is later assembled for this task, the app applies the throttle (only
     * after EDC_TODO_SYNC_REMIND_GAP requests without a todowrite) and recomputes the
     * current list into the tail reminder (edc_context_make_todo_reminder) injected into
     * the request -- the same source as this tool's returned text
     * (edc_context_render_todo_reminder), so the model in a multi-round tool loop always
     * sees a current list byte-identical to the tool result, without depending on a stale
     * todowrite tool message still in the window. */
    if (ctx->task)
    {
        if (ctx->task->todos)
            cJSON_Delete(ctx->task->todos);
        ctx->task->todos = cJSON_Duplicate(items, 1);
        /* todowrite succeeded: reset the todo-sync throttle (each tool call is itself a
         * list sync). */
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
        {"content", edc_param_str_type("Task description", NULL), true},
        {"status", edc_param_str_type("Status of task.", "pending", "in_progress", "completed", "cancelled", NULL), true},
        {"priority", edc_param_str_type("Priority of task.", "high", "medium", "low", NULL), true}};

    struct edc_param_type *item_type = edc_param_object_type("Todo item", properties, G_N_ELEMENTS(properties));
    struct edc_param_type *items_type = edc_param_array_type("New todo items", item_type);

    struct edc_tool_param *param = edc_tool_param_new("items", items_type, true);

    struct edc_tool *tool = edc_tool_new(name, desc, param, edc_tools_todo_exec_async);
    edc_tool_set_snippet(tool, "Maintain the plan as a list of steps");
    edc_toolbox_register(tb, tool);
}
