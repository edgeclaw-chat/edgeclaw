/* L2 session 记忆的数据骨架 (P4a)。

 * 这一阶段只做**数据**: 事件表 (seq 单调) / 条目 (索引 + 明细) / 消化水位
 * (按组) / 落盘往返 / task 出生点采纳。装配 (补丁注入) 与 L2 压缩任务分别在
 * P4b / P4c 两个阶段。 */
#include "edc_session_memory.h"
#include "edc_compat.h"
#include "edc_config.h"
#include "edc_session.h"
#include "edc_task.h"
#include "log.h"
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

#define CHECK_STREQ(a, b, msg) CHECK(strcmp((a) ? (a) : "", (b) ? (b) : "") == 0, msg)

/* ── 1. 事件表: seq 从 1 起、单调、永不复用 ───────────────────── */

static void test_events(void)
{
    TEST("事件表: seq 从 1 起且单调; 新表 next_seq = 0");
    struct edc_session_memory *m = edc_session_memory_new();
    CHECK(m->next_seq == 0, "空表 next_seq = 0 (与 task 的 0 水位同义)");

    uint64_t a = edc_session_memory_add_event(m, "m1", EDC_MEMO_CREATED, "标题一", "摘要一");
    uint64_t b = edc_session_memory_add_event(m, "m1", EDC_MEMO_UPDATED, "标题一改", "摘要一改");
    uint64_t c = edc_session_memory_add_event(m, "m2", EDC_MEMO_CREATED, "标题二", NULL);

    CHECK(a == 1 && b == 2 && c == 3, "seq 依次 1/2/3");
    CHECK(m->next_seq == 3, "next_seq 抬到最大已分配");
    CHECK(m->event_count == 3, "三条事件都在");
    /* 同一个条目可以不断产生事件 —— patch 语义就靠这个 */
    CHECK(m->events[0].entry_id && strcmp(m->events[0].entry_id, "m1") == 0, "事件指向条目 id");
    CHECK(m->events[1].kind == EDC_MEMO_UPDATED, "kind 存住了 (updated)");
    CHECK(m->events[2].summary == NULL, "summary 可缺 (NULL 而不是空串)");

    /* 值词表往返 */
    CHECK(strcmp(edc_memo_event_kind_name(EDC_MEMO_SUPERSEDED), "superseded") == 0, "kind 名 = superseded");
    CHECK(edc_memo_event_classify_kind("updated") == EDC_MEMO_UPDATED, "classify('updated')");
    CHECK(edc_memo_event_classify_kind("垃圾") == EDC_MEMO_CREATED, "不认识的 kind 退化成 created");
    CHECK(edc_memo_event_classify_kind(NULL) == EDC_MEMO_CREATED, "NULL 也退化成 created");

    /* 参数防线 */
    CHECK(edc_session_memory_add_event(NULL, "m1", EDC_MEMO_CREATED, "t", "s") == 0, "m = NULL → 0");
    CHECK(edc_session_memory_add_event(m, NULL, EDC_MEMO_CREATED, "t", "s") == 0, "entry_id = NULL → 0");

    edc_session_memory_free(m);
}

/* ── 2. 条目: 按 id upsert (索引 + 明细分开) ─────────────────── */

static void test_entries(void)
{
    TEST("条目: 按 id upsert; 索引与明细分开存");
    struct edc_session_memory *m = edc_session_memory_new();

    struct edc_memo_entry *e = edc_session_memory_upsert_entry(m, "m1", "构建系统", "唯一入口 bazelisk",
                                                              "明细: 迁到 bazel 的完整经过……", "group 768");
    CHECK(e != NULL && m->entry_count == 1, "第一次 upsert = 新建");
    CHECK_STREQ(e->id, "m1", "id 存住");
    CHECK_STREQ(e->title, "构建系统", "title 存住");
    CHECK(e->detail && strstr(e->detail, "完整经过") != NULL, "明细存住 (sds)");
    CHECK_STREQ(e->sources, "group 768", "来源存住");

    struct edc_memo_entry *again =
        edc_session_memory_upsert_entry(m, "m1", "构建系统 v2", "入口换成 bazel", NULL, "group 768");
    CHECK(again == e && m->entry_count == 1, "同 id 再 upsert = 就地更新 (不新增)");
    CHECK_STREQ(e->title, "构建系统 v2", "title 被替换");
    CHECK(e->detail == NULL, "detail 传 NULL = 清空 (不残留旧明细)");

    struct edc_memo_entry *m2 = edc_session_memory_upsert_entry(m, "m2", "沙箱", NULL, NULL, NULL);
    CHECK(m2 != NULL && m->entry_count == 2, "另一个 id = 新条目");

    CHECK(edc_session_memory_entry(m, "m1") == e, "按 id 查得到");
    CHECK(edc_session_memory_entry(m, "没有这个") == NULL, "查不到就 NULL");
    CHECK(edc_session_memory_entry(NULL, "m1") == NULL, "m = NULL → NULL");

    edc_session_memory_free(m);
}

/* ── 3.5 读/写口（工具与通知共用） ──────────────────────────── */

static void test_read_write_api(void)
{
    TEST("读/写口: 建条目发号 / 索引行 / 翻页 / 搜索 / 取明细");
    struct edc_session_memory *m = edc_session_memory_new();

    sds id1 = edc_session_memory_create_entry(m, "构建系统", "唯一入口 bazelisk", "明细一", "task-7");
    sds id2 = edc_session_memory_create_entry(m, "沙箱策略", "uid 1000 + sudo", "明细二", NULL);
    sds id3 = edc_session_memory_create_entry(m, "缓存", NULL, NULL, NULL);
    CHECK_STREQ(id1, "m1", "第一个 id = m1");
    CHECK_STREQ(id2, "m2", "第二个 id = m2");
    CHECK_STREQ(id3, "m3", "第三个 id = m3");
    CHECK(m->next_entry_id == 3, "号抬到 3");
    struct edc_memo_entry *e1 = edc_session_memory_entry(m, "m1");
    CHECK(e1 && e1->proposed_by && strcmp(e1->proposed_by, "task-7") == 0, "proposed_by 记下");
    CHECK(e1 && e1->summary != NULL, "摘要存住");
    /* 建条目即带 wall-clock，且 索引行 里能看到 */
    CHECK(e1 && e1->created_at > 0 && e1->created_at == e1->updated_at, "建条目即带 created/updated（相等且非 0）");
    {
        sds idx0 = edc_session_memory_index_lines(m, 0, 10, NULL);
        CHECK(strstr(idx0, " (20") != NULL, "索引行带时间");
        sdsfree(idx0);
    }
    /* update 只抬 updated_at，不动 created_at */
    {
        struct edc_memo_entry *e2 = edc_session_memory_entry(m, "m2");
        uint64_t created_before = e2->created_at;
        e2->updated_at = 100; /* 假装很久以前，好验证会被抬到现在 */
        uint64_t rev_out = 0;
        /* 三个字段都 NULL = 不改内容，只追加一次 UPDATED 事件（验证时间被抬）。 */
        CHECK(edc_session_memory_update(m, "m2", NULL, NULL, NULL, e2->updated_seq, &rev_out) == EDC_MEMO_WRITE_OK,
              "update 成功");
        CHECK(e2->updated_at > 100, "update 把 updated_at 抬到现在");
        CHECK(e2->created_at == created_before, "update 不动 created_at");
    }

    /* 停用一条: 索引行要标 retired, 但**不删** */
    e1->superseded = true;

    size_t total = 0;
    sds all = edc_session_memory_index_lines(m, 0, 10, &total);
    CHECK(total == 3, "总数 3");
    CHECK(strstr(all, "[m1] (retired) 构建系统 — 唯一入口 bazelisk") != NULL, "retired 标记 + 标题 + 摘要");
    CHECK(strstr(all, "[m2] 沙箱策略 — uid 1000 + sudo") != NULL, "普通行");
    CHECK(strstr(all, "[m3] 缓存") != NULL, "无摘要的行不拖尾巴");
    sdsfree(all);

    /* 翻页: 按 id 升序, offset/n 生效 */
    sds page = edc_session_memory_index_lines(m, 1, 1, NULL);
    CHECK(strstr(page, "[m2]") != NULL && strstr(page, "[m1]") == NULL && strstr(page, "[m3]") == NULL,
          "offset=1,n=1 只给 m2");
    sdsfree(page);
    sds empty = edc_session_memory_index_lines(m, 3, 5, NULL);
    CHECK(sdslen(empty) == 0, "越界 = 空 (不是崩)");
    sdsfree(empty);

    /* 搜索: 命中标题或摘要或明细; 命中数 cap 之外只计数 */
    size_t hits = 0;
    sds r = edc_session_memory_search_lines(m, "ZEBRA", 5, &hits);
    CHECK(hits == 0 && sdslen(r) == 0, "没命中 = 空");
    sdsfree(r);
    r = edc_session_memory_search_lines(m, "沙箱", 5, &hits);
    CHECK(hits == 1 && strstr(r, "[m2]") != NULL, "命中摘要");
    sdsfree(r);
    r = edc_session_memory_search_lines(m, "明细二", 5, &hits);
    CHECK(hits == 1 && strstr(r, "[m2]") != NULL, "命中明细");
    sdsfree(r);
    r = edc_session_memory_search_lines(m, "bazelisk", 5, &hits);
    CHECK(hits == 1 && strstr(r, "[m1]") != NULL, "大小写不敏感");
    sdsfree(r);
    r = edc_session_memory_search_lines(m, "明细", 1, &hits);
    CHECK(hits == 2 && sdslen(r) > 0, "两条命中但只回 1 条 (cap), hits 仍报 2");
    sdsfree(r);

    /* 正则搜索: POSIX ERE + 大小写不敏感; 非法正则置 out_regerror 且不回行 */
    int regerr = 0;
    sds rr = edc_session_memory_search_regex_lines(m, "^沙", 5, &hits, &regerr);
    CHECK(regerr == 0 && hits == 1 && strstr(rr, "[m2]") != NULL, "正则锚定命中标题");
    sdsfree(rr);
    rr = edc_session_memory_search_regex_lines(m, "uid 1000|BAZELISK", 5, &hits, &regerr);
    CHECK(regerr == 0 && hits == 2, "交替 + 大小写不敏感命中 2 条");
    sdsfree(rr);
    rr = edc_session_memory_search_regex_lines(m, "(", 5, &hits, &regerr);
    CHECK(regerr == 1 && hits == 0 && sdslen(rr) == 0, "非法正则: 置 regerror, 不回行");
    sdsfree(rr);

    /* 取明细: 有条目 = created/updated 头 + 明细; 没明细 = 索引行; 不存在 = NULL */
    sds d = edc_session_memory_get_detail(m, "m2");
    CHECK(d && strstr(d, "明细二") != NULL, "明细在（前面带头）");
    CHECK(d && strstr(d, "created: ") != NULL && strstr(d, "updated: ") != NULL, "明细带 created/updated 头");
    sdsfree(d);
    d = edc_session_memory_get_detail(m, "m3");
    CHECK(d && strstr(d, "[m3]") != NULL, "没有明细时退化成索引行");
    sdsfree(d);
    CHECK(edc_session_memory_get_detail(m, "m99") == NULL, "不存在的 id = NULL");
    CHECK(edc_session_memory_get_detail(NULL, "m1") == NULL, "m = NULL = NULL");

    edc_session_memory_free(m);
}

/* ── 3.6 队尾通知（请求期临时物） ───────────────────────────── */

static void add_entry_with_event(struct edc_session_memory *m, const char *title, const char *summary, sds *out_id)
{
    sds id = edc_session_memory_create_entry(m, title, summary, NULL, NULL);
    uint64_t seq = edc_session_memory_add_event(m, id, EDC_MEMO_CREATED, title, summary);
    struct edc_memo_entry *e = edc_session_memory_entry(m, id);
    if (e)
        e->created_seq = e->updated_seq = seq;
    if (out_id)
        *out_id = id;
    else
        sdsfree(id);
}

static void test_notice(void)
{
    TEST("通知: 差集 / 同一条目只出最新一条 / cap 截断 / 总条数 / max_injected");
    struct edc_session_memory *m = edc_session_memory_new();
    uint64_t max_inj = 0;

    CHECK(edc_session_memory_notice(m, 0, 10, &max_inj) == NULL, "空表 = 不注入");
    CHECK(max_inj == 0, "空表 max_injected = 0");

    sds id1 = NULL, id2 = NULL;
    add_entry_with_event(m, "构建入口", "唯一入口是 bazelisk", &id1);
    add_entry_with_event(m, "沙箱策略", NULL, &id2);

    sds n = edc_session_memory_notice(m, 0, 10, &max_inj);
    CHECK(n != NULL, "有东西可注入");
    CHECK(max_inj == 2, "max_injected = 最新事件 seq");
    CHECK(strstr(n, "## Session Memory Updates  (共 2 条)") != NULL, "抬头带总条数");
    CHECK(strstr(n, "- [m1] created: 构建入口 — 唯一入口是 bazelisk") != NULL, "第一条一行");
    CHECK(strstr(n, "- [m2] created: 沙箱策略") != NULL, "第二条一行 (无摘要不拖尾巴)");
    CHECK(strstr(n, "memory_list") != NULL && strstr(n, "memory_get") != NULL, "带用法提示");
    CHECK(strstr(n, "RECONCILE") != NULL && strstr(n, "expected_rev") != NULL, "带 reconcile/merge/create 纪律");
    CHECK(strstr(n, " (20") != NULL, "每行带事件时间");
    sdsfree(n);

    /* 同一条目再更新: 只出最新那行 (kind = updated), 不是两行 */
    uint64_t s3 = edc_session_memory_add_event(m, id1, EDC_MEMO_UPDATED, "构建入口", "唯一入口是 bazelisk");
    struct edc_memo_entry *e1 = edc_session_memory_entry(m, id1);
    if (e1)
        e1->updated_seq = s3;
    sds n2 = edc_session_memory_notice(m, 0, 10, &max_inj);
    CHECK(max_inj == 3, "max_injected 跟上更新");
    CHECK(strstr(n2, "[m1] updated:") != NULL, "显示最新的 kind");
    const char *first = strstr(n2, "[m1]");
    CHECK(first && strstr(first + 1, "[m1]") == NULL, "同一条目只出现一次");
    sdsfree(n2);

    /* cap: 3 条新事件只给 1 行, 其余报数 */
    sds n3 = edc_session_memory_notice(m, 0, 1, &max_inj);
    CHECK(max_inj == 3, "cap 截断也要把水位推到底 (被截的靠 memory_list 兜)");
    CHECK(strstr(n3, "more") != NULL, "报出被截的条数");
    CHECK(strstr(n3, "[m1]") != NULL && strstr(n3, "[m2]") == NULL, "只给最新那条");
    sdsfree(n3);

    /* 看过之后: 不再注入 */
    CHECK(edc_session_memory_notice(m, 3, 10, NULL) == NULL, "水位追平后 = NULL");
    sds n4 = edc_session_memory_notice(m, 2, 10, &max_inj);
    CHECK(n4 != NULL && max_inj == 3, "只补水位之后那一条");
    CHECK(strstr(n4, "[m1] updated:") != NULL, "水位之后的那条在");
    CHECK(strstr(n4, "[m2]") == NULL, "**已经看过的 m2 不许再出现** (delta 语义)");
    sdsfree(n4);

    sdsfree(id1);
    sdsfree(id2);
    edc_session_memory_free(m);
}

/* ── 4. 落盘往返 ─────────────────────────────────────────────── */

static void test_round_trip(void)
{
    TEST("落盘往返: 事件 / 条目 / 水位 / next_seq 全部逐字段回来");
    struct edc_session_memory *m = edc_session_memory_new();
    edc_session_memory_add_event(m, "m1", EDC_MEMO_CREATED, "标题", "摘要");
    edc_session_memory_add_event(m, "m1", EDC_MEMO_SUPERSEDED, "标题", NULL);
    struct edc_memo_entry *e = edc_session_memory_upsert_entry(m, "m1", "标题", "摘要", "明细正文", "group 1");
    e->created_seq = 1;
    e->updated_seq = 2;
    e->created_at = 111;
    e->updated_at = 222;
    e->superseded = true;
    edc_session_memory_note_proposer(e, "task-7");
    uint64_t ev_ts = m->events[0].ts;
    CHECK(ev_ts > 0, "add_event 带上了 wall-clock 时间");

    cJSON *json = edc_session_memory_to_json(m);
    CHECK(json != NULL, "能序列化");
    struct edc_session_memory *back = edc_session_memory_from_json(json);
    cJSON_Delete(json);

    CHECK(back != NULL && back->next_seq == 2, "next_seq 回来");
    CHECK(back->event_count == 2, "事件条数回来");
    CHECK(back->events[1].seq == 2 && back->events[1].kind == EDC_MEMO_SUPERSEDED, "第二条事件的 seq/kind 回来");
    CHECK(back->event_count == 2 && back->events[0].summary != NULL, "summary 回来");
    CHECK(back->entry_count == 1, "条目条数回来");
    CHECK(back->next_entry_id == 1, "next_entry_id 回来");
    const struct edc_memo_entry *be = edc_session_memory_entry(back, "m1");
    CHECK(be != NULL, "条目按 id 回来");
    CHECK(be && be->detail && strstr(be->detail, "明细正文") != NULL, "明细回来");
    CHECK(be && be->created_seq == 1 && be->updated_seq == 2, "两个 seq 回来");
    CHECK(be && be->created_at == 111 && be->updated_at == 222, "两个 wall-clock 时间回来");
    CHECK(back->events[0].ts == ev_ts && back->events[1].ts > 0, "事件时间回来");
    CHECK(be && be->superseded, "superseded 回来");
    CHECK(be && be->sources && strcmp(be->sources, "group 1") == 0, "来源回来");
    CHECK(be && be->proposed_by && strcmp(be->proposed_by, "task-7") == 0, "谁提议写的回来");

    /* 落盘的 seq 更权威: 盘上跳号 / 有洞也不重号 */
    cJSON *weird = cJSON_Parse("{\"next_seq\":0,\"events\":[{\"seq\":7,\"entry_id\":\"x\",\"kind\":\"created\"}]}");
    struct edc_session_memory *w = edc_session_memory_from_json(weird);
    cJSON_Delete(weird);
    CHECK(w->next_seq == 7, "next_seq 跟着盘上的最大 seq 抬起来 (不会重号)");
    uint64_t next = edc_session_memory_add_event(w, "y", EDC_MEMO_CREATED, "t", NULL);
    CHECK(next == 8, "下一条事件拿到 8");

    /* 缺键 / 非对象 / NULL: 给一个空表, 不是 NULL —— 加载端不必判空 */
    struct edc_session_memory *empty = edc_session_memory_from_json(NULL);
    CHECK(empty != NULL && empty->next_seq == 0 && empty->event_count == 0, "NULL 输入 → 空表");
    cJSON *notobj = cJSON_Parse("[]");
    struct edc_session_memory *empty2 = edc_session_memory_from_json(notobj);
    CHECK(empty2 != NULL && empty2->next_seq == 0, "非对象输入 → 空表");
    cJSON_Delete(notobj);

    edc_session_memory_free(m);
    edc_session_memory_free(back);
    edc_session_memory_free(w);
    edc_session_memory_free(empty);
    edc_session_memory_free(empty2);
}

/* ── 5. task 出生点: 水位 = 0 (首轮自带回填通知) ─────────────── */

static void test_adopt(void)
{
    TEST("出生点: 新 task 的水位是 0 —— 它没有最新条目, 首轮要回填");
    struct edc_config *cfg = g_new0(struct edc_config, 1);
    cfg->workspace = sdsnew("/tmp/test_l2_workspace");
    struct edc_session *s = edc_session_new(cfg, "l2", "L2");
    CHECK(s->memo != NULL, "session 构造即带 session 记忆");

    edc_session_memory_add_event(s->memo, "m1", EDC_MEMO_CREATED, "t", "s");
    edc_session_memory_add_event(s->memo, "m2", EDC_MEMO_CREATED, "t", "s");
    struct edc_task *early = edc_task_new("100", "l2", "100", "早的", edc_channel_adapter);
    struct edc_task *late = edc_task_new("101", "l2", "100", "晚的", edc_channel_adapter);
    edc_session_memory_adopt_task(s, early);
    CHECK(early->memo_seen_seq == 0, "采纳 = 0 (不是表头 2)");
    CHECK(s->memo->next_seq == 2, "表头是 2 ⇒ 差集 = {1,2} = 首轮回填");
    CHECK(late->memo_seen_seq == 0, "没采纳的也是 0 (采纳只是显式声明)");

    /* 之后再来一个事件: 任何还没推进过的 task 都会连同它一起收到 */
    edc_session_memory_adopt_task(s, late);
    edc_session_memory_add_event(s->memo, "m3", EDC_MEMO_CREATED, "t", "s");
    CHECK(late->memo_seen_seq == 0 && s->memo->next_seq == 3, "晚的 task 差集 = {1,2,3}");

    /* 参数防线 */
    edc_session_memory_adopt_task(NULL, early);
    edc_session_memory_adopt_task(s, NULL);
    CHECK(early->memo_seen_seq == 0, "两个 NULL 参数都是 no-op");

    edc_task_free(early);
    edc_task_free(late);
    edc_session_free(s);
    edc_config_free(cfg);
}

/* ── 7. 逻辑删除 + 压缩 ────────────────────────────────────── */

static void test_delete_and_trim(void)
{
    TEST("删除: 真删（表里没有、按 id 读不到、视图没有），并留一条 DELETED 事件");
    struct edc_session_memory *m = edc_session_memory_new();
    sds id1 = edc_session_memory_create_entry(m, "要删的", "过期了", "明细 A", NULL);
    sds id2 = edc_session_memory_create_entry(m, "留着的", "还在用", "明细 B", NULL);
    CHECK_STREQ(id1, "m1", "id1 = m1");
    CHECK_STREQ(id2, "m2", "id2 = m2");

    const char *one[] = {"m1"};
    CHECK(edc_session_memory_delete(m, one, 1), "删除 m1");
    CHECK(m->entry_count == 1, "条目表真的少了一条");
    CHECK(edc_session_memory_entry(m, "m1") == NULL, "按 id 查不到了");
    CHECK(edc_session_memory_get_detail(m, "m1") == NULL, "明细也读不到了");
    CHECK(m->events[m->event_count - 1].kind == EDC_MEMO_DELETED, "留了一条 DELETED 事件（变化流要告诉别人）");
    CHECK(!edc_session_memory_delete(m, one, 1), "再删一次 = 未知 id（不是幂等）");
    const char *mixed[] = {"m2", "m404"};
    CHECK(!edc_session_memory_delete(m, mixed, 2), "原子: 含未知 id 就整批拒绝");
    CHECK(edc_session_memory_entry(m, "m2") != NULL, "m2 没被牵连");

    size_t total = 0;
    sds idx = edc_session_memory_index_lines(m, 0, 10, &total);
    CHECK(total == 1 && strstr(idx, "要删的") == NULL, "索引只剩一条、没有它的标题");
    sdsfree(idx);
    size_t hits = 0;
    sds found = edc_session_memory_search_lines(m, "过期了", 10, &hits);
    CHECK(hits == 0, "搜索搜不到");
    sdsfree(found);
    sds shown = edc_session_memory_show_text(m, "m1");
    CHECK(shown && strstr(shown, "no entry") != NULL, "show 报没有这条");
    sdsfree(shown);

    TEST("事件日志: 写路径上自动裁剪, 不会无限增长");
    uint64_t rev = edc_session_memory_entry(m, "m2")->updated_seq;
    for (int i = 0; i < EDC_SESSION_MEMORY_TRIM_EVENTS + 20; i++)
    {
        enum edc_memo_write_result r = edc_session_memory_update(m, "m2", NULL, "改了", NULL, rev, &rev);
        if (r != EDC_MEMO_WRITE_OK)
            break;
    }
    CHECK(m->event_count <= EDC_SESSION_MEMORY_TRIM_EVENTS, "日志被压在触发线以内（不再无限增长）");
    CHECK(m->pruned_before > 0, "pruned_before 前进了");
    CHECK(edc_session_memory_entry(m, "m2") != NULL, "裁剪不会碰活着的条目");

    edc_session_memory_free(m);
}

int main(void)
{

    test_events();
    test_entries();
    test_read_write_api();
    test_notice();
    test_round_trip();
    test_adopt();
    test_delete_and_trim();
    printf("\n%d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
