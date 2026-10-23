/* `/session new` 必须绑定工作目录。

 * 钉住这些（2026-10-22 用户要求：`new` 时强制绑定目录，绑定失败就不能建出一个
 * "未绑定"的 session）：
 *   ① 裸 `/session new` = usage，**不建** session；
 *   ② 目录不存在 = 失败，**不建** session（不静默降级成未绑定）；
 *   ②b 单 token 且不是存在的目录 = 同样失败（单 token 一定被当成目录）；
 *   ③ `/session new <name> <dir>` = 建出来的 session 就绑定到该目录；
 *   ④ 单 token 是存在的目录 = 未命名但已绑定；
 *   ⑤ 引号是语法不是路径字节：`'<dir>'` / `"<dir>"` 都绑定到 <dir>
 *      （用户在 Windows 上把路径包成单引号是肌肉记忆，不剥引号就会把
 *      `'D:\repo'` 当成相对路径拼到 cwd 后面，报 not a directory）；
 *   ⑥ 名字里的空格用引号包：`'my dev' <dir>`；
 *   ⑦ 目录里的空格不用引号：目录是行尾剩余部分。

 * harness 抄 test_commands_session_memo.c：走公开命令入口 edc_commands_handler。
 * 没有通道 actor ⇒ 回话收不到（edc_actor_send 对 NULL actor 直接返回），所以命令层
 * 只看**行为**：session 数与 edc_session_get_workdir()。 */
#include "edc_channel_actor.h"
#include "test_ws.h"
#include "edc_commands.h"
#include "edc_compat.h"
#include "edc_config.h"
#include "edc_scheduler.h"
#include "edc_session.h"
#include "edc_session_manager.h"
#include "log.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

volatile int _run_loop = 1;

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

/* 走真正的命令入口；这里没有任务，所以不必 schedule。 */
static void drive(struct edc_config *cfg, struct edc_session_manager *sm, struct edc_scheduler *sched,
                  const char *content)
{
    struct edc_channel_message *cm =
        edc_channel_message_new(edc_channel_adapter, EDC_CHANNEL_MESSAGE_USER_TYPE, sdsnew(content));
    edc_commands_handler(cfg, sm, sched, (struct edc_reply_to){.type = cm->type}, cm);
}

static bool bound_to(struct edc_session *s, const char *dir)
{
    const char *w = s ? edc_session_get_workdir(s) : NULL;
    return w && strcmp(w, dir) == 0;
}

int main(void)
{
    edc_test_workspace(); /* disposable store for anything that writes state */
    char ws[256];
    snprintf(ws, sizeof(ws), "%s", "/tmp/edc_cmd_session_new_XXXXXX");
    if (!mkdtemp(ws))
    {
        perror("mkdtemp");
        return 2;
    }

    /* 一个真实存在的目录（绑定的目标）。 */
    char dir[300];
    snprintf(dir, sizeof(dir), "%s/repo", ws);
    if (mkdir(dir, 0755) != 0)
    {
        perror("mkdir");
        return 2;
    }
    char real_dir[PATH_MAX];
    if (!realpath(dir, real_dir))
    {
        perror("realpath");
        return 2;
    }

    /* 名字/路径里带空格的目录。 */
    char spaced[300];
    snprintf(spaced, sizeof(spaced), "%s/my repo", ws);
    if (mkdir(spaced, 0755) != 0)
    {
        perror("mkdir spaced");
        return 2;
    }
    char real_spaced[PATH_MAX];
    if (!realpath(spaced, real_spaced))
    {
        perror("realpath spaced");
        return 2;
    }

    struct edc_config *cfg = g_new0(struct edc_config, 1);
    cfg->workspace = sdsnew(ws);
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    struct edc_session_manager *sm = edc_session_manager_new(cfg);
    if (!sm)
    {
        printf("FAIL: cannot build the session manager\n");
        return 2;
    }
    /* 命令层通过 scheduler 找 session（与生产装配一致）。 */
    sched->sessions = sm;
    sched->ctx = edc_session_manager_get_active(sm);

    const int base = sm->session_count;
    char cmd[512];
    printf("  (baseline: %d session(s))\n", base);

    /* ── ① 裸 `/session new` ─────────────────────────────────── */
    TEST("bare /session new: usage, no session created");
    drive(cfg, sm, sched, "/session new");
    CHECK(sm->session_count == base, "session count unchanged");

    /* ── ② 目录不存在 ────────────────────────────────────────── */
    TEST("nonexistent dir: fails, no session created");
    drive(cfg, sm, sched, "/session new bad /no/such/dir/edgeclaw-xyz");
    CHECK(sm->session_count == base, "session count unchanged");
    CHECK(edc_session_manager_find(sm, "bad") == NULL, "the named session was not created");

    /* ── ②b 单 token 且不是目录 ──────────────────────────────── */
    TEST("single nonexistent token: read as dir, no session created");
    drive(cfg, sm, sched, "/session new no-such-dir-token");
    CHECK(sm->session_count == base, "session count unchanged");
    CHECK(edc_session_manager_find(sm, "no-such-dir-token") == NULL, "no session named like the token");

    /* ── ③ `/session new <name> <dir>` ──────────────────────── */
    TEST("/session new <name> <dir>: created and bound");
    snprintf(cmd, sizeof(cmd), "/session new dev %s", real_dir);
    drive(cfg, sm, sched, cmd);
    CHECK(sm->session_count == base + 1, "one session added");
    struct edc_session *dev = edc_session_manager_find(sm, "dev");
    CHECK(dev != NULL, "session named dev exists");
    CHECK(bound_to(dev, real_dir), "dev is bound to the given dir (normal form)");
    CHECK(edc_session_manager_get_active(sm) == dev, "the new session became active");

    /* ── ④ 单 token 是存在的目录 ────────────────────────────── */
    TEST("single token that is a dir: unnamed but bound");
    snprintf(cmd, sizeof(cmd), "/session new %s", real_dir);
    drive(cfg, sm, sched, cmd);
    CHECK(sm->session_count == base + 2, "one session added");
    struct edc_session *unnamed = edc_session_manager_get_active(sm);
    CHECK(unnamed != NULL && unnamed != dev, "a different session became active");
    CHECK(bound_to(unnamed, real_dir), "the unnamed session is bound to the given dir");

    /* ── ⑤ 单引号路径（用户实际踩的坑） ─────────────────────── */
    TEST("single-quoted dir: quotes are syntax, session bound");
    snprintf(cmd, sizeof(cmd), "/session new quoted1 '%s'", real_dir);
    drive(cfg, sm, sched, cmd);
    CHECK(sm->session_count == base + 3, "one session added");
    CHECK(bound_to(edc_session_manager_find(sm, "quoted1"), real_dir), "single-quoted dir stripped and bound");

    /* ── ⑤b 双引号路径，且不带名字 ──────────────────────────── */
    TEST("double-quoted dir as the only token: bound");
    snprintf(cmd, sizeof(cmd), "/session new \"%s\"", real_dir);
    drive(cfg, sm, sched, cmd);
    CHECK(sm->session_count == base + 4, "one session added");
    CHECK(bound_to(edc_session_manager_get_active(sm), real_dir), "double-quoted dir stripped and bound");

    /* ── ⑤c 引号括起来的坏目录照样失败 ──────────────────────── */
    TEST("quoted nonexistent dir: still fails, no session");
    snprintf(cmd, sizeof(cmd), "/session new quotedbad '%s/nope'", ws);
    drive(cfg, sm, sched, cmd);
    CHECK(sm->session_count == base + 4, "session count unchanged");
    CHECK(edc_session_manager_find(sm, "quotedbad") == NULL, "the named session was not created");

    /* ── ⑥ 名字带空格 ──────────────────────────────────────── */
    TEST("quoted name with a space: <name> = 'my dev'");
    snprintf(cmd, sizeof(cmd), "/session new 'my dev' %s", real_dir);
    drive(cfg, sm, sched, cmd);
    CHECK(sm->session_count == base + 5, "one session added");
    struct edc_session *spaced_name = edc_session_manager_find(sm, "my dev");
    CHECK(spaced_name != NULL, "session named 'my dev' exists");
    CHECK(bound_to(spaced_name, real_dir), "and it is bound to the given dir");

    /* ── ⑦ 目录带空格：必须加引号（bash 规则：空白分词） ─────── */
    TEST("quoted dir with a space: bound");
    snprintf(cmd, sizeof(cmd), "/session new spaced '%s'", real_spaced);
    drive(cfg, sm, sched, cmd);
    CHECK(sm->session_count == base + 6, "one session added");
    CHECK(bound_to(edc_session_manager_find(sm, "spaced"), real_spaced), "quoted dir with a space bound");

    TEST("unquoted dir with a space: refused, no session");
    snprintf(cmd, sizeof(cmd), "/session new spaced2 %s", real_spaced);
    drive(cfg, sm, sched, cmd);
    CHECK(sm->session_count == base + 6, "session count unchanged");
    CHECK(edc_session_manager_find(sm, "spaced2") == NULL, "no session created from an ambiguous tail");

    /* ── ⑦b 未闭合引号 = 语法错误，不建 session ───────────── */
    TEST("unterminated quote: syntax error, no session");
    drive(cfg, sm, sched, "/session new broken 'D:\github\edgeclaw");
    CHECK(sm->session_count == base + 6, "session count unchanged");
    CHECK(edc_session_manager_find(sm, "broken") == NULL, "no session created");

    /* ── ⑧ switch / rename 也吃 bash 引号 ───────────────────── */
    TEST("/session switch 'my dev': quoted name found");
    drive(cfg, sm, sched, "/session switch 'my dev'");
    CHECK(edc_session_manager_get_active(sm) == spaced_name, "the quoted name switched the active session");

    TEST("/session switch my dev (unquoted): refused as ambiguous");
    drive(cfg, sm, sched, "/session switch my dev");
    CHECK(edc_session_manager_get_active(sm) == spaced_name, "still on the same session");

    TEST("/session rename 'my dev' dev3: two quoted words");
    drive(cfg, sm, sched, "/session rename 'my dev' dev3");
    CHECK(edc_session_manager_find(sm, "dev3") == spaced_name, "renamed to dev3");
    CHECK(bound_to(spaced_name, real_dir), "and it kept its working directory");

    TEST("/session rename one: refused (needs two words)");
    drive(cfg, sm, sched, "/session rename dev3");
    CHECK(edc_session_manager_find(sm, "dev3") == spaced_name, "name unchanged");

    /* ── ⑨ 参数个数由解析器给，多余/缺失都是 usage ──────────── */
    TEST("/session switch a b: two words refused");
    drive(cfg, sm, sched, "/session switch a b");
    CHECK(edc_session_manager_get_active(sm) == spaced_name, "active session unchanged");

    TEST("/session remove (no word): usage, nothing removed");
    drive(cfg, sm, sched, "/session remove");
    CHECK(sm->session_count == base + 6, "session count unchanged");

    TEST("/session stop x: takes no arguments");
    drive(cfg, sm, sched, "/session stop x");
    CHECK(sm->session_count == base + 6, "session count unchanged");

    TEST("/session status with a quoted name: found");
    drive(cfg, sm, sched, "/session status 'dev3'");
    CHECK(sm->session_count == base + 6, "still just reporting");

    /* ── 绑定的目录进了 sandbox 的 rw 集合 ───────────────────── */
    TEST("bound dir granted rw in the sandbox");
    bool granted = false;
    for (int i = 0; cfg->sandbox_rw_paths && cfg->sandbox_rw_paths[i]; i++)
        if (strcmp(cfg->sandbox_rw_paths[i], real_dir) == 0)
            granted = true;
    CHECK(granted, "bound dir present in sandbox rw paths");

    printf("\n  %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
