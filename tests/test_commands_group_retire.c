/* `/group archive|restore|rm [--idle]` — command-level behavior tests.
 *
 * No channel actor in this harness: assert on EFFECTS (the archived flag, foreground
 * move, memory seed, dropped edges, freed group), not reply text. Harness copied from
 * test_commands_group_fold.c: a disposable DB store, the public command entry, then a
 * scheduler tick at the message boundary. */
#include "edc_channel_actor.h"
#include "test_ws.h"
#include "edc_commands.h"
#include "edc_compat.h"
#include "edc_config.h"
#include "edc_scheduler.h"
#include "edc_session.h"
#include "edc_session_manager.h"
#include "edc_session_memory.h"
#include "edc_state_store.h"
#include "edc_task.h"
#include "edc_task_group.h"
#include "utlist_fix.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

volatile int _run_loop = 1;

static int passed = 0, failed = 0;
#define TEST(n)                                                                                                        \
    do                                                                                                                 \
    {                                                                                                                  \
        printf("  TEST: %-55s ", n);                                                                                   \
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

#define RID_BASE 1000000000000000LL
static int64_t fid(const char *name)
{
    return RID_BASE - edc_test_id(name);
}

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

static void drive_command(struct edc_config *cfg, struct edc_session_manager *sm, struct edc_scheduler *sched,
                          const char *content)
{
    struct edc_channel_message *cm =
        edc_channel_message_new(edc_channel_adapter, EDC_CHANNEL_MESSAGE_USER_TYPE, sdsnew(content));
    edc_commands_handler(cfg, sm, sched, (struct edc_reply_to){.type = cm->type}, cm);
    edc_scheduler_schedule(sched);
}

static struct edc_task_group *make_group(struct edc_session *sess, const char *id, bool finished)
{
    struct edc_task *leader = edc_task_new(fid(id), sess->session_id, fid(id), "leader", edc_channel_adapter);
    struct edc_task_group *g = edc_task_group_new(leader);
    edc_session_add_group(sess, g);
    if (finished)
    {
        edc_task_await_exec(leader);
        edc_task_mark_completed(leader);
        leader->completion_seq = 1;
    }
    return g;
}

static struct edc_task *make_member(struct edc_task_group *g, struct edc_session *sess, const char *id, bool finished)
{
    struct edc_task *t = edc_task_new(fid(id), sess->session_id, g->group_id, "member", edc_channel_adapter);
    edc_task_group_add_task(g, t);
    if (finished)
    {
        edc_task_await_exec(t);
        edc_task_mark_completed(t);
    }
    return t;
}

static char *idstr(int64_t id)
{
    static char buf[32];
    snprintf(buf, sizeof buf, "%lld", (long long)id);
    return buf;
}

static void test_archive_restore(void)
{
    TEST("archive hides a finished group, seeds memory, moves foreground; restore un-hides");
    struct edc_config *cfg = g_new0(struct edc_config, 1);
    cfg->workspace = sdsnew(edc_test_workspace());
    cfg->context_window = 1024 * 1024;
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    struct edc_session *sess = edc_session_new(cfg, edc_test_session_id("s-retire"), "Retire");
    struct edc_session_manager *sm = NULL;
    install_resolver(sched, sess);
    sm = sched->sessions;

    struct edc_task_group *g1 = make_group(sess, "retire-g1", true);
    struct edc_task_group *g2 = make_group(sess, "retire-g2", true);
    edc_scheduler_set_foreground(sess, g1);
    struct edc_task *fg_task = make_member(g1, sess, "retire-m1", true);
    sess->active_task = fg_task;

    /* Give the group a NON-EMPTY folded memory. This is the path that returned an sds
     * from group_memory_load; freeing it with free() instead of sdsfree() corrupted the
     * heap and crashed archive (the regression this test pins). */
    CHECK(edc_state_store_group_memory_save(sess->session_id, g1->group_id,
                                            "# folded memory about this group", 0, 0, 0),
          "group memory saved before archive");
    g1->memory_tokens = 8;

    size_t before = sess->memo->entry_count;
    char cmd[256];
    snprintf(cmd, sizeof cmd, "/group archive %s", idstr(g1->group_id));
    drive_command(cfg, sm, sched, cmd);
    CHECK(edc_task_group_is_archived(g1), "group 1 is archived");
    CHECK(g1->archived_at > 0, "archived_at stamped");
    CHECK(edc_scheduler_get_foreground(sess) == g2, "foreground moved to group 2");
    CHECK(sess->active_task == NULL, "active task cleared on foreground move");
    CHECK(sess->memo->entry_count == before + 1, "non-empty group memory seeds exactly one entry");
    CHECK(!edc_task_group_is_archived(g2), "group 2 untouched");

    /* -g on an archived group is refused via the shared accepts_work predicate. */
    int before_members = g1->member_count;
    snprintf(cmd, sizeof cmd, "/task new work -g %s", idstr(g1->group_id));
    drive_command(cfg, sm, sched, cmd);
    CHECK(g1->member_count == before_members, "no task placed into an archived group via -g");

    snprintf(cmd, sizeof cmd, "/group restore %s", idstr(g1->group_id));
    drive_command(cfg, sm, sched, cmd);
    CHECK(!edc_task_group_is_archived(g1), "restore clears the flag");
    CHECK(g1->archived_at == 0, "restore clears the clock");

    edc_scheduler_free(sched);
    edc_session_free(sess);
    free(cfg);
}

static void test_archive_guards(void)
{
    TEST("archive refuses system group, alive members without --force, unknown id");
    struct edc_config *cfg = g_new0(struct edc_config, 1);
    cfg->workspace = sdsnew(edc_test_workspace());
    cfg->context_window = 1024 * 1024;
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    struct edc_session *sess = edc_session_new(cfg, edc_test_session_id("s-guard"), "Guard");
    install_resolver(sched, sess);
    struct edc_session_manager *sm = sched->sessions;

    struct edc_task *sys_task = edc_task_new(fid("guard-sys"), sess->session_id, fid("guard-sys"), "sys", edc_channel_adapter);
    sys_task->sink = EDC_SINK_GROUP_MEMORY;
    struct edc_task_group *sys = edc_task_group_new(sys_task);
    edc_session_add_group(sess, sys);
    char cmd[256];
    snprintf(cmd, sizeof cmd, "/group archive %s", idstr(sys->group_id));
    drive_command(cfg, sm, sched, cmd);
    CHECK(!edc_task_group_is_archived(sys), "system group not archived");

    struct edc_task_group *g = make_group(sess, "guard-g", true);
    make_member(g, sess, "guard-alive", false);
    snprintf(cmd, sizeof cmd, "/group archive %s", idstr(g->group_id));
    drive_command(cfg, sm, sched, cmd);
    CHECK(!edc_task_group_is_archived(g), "alive group not archived without --force");

    drive_command(cfg, sm, sched, "/group archive 999999999999");
    CHECK(!edc_task_group_is_archived(g), "unknown id changes nothing");

    /* archive an already-archived group (covers the "already archived" branch) */
    edc_task_group_set_archived(g, true);
    snprintf(cmd, sizeof cmd, "/group archive %s", idstr(g->group_id));
    drive_command(cfg, sm, sched, cmd);
    CHECK(edc_task_group_is_archived(g), "already-archived group stays archived");

    /* restore a group that is not archived (covers the "not archived" branch) */
    struct edc_task_group *active = make_group(sess, "guard-active", true);
    snprintf(cmd, sizeof cmd, "/group restore %s", idstr(active->group_id));
    drive_command(cfg, sm, sched, cmd);
    CHECK(!edc_task_group_is_archived(active), "restore of a non-archived group is a no-op");

    /* --curate path: opt-in curation is started (no system task pending here). */
    struct edc_task_group *curated = make_group(sess, "guard-curate", true);
    snprintf(cmd, sizeof cmd, "/group archive %s --curate", idstr(curated->group_id));
    drive_command(cfg, sm, sched, cmd);
    CHECK(edc_task_group_is_archived(curated), "--curate archive still archives the group");

    edc_scheduler_free(sched);
    edc_session_free(sess);
    free(cfg);
}

static void test_archive_force(void)
{
    TEST("--force cancels alive members then archives");
    struct edc_config *cfg = g_new0(struct edc_config, 1);
    cfg->workspace = sdsnew(edc_test_workspace());
    cfg->context_window = 1024 * 1024;
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    struct edc_session *sess = edc_session_new(cfg, edc_test_session_id("s-force"), "Force");
    install_resolver(sched, sess);
    struct edc_session_manager *sm = sched->sessions;

    struct edc_task_group *g = make_group(sess, "force-g", true);
    struct edc_task *alive = make_member(g, sess, "force-alive", false);
    char cmd[256];
    snprintf(cmd, sizeof cmd, "/group archive %s --force", idstr(g->group_id));
    drive_command(cfg, sm, sched, cmd);
    CHECK(edc_task_group_is_archived(g), "force-archived with alive member");
    CHECK(edc_task_is_cancelled(alive), "alive member cancelled by force");

    edc_scheduler_free(sched);
    edc_session_free(sess);
    free(cfg);
}

static void test_rm(void)
{
    TEST("rm needs archive + --yes, drops cross-group deps both ways, frees the group");
    struct edc_config *cfg = g_new0(struct edc_config, 1);
    cfg->workspace = sdsnew(edc_test_workspace());
    cfg->context_window = 1024 * 1024;
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    struct edc_session *sess = edc_session_new(cfg, edc_test_session_id("s-rm"), "Rm");
    install_resolver(sched, sess);
    struct edc_session_manager *sm = sched->sessions;

    /* rm of the system group is refused (covers that branch). */
    struct edc_task *sys_task = edc_task_new(fid("rm-sys"), sess->session_id, fid("rm-sys"), "sys", edc_channel_adapter);
    sys_task->sink = EDC_SINK_GROUP_MEMORY;
    struct edc_task_group *sys = edc_task_group_new(sys_task);
    edc_session_add_group(sess, sys);
    char cmd[256];
    snprintf(cmd, sizeof cmd, "/group rm %s --yes", idstr(sys->group_id));
    drive_command(cfg, sm, sched, cmd);
    CHECK(edc_session_find_group(sess, sys->group_id) != NULL, "system group cannot be rm'd");

    struct edc_task_group *victim = make_group(sess, "rm-victim", true);
    const int64_t victim_gid = victim->group_id; /* save: the final rm frees victim */
    struct edc_task *vmem = make_member(victim, sess, "rm-vmem", true);
    struct edc_task_group *other = make_group(sess, "rm-other", true);
    struct edc_task *omem = make_member(other, sess, "rm-omem", true);
    /* An UNFINISHED external dependent keeps its edge to vmem live, so rm's reverse
     * (dependents) drop branch actually fires. */
    struct edc_task *live_dep = make_member(other, sess, "rm-live-dep", false);
    edc_task_add_dependency(live_dep, vmem, TASK_DEP_SEQUENCE);

    edc_task_add_dependency(vmem, omem, TASK_DEP_JOIN);
    edc_task_add_dependency(omem, vmem, TASK_DEP_SEQUENCE);

    snprintf(cmd, sizeof cmd, "/group rm %s --yes", idstr(victim_gid));
    drive_command(cfg, sm, sched, cmd);
    CHECK(edc_session_find_group(sess, victim_gid) != NULL, "rm refused while active");

    snprintf(cmd, sizeof cmd, "/group archive %s --no-harvest", idstr(victim_gid));
    drive_command(cfg, sm, sched, cmd);
    CHECK(edc_task_group_is_archived(victim), "victim archived first");

    snprintf(cmd, sizeof cmd, "/group rm %s", idstr(victim_gid));
    drive_command(cfg, sm, sched, cmd);
    CHECK(edc_session_find_group(sess, victim_gid) != NULL, "rm refused without --yes");

    /* rm of an unknown id and rm with a stray extra operand (usage branch). */
    drive_command(cfg, sm, sched, "/group rm 999999999999 --yes");
    drive_command(cfg, sm, sched, "/group rm extra --yes");

    /* Make victim the foreground AND its member the active task: covers both the
     * foreground-clearing and active_task-in-group branches of hard delete. */
    edc_scheduler_set_foreground(sess, victim);
    sess->active_task = vmem;
    snprintf(cmd, sizeof cmd, "/group rm %s --yes", idstr(victim_gid));
    drive_command(cfg, sm, sched, cmd);
    CHECK(edc_session_find_group(sess, victim_gid) == NULL, "archived group removed");
    CHECK(sess->active_task == NULL, "active task inside removed group is cleared");
    CHECK(omem->deps == NULL, "surviving task's dep on removed member dropped");
    CHECK(edc_task_is_finished(omem), "surviving task still finished");

    edc_scheduler_free(sched);
    edc_session_free(sess);
    free(cfg);
}

static void test_batch_idle_dry_run(void)
{
    TEST("--idle dry-run previews; real run archives only old idle non-foreground groups");
    struct edc_config *cfg = g_new0(struct edc_config, 1);
    cfg->workspace = sdsnew(edc_test_workspace());
    cfg->context_window = 1024 * 1024;
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    struct edc_session *sess = edc_session_new(cfg, edc_test_session_id("s-idle"), "Idle");
    install_resolver(sched, sess);
    struct edc_session_manager *sm = sched->sessions;

    struct edc_task_group *fg = make_group(sess, "idle-fg", true);
    edc_scheduler_set_foreground(sess, fg);
    /* Only groups with a real idle clock (busy_members dropped to 0) are candidates. */
    struct edc_task_group *old = make_group(sess, "idle-old", true);
    edc_task_group_refresh_busy(old, time(NULL) - 86400 * 60);
    /* Pin the idle clock directly: a scheduler tick in drive_command may run the extract
     * scan, which re-stamps idle_since only when busy_members crosses into idle. Setting
     * both to a consistent old-idle state keeps it pinned across those ticks. */
    old->busy_members = 0;
    old->idle_since = time(NULL) - 86400 * 60;

    drive_command(cfg, sm, sched, "/group archive --idle 30d --dry-run --no-harvest");
    CHECK(!edc_task_group_is_archived(old), "dry run archives nothing");
    drive_command(cfg, sm, sched, "/group archive --idle 30d --no-harvest");
    CHECK(edc_task_group_is_archived(old), "idle old group archived");
    CHECK(!edc_task_group_is_archived(fg), "foreground group skipped");

    drive_command(cfg, sm, sched, "/group archive --idle nonsense");
    CHECK(edc_task_group_is_archived(old), "bad duration leaves state unchanged");

    /* Cover the remaining batch branches: bare archive, --idle with no duration, an
     * unknown option, an idle group with a live member (skip, then --force). */
    drive_command(cfg, sm, sched, "/group archive");
    drive_command(cfg, sm, sched, "/group archive --idle");
    drive_command(cfg, sm, sched, "/group archive --idle 30d --bogus");
    struct edc_task_group *busy_idle = make_group(sess, "idle-busy", false);
    busy_idle->busy_members = 0;
    busy_idle->idle_since = time(NULL) - 86400 * 60;
    drive_command(cfg, sm, sched, "/group archive --idle 30d --no-harvest");
    CHECK(!edc_task_group_is_archived(busy_idle), "idle group with live member skipped without --force");
    drive_command(cfg, sm, sched, "/group archive --idle 30d --force --no-harvest");
    CHECK(edc_task_group_is_archived(busy_idle), "--force archives idle group with live member");

    drive_command(cfg, sm, sched, "/group list");
    drive_command(cfg, sm, sched, "/group list --all");
    drive_command(cfg, sm, sched, "/group frobnicate");

    edc_scheduler_free(sched);
    edc_session_free(sess);
    free(cfg);
}

/* The whole point: an idle clock set in a PREVIOUS process must survive a restart and
 * still be selected by --idle. Before persistence, load re-stamped it to "now" and the
 * old group was invisible to --idle forever. */
static void test_idle_survives_restart(void)
{
    TEST("idle_since survives flush -> load (restart); --idle still archives the old group");
    struct edc_config *cfg = g_new0(struct edc_config, 1);
    cfg->workspace = sdsnew(edc_test_workspace());
    cfg->context_window = 1024 * 1024;

    /* --- process 1: build an old-idle group and flush it to the DB --- */
    struct edc_scheduler *s1 = edc_scheduler_new(cfg);
    struct edc_session *sess1 = edc_session_new(cfg, edc_test_session_id("s-restart"), "Restart");
    install_resolver(s1, sess1);
    struct edc_task_group *old = make_group(sess1, "restart-old", true);
    old->busy_members = 0;
    old->idle_since = time(NULL) - 86400 * 60; /* 60 days idle */
    edc_state_store_flush(sess1, s1);
    const int64_t old_gid = old->group_id;
    edc_scheduler_free(s1);
    edc_session_free(sess1);

    /* --- process 2: a FRESH session loads from the DB, then --idle runs --- */
    struct edc_scheduler *s2 = edc_scheduler_new(cfg);
    struct edc_session *sess2 = edc_session_new(cfg, edc_test_session_id("s-restart"), "Restart");
    install_resolver(s2, sess2);
    struct edc_session_manager *sm2 = s2->sessions;
    CHECK(edc_state_store_load(sess2, s2), "session loads after 'restart'");
    struct edc_task_group *reloaded = edc_session_find_group(sess2, old_gid);
    CHECK(reloaded != NULL, "old group is present after reload");
    CHECK(reloaded && reloaded->idle_since > 0 && reloaded->idle_since < time(NULL) - 86400 * 30,
          "idle_since survived the reload (still 60 days old, not re-stamped to now)");

    drive_command(cfg, sm2, s2, "/group archive --idle 30d --no-harvest");
    CHECK(reloaded && edc_task_group_is_archived(reloaded),
          "--idle archives the old group even after a restart");

    edc_scheduler_free(s2);
    edc_session_free(sess2);
    free(cfg);
}

int main(void)
{
    edc_test_workspace();
    test_archive_restore();
    test_archive_guards();
    test_archive_force();
    test_rm();
    test_batch_idle_dry_run();
    test_idle_survives_restart();
    printf("\n%d passed, %d failed\n", passed, failed);
    edc_test_store_down(NULL);
    return failed ? 1 : 0;
}
