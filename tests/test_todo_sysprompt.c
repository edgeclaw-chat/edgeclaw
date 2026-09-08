/* 验证: task 的 todo 状态以"运行时 reminder"的形式、在每次 LLM 请求的消息
 * 列表【最末尾】追加为 role=system 消息 (req->tail_system, llm actor 在
 * history + 当前 turn 之后发出) —— 而不是拼进头部 system prompt。这样每轮
 * 请求都是重新计算、绝对新鲜的提醒, 紧贴模型开始生成的位置, 长上下文里
 * 不易被遗忘。
 *
 * 链路 (生产代码, 本测试逐一验证可直接验证的环节):
 *   1. update_todolist 工具 (edc_tools_todo.c) 经真实 toolbox 执行后把校验
 *      通过的 items 挂到 ctx->task->todolist (整体替换) —— 本测试执行真实工具。
 *   2. 工具回显 content == task->todolist 经 edc_tools_todo_render_md 的渲染
 *      文本 (同一渲染函数); reminder 里的清单行与其同格式 (共享 render_body)。
 *   3. edc_agent_cb_send_llm 每次组装请求时调用
 *      edc_context_make_todo_reminder (edc_context.c, 实现于生产代码, 在此
 *      直测): 有清单 → role=system 的 reminder 消息 (维护引导 + 状态摘要 +
 *      清单行, 不带 base, 独立成消息); 无清单且 task 非空 → 短规划引导;
 *      task 为 NULL → NULL。副本语义: 不改共享 history[0] / task。
 *   4. 头部 edc_context_append_system_part 恢复纯净: 只挂 group memory +
 *      system prompt (引用/现拼), 不含任何 todo 内容。
 *
 * 本测试不依赖 secret/网络/真实 home: 不构造 agent, 直接驱动 toolbox 与 task。
 */
#include "edc_context.h"
#include "edc_channel_actor.h"
#include "edc_compat.h"
#include "edc_config.h"
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
#define C(cond, ...)                                                       \
    do                                                                     \
    {                                                                      \
        if (!(cond))                                                       \
        {                                                                  \
            printf("    FAIL: " __VA_ARGS__);                              \
            printf("\n");                                                  \
            failed++;                                                      \
        }                                                                  \
        else                                                               \
        {                                                                  \
            passed++;                                                      \
        }                                                                  \
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
    return content_starts_with(c, "<system-reminder>\n## Task Todo List") &&
           content_ends_with(c, "\n</system-reminder>");
}

static int is_planning_reminder(const char *c)
{
    return content_starts_with(c, "<system-reminder>\n## Task Planning") &&
           content_ends_with(c, "\n</system-reminder>");
}

/* 头部 system 部分必须连 reminder 标签都不出现 (标签即提醒的标记, 头部只准
 * 有 group memory + 纯净 system prompt)。 */
static int head_has_no_reminder(const char *c)
{
    return c && strstr(c, "<system-reminder>") == NULL && strstr(c, "## Task Todo List") == NULL &&
           strstr(c, "## Task Planning") == NULL;
}

/* ── 1. 真实 update_todolist 执行: 挂载 task->todolist; reminder 组装 ── */

static void test_attach_and_reminder(void)
{
    T("update_todolist attaches items to task; make_todo_reminder builds tail reminder");

    struct edc_config *conf = g_new0(struct edc_config, 1);
    struct edc_toolbox *tb = edc_toolbox_new(conf, NULL, NULL);
    struct edc_task *task = edc_task_new("t-attach", "s", "g", "test", edc_channel_cli);
    C(task->todolist == NULL, "fresh task has no todolist");

    cJSON *args = cJSON_Parse("{"
                              "\"items\":["
                              "{\"content\":\"step one\",\"status\":\"pending\",\"activeForm\":\"\"},"
                              "{\"content\":\"step two\",\"status\":\"in_progress\",\"activeForm\":\"reading code\"},"
                              "{\"content\":\"step three\",\"status\":\"completed\",\"activeForm\":\"\"}"
                              "]}");
    C(args != NULL, "args json parses");

    struct edc_toolbox_call_ctx *ctx =
        edc_toolbox_call_ctx_new(tb, edc_channel_cli, (void *)(intptr_t)1, task, NULL);
    edc_toolbox_exec(ctx, "update_todolist", args, capture_cb);

    C(g_err == NULL, "no validation error");
    C(g_result != NULL, "tool returned a result");
    C(task->todolist != NULL, "items attached to task->todolist");
    C(cJSON_GetArraySize(task->todolist) == 3, "todolist has 3 items");
    C(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetArrayItem(task->todolist, 1), "status")),
             "in_progress") == 0,
      "item 2 kept in_progress status");

    /* 工具回显 == task->todolist 的完整渲染 (同一 render_md) */
    sds md = edc_tools_todo_render_md(task->todolist);
    C(md != NULL, "render_md returns md for attached list");
    C(strcmp(md, g_result) == 0, "tool result == render of attached list (same renderer)");
    C(strstr(md, "- [ ] step one") != NULL, "pending item rendered");
    C(strstr(md, "- [ ] step two <- reading code") != NULL, "in_progress with activeForm rendered");
    C(strstr(md, "- [x] step three") != NULL, "completed item rendered");
    C(strstr(md, "1/3 completed") != NULL, "summary rendered");

    /* make_todo_reminder: 独立 role=system 消息, content 即 reminder (无 base
     * 前缀 —— 不再拼进头部 system prompt) */
    cJSON *sys = edc_context_make_todo_reminder(task);
    C(sys != NULL, "make_todo_reminder returns a message for task with list");
    C(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(sys, "role")), "system") == 0, "role is system");
    const char *content = cJSON_GetStringValue(cJSON_GetObjectItem(sys, "content"));
    C(content != NULL, "content present");

    /* (a) 消息以 <system-reminder> 标签包裹, 开标签后第一行即本清单的章节
     *     标题 (独立成条, 不再是 base 的追加尾巴) */
    C(is_todo_reminder(content),
      "reminder wrapped in <system-reminder> tags, opens with its own section title");
    C(strstr(content, "SYSTEM_BASE") == NULL, "no base prompt text inside reminder");
    C(strstr(content, "live progress record") != NULL, "section framed as live progress record");
    C(strstr(content, "recomputed and appended as a fresh system message at the end of every request") != NULL &&
          strstr(content, "trust it over update_todolist echoes") != NULL,
      "tail-reminder note (fresh at end of every request; echoes may be trimmed)");

    /* (b) 提醒规则: 何时调用 update_todolist、状态如何迁移 */
    C(strstr(content, "update_todolist") != NULL, "reminder to call update_todolist present");
    C(strstr(content, "FULL updated list") != NULL, "whole-list replacement stated");
    C(strstr(content, "\"in_progress\"") != NULL && strstr(content, "activeForm") != NULL,
      "start-item guidance (in_progress + activeForm) present");
    C(strstr(content, "\"completed\"") != NULL, "finish-item guidance present");
    C(strstr(content, "ONE item may be in_progress") != NULL, "single-in_progress rule present");
    C(strstr(content, "replaces the entire list (max 20 items)") != NULL,
      "max-20 + replace-all rule present");

    /* (c) 当前执行状态摘要: 一眼可读的计数 + 正在做 + 下一个待办 */
    C(strstr(content, "Current state:\n- 1/3 completed\n- in progress: \"step two\" (reading code)\n"
                      "- next pending: \"step one\"") != NULL,
      "state summary shows counts / in-progress w/ activeForm / next pending");

    /* (d) 清单行与工具回显同格式 (共享 render_body) */
    C(strstr(content, "- [ ] step one\n- [ ] step two <- reading code\n- [x] step three\n") != NULL,
      "checklist lines identical to tool rendering");

    /* (e) 不是 render_md 的干贴: 无裸 "# TODO" 头 */
    C(strstr(content, "# TODO\n") == NULL, "bare render_md header is NOT duplicated");

    /* 副本语义: 返回的是未挂到任何数组的新对象 */
    C(sys->prev == NULL && sys->next == NULL, "sys msg is detached (owned by caller)");

    cJSON_Delete(sys);
    sdsfree(md);
    reset_capture();
    edc_toolbox_call_ctx_free(ctx);
    cJSON_Delete(args);

    /* ── 清空清单 → 摘除挂载; 之后 reminder 退回"规划引导"形态 ── */
    T("empty update_todolist detaches list; reminder becomes planning guide");
    cJSON *empty_args = cJSON_Parse("{\"items\":[]}");
    struct edc_toolbox_call_ctx *ctx2 =
        edc_toolbox_call_ctx_new(tb, edc_channel_cli, (void *)(intptr_t)2, task, NULL);
    edc_toolbox_exec(ctx2, "update_todolist", empty_args, capture_cb);
    C(g_err == NULL, "no error on empty list");
    C(g_result && strcmp(g_result, "No todos") == 0, "empty list tool result is 'No todos'");
    C(task->todolist == NULL, "todolist detached after clearing");
    cJSON *g2 = edc_context_make_todo_reminder(task);
    C(g2 != NULL, "cleared task still gets a tail reminder");
    const char *gc = cJSON_GetStringValue(cJSON_GetObjectItem(g2, "content"));
    C(is_planning_reminder(gc),
      "cleared list -> planning guide (encourages creating a fresh plan)");
    C(strstr(gc, "## Task Todo List") == NULL, "planning guide carries no maintenance block");
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
    struct edc_task *task = edc_task_new("t-nolist", "s", "g", "no list", edc_channel_cli);
    cJSON *r = edc_context_make_todo_reminder(task);
    C(r != NULL, "task without list still gets a tail message (planning guide)");
    const char *c = cJSON_GetStringValue(cJSON_GetObjectItem(r, "content"));
    C(is_planning_reminder(c), "no list -> planning guide");
    C(strstr(c, "update_todolist") != NULL && strstr(c, "multi-step") != NULL,
      "planning guide mentions creating list for multi-step work");
    C(strstr(c, "## Task Todo List") == NULL, "planning guide has no maintenance block");
    cJSON_Delete(r);
    C(edc_context_make_todo_reminder(NULL) == NULL, "NULL task -> NULL (no reminder)");
    edc_task_free(task);

    /* 空数组挂载 (防御路径): 等价于无清单 → 规划引导 */
    struct edc_config *conf = g_new0(struct edc_config, 1);
    struct edc_toolbox *tb = edc_toolbox_new(conf, NULL, NULL);
    struct edc_task *t2 = edc_task_new("t-empty", "s", "g", "empty", edc_channel_cli);
    t2->todolist = cJSON_CreateArray();
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
    struct edc_task *task = edc_task_new("t-bad", "s", "g", "bad", edc_channel_cli);

    cJSON *bad = cJSON_Parse("{\"items\":[{\"content\":\"x\",\"status\":\"bogus\",\"activeForm\":\"\"}]}");
    struct edc_toolbox_call_ctx *ctx =
        edc_toolbox_call_ctx_new(tb, edc_channel_cli, (void *)(intptr_t)3, task, NULL);
    edc_toolbox_exec(ctx, "update_todolist", bad, capture_cb);
    C(g_err != NULL, "invalid status produces error");
    if (g_err)
        printf("      (validation error: %s)\n", g_err->message ? g_err->message : "(null)");
    C(g_result == NULL || strstr(g_result, "invalid") != NULL, "error text mentions invalid status");
    C(task->todolist == NULL, "list NOT attached after validation failure");
    cJSON *r = edc_context_make_todo_reminder(task);
    C(r != NULL && is_planning_reminder(cJSON_GetStringValue(cJSON_GetObjectItem(r, "content"))),
      "invalid list never attached -> planning guide only");
    cJSON_Delete(r);
    reset_capture();
    edc_toolbox_call_ctx_free(ctx);
    cJSON_Delete(bad);
    edc_task_free(task);
    edc_toolbox_free(tb);
    free(conf);
}

/* ── 4. 渲染函数边界 ── */

static void test_render_boundaries(void)
{
    T("render_md(NULL / empty array) renders an empty checklist");
    sds md = edc_tools_todo_render_md(NULL);
    C(strcmp(md, "# TODO\n\n0/0 completed") == 0, "NULL items renders empty checklist");
    sdsfree(md);
    cJSON *arr = cJSON_CreateArray();
    md = edc_tools_todo_render_md(arr);
    C(strcmp(md, "# TODO\n\n0/0 completed") == 0, "empty array renders empty checklist");
    sdsfree(md);
    cJSON_Delete(arr);

    T("render_body(NULL) returns empty sds, count 0");
    int completed = -1;
    sds body = edc_tools_todo_render_body(NULL, &completed);
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
    struct edc_task *task = edc_task_new("t-done", "s", "g", "all done", edc_channel_cli);

    task->todolist = cJSON_Parse("["
                                 "{\"content\":\"pack\",\"status\":\"completed\",\"activeForm\":\"\"},"
                                 "{\"content\":\"ship\",\"status\":\"completed\",\"activeForm\":\"\"}"
                                 "]");
    C(task->todolist != NULL && cJSON_GetArraySize(task->todolist) == 2, "attached 2 completed items");

    cJSON *sys = edc_context_make_todo_reminder(task);
    const char *content = cJSON_GetStringValue(cJSON_GetObjectItem(sys, "content"));
    C(is_todo_reminder(content), "all-done reminder wrapped, opens with its own title");
    C(strstr(content, "Current state:\n- 2/2 completed\n- all items completed") != NULL,
      "state shows full completion");
    C(strstr(content, "nothing left to do") != NULL, "explicit no-work-left note");
    C(strstr(content, "- [x] pack\n- [x] ship\n") != NULL, "completed checklist lines present");
    C(strstr(content, "do not clear it") != NULL, "keep-finished-list rule still present");

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

static void test_append_system_part(void)
{
    T("append_system_part: pure head (ref/copy), group memory order, fallback pristine");

    /* ── history 分支: 引用 history[0], 头部无规划引导 ── */
    struct edc_task *t1 = edc_task_new("ap-1", "s", "g", "no list", edc_channel_cli);
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
    int n = edc_context_append_system_part(msgs, NULL, g1, &pristine);
    C(n == 1, "head = base reference only (no planning guide in head anymore)");
    C(pristine == NULL, "no pristine in history branch");
    cJSON *m0 = cJSON_GetArrayItem(msgs, 0);
    C(m0 != NULL && (m0->type & cJSON_IsReference) && m0->child == sys0->child,
      "history[0] attached by REFERENCE (aliases role/content), not deep copy");
    C(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(m0, "content")), "SYS0_BASE") == 0,
      "referenced content readable through array");
    C(strstr(cJSON_GetStringValue(cJSON_GetObjectItem(m0, "content")), "## Task Planning") == NULL &&
          strstr(cJSON_GetStringValue(cJSON_GetObjectItem(m0, "content")), "## Task Todo List") == NULL &&
          strstr(cJSON_GetStringValue(cJSON_GetObjectItem(m0, "content")), "<system-reminder>") == NULL,
      "head message stays pure (no todo sections, no reminder marker)");
    cJSON_Delete(msgs);
    C(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetArrayItem(g1->history, 0), "content")),
             "SYS0_BASE") == 0,
      "history[0] survives msgs delete (reference did not transfer/copy)");

    /* todo 引导独立于头部: no-list task 的 reminder = 规划引导 (owned), 模拟
     * agent 把它作为队尾消息塞进请求数组 → 落在 base 引用之后 */
    cJSON *tail = edc_context_make_todo_reminder(t1);
    C(tail != NULL && !(tail->type & cJSON_IsReference), "planning-guide reminder is an owned msg");
    C(is_planning_reminder(cJSON_GetStringValue(cJSON_GetObjectItem(tail, "content"))),
      "no-list tail message is the task-planning guide");
    C(strstr(cJSON_GetStringValue(cJSON_GetObjectItem(tail, "content")), "update_todolist") != NULL &&
          strstr(cJSON_GetStringValue(cJSON_GetObjectItem(tail, "content")), "multi-step") != NULL,
      "planning guide mentions creating list for multi-step work");
    msgs = cJSON_CreateArray();
    edc_context_append_system_part(msgs, NULL, g1, &pristine);
    cJSON_AddItemToArray(msgs, tail); /* req->tail_system 位置 (请求数组队尾) */
    C(cJSON_GetArraySize(msgs) == 2, "request = head ref + tail reminder");
    C(cJSON_GetArrayItem(msgs, 1) == tail, "reminder is the LAST message of the request");
    C(strstr(cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetArrayItem(msgs, 0), "content")),
             "## Task Planning") == NULL,
      "no planning guide inside the head part");
    cJSON_Delete(msgs); /* 连带释放 tail reminder */

    /* ── history 分支: group memory 在前 + 有 todo → 头部仍纯净, todo 只在
     *    队尾 reminder; 共享 history[0] 不被改动 ── */
    g1->memory = sdsnew("MEM-SUMMARY");
    t1->todolist = cJSON_Parse("[{\"content\":\"do it\",\"status\":\"pending\",\"activeForm\":\"\"}]");
    C(t1->todolist != NULL && cJSON_GetArraySize(t1->todolist) == 1, "attached 1 todo item");
    msgs = cJSON_CreateArray();
    pristine = NULL;
    n = edc_context_append_system_part(msgs, NULL, g1, &pristine);
    C(n == 2, "group memory + base ref -> 2 head appends");
    C(pristine == NULL, "still no pristine (history exists)");
    cJSON *mem0 = cJSON_GetArrayItem(msgs, 0);
    C(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(mem0, "content")), "## Group Memory\nMEM-SUMMARY") == 0,
      "group memory message comes first");
    cJSON *m1b = cJSON_GetArrayItem(msgs, 1);
    C(m1b != NULL && (m1b->type & cJSON_IsReference) && m1b->child == sys0->child,
      "base attached by reference (not a todo-injected copy)");
    const char *h1 = cJSON_GetStringValue(cJSON_GetObjectItem(m1b, "content"));
    C(h1 && strcmp(h1, "SYS0_BASE") == 0, "head base stays PURE even when task has a todo list");
    C(strstr(h1, "## Task Todo List") == NULL && strstr(h1, "## Task Planning") == NULL &&
          strstr(h1, "<system-reminder>") == NULL,
      "no todo sections / reminder marker anywhere in the head");

    cJSON *r2 = edc_context_make_todo_reminder(t1);
    C(r2 != NULL && !(r2->type & cJSON_IsReference), "todo reminder is an owned msg");
    const char *rc = cJSON_GetStringValue(cJSON_GetObjectItem(r2, "content"));
    C(is_todo_reminder(rc), "todo reminder wrapped, opens with its title");
    C(strstr(rc, "Current state:\n- 0/1 completed\n- next pending: \"do it\"") != NULL,
      "fresh state summary inside reminder");
    C(strstr(rc, "## Task Planning") == NULL,
      "list present: planning guide NOT duplicated (maintenance guide takes over)");
    C(strstr(rc, "- [ ] do it\n") != NULL, "checklist line present");

    /* 模拟 agent: reminder 经 req->tail_system 成为请求数组队尾 */
    cJSON_AddItemToArray(msgs, r2);
    C(cJSON_GetArraySize(msgs) == 3 && cJSON_GetArrayItem(msgs, 2) == r2,
      "todo reminder appended as LAST message, after memory + base ref");
    C(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(sys0, "content")), "SYS0_BASE") == 0,
      "shared history[0] untouched by reminder");
    cJSON_Delete(msgs);
    edc_task_group_free(g1); /* 连带释放 t1 / sys0 / usr / todolist / memory */

    /* ── fallback: group 空历史 + conf → 现拼纯净 prompt 经 out_pristine 交还;
     *    写回 group history 属调用方; todo 仍只在队尾 reminder ── */
    struct edc_config *conf = g_new0(struct edc_config, 1);
    conf->workspace = sdsnew("/tmp/test-session");
    struct edc_task *t2 = edc_task_new("ap-2", "s", "g", "no list", edc_channel_cli);
    struct edc_task_group *g2 = edc_task_group_new(t2);

    msgs = cJSON_CreateArray();
    pristine = NULL;
    n = edc_context_append_system_part(msgs, conf, g2, &pristine);
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
    C(cJSON_GetArraySize(msgs) == 2 && cJSON_GetArrayItem(msgs, 1) == pg,
      "planning guide is the request tail");
    cJSON_Delete(msgs);
    sdsfree(pristine);

    /* fallback + todo: 头部纯净, todo 只在队尾 reminder */
    t2->todolist = cJSON_Parse("[{\"content\":\"plan\",\"status\":\"in_progress\",\"activeForm\":\"working\"}]");
    msgs = cJSON_CreateArray();
    pristine = NULL;
    n = edc_context_append_system_part(msgs, conf, g2, &pristine);
    C(n == 1, "fallback+todo head: one pure sys msg");
    m0 = cJSON_GetArrayItem(msgs, 0);
    const char *ft = cJSON_GetStringValue(cJSON_GetObjectItem(m0, "content"));
    C(ft && strncmp(ft, "# EdgeClaw", 10) == 0 && strstr(ft, "## Task Todo List") == NULL &&
          strstr(ft, "<system-reminder>") == NULL,
      "fallback+todo request head = fresh pure base (no todo section)");
    C(pristine && strstr(pristine, "## Task Todo List") == NULL &&
          strstr(pristine, "## Task Planning") == NULL && strstr(pristine, "<system-reminder>") == NULL,
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
          strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetArrayItem(g2->history, 0), "content")),
                 pristine) == 0,
      "caller-side write-back keeps shared history pure");

    cJSON *tm = edc_context_make_todo_reminder(t2);
    C(tm != NULL && is_todo_reminder(cJSON_GetStringValue(cJSON_GetObjectItem(tm, "content"))),
      "fallback+todo tail reminder carries the list state");
    C(strstr(cJSON_GetStringValue(cJSON_GetObjectItem(tm, "content")),
             "Current state:\n- 0/1 completed\n- in progress: \"plan\" (working)") != NULL,
      "tail reminder state is recomputed fresh");
    cJSON_Delete(tm);

    /* 写回后再次组装 → 走引用分支 (history[0] 纯净), 且仍无 todo 混入头部 */
    msgs = cJSON_CreateArray();
    pristine = NULL;
    n = edc_context_append_system_part(msgs, conf, g2, &pristine);
    C(n == 1 && pristine == NULL, "after write-back: head = reference, no pristine");
    m0 = cJSON_GetArrayItem(msgs, 0);
    C(m0 != NULL && (m0->type & cJSON_IsReference) && m0->child == cJSON_GetArrayItem(g2->history, 0)->child,
      "post-write-back request references history[0]");
    const char *pw = cJSON_GetStringValue(cJSON_GetObjectItem(m0, "content"));
    C(head_has_no_reminder(pw), "referenced history[0] is pure (todo only at tail)");
    cJSON_Delete(msgs);
    sdsfree(pristine);
    edc_task_group_free(g2); /* 连带释放 t2 / todolist */
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
    test_append_system_part();

    printf("\n%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
