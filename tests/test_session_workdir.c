#include "edc_compat.h"
#include "edc_config.h"
#include "edc_context.h"
#include "edc_channel_actor.h"
#include "edc_error.h"
#include "edc_file_utils.h"
#include "edc_sandbox.h"
#include "edc_scheduler.h"
#include "edc_session.h"
#include "edc_session_manager.h"
#include "edc_state_serializer.h"
#include "log.h"
#include <ctype.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
/* ── 单测的 session 仓库 ─────────────────────────────────────────
 * scheduler 现在**同包直调 store** (P3 的 `resolve_session` 端口已退休),
 * 所以测试给它的不是"解析函数"而是**仓库**: 一个只装着
 * 这些 session 的内存 manager (普通结构体, 没有构造函数 —— 与 poke 别的结构体同法)。
 * 标签为空 = 用活跃的那个 (active_index 指向第一个), 与真 store 的策略一致。
 * `ctx` 仍留给测试自己用 (它不再是读口的入参)。 */
static void install_resolver(struct edc_scheduler *sched, struct edc_session *sess)
{
    sched->ctx = sess;
    struct edc_session_manager *sm = g_new0(struct edc_session_manager, 1);
    sm->sessions = g_new0(struct edc_session *, 1);
    sm->sessions[0] = sess;
    sm->session_count = sess ? 1 : 0;
    sm->active_index = 0;
    sched->sessions = sm; /* 测试进程内不释放: 与测试同生共死 */
}
/* Stub: referenced transitively by the shell/command layer. */
volatile sig_atomic_t _run_loop = 1;

static int test_failed = 0;
static int test_passed = 0;

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
            test_failed++;                                                                                             \
        }                                                                                                              \
        else                                                                                                           \
        {                                                                                                              \
            test_passed++;                                                                                             \
        }                                                                                                              \
    } while (0)

static struct edc_config *make_config(const char *workspace)
{
    struct edc_config *cfg = g_new0(struct edc_config, 1);
    cfg->workspace = sdsnew(workspace);
    return cfg;
}

/* Create a fresh temporary directory; caller rmdir()s it. */
static void make_tmpdir(char *buf, size_t cap)
{
    snprintf(buf, cap, "/tmp/edc_workdir_test_XXXXXX");
    if (!mkdtemp(buf))
    {
        perror("mkdtemp");
        exit(2);
    }
}

/* ── 1. Explicit working directory is resolved and granted in the sandbox ── */
static void test_explicit_workdir(void)
{
    TEST("explicit workdir is bound + granted rw");
    char dir[256];
    make_tmpdir(dir, sizeof(dir));

    struct edc_config *cfg = make_config("/tmp/test_edc_workdir_ws");
    struct edc_session *s = edc_session_new_with_workdir(cfg, "sess-w", "W", dir);

    char real[PATH_MAX];
    CHECK(realpath(dir, real) != NULL, "realpath(dir)");
    CHECK(s->workdir != NULL, "workdir bound");
    CHECK(s->workdir && strcmp(s->workdir, real) == 0, "workdir == realpath(dir)");
    CHECK(s->workdir && strcmp(edc_session_get_workdir(s), real) == 0, "get_workdir == realpath(dir)");

    /* The bound directory must be admissible: present in the rw set. */
    bool found = false;
    for (int i = 0; cfg->sandbox_rw_paths && cfg->sandbox_rw_paths[i]; i++)
        if (strcmp(cfg->sandbox_rw_paths[i], real) == 0)
            found = true;
    CHECK(found, "bound workdir present in sandbox rw paths");

    edc_session_free(s);
    edc_config_free(cfg);
    rmdir(dir);
}

/* ── 2. No workdir → unbound (default) ── */
static void test_default_unbound(void)
{
    TEST("no workdir → unbound");
    struct edc_config *cfg = make_config("/tmp/test_edc_workdir_ws");
    struct edc_session *s = edc_session_new(cfg, "sess-def", "Def");
    CHECK(edc_session_get_workdir(s) == NULL, "default session is unbound");
    edc_session_free(s);
    edc_config_free(cfg);
}

/* ── 3. Bad directory → creation fails (no default fallback) ── */
static void test_invalid_workdir(void)
{
    TEST("invalid workdir fails creation (no default fallback)");
    struct edc_config *cfg = make_config("/tmp/test_edc_workdir_ws");
    struct edc_session *s = edc_session_new_with_workdir(cfg, "sess-bad", "Bad", "/no/such/dir/edgeclaw-xyz");
    CHECK(s == NULL, "session creation fails on a nonexistent workdir");
    edc_config_free(cfg);
}

/* ── 3b. Explicit relative workdir resolves against a base ── */
static void test_resolve_relative(void)
{
    TEST("relative workdir resolves to absolute against base");
    char dir[256];
    make_tmpdir(dir, sizeof(dir));
    char sub[300];
    snprintf(sub, sizeof(sub), "%s/sub", dir);
    mkdir(sub, 0755);

    char real_sub[PATH_MAX];
    CHECK(realpath(sub, real_sub) != NULL, "realpath(sub)");

    /* Relative to an explicit base. */
    EdcError *err = NULL;
    sds resolved = edc_session_resolve_workdir(dir, "sub", &err);
    CHECK(resolved != NULL, "relative path resolved");
    CHECK(resolved && strcmp(resolved, real_sub) == 0, "relative path anchored at base");
    sdsfree(resolved);
    edc_error_free(err);

    /* A nonexistent relative path fails (no fallback). */
    EdcError *err2 = NULL;
    sds missing = edc_session_resolve_workdir(dir, "does-not-exist", &err2);
    CHECK(missing == NULL, "nonexistent relative path fails");
    edc_error_free(err2);

    rmdir(sub);
    rmdir(dir);
}

/* ── 3c. "~" expands to the home directory (not anchored at base) ── */
static void test_resolve_tilde(void)
{
    TEST("~ expands to home, not anchored at base");
    sds home = edc_homedir();
    char real_home[PATH_MAX];
    CHECK(realpath(home, real_home) != NULL, "realpath(home)");

    EdcError *err = NULL;
    sds r = edc_session_resolve_workdir("/tmp/ignored-base", "~", &err);
    CHECK(r != NULL, "~ resolved");
    CHECK(r && strcmp(r, real_home) == 0, "~ == home (ignore base)");
    sdsfree(r);
    edc_error_free(err);

    EdcError *err2 = NULL;
    sds r2 = edc_session_resolve_workdir("/tmp/ignored-base", "~/.", &err2);
    CHECK(r2 != NULL, "~/. resolved");
    CHECK(r2 && strcmp(r2, real_home) == 0, "~/. == home");
    sdsfree(r2);
    edc_error_free(err2);

    sdsfree(home);
}

/* ── 4. Workdir survives a serialize/deserialize round-trip ── */
static void test_persist_roundtrip(void)
{
    TEST("workdir persists across state round-trip");
    char dir[256];
    make_tmpdir(dir, sizeof(dir));
    char real[PATH_MAX];
    realpath(dir, real);

    struct edc_config *cfg = make_config("/tmp/test_edc_workdir_ws");
    struct edc_session *s = edc_session_new_with_workdir(cfg, "sess-p", "P", dir);

    cJSON *json = edc_state_serialize(s, NULL);
    CHECK(json != NULL, "serialize");

    struct edc_session *s2 = edc_session_new(cfg, "sess-p", "P");
    CHECK(edc_session_get_workdir(s2) == NULL, "fresh session starts unbound");
    bool ok = edc_state_deserialize(json, s2, NULL);
    CHECK(ok, "deserialize");
    CHECK(s2->workdir && strcmp(s2->workdir, real) == 0, "workdir restored from state");

    cJSON_Delete(json);
    edc_session_free(s);
    edc_session_free(s2);
    edc_config_free(cfg);
    rmdir(dir);
}

/* ── 5. Relative paths resolve against the session base dir ── */
static void test_verify_path_at(void)
{
    TEST("relative path resolves against bound dir");
    char dir[256];
    make_tmpdir(dir, sizeof(dir));
    char real[PATH_MAX];
    realpath(dir, real);

    struct edc_config *cfg = make_config("/tmp/test_edc_workdir_ws");
    cfg->enable_sandbox = true;
    cfg->sandbox_root_real = sdsnew(real);
    edc_config_add_sandbox_rw(cfg, real);

    /* A relative path inside the bound dir is allowed and resolved under it.
     * The parent (the bound dir itself) exists, so a not-yet-created file is
     * resolvable for writing. */
    EdcError *err = NULL;
    sds resolved = edc_sandbox_verify_path_at(cfg, real, "note.txt", true, &err);
    CHECK(resolved != NULL, "relative path inside base allowed");
    if (resolved)
    {
        sds expect = edc_path_join(real, "note.txt", NULL);
        CHECK(strcmp(resolved, expect) == 0, "relative path joined onto base");
        sdsfree(expect);
        sdsfree(resolved);
    }
    else
    {
        edc_error_free(err);
    }

    /* An absolute path outside the sandbox is still denied. */
    EdcError *err2 = NULL;
    sds denied = edc_sandbox_verify_path_at(cfg, real, "/etc/shadow", false, &err2);
    CHECK(denied == NULL, "absolute path outside sandbox denied");
    if (denied)
        sdsfree(denied);
    else
        edc_error_free(err2);

    edc_config_free(cfg);
    rmdir(dir);
}

/* ── 6. System prompt reports the session's directory as {{cwd}} ── */
static void test_system_prompt_cwd(void)
{
    TEST("system prompt uses the session workdir as current dir");
    char dir[256];
    make_tmpdir(dir, sizeof(dir));
    char real[PATH_MAX];
    realpath(dir, real);

    struct edc_config *cfg = make_config("/tmp/test_edc_workdir_ws");
    sds prompt = edc_context_build_system_prompt(cfg, real);
    CHECK(prompt != NULL, "prompt built");
    CHECK(prompt && strstr(prompt, real) != NULL, "prompt contains bound workdir");
    sdsfree(prompt);
    edc_config_free(cfg);
    rmdir(dir);
}

/* pi 的纪律 (docs/pi-system-prompt-guide.md §7: "无任何日期/时间戳注入 … 跨天、重启、resume
 * prompt 字节完全不变")。这里钉住它 —— 提示词里不许有时钟, 也不许留未替换的占位符。 */
static void test_system_prompt_has_no_clock(void)
{
    TEST("system prompt: 无时钟 / 无未替换占位符 (pi §7 的确定性纪律)");

    struct edc_config *cfg = make_config("/tmp/test_edc_noclock_ws");
    sds a = edc_context_build_system_prompt(cfg, NULL);
    sds b = edc_context_build_system_prompt(cfg, NULL);
    CHECK(a && b && strcmp(a, b) == 0, "连续构造两次逐字节相同");
    CHECK(a && strstr(a, "Current Time") == NULL, "没有 Current Time 段");
    CHECK(a && strstr(a, "no clock") != NULL, "有常量行说明：要时间就跑 date");
    CHECK(a && strstr(a, "{{time}}") == NULL, "没有未替换的 {{time}} 占位符");
    /* 注意 {{skill-name}} 是**故意**留给模型看的路径模板, 不是漏替换。 */

    /* 时间戳形如 2026-10-10 … —— 提示词里不该出现任何 20xx- 的日期串。 */
    bool has_date = false;
    if (a)
    {
        for (const char *q = a; (q = strstr(q, "20")) != NULL; q += 2)
        {
            if (isdigit((unsigned char)q[2]) && isdigit((unsigned char)q[3]) && q[4] == '-')
                has_date = true;
        }
    }
    CHECK(!has_date, "不含 20xx- 形式的日期");

    sdsfree(a);
    sdsfree(b);
    edc_config_free(cfg);
}

/* 全局记忆不再是提示词里的常驻块 (2026-10-10 裁决 ⑥ + pi §2.10 的方向): 提示词里只有
 * 常量指路, MEMORY.md 的**正文**一个字都不许出现 —— 它是 agent 自己会写的东西, 常驻就等于
 * 「一改就把整段前缀作废」, 而且本次会话里模型反而读不到最新版。 */
static void test_memory_is_not_resident(void)
{
    TEST("system prompt: 不装 MEMORY.md 正文 (只留常量指路)");

    char dir[256];
    make_tmpdir(dir, sizeof(dir)); /* 本文件的约定: 传缓冲 + 容量 */
    char memdir[512];
    snprintf(memdir, sizeof(memdir), "%s/memory", dir);
    EdcError *err = NULL;
    CHECK(edc_dir_create_with_parents(memdir, 0755, &err), "建 memory 目录");
    char memfile[600];
    snprintf(memfile, sizeof(memfile), "%s/MEMORY.md", memdir);
    const char *marker = "MEMORY-CONTENT-MARKER-9f3a\n";
    CHECK(edc_file_write(memfile, marker, -1, &err), "写 MEMORY.md");

    struct edc_config *cfg = make_config(dir);
    sds prompt = edc_context_build_system_prompt(cfg, NULL);
    CHECK(prompt && strstr(prompt, "MEMORY-CONTENT-MARKER-9f3a") == NULL, "正文不在提示词里");
    CHECK(prompt && strstr(prompt, "MEMORY.md") != NULL, "但指了路 (路径还在)");
    CHECK(prompt && strstr(prompt, "not** in this prompt") != NULL, "并说明要自己去读");
    sdsfree(prompt);
    edc_config_free(cfg);
    unlink(memfile);
    rmdir(memdir);
    rmdir(dir);
}

/* ── 7. Tool calls use the task's OWNER session workdir ── */
static void test_task_workdir_owner(void)
{
    TEST("tool workdir follows the task's owner session");
    char dirA[256], dirB[256];
    make_tmpdir(dirA, sizeof(dirA));
    make_tmpdir(dirB, sizeof(dirB));
    char realA[PATH_MAX], realB[PATH_MAX];
    CHECK(realpath(dirA, realA) != NULL, "realpath(A)");
    CHECK(realpath(dirB, realB) != NULL, "realpath(B)");

    struct edc_config *cfg = make_config("/tmp/test_edc_workdir_ws");
    struct edc_session *A = edc_session_new_with_workdir(cfg, "sess-A", "A", dirA);
    struct edc_session *B = edc_session_new_with_workdir(cfg, "sess-B", "B", dirB);

    /* The task belongs to B (its group is bound to B): a tool call from that task
     * must resolve against B's directory. P3 ③: 目录**只看属主** —— core 里已经没有
     * "当前 session"(A) 可以回退, 所以"谁在看"这件事根本进不来。 */
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, A);

    struct edc_task *tB = edc_task_new("tB", "sess-B", "tB", "desc", edc_channel_adapter);
    struct edc_task_group *gB = edc_task_group_new(tB);
    edc_session_add_group(B, gB);

    CHECK(strcmp(edc_session_get_workdir(A), realA) == 0, "session A's own dir");
    CHECK(strcmp(edc_scheduler_task_workdir(tB), realB) == 0, "task owner dir = B (not the displayed A)");
    /* 没有属主就没有目录: 不再是"回退到当前 session"(那份副本已删)。工具层拿到
     * NULL 时落到沙箱根 (见 edc_tools_shell / edc_tools_fs)。 */
    struct edc_task *ownerless = edc_task_new("tX", "sess-X", "tX", "desc", edc_channel_adapter);
    CHECK(edc_scheduler_task_workdir(ownerless) == NULL, "ownerless task → NULL (core 不猜)");
    CHECK(edc_scheduler_task_workdir(NULL) == NULL, "no task → NULL");

    if (ownerless)
        edc_task_free(ownerless);
    edc_scheduler_free(sched);
    edc_session_free(A);
    edc_session_free(B);
    edc_config_free(cfg);
    rmdir(dirA);
    rmdir(dirB);
}

/* ── 10. 会话目录的约定 ─────────────────────────────────────
 * `edc_app_chat_log_path_for` (src/app/edc_api.c) 现在**按约定直接拼**路径
 * `<workspace>/<session_id>/chat.jsonl`, 不再问 session 仓库 (仓库由 actor 线程改)。
 * 那条推导的根据就是这里钉住的约定 —— 它一旦变了, 前端会静默写到另一个地方,
 * 所以两边都得看得见这条断言。 */
static void test_history_dir_convention(void)
{
    TEST("session dir is <workspace>/<session_id> (frontend derives chat.jsonl from it)");
    struct edc_config *cfg = make_config("/tmp/test_edc_workdir_ws");
    struct edc_session *s = edc_session_new(cfg, "sess-dir", "D");

    sds expected = edc_path_join(cfg->workspace, "sess-dir", NULL);
    CHECK(s->history_dir != NULL, "history_dir is set");
    CHECK(s->history_dir && strcmp(s->history_dir, expected) == 0, "history_dir == <workspace>/<session_id>");
    sdsfree(expected);
    edc_session_free(s);

    /* Binding a workdir must not move the state directory: cwd and storage are
     * two different things, and the frontend only knows the second one. */
    struct edc_session *b = edc_session_new_with_workdir(cfg, "sess-dir2", "B", "/tmp");
    sds expected2 = edc_path_join(cfg->workspace, "sess-dir2", NULL);
    CHECK(b->history_dir && strcmp(b->history_dir, expected2) == 0, "bound workdir does not change history_dir");
    sdsfree(expected2);
    edc_session_free(b);

    edc_config_free(cfg);
}

int main(void)
{
    log_info("== test_session_workdir ==");

    test_explicit_workdir();
    test_default_unbound();
    test_invalid_workdir();
    test_resolve_relative();
    test_resolve_tilde();
    test_persist_roundtrip();
    test_verify_path_at();
    test_system_prompt_cwd();
    test_system_prompt_has_no_clock();
    test_memory_is_not_resident();
    test_task_workdir_owner();
    test_history_dir_convention();

    log_info("passed=%d failed=%d", test_passed, test_failed);
    return test_failed == 0 ? 0 : 1;
}
