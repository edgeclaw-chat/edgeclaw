/* L2 session 记忆的工具层 (P4b): search / list / get / write。

 * 走**真工具链**: 建真 toolbox + 真 call_ctx (带真 task → 属主 session),
 * 用 `edc_toolbox_exec()` 按名字调用 —— 覆盖参数解析、属主解析、结果与 error。
 * 捕获回调经 `ctx->priv` 拿状态 (与真链路同一条 ctx)。 */
#include "edc_compat.h"
#include "edc_config.h"
#include "edc_error.h"
#include "edc_session.h"
#include "edc_session_memory.h"
#include "edc_task.h"
#include "edc_task_group.h"
#include "edc_toolbox.h"
#include "edc_tools_memory.h"
#include "log.h"
#include "sds.h"
#include <cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failed = 0;
static int g_passed = 0;

#define TEST(name)                                                                                                     \
    do                                                                                                                 \
    {                                                                                                                  \
        log_info("  TEST: %s", name);                                                                                  \
    } while (0)

#define CHECK(cond, msg)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            log_error("    FAIL: %s", msg);                                                                            \
            g_failed++;                                                                                                \
        }                                                                                                              \
        else                                                                                                           \
        {                                                                                                              \
            g_passed++;                                                                                                \
        }                                                                                                              \
    } while (0)

struct capture
{
    sds result;
    EdcError *error;
    int calls;
};

static void capture_cb(struct edc_toolbox_call_ctx *ctx, sds result, EdcError *error)
{
    struct capture *c = (struct capture *)ctx->priv;
    c->calls++;
    if (c->result)
        sdsfree(c->result);
    c->result = result ? sdsdup(result) : NULL;
    c->error = error; /* 所有权归接收方 (本测试随后释放) */
}

/* 跑一个工具: name + 一段 JSON 参数 */
static struct capture run_tool(struct edc_toolbox *tb, struct edc_task *task, const char *name, const char *args_json)
{
    struct capture c = {0};
    struct edc_toolbox_call_ctx *ctx = edc_toolbox_call_ctx_new(tb, edc_channel_adapter, NULL, task, &c);
    cJSON *args = args_json ? cJSON_Parse(args_json) : NULL;
    if (!args)
        args = cJSON_CreateObject();
    edc_toolbox_exec(ctx, name, args, capture_cb);
    cJSON_Delete(args);
    edc_toolbox_call_ctx_free(ctx);
    return c;
}

static void free_capture(struct capture *c)
{
    if (c->result)
        sdsfree(c->result);
    if (c->error)
        edc_error_free(c->error);
    memset(c, 0, sizeof(*c));
}

int main(void)
{
    struct edc_config *cfg = g_new0(struct edc_config, 1);
    cfg->workspace = sdsnew("/tmp/test_tools_memory");
    cfg->tool_max_output_size = 10000;

    struct edc_session *session = edc_session_new(cfg, "s1", "S1");
    struct edc_task *task = edc_task_new("100", "s1", "100", "把构建迁到 bazel", edc_channel_adapter);
    struct edc_task_group *group = edc_task_group_new(task);
    edc_session_add_group(session, group); /* 这一步挂上 group->session (属主) */

    struct edc_toolbox *tb = edc_toolbox_new(cfg, NULL, NULL);
    edc_tools_memory_register(tb);

    TEST("memory_write: 落条目 + 一条 CREATED 事件 + 记来源");
    struct capture w = run_tool(tb, task, "memory_write",
                                "{\"title\":\"构建入口\",\"summary\":\"唯一入口是 bazelisk\","
                                "\"detail\":\"明细：CI 与本地都用 bazelisk；不要再用 make。\"}");
    CHECK(w.calls == 1 && w.error == NULL, "工具成功返回");
    CHECK(w.result && strstr(w.result, "m1") != NULL, "结果里给出新 id");
    CHECK(session->memo->entry_count == 1, "条目落了一条");
    CHECK(session->memo->event_count == 1, "事件落了一条");
    CHECK(session->memo->events[0].kind == EDC_MEMO_CREATED, "事件是 CREATED");
    CHECK(session->memo->next_seq == 1 && session->memo->events[0].seq == 1, "seq 从 1 起");
    const struct edc_memo_entry *e1 = edc_session_memory_entry(session->memo, "m1");
    CHECK(e1 != NULL && e1->created_seq == 1 && e1->updated_seq == 1, "条目的两个 seq 记上了");
    CHECK(e1 && e1->proposed_by && strcmp(e1->proposed_by, "100") == 0, "proposed_by = 发起写的 task");
    CHECK(e1 && e1->sources && strcmp(e1->sources, "100") == 0, "sources = 所属组");
    free_capture(&w);

    TEST("memory_write: 缺 title 报错且不落地");
    struct capture bad = run_tool(tb, task, "memory_write", "{\"summary\":\"x\"}");
    CHECK(bad.error != NULL, "返回 error");
    CHECK(session->memo->entry_count == 1, "条目数没变");
    free_capture(&bad);

    TEST("memory_write: 第二次写拿到 m2 (号永不复用)");
    struct capture w2 = run_tool(tb, task, "memory_write", "{\"title\":\"沙箱\",\"summary\":\"uid 1000 + sudo\"}");
    CHECK(w2.result && strstr(w2.result, "m2") != NULL, "新 id = m2");
    CHECK(session->memo->next_entry_id == 2, "号抬到 2");
    CHECK(session->memo->event_count == 2 && session->memo->events[1].seq == 2, "第二条事件 seq = 2");
    free_capture(&w2);

    TEST("memory_list: 索引行 + 总数; offset/n 生效");
    struct capture l = run_tool(tb, task, "memory_list", "{}");
    CHECK(l.error == NULL, "成功");
    CHECK(l.result && strstr(l.result, "[m1] 构建入口 — 唯一入口是 bazelisk") != NULL, "第一行");
    CHECK(l.result && strstr(l.result, "[m2] 沙箱 — uid 1000 + sudo") != NULL, "第二行");
    CHECK(l.result && strstr(l.result, "of 2") != NULL, "报了总数");
    free_capture(&l);
    struct capture l1 = run_tool(tb, task, "memory_list", "{\"offset\":1,\"n\":1}");
    CHECK(l1.result && strstr(l1.result, "[m2]") != NULL && strstr(l1.result, "[m1]") == NULL, "翻页只给 m2");
    free_capture(&l1);

    TEST("memory_search: 命中摘要 / 明细; 没命中也给计数");
    struct capture s1 = run_tool(tb, task, "memory_search", "{\"query\":\"沙箱\"}");
    CHECK(s1.result && strstr(s1.result, "1 hit") != NULL && strstr(s1.result, "[m2]") != NULL, "命中 1 条");
    free_capture(&s1);
    struct capture s2 = run_tool(tb, task, "memory_search", "{\"query\":\"bazelisk\"}");
    CHECK(s2.result && strstr(s2.result, "[m1]") != NULL, "命中明细里的词");
    free_capture(&s2);
    struct capture s3 = run_tool(tb, task, "memory_search", "{\"query\":\"不存在的词\"}");
    CHECK(s3.error == NULL && s3.result && strstr(s3.result, "0 hit") != NULL, "0 命中也是正常结果");
    free_capture(&s3);
    struct capture s4 = run_tool(tb, task, "memory_search", "{}");
    CHECK(s4.error != NULL, "缺 query 报错");
    free_capture(&s4);

    TEST("memory_get: 明细全文 / 未知 id 报错");
    struct capture g1 = run_tool(tb, task, "memory_get", "{\"id\":\"m1\"}");
    CHECK(g1.error == NULL && g1.result && strstr(g1.result, "不要再用 make") != NULL, "明细回来");
    free_capture(&g1);
    struct capture g2 = run_tool(tb, task, "memory_get", "{\"id\":\"m99\"}");
    CHECK(g2.error != NULL && g2.result && strstr(g2.result, "m99") != NULL, "未知 id: 报错 + 说明");
    free_capture(&g2);

    TEST("读工具是只读的: 事件表与水位纹丝不动");
    size_t events_before = session->memo->event_count;
    uint64_t seq_before = session->memo->next_seq;
    struct capture r1 = run_tool(tb, task, "memory_list", "{}");
    struct capture r2 = run_tool(tb, task, "memory_get", "{\"id\":\"m1\"}");
    CHECK(session->memo->event_count == events_before && session->memo->next_seq == seq_before,
          "读完之后事件表与 next_seq 没变");
    free_capture(&r1);
    free_capture(&r2);

    TEST("没有属主 session 的调用: 报错而不是崩");
    struct edc_task *orphan = edc_task_new("200", "s1", "200", "孤儿", edc_channel_adapter);
    struct capture o = run_tool(tb, orphan, "memory_list", "{}");
    CHECK(o.error != NULL && o.result != NULL, "拿不到 session 记忆 ⇒ error + 说明");
    free_capture(&o);
    edc_task_free(orphan);

    edc_toolbox_free(tb);
    edc_session_free(session); /* 连带释放 group / task / memo */
    edc_config_free(cfg);

    printf("\n%d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
