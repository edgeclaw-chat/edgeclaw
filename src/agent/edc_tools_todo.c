#include "edc_tools_todo.h"
#include "edc_channel_actor.h"
#include "edc_compat.h"
#include "edc_param_types.h"
#include "edc_task.h"
#include "edc_toolbox.h"
#include "edc_tools.h"
#include "sds.h"
#include <stdlib.h>

/* 纯渲染"- [ ]/- [x]" 清单行 (无 "<system-reminder>" 包裹、无引导段) ——
 * 与 render_reminder 共用同一行格式源: 它是 todo reminder 状态段的正文, 供
 * 需要把清单行单独嵌入的调用方使用。items 已在工具入口校验过 (status 合法、
 * 至多一个 in_progress)。对缺失字段做保守兜底 (缺省视为 pending / 空文本)，
 * 避免 agent 侧读取历史数据时因脏数据崩溃。
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

/* ── todo reminder 两段式骨架 (单一渲染源) ──────────────────────
 * 提醒 = "系统状态 + 本轮指令" (2026-09-09, 参照用户模板重构):
 *   ① ## 🚨 CURRENT TASK SYSTEM STATE —— 当前 todo 清单全貌 (与工具回显
 *      同格式的清单行), 让模型看到整个计划与执行到哪一步;
 *   ② ## 🛠️ INSTRUCTIONS FOR THIS TURN —— 每轮必须遵守的节拍: 检查上一个
 *     工具的执行结果; 当前条目干完后的【下一个动作必须是调用 update_todolist】
 *     同步状态; 当前条目未标 [x] 不得跳到下一项。逐轮强约束, 解决"执行完
 *     好几步才想起来更新一次清单"的滞后问题。
 * 本文件 (edc_tools_todo.c) 是这套文本的唯一生成地: update_todolist 工具每次
 * 成功返回的 result/回显 就是这条 todo reminder 本身, LLM 请求队尾的 reminder
 * (edc_context_make_todo_reminder 的 content) 也由同一函数生成 —— 不再存在
 * 独立的 "# TODO" checklist markdown 形态, 模型在任何位置看到的清单格式
 * 逐字节一致。 */

/* 状态段标题 / 指令段标题 / 无清单状态行 (两分支共用) */
static const char *kTodoStateHeader = "## 🚨 CURRENT TASK SYSTEM STATE\n";
static const char *kTodoTurnHeader = "## 🛠️ INSTRUCTIONS FOR THIS TURN\n";
static const char *kTodoNoListState = "No todo list exists for this task yet.\n";

/* 逐轮同步节拍 (清单非空时注入)。核心诉求: 不许干完好几步才更新一次 todo——
 * 当前条目一完成, 下一步就强制 update_todolist 同步。 */
static const char *kTodoTurnGuide =
    "- Check the execution result of the last tool call.\n"
    "- If the item you are working on is fully finished, your NEXT SINGLE ACTION must be calling "
    "update_todolist to update the state (mark it \"completed\", promote the next pending item to "
    "\"in_progress\" with activeForm set to the concrete action you are doing now, send the FULL "
    "updated list).\n"
    "- Do NOT jump to the next item until the current one is marked as [x].\n"
    "- When the whole list is done, keep the completed list — do not clear it or invent items.\n";

/* 建清单引导 (尚无清单时注入)。多步任务先把计划落到 update_todolist, 之后每轮
 * 自然切换到 kTodoTurnGuide 的逐轮同步节拍。 */
static const char *kTodoPlanningGuide =
    "- If the request is multi-step — several distinct steps or multiple tool calls (research, coding, "
    "debugging, batch edits, ...) — your FIRST action must be calling update_todolist to create a "
    "step-by-step plan, then execute it.\n"
    "- Simple single-step requests or direct conversation do NOT need a todo list — just answer "
    "without creating one.\n";

/* 完整 todo reminder 文本 (含 <system-reminder> 包裹), 与 LLM 请求队尾的
 * reminder (edc_context_make_todo_reminder 的 content) 逐字节一致 ——
 * update_todolist 工具 result/回显与上下文提醒共用此单一渲染源。两分支互斥:
 *  - items 非空 → 状态段 = 清单行 (render_body), 指令段 = 逐轮同步节拍;
 *  - items 为 NULL/空 → 状态段 = "尚无清单", 指令段 = 建计划引导。
 * 返回 sds, 调用方负责 sdsfree。 */
sds edc_tools_todo_render_reminder(cJSON *items)
{
    int has_list = items && cJSON_GetArraySize(items) > 0;
    sds content = sdsnew("<system-reminder>\n");
    content = sdscat(content, kTodoStateHeader);

    if (has_list)
    {
        sds body = edc_tools_todo_render_body(items, NULL);
        content = sdscat(content, body); /* 清单行, 每行以 \n 结尾 */
        sdsfree(body);
    }
    else
    {
        content = sdscat(content, kTodoNoListState);
    }

    content = sdscat(content, "\n"); /* 状态段与指令段之间留空行 */
    content = sdscat(content, kTodoTurnHeader);

    content = sdscat(content, has_list ? kTodoTurnGuide : kTodoPlanningGuide);

    content = sdscat(content, "\n</system-reminder>");
    return content;
}

static void edc_tools_todo_exec_async(struct edc_toolbox_call_ctx *ctx, struct edc_tool *tool, cJSON *args,
                                      void (*callback)(struct edc_toolbox_call_ctx *, sds, EdcError *))
{
    cJSON *items = cJSON_GetObjectItem(args, "items");
    int total = items ? cJSON_GetArraySize(items) : 0;

    /* 无论清单是否为空都回显一条带 header 的 TOOL 消息。非空时文本 = todo
     * reminder (与 LLM 请求队尾的 reminder 逐字节一致, 见
     * edc_tools_todo_render_reminder), 它就是工具返回的 result 本身; 空清单
     * 时这里只发展示用的空 checklist (callback 结果仍是 "No todos")。TUI 对
     * update_todolist 消息不进对话区 —— TODO 展示完全走任务快照 (见 scheduler:
     * 每次回显推送一次快照, active task 的 todolist 渲染到右侧 TodoWidget)；
     * content 同时作为 tool result 回给 LLM，CLI 等无侧栏渲染端也照常显示。 */

    if (!total)
    {
        /* 空清单同样回显一次：TUI 借此推送快照把 TodoWidget 清成 0/0
         * (active task 的 todolist 已被摘除)。这里只发展示消息，callback
         * 结果仍是 "No todos"，LLM 看到的 tool result 与原来一致。 */
        sds empty = sdsnew("# TODO\n\n0/0 completed");
        edc_toolbox_emit_tool_message_md(ctx, "update_todolist", empty);
        sdsfree(empty);

        /* 清单被清空 → 该 task 不再持有 todo（后续请求的队尾 reminder 自动
         * 退回"尚无清单"的建计划引导） */
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

    sds lines = edc_tools_todo_render_reminder(items);

    /* 把这份 todo 清单挂到调用它的 task 上 (整体替换)。todo list 是 task
     * 状态的一部分: task->todolist 已被 state_serializer 持久化；之后每次为
     * 该 task 组装 LLM 请求时 (见 agent send_llm), agent 会把当前清单重新
     * 计算成队尾 todo reminder (edc_context_make_todo_reminder) 注入请求 ——
     * 与本次工具返回的文本同源 (edc_tools_todo_render_reminder), 让多轮工具
     * 循环中的模型始终能看到与工具结果逐字节一致的当前清单, 而不依赖窗口内
     * 残留的 update_todolist tool 消息。 */
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
