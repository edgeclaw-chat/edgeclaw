/* Regression: a memory system task's description must say WHAT it works on.
 *
 * The sidebar / `/task status` used to show the same bare literal for every
 * round -- "fold group memory", "curate session memory", "extract session
 * memory" -- with no session, group, member tasks or cut boundary. In a session
 * with hundreds of groups and many background rounds, the user could not tell
 * which group a round belonged to or which messages it covered.
 *
 * Pinned here: the description names the kind, the session, the source group,
 * at least one member task that contributed material, and the cut (task / turn /
 * message). The manual and compact variants name their session-wide scope
 * instead of a single group. These assertions are RED against the old literals.
 */

#include "test_ws.h"
#include "edc_compat.h"
#include "edc_config.h"
#include "edc_group_fold.h"
#include "edc_scheduler.h"
#include "edc_session.h"
#include "edc_session_manager.h"
#include "edc_session_memory.h"
#include "edc_task.h"
#include "edc_task_group.h"
#include "edc_turn.h"
#include "log.h"
#include "utlist_fix.h"
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

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

static void install_resolver(struct edc_scheduler *sched, struct edc_session *sess)
{
    struct edc_session_manager *sm = g_new0(struct edc_session_manager, 1);
    sm->sessions = g_new0(struct edc_session *, 1);
    sm->sessions[0] = sess;
    sm->session_count = sess ? 1 : 0;
    sm->active_index = 0;
    sched->sessions = sm; /* 测试进程内不释放 */
}

static struct edc_config *make_config(void)
{
    struct edc_config *cfg = g_new0(struct edc_config, 1);
    cfg->workspace = sdsnew(edc_test_workspace());
    return cfg;
}

static void turn_add(struct edc_turn *turn, const char *role, const char *content, const char *ts)
{
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "role", role);
    cJSON_AddStringToObject(m, "content", content);
    if (ts)
        cJSON_AddStringToObject(m, "ts", ts);
    edc_turn_add_message(turn, m);
}

/* A finished user group with ONE member and a few committed messages -- enough
 * material for a fold or an extraction round. */
static struct edc_task_group *make_group(struct edc_session *sess, const char *gid, int n_msgs)
{
    struct edc_task *leader = edc_task_new(edc_test_id(gid), sess->session_id, edc_test_id(gid), "member work",
                                           edc_channel_adapter);
    struct edc_task_group *g = edc_task_group_new(leader);
    edc_session_add_group(sess, g);
    struct edc_turn *turn = edc_turn_new();
    turn->task_id = leader->task_id;
    for (int i = 0; i < n_msgs; i++)
        turn_add(turn, "user", "some material to compress", "2000-01-01 00:00:00");
    edc_task_add_turn(leader, turn);
    edc_task_await_exec(leader);
    edc_task_mark_completed(leader);
    leader->completion_seq = 1;
    g->busy_members = 0;
    g->idle_since = time(NULL) - (EDC_MEMO_EXTRACT_IDLE_SECS + 60);
    return g;
}

static struct edc_task *find_system_task(struct edc_session *sess, const char *prefix)
{
    struct edc_task_group *sg = edc_session_find_system_group(sess);
    if (!sg)
        return NULL;
    struct edc_task *t;
    LL_FOREACH2(sg->members, t, group_next)
    {
        if (t->description && strncmp(t->description, prefix, strlen(prefix)) == 0 && !edc_task_is_finished(t))
            return t;
    }
    return NULL;
}

/* The description must put the four coordinates on ONE line. |id| is rendered
 * with the same edc_id_str the implementation uses. */
static void check_scope(const char *what, const char *desc, int64_t session_id, int64_t group_id,
                        int64_t member_id, bool with_cut)
{
    char sid[32], gid[32], mid[32];
    snprintf(sid, sizeof(sid), "%" PRId64, session_id);
    snprintf(gid, sizeof(gid), "%" PRId64, group_id);
    snprintf(mid, sizeof(mid), "%" PRId64, member_id);
    char buf[256];

    snprintf(buf, sizeof(buf), "%s: description exists", what);
    CHECK(desc != NULL, buf);
    if (!desc)
        return;
    log_info("    %s desc: %s", what, desc);
    snprintf(buf, sizeof(buf), "%s: names the session %s", what, sid);
    CHECK(strstr(desc, sid) != NULL, buf);
    snprintf(buf, sizeof(buf), "%s: names the source group %s", what, gid);
    CHECK(strstr(desc, gid) != NULL, buf);
    snprintf(buf, sizeof(buf), "%s: names a member task %s", what, mid);
    CHECK(strstr(desc, mid) != NULL, buf);
    if (with_cut)
    {
        snprintf(buf, sizeof(buf), "%s: names the cut boundary", what);
        CHECK(strstr(desc, "cut") != NULL, buf);
    }
    /* A bare literal is the bug. */
    snprintf(buf, sizeof(buf), "%s: not the old bare literal", what);
    CHECK(strstr(desc, "(") != NULL, buf);
}

static void test_fold_and_curate_desc(void)
{
    TEST("fold + chained curate describe their group, tasks and cut");
    struct edc_config *cfg = make_config();
    struct edc_session *sess = edc_session_new(cfg, edc_test_session_id("sess-desc1"), "Desc1");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, sess);
    struct edc_task_group *g = make_group(sess, "gd1", 6);

    struct edc_task *fold = edc_group_fold_start(sched, sess, g, NULL);
    CHECK(fold != NULL, "fold started");
    if (!fold)
        return;
    check_scope("fold", fold->description, sess->session_id, g->group_id, edc_test_id("gd1"), true);
    CHECK(strstr(fold->description, "fold group memory") != NULL, "fold: still says what kind of task it is");

    /* The fold chain creates the L2 curation task in the same system group. */
    struct edc_task *curate = NULL;
    struct edc_task_group *sg = edc_session_find_system_group(sess);
    struct edc_task *t;
    LL_FOREACH2(sg->members, t, group_next)
    {
        if (t != fold && t->sink == EDC_SINK_SESSION_MEMORY)
            curate = t;
    }
    CHECK(curate != NULL, "chained curation task exists");
    if (curate)
    {
        check_scope("curate", curate->description, sess->session_id, g->group_id, edc_test_id("gd1"), false);
        CHECK(strstr(curate->description, "curate session memory") != NULL,
              "curate: still says what kind of task it is");
        CHECK(strstr(curate->description, "from fold") != NULL,
              "curate: names the fold task whose output it curates (it has no cut of its own)");
    }
}

static void test_extract_desc(void)
{
    TEST("extract describes its group, tasks and cut");
    struct edc_config *cfg = make_config();
    struct edc_session *sess = edc_session_new(cfg, edc_test_session_id("sess-desc2"), "Desc2");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, sess);
    struct edc_task_group *g = make_group(sess, "gd2", 4);

    struct edc_session *out = NULL;
    CHECK(edc_session_memory_extract_try(sched, sched->sessions, cfg, &out), "extraction round started");
    struct edc_task *task = find_system_task(sess, "extract");
    CHECK(task != NULL, "extraction task found");
    if (task)
    {
        check_scope("extract", task->description, sess->session_id, g->group_id, edc_test_id("gd2"), true);
        CHECK(strstr(task->description, "extract knowledge candidates") != NULL,
              "extract: still says what kind of task it is (L1 knowledge-candidate summary)");
    }
}

static void test_manual_curate_and_compact_desc(void)
{
    TEST("manual curate / compact describe their session-wide scope");
    struct edc_config *cfg = make_config();

    /* Manual curation: all groups' memories. */
    {
        struct edc_session *sess = edc_session_new(cfg, edc_test_session_id("sess-desc3"), "Desc3");
        struct edc_scheduler *sched = edc_scheduler_new(cfg);
        install_resolver(sched, sess);
        struct edc_task *t = edc_session_memory_start_manual(sched, sess, "keep the numbers");
        CHECK(t != NULL, "manual curation started");
        if (t)
        {
            char sid[32];
            snprintf(sid, sizeof(sid), "%" PRId64, sess->session_id);
            log_info("    manual desc: %s", t->description);
            CHECK(strstr(t->description, "curate session memory") != NULL, "manual: still says what kind it is");
            CHECK(strstr(t->description, sid) != NULL, "manual: names the session");
            CHECK(strstr(t->description, "all groups") != NULL, "manual: says it covers all groups");
        }
    }

    /* Compaction: whole session memory. */
    {
        struct edc_session *sess = edc_session_new(cfg, edc_test_session_id("sess-desc4"), "Desc4");
        struct edc_scheduler *sched = edc_scheduler_new(cfg);
        install_resolver(sched, sess);
        struct edc_task *t = edc_session_memory_start_compact(sched, sess);
        CHECK(t != NULL, "compaction started");
        if (t)
        {
            char sid[32];
            snprintf(sid, sizeof(sid), "%" PRId64, sess->session_id);
            log_info("    compact desc: %s", t->description);
            CHECK(strstr(t->description, "compact session memory") != NULL, "compact: still says what kind it is");
            CHECK(strstr(t->description, sid) != NULL, "compact: names the session");
            CHECK(strstr(t->description, "all entries") != NULL, "compact: says it covers all entries");
        }
    }
}

int main(void)
{
    edc_test_workspace();
    log_info("=== system task descriptions: scope is visible ===");
    test_fold_and_curate_desc();
    test_extract_desc();
    test_manual_curate_and_compact_desc();
    log_info("=== Results: %d passed, %d failed ===", test_passed, test_failed);
    return test_failed > 0 ? 1 : 0;
}
