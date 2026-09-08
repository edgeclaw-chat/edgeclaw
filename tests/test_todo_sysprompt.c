/* 验证: task 的 todo list 会以"引导 + 当前状态 + 清单行"区块拼接到发给 LLM
 * 的 system prompt —— 不只是干贴渲染 md, 还让模型知道执行情况并提醒它及时
 * 调用 update_todolist 同步进度。
 *
 * 链路 (生产代码, 本测试逐一验证可直接验证的环节):
 *   1. update_todolist 工具 (edc_tools_todo.c) 经真实 toolbox 执行后把校验
 *      通过的 items 挂到 ctx->task->todolist (整体替换) —— 本测试执行真实工具。
 *   2. 工具回显 content == task->todolist 经 edc_tools_todo_render_md 的渲染
 *      文本 (同一渲染函数); 注入块里的清单行与其同格式 (共享 render_body)。
 *   3. edc_agent_cb_send_llm 每次组装请求时经 edc_context_append_system_part
 *      调用 edc_context_make_sys_with_todo (edc_context.c, 实现于生产代码,
 *      在此直测): 无清单/空清单 → 不注入 (NULL); 有清单 → 生成 role=system 的
 *      新消息, content = 原 system prompt + "\n\n" + [标题 + 使用规则提醒 +
 *      当前执行状态摘要 + 清单行] —— 用副本, 不改共享 history[0]。
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

/* ── 1. 真实 update_todolist 执行: 挂载 task->todolist + 注入助手 ── */

static void test_attach_and_inject(void)
{
    T("update_todolist attaches items to task; sys-msg helper injects guided block");

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

    /* make_sys_with_todo: 副本 system 消息 = base + 引导区块 (非干贴 md) */
    cJSON *sys = edc_context_make_sys_with_todo("SYSTEM_BASE", task);
    C(sys != NULL, "make_sys_with_todo returns a message");
    C(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(sys, "role")), "system") == 0, "role is system");
    const char *content = cJSON_GetStringValue(cJSON_GetObjectItem(sys, "content"));
    C(content != NULL, "content present");

    /* (a) 区块标题紧跟 base, 定位清单是权威进度记录 */
    C(strncmp(content, "SYSTEM_BASE\n\n## Task Todo List", 30) == 0,
      "content starts with base + section title");
    C(strstr(content, "live progress record") != NULL, "section framed as live progress record");
    C(strstr(content, "re-injected into this system prompt") != NULL &&
          strstr(content, "trust it over update_todolist echoes") != NULL,
      "authoritative-copy note (echoes may be trimmed)");

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

    /* (e) 不是 base + 完整 render_md 的干拼接: 无裸 "# TODO" 头 */
    C(strstr(content, "# TODO\n") == NULL, "bare render_md header is NOT duplicated");

    /* 副本语义: 返回的是未挂到任何数组的新对象 */
    C(sys->prev == NULL && sys->next == NULL, "sys msg is detached (owned by caller)");

    cJSON_Delete(sys);
    sdsfree(md);
    reset_capture();
    edc_toolbox_call_ctx_free(ctx);
    cJSON_Delete(args);

    /* ── 清空清单 → 摘除挂载, 后续不再注入 ── */
    T("empty update_todolist detaches list; helpers return NULL afterwards");
    cJSON *empty_args = cJSON_Parse("{\"items\":[]}");
    struct edc_toolbox_call_ctx *ctx2 =
        edc_toolbox_call_ctx_new(tb, edc_channel_cli, (void *)(intptr_t)2, task, NULL);
    edc_toolbox_exec(ctx2, "update_todolist", empty_args, capture_cb);
    C(g_err == NULL, "no error on empty list");
    C(g_result && strcmp(g_result, "No todos") == 0, "empty list tool result is 'No todos'");
    C(task->todolist == NULL, "todolist detached after clearing");
    C(edc_context_make_sys_with_todo("SYSTEM_BASE", task) == NULL, "no sys msg after clearing");
    reset_capture();
    edc_toolbox_call_ctx_free(ctx2);
    cJSON_Delete(empty_args);

    edc_task_free(task);
    edc_toolbox_free(tb);
    free(conf);
}

/* ── 2. 无清单 / 空清单 / NULL 安全性 ── */

static void test_no_list_no_inject(void)
{
    T("task without list / empty list / NULL task -> no injection");
    struct edc_task *task = edc_task_new("t-nolist", "s", "g", "no list", edc_channel_cli);
    C(edc_context_make_sys_with_todo("BASE", task) == NULL, "no sys msg without list");
    C(edc_context_make_sys_with_todo("BASE", NULL) == NULL, "NULL task safe (sys)");
    C(edc_context_make_sys_with_todo(NULL, task) == NULL, "NULL base + no list -> NULL");
    edc_task_free(task);

    /* 空数组挂载也不注入 (等价于无清单) */
    struct edc_config *conf = g_new0(struct edc_config, 1);
    struct edc_toolbox *tb = edc_toolbox_new(conf, NULL, NULL);
    struct edc_task *t2 = edc_task_new("t-empty", "s", "g", "empty", edc_channel_cli);
    /* 直接模拟工具挂一个空数组 (正常工具路径清空时是置 NULL, 这里验证防御) */
    t2->todolist = cJSON_CreateArray();
    C(edc_context_make_sys_with_todo("BASE", t2) == NULL, "empty array -> no sys msg");
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
    C(edc_context_make_sys_with_todo("BASE", task) == NULL, "no injection after validation failure");
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

    cJSON *sys = edc_context_make_sys_with_todo("BASE", task);
    const char *content = cJSON_GetStringValue(cJSON_GetObjectItem(sys, "content"));
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

/* ── 6. append_system_part: system 部分组装语义 ──
 * 直接驱动生产入口 edc_context_append_system_part, 验证迁移前后承诺的语义:
 *   - 有 history[0] + 无 todo → 引用 history[0] (不复制, msgs 删除后原消息还在)
 *   - 有 history[0] + 有 todo → 生成注入副本 (owned), history[0] 不被改动
 *   - group memory 摘要永远排在 system prompt 之前
 *   - group 无历史 + conf → 现拼纯净 prompt; todo 注入只进请求副本,
 *     *out_pristine 交还纯净文本 (写回 group history 是调用方编排职责) */

static void test_append_system_part(void)
{
    T("append_system_part: reference vs copy, group memory order, fallback pristine");

    /* ── history 分支: 无 todo → 引用 ── */
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
    int n = edc_context_append_system_part(msgs, NULL, g1, t1, &pristine);
    C(n == 1, "history branch appends one sys msg (conf not needed)");
    C(pristine == NULL, "no pristine in history branch");
    cJSON *m0 = cJSON_GetArrayItem(msgs, 0);
    C(m0 != NULL && (m0->type & cJSON_IsReference) && m0->child == sys0->child,
      "no-todo: history[0] attached by REFERENCE (aliases role/content), not deep copy");
    C(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(m0, "content")), "SYS0_BASE") == 0,
      "referenced content readable through array");
    cJSON_Delete(msgs);
    C(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetArrayItem(g1->history, 0), "content")),
             "SYS0_BASE") == 0,
      "history[0] survives msgs delete (reference did not transfer/copy)");

    /* ── history 分支: group memory + 有 todo → memory 在前, todo 副本在后,
     *    共享 history[0] 不被改动 ── */
    g1->memory = sdsnew("MEM-SUMMARY");
    t1->todolist = cJSON_Parse("[{\"content\":\"do it\",\"status\":\"pending\",\"activeForm\":\"\"}]");
    C(t1->todolist != NULL && cJSON_GetArraySize(t1->todolist) == 1, "attached 1 todo item");
    msgs = cJSON_CreateArray();
    pristine = NULL;
    n = edc_context_append_system_part(msgs, NULL, g1, t1, &pristine);
    C(n == 2, "memory + todo sys msg -> 2 appends");
    C(pristine == NULL, "still no pristine (history exists)");
    cJSON *mem0 = cJSON_GetArrayItem(msgs, 0);
    C(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(mem0, "content")), "## Group Memory\nMEM-SUMMARY") == 0,
      "group memory message comes first");
    cJSON *m1 = cJSON_GetArrayItem(msgs, 1);
    C(m1 != NULL && !(m1->type & cJSON_IsReference), "todo: sys msg is an owned copy");
    const char *c1 = cJSON_GetStringValue(cJSON_GetObjectItem(m1, "content"));
    C(c1 != NULL && strncmp(c1, "SYS0_BASE\n\n## Task Todo List", 28) == 0,
      "todo copy = history[0] base + injected section");
    C(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(sys0, "content")), "SYS0_BASE") == 0,
      "shared history[0] untouched by todo injection");
    cJSON_Delete(msgs);
    edc_task_group_free(g1); /* 连带释放 t1 / sys0 / usr / todolist / memory */

    /* ── fallback: group 空历史 + conf → 现拼纯净 prompt; todo 只进请求副本,
     *    纯净文本经 out_pristine 交还; 写回 group history 属调用方 ── */
    struct edc_config *conf = g_new0(struct edc_config, 1);
    conf->workspace = sdsnew("/tmp/test-session");
    struct edc_task *t2 = edc_task_new("ap-2", "s", "g", "no list", edc_channel_cli);
    struct edc_task_group *g2 = edc_task_group_new(t2);

    msgs = cJSON_CreateArray();
    pristine = NULL;
    n = edc_context_append_system_part(msgs, conf, g2, t2, &pristine);
    C(n == 1, "fallback appends one sys msg");
    C(pristine != NULL && strncmp(pristine, "# EdgeClaw", 10) == 0, "pristine = fresh base prompt");
    m0 = cJSON_GetArrayItem(msgs, 0);
    C(!(m0->type & cJSON_IsReference), "fallback msg is owned");
    const char *f0 = cJSON_GetStringValue(cJSON_GetObjectItem(m0, "content"));
    C(f0 && strcmp(f0, pristine) == 0, "no-todo fallback request content == pristine");
    C(strstr(f0, "## Task Todo List") == NULL, "pure prompt has no todo section");
    C(cJSON_GetArraySize(g2->history) == 0, "context does NOT write group history (caller's job)");
    cJSON_Delete(msgs);
    sdsfree(pristine);

    /* fallback + todo: 请求里是注入副本, pristine 仍纯净 */
    t2->todolist = cJSON_Parse("[{\"content\":\"plan\",\"status\":\"in_progress\",\"activeForm\":\"working\"}]");
    msgs = cJSON_CreateArray();
    pristine = NULL;
    n = edc_context_append_system_part(msgs, conf, g2, t2, &pristine);
    C(n == 1, "fallback+todo appends one sys msg");
    m0 = cJSON_GetArrayItem(msgs, 0);
    const char *ft = cJSON_GetStringValue(cJSON_GetObjectItem(m0, "content"));
    C(ft && strncmp(ft, "# EdgeClaw", 10) == 0 && strstr(ft, "## Task Todo List") != NULL,
      "fallback+todo request content = fresh base + injected section");
    C(pristine && strstr(pristine, "## Task Todo List") == NULL,
      "pristine stays pure even when request got the todo copy");

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

    cJSON_Delete(msgs);
    sdsfree(pristine);
    edc_task_group_free(g2); /* 连带释放 t2 / todolist */
    sdsfree(conf->workspace);
    free(conf);
}

int main(void)
{
    test_attach_and_inject();
    test_no_list_no_inject();
    test_validation_gate();
    test_render_boundaries();
    test_state_summary_all_done();
    test_append_system_part();

    printf("\n%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
