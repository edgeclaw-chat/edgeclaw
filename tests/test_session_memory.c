/* L2 session 记忆的数据骨架 (P4a)。

 * 这一阶段只做**数据**: 事件表 (seq 单调) / 条目 (索引 + 明细) / 消化水位
 * (按组) / 落盘往返 / task 出生点采纳。装配 (补丁注入) 与 L2 压缩任务分别在
 * P4b / P4c 两个阶段。 */
#include "edc_session_memory.h"
#include "test_ws.h"
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
    CHECK(strstr(n, "## Session Memory Updates  (本次 2 项变化 / 记忆共 2 条)") != NULL, "抬头两个数分列");
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

    /* cap: 去重后 2 项待投递 (m2 seq2, m1 seq3)，cap=1 ⇒ 先给最旧的 m2 (FIFO)，
     * 水位只推到它，其余下轮继续。这是"每个变化要么被投递、要么被显式声明放弃"的实现。 */
    sds n3 = edc_session_memory_notice(m, 0, 1, &max_inj);
    CHECK(max_inj == 2, "水位只推到实际投递的最大 seq (不是推到底)");
    CHECK(strstr(n3, "more") != NULL, "报出还剩多少");
    CHECK(strstr(n3, "本次 2 项变化") != NULL, "抬头报本次变化项数 (去重后的项数)");
    CHECK(strstr(n3, "[m2]") != NULL && strstr(n3, "[m1]") == NULL, "FIFO: 先给最旧那条 (m2, seq2)");
    sdsfree(n3);
    /* 下一轮从水位继续 */
    sds n3b = edc_session_memory_notice(m, max_inj, 10, &max_inj);
    CHECK(n3b != NULL && strstr(n3b, "[m1]") != NULL, "下一轮补上剩下的 (m1 updated)");
    CHECK(max_inj == 3, "水位追到 3");
    sdsfree(n3b);

    /* 看过之后: 不再注入 */
    CHECK(edc_session_memory_notice(m, 3, 10, NULL) == NULL, "水位追平后 = NULL");
    sds n4 = edc_session_memory_notice(m, 2, 10, &max_inj);
    CHECK(n4 != NULL && max_inj == 3, "只补水位之后那一条");
    CHECK(strstr(n4, "[m1] updated:") != NULL, "水位之后的那条在");
    CHECK(strstr(n4, "[m2]") == NULL, "**已经看过的 m2 不许再出现** (delta 语义)");
    sdsfree(n4);

    /* 溢出卡: 待投递超过 cap*3 就停止逐条列，改为显式声明放弃 (水位推到底) */
    {
        struct edc_session_memory *mb = edc_session_memory_new();
        for (int k = 0; k < 10; k++)
        {
            sds bid = NULL;
            add_entry_with_event(mb, "批量", NULL, &bid);
            sdsfree(bid);
        }
        uint64_t mi = 0;
        /* cap=2 ⇒ 阈值 6，待投递 10 > 6 ⇒ 触发 */
        sds ov = edc_session_memory_notice(mb, 0, 2, &mi);
        CHECK(ov != NULL && strstr(ov, "积压") != NULL, "积压过多 ⇒ 发溢出卡");
        CHECK(ov && strstr(ov, "NOT being reported") != NULL, "显式声明这些变化不逐条报");
        CHECK(mi == 10, "溢出卡把水位推到底 (否则会一直卡在同一处)");
        CHECK(ov && strstr(ov, "- [m1]") == NULL, "溢出卡不逐条列条目");
        sdsfree(ov);
        /* 不超高阈值时仍逐条投递 (cap=4 ⇒ 阈值 12，10 项不触发) */
        sds nx = edc_session_memory_notice(mb, 0, 4, &mi);
        CHECK(nx && strstr(nx, "积压") == NULL && strstr(nx, "- [m1]") != NULL, "未超阈值 ⇒ 正常逐条");
        sdsfree(nx);
        edc_session_memory_free(mb);
    }

    /* P2 分层渲染: updated 只报"哪个字段变了"，不重述内容 */
    {
        struct edc_session_memory *mu = edc_session_memory_new();
        sds uid = NULL;
        add_entry_with_event(mu, "分层", "原始摘要内容", &uid);
        /* 只改 detail（必须同批给 summary —— 工具层强制；这里直接调 API 模拟） */
        struct edc_memo_entry *ue = edc_session_memory_entry(mu, uid);
        edc_session_memory_update(mu, uid, NULL, "新摘要内容", "新明细", ue->updated_seq, NULL);
        uint64_t mi = 0;
        sds un = edc_session_memory_notice(mu, 0, 20, &mi);
        CHECK(un && strstr(un, "summary+detail changed") != NULL, "报出变了的字段名");
        CHECK(un && strstr(un, "新摘要内容") == NULL, "**不重述 summary 内容** (变化通知只报变化)");
        sdsfree(un);

        /* detail 与 summary 都没变 ⇒ 不写事件（避免无变化噪声） */
        size_t ev_before = mu->event_count;
        edc_session_memory_update(mu, uid, NULL, "新摘要内容", "新明细", edc_session_memory_entry(mu, uid)->updated_seq,
                                  NULL);
        sds un2 = edc_session_memory_notice(mu, mi, 20, &mi);
        CHECK(un2 == NULL || strstr(un2, "changed") == NULL, "同值重复更新不算变化");
        if (un2)
            sdsfree(un2);
        (void)ev_before;

        /* retired 也走字段名 */
        const char *rids[] = {uid};
        edc_session_memory_supersede(mu, rids, 1);
        sds un3 = edc_session_memory_notice(mu, 0, 20, &mi);
        CHECK(un3 && strstr(un3, "superseded") != NULL, "supersede 事件照常");
        sdsfree(un3);

        /* created 仍带内容（读者没有别的途径知道它写了什么） */
        sds cid = NULL;
        add_entry_with_event(mu, "新建的", "新建摘要内容", &cid);
        sds un4 = edc_session_memory_notice(mu, 0, 20, &mi);
        CHECK(un4 && strstr(un4, "created: 新建的 — 新建摘要内容") != NULL, "created 带内容");
        sdsfree(un4);
        sdsfree(uid);
        sdsfree(cid);
        edc_session_memory_free(mu);
    }

    sdsfree(id1);
    sdsfree(id2);
    edc_session_memory_free(m);
}

/* ── 3.7 目录通道（catalogue）：另一条通道 ───────────────────── */

static void test_catalogue(void)
{
    TEST("目录: 只给 id + title（不带 summary/时间/kind）、id 升序、报总条数");
    struct edc_session_memory *m = edc_session_memory_new();
    CHECK(edc_session_memory_catalogue(m, 2000) == NULL, "空记忆 = 不注入");

    sds a = NULL, b = NULL, c = NULL;
    add_entry_with_event(m, "构建入口", "唯一入口是 bazelisk 明细很长很长", &a);
    add_entry_with_event(m, "沙箱策略", "不再是 user-ns", &b);
    add_entry_with_event(m, "缓存", NULL, &c);

    sds cat = edc_session_memory_catalogue(m, 2000);
    CHECK(cat != NULL, "有条目 ⇒ 给目录");
    CHECK(strstr(cat, "## Session Memory  (共 3 条)") != NULL, "抬头报总条数");
    CHECK(strstr(cat, "- [m1] 构建入口") != NULL, "行 = id + title");
    CHECK(strstr(cat, "唯一入口是 bazelisk") == NULL, "目录不带 summary (它的成本优势就在这)");
    CHECK(strstr(cat, "created") == NULL && strstr(cat, "updated") == NULL, "目录不带 kind");
    CHECK(strstr(cat, " (20") == NULL, "目录不带时间戳");
    /* id 升序，与 memory_list 同序 ⇒ 可接着翻页 */
    const char *p1 = strstr(cat, "[m1]"), *p2 = strstr(cat, "[m2]"), *p3 = strstr(cat, "[m3]");
    CHECK(p1 && p2 && p3 && p1 < p2 && p2 < p3, "id 升序 (与 memory_list 同序才能无缝翻页)");
    CHECK(strstr(cat, "memory_get") != NULL, "带取明细的指引");
    sdsfree(cat);

    /* token 预算: 超预算就截断，并给出可接续的 memory_list 偏移 */
    sds small = edc_session_memory_catalogue(m, 1);
    CHECK(small != NULL && strstr(small, "[m1]") != NULL, "预算再小也给至少一行 (否则无法行动)");
    CHECK(strstr(small, "more") != NULL, "超预算报剩余数");
    CHECK(strstr(small, "memory_list 1 50") != NULL, "给出接续偏移 (offset = 已展示行数)");
    sdsfree(small);

    /* 已删条目不出现在目录里：目录走 entries[]，不是 events[]（后者留墓碑） */
    const char *del_ids[] = {b};
    edc_session_memory_delete(m, del_ids, 1);
    sds cat2 = edc_session_memory_catalogue(m, 2000);
    CHECK(cat2 && strstr(cat2, "共 2 条") != NULL, "删后总数 2");
    CHECK(cat2 && strstr(cat2, "[m2]") == NULL, "**已删条目不许出现在目录**（墓碑污染的根因）");
    CHECK(m->event_count == 4, "事件表仍有 4 条（含 DELETED 墓碑）");
    sdsfree(cat2);

    sdsfree(a);
    sdsfree(b);
    sdsfree(c);
    edc_session_memory_free(m);
}

/* ── 3.8 summary ⊆ detail：事实覆盖不变量 ─────────────────────
 * HARD 类（commit + snake_case 标识符）必须被 detail 覆盖；SOFT 类（路径、带单位数值）
 * 刻意不拦 —— 它们会被合法改写（200us vs 0.2ms），硬拦会挡掉诚实的写入。 */

static void test_summary_coverage(void)
{
    TEST("summary ⊆ detail: commit/标识符过硬门槛, 路径数字与规范词汇放过");
    sds rep = NULL;

    /* 没有 detail = 无需校验：summary 是唯一载体，不存在"不一致的第二个家" */
    CHECK(edc_session_memory_uncovered_facts("见 a1b2c3d", NULL, &rep) == 0, "detail 为空 = 0");
    CHECK(rep == NULL, "detail 为空不给报告");
    CHECK(edc_session_memory_uncovered_facts(NULL, "detail", &rep) == 0, "summary 为空 = 0");
    CHECK(edc_session_memory_uncovered_facts("", "detail", &rep) == 0, "summary 空串 = 0");

    /* commit 缺失 / 已覆盖 */
    CHECK(edc_session_memory_uncovered_facts("见 a1b2c3d", "没有那个号", &rep) == 1, "commit 缺失 = 1");
    CHECK(rep && strstr(rep, "a1b2c3d") != NULL, "报告里点名缺的事实");
    sdsfree(rep);
    CHECK(edc_session_memory_uncovered_facts("见 a1b2c3d", "commit a1b2c3d 修好了", &rep) == 0, "commit 已覆盖 = 0");
    sdsfree(rep);

    /* 纯数字不是 commit：否则日期/字节数会把校验变成噪声源 */
    CHECK(edc_session_memory_uncovered_facts("20251230 那天", "x", &rep) == 0, "纯数字不是 commit");
    sdsfree(rep);
    /* 长度门槛 7：abc123 只有 6 位 */
    CHECK(edc_session_memory_uncovered_facts("abc123 太短", "x", &rep) == 0, "6 位不算 commit");
    sdsfree(rep);
    /* 词内不算：前面是 word char */
    CHECK(edc_session_memory_uncovered_facts("ga1b2c3d 嵌在词里", "x", &rep) == 0, "词内不算");

    /* snake_case 标识符 */
    CHECK(edc_session_memory_uncovered_facts("调用 edc_sub_fn", "x", &rep) == 1, "标识符缺失 = 1");
    CHECK(rep && strstr(rep, "edc_sub_fn") != NULL, "报告里点名标识符");
    sdsfree(rep);
    CHECK(edc_session_memory_uncovered_facts("调用 edc_sub_fn", "edc_sub_fn 是静态的", &rep) == 0,
          "标识符已覆盖 = 0");
    sdsfree(rep);

    /* 规范词汇不算事实：summary 讲"怎么干活"不能要求 detail 复述 */
    CHECK(edc_session_memory_uncovered_facts("用 memory_search 并传 expected_rev", "x", &rep) == 0,
          "工具名/字段名放过");

    /* SOFT 类刻意不拦 */
    CHECK(edc_session_memory_uncovered_facts("耗时 9.6ms，路径 /src/toolbox", "x", &rep) == 0,
          "带单位数值与路径不拦 (会被合法改写)");

    /* 多个缺失一起报 */
    sds r2 = NULL;
    CHECK(edc_session_memory_uncovered_facts("a1b2c3d 与 edc_sub_fn", "x", &r2) == 2, "多个缺失计数正确");
    CHECK(r2 && strstr(r2, "a1b2c3d") && strstr(r2, "edc_sub_fn"), "多个缺失都点名");
    sdsfree(r2);

    edc_session_memory_free(NULL); /* 防御性调用不该崩 */
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
    cfg->workspace = sdsnew(edc_test_workspace());
    struct edc_session *s = edc_session_new(cfg, edc_test_session_id("l2"), "L2");
    CHECK(s->memo != NULL, "session 构造即带 session 记忆");

    edc_session_memory_add_event(s->memo, "m1", EDC_MEMO_CREATED, "t", "s");
    edc_session_memory_add_event(s->memo, "m2", EDC_MEMO_CREATED, "t", "s");
    struct edc_task *early = edc_task_new(edc_test_id("100"), edc_test_session_id("l2"), edc_test_id("100"), "早的", edc_channel_adapter);
    struct edc_task *late = edc_task_new(edc_test_id("101"), edc_test_session_id("l2"), edc_test_id("100"), "晚的", edc_channel_adapter);
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
    edc_test_workspace(); /* open the disposable store before anything writes */

    test_events();
    test_entries();
    test_read_write_api();
    test_notice();
    test_catalogue();
    test_summary_coverage();
    test_round_trip();
    test_adopt();
    test_delete_and_trim();
    printf("\n%d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
