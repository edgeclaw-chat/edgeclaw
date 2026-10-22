/* `@` 文件引用 —— 流水线里的一个 pass（parse 之后、解释执行之前）。
 *
 * 这里直接驱动 edc_shell_execute（词法器 + 分类 + passes + 解释器），然后看 task 的 turn：
 *   msg0 = 用户写的文本（`@a.txt` 原样保留）
 *   msg1 = synthetic（read 工具形态的正文）—— 没有展开时不存在
 *
 * 覆盖：识别规则、展开、转义、去重、行范围、失败、二进制、截断、shell 行的 stage。 */
#include "edc_ast.h"
#include "edc_channel_actor.h"
#include "edc_compat.h"
#include "edc_config.h"
#include "edc_scheduler.h"
#include "edc_session.h"
#include "edc_session_manager.h"
#include "edc_shell.h"
#include "edc_task.h"
#include "edc_task_group.h"
#include "edc_turn.h"
#include "utlist_fix.h"

#include <cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int passed = 0, failed = 0;
#define TEST(n)                                                                                                        \
    do                                                                                                                 \
    {                                                                                                                  \
        printf("  TEST: %-58s ", n);                                                                                   \
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

static void install_resolver(struct edc_scheduler *sched, struct edc_session *sess)
{
    sched->ctx = sess;
    struct edc_session_manager *sm = g_new0(struct edc_session_manager, 1);
    sm->sessions = g_new0(struct edc_session *, 1);
    sm->sessions[0] = sess;
    sm->session_count = 1;
    sm->active_index = 0;
    sched->sessions = sm;
}

/* 一次输入：走真实的执行入口（不经过命令层），返回新建的任务。 */
static struct edc_task *run(struct edc_config *cfg, struct edc_session_manager *sm, struct edc_scheduler *sched,
                            struct edc_session *sess, const char *line)
{
    struct edc_channel_message *cm =
        edc_channel_message_new(edc_channel_adapter, EDC_CHANNEL_MESSAGE_USER_TYPE, sdsnew(line));
    edc_shell_execute(sm, sched, (struct edc_reply_to){.type = cm->type}, cm);

    /* 最新的 task = 这次输入建的那个（同组内追加）。 */
    struct edc_task *last = NULL;
    struct edc_task_group *g;
    DL_FOREACH(sess->groups, g)
    {
        struct edc_task *t;
        LL_FOREACH2(g->members, t, group_next)
        last = t;
    }
    return last;
}

static size_t msg_count(struct edc_task *t)
{
    if (!t || !t->active_turn)
        return 0;
    return t->active_turn->messages.tail - t->active_turn->messages.head;
}

static cJSON *msg_at(struct edc_task *t, size_t i)
{
    if (!t || !t->active_turn || i >= msg_count(t))
        return NULL;
    return t->active_turn->messages.data[t->active_turn->messages.head + i];
}

static const char *content_at(struct edc_task *t, size_t i)
{
    cJSON *m = msg_at(t, i);
    return m ? cJSON_GetStringValue(cJSON_GetObjectItem(m, "content")) : NULL;
}

static bool synthetic_at(struct edc_task *t, size_t i)
{
    cJSON *m = msg_at(t, i);
    if (!m)
        return false;
    cJSON *s = cJSON_GetObjectItem(m, "synthetic");
    return s && cJSON_IsTrue(s);
}

static bool has(const char *hay, const char *needle)
{
    return hay && needle && strstr(hay, needle) != NULL;
}

static int count_of(const char *hay, const char *needle)
{
    if (!hay || !needle || !needle[0])
        return 0;
    int n = 0;
    for (const char *p = hay; (p = strstr(p, needle)) != NULL; p += strlen(needle))
        n++;
    return n;
}

static void write_file(const char *path, const char *content)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return;
    fwrite(content, 1, strlen(content), f);
    fclose(f);
}

int main(void)
{
    const char *ws = "/tmp/test_mention_pass_ws";
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s && mkdir -p %s/sub/deep %s/many", ws, ws, ws);
    if (system(cmd) != 0)
        return 1;

    char path[512];
    snprintf(path, sizeof(path), "%s/a.txt", ws);
    write_file(path, "line-1-alpha\nline-2-bravo\nline-3-charlie\nline-4-delta\n");

    snprintf(path, sizeof(path), "%s/sub/b.txt", ws);
    write_file(path, "nested-bravo\n");

    /* 250 个文件：越过目录清单的 200 条上限。 */
    for (int i = 0; i < 250; i++)
    {
        snprintf(path, sizeof(path), "%s/many/e-%03d.txt", ws, i);
        write_file(path, "x\n");
    }

    /* A path with a space in it: unquotable before quoting worked, which is why this
     * fixture exists. */
    snprintf(path, sizeof(path), "%s/my file.txt", ws);
    write_file(path, "spaced-alpha\nspaced-bravo\n");

    snprintf(path, sizeof(path), "%s/bin.dat", ws);
    {
        FILE *f = fopen(path, "wb");
        if (f)
        {
            fwrite("abc\0def", 1, 7, f);
            fclose(f);
        }
    }

    /* 2500 行：越过 2000 行的上限。 */
    snprintf(path, sizeof(path), "%s/big.txt", ws);
    {
        FILE *f = fopen(path, "w");
        if (f)
        {
            for (int i = 0; i < 2500; i++)
                fprintf(f, "row-%d\n", i);
            fclose(f);
        }
    }

    struct edc_config *cfg = calloc(1, sizeof(*cfg));
    cfg->workspace = sdsnew(ws);
    cfg->shell_syntax = true; /* 顺便验证 `+stage` 那条路 */
    struct edc_session_manager *sm = edc_session_manager_new(cfg);
    struct edc_session *sess = edc_session_manager_get_active(sm);
    if (!sess)
        return 1;
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, sess);
    /* 相对路径按 session 的绑定目录解析（工具与 pass 用的是同一个 workdir）。 */
    if (!edc_session_set_workdir(sess, ws, NULL))
        return 1;

    const char *workdir = edc_session_get_workdir(sess);
    printf("workdir = %s\n", workdir ? workdir : "(null)");

    /* ── 1. 基本展开 ───────────────────────────────────────────── */
    TEST("@a.txt: text keeps the mention, synthetic carries the body");
    {
        struct edc_task *t = run(cfg, sm, sched, sess, "看下 @a.txt 有什么");
        CHECK(msg_count(t) == 2, "turn has 2 messages (user text + synthetic)");
        CHECK(has(content_at(t, 0), "@a.txt"), "the user text still says @a.txt");
        CHECK(!synthetic_at(t, 0), "the first message is not synthetic");
        CHECK(synthetic_at(t, 1), "the second message is synthetic");
        CHECK(has(content_at(t, 1), "Called the Read tool with the following input: {\"path\":\"a.txt\"}"),
              "synthetic starts with the read-tool header");
        CHECK(has(content_at(t, 1), "line-2-bravo"), "synthetic carries the file body");
    }

    /* ── 2. 识别规则：词中不触发 ──────────────────────────────── */
    TEST("foo@a.txt is not a mention (only line start / after space)");
    {
        struct edc_task *t = run(cfg, sm, sched, sess, "写信给 foo@a.txt 这个人");
        CHECK(msg_count(t) == 1, "nothing was attached");
    }

    /* ── 3. 转义 ───────────────────────────────────────────────── */
    /* ── 引号：路径里有空格 ─────────────────────────────────────── */
    TEST("@\"my file.txt\": quoting makes a path with a space expressible");
    {
        struct edc_task *t = run(cfg, sm, sched, sess, "看下 @\"my file.txt\" 有什么");
        CHECK(msg_count(t) == 2, "expanded");
        CHECK(has(content_at(t, 1), "{\"path\":\"my file.txt\"}"), "the header has the unquoted path");
        CHECK(has(content_at(t, 1), "spaced-alpha"), "and the body was read");
        /* The user's own text is untouched: the quotes stay where they typed them. */
        CHECK(has(content_at(t, 0), "@\"my file.txt\""), "the message text keeps the quotes");
    }

    TEST("@'my file.txt': single quotes are the same thing");
    {
        struct edc_task *t = run(cfg, sm, sched, sess, "看下 @'my file.txt'");
        CHECK(msg_count(t) == 2, "expanded");
        CHECK(has(content_at(t, 1), "spaced-bravo"), "the body was read");
    }

    /* A quoted reference names exactly what is inside the quotes: punctuation included. */
    TEST("@\"a.txt.\" keeps the full stop -- the user delimited it");
    {
        struct edc_task *t = run(cfg, sm, sched, sess, "看下 @\"a.txt.\"");
        CHECK(msg_count(t) == 2, "it was treated as a reference");
        CHECK(has(content_at(t, 1), "Read tool failed to read a.txt."), "and the full stop is part of the path");
    }

    TEST("@\"my file.txt\"#2-3: a quoted path still takes a line range");
    {
        struct edc_task *t = run(cfg, sm, sched, sess, "看下 @\"my file.txt\"#2-3");
        CHECK(msg_count(t) == 2, "expanded");
        CHECK(has(content_at(t, 1), "\"offset\":2,\"limit\":2"), "the range survived the quotes");
        CHECK(has(content_at(t, 1), "spaced-bravo"), "line 2 is there");
        CHECK(!has(content_at(t, 1), "spaced-alpha"), "line 1 is not");
    }

    /* An unterminated quote is not a reference at all: guessing where it ends would be
     * worse than leaving the text alone. */
    TEST("@\"my file.txt is not a reference (unterminated quote)");
    {
        struct edc_task *t = run(cfg, sm, sched, sess, "看下 @\"my file.txt");
        CHECK(msg_count(t) == 1, "nothing attached");
        CHECK(has(content_at(t, 0), "@\"my file.txt"), "and the text is untouched");
    }

    TEST("\\@a.txt unescapes but does not expand");
    {
        struct edc_task *t = run(cfg, sm, sched, sess, "为什么 \\@a.txt 没展开");
        CHECK(msg_count(t) == 1, "no synthetic message");
        CHECK(has(content_at(t, 0), "@a.txt"), "the text keeps a literal @a.txt");
        CHECK(!has(content_at(t, 0), "\\@"), "the backslash is gone");
    }

    /* ── 4. 失败不静默 ────────────────────────────────────────── */
    TEST("@ghost.txt reports a read failure instead of vanishing");
    {
        struct edc_task *t = run(cfg, sm, sched, sess, "@ghost.txt 看看");
        CHECK(msg_count(t) == 2, "a synthetic message was still produced");
        CHECK(has(content_at(t, 1), "Read tool failed to read ghost.txt"), "it says the read failed");
    }

    /* ── 5. 去重 ───────────────────────────────────────────────── */
    TEST("the same file mentioned twice expands once");
    {
        struct edc_task *t = run(cfg, sm, sched, sess, "@a.txt 和 @a.txt 再比一次");
        CHECK(msg_count(t) == 2, "one synthetic message");
        CHECK(count_of(content_at(t, 1), "line-2-bravo") == 1, "the body appears exactly once");
    }

    /* ── 6. 行范围 ────────────────────────────────────────────── */
    TEST("@a.txt#2-3 attaches only lines 2..3");
    {
        struct edc_task *t = run(cfg, sm, sched, sess, "@a.txt#2-3 只看这两行");
        CHECK(msg_count(t) == 2, "expanded");
        CHECK(has(content_at(t, 1), "\"offset\":2,\"limit\":2"), "the header names the range");
        CHECK(has(content_at(t, 1), "line-2-bravo") && has(content_at(t, 1), "line-3-charlie"), "both lines");
        CHECK(!has(content_at(t, 1), "line-1-alpha") && !has(content_at(t, 1), "line-4-delta"), "no other lines");
    }

    /* ── 7. 目录 ───────────────────────────────────────────────── */
    TEST("@sub/ lists the directory");
    {
        struct edc_task *t = run(cfg, sm, sched, sess, "@sub/ 里面有什么");
        CHECK(msg_count(t) == 2, "expanded");
        CHECK(has(content_at(t, 1), "[FILE] b.txt"), "the listing uses the list_dir shape");
        CHECK(has(content_at(t, 1), "[DIR]  deep"), "a nested directory is marked as one");
        /* Every directory contains itself and its parent; the model does not need to hear
         * about it (and the `@` completion does not offer them either). */
        CHECK(!has(content_at(t, 1), "[DIR]  .\n"), "`.` is not listed");
        CHECK(!has(content_at(t, 1), "[DIR]  ..\n"), "`..` is not listed");
    }

    TEST("@sub/#2-3 drops the range: a listing has no lines to slice");
    {
        struct edc_task *t = run(cfg, sm, sched, sess, "@sub/#2-3 只看两行");
        CHECK(msg_count(t) == 2, "expanded");
        CHECK(!has(content_at(t, 1), "\"offset\""), "the header does not advertise an offset");
        CHECK(has(content_at(t, 1), "[FILE] b.txt"), "and the whole listing is still there");
    }

    TEST("@many/ stops at the entry cap");
    {
        struct edc_task *t = run(cfg, sm, sched, sess, "@many/ 有多少");
        CHECK(msg_count(t) == 2, "expanded");
        CHECK(count_of(content_at(t, 1), "[FILE] ") == 200, "exactly 200 entries");
        CHECK(has(content_at(t, 1), "… [truncated]"), "and it says so");
    }

    /* ── 8. 二进制 ────────────────────────────────────────────── */
    TEST("@bin.dat is skipped, not attached as garbage");
    {
        struct edc_task *t = run(cfg, sm, sched, sess, "@bin.dat 读一下");
        CHECK(msg_count(t) == 2, "a synthetic message was produced");
        CHECK(has(content_at(t, 1), "Read tool skipped bin.dat: binary file"), "it says it was skipped");
    }

    /* ── 9. 截断 ───────────────────────────────────────────────── */
    TEST("@big.txt is truncated at the line cap");
    {
        struct edc_task *t = run(cfg, sm, sched, sess, "@big.txt 数一下");
        CHECK(msg_count(t) == 2, "expanded");
        CHECK(has(content_at(t, 1), "row-1999"), "the first 2000 lines are there");
        CHECK(!has(content_at(t, 1), "row-2499"), "the rest is not");
        CHECK(has(content_at(t, 1), "[truncated]"), "and it says so");
    }

    /* ── 10. shell 行的 stage 也走同一条路 ─────────────────────── */
    TEST("a shell stage (+\"…\") expands its @mention too");
    {
        struct edc_task *t = run(cfg, sm, sched, sess, "+\"看 @a.txt\"");
        CHECK(t != NULL, "a task was created");
        CHECK(msg_count(t) == 2, "the stage carries a synthetic message");
        CHECK(has(content_at(t, 0), "@a.txt"), "its input text keeps the mention");
        CHECK(has(content_at(t, 1), "line-2-bravo"), "and the body is attached");
    }

    /* ── 11. 没有 @ 的输入不生任何额外消息 ─────────────────────── */
    TEST("a plain message gains nothing");
    {
        struct edc_task *t = run(cfg, sm, sched, sess, "普通的一句话");
        CHECK(msg_count(t) == 1, "exactly one message");
    }

    /* ── 12. task 层遍历 part 列表（不是 case by case）───────────────── */
    TEST("the task layer loops over parts: N parts -> N messages");
    {
        struct edc_msg_part parts[2] = {
            {.role = EDC_MSG_USER, .synthetic = true, .content = sdsnew("first-block")},
            {.role = EDC_MSG_SYSTEM, .synthetic = false, .content = sdsnew("second-block")},
        };
        struct edc_task_spec spec;
        memset(&spec, 0, sizeof(spec));
        spec.desc = "parts loop";
        spec.input = "user text";
        spec.parts = parts;
        spec.nparts = 2;
        spec.place = EDC_TASK_PLACE_NEW_GROUP;
        spec.take_foreground = true;

        struct edc_task *t = edc_scheduler_task_create(sched, sess, &spec);
        CHECK(msg_count(t) == 3, "one user message + one message per part");
        CHECK(!synthetic_at(t, 0) && has(content_at(t, 0), "user text"), "the input is the first message");
        CHECK(has(content_at(t, 1), "first-block") && synthetic_at(t, 1), "part 1 keeps its synthetic flag");
        CHECK(has(content_at(t, 2), "second-block") && !synthetic_at(t, 2), "part 2 is not synthetic");
        CHECK(has(cJSON_GetStringValue(cJSON_GetObjectItem(msg_at(t, 2), "role")), "system"),
              "its role was honoured (no per-pass code in the task layer)");

        sdsfree(parts[0].content);
        sdsfree(parts[1].content);
    }

    printf("\n%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
