/* 验证: task 的 todo 状态以"运行时 reminder"的形式、在 LLM 请求的消息
 * 列表【最末尾】追加为 role=system 消息 (req->tail_system, llm actor 在
 * history + 当前 turn 之后发出) —— 而不是拼进头部 system prompt。队尾紧贴
 * 模型开始生成的位置, 每次都是重新计算的当前状态, 长上下文里不易被遗忘。
 *
 * 节流 (2026-09-09): reminder 不每轮请求都发 —— 只有"隔了
 * EDC_TODO_SYNC_REMIND_GAP (10) 条对话 (LLM 请求) 都没调用过
 * todowrite"时, 该次请求才 append 一次提醒 (并清零重计); todowrite
 * 工具成功调用同样清零。
 *
 * 链路 (生产代码, 本测试逐一验证可直接验证的环节):
 *   1. todowrite 工具 (edc_tools_todo.c) 经真实 toolbox 执行后把校验
 *      通过的 items 挂到 ctx->task->todos (整体替换), 并重置 todo 同步
 *      节流计数 (task->todo_sync_gap = 0) —— 本测试执行真实工具。
 *   2. 工具返回的 result == task->todos 经 edc_context_render_todo_reminder
 *      生成的 todo reminder 文本 (同一渲染函数, 两段式骨架 + <system-reminder>
 *      包裹, 不再是独立的 "# TODO" checklist markdown)。
 *   3. edc_task_llm_request_poke 的节流状态机 (每次 send_llm 前调用): 计
 *      数累加, 达阈值返回 true 并清零 —— 直测。
 *   4. edc_app_cb_send_llm 仅在 poke 返回 true 时调用
 *      edc_context_make_todo_reminder (edc_context.c, 实现于生产代码, 在此
 *      直测): 有清单 → role=system 的 reminder 消息, content 与工具 result
 *      逐字节一致 (单一渲染源 edc_context_render_todo_reminder), 不带 base,
 *      独立成消息; 无清单且 task 非空 → 短规划引导; task 为 NULL → NULL。
 *      副本语义: 不改共享 history[0] / task。
 *   5. 头部 edc_context_append_system_part 恢复纯净: 只挂 group memory +
 *      system prompt (引用/现拼), 不含任何 todo 内容。
 *
 * 本测试不依赖 secret/网络/真实 home: 不构造 app, 直接驱动 toolbox 与 task。
 */
#include "edc_channel_actor.h"
#include "edc_compat.h"
#include "edc_config.h"
#include "edc_context.h"
#include "edc_error.h"
#include "edc_task.h"
#include "edc_task_group.h"
#include "edc_toolbox.h"
#include "edc_tools_todo.h"
#include "sds.h"
#include <cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failed = 0;
static int passed = 0;

#define T(name) printf("  TEST: %s\n", name)
#define C(cond, ...)                                                                                                   \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            printf("    FAIL: " __VA_ARGS__);                                                                          \
            printf("\n");                                                                                              \
            failed++;                                                                                                  \
        }                                                                                                              \
        else                                                                                                           \
        {                                                                                                              \
            passed++;                                                                                                  \
        }                                                                                                              \
    } while (0)

/* 工具回调: 拥有 result/error 的所有权 (真实 actor 里由回调方释放)。 */
static sds g_result = NULL;
static EdcError *g_err = NULL;

static void capture_cb(struct edc_toolbox_call_ctx *ctx, sds result, EdcError *error)
{
    (void)ctx;
    if (g_result)
        sdsfree(g_result);
    if (g_err)
        edc_error_free(g_err);
    g_result = result; /* take ownership */
    g_err = error;
}

static void reset_capture(void)
{
    if (g_result)
    {
        sdsfree(g_result);
        g_result = NULL;
    }
    if (g_err)
    {
        edc_error_free(g_err);
        g_err = NULL;
    }
}

/* reminder 内容以 <system-reminder> 标签包裹 (与平台原生 system reminder 同构:
 * 开标签后紧跟章节标题, 内容末尾空一行后闭标签)。断言辅助只认"开标签 + 对应
 * 标题 + 闭标签收尾"的完整形态, 防止半截包裹 / 标签错位回归。 */
static int content_starts_with(const char *c, const char *prefix)
{
    return c && strncmp(c, prefix, strlen(prefix)) == 0;
}

static int content_ends_with(const char *c, const char *suffix)
{
    size_t cl = c ? strlen(c) : 0;
    size_t sl = strlen(suffix);
    return cl >= sl && strcmp(c + cl - sl, suffix) == 0;
}

static int is_todo_reminder(const char *c)
{
    return content_starts_with(c, "<system-reminder>\n## 🚨 CURRENT TASK SYSTEM STATE\n") && c &&
           strstr(c, "No todo list exists for this task yet.") == NULL && content_ends_with(c, "\n</system-reminder>");
}

static int is_planning_reminder(const char *c)
{
    return content_starts_with(c, "<system-reminder>\n## 🚨 CURRENT TASK SYSTEM STATE\n"
                                  "No todo list exists for this task yet.") &&
           content_ends_with(c, "\n</system-reminder>");
}

/* 头部 system 部分必须连 reminder 标签都不出现 (标签即提醒的标记, 头部只准
 * 有 group memory + 纯净 system prompt)。 */
static int head_has_no_reminder(const char *c)
{
    return c && strstr(c, "<system-reminder>") == NULL && strstr(c, "CURRENT TASK SYSTEM STATE") == NULL &&
           strstr(c, "INSTRUCTIONS FOR THIS TURN") == NULL;
}

/* ── 1. 真实 todowrite 执行: 挂载 task->todos; reminder 组装 ── */

static void test_attach_and_reminder(void)
{
    T("todowrite attaches items to task; make_todo_reminder builds tail reminder");

    struct edc_config *conf = g_new0(struct edc_config, 1);
    struct edc_toolbox *tb = edc_toolbox_new(conf, NULL, NULL);
    struct edc_task *task = edc_task_new("t-attach", "s", "g", "test", edc_channel_adapter);
    C(task->todos == NULL, "fresh task has no todos");

    cJSON *args = cJSON_Parse("{"
                              "\"items\":["
                              "{\"content\":\"step one\",\"status\":\"pending\",\"priority\":\"low\"},"
                              "{\"content\":\"step two\",\"status\":\"in_progress\",\"priority\":\"high\"},"
                              "{\"content\":\"step three\",\"status\":\"completed\",\"priority\":\"medium\"},"
                              "{\"content\":\"step four\",\"status\":\"cancelled\",\"priority\":\"low\"}"
                              "]}");
    C(args != NULL, "args json parses");

    struct edc_toolbox_call_ctx *ctx =
        edc_toolbox_call_ctx_new(tb, edc_channel_adapter, (void *)(intptr_t)1, task, NULL);
    edc_toolbox_exec(ctx, "todowrite", args, capture_cb);

    C(g_err == NULL, "no validation error");
    C(g_result != NULL, "tool returned a result");
    C(task->todos != NULL, "items attached to task->todos");
    C(cJSON_GetArraySize(task->todos) == 4, "todos has 4 items");
    C(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetArrayItem(task->todos, 1), "status")),
             "in_progress") == 0,
      "item 2 kept in_progress status");

    /* 工具 result == task->todos 经 edc_context_render_todo_reminder 的完整
     * todo reminder 文本 (不再是独立的 "# TODO" checklist markdown) */
    sds rem = edc_context_render_todo_reminder(task->todos);
    C(rem != NULL, "render_reminder returns reminder text for attached list");
    C(strcmp(rem, g_result) == 0, "tool result == todo reminder text (same renderer)");
    C(strstr(rem, "- [ ] step one [low]") != NULL, "pending item rendered with priority tag");
    C(strstr(rem, "- [•] step two [high]") != NULL, "in_progress item rendered with marker + priority tag");
    C(strstr(rem, "- [x] step three [medium]") != NULL, "completed item rendered with priority tag");
    C(strstr(rem, "- [-] step four [low]") != NULL, "cancelled item rendered with dash marker");
    C(strstr(rem, "# TODO") == NULL, "no standalone '# TODO' checklist header anymore");

    /* make_todo_reminder: 独立 role=system 消息, content 即 reminder (无 base
     * 前缀 —— 不再拼进头部 system prompt) */
    cJSON *sys = edc_context_make_todo_reminder(task);
    C(sys != NULL, "make_todo_reminder returns a message for task with list");
    C(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(sys, "role")), "system") == 0, "role is system");
    const char *content = cJSON_GetStringValue(cJSON_GetObjectItem(sys, "content"));
    C(content != NULL, "content present");

    /* (a) 消息以 <system-reminder> 标签包裹, 两段式骨架 (独立成条, 不再是
     *     base 的追加尾巴): ① ## 🚨 CURRENT TASK SYSTEM STATE 状态段 = 清单行;
     *     ② ## 🛠️ INSTRUCTIONS FOR THIS TURN 指令段 = 逐轮同步节拍。冗长的
     *     "权威记录/防裁剪"论证与逐条维护规则已删 */
    C(is_todo_reminder(content), "reminder wrapped in <system-reminder> tags, opens with state header");
    C(strstr(content, "SYSTEM_BASE") == NULL, "no base prompt text inside reminder");
    C(strstr(content, "## 🚨 CURRENT TASK SYSTEM STATE\n") != NULL, "opens with state header");
    C(strstr(content, "## 🛠️ INSTRUCTIONS FOR THIS TURN\n") != NULL, "has per-turn instructions header");
    C(strstr(content, "recomputed and appended as a fresh system message") == NULL &&
          strstr(content, "trust it over todowrite echoes") == NULL &&
          strstr(content, "trimmed out of the context window") == NULL,
      "verbose authority/anti-trimming essay removed");

    /* (b) 指令段 (逐轮同步节拍): 先检查上个工具结果; 当前条目干完 → 下一动作
     *     必须是 todowrite (状态迁移 + 全量替换); 未标 [x] 不得跳项 */
    C(strstr(content, "Check the execution result of the last tool call.") != NULL,
      "turn instruction: check last tool result first");
    C(strstr(content, "NEXT SINGLE ACTION must be calling todowrite") != NULL,
      "finished item forces immediate todowrite call (next single action)");
    C(strstr(content, "todowrite") != NULL, "reminder to call todowrite present");
    C(strstr(content, "\"completed\"") != NULL && strstr(content, "\"in_progress\"") != NULL,
      "state transition guidance (completed -> in_progress) present");
    C(strstr(content, "FULL updated list") != NULL, "whole-list replacement stated");
    C(strstr(content, "marked as [x]") != NULL, "must-not-jump-ahead rule (mark [x] first)");
    C(strstr(content, "do not clear it") != NULL, "keep-finished-list rule present");
    C(strstr(content, "replaces the entire list (max 20 items)") == NULL &&
          strstr(content, "Every call replaces") == NULL,
      "tool-contract trivia (max-20 / replace-all essay) left out of the reminder");
    C(strstr(content, "Current state:") == NULL && strstr(content, "next pending:") == NULL &&
          strstr(content, "N/M completed") == NULL,
      "no derived count/next-pending block — the checklist itself is the state");

    /* (c) 单一渲染源: 工具 result 与队尾 reminder 逐字节一致 —— todowrite
     *     返回的正是这条 todo reminder 本身 (edc_context_make_todo_reminder 只
     *     包 role=system, 文本来自 edc_context_render_todo_reminder) */
    C(strcmp(content, g_result) == 0, "tail reminder content == tool result (byte-identical)");
    C(strcmp(content, rem) == 0, "tail reminder content == render_reminder(todos) (same renderer)");

    /* (d) 清单行 (共享 render_body) */
    C(strstr(content,
             "- [ ] step one [low]\n- [•] step two [high]\n- [x] step three [medium]\n- [-] step four [low]\n") != NULL,
      "checklist lines identical to tool rendering");

    /* (e) reminder 不含独立的 "# TODO" checklist 头 (该 md 形态已删除) */
    C(strstr(content, "# TODO\n") == NULL, "no bare '# TODO' header inside reminder");

    /* 副本语义: 返回的是未挂到任何数组的新对象 */
    C(sys->prev == NULL && sys->next == NULL, "sys msg is detached (owned by caller)");

    cJSON_Delete(sys);
    sdsfree(rem);
    reset_capture();
    edc_toolbox_call_ctx_free(ctx);
    cJSON_Delete(args);

    /* ── 清空清单 → 摘除挂载; 之后 reminder 退回"规划引导"形态 ── */
    T("empty todowrite detaches list; reminder becomes planning guide");
    cJSON *empty_args = cJSON_Parse("{\"items\":[]}");
    struct edc_toolbox_call_ctx *ctx2 =
        edc_toolbox_call_ctx_new(tb, edc_channel_adapter, (void *)(intptr_t)2, task, NULL);
    edc_toolbox_exec(ctx2, "todowrite", empty_args, capture_cb);
    C(g_err == NULL, "no error on empty list");
    C(g_result && strcmp(g_result, "No todos") == 0, "empty list tool result is 'No todos'");
    C(task->todos == NULL, "todos detached after clearing");
    cJSON *g2 = edc_context_make_todo_reminder(task);
    C(g2 != NULL, "cleared task still gets a tail reminder");
    const char *gc = cJSON_GetStringValue(cJSON_GetObjectItem(g2, "content"));
    C(is_planning_reminder(gc), "cleared list -> planning reminder (no-list branch of the same skeleton)");
    C(strstr(gc, "No todo list exists for this task yet.") != NULL, "state section says no list yet");
    C(strstr(gc, "- [ ]") == NULL && strstr(gc, "- [x]") == NULL, "planning branch carries no checklist lines");
    cJSON_Delete(g2);
    reset_capture();
    edc_toolbox_call_ctx_free(ctx2);
    cJSON_Delete(empty_args);

    edc_task_free(task);
    edc_toolbox_free(tb);
    free(conf);
}

/* ── 2. 无清单 / 空清单 / NULL task ── */

static void test_no_list_planning_guide(void)
{
    T("task without list / empty list -> planning guide; NULL task -> NULL");
    struct edc_task *task = edc_task_new("t-nolist", "s", "g", "no list", edc_channel_adapter);
    cJSON *r = edc_context_make_todo_reminder(task);
    C(r != NULL, "task without list still gets a tail message (planning guide)");
    const char *c = cJSON_GetStringValue(cJSON_GetObjectItem(r, "content"));
    C(is_planning_reminder(c), "no list -> planning guide");
    C(strstr(c, "todowrite") != NULL && strstr(c, "multi-step") != NULL,
      "planning branch tells to create a list first for multi-step work");
    C(strstr(c, "- [ ]") == NULL && strstr(c, "- [x]") == NULL, "planning branch has no checklist lines");
    cJSON_Delete(r);
    C(edc_context_make_todo_reminder(NULL) == NULL, "NULL task -> NULL (no reminder)");
    edc_task_free(task);

    /* 空数组挂载 (防御路径): 等价于无清单 → 规划引导 */
    struct edc_config *conf = g_new0(struct edc_config, 1);
    struct edc_toolbox *tb = edc_toolbox_new(conf, NULL, NULL);
    struct edc_task *t2 = edc_task_new("t-empty", "s", "g", "empty", edc_channel_adapter);
    t2->todos = cJSON_CreateArray();
    r = edc_context_make_todo_reminder(t2);
    C(r != NULL && is_planning_reminder(cJSON_GetStringValue(cJSON_GetObjectItem(r, "content"))),
      "empty array -> planning guide");
    cJSON_Delete(r);
    edc_task_free(t2);
    edc_toolbox_free(tb);
    free(conf);
}

/* ── 3. 校验失败不挂载 ── */

static void test_validation_gate(void)
{
    T("failed validation does NOT attach the list");
    struct edc_config *conf = g_new0(struct edc_config, 1);
    struct edc_toolbox *tb = edc_toolbox_new(conf, NULL, NULL);
    struct edc_task *task = edc_task_new("t-bad", "s", "g", "bad", edc_channel_adapter);

    cJSON *bad = cJSON_Parse("{\"items\":[{\"content\":\"x\",\"status\":\"bogus\",\"priority\":\"high\"}]}");
    struct edc_toolbox_call_ctx *ctx =
        edc_toolbox_call_ctx_new(tb, edc_channel_adapter, (void *)(intptr_t)3, task, NULL);
    edc_toolbox_exec(ctx, "todowrite", bad, capture_cb);
    C(g_err != NULL, "invalid status produces error");
    if (g_err)
        printf("      (validation error: %s)\n", g_err->message ? g_err->message : "(null)");
    C(g_result == NULL || strstr(g_result, "invalid") != NULL, "error text mentions invalid status");
    C(task->todos == NULL, "list NOT attached after validation failure");
    cJSON *r = edc_context_make_todo_reminder(task);
    C(r != NULL && is_planning_reminder(cJSON_GetStringValue(cJSON_GetObjectItem(r, "content"))),
      "invalid list never attached -> planning guide only");
    cJSON_Delete(r);
    reset_capture();
    edc_toolbox_call_ctx_free(ctx);
    cJSON_Delete(bad);

    /* priority 非法同样被拒 (enum: high | medium | low) */
    cJSON *badpri = cJSON_Parse("{\"items\":[{\"content\":\"x\",\"status\":\"pending\",\"priority\":\"urgent\"}]}");
    struct edc_toolbox_call_ctx *ctxp =
        edc_toolbox_call_ctx_new(tb, edc_channel_adapter, (void *)(intptr_t)4, task, NULL);
    edc_toolbox_exec(ctxp, "todowrite", badpri, capture_cb);
    C(g_err != NULL, "invalid priority produces error");
    C(task->todos == NULL, "list NOT attached after invalid priority");
    reset_capture();
    edc_toolbox_call_ctx_free(ctxp);
    cJSON_Delete(badpri);

    edc_task_free(task);
    edc_toolbox_free(tb);
    free(conf);
}

/* ── 4. 渲染函数边界 ── */

static void test_render_boundaries(void)
{
    T("render_reminder(NULL / empty array) falls into the no-list planning branch");
    sds rem = edc_context_render_todo_reminder(NULL);
    C(rem != NULL && is_planning_reminder(rem), "NULL items -> planning reminder (no-list branch)");
    sdsfree(rem);
    cJSON *arr = cJSON_CreateArray();
    rem = edc_context_render_todo_reminder(arr);
    C(rem != NULL && is_planning_reminder(rem), "empty array -> planning reminder (no-list branch)");
    sdsfree(rem);
    cJSON_Delete(arr);

    T("render_body(NULL) returns empty sds, count 0");
    int completed = -1;
    sds body = edc_context_render_todo_body(NULL, &completed);
    C(body != NULL && sdslen(body) == 0, "NULL items -> empty body");
    C(completed == 0, "count is 0 for empty body");
    sdsfree(body);
}

/* ── 5. 状态摘要的其它形态: 全完成 (无 in_progress / 无 pending) ── */

static void test_state_summary_all_done(void)
{
    T("state summary when every item completed");
    struct edc_config *conf = g_new0(struct edc_config, 1);
    struct edc_toolbox *tb = edc_toolbox_new(conf, NULL, NULL);
    struct edc_task *task = edc_task_new("t-done", "s", "g", "all done", edc_channel_adapter);

    task->todos = cJSON_Parse("["
                                 "{\"content\":\"pack\",\"status\":\"completed\",\"priority\":\"low\"},"
                                 "{\"content\":\"ship\",\"status\":\"completed\",\"priority\":\"high\"}"
                                 "]");
    C(task->todos != NULL && cJSON_GetArraySize(task->todos) == 2, "attached 2 completed items");

    cJSON *sys = edc_context_make_todo_reminder(task);
    const char *content = cJSON_GetStringValue(cJSON_GetObjectItem(sys, "content"));
    C(is_todo_reminder(content), "all-done reminder wrapped, opens with its own title");
    C(strstr(content, "## 🚨 CURRENT TASK SYSTEM STATE\n- [x] pack [low]\n- [x] ship [high]\n") != NULL,
      "state section = full list with all items marked [x] (list itself is the state)");
    C(strstr(content, "No todo list exists for this task yet.") == NULL, "list present: not the no-list branch");
    C(strstr(content, "NEXT SINGLE ACTION must be calling todowrite") != NULL &&
          strstr(content, "do not clear it") != NULL,
      "turn instructions still present when all items done (keep completed list)");

    cJSON_Delete(sys);
    edc_task_free(task);
    edc_toolbox_free(tb);
    free(conf);
}

/* ── 6. append_system_part: 头部 system 部分组装语义 ──
 * 直接驱动生产入口 edc_context_append_system_part, 验证头部已恢复纯净:
 *   - 只挂 group memory (可选) + system prompt (history[0] 引用或现拼),
 *     不含任何 todo 内容 / 规划引导 (那些走 make_todo_reminder → 队尾)
 *   - 有 history[0] → 引用 (不复制, msgs 删除后原消息还在)
 *   - group memory 摘要永远排在 system prompt 之前
 *   - group 无历史 + conf → 现拼纯净 prompt, *out_pristine 交还纯净文本
 *     (写回 group history 是调用方编排职责) */

/* ── 5. todo 同步节流: reminder 不再每轮请求都发 (2026-09-09) ──
 * 语义: 距上次 todowrite 成功调用 (或上次提醒) 累计 EDC_TODO_SYNC_REMIND_GAP
 * 次 LLM 请求 (每次组装请求 = 对话里模型生成一条消息/一次决策, 含工具链续发)
 * 仍未同步 → 该次请求 append 一次 reminder 并清零重计; todowrite 成功
 * 调用同样清零。 */

static void test_todo_sync_gap_throttle(void)
{
    T("llm_request_poke accumulates; fires once at gap then resets; note_todo_sync resets");

    struct edc_task *task = edc_task_new("t-gap", "s", "g", "gap", edc_channel_adapter);
    C(task->todo_sync_gap == 0, "fresh task gap == 0");

    /* 前 gap-1 次请求 (对话) 未达阈值 → 从不提醒 */
    bool any = false;
    for (int i = 0; i < EDC_TODO_SYNC_REMIND_GAP - 1; i++)
        if (edc_task_llm_request_poke(task))
            any = true;
    C(!any, "no reminder within the first gap-1 requests");
    C(task->todo_sync_gap == EDC_TODO_SYNC_REMIND_GAP - 1, "gap accumulates to gap-1");

    /* 第 gap 次请求仍没 todowrite → 达阈值: 提醒一次, 清零重计 */
    C(edc_task_llm_request_poke(task), "reminder fires on the gap-th request");
    C(task->todo_sync_gap == 0, "gap reset right after the reminder fires");

    /* 提醒后不会紧接着每轮都提醒 */
    C(!edc_task_llm_request_poke(task), "no immediate re-reminder next request");
    C(task->todo_sync_gap == 1, "counting restarts from 1 after reminder");

    /* todowrite 成功调用清零 (工具清零在 test_tools_todo_resets_gap) */
    task->todo_sync_gap = 6;
    edc_task_note_todo_sync(task);
    C(task->todo_sync_gap == 0, "note_todo_sync resets gap");

    /* 清零后再隔整整一个 gap 又会提醒一次 */
    for (int i = 0; i < EDC_TODO_SYNC_REMIND_GAP - 1; i++)
        edc_task_llm_request_poke(task);
    C(task->todo_sync_gap == EDC_TODO_SYNC_REMIND_GAP - 1, "counting again after reset");
    C(edc_task_llm_request_poke(task), "second reminder fires after another full gap");

    /* NULL 安全 */
    C(!edc_task_llm_request_poke(NULL), "poke(NULL) is safely false");
    edc_task_note_todo_sync(NULL); /* must not crash */

    edc_task_free(task);
}

static void test_tools_todo_resets_gap(void)
{
    T("todowrite tool success (set & clear) resets gap; validation failure keeps it");

    struct edc_config *conf = g_new0(struct edc_config, 1);
    struct edc_toolbox *tb = edc_toolbox_new(conf, NULL, NULL);
    struct edc_task *task = edc_task_new("t-reset", "s", "g", "reset", edc_channel_adapter);

    /* 模拟已隔了 7 次请求没同步 */
    task->todo_sync_gap = 7;

    cJSON *args = cJSON_Parse("{\"items\":[{\"content\":\"a\",\"status\":\"pending\",\"priority\":\"medium\"}]}");
    struct edc_toolbox_call_ctx *ctx =
        edc_toolbox_call_ctx_new(tb, edc_channel_adapter, (void *)(intptr_t)1, task, NULL);
    edc_toolbox_exec(ctx, "todowrite", args, capture_cb);
    C(g_err == NULL, "no validation error");
    C(task->todos != NULL && cJSON_GetArraySize(task->todos) == 1, "list attached");
    C(task->todo_sync_gap == 0, "successful set resets gap (a sync just happened)");
    reset_capture();
    edc_toolbox_call_ctx_free(ctx);
    cJSON_Delete(args);

    /* 清空清单同样是一次成功的 todowrite 调用 → 清零 */
    task->todo_sync_gap = 5;
    cJSON *empty_args = cJSON_Parse("{\"items\":[]}");
    struct edc_toolbox_call_ctx *ctx2 =
        edc_toolbox_call_ctx_new(tb, edc_channel_adapter, (void *)(intptr_t)2, task, NULL);
    edc_toolbox_exec(ctx2, "todowrite", empty_args, capture_cb);
    C(g_err == NULL, "no error on clear");
    C(task->todos == NULL, "list detached on clear");
    C(task->todo_sync_gap == 0, "successful clear also resets gap");
    reset_capture();
    edc_toolbox_call_ctx_free(ctx2);
    cJSON_Delete(empty_args);

    /* 校验失败 (status 非法) → 不构成同步, 不清零 */
    task->todo_sync_gap = 8;
    cJSON *bad_args = cJSON_Parse("{\"items\":[{\"content\":\"b\",\"status\":\"bogus\",\"priority\":\"medium\"}]}");
    struct edc_toolbox_call_ctx *ctx3 =
        edc_toolbox_call_ctx_new(tb, edc_channel_adapter, (void *)(intptr_t)3, task, NULL);
    edc_toolbox_exec(ctx3, "todowrite", bad_args, capture_cb);
    C(g_err != NULL, "invalid status rejected");
    C(task->todo_sync_gap == 8, "failed validation does NOT reset gap");
    reset_capture();
    edc_toolbox_call_ctx_free(ctx3);
    cJSON_Delete(bad_args);

    edc_task_free(task);
    edc_toolbox_free(tb);
    free(conf);
}

static void test_append_system_part(void)
{
    T("append_system_part: pure head (ref/copy), group memory order, fallback pristine");

    /* ── history 分支: 引用 history[0], 头部无规划引导 ── */
    struct edc_task *t1 = edc_task_new("ap-1", "s", "g", "no list", edc_channel_adapter);
    struct edc_task_group *g1 = edc_task_group_new(t1);
    cJSON *sys0 = cJSON_CreateObject();
    cJSON_AddStringToObject(sys0, "role", "system");
    cJSON_AddStringToObject(sys0, "content", "SYS0_BASE");
    cJSON_AddItemToArray(g1->history, sys0);
    cJSON *usr = cJSON_CreateObject();
    cJSON_AddStringToObject(usr, "role", "user");
    cJSON_AddStringToObject(usr, "content", "hi");
    cJSON_AddItemToArray(g1->history, usr);

    cJSON *msgs = cJSON_CreateArray();
    sds pristine = NULL;
    int n = edc_context_append_system_part(msgs, NULL, g1, NULL, &pristine);
    C(n == 1, "head = base reference only (no planning guide in head anymore)");
    C(pristine == NULL, "no pristine in history branch");
    cJSON *m0 = cJSON_GetArrayItem(msgs, 0);
    C(m0 != NULL && (m0->type & cJSON_IsReference) && m0->child == sys0->child,
      "history[0] attached by REFERENCE (aliases role/content), not deep copy");
    C(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(m0, "content")), "SYS0_BASE") == 0,
      "referenced content readable through array");
    C(strstr(cJSON_GetStringValue(cJSON_GetObjectItem(m0, "content")), "CURRENT TASK SYSTEM STATE") == NULL &&
          strstr(cJSON_GetStringValue(cJSON_GetObjectItem(m0, "content")), "INSTRUCTIONS FOR THIS TURN") == NULL &&
          strstr(cJSON_GetStringValue(cJSON_GetObjectItem(m0, "content")), "<system-reminder>") == NULL,
      "head message stays pure (no reminder sections/marker)");
    cJSON_Delete(msgs);
    C(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetArrayItem(g1->history, 0), "content")), "SYS0_BASE") ==
          0,
      "history[0] survives msgs delete (reference did not transfer/copy)");

    /* todo 引导独立于头部: no-list task 的 reminder = 规划引导 (owned), 模拟
     * app 把它作为队尾消息塞进请求数组 → 落在 base 引用之后 */
    cJSON *tail = edc_context_make_todo_reminder(t1);
    C(tail != NULL && !(tail->type & cJSON_IsReference), "planning-guide reminder is an owned msg");
    C(is_planning_reminder(cJSON_GetStringValue(cJSON_GetObjectItem(tail, "content"))),
      "no-list tail message is the task-planning guide");
    C(strstr(cJSON_GetStringValue(cJSON_GetObjectItem(tail, "content")), "todowrite") != NULL &&
          strstr(cJSON_GetStringValue(cJSON_GetObjectItem(tail, "content")), "multi-step") != NULL,
      "planning guide mentions creating list for multi-step work");
    msgs = cJSON_CreateArray();
    edc_context_append_system_part(msgs, NULL, g1, NULL, &pristine);
    cJSON_AddItemToArray(msgs, tail); /* req->tail_system 位置 (请求数组队尾) */
    C(cJSON_GetArraySize(msgs) == 2, "request = head ref + tail reminder");
    C(cJSON_GetArrayItem(msgs, 1) == tail, "reminder is the LAST message of the request");
    C(strstr(cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetArrayItem(msgs, 0), "content")),
             "INSTRUCTIONS FOR THIS TURN") == NULL,
      "no turn-instructions inside the head part");
    cJSON_Delete(msgs); /* 连带释放 tail reminder */

    /* ── history 分支: **system prompt 在前、group memory 在后** (常量在前, 易变的在后
     *    —— cache 友好, 2026-10-10) + 有 todo → 头部仍纯净, todo 只在队尾 reminder;
     *    共享 history[0] 不被改动 ── */
    g1->memory = sdsnew("MEM-SUMMARY");
    t1->todos = cJSON_Parse("[{\"content\":\"do it\",\"status\":\"pending\",\"priority\":\"high\"}]");
    C(t1->todos != NULL && cJSON_GetArraySize(t1->todos) == 1, "attached 1 todo item");
    msgs = cJSON_CreateArray();
    pristine = NULL;
    n = edc_context_append_system_part(msgs, NULL, g1, NULL, &pristine);
    C(n == 2, "base ref + group memory -> 2 head appends");
    C(pristine == NULL, "still no pristine (history exists)");
    /* 顺序: 常量 (system prompt) 在前, 易变的 (group memory) 在后 —— 折叠落地只作废后者
     * 之后的内容, 前缀 (系统提示词) 仍命中 cache。 */
    cJSON *m0b = cJSON_GetArrayItem(msgs, 0);
    C(m0b != NULL && (m0b->type & cJSON_IsReference) && m0b->child == sys0->child,
      "base attached by reference and comes FIRST (it is the constant part)");
    const char *h1 = cJSON_GetStringValue(cJSON_GetObjectItem(m0b, "content"));
    C(h1 && strcmp(h1, "SYS0_BASE") == 0, "head base stays PURE even when task has a todo list");
    C(strstr(h1, "CURRENT TASK SYSTEM STATE") == NULL && strstr(h1, "INSTRUCTIONS FOR THIS TURN") == NULL &&
          strstr(h1, "<system-reminder>") == NULL,
      "no reminder sections/marker anywhere in the head");
    cJSON *mem1 = cJSON_GetArrayItem(msgs, 1);
    C(mem1 && strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(mem1, "content")), "## Group Memory\nMEM-SUMMARY") == 0,
      "group memory comes AFTER the system prompt (mutable part last)");

    cJSON *r2 = edc_context_make_todo_reminder(t1);
    C(r2 != NULL && !(r2->type & cJSON_IsReference), "todo reminder is an owned msg");
    const char *rc = cJSON_GetStringValue(cJSON_GetObjectItem(r2, "content"));
    C(is_todo_reminder(rc), "todo reminder wrapped, opens with its title");
    C(strstr(rc, "## 🚨 CURRENT TASK SYSTEM STATE\n- [ ] do it [high]\n") != NULL,
      "fresh list rendered right under the state header");
    C(strstr(rc, "No todo list exists for this task yet.") == NULL, "list present: no-list branch NOT used");
    C(strstr(rc, "NEXT SINGLE ACTION must be calling todowrite") != NULL,
      "per-turn sync instruction present in the list branch");

    /* 模拟 app: reminder 经 req->tail_system 成为请求数组队尾 */
    cJSON_AddItemToArray(msgs, r2);
    C(cJSON_GetArraySize(msgs) == 3 && cJSON_GetArrayItem(msgs, 2) == r2,
      "todo reminder appended as LAST message, after memory + base ref");
    C(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(sys0, "content")), "SYS0_BASE") == 0,
      "shared history[0] untouched by reminder");
    cJSON_Delete(msgs);
    edc_task_group_free(g1); /* 连带释放 t1 / sys0 / usr / todos / memory */

    /* ── fallback: group 空历史 + conf → 现拼纯净 prompt 经 out_pristine 交还;
     *    写回 group history 属调用方; todo 仍只在队尾 reminder ── */
    struct edc_config *conf = g_new0(struct edc_config, 1);
    conf->workspace = sdsnew("/tmp/test-session");
    struct edc_task *t2 = edc_task_new("ap-2", "s", "g", "no list", edc_channel_adapter);
    struct edc_task_group *g2 = edc_task_group_new(t2);

    msgs = cJSON_CreateArray();
    pristine = NULL;
    n = edc_context_append_system_part(msgs, conf, g2, NULL, &pristine);
    C(n == 1, "fallback: one pure sys msg (no planning guide in head)");
    C(pristine != NULL && strncmp(pristine, "# EdgeClaw", 10) == 0, "pristine = fresh base prompt");
    m0 = cJSON_GetArrayItem(msgs, 0);
    C(!(m0->type & cJSON_IsReference), "fallback msg is owned");
    const char *f0 = cJSON_GetStringValue(cJSON_GetObjectItem(m0, "content"));
    C(f0 && strcmp(f0, pristine) == 0, "fallback request content == pristine");
    C(head_has_no_reminder(f0), "pure prompt has no todo section or reminder marker at all");
    C(cJSON_GetArraySize(g2->history) == 0, "context does NOT write group history (caller's job)");

    /* fallback + 无清单: 队尾 reminder = 规划引导 */
    cJSON *pg = edc_context_make_todo_reminder(t2);
    C(pg != NULL && is_planning_reminder(cJSON_GetStringValue(cJSON_GetObjectItem(pg, "content"))),
      "fallback no-list tail reminder is the planning guide");
    cJSON_AddItemToArray(msgs, pg);
    C(cJSON_GetArraySize(msgs) == 2 && cJSON_GetArrayItem(msgs, 1) == pg, "planning guide is the request tail");
    cJSON_Delete(msgs);
    sdsfree(pristine);

    /* fallback + todo: 头部纯净, todo 只在队尾 reminder */
    t2->todos = cJSON_Parse("[{\"content\":\"plan\",\"status\":\"in_progress\",\"priority\":\"high\"}]");
    msgs = cJSON_CreateArray();
    pristine = NULL;
    n = edc_context_append_system_part(msgs, conf, g2, NULL, &pristine);
    C(n == 1, "fallback+todo head: one pure sys msg");
    m0 = cJSON_GetArrayItem(msgs, 0);
    const char *ft = cJSON_GetStringValue(cJSON_GetObjectItem(m0, "content"));
    C(ft && strncmp(ft, "# EdgeClaw", 10) == 0 && strstr(ft, "CURRENT TASK SYSTEM STATE") == NULL &&
          strstr(ft, "INSTRUCTIONS FOR THIS TURN") == NULL && strstr(ft, "<system-reminder>") == NULL,
      "fallback+todo request head = fresh pure base (no reminder sections/marker)");
    C(pristine && strstr(pristine, "CURRENT TASK SYSTEM STATE") == NULL &&
          strstr(pristine, "INSTRUCTIONS FOR THIS TURN") == NULL && strstr(pristine, "<system-reminder>") == NULL,
      "pristine stays pure even when request will carry the todo reminder");

    /* 模拟调用方编排: 把纯净 prompt 写回 group history[0] */
    if (pristine)
    {
        cJSON *sys_hist = cJSON_CreateObject();
        cJSON_AddStringToObject(sys_hist, "role", "system");
        cJSON_AddStringToObject(sys_hist, "content", pristine);
        cJSON_AddItemToArray(g2->history, sys_hist);
    }
    C(cJSON_GetArraySize(g2->history) == 1 &&
          strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetArrayItem(g2->history, 0), "content")), pristine) ==
              0,
      "caller-side write-back keeps shared history pure");

    cJSON *tm = edc_context_make_todo_reminder(t2);
    C(tm != NULL && is_todo_reminder(cJSON_GetStringValue(cJSON_GetObjectItem(tm, "content"))),
      "fallback+todo tail reminder carries the list state");
    C(strstr(cJSON_GetStringValue(cJSON_GetObjectItem(tm, "content")),
             "## 🚨 CURRENT TASK SYSTEM STATE\n- [•] plan [high]\n") != NULL &&
          strstr(cJSON_GetStringValue(cJSON_GetObjectItem(tm, "content")),
                 "NEXT SINGLE ACTION must be calling todowrite") != NULL,
      "tail reminder carries fresh list state + per-turn sync instruction");
    cJSON_Delete(tm);

    /* 写回后再次组装 → 走引用分支 (history[0] 纯净), 且仍无 todo 混入头部 */
    msgs = cJSON_CreateArray();
    pristine = NULL;
    n = edc_context_append_system_part(msgs, conf, g2, NULL, &pristine);
    C(n == 1 && pristine == NULL, "after write-back: head = reference, no pristine");
    m0 = cJSON_GetArrayItem(msgs, 0);
    C(m0 != NULL && (m0->type & cJSON_IsReference) && m0->child == cJSON_GetArrayItem(g2->history, 0)->child,
      "post-write-back request references history[0]");
    const char *pw = cJSON_GetStringValue(cJSON_GetObjectItem(m0, "content"));
    C(head_has_no_reminder(pw), "referenced history[0] is pure (todo only at tail)");
    cJSON_Delete(msgs);
    sdsfree(pristine);
    edc_task_group_free(g2); /* 连带释放 t2 / todos */
    sdsfree(conf->workspace);
    free(conf);
}

int main(void)
{
    test_attach_and_reminder();
    test_no_list_planning_guide();
    test_validation_gate();
    test_render_boundaries();
    test_state_summary_all_done();
    test_todo_sync_gap_throttle();
    test_tools_todo_resets_gap();
    test_append_system_part();

    printf("\n%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
