#include "edc_scheduler.h"
#include "edc_session.h"
#include "edc_session_manager.h"
#include "edc_compat.h"
#include "edc_config.h"
#include "edc_channel_actor.h"
#include "edc_llm_actor.h"
#include "edc_toolbox_actor.h"
#include "edc_task_group.h"
#include "edc_turn.h"
#include "log.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <sys/stat.h>
/* The in-memory session store handed to the scheduler; an empty label means the
 * active session (active_index points at the first). */
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
volatile sig_atomic_t _run_loop;

static int test_failed = 0;
static int test_passed = 0;

#define TEST(name) do { log_info("  TEST: %s", name); } while(0)
#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        log_error("    FAIL: " __VA_ARGS__); \
        test_failed++; \
    } else { \
        test_passed++; \
    } \
} while(0)

/* ── 辅助: 创建最小 session/config ────────────────────────── */

static struct edc_config *make_config(void)
{
    struct edc_config *c = calloc(1, sizeof(struct edc_config));
    c->workspace = sdsnew("/tmp/test-workspace");
    return c;
}

static struct edc_session *make_session(struct edc_config *config)
{
    /* 直接分配 session 结构，跳过 edc_session_new 的初始化 */
    struct edc_session *s = calloc(1, sizeof(struct edc_session));
    s->config = config;
    return s;
}

static void free_config(struct edc_config *c)
{
    if (!c) return;
    sdsfree(c->workspace);
    free(c);
}

/* Remove the persisted task-id high-water file so a test starts from a clean
 * counter. Also make sure the workspace dir exists (writes need it). */
static void clean_task_seq(struct edc_config *cfg)
{
    if (!cfg || !cfg->workspace) return;
    mkdir(cfg->workspace, 0755); /* EEXIST is fine */
    sds seq = sdscatprintf(sdsempty(), "%s/.task_seq", cfg->workspace);
    remove(seq);
    sdsfree(seq);
}

/* P3: 入站消息经 store 读口解析 session —— 定义在下面 (drive_input 附近), 这里
 * 只是前向声明 (第一个用它的测试在上面)。 */
static void install_resolver(struct edc_scheduler *sched, struct edc_session *sess);

/* ── 1. 生命周期 ──────────────────────────────────────────── */

static void test_scheduler_lifecycle(void)
{
    TEST("scheduler_new / free");
    struct edc_config *cfg = make_config();
    clean_task_seq(cfg);
    struct edc_session *sess = make_session(cfg);
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, sess);

    CHECK(sched != NULL, "scheduler_new returns non-NULL");
    CHECK(sched->next_task_id == 1, "next_task_id starts at 1");
    CHECK(edc_scheduler_get_actor(sched) != NULL, "get_actor returns non-NULL");
    CHECK(edc_scheduler_get_foreground(sess) == NULL, "no foreground group initially");

    edc_scheduler_free(sched);
    free(sess);
    free_config(cfg);

    TEST("scheduler free handles NULL");
    edc_scheduler_free(NULL);
    CHECK(1, "scheduler_free(NULL) survived");
}

/* ── 2. Task 管理 ─────────────────────────────────────────── */

static void test_scheduler_task_mgmt(void)
{
    TEST("create_task generates IDs");
    struct edc_config *cfg = make_config();
    struct edc_session *sess = make_session(cfg);
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, sess);

    struct edc_task *t1 = edc_scheduler_task_new(sched, sess, "task one", edc_channel_adapter);
    struct edc_task *t2 = edc_scheduler_task_new(sched, sess, "task two", edc_channel_adapter);

    CHECK(t1 != NULL, "task 1 created");
    CHECK(t2 != NULL, "task 2 created");
    CHECK(t1->task_id != NULL, "task 1 id non-NULL");
    CHECK(t2->task_id != NULL, "task 2 id non-NULL");
    CHECK(strcmp(t1->task_id, t2->task_id) != 0, "task IDs are unique");
    CHECK(edc_task_is_ready(t1), "task starts READY");

    TEST("find_task_by_id cross-group");
    struct edc_task_group *g1 = edc_scheduler_create_group(sched, sess, t1);
    edc_task_group_add_task(g1, t2);

    CHECK(edc_scheduler_find_task_by_id(sched, sess, t1->task_id) == t1, "find task-1 by ID");
    CHECK(edc_scheduler_find_task_by_id(sched, sess, t2->task_id) == t2, "find task-2 by ID");
    CHECK(edc_scheduler_find_task_by_id(sched, sess, "nonexistent") == NULL, "find nonexistent → NULL");

    TEST("find_task_by_id NULL safety");
    CHECK(edc_scheduler_find_task_by_id(sched, sess, NULL) == NULL, "NULL id → NULL");
    CHECK(edc_scheduler_find_task_by_id(NULL, sess, "x") == NULL, "NULL sched → NULL");

    edc_scheduler_free(sched);
    free(sess);
    free_config(cfg);
}

/* 2b. The "task created" event, emitted by edc_scheduler_task_new, carrying the
 * task id and the raw input (sched->input_raw). The frontend half is in
 * tests/test_tui_dispatch.cpp. */

static void test_task_history_pull(void)
{
    struct edc_config *cfg = make_config();
    struct edc_session *sess = make_session(cfg);
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, sess);

    sched->input_raw = "please fix the parser";
    struct edc_task *t1 = edc_scheduler_task_new(sched, sess, "fix", edc_channel_adapter);
    sched->input_raw = NULL;
    struct edc_turn *turn = edc_turn_new();
    edc_turn_add_user_message(turn, "please fix the parser");
    edc_turn_add_assistant_message(turn, cJSON_Parse("{\"role\":\"assistant\",\"content\":\"on it\"}"));
    edc_task_add_turn(t1, turn);
    edc_scheduler_register_task(sched, t1, NULL); /* 生产路径总会登记: 组内成员 + task_hash */
    struct edc_task_group *g1 = edc_scheduler_create_group(sched, sess, t1); /* 让它成为 session 的成员组 */
    edc_task_group_add_task(g1, t1);

    TEST("a task's history comes back by id, with the input that created it");
    char *json = edc_scheduler_task_history_json(sched, sess, NULL, t1->task_id);
    CHECK(json != NULL, "got a payload");
    cJSON *root = cJSON_Parse(json);
    CHECK(root != NULL, "and it parses");
    if (root)
    {
        CHECK(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(root, "kind")), "task") == 0, "kind=task");
        CHECK(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(root, "input")), "please fix the parser") == 0,
              "the input rides along (the frontend draws it as the first user line)");
        cJSON *msgs = cJSON_GetObjectItem(root, "messages");
        CHECK(cJSON_GetArraySize(msgs) == 2, "both of the turn's messages are there");
        CHECK(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetArrayItem(msgs, 1), "content")), "on it") == 0,
              "in order, unmodified");
        cJSON_Delete(root);
    }
    free(json);

    TEST("a group's history is its members, one block each");
    char *gjson = edc_scheduler_task_history_json(sched, sess, g1->group_id, NULL);
    CHECK(gjson != NULL, "got a payload");
    root = cJSON_Parse(gjson);
    CHECK(root != NULL, "and it parses");
    if (root)
    {
        CHECK(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(root, "kind")), "group") == 0, "kind=group");
        cJSON *blocks = cJSON_GetObjectItem(root, "tasks");
        CHECK(blocks && cJSON_GetArraySize(blocks) == 1, "one block per member");
        CHECK(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetArrayItem(blocks, 0), "id")), t1->task_id) == 0,
              "the block names the member");
        cJSON_Delete(root);
    }
    free(gjson);

    TEST("an id that is not there yields nothing (frontend shows nothing, not a wrong task)");
    CHECK(edc_scheduler_task_history_json(sched, sess, NULL, "999999") == NULL, "no such task");
    CHECK(edc_scheduler_task_history_json(sched, sess, "999999", NULL) == NULL, "no such group");
    CHECK(edc_scheduler_task_history_json(sched, sess, NULL, NULL) == NULL, "no id at all");

    edc_scheduler_free(sched);
    free(sess);
    free_config(cfg);
}

static void test_task_raw_input(void)
{
    struct edc_config *cfg = make_config();
    struct edc_session *sess = make_session(cfg);
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, sess);

    TEST("no raw input when no input is being handled (fork / system task / direct)");
    struct edc_task *t0 = edc_scheduler_task_new(sched, sess, "no input", edc_channel_adapter);
    CHECK(t0->raw_input == NULL, "and attached no raw input");

    TEST("create_task keeps the input being handled as the task's raw input");
    sched->input_raw = "hello \"\"\"world\"\"\"";
    struct edc_task *t1 = edc_scheduler_task_new(sched, sess, "hi", edc_channel_adapter);
    CHECK(t1->raw_input && strcmp(t1->raw_input, "hello \"\"\"world\"\"\"") == 0,
          "the task keeps the user's own (unparsed) text");
    sched->input_raw = NULL;

    edc_scheduler_free(sched);
    free(sess);
    free_config(cfg);
}

/* ── 3. Group 管理 ────────────────────────────────────────── */

static void test_scheduler_group_mgmt(void)
{
    TEST("create_group adds to scheduler's list");
    struct edc_config *cfg = make_config();
    struct edc_session *sess = make_session(cfg);
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, sess);

    struct edc_task *leader = edc_scheduler_task_new(sched, sess, "group leader", edc_channel_adapter);
    struct edc_task_group *g = edc_scheduler_create_group(sched, sess, leader);

    CHECK(g != NULL, "group created");
    CHECK(g->leader == leader, "leader set");
    CHECK(leader->is_group_leader == 1, "leader marked");
    CHECK(strcmp(g->group_id, leader->task_id) == 0, "group_id = leader task_id");

    TEST("add_group appends");
    struct edc_task *l2 = edc_scheduler_task_new(sched, sess, "group 2 leader", edc_channel_adapter);
    struct edc_task_group *g2 = edc_scheduler_create_group(sched, sess, l2);
    edc_scheduler_add_group(sched, sess, g2);
    CHECK(sess->groups == g, "first group at head");
    CHECK(sess->groups->next == g2, "second at next");

    TEST("add_group NULL safety");
    edc_scheduler_add_group(sched, sess, NULL);
    CHECK(1, "add_group(NULL) survived");

    edc_scheduler_free(sched);
    free(sess);
    free_config(cfg);
}

/* ── 4. 前台/后台 ─────────────────────────────────────────── */

static void test_scheduler_fg_bg(void)
{
    TEST("set_foreground / get_foreground");
    struct edc_config *cfg = make_config();
    struct edc_session *sess = make_session(cfg);
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, sess);

    struct edc_task *l = edc_scheduler_task_new(sched, sess, "fg leader", edc_channel_adapter);
    struct edc_task_group *g = edc_scheduler_create_group(sched, sess, l);

    edc_scheduler_set_foreground(sess, g);
    CHECK(edc_scheduler_get_foreground(sess) == g, "foreground set correctly");

    edc_scheduler_set_foreground(sess, NULL);
    CHECK(edc_scheduler_get_foreground(sess) == NULL, "foreground cleared");

    edc_scheduler_free(sched);
    free(sess);
    free_config(cfg);
}

/* ── 4b. 任务落位（edc_scheduler_task_create） ────────────── */

/* 五种意图落到哪个组、谁拿前台，是这一处唯一的答案；这里逐个钉住。 */
static void test_task_placement(void)
{
    struct edc_config *cfg = make_config();
    clean_task_seq(cfg);
    struct edc_session *sess = make_session(cfg);
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, sess);

    struct edc_task_spec spec;
    memset(&spec, 0, sizeof(spec));

    TEST("PLACE_NEW_GROUP + take_foreground: leads a fresh group and takes the pointer");
    spec.desc = "first";
    spec.channel_type = edc_channel_adapter;
    spec.input = "first";
    spec.place = EDC_TASK_PLACE_NEW_GROUP;
    spec.take_foreground = true;
    struct edc_task *t1 = edc_scheduler_task_create(sched, sess, &spec);
    CHECK(t1 != NULL, "task created");
    struct edc_task_group *g1 = t1 ? t1->group : NULL;
    CHECK(g1 != NULL && g1->leader == t1, "leads a fresh group");
    CHECK(edc_scheduler_get_foreground(sess) == g1, "foreground moved onto it");
    CHECK(edc_session_get_active_task(sess) == t1, "active task is the new one");
    CHECK(t1 && t1->active_turn != NULL, "turn attached");
    CHECK(t1 && t1->raw_input == NULL, "no raw input outside a message");
    CHECK(t1 && edc_task_is_ready(t1), "left READY (the scan decides when it runs)");

    TEST("PLACE_FOREGROUND: joins the group already in front, behind the one holding it");
    memset(&spec, 0, sizeof(spec));
    spec.desc = "second";
    spec.input = "second";
    spec.place = EDC_TASK_PLACE_FOREGROUND;
    spec.take_foreground = true;
    struct edc_task *t2 = edc_scheduler_task_create(sched, sess, &spec);
    CHECK(t2 && t2->group == g1, "joined the existing foreground group");
    CHECK(g1 && g1->member_count == 2, "member count grew");
    CHECK(g1 && g1->leader == t1, "leader unchanged");
    /* 前台组一次只跑一个: t1 还攥着前台, 所以新任务排在它后面。 */
    CHECK(t2 && edc_task_awaits_deps(t2), "queued behind the member in front");
    CHECK(t2 && t2->deps_total == 1, "one SEQUENCE edge");
    CHECK(edc_session_get_active_task(sess) == t1, "the front stays on the one that holds it");

    TEST("PLACE_FOREGROUND with nothing in front makes a group");
    edc_scheduler_set_foreground(sess, NULL);
    memset(&spec, 0, sizeof(spec));
    spec.desc = "third";
    spec.place = EDC_TASK_PLACE_FOREGROUND;
    spec.take_foreground = true;
    struct edc_task *t3 = edc_scheduler_task_create(sched, sess, &spec);
    struct edc_task_group *g3 = t3 ? t3->group : NULL;
    CHECK(g3 != NULL && g3 != g1, "made its own group");
    CHECK(edc_scheduler_get_foreground(sess) == g3, "and took the foreground");

    TEST("PLACE_GROUP: joins the named group, never touches the foreground");
    memset(&spec, 0, sizeof(spec));
    spec.desc = "chained";
    spec.place = EDC_TASK_PLACE_GROUP;
    spec.group_id = g1 ? g1->group_id : NULL;
    struct edc_task *t4 = edc_scheduler_task_create(sched, sess, &spec);
    CHECK(t4 && t4->group == g1, "joined the named group");
    CHECK(edc_scheduler_get_foreground(sess) == g3, "foreground untouched");
    CHECK(edc_session_get_active_task(sess) == t3, "active untouched");

    TEST("PLACE_GROUP with an unknown id makes a group led by the task");
    memset(&spec, 0, sizeof(spec));
    spec.desc = "orphan";
    spec.place = EDC_TASK_PLACE_GROUP;
    spec.group_id = "999999";
    struct edc_task *t5 = edc_scheduler_task_create(sched, sess, &spec);
    CHECK(t5 && t5->group && t5->group->leader == t5, "led a group created on demand");
    CHECK(edc_scheduler_get_foreground(sess) == g3, "still no foreground change");

    TEST("PLACE_SYSTEM: system group + sink, foreground untouched, no turn");
    memset(&spec, 0, sizeof(spec));
    spec.desc = "fold";
    spec.place = EDC_TASK_PLACE_SYSTEM;
    spec.sink = EDC_SINK_GROUP_MEMORY;
    struct edc_task *t6 = edc_scheduler_task_create(sched, sess, &spec);
    CHECK(t6 && t6->sink == EDC_SINK_GROUP_MEMORY, "sink set");
    struct edc_task_group *sysg = edc_session_find_system_group(sess);
    CHECK(sysg != NULL && t6 && t6->group == sysg, "landed in the system group");
    CHECK(sysg && edc_task_group_is_system(sysg), "group kind is SYSTEM");
    CHECK(edc_scheduler_get_foreground(sess) == g3, "foreground untouched");
    CHECK(t6 && t6->active_turn == NULL, "no turn (the caller builds one)");

    TEST("edges: a JOIN dep parks the task, a prev SEQUENCE edge is an order only");
    memset(&spec, 0, sizeof(spec));
    spec.desc = "waiter";
    spec.place = EDC_TASK_PLACE_NEW_GROUP;
    spec.deps = &t1;
    spec.dep_count = 1;
    struct edc_task *t7 = edc_scheduler_task_create(sched, sess, &spec);
    CHECK(t7 && edc_task_awaits_deps(t7), "parked waiting for t1");
    CHECK(t7 && t7->deps_total == 1, "one dep recorded");

    memset(&spec, 0, sizeof(spec));
    spec.desc = "sequenced";
    spec.place = EDC_TASK_PLACE_NEW_GROUP;
    spec.prev = t1;
    spec.prev_type = TASK_DEP_SEQUENCE;
    struct edc_task *t8 = edc_scheduler_task_create(sched, sess, &spec);
    CHECK(t8 && edc_task_awaits_deps(t8), "sequenced behind t1");
    CHECK(t8 && t8->deps_total == 1, "one edge recorded");

    TEST("input: the turn is attached (running it is the scan's job)");
    memset(&spec, 0, sizeof(spec));
    spec.desc = "later";
    spec.input = "later";
    spec.place = EDC_TASK_PLACE_NEW_GROUP;
    struct edc_task *t9 = edc_scheduler_task_create(sched, sess, &spec);
    CHECK(t9 && t9->active_turn != NULL, "turn attached");
    CHECK(t9 && edc_task_is_ready(t9), "left READY (nothing parks a new task any more)");

    TEST("missing session / missing spec is refused");
    CHECK(edc_scheduler_task_create(sched, NULL, &spec) == NULL, "no session -> NULL");
    CHECK(edc_scheduler_task_create(sched, sess, NULL) == NULL, "no spec -> NULL");
    CHECK(edc_scheduler_task_create(NULL, sess, &spec) == NULL, "no scheduler -> NULL");

    edc_scheduler_free(sched);
    free(sess);
    free_config(cfg);
}

/* ── 4c. 无 id 命令的默认对象（可推导，不是 active） ─────────── */

static void test_current_task(void)
{
    struct edc_config *cfg = make_config();
    clean_task_seq(cfg);
    struct edc_session *sess = make_session(cfg);
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, sess);

    TEST("no foreground group -> no current task, nothing resumable");
    CHECK(edc_scheduler_current_task(sess) == NULL, "NULL without a foreground group");
    CHECK(edc_scheduler_resumable_task(sess) == NULL, "and for resume too");

    /* 前台组三个成员: 已完成 / 已取消 / 还活着; 焦点故意放在已完成的那个上。 */
    struct edc_task *done = edc_scheduler_task_new(sched, sess, "done", edc_channel_adapter);
    struct edc_task_group *g = edc_scheduler_create_group(sched, sess, done);
    edc_scheduler_set_foreground(sess, g);
    edc_task_await_exec(done);
    edc_task_mark_completed(done);

    struct edc_task *cancelled = edc_scheduler_task_new(sched, sess, "cancelled", edc_channel_adapter);
    edc_task_group_add_task(g, cancelled);
    edc_task_mark_cancelled(cancelled);

    struct edc_task *alive = edc_scheduler_task_new(sched, sess, "alive", edc_channel_adapter);
    edc_task_group_add_task(g, alive);

    edc_session_set_active_task(sess, done); /* the focus sits on a finished task */

    TEST("current task: the last unfinished member, not the focus");
    CHECK(edc_scheduler_current_task(sess) == alive, "the finished ones are skipped");
    CHECK(edc_scheduler_resumable_task(sess) == alive, "alive is the resumable pick too");

    TEST("a CANCELLED member stays resumable while an unfinished one exists");
    edc_task_await_exec(alive);
    edc_task_mark_completed(alive);
    CHECK(edc_scheduler_current_task(sess) == NULL, "nothing unfinished left to act on");
    CHECK(edc_scheduler_resumable_task(sess) == cancelled,
          "only COMPLETED is final, so the cancelled one is still a resume candidate");

    TEST("all members COMPLETED -> neither derivation yields anything");
    struct edc_task *d2 = edc_scheduler_task_new(sched, sess, "done 2", edc_channel_adapter);
    struct edc_task_group *g2 = edc_scheduler_create_group(sched, sess, d2);
    edc_scheduler_set_foreground(sess, g2);
    edc_task_await_exec(d2);
    edc_task_mark_completed(d2);
    CHECK(edc_scheduler_current_task(sess) == NULL, "current: NULL");
    CHECK(edc_scheduler_resumable_task(sess) == NULL, "resumable: NULL");

    edc_scheduler_free(sched);
    free(sess);
    free_config(cfg);
}

/* ── 4d. resume 也要排队（它是"让一个已有任务跑起来"的另一道门） ─── */

static void test_resume_queues(void)
{
    struct edc_config *cfg = make_config();
    clean_task_seq(cfg);
    struct edc_session *sess = make_session(cfg);
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, sess);

    /* 前台组里 A 攥着前台（有 turn），B 是同组一个挂起的任务。 */
    struct edc_task *a = edc_scheduler_task_new(sched, sess, "a", edc_channel_adapter);
    struct edc_task_group *g = edc_scheduler_create_group(sched, sess, a);
    edc_scheduler_set_foreground(sess, g);
    edc_scheduler_attach_input(sched, a, "a's input");

    struct edc_task *b = edc_scheduler_task_new(sched, sess, "b", edc_channel_adapter);
    edc_task_group_add_task(g, b);
    edc_scheduler_attach_input(sched, b, "b's input");
    edc_task_mark_suspended(b);
    CHECK(edc_task_is_suspended(b), "B starts parked");

    TEST("resume does not jump the queue of the group in front");
    edc_scheduler_resume_task(sched, b);
    CHECK(edc_task_awaits_deps(b), "B is parked behind A");
    CHECK(b->deps_total == 1 && b->deps && b->deps->source == a, "one SEQUENCE edge, pointing at A");
    CHECK(!edc_task_is_suspended(b), "it is no longer suspended -- it is queued now");

    TEST("a second resume changes nothing while it still waits");
    edc_scheduler_resume_task(sched, b);
    CHECK(edc_task_awaits_deps(b), "still parked (the dependency gate catches it)");
    CHECK(b->deps_total == 1, "no duplicate edge");

    TEST("and it becomes runnable once the task in front finishes");
    edc_task_await_exec(a);
    edc_task_mark_completed(a);
    edc_task_notify_dependents(a);
    CHECK(edc_task_is_ready(b), "B was released");

    edc_scheduler_free(sched);
    free(sess);
    free_config(cfg);
}

/* ── 5. 信号发送 ──────────────────────────────────────────── */

/* Persistence is no longer an injectable port, so "cancel persists immediately" is
 * covered by the on-disk round trip in test_session_leave_cancel.c. */

static void test_scheduler_signals(void)
{
    TEST("send_signal_to_task");
    struct edc_config *cfg = make_config();
    struct edc_session *sess = make_session(cfg);
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, sess);

    struct edc_task *t = edc_scheduler_task_new(sched, sess, "signal target", edc_channel_adapter);
    struct edc_task_group *g = edc_scheduler_create_group(sched, sess, t);

    CHECK(edc_task_is_ready(t), "task at READY");

    edc_scheduler_send_signal_to_task(sched, sess, t->task_id, TASK_SIGTERM);
    CHECK(edc_task_is_cancelled(t), "task cancelled by SIGTERM");

    TEST("send_signal_to_task nonexistent");
    edc_scheduler_send_signal_to_task(sched, sess, "nonexistent", TASK_SIGTERM);
    CHECK(1, "signal to nonexistent task survived");

    TEST("send_signal_to_task triggers persist on cancel");

    /* WAITING_INPUT task: cancel → must persist */
    struct edc_task *t2 = edc_scheduler_task_new(sched, sess, "signal target 2", edc_channel_adapter);
    edc_task_group_add_task(g, t2);
    edc_scheduler_send_signal_to_task(sched, sess, t2->task_id, TASK_SIGTERM);
    CHECK(edc_task_is_cancelled(t2), "t2 cancelled");

    /* COMPLETED task is terminal: cancel is a no-op */
    struct edc_task *t3 = edc_scheduler_task_new(sched, sess, "done task", edc_channel_adapter);
    edc_task_group_add_task(g, t3);
    edc_task_await_exec(t3);
    edc_task_mark_completed(t3);
    edc_scheduler_send_signal_to_task(sched, sess, t3->task_id, TASK_SIGTERM);
    CHECK(edc_task_is_completed(t3), "completed task stays COMPLETED");

    /* nonexistent task → survived */
    edc_scheduler_send_signal_to_task(sched, sess, "nonexistent", TASK_SIGTERM);
    CHECK(1, "signal to a nonexistent task survived");

    edc_scheduler_free(sched);
    free(sess);
    free_config(cfg);
}

static void test_scheduler_group_signals(void)
{
    TEST("send_signal_to_group cancels all members");
    struct edc_config *cfg = make_config();
    struct edc_session *sess = make_session(cfg);
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, sess);

    struct edc_task *l = edc_scheduler_task_new(sched, sess, "group L", edc_channel_adapter);
    struct edc_task_group *g = edc_scheduler_create_group(sched, sess, l);
    struct edc_task *m1 = edc_scheduler_task_new(sched, sess, "member 1", edc_channel_adapter);
    struct edc_task *m2 = edc_scheduler_task_new(sched, sess, "member 2", edc_channel_adapter);
    edc_task_group_add_task(g, m1);
    edc_task_group_add_task(g, m2);

    /* 清零脏表: 下面的判据是"组级取消**自己**把 session 标脏了", 不算前面建任务
     * 留下的。测试独占这个 scheduler, 直接清。 */
    sched->dirty_count = 0;

    edc_scheduler_send_signal_to_group(sched, sess, g->group_id, TASK_SIGTERM);
    CHECK(edc_task_is_cancelled(l), "leader cancelled");
    CHECK(edc_task_is_cancelled(m1), "member 1 cancelled");
    CHECK(edc_task_is_cancelled(m2), "member 2 cancelled");
    /* 判据: 每个成员都走过了统一入口的簿记 (② 置脏)。这条路径从前是**手工补丁**,
     * 它不置脏 → 侧栏不推快照、也不落盘 (靠调用方各自记得 save 兜着)。
     * 变异自检: 去掉 sched_signal_one 里的 apply_advance 调用 → 这条必红。 */
    CHECK(sched->dirty_count > 0, "group cancel marks the session dirty (unified entry)");
    CHECK(sched->dirty_count > 0 && sched->dirty[0].session == sess, "dirty entry is the owning session");

    TEST("send_signal_to_group nonexistent");
    edc_scheduler_send_signal_to_group(sched, sess, "nonexistent-gid", TASK_SIGTERM);
    CHECK(1, "signal to nonexistent group survived");

    edc_scheduler_free(sched);
    free(sess);
    free_config(cfg);
}

/* Cancel propagates: a JOIN dependent is cancelled too, and its bookkeeping runs
 * through the same entry (the hash entry is gone).
 * Mutation: dropping task_record_move in edc_task_on_dep_cancelled must fail this. */
static void test_signal_cascade_settles_dependents(void)
{
    TEST("JOIN 级联取消: 被带动的依赖者也过统一入口");
    struct edc_config *cfg = make_config();
    struct edc_session *sess = make_session(cfg);
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, sess);

    struct edc_task *src = edc_scheduler_task_new(sched, sess, "src", edc_channel_adapter);
    struct edc_task *dst = edc_scheduler_task_new(sched, sess, "dst", edc_channel_adapter);
    edc_task_add_dependency(dst, src, TASK_DEP_JOIN);
    CHECK(edc_task_awaits_deps(dst), "dst waits for src");

    void *src_rid = (void *)(intptr_t)0xC001;
    void *dst_rid = (void *)(intptr_t)0xC002;
    edc_scheduler_register_task(sched, src, src_rid);
    edc_scheduler_register_task(sched, dst, dst_rid);
    src->current_request_id = src_rid;
    dst->current_request_id = dst_rid;

    edc_scheduler_send_signal_to_task_ptr(sched, src, TASK_SIGTERM);
    CHECK(edc_task_is_cancelled(src), "src cancelled");
    CHECK(edc_task_is_cancelled(dst), "dst cancelled by cascade");
    CHECK(edc_scheduler_find_task(sched, src_rid) == NULL, "src 的请求不再欠着");
    CHECK(edc_scheduler_find_task(sched, dst_rid) == NULL, "级联取消的依赖者也不再欠着");

    edc_scheduler_free(sched);
    free(sess);
    free_config(cfg);
}

static void test_scheduler_signal_all(void)
{
    TEST("send_signal_to_all cancels everything");
    struct edc_config *cfg = make_config();
    struct edc_session *sess = make_session(cfg);
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, sess);

    struct edc_task *l1 = edc_scheduler_task_new(sched, sess, "G1L", edc_channel_adapter);
    struct edc_task_group *g1 = edc_scheduler_create_group(sched, sess, l1);
    edc_task_group_add_task(g1, edc_scheduler_task_new(sched, sess, "G1M", edc_channel_adapter));

    struct edc_task *l2 = edc_scheduler_task_new(sched, sess, "G2L", edc_channel_adapter);
    struct edc_task_group *g2 = edc_scheduler_create_group(sched, sess, l2);

    CHECK(g1->member_count == 2, "group 1 has 2 members");
    CHECK(g2->member_count == 1, "group 2 has 1 member");

    edc_scheduler_send_signal_to_all(sched, sess, TASK_SIGTERM);
    CHECK(edc_task_is_cancelled(l1), "G1 leader cancelled");
    CHECK(edc_task_is_cancelled(l2), "G2 leader cancelled");

    edc_scheduler_free(sched);
    free(sess);
    free_config(cfg);
}

/* ── 6. 输入路由: 普通输入一律新建 task (attach 已退休) ── */

static int g_cmd_calls = 0;
static int g_llm_calls = 0;
static struct edc_task *g_llm_task = NULL;

static bool mock_handle_command(struct edc_scheduler *sched, struct edc_actor *actor,
                                const struct edc_sched_input *input)
{
    (void)sched;
    (void)actor;
    (void)input;
    g_cmd_calls++;
    return true;
}

static void mock_send_llm(struct edc_scheduler *sched, struct edc_task *task, void *request_id)
{
    (void)sched;
    (void)request_id;
    g_llm_calls++;
    g_llm_task = task;
}

/* 发起请求的那个 request_id 现在由**调度**发号 (唯一驱动入口), 测试不能再自己写死
 * 一个数字: 后续投递的响应必须带**登记过的那个** id, 否则 task_hash 路由不到任务。
 * 任务自己记着它 (current_request_id) —— 这也是那条不变量的可读证据。 */
static long req_id_of(struct edc_task *t)
{
    /* NULL 安全: 断言失败时 (没有任务被派发) 后续投递要变成一个良性的"找不到任务"，
     * 而不是解引用 NULL。 */
    return t ? (long)(intptr_t)t->current_request_id : 0;
}

/* Failure propagates the same way: an ERROR source cascades to its JOIN dependents.
 * Mutation checks: remove the broadcast, remove the JOIN task_record_move, or
 * restore the edge unlink -- each must fail this. */
static void test_failure_cascade_settles_dependents(void)
{
    TEST("上游失败: 连带失败者也过统一入口; 边留着 ⇒ resume 它回到等依赖");
    struct edc_config *cfg = make_config();
    struct edc_session *sess = make_session(cfg);
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, sess);
    g_llm_calls = 0;
    g_llm_task = NULL;
    sched->send_llm = mock_send_llm;

    struct edc_task *src = edc_scheduler_task_new(sched, sess, "src", edc_channel_adapter);
    /* 脏表按 **session** 去重, 而 task 认 session 要经它所属的 group —— 测试里得真绑上,
     * 否则 `edc_task_session()` 是 NULL, 置脏整条路径静默不动作。 */
    struct edc_task_group *group = edc_scheduler_create_group(sched, sess, src);
    struct edc_task *dst = edc_scheduler_task_new(sched, sess, "dst", edc_channel_adapter);
    edc_task_group_add_task(group, dst);
    edc_task_add_dependency(dst, src, TASK_DEP_JOIN);
    /* 依赖者**带着 input 停在依赖上** (生产: `/task new """x""" -d <id>` 就是这样) */
    struct edc_turn *turn = edc_turn_new();
    turn->task_id = strdup(dst->task_id);
    edc_turn_add_user_message(turn, "queued input");
    dst->active_turn = turn;

    void *src_rid = (void *)(intptr_t)0xF001;
    void *dst_rid = (void *)(intptr_t)0xF002;
    edc_scheduler_register_task(sched, src, src_rid);
    edc_scheduler_register_task(sched, dst, dst_rid);
    src->current_request_id = src_rid;
    dst->current_request_id = dst_rid;

    /* 上游失败 —— 走 executor 的那个出口 (mark_failed) 加宿主的结算 */
    CHECK(edc_task_await_exec(src), "src 在跑");
    CHECK(edc_task_mark_failed(src), "src 失败");
    struct edc_advance_result res;
    memset(&res, 0, sizeof(res));
    res.outcome = EDC_STEP_ERROR;
    res.changed = true;
    res.settled = true;
    res.notice = "LLM request failed";
    sched->dirty_count = 0;
    edc_scheduler_apply_advance(sched, src, src_rid, &res);

    CHECK(edc_task_has_failed(src), "src 进 ERROR");
    CHECK(edc_task_has_failed(dst), "dst 连带失败 (否则它永远停在等依赖)");
    CHECK(edc_scheduler_find_task(sched, src_rid) == NULL, "src 的请求不再欠着");
    CHECK(edc_scheduler_find_task(sched, dst_rid) == NULL, "连带失败者也不再欠着请求");
    CHECK(sched->dirty_count > 0, "连带失败也置脏 (推 TUI)");
    CHECK(dst->deps != NULL && dst->deps_total == 1, "那条边**留着** (源还能 resume, 边只是暂时满足不了)");

    /* resume 依赖者: 它被放回"等依赖" —— 而不是当作没依赖直接跑。 */
    int calls_before = g_llm_calls;
    edc_scheduler_resume_task(sched, dst);
    CHECK(edc_task_awaits_deps(dst), "resume 把它放回等依赖");
    CHECK(g_llm_calls == calls_before, "没有为它发请求 (它得先等到源的产出)");

    /* 出路 = 让源真的产出 (源产出后那条边完成, 依赖者会被自动放行 —— 那条性质在
     * tests/test_edc_task.c 的 "dep_completion_releases" 里单测; 这里只看闸门行为)。 */

    edc_scheduler_free(sched);
    free(sess);
    free_config(cfg);
}

/* P3: 入站消息带一个 **session 标签**, scheduler 经 store 读口把它解析成 session
 * (它不再持有"当前 session"的副本)。单测里只有一个 session: 标签为空或与它一致
 * 就给它; 标签明确指向别人 → NULL = 调用方拒绝这条输入 (不猜)。 */

/* 驱动一条入站输入 (模拟 TUI): TUI 永远**显式**带 session 标签 (P3 ①a)。 */

/* 直接驱动 scheduler actor handler 投递一条 CHANNEL_RECV 消息 (模拟 TUI 输入)。 */
static void drive_input(struct edc_scheduler *sched, const char *content, long req_id)
{
    edc_actor_t *sched_actor = edc_scheduler_get_actor(sched);
    struct edc_channel_message *cm =
        edc_channel_message_new(edc_channel_adapter, EDC_CHANNEL_MESSAGE_USER_TYPE, sdsnew(content));
    /* 标签 = 该 scheduler 的读口指向的那棵树 (ctx 就是它)。 */
    struct edc_session *owner = sched->ctx;
    if (owner && owner->session_id)
        cm->session_id = sdsnew(owner->session_id);
    edc_actor_msg_t *msg =
        edc_actor_msg_new(BC_ACTOR_MSG_CHANNEL_RECV, NULL, (void *)(intptr_t)req_id, edc_channel_input_new(cm),
                          (edc_actor_data_free_fn)edc_channel_input_free);
    sched_actor->handler(sched_actor, msg); /* dispatches + frees msg */
}

/* Scheduling: the message-end full-table scan. "Turned READY" is a direct result of
 * the scan, not a special case of any path. */

/* 投递一条**只读**消息 (拉快照), 让 actor handler 走到"消息末尾的调度"那一步 ——
 * 生产里每条消息都走到那里; 测试用它证明**接线** (而不只是那个函数本身)。 */
static void pump_message(struct edc_scheduler *sched)
{
    edc_actor_t *actor = edc_scheduler_get_actor(sched);
    struct edc_session *owner = sched->ctx;
    edc_actor_msg_t *msg = edc_actor_msg_new(BC_ACTOR_MSG_TASK_SNAPSHOT, NULL, NULL,
                                             owner && owner->session_id ? sdsnew(owner->session_id) : NULL,
                                             (edc_actor_data_free_fn)sdsfree);
    actor->handler(actor, msg);
}

/* 一个带着 input 停着 (READY/ALIVE) 的任务 —— 扫描谓词的原料。 */
static struct edc_task *add_ready_task_with_turn(struct edc_scheduler *sched, struct edc_session *sess,
                                                 struct edc_task_group *group, const char *desc)
{
    struct edc_task *t = edc_scheduler_task_new(sched, sess, desc, edc_channel_adapter);
    if (group)
        edc_task_group_add_task(group, t);
    else
        edc_scheduler_create_group(sched, sess, t);
    struct edc_turn *turn = edc_turn_new();
    turn->task_id = strdup(t->task_id);
    edc_turn_add_user_message(turn, desc);
    t->active_turn = turn;
    return t;
}

/* (1) A `;` dependent: cancelling the source releases it to READY and the scan runs
 * it. Mutation: removing the schedule call from the actor handler must fail this. */
static void test_schedule_drives_released_sequence_dep(void)
{
    TEST("调度: ; 下游被取消释放 → 消息末尾的扫描启动它 (id 可路由)");
    struct edc_config *cfg = make_config();
    struct edc_session *sess = make_session(cfg);
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, sess);
    g_llm_calls = 0;
    g_llm_task = NULL;
    sched->send_llm = mock_send_llm;

    struct edc_task *src = edc_scheduler_task_new(sched, sess, "src", edc_channel_adapter);
    struct edc_task_group *group = edc_scheduler_create_group(sched, sess, src);
    struct edc_task *dst = add_ready_task_with_turn(sched, sess, group, "released by cancel");
    edc_task_add_dependency(dst, src, TASK_DEP_SEQUENCE);
    CHECK(edc_task_awaits_deps(dst), "dst 先被依赖挡住");
    void *src_rid = (void *)(intptr_t)0xE001;
    edc_scheduler_register_task(sched, src, src_rid);
    src->current_request_id = src_rid;

    edc_scheduler_send_signal_to_task_ptr(sched, src, TASK_SIGTERM);
    CHECK(edc_task_is_cancelled(src), "src 取消");
    CHECK(edc_task_is_cancelled(dst), "dst 跟着级联取消 (; 与 JOIN 一致)");
    CHECK(dst->deps_total == 1, "边留着: resume dst 会回到等依赖, 不再越过 src");
    CHECK(g_llm_calls == 0, "信号路径只改状态, 不驱动");

    pump_message(sched); /* ← 消息末尾 */
    CHECK(g_llm_calls == 0, "扫描也不会启动它 —— 它不再是自己能跑的任务");

    edc_scheduler_free(sched);
    free(sess);
    free_config(cfg);
}

/* (2) Cancelling a whole group sends no request: the scan judges state, not cause.
 * Moving it back into apply_advance would send one halfway through the cancel. */
static void test_schedule_after_session_stop_sends_nothing(void)
{
    TEST("调度: /session stop 之后一个请求都不发 (判据在状态上)");
    struct edc_config *cfg = make_config();
    struct edc_session *sess = make_session(cfg);
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, sess);
    g_llm_calls = 0;
    g_llm_task = NULL;
    sched->send_llm = mock_send_llm;

    struct edc_task *src = edc_scheduler_task_new(sched, sess, "src", edc_channel_adapter);
    struct edc_task_group *group = edc_scheduler_create_group(sched, sess, src);
    struct edc_task *dst = add_ready_task_with_turn(sched, sess, group, "queued after src");
    edc_task_add_dependency(dst, src, TASK_DEP_SEQUENCE);

    edc_scheduler_send_signal_to_all(sched, sess, TASK_SIGTERM); /* 会话级扇出 */
    CHECK(edc_task_is_cancelled(src) && edc_task_is_cancelled(dst), "两个都取消了");
    CHECK(!edc_task_is_ready(dst), "dst 不再是「可跑」状态");

    pump_message(sched);
    CHECK(g_llm_calls == 0, "扫描没替被取消的下游发请求");

    edc_scheduler_free(sched);
    free(sess);
    free_config(cfg);
}

/* ③ 失败释放的 `;` 下游同样被扫描启动 (三条终结路收敛到同一个驱动点)。 */
static void test_schedule_drives_dep_released_by_failure(void)
{
    TEST("调度: 上游失败 → ; 下游被扫描启动");
    struct edc_config *cfg = make_config();
    struct edc_session *sess = make_session(cfg);
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, sess);
    g_llm_calls = 0;
    g_llm_task = NULL;
    sched->send_llm = mock_send_llm;

    struct edc_task *src = edc_scheduler_task_new(sched, sess, "src", edc_channel_adapter);
    struct edc_task_group *group = edc_scheduler_create_group(sched, sess, src);
    /* A BARRIER is the kind that releases on the source's death (that is what the fold
     * hangs); a user dependency (JOIN / SEQUENCE) takes its dependent with it instead --
     * the next case pins that difference. */
    struct edc_task *dst = add_ready_task_with_turn(sched, sess, group, "released by failure");
    edc_task_add_dependency(dst, src, TASK_DEP_BARRIER);

    CHECK(edc_task_await_exec(src), "src 在跑");
    CHECK(edc_task_mark_failed(src), "src 失败");
    struct edc_advance_result res;
    memset(&res, 0, sizeof(res));
    res.outcome = EDC_STEP_ERROR;
    res.changed = true;
    res.settled = true;
    res.notice = "boom";
    edc_scheduler_apply_advance(sched, src, NULL, &res);
    CHECK(edc_task_is_ready(dst), "dst 被放行 (barrier: 源死了就作废边)");

    edc_scheduler_schedule(sched);
    CHECK(g_llm_calls == 1 && g_llm_task == dst, "扫描启动它");
    CHECK(edc_task_awaits_exec(dst), "相位 AWAIT_EXEC");

    edc_scheduler_free(sched);
    free(sess);
    free_config(cfg);
}

/* (4) A dependent that was never registered (production: /task new """x""" -d <id>).
 * Reusing current_request_id would send NULL and the response could never route
 * back. Mutation: restoring that reuse must fail this. */
static void test_schedule_drives_queued_dep_without_registered_id(void)
{
    TEST("调度: 排队依赖被驱动时用的是**自己发号**的可路由 id");
    struct edc_config *cfg = make_config();
    struct edc_session *sess = make_session(cfg);
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, sess);
    g_llm_calls = 0;
    g_llm_task = NULL;
    sched->send_llm = mock_send_llm;

    struct edc_task *src = edc_scheduler_task_new(sched, sess, "src", edc_channel_adapter);
    struct edc_task_group *group = edc_scheduler_create_group(sched, sess, src);
    struct edc_task *dst = add_ready_task_with_turn(sched, sess, group, "queued dependent");
    edc_task_add_dependency(dst, src, TASK_DEP_JOIN);
    CHECK(dst->current_request_id == NULL, "**没有**登记过任何请求 (与 /task new -d 同形)");

    CHECK(edc_task_await_exec(src), "src 在跑");
    CHECK(edc_task_mark_completed(src), "src 完成");
    struct edc_advance_result res;
    memset(&res, 0, sizeof(res));
    res.outcome = EDC_STEP_DONE;
    res.changed = true;
    res.settled = true;
    edc_scheduler_apply_advance(sched, src, NULL, &res);
    CHECK(edc_task_is_ready(dst), "依赖齐了 → READY");

    edc_scheduler_schedule(sched);
    CHECK(g_llm_calls == 1 && g_llm_task == dst, "扫描启动它");
    void *rid = (void *)(intptr_t)req_id_of(dst);
    CHECK(rid != NULL, "驱动时给它发了号");
    CHECK(edc_scheduler_find_task(sched, rid) == dst, "而且登记得回来 —— 回包不会再找不到任务");

    edc_scheduler_free(sched);
    free(sess);
    free_config(cfg);
}

/* ⑤ 幂等: 同一个任务再扫一次不会再发一份请求 (驱动把它推出了 READY)。 */
static void test_schedule_is_idempotent(void)
{
    TEST("调度: 重复扫同一个任务只发一次请求");
    struct edc_config *cfg = make_config();
    struct edc_session *sess = make_session(cfg);
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, sess);
    g_llm_calls = 0;
    g_llm_task = NULL;
    sched->send_llm = mock_send_llm;

    struct edc_task *t = add_ready_task_with_turn(sched, sess, NULL, "run me once");
    edc_scheduler_schedule(sched);
    edc_scheduler_schedule(sched);
    pump_message(sched);
    CHECK(g_llm_calls == 1, "只有一次 (第二次扫描时它已在 AWAIT_EXEC)");
    CHECK(edc_task_awaits_exec(t), "相位没被重复推进");

    edc_scheduler_free(sched);
    free(sess);
    free_config(cfg);
}

/* ⑥ 推不动 (宿主没装 LLM 端口) = 失败 + 说得出的原因, 不是静默停在 READY。
 * ERROR 在 /task resume 接受的三类里 —— 端口补上后一条 resume 就能续。 */
static void test_schedule_without_llm_port_fails_loudly(void)
{
    TEST("调度: 没装 LLM 端口 → 任务进 ERROR 且原因写进 output");
    struct edc_config *cfg = make_config();
    struct edc_session *sess = make_session(cfg);
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, sess);
    /* 故意**不装** send_llm */
    struct edc_task *t = add_ready_task_with_turn(sched, sess, NULL, "cannot run here");

    edc_scheduler_schedule(sched);
    CHECK(edc_task_has_failed(t), "推不动就是失败 (不是「看起来在跑」的僵尸)");
    CHECK(t->output && strstr(t->output, "no LLM port") != NULL, "原因写进 output (结算拿它当通知文案)");
    CHECK(t->current_request_id == NULL, "没留下没人认领的 request_id");

    /* 出路: 装上端口后 resume 能续。 */
    g_llm_calls = 0;
    g_llm_task = NULL;
    sched->send_llm = mock_send_llm;
    edc_scheduler_resume_task(sched, t);
    CHECK(edc_task_awaits_exec(t), "resume 把它推起来了");
    CHECK(g_llm_calls == 1, "并且真发了请求");

    edc_scheduler_free(sched);
    free(sess);
    free_config(cfg);
}

/* ⑦ 确定性: 同一批可跑任务按"session 链序 × 组链序 × 成员链序"依次驱动。 */
static struct edc_task *g_order[4];
static int g_order_n;
static void order_send_llm(struct edc_scheduler *sched, struct edc_task *task, void *request_id)
{
    (void)sched;
    (void)request_id;
    if (g_order_n < 4)
        g_order[g_order_n++] = task;
}

static void test_schedule_order_is_deterministic(void)
{
    TEST("调度: 多个可跑任务按成员顺序依次驱动");
    struct edc_config *cfg = make_config();
    struct edc_session *sess = make_session(cfg);
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, sess);
    g_order_n = 0;
    memset(g_order, 0, sizeof(g_order));
    sched->send_llm = order_send_llm;

    struct edc_task *a = add_ready_task_with_turn(sched, sess, NULL, "first");
    struct edc_task *b = edc_scheduler_task_new(sched, sess, "second", edc_channel_adapter);
    edc_task_group_add_task(a->group, b);
    struct edc_turn *turn = edc_turn_new();
    turn->task_id = strdup(b->task_id);
    edc_turn_add_user_message(turn, "second");
    b->active_turn = turn;

    edc_scheduler_schedule(sched);
    CHECK(g_order_n == 2, "两个都驱动了 (今天没有并发上限)");
    CHECK(g_order_n == 2 && g_order[0] == a && g_order[1] == b, "顺序 = 成员链序 (确定性)");

    edc_scheduler_free(sched);
    free(sess);
    free_config(cfg);
}

/* (8) Scan to a fixed point: a failed drive broadcasts, releasing a SEQUENCE
 * dependent possibly earlier in the scan order. Mutation: a single-round scan must
 * fail this. */
static void test_schedule_to_fixpoint_covers_failure_cascade(void)
{
    TEST("调度: 扫描途中被失败释放的下游 (排在前面) 同一轮也被驱动");
    struct edc_config *cfg = make_config();
    struct edc_session *sess = make_session(cfg);
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, sess);
    g_llm_calls = 0;
    g_llm_task = NULL;
    sched->send_llm = mock_send_llm; /* 但**不装** redispatch_tools */

    /* 成员顺序: dst 在前, src 在后 —— 这样扫描到 src (它失败、释放 dst) 时,
     * dst **已经**被看过一遍了。 */
    struct edc_task *dst = add_ready_task_with_turn(sched, sess, NULL, "released mid-sweep");
    struct edc_task_group *group = dst->group;
    struct edc_task *src = add_ready_task_with_turn(sched, sess, group, "fails at START");
    edc_task_add_dependency(dst, src, TASK_DEP_BARRIER);
    /* src 的账本里有一条没派出去的工具 + 宿主没装工具箱端口 → START 在它身上失败 */
    edc_task_tool_call_add(src, "call_z", "shell", NULL);
    CHECK(group->members == dst && dst->group_next == src, "顺序钉住: dst 先被扫描, src 后");

    edc_scheduler_schedule(sched);

    CHECK(edc_task_has_failed(src), "src 在 START 上失败 (推不动的工具链)");
    CHECK(edc_task_is_ready(dst) == false, "dst 没有留在 READY 等下一条消息");
    CHECK(g_llm_calls == 1 && g_llm_task == dst, "同一轮里 dst 也被驱动了 (扫到不动点)");
    CHECK(edc_task_awaits_exec(dst), "dst 相位 AWAIT_EXEC");

    edc_scheduler_free(sched);
    free(sess);
    free_config(cfg);
}

static void test_input_routing(void)
{
    TEST("a parked task no longer captures plain input (attach is retired)");
    struct edc_config *cfg = make_config();
    struct edc_session *sess = make_session(cfg);
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, sess);

    /* 旧模型里, 一个停在前台 group 的 active task 会把普通输入"吃"进去 (attach);
     * 新模型下它没有任何特殊地位 —— 输入交给命令处理器 (shell), 由它建一条新任务。 */
    struct edc_task *parked = edc_scheduler_task_new(sched, sess, "parked", edc_channel_adapter);
    struct edc_task_group *g = edc_scheduler_create_group(sched, sess, parked);
    edc_scheduler_set_foreground(sess, g);
    edc_session_set_active_task(sess, parked);
    CHECK(edc_task_is_ready(parked), "parked task starts READY");

    g_cmd_calls = 0;
    g_llm_calls = 0;
    g_llm_task = NULL;
    sched->handle_command = mock_handle_command;
    sched->send_llm = mock_send_llm;

    drive_input(sched, "hello", 1);

    CHECK(g_cmd_calls == 1, "plain input reached the shell (it builds the task)");
    CHECK(g_llm_calls == 0, "nothing was attached to the parked task");
    CHECK(edc_task_is_ready(parked), "parked task untouched");
    CHECK(parked->active_turn == NULL, "parked task got no turn");

    edc_scheduler_free(sched);
    free(sess);
    free_config(cfg);

    TEST("plain input spawns via shell when no parked task");
    cfg = make_config();
    sess = make_session(cfg);
    sched = edc_scheduler_new(cfg);
    install_resolver(sched, sess);

    /* 已完成的前台 task → 本来就是新建 task, 现在不再有"停着就吃掉输入"那条路 */
    struct edc_task *done = edc_scheduler_task_new(sched, sess, "done", edc_channel_adapter);
    struct edc_task_group *g2 = edc_scheduler_create_group(sched, sess, done);
    edc_scheduler_set_foreground(sess, g2);
    edc_session_set_active_task(sess, done);
    edc_task_await_exec(done);
    edc_task_mark_completed(done);

    g_cmd_calls = 0;
    g_llm_calls = 0;
    g_llm_task = NULL;
    sched->handle_command = mock_handle_command;
    sched->send_llm = mock_send_llm;

    drive_input(sched, "hello", 2);

    CHECK(g_cmd_calls == 1, "shell invoked when no waiting task");
    CHECK(g_llm_calls == 0, "send_llm NOT called (shell handles spawn)");

    edc_scheduler_free(sched);
    free(sess);
    free_config(cfg);

    TEST("slash command reaches the shell");
    cfg = make_config();
    sess = make_session(cfg);
    sched = edc_scheduler_new(cfg);
    install_resolver(sched, sess);

    struct edc_task *leader2 = edc_scheduler_task_new(sched, sess, "waiting", edc_channel_adapter);
    struct edc_task_group *g3 = edc_scheduler_create_group(sched, sess, leader2);
    edc_scheduler_set_foreground(sess, g3);
    edc_session_set_active_task(sess, leader2);

    g_cmd_calls = 0;
    g_llm_calls = 0;
    g_llm_task = NULL;
    sched->handle_command = mock_handle_command;
    sched->send_llm = mock_send_llm;

    drive_input(sched, "/task list", 3);

    CHECK(g_cmd_calls == 1, "slash command reached shell");
    CHECK(g_llm_calls == 0, "slash command did not attach/dispatch anything");

    edc_scheduler_free(sched);
    free(sess);
    free_config(cfg);
}

/* ── 7. max_iter: LLM 工具循环迭代预算 ────────────────────── */

static int g_dispatch_calls = 0;
static int g_reply_calls = 0;
static char g_reply_content[512];

static void mock_dispatch_tools(struct edc_scheduler *sched, struct edc_task *task, void *llm_resp,
                                void *request_id)
{
    (void)sched;
    (void)task;
    (void)llm_resp;
    (void)request_id;
    g_dispatch_calls++;
}

static void mock_send_reply(struct edc_scheduler *sched, struct edc_task *task, const char *content,
                            edc_channel_message_type message_type, void *request_id)
{
    (void)sched;
    (void)task;
    (void)message_type;
    (void)request_id;
    g_reply_calls++;
    snprintf(g_reply_content, sizeof(g_reply_content), "%s", content ? content : "");
}

/* 投递一条 TOOL_CALL 的 LLM_RESPONSE (模拟模型又请求了一次工具)。 */
static void deliver_llm_tool_call(struct edc_scheduler *sched, long req_id)
{
    edc_actor_t *actor = edc_scheduler_get_actor(sched);
    struct edc_llm_actor_resp *r = edc_llm_actor_resp_new();
    r->reason = BC_LLM_FINISH_REASON_TOOL_CALL;
    r->content = sdsnew("tool round");
    r->msg = cJSON_CreateObject();
    cJSON_AddStringToObject(r->msg, "role", "assistant");
    cJSON_AddStringToObject(r->msg, "content", "let me run a tool");
    edc_actor_msg_t *msg = edc_actor_msg_new(BC_ACTOR_MSG_LLM_RESPONSE, NULL, (void *)(intptr_t)req_id, r,
                                             (edc_actor_data_free_fn)edc_llm_actor_resp_free);
    actor->handler(actor, msg); /* dispatches + frees msg (and resp) */
}

/* 投递一条单工具链 (tr->next == NULL) 的 TOOL_RESPONSE, 模拟工具已执行完。 */
static void deliver_tool_resp(struct edc_scheduler *sched, long req_id)
{
    edc_actor_t *actor = edc_scheduler_get_actor(sched);
    struct edc_toolbox_actor_req *tr = calloc(1, sizeof(*tr));
    tr->name = sdsnew("exec_shell");
    tr->call_id = sdsnew("call_1");
    tr->result = sdsnew("tool result");
    tr->next = NULL;
    edc_actor_msg_t *msg = edc_actor_msg_new(BC_ACTOR_MSG_TOOL_RESPONSE, NULL, (void *)(intptr_t)req_id, tr, NULL);
    actor->handler(actor, msg); /* dispatches + frees msg (not data) */
    sdsfree(tr->name);
    sdsfree(tr->call_id);
    sdsfree(tr->result);
    free(tr);
}

/* 一轮工具循环: LLM 要工具 → 派发 → 工具结果回链尾。返回后由 send_llm
 * 决定是否续发 (或到 max_iter 被截断)。 */
static void run_one_tool_round(struct edc_scheduler *sched, long req_id)
{
    deliver_llm_tool_call(sched, req_id);
    deliver_tool_resp(sched, req_id);
}

static void test_scheduler_max_iter_cap(void)
{
    TEST("max_iter=2 caps tool loop after 2 rounds");
    struct edc_config *cfg = make_config();
    cfg->max_iter = 2;
    struct edc_session *sess = make_session(cfg);
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, sess);

    struct edc_task *leader = edc_scheduler_task_new(sched, sess, "leader", edc_channel_adapter);
    struct edc_task_group *g = edc_scheduler_create_group(sched, sess, leader);
    edc_scheduler_set_foreground(sess, g);
    edc_session_set_active_task(sess, leader);

    g_llm_calls = 0;
    g_dispatch_calls = 0;
    g_reply_calls = 0;
    g_llm_task = NULL;
    sched->send_llm = mock_send_llm;
    sched->dispatch_tools = mock_dispatch_tools;
    sched->send_reply = mock_send_reply;

    drive_input(sched, "hello", 101);
    /* 新模型: 普通输入经命令处理器建一条**新**任务并对它派发, 不再是 attach 到那个
     * 停着的 leader。后面断言的对象换成真正拿到输入的任务 (mock_send_llm 记下了它)。 */
    leader = g_llm_task;
    CHECK(leader != NULL && leader->llm_rounds == 1, "first llm request consumes one round");
    CHECK(g_llm_calls == 1, "first llm request sent");

    run_one_tool_round(sched, req_id_of(leader)); /* 第1轮: 链尾 rounds=1 < 2 → 续发 */
    CHECK(leader->llm_rounds == 2, "chain-end resend consumes round 2");
    CHECK(g_dispatch_calls == 1, "tools dispatched in round 1");
    CHECK(edc_task_awaits_exec(leader), "round 1 continues to llm");
    CHECK(g_llm_calls == 2, "round 1 chain end re-sends llm");

    run_one_tool_round(sched, req_id_of(leader)); /* rounds=2 >= 2 → 截断 */
    CHECK(leader->llm_rounds == 2, "rounds consumed up to the cap");
    CHECK(g_llm_calls == 2, "no third llm request after budget exhausted");
    CHECK(edc_task_is_completed(leader), "task completed at cap");
    CHECK(leader->active_turn == NULL, "active turn committed");
    CHECK(leader->output && strstr(leader->output, "max_iter=2") != NULL, "output carries cap notice");
    CHECK(g_reply_calls == 1, "cap notice replied once");
    CHECK(strstr(g_reply_content, "max_iter=2") != NULL, "reply content explains budget");

    edc_scheduler_free(sched);
    free(sess);
    free_config(cfg);

    TEST("max_iter=1 stops immediately after first tool chain");
    cfg = make_config();
    cfg->max_iter = 1;
    sess = make_session(cfg);
    sched = edc_scheduler_new(cfg);
    install_resolver(sched, sess);

    leader = edc_scheduler_task_new(sched, sess, "leader", edc_channel_adapter);
    g = edc_scheduler_create_group(sched, sess, leader);
    edc_scheduler_set_foreground(sess, g);
    edc_session_set_active_task(sess, leader);

    g_llm_calls = 0;
    g_reply_calls = 0;
    sched->send_llm = mock_send_llm;
    sched->dispatch_tools = mock_dispatch_tools;
    sched->send_reply = mock_send_reply;

    drive_input(sched, "hello", 102);
    leader = g_llm_task; /* 输入建的新任务 (见上) */
    run_one_tool_round(sched, req_id_of(leader));
    CHECK(g_llm_calls == 1, "only the initial llm request");
    CHECK(edc_task_is_completed(leader), "capped right after first chain");
    CHECK(g_reply_calls == 1, "cap notice replied");

    edc_scheduler_free(sched);
    free(sess);
    free_config(cfg);

    TEST("max_iter<=0 keeps unlimited loop (legacy behavior)");
    cfg = make_config(); /* calloc → max_iter == 0 */
    sess = make_session(cfg);
    sched = edc_scheduler_new(cfg);
    install_resolver(sched, sess);

    leader = edc_scheduler_task_new(sched, sess, "leader", edc_channel_adapter);
    g = edc_scheduler_create_group(sched, sess, leader);
    edc_scheduler_set_foreground(sess, g);
    edc_session_set_active_task(sess, leader);

    g_llm_calls = 0;
    sched->send_llm = mock_send_llm;
    sched->dispatch_tools = mock_dispatch_tools;
    sched->send_reply = mock_send_reply;

    drive_input(sched, "hello", 103);
    leader = g_llm_task; /* 输入建的新任务 (见上) */
    run_one_tool_round(sched, req_id_of(leader));
    run_one_tool_round(sched, req_id_of(leader));
    run_one_tool_round(sched, req_id_of(leader));
    CHECK(leader->llm_rounds == 4, "rounds counted while unlimited");
    CHECK(g_llm_calls == 4, "llm keeps being re-sent (no cap)");
    CHECK(leader->active_turn != NULL, "turn stays open while looping");

    edc_scheduler_free(sched);
    free(sess);
    free_config(cfg);

    TEST("max_iter=-1 keeps unlimited loop (default sentinel)");
    cfg = make_config();
    cfg->max_iter = -1;
    sess = make_session(cfg);
    sched = edc_scheduler_new(cfg);
    install_resolver(sched, sess);

    leader = edc_scheduler_task_new(sched, sess, "leader", edc_channel_adapter);
    g = edc_scheduler_create_group(sched, sess, leader);
    edc_scheduler_set_foreground(sess, g);
    edc_session_set_active_task(sess, leader);

    g_llm_calls = 0;
    sched->send_llm = mock_send_llm;
    sched->dispatch_tools = mock_dispatch_tools;
    sched->send_reply = mock_send_reply;

    drive_input(sched, "hello", 104);
    leader = g_llm_task; /* 输入建的新任务 (见上) */
    run_one_tool_round(sched, req_id_of(leader));
    run_one_tool_round(sched, req_id_of(leader));
    CHECK(leader->llm_rounds == 3, "rounds counted while unlimited (-1)");
    CHECK(g_llm_calls == 3, "llm keeps being re-sent with -1 (no cap)");
    CHECK(leader->active_turn != NULL, "turn stays open while looping (-1)");

    edc_scheduler_free(sched);
    free(sess);
    free_config(cfg);
}

/* ── main ──────────────────────────────────────────────────── */

/* ── 任务树快照: 只带侧边栏数据, 不再携带对话消息 ───────────── */

static sds g_snapshot_json = NULL;

static void capture_snapshot(struct edc_scheduler *sched, sds serialized)
{
    (void)sched;
    if (g_snapshot_json)
        sdsfree(g_snapshot_json);
    g_snapshot_json = serialized; /* 接管所有权 */
}

static cJSON *snap_mk_msg(const char *role, const char *content)
{
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "role", role);
    cJSON_AddStringToObject(m, "content", content);
    return m;
}

/* 遍历快照 groups[], 取 fg 组。 */
static cJSON *snap_find_fg(cJSON *root)
{
    cJSON *groups = root ? cJSON_GetObjectItem(root, "groups") : NULL;
    if (!cJSON_IsArray(groups))
        return NULL;
    cJSON *gj;
    cJSON_ArrayForEach(gj, groups)
    {
        if (cJSON_IsTrue(cJSON_GetObjectItem(gj, "fg")))
            return gj;
    }
    return NULL;
}

/* P3 ①b: 快照是**拉取**的 —— 请求里点名要哪棵树 (data = session_id)。 */
static void drive_snapshot(struct edc_scheduler *sched, struct edc_session *session)
{
    edc_actor_t *actor = edc_scheduler_get_actor(sched);
    edc_actor_msg_t *msg = edc_actor_msg_new_sub_type(BC_ACTOR_MSG_TASK_SNAPSHOT, 0, NULL, NULL,
                                                      sdsnew(session ? session->session_id : ""),
                                                      (edc_actor_data_free_fn)sdsfree);
    actor->handler(actor, msg); /* dispatches + frees msg */
}

static void test_snapshot_carries_no_chat_messages(void)
{
    TEST("task snapshot carries sidebar data, never chat messages");
    struct edc_config *cfg = make_config();
    struct edc_session *sess = make_session(cfg);
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, sess);

    struct edc_task *leader = edc_scheduler_task_new(sched, sess, "leader", edc_channel_adapter);
    struct edc_task *member = edc_scheduler_task_new(sched, sess, "member", edc_channel_adapter);
    struct edc_task_group *g = edc_scheduler_create_group(sched, sess, leader);
    edc_task_group_add_task(g, member);
    edc_scheduler_set_foreground(sess, g);

    /* 组历史里有消息, 但快照不应把对话内容下发 —— 聊天区由 TUI 自己持久化。 */
    edc_task_group_history_add_owned(g, snap_mk_msg("system", "group prompt"));
    edc_task_group_history_add_owned(g, snap_mk_msg("user", "q"));

    g_snapshot_json = NULL;
    sched->snapshot_reply = capture_snapshot;
    drive_snapshot(sched, sess);

    cJSON *root = g_snapshot_json ? cJSON_Parse(g_snapshot_json) : NULL;
    CHECK(root != NULL, "snapshot parses as JSON");

    cJSON *fg = snap_find_fg(root);
    CHECK(fg != NULL, "foreground group present");
    CHECK(fg && cJSON_GetObjectItem(fg, "messages") == NULL, "snapshot has no messages[]");
    CHECK(fg && cJSON_GetArraySize(cJSON_GetObjectItem(fg, "tasks")) == 2, "both tasks present");
    cJSON_Delete(root);

    if (g_snapshot_json)
        sdsfree(g_snapshot_json);
    g_snapshot_json = NULL;

    edc_scheduler_free(sched);
    free(sess);
    free_config(cfg);
}

/* ── Task 序号跨重启持久化 ────────────────────────────────── */

static void test_task_id_survives_restart(void)
{
    TEST("task_id does not restart from 1 after a restart");
    struct edc_config *cfg = make_config();
    clean_task_seq(cfg);

    /* 第一次运行: 连发 3 个编号, 随后把它们全部回收 (编号已发出但任务对象不再存活),
     * 再"退出"。 */
    {
        struct edc_session *sess = make_session(cfg);
        struct edc_scheduler *sched = edc_scheduler_new(cfg);
        install_resolver(sched, sess);
        struct edc_task *t1 = edc_scheduler_task_new(sched, sess, "a", edc_channel_adapter);
        struct edc_task *t2 = edc_scheduler_task_new(sched, sess, "b", edc_channel_adapter);
        struct edc_task *t3 = edc_scheduler_task_new(sched, sess, "c", edc_channel_adapter);
        CHECK(t1 && strcmp(t1->task_id, "1") == 0, "first id is 1");
        CHECK(t3 && strcmp(t3->task_id, "3") == 0, "third id is 3");
        edc_task_free(t1);
        edc_task_free(t2);
        edc_task_free(t3);
        edc_scheduler_free(sched);
        free(sess);
    }

    /* 重启: 存活任务为空 (全部被回收), 发号器仍必须从 4 续, 不能退回 1。 */
    {
        struct edc_session *sess = make_session(cfg);
        struct edc_scheduler *sched = edc_scheduler_new(cfg);
        install_resolver(sched, sess);
        struct edc_task *t = edc_scheduler_task_new(sched, sess, "d", edc_channel_adapter);
        CHECK(t && strcmp(t->task_id, "4") == 0, "id continues at 4 after restart (no reuse)");
        edc_task_free(t);
        edc_scheduler_free(sched);
        free(sess);
    }

    /* 收尾: 清掉高水位文件, 免得污染其它测试 / 重复运行。 */
    clean_task_seq(cfg);
    free_config(cfg);
}

/* 反序列化旧存档时, 存活任务的编号要 "喂" 进发号器 (并落盘高水位), 否则
 * 从没有 .task_seq 的旧版本升级上来会重复发号。 */
static void test_observe_task_id_raises_high_water(void)
{
    TEST("observe_task_id raises and persists the high-water mark");
    struct edc_config *cfg = make_config();
    clean_task_seq(cfg);

    {
        struct edc_session *sess = make_session(cfg);
        struct edc_scheduler *sched = edc_scheduler_new(cfg);
        install_resolver(sched, sess);
        CHECK(sched->next_task_id == 1, "fresh counter is 1");
        edc_scheduler_observe_task_id(sched, "7");
        CHECK(sched->next_task_id == 8, "observed id 7 raises counter to 8");
        edc_scheduler_observe_task_id(sched, "task-3"); /* non-numeric: ignored */
        CHECK(sched->next_task_id == 8, "non-numeric id is ignored");
        edc_scheduler_free(sched);
        free(sess);
    }

    /* 重启: 高水位从磁盘恢复。 */
    {
        struct edc_session *sess = make_session(cfg);
        struct edc_scheduler *sched = edc_scheduler_new(cfg);
        install_resolver(sched, sess);
        CHECK(sched->next_task_id == 8, "high-water survives restart");
        char *id = edc_scheduler_gen_task_id(sched);
        CHECK(strcmp(id, "8") == 0, "next generated id is 8");
        free(id);
        edc_scheduler_free(sched);
        free(sess);
    }

    clean_task_seq(cfg);
    free_config(cfg);
}

/* 8. A cancelled task must receive a response after resume.
 * `cancelled_request` is set on cancel and cleared when a new request is registered
 * (edc_scheduler_register_task); without the clear, resume's own response is
 * dropped. This asserts the behaviour, not the flag. */
static void test_cancelled_task_resume_accepts_reply(void)
{
    TEST("a cancelled task accepts the reply of the request resume issued");
    struct edc_config *cfg = make_config();
    struct edc_session *sess = make_session(cfg);
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, sess);
    sched->send_llm = mock_send_llm;
    sched->dispatch_tools = mock_dispatch_tools;

    struct edc_task *t = edc_scheduler_task_new(sched, sess, "run something", edc_channel_adapter);
    struct edc_task_group *g = edc_scheduler_create_group(sched, sess, t);
    edc_scheduler_set_foreground(sess, g);
    edc_session_set_active_task(sess, t);

    /* 停在一轮进行中的 turn 上 (除取消外, 与"跑到一半"没有区别)。 */
    t->active_turn = edc_turn_new();
    edc_turn_add_user_message(t->active_turn, "run something");
    edc_task_await_exec(t);
    edc_task_await_tools(t);

    edc_task_send_signal(t, TASK_SIGTERM); /* = /task stop */
    CHECK(edc_task_is_cancelled(t), "task is CANCELLED");
    CHECK(t->cancelled_request == 1, "cancel marked the in-flight request dead");

    g_llm_calls = 0;
    g_dispatch_calls = 0;
    edc_scheduler_resume_task(sched, t);
    CHECK(edc_task_awaits_exec(t), "resume re-issued the LLM request");
    CHECK(g_llm_calls == 1, "exactly one LLM request went out");
    CHECK(t->cancelled_request == 0, "the marker was cleared for the new request");

    /* 关键断言: 那条新请求的应答被**接受**。若被丢弃, 任务会永远停在 AWAIT_EXEC
     * (界面就显示成"执行中"而再无消息 —— 正是真机看到的那个样子)。 */
    deliver_llm_tool_call(sched, (long)(intptr_t)t->current_request_id);
    CHECK(g_dispatch_calls == 1, "the reply was accepted -> tools dispatched");
    CHECK(edc_task_awaits_tools(t), "task advanced to AWAIT_TOOLS");

    edc_scheduler_free(sched);
    free(sess);
    free_config(cfg);
}

int main(void)
{
    log_info("=== Phase 3: edc_scheduler 模块测试 ===");
    log_info("");

    test_scheduler_lifecycle();
    test_scheduler_task_mgmt();
    test_task_raw_input();
    test_task_history_pull();
    test_scheduler_group_mgmt();
    test_scheduler_fg_bg();
    test_task_placement();
    test_current_task();
    test_resume_queues();
    test_scheduler_signals();
    test_scheduler_group_signals();
    test_signal_cascade_settles_dependents();
    test_failure_cascade_settles_dependents();
    test_schedule_drives_released_sequence_dep();
    test_schedule_after_session_stop_sends_nothing();
    test_schedule_drives_dep_released_by_failure();
    test_schedule_drives_queued_dep_without_registered_id();
    test_schedule_is_idempotent();
    test_schedule_without_llm_port_fails_loudly();
    test_schedule_order_is_deterministic();
    test_schedule_to_fixpoint_covers_failure_cascade();
    test_scheduler_signal_all();
    test_input_routing();
    test_scheduler_max_iter_cap();
    test_snapshot_carries_no_chat_messages();
    test_task_id_survives_restart();
    test_observe_task_id_raises_high_water();
    test_cancelled_task_resume_accepts_reply();

    log_info("");
    log_info("=== 测试结果: %d passed, %d failed ===", test_passed, test_failed);
    return test_failed > 0 ? 1 : 0;
}
