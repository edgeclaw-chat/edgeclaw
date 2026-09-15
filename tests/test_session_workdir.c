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
    test_task_workdir_owner();

    log_info("passed=%d failed=%d", test_passed, test_failed);
    return test_failed == 0 ? 0 : 1;
}
