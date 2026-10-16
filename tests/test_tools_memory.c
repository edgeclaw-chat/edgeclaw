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
#include "edc_tools.h" /* struct edc_tool: the registration flags */
#include "edc_tools_memory.h"
#include <uthash.h> /* HASH_FIND_STR: the test looks tools up by name */
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

/* 回显 header 的小工具: 解析一段 JSON 参数 → 取 header 文本 (调用方 sdsfree) */
static sds hdr(const char *name, const char *args_json)
{
    cJSON *a = args_json ? cJSON_Parse(args_json) : NULL;
    sds h = edc_tools_memory_call_header(name, a);
    if (a)
        cJSON_Delete(a);
    return h;
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
    CHECK(w.result && strstr(w.result, ", 20") != NULL, "回显带创建时间（rev, 时间）");
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

    TEST("memory_search: 多正则分组命中; 非法正则逐项报, 不整体失败");
    struct capture s1 = run_tool(tb, task, "memory_search", "{\"patterns\":[\"^沙箱\",\"bazel|sudo\",\"不存在的词\"]}");
    CHECK(s1.error == NULL, "多正则搜索成功");
    CHECK(s1.result && strstr(s1.result, "1 hit(s) for /^沙箱/") != NULL && strstr(s1.result, "[m2]") != NULL,
          "正则1 锚定命中 1 条");
    CHECK(s1.result && strstr(s1.result, "2 hit(s) for /bazel|sudo/") != NULL && strstr(s1.result, "[m1]") != NULL,
          "正则2 交替命中 2 条");
    CHECK(s1.result && strstr(s1.result, "0 hit(s) for /不存在的词/") != NULL, "正则3 0 命中也是正常结果");
    free_capture(&s1);
    struct capture s5 = run_tool(tb, task, "memory_search", "{\"patterns\":[\"(\"]}");
    CHECK(s5.error == NULL && s5.result && strstr(s5.result, "invalid regex") != NULL, "非法正则逐项报错");
    free_capture(&s5);
    struct capture s4 = run_tool(tb, task, "memory_search", "{}");
    CHECK(s4.error != NULL, "缺 patterns 报错");
    free_capture(&s4);

    TEST("memory_get: 批量读; 未知 id 给说明而不是整体报错");
    struct capture g1 = run_tool(tb, task, "memory_get", "{\"ids\":[\"m1\",\"m99\"]}");
    CHECK(g1.error == NULL, "批量读成功");
    CHECK(g1.result && strstr(g1.result, "[m1] rev ") != NULL && strstr(g1.result, "不要再用 make") != NULL,
          "m1 明细 + rev 回来");
    CHECK(g1.result && strstr(g1.result, "created: ") != NULL && strstr(g1.result, "updated: ") != NULL,
          "明细带 created/updated 时间");
    CHECK(g1.result && strstr(g1.result, "[m99] (no such entry") != NULL, "m99 给说明");
    free_capture(&g1);
    struct capture g0 = run_tool(tb, task, "memory_get", "{}");
    CHECK(g0.error != NULL, "缺 ids 报错");
    free_capture(&g0);

    TEST("memory_update: 批量就地合并 (给的字段覆盖, 没给的保留) + UPDATED 事件");
    struct capture u = run_tool(tb, task, "memory_update",
                                "{\"entries\":[{\"id\":\"m1\",\"expected_rev\":1,\"summary\":\"唯一入口是 bazelisk (v2)\"}]}");
    CHECK(u.error == NULL && u.result && strstr(u.result, "m1") != NULL, "更新成功");
    const struct edc_memo_entry *e1u = edc_session_memory_entry(session->memo, "m1");
    CHECK(e1u && e1u->summary && strcmp(e1u->summary, "唯一入口是 bazelisk (v2)") == 0, "摘要被覆盖");
    CHECK(e1u && e1u->title && strcmp(e1u->title, "构建入口") == 0, "没给的 title 保留");
    CHECK(session->memo->events[session->memo->event_count - 1].kind == EDC_MEMO_UPDATED, "最新事件是 UPDATED");
    CHECK(e1u && e1u->updated_seq == session->memo->events[session->memo->event_count - 1].seq, "updated_seq 抬到新 seq");
    uint64_t m1_rev = e1u->updated_seq;
    free_capture(&u);

    TEST("memory_update: 缺 expected_rev ⇒ 参数校验报错");
    struct capture nrev = run_tool(tb, task, "memory_update", "{\"entries\":[{\"id\":\"m1\",\"summary\":\"x\"}]}");
    CHECK(nrev.error != NULL, "缺 expected_rev 报错");
    free_capture(&nrev);

    TEST("memory_update: 原子 —— 批里任一项 rev 过期 ⇒ 整批不落地");
    size_t c_events = session->memo->event_count;
    struct capture c = run_tool(tb, task, "memory_update",
                                "{\"entries\":[{\"id\":\"m1\",\"expected_rev\":1,\"summary\":\"stale overwrite\"}]}");
    CHECK(c.error != NULL, "被拒");
    CHECK(c.result && strstr(c.result, "stale expected_rev") != NULL, "说明冲突");
    const struct edc_memo_entry *e1c = edc_session_memory_entry(session->memo, "m1");
    CHECK(e1c && e1c->updated_seq == m1_rev, "rev 没动");
    CHECK(e1c && strcmp(e1c->summary, "唯一入口是 bazelisk (v2)") == 0, "没被旧快照覆盖");
    CHECK(session->memo->event_count == c_events, "没发事件");
    free_capture(&c);

    TEST("memory_get 的 rev 可用来批量成功更新");
    struct capture mg = run_tool(tb, task, "memory_get", "{\"ids\":[\"m1\"]}");
    CHECK(mg.error == NULL && mg.result && strstr(mg.result, "rev ") != NULL, "memory_get 带 rev");
    char upd[256];
    snprintf(upd, sizeof(upd), "{\"entries\":[{\"id\":\"m1\",\"expected_rev\":%llu,\"summary\":\"ok after re-read\"}]}",
             (unsigned long long)m1_rev);
    struct capture mg2 = run_tool(tb, task, "memory_update", upd);
    CHECK(mg2.error == NULL && mg2.result && strstr(mg2.result, "Updated 1 entr") != NULL, "带正确 rev 更新成功");
    free_capture(&mg);
    free_capture(&mg2);

    TEST("memory_update: 未知 id / 空更新 都报错且整批不落地");
    size_t u_events = session->memo->event_count;
    struct capture ub =
        run_tool(tb, task, "memory_update", "{\"entries\":[{\"id\":\"m99\",\"expected_rev\":1,\"summary\":\"x\"}]}");
    CHECK(ub.error != NULL && ub.result && strstr(ub.result, "unknown id") != NULL, "未知 id 报错");
    struct capture ue = run_tool(tb, task, "memory_update", "{\"entries\":[{\"id\":\"m1\",\"expected_rev\":1}]}");
    CHECK(ue.error != NULL, "什么都没给报错");
    CHECK(session->memo->event_count == u_events, "两条都没落地");
    free_capture(&ub);
    free_capture(&ue);

    TEST("memory_supersede: 列表形式; 标 retired + SUPERSEDED 事件; 条目不删");
    struct capture sp = run_tool(tb, task, "memory_supersede", "{\"ids\":[\"m2\"]}");
    CHECK(sp.error == NULL && sp.result && strstr(sp.result, "m2") != NULL, "作废成功");
    const struct edc_memo_entry *e2 = edc_session_memory_entry(session->memo, "m2");
    CHECK(e2 && e2->superseded, "标记 retired");
    CHECK(session->memo->entry_count == 2, "条目没被删");
    CHECK(session->memo->events[session->memo->event_count - 1].kind == EDC_MEMO_SUPERSEDED, "最新事件 SUPERSEDED");
    free_capture(&sp);

    TEST("memory_supersede: 原子 —— 列表含未知 id ⇒ 整批不退休");
    size_t s_events = session->memo->event_count;
    struct capture sb = run_tool(tb, task, "memory_supersede", "{\"ids\":[\"m1\",\"m99\"]}");
    CHECK(sb.error != NULL, "有未知 id ⇒ 报错");
    CHECK(sb.result && strstr(sb.result, "m99") != NULL, "点名未知 id");
    const struct edc_memo_entry *e1s = edc_session_memory_entry(session->memo, "m1");
    CHECK(e1s && !e1s->superseded, "m1 没被退休 (原子)");
    CHECK(session->memo->event_count == s_events, "没发事件");
    free_capture(&sb);

    TEST("memory_supersede: 空列表 / 已有条目重复退休 (幂等)");
    struct capture se = run_tool(tb, task, "memory_supersede", "{\"ids\":[]}");
    CHECK(se.error != NULL, "空列表报错");
    free_capture(&se);
    size_t idem_events = session->memo->event_count;
    struct capture si = run_tool(tb, task, "memory_supersede", "{\"ids\":[\"m2\"]}");
    CHECK(si.error == NULL, "重复退休不报错 (幂等)");
    CHECK(session->memo->event_count == idem_events, "已 retired ⇒ 不再发事件");
    free_capture(&si);

    TEST("策展任务 (sink_group) 写条目: sources = 被折叠的组, 不是自己的系统组");
    struct edc_task *curator = edc_task_new("300", "s1", "300", "curate", edc_channel_adapter);
    struct edc_task_group *cg = edc_task_group_new(curator);
    edc_session_add_group(session, cg); /* 挂上 group->session (属主) */
    curator->sink = EDC_SINK_SESSION_MEMORY;
    curator->sink_group = group; /* 被折叠的那个组 (id = "100") */
    struct capture wc = run_tool(tb, curator, "memory_write", "{\"title\":\"策展写的\",\"summary\":\"s\"}");
    CHECK(wc.error == NULL, "策展任务能写");
    const struct edc_memo_entry *ec = edc_session_memory_entry(session->memo, "m3");
    CHECK(ec && ec->sources && strcmp(ec->sources, "100") == 0, "sources = 被折叠组 id");
    free_capture(&wc);
    edc_session_remove_group(session, cg); /* 连带释放 task */
    edc_task_group_free(cg);

    TEST("memory_write: 新建时给出相关旧条目候选 (不拦截, 照样建)");
    size_t n_cnt = session->memo->entry_count;
    struct capture nudge = run_tool(tb, task, "memory_write",
                                    "{\"title\":\"构建入口\",\"summary\":\"CI 与本地都用 bazelisk\"}");
    CHECK(nudge.error == NULL, "照样建成功 (不拦截)");
    CHECK(session->memo->entry_count == n_cnt + 1, "条目 +1");
    CHECK(nudge.result && strstr(nudge.result, "look related") != NULL, "结果里带相关提示");
    CHECK(nudge.result && strstr(nudge.result, "[m1]") != NULL, "点名了相关的 m1");
    free_capture(&nudge);

    TEST("读工具是只读的: 事件表与水位纹丝不动");
    size_t events_before = session->memo->event_count;
    uint64_t seq_before = session->memo->next_seq;
    struct capture r1 = run_tool(tb, task, "memory_list", "{}");
    struct capture r2 = run_tool(tb, task, "memory_get", "{\"ids\":[\"m1\"]}");
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

    TEST("回显 header 带输入参数: 用户能看到 search 什么 / get 什么");
    {
        sds h = hdr("memory_search", "{\"patterns\":[\"^沙箱\",\"bazel|sudo\"]}");
        CHECK(h && strcmp(h, "memory_search patterns=[^沙箱 bazel|sudo]") == 0, "search: 每个正则都列出来");
        sdsfree(h);

        h = hdr("memory_get", "{\"ids\":[\"m1\",\"m99\"]}");
        CHECK(h && strcmp(h, "memory_get ids=[m1 m99]") == 0, "get: 每个 id 都列出来");
        sdsfree(h);

        h = hdr("memory_list", "{\"offset\":1,\"n\":1}");
        CHECK(h && strcmp(h, "memory_list offset=1 n=1") == 0, "list: 翻页窗口");
        sdsfree(h);

        h = hdr("memory_update", "{\"entries\":[{\"id\":\"m1\",\"expected_rev\":2},{\"id\":\"m3\"}]}");
        CHECK(h && strcmp(h, "memory_update ids=[m1 m3]") == 0, "update: 批里每一项的 id");
        sdsfree(h);

        h = hdr("memory_write", "{\"title\":\"构建入口\",\"summary\":\"s\"}");
        CHECK(h && strcmp(h, "memory_write title=构建入口") == 0, "write: 标题 (不看 summary/detail)");
        sdsfree(h);

        h = hdr("memory_supersede", "{\"ids\":[\"m9\",\"m12\"]}");
        CHECK(h && strcmp(h, "memory_supersede ids=[m9 m12]") == 0, "supersede: 退休的 id");
        sdsfree(h);

        /* 缺字段 / 非数组都只影响这一行文字, 不会让调用失败 */
        h = hdr("memory_list", "{}");
        CHECK(h && strcmp(h, "memory_list offset=0 n=20") == 0, "list 缺参数 = 默认窗口");
        sdsfree(h);
        h = hdr("memory_get", "{}");
        CHECK(h && strcmp(h, "memory_get ids=[]") == 0, "get 缺 ids = 空列表 (校验那步会报错)");
        sdsfree(h);
        h = edc_tools_memory_call_header("memory_search", NULL);
        CHECK(h && strcmp(h, "memory_search") == 0, "args = NULL = 只有工具名");
        sdsfree(h);
    }

    size_t entries_before = session->memo->entry_count;

    TEST("memory_delete: 真删（列表/搜索/按 id 都拿不到）; 事件是 DELETED");
    struct capture de = run_tool(tb, task, "memory_delete", "{\"ids\":[\"m2\"]}");
    CHECK(de.error == NULL && de.result && strstr(de.result, "m2") != NULL, "删除成功");
    CHECK(session->memo->entry_count + 1 == entries_before, "条目表少了一条（不是只打标记）");
    CHECK(edc_session_memory_entry(session->memo, "m2") == NULL, "按 id 也查不到了");
    CHECK(session->memo->events[session->memo->event_count - 1].kind == EDC_MEMO_DELETED, "最新事件 DELETED");
    free_capture(&de);

    struct capture dl = run_tool(tb, task, "memory_list", "{}");
    CHECK(dl.result && strstr(dl.result, "[m1]") != NULL, "m1 还在列");
    CHECK(dl.result && strstr(dl.result, "[m2]") == NULL, "m2 不再出现在索引里");
    free_capture(&dl);

    struct capture ds = run_tool(tb, task, "memory_search", "{\"patterns\":[\"uid 1000\"]}");
    CHECK(ds.result && strstr(ds.result, "0 hit(s)") != NULL, "搜索也搜不到");
    free_capture(&ds);

    struct capture dg = run_tool(tb, task, "memory_get", "{\"ids\":[\"m2\"]}");
    CHECK(dg.error == NULL && dg.result && strstr(dg.result, "no such entry") != NULL, "按 id 也取不到了");
    free_capture(&dg);

    TEST("memory_delete: 原子 —— 含未知 id ⇒ 整批不删");
    struct capture da = run_tool(tb, task, "memory_delete", "{\"ids\":[\"m1\",\"m404\"]}");
    CHECK(da.error != NULL && da.result && strstr(da.result, "unknown") != NULL, "整批被拒");
    CHECK(edc_session_memory_entry(session->memo, "m1") != NULL, "m1 没被牵连");
    free_capture(&da);

    TEST("工具声明: 记忆工具的输出不被 tool_max_output_size 截断 (别的工具照旧)");
    {
        struct edc_tool *t = NULL;
        HASH_FIND_STR(tb->tools, "memory_get", t);
        CHECK(t && t->keep_full_output, "memory_get 声明全文输出");
        HASH_FIND_STR(tb->tools, "memory_delete", t);
        CHECK(t != NULL && t->keep_full_output, "memory_delete 已注册且声明全文输出");
        HASH_FIND_STR(tb->tools, "exec_shell", t);
        CHECK(t == NULL || !t->keep_full_output, "对照组: shell 没有声明（它有自己的上限）");
    }

    edc_toolbox_free(tb);
    edc_session_free(session); /* 连带释放 group / task / memo */
    edc_config_free(cfg);

    printf("\n%d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
