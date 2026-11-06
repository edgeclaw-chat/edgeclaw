/* `edc_fold_files` -- the tracked-file lists as data.
 *
 * 这一层的存在理由：清单曾经只以 `<read-files>`/`<modified-files>` 文本块的形式活在
 * `group->memory` 里，任何消费者都得把字符串反解回来，模型改写块就会污染累积结果。
 * 现在数据是唯一真相，块是它给模型看的投影。
 *
 * 最重要的用例是第一个：`edc_fold_files_render(body, f)` 必须与旧实现
 * `fold_memory_with_files(NULL, summary, reads, writes)`（bc9c431..79d2c76 里那份，
 * 已随本提交删除）**逐字节相同**。这段文本是每轮请求都发的 system message，
 * 多一个换行就是缓存前缀从 token 0 起全废。所以先钉字节，再谈别的。 */
#include "edc_fold_files.h"
#include "sds.h"

#include <cJSON.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int passed = 0, failed = 0;
#define TEST(name)                                                                                                     \
    do                                                                                                                 \
    {                                                                                                                  \
        printf("  TEST: %-58s ", name);                                                                                \
    } while (0)
#define CHECK(c, m)                                                                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(c))                                                                                                      \
        {                                                                                                              \
            printf("FAIL: %s\n", m);                                                                                   \
            failed++;                                                                                                  \
        }                                                                                                              \
        else                                                                                                           \
        {                                                                                                              \
            printf("PASS\n");                                                                                          \
            passed++;                                                                                                  \
        }                                                                                                              \
    } while (0)

/* ── 旧实现的期望输出（fixture，不是被测代码） ──────────────────
 *
 * 这些字节用真 sds 跑过 bc9c431..79d2c76 的 `fold_memory_with_files` 得到（探针链
 * bazel external 下的 sds.c）。写成字面量而不是"再实现一遍"：如果两者都由同一份
 * 代码生成，这个测试就只是在验证自己。 */
struct snapshot
{
    const char *name;
    const char *summary;   /* 模型给的摘要 */
    const char *read_paths; /* 逗号分隔，NULL = 无 */
    const char *mod_paths;
    const char *expect;
};

static const struct snapshot k_snapshots[] = {
    {"两块都有", "# Summary\n- work done", "src/a.c,src/", "src/b.c,src/c.c",
     "# Summary\n- work done\n\n<read-files>\nsrc/a.c\nsrc/\n</read-files>\n\n"
     "<modified-files>\nsrc/b.c\nsrc/c.c\n</modified-files>\n"},
    {"只有读", "# Summary\n- work done", "src/a.c", NULL,
     "# Summary\n- work done\n\n<read-files>\nsrc/a.c\n</read-files>\n"},
    {"只有改", "# Summary\n- work done", NULL, "src/b.c,src/c.c",
     "# Summary\n- work done\n\n<modified-files>\nsrc/b.c\nsrc/c.c\n</modified-files>\n"},
    {"一个文件都没有", "# Summary\n- work done", NULL, NULL, "# Summary\n- work done\n"},
    {"摘要已带尾部换行", "# Summary\n- work done\n", "src/a.c", NULL,
     "# Summary\n- work done\n\n<read-files>\nsrc/a.c\n</read-files>\n"},
    {"摘要尾部是空格+换行", "# Summary  \n\n", "src/a.c", NULL,
     "# Summary\n\n<read-files>\nsrc/a.c\n</read-files>\n"},
    {"摘要尾部是 TAB（剥不掉，保持原样）", "# Summary\n\t", "src/a.c", NULL,
     "# Summary\n\t\n\n<read-files>\nsrc/a.c\n</read-files>\n"},
    {"摘要是空串", "", "src/a.c", NULL, "\n\n<read-files>\nsrc/a.c\n</read-files>\n"},
    {"摘要是 NULL，且没有文件", NULL, NULL, NULL, "\n"},
    {"模型回显了旧块", "# Summary\n- ok\n\n<read-files>\nold/x.c\n</read-files>", "src/a.c", NULL,
     "# Summary\n- ok\n\n<read-files>\nsrc/a.c\n</read-files>\n"},
    {"模型回显了两块", "# S\n\n<read-files>\nold/x\n</read-files>\n\n<modified-files>\nold/y\n</modified-files>\n",
     "src/a.c", "src/b.c",
     "# S\n\n<read-files>\nsrc/a.c\n</read-files>\n\n<modified-files>\nsrc/b.c\n</modified-files>\n"},
    {"没有文件时摘要本身不收尾（尾部换行仍剥）", "# Summary\n", NULL, NULL, "# Summary\n"},
};

/* 把 "a,b" 拆进清单（保持顺序，去重交给 add）。 */
static void fill(struct edc_fold_files *f, const char *csv, bool modified)
{
    if (!csv)
        return;
    char *copy = strdup(csv);
    if (!copy)
        return;
    for (char *tok = strtok(copy, ","); tok; tok = strtok(NULL, ","))
    {
        if (modified)
            edc_fold_files_add_modified(f, tok);
        else
            edc_fold_files_add_read(f, tok);
    }
    free(copy);
}

/* ── ① 字节快照：投影必须与旧实现一致 ───────────────────────── */
static void test_render_is_byte_identical(void)
{
    TEST("render/body 与旧实现逐字节相同（缓存前缀纪律）");
    for (size_t i = 0; i < sizeof(k_snapshots) / sizeof(k_snapshots[0]); i++)
    {
        const struct snapshot *s = &k_snapshots[i];
        struct edc_fold_files *f = edc_fold_files_new();
        fill(f, s->read_paths, false);
        fill(f, s->mod_paths, true);
        sds body = edc_fold_files_body(s->summary);
        sds out = edc_fold_files_render(body, f);
        if (strcmp(out, s->expect) != 0)
        {
            printf("\n    [%s]\n    期望: ", s->name);
            for (const char *p = s->expect; *p; p++)
                (void)((*p == '\n') ? printf("\\n") : putchar(*p));
            printf("\n    实得: ");
            for (const char *p = out; *p; p++)
                (void)((*p == '\n') ? printf("\\n") : putchar(*p));
            printf("\n    ");
        }
        CHECK(strcmp(out, s->expect) == 0, s->name);
        sdsfree(out);
        sdsfree(body);
        edc_fold_files_free(f);
    }
}

/* ── ② 确定性：同状态两次渲染同字节 ─────────────────────────── */
static void test_render_is_deterministic(void)
{
    TEST("render 确定性（渲染从'落地算一次'变成'每请求一次'后的新要求）");
    struct edc_fold_files *f = edc_fold_files_new();
    fill(f, "b.c,a.c,c.c", false);
    fill(f, "z.c,y.c", true);
    sds body = edc_fold_files_body("# S");
    sds one = edc_fold_files_render(body, f);
    sds two = edc_fold_files_render(body, f);
    CHECK(strcmp(one, two) == 0, "同输入两次渲染字节相同");
    /* 渲染不修改自己的输入 —— 否则"再渲染一次"就会漂移。 */
    CHECK(f->read_n == 3 && f->modified_n == 2, "render 没有动清单");
    CHECK(strcmp(one, "# S\n\n<read-files>\nb.c\na.c\nc.c\n</read-files>\n\n"
                       "<modified-files>\nz.c\ny.c\n</modified-files>\n") == 0,
          "顺序 = 首见序（不排序）");
    sdsfree(one);
    sdsfree(two);
    sdsfree(body);
    edc_fold_files_free(f);
}

/* ── ③ 合并语义 ───────────────────────────────────────────── */
static void test_merge(void)
{
    TEST("merge：并集去重 + read 减 modified + 各自 cap");
    struct edc_fold_files *a = edc_fold_files_new();
    struct edc_fold_files *b = edc_fold_files_new();
    edc_fold_files_add_read(a, "old.c");
    edc_fold_files_add_modified(a, "m1.c");
    edc_fold_files_add_read(b, "new.c");
    edc_fold_files_add_read(b, "m1.c"); /* 既读又改 ⇒ 只留在 modified */
    edc_fold_files_add_modified(b, "m1.c");
    edc_fold_files_add_read(b, "old.c"); /* 旧的重叠 ⇒ 不重复 */

    edc_fold_files_merge(a, b);
    CHECK(a->read_n == 2, "读清单：old.c new.c（m1.c 被减掉）");
    CHECK(a->modified_n == 1, "改清单：m1.c 去重后一条");
    CHECK(strcmp(a->read[0], "old.c") == 0 && strcmp(a->read[1], "new.c") == 0, "首见序：旧的在前");
    sds out = edc_fold_files_render("", a);
    CHECK(strstr(out, "<read-files>\nold.c\nnew.c\n</read-files>") != NULL, "m1.c 不在读块里");
    CHECK(strstr(out, "<modified-files>\nm1.c\n</modified-files>") != NULL, "m1.c 在改块里");
    sdsfree(out);

    /* 幂等：同一份再并一次不涨 */
    size_t before_r = a->read_n, before_m = a->modified_n;
    edc_fold_files_merge(a, b);
    CHECK(a->read_n == before_r && a->modified_n == before_m, "重复合并结果不变");
    edc_fold_files_free(a);
    edc_fold_files_free(b);
}

static void test_merge_caps(void)
{
    TEST("merge：每份清单 cap 50，丢最老的");
    struct edc_fold_files *a = edc_fold_files_new();
    for (int i = 0; i < 60; i++)
    {
        char p[32];
        snprintf(p, sizeof(p), "f%02d.c", i);
        edc_fold_files_add_read(a, p);
    }
    edc_fold_files_merge(a, NULL);
    CHECK(a->read_n == EDC_FOLD_MAX_TRACKED_FILES, "50 条封顶");
    CHECK(strcmp(a->read[0], "f10.c") == 0, "最老的 f00..f09 被挤出");
    CHECK(strcmp(a->read[a->read_n - 1], "f59.c") == 0, "最新的还在");
    edc_fold_files_free(a);
}

/* ── ④ 边界：NULL / 空 / 非法 ─────────────────────────────── */
static void test_edges(void)
{
    TEST("边界：NULL 清单、NULL 路径、空串路径");
    CHECK(edc_fold_files_count(NULL) == 0, "NULL 计数为 0");
    edc_fold_files_add_read(NULL, "x.c"); /* 不崩 */
    edc_fold_files_merge(NULL, NULL);     /* 不崩 */
    edc_fold_files_free(NULL);            /* 不崩 */

    struct edc_fold_files *f = edc_fold_files_new();
    edc_fold_files_add_read(f, NULL);
    edc_fold_files_add_read(f, "");
    CHECK(f->read_n == 0, "NULL / 空串路径被忽略");
    sds out = edc_fold_files_render(NULL, NULL);
    CHECK(strcmp(out, "\n") == 0, "body 与清单都为空 ⇒ 单个换行");
    sdsfree(out);
    out = edc_fold_files_render(NULL, f);
    CHECK(strcmp(out, "\n") == 0, "空清单不产生块");
    sdsfree(out);
    edc_fold_files_free(f);
}

/* ── ⑤ 持久化往返 ─────────────────────────────────────────── */
static void test_json(void)
{
    TEST("JSON 往返 + 缺键/非法容错");
    struct edc_fold_files *f = edc_fold_files_new();
    fill(f, "b.c,a.c", false);
    fill(f, "m.c", true);
    cJSON *j = edc_fold_files_to_json(f);
    CHECK(j && cJSON_GetArraySize(cJSON_GetObjectItem(j, "read")) == 2, "read 两个元素");
    struct edc_fold_files *back = edc_fold_files_from_json(j);
    CHECK(back && back->read_n == 2 && back->modified_n == 1, "读回条数一致");
    CHECK(back && strcmp(back->read[0], "b.c") == 0 && strcmp(back->read[1], "a.c") == 0, "顺序保持（不排序）");
    sds orig = edc_fold_files_render("# S", f);
    sds rt = edc_fold_files_render("# S", back);
    CHECK(strcmp(orig, rt) == 0, "往返后渲染字节相同");
    sdsfree(orig);
    sdsfree(rt);
    edc_fold_files_free(back);
    cJSON_Delete(j);

    struct edc_fold_files *empty = edc_fold_files_from_json(NULL);
    CHECK(empty && edc_fold_files_count(empty) == 0, "缺键 ⇒ 空清单（不是 NULL）");
    edc_fold_files_free(empty);
    edc_fold_files_free(f);
}

/* ── ⑥ 估算：漏算清单会让 ctx 偏低 ────────────────────────── */
static void test_estimate(void)
{
    TEST("token 估算覆盖清单（漏算会让 ctx 偏低）");
    struct edc_fold_files *f = edc_fold_files_new();
    CHECK(edc_fold_files_token_estimate(NULL) == 0, "NULL ⇒ 0");
    CHECK(edc_fold_files_token_estimate(f) == 0, "空清单只算 0（不出块）");
    edc_fold_files_add_modified(f, "/work/project/some/rather/long/path/file.c");
    long long one = edc_fold_files_token_estimate(f);
    CHECK(one > 0, "有清单就非零");
    for (int i = 0; i < 10; i++)
    {
        char p[64];
        snprintf(p, sizeof(p), "/work/project/dir%d/file%d.c", i, i);
        edc_fold_files_add_read(f, p);
    }
    long long many = edc_fold_files_token_estimate(f);
    CHECK(many > one, "条数多了估算跟着涨");
    edc_fold_files_free(f);
}

int main(void)
{
    printf("=== test_fold_files ===\n");
    test_render_is_byte_identical();
    test_render_is_deterministic();
    test_merge();
    test_merge_caps();
    test_edges();
    test_json();
    test_estimate();
    printf("\n%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
