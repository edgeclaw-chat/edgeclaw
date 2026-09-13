/* Chat-log replay: session.json -> display-ordered bubble stream.
 *
 * This is what replaced the TUI's private chat.jsonl transcript. The walk lives
 * in edc_session_chat_replay (agent layer), so it is covered here in C rather
 * than through the C++ frontend.
 *
 * Covered:
 *   a. role -> kind mapping matches the live senders (assistant+tool_calls is a
 *      "thought", a final assistant message a "reply", tool carries its name as
 *      the header, session-level messages are "sys");
 *   b. messages with nothing to show are skipped;
 *   c. ordering is by message.ts, so a system/one-off note (command echo,
 *      command output) lands between the turns it was interleaved with;
 *   d. messages of different tasks are merged by timestamp too.
 */

#include "edc_channel_actor.h"
#include "edc_compat.h"
#include "edc_config.h"
#include "edc_session.h"
#include "edc_task.h"
#include "edc_task_group.h"
#include "edc_turn.h"
#include "log.h"
#include "sds.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* ── emitted-bubble collector ─────────────────────────────── */

#define MAX_BUBBLES 32
struct bubble
{
    char kind[16];
    char header[64];
    char content[256];
    char task_id[32];
    char render[16];
};

static struct bubble got[MAX_BUBBLES];
static int got_n = 0;

static void collect(void *user, const char *kind, const char *header, const char *content, const char *task_id,
                    const char *render)
{
    (void)user;
    if (got_n >= MAX_BUBBLES)
        return;
    snprintf(got[got_n].kind, sizeof(got[0].kind), "%s", kind ? kind : "");
    snprintf(got[got_n].header, sizeof(got[0].header), "%s", header ? header : "");
    snprintf(got[got_n].content, sizeof(got[0].content), "%s", content ? content : "");
    snprintf(got[got_n].task_id, sizeof(got[0].task_id), "%s", task_id ? task_id : "");
    snprintf(got[got_n].render, sizeof(got[0].render), "%s", render ? render : "");
    got_n++;
}

static void reset_bubbles(void)
{
    got_n = 0;
    memset(got, 0, sizeof(got));
}

static void check_bubble(int i, const char *kind, const char *header, const char *content, const char *render,
                         const char *label)
{
    char msg[256];
    if (i >= got_n)
    {
        snprintf(msg, sizeof(msg), "%s: bubble %d missing (got %d)", label, i, got_n);
        CHECK(false, msg);
        return;
    }
    snprintf(msg, sizeof(msg), "%s: bubble %d kind=%s", label, i, kind);
    CHECK(strcmp(got[i].kind, kind) == 0, msg);
    snprintf(msg, sizeof(msg), "%s: bubble %d header", label, i);
    CHECK(strcmp(got[i].header, header) == 0, msg);
    snprintf(msg, sizeof(msg), "%s: bubble %d content", label, i);
    CHECK(strcmp(got[i].content, content) == 0, msg);
    snprintf(msg, sizeof(msg), "%s: bubble %d render", label, i);
    CHECK(strcmp(got[i].render, render) == 0, msg);
}

/* ── helpers ──────────────────────────────────────────────── */

static struct edc_config *make_config(void)
{
    struct edc_config *cfg = g_new0(struct edc_config, 1);
    cfg->workspace = sdsnew("/tmp/test_chat_replay_ws");
    return cfg;
}

/* Append |json| to |turn| with an explicit timestamp (so ordering is testable
 * without sleeping) — edc_turn_add_message keeps a ts that is already there. */
static void add_msg_ts(struct edc_turn *turn, const char *ts, const char *json)
{
    cJSON *m = cJSON_Parse(json);
    cJSON_AddStringToObject(m, "ts", ts);
    edc_turn_add_message(turn, m);
}

static struct edc_task *add_task(struct edc_session *session, const char *id, const char *desc)
{
    struct edc_task *t = edc_task_new(id, session->session_id, id, desc, edc_channel_adapter);
    edc_task_set_state(t, TASK_WAITING_INPUT);
    edc_task_set_state(t, TASK_LLM_PROCESSING);
    edc_task_set_state(t, TASK_COMPLETED);
    edc_session_add_group(session, edc_task_group_new(t));
    return t;
}

/* ── 1. mapping ───────────────────────────────────────────── */

static void test_mapping(void)
{
    TEST("role -> kind mapping matches the live senders");
    struct edc_config *cfg = make_config();
    struct edc_session *s = edc_session_new(cfg, "s-map", "Map");

    struct edc_task *t = add_task(s, "t1", "one");
    struct edc_turn *turn = edc_turn_new();
    turn->task_id = strdup("t1");

    add_msg_ts(turn, "2026-09-13 10:00:00", "{\"role\":\"user\",\"content\":\"hello\"}");
    add_msg_ts(turn, "2026-09-13 10:00:01",
               "{\"role\":\"assistant\",\"reasoning_content\":\"thinking\",\"tool_calls\":[{\"id\":\"c1\"}]}");
    add_msg_ts(turn, "2026-09-13 10:00:02",
               "{\"role\":\"tool\",\"name\":\"exec_shell\",\"tool_call_id\":\"c1\",\"content\":\"out\"}");
    add_msg_ts(turn, "2026-09-13 10:00:03",
               "{\"role\":\"assistant\",\"content\":\"final\",\"render\":\"markdown\"}");
    edc_task_add_turn(t, turn);

    reset_bubbles();
    edc_session_chat_replay(s, collect, NULL);
    CHECK(got_n == 4, "4 bubbles emitted");
    check_bubble(0, "user", "", "hello", "plain", "map");
    check_bubble(1, "thought", "", "thinking", "plain", "map");
    check_bubble(2, "tool", "exec_shell", "out", "plain", "map");
    check_bubble(3, "reply", "", "final", "markdown", "map");
    CHECK(strcmp(got[2].task_id, "t1") == 0, "tool bubble carries its task id");

    edc_session_free(s);
    edc_config_free(cfg);
}

/* ── 2. nothing-to-show messages ──────────────────────────── */

static void test_skips(void)
{
    TEST("messages with nothing to show are skipped");
    struct edc_config *cfg = make_config();
    struct edc_session *s = edc_session_new(cfg, "s-skip", "Skip");

    struct edc_task *t = add_task(s, "t1", "one");
    struct edc_turn *turn = edc_turn_new();
    turn->task_id = strdup("t1");

    add_msg_ts(turn, "2026-09-13 10:00:00", "{\"role\":\"user\",\"content\":\"hi\"}");
    /* tool-call step with neither content nor reasoning */
    add_msg_ts(turn, "2026-09-13 10:00:01", "{\"role\":\"assistant\",\"tool_calls\":[{\"id\":\"c1\"}]}");
    /* final assistant message with empty content */
    add_msg_ts(turn, "2026-09-13 10:00:02", "{\"role\":\"assistant\",\"content\":\"\"}");
    /* tool result with empty content */
    add_msg_ts(turn, "2026-09-13 10:00:03", "{\"role\":\"tool\",\"name\":\"exec_shell\",\"content\":\"\"}");
    /* unknown role */
    add_msg_ts(turn, "2026-09-13 10:00:04", "{\"role\":\"weird\",\"content\":\"?\"}");
    /* no role at all */
    add_msg_ts(turn, "2026-09-13 10:00:05", "{\"content\":\"?\"}");
    edc_task_add_turn(t, turn);

    reset_bubbles();
    edc_session_chat_replay(s, collect, NULL);
    CHECK(got_n == 1, "only the user message is emitted");
    check_bubble(0, "user", "", "hi", "plain", "skip");

    edc_session_free(s);
    edc_config_free(cfg);
}

/* ── 3. session-level messages interleaved by ts ──────────── */

static void test_system_interleave(void)
{
    TEST("command echo/output interleave with the conversation by ts");
    struct edc_config *cfg = make_config();
    struct edc_session *s = edc_session_new(cfg, "s-sys", "Sys");

    struct edc_task *t = add_task(s, "t1", "one");
    struct edc_turn *turn = edc_turn_new();
    turn->task_id = strdup("t1");
    add_msg_ts(turn, "2026-09-13 10:00:00", "{\"role\":\"user\",\"content\":\"do the thing\"}");
    add_msg_ts(turn, "2026-09-13 10:00:04", "{\"role\":\"assistant\",\"content\":\"done\"}");
    edc_task_add_turn(t, turn);

    /* the /task list echo + its output land in the system task in between */
    cJSON *echo = cJSON_Parse("{\"role\":\"user\",\"content\":\"/task list\",\"ts\":\"2026-09-13 10:00:01\"}");
    edc_session_add_system_message(s, echo);
    cJSON *out = cJSON_Parse("{\"role\":\"system\",\"content\":\"**Tasks**\\n- t1\",\"ts\":\"2026-09-13 10:00:02\"}");
    edc_session_add_system_message(s, out);

    reset_bubbles();
    edc_session_chat_replay(s, collect, NULL);
    CHECK(got_n == 4, "4 bubbles emitted");
    check_bubble(0, "user", "", "do the thing", "plain", "sys");
    check_bubble(1, "user", "", "/task list", "plain", "sys");
    check_bubble(2, "sys", "", "**Tasks**\n- t1", "plain", "sys");
    check_bubble(3, "reply", "", "done", "plain", "sys");
    CHECK(strcmp(got[1].task_id, "system") == 0, "command echo belongs to the system task");

    edc_session_free(s);
    edc_config_free(cfg);
}

/* ── 4. ordering across tasks ─────────────────────────────── */

static void test_cross_task_order(void)
{
    TEST("messages of different tasks merge by timestamp");
    struct edc_config *cfg = make_config();
    struct edc_session *s = edc_session_new(cfg, "s-x", "X");

    struct edc_task *a = add_task(s, "tA", "A");
    struct edc_turn *turn_a = edc_turn_new();
    turn_a->task_id = strdup("tA");
    add_msg_ts(turn_a, "2026-09-13 10:00:05", "{\"role\":\"user\",\"content\":\"later\"}");
    edc_task_add_turn(a, turn_a);

    struct edc_task *b = add_task(s, "tB", "B");
    struct edc_turn *turn_b = edc_turn_new();
    turn_b->task_id = strdup("tB");
    add_msg_ts(turn_b, "2026-09-13 10:00:01", "{\"role\":\"user\",\"content\":\"earlier\"}");
    edc_task_add_turn(b, turn_b);

    reset_bubbles();
    edc_session_chat_replay(s, collect, NULL);
    CHECK(got_n == 2, "2 bubbles emitted");
    check_bubble(0, "user", "", "earlier", "plain", "x-task");
    check_bubble(1, "user", "", "later", "plain", "x-task");

    edc_session_free(s);
    edc_config_free(cfg);
}

/* ── 5. the active (uncommitted) turn is included ─────────── */

static void test_active_turn(void)
{
    TEST("the in-flight turn is replayed after the committed ones");
    struct edc_config *cfg = make_config();
    struct edc_session *s = edc_session_new(cfg, "s-live", "Live");

    struct edc_task *t = add_task(s, "t1", "one");
    struct edc_turn *committed = edc_turn_new();
    committed->task_id = strdup("t1");
    add_msg_ts(committed, "2026-09-13 10:00:00", "{\"role\":\"user\",\"content\":\"first\"}");
    edc_task_add_turn(t, committed);

    t->active_turn = edc_turn_new();
    t->active_turn->task_id = strdup("t1");
    add_msg_ts(t->active_turn, "2026-09-13 10:00:01", "{\"role\":\"user\",\"content\":\"second\"}");
    add_msg_ts(t->active_turn, "2026-09-13 10:00:02", "{\"role\":\"assistant\",\"content\":\"partial\"}");

    reset_bubbles();
    edc_session_chat_replay(s, collect, NULL);
    CHECK(got_n == 3, "3 bubbles emitted (committed + in-flight)");
    check_bubble(0, "user", "", "first", "plain", "live");
    check_bubble(1, "user", "", "second", "plain", "live");
    check_bubble(2, "reply", "", "partial", "plain", "live");

    edc_session_free(s);
    edc_config_free(cfg);
}

/* ── 6. session-level helpers land in the system task ─────── */

static void test_system_text_helper(void)
{
    TEST("edc_session_add_system_text feeds the system task and the replay");
    struct edc_config *cfg = make_config();
    struct edc_session *s = edc_session_new(cfg, "s-note", "Note");

    edc_session_add_system_text(s, "user", "/task list");
    edc_session_add_system_text(s, "system", "**Tasks**");
    edc_session_add_system_text(s, "", "dropped");     /* empty role: ignored */
    edc_session_add_system_text(s, "system", "");      /* empty text: ignored */

    struct edc_task *sys = edc_session_find_task(s, "system");
    CHECK(sys != NULL, "system task created on demand");
    CHECK(sys && sys->is_system, "system task is flagged is_system");
    CHECK(sys && sys->active_turn && edc_queue_length(&sys->active_turn->messages) == 2, "2 messages stored");

    reset_bubbles();
    edc_session_chat_replay(s, collect, NULL);
    CHECK(got_n == 2, "2 bubbles emitted");
    check_bubble(0, "user", "", "/task list", "plain", "note");
    check_bubble(1, "sys", "", "**Tasks**", "plain", "note");

    edc_session_free(s);
    edc_config_free(cfg);
}

/* ── 7. tool display metadata (header / render / displayed) ── */

static void test_tool_display_meta(void)
{
    TEST("tool bubbles replay the header/render the tool actually emitted");
    struct edc_config *cfg = make_config();
    struct edc_session *s = edc_session_new(cfg, "s-toolmeta", "ToolMeta");

    struct edc_task *t = add_task(s, "t1", "one");
    struct edc_turn *turn = edc_turn_new();
    turn->task_id = strdup("t1");

    add_msg_ts(turn, "2026-09-13 10:00:00", "{\"role\":\"user\",\"content\":\"build it\"}");
    /* what exec_shell persists: header = the command, render = plain */
    add_msg_ts(turn, "2026-09-13 10:00:01",
               "{\"role\":\"tool\",\"name\":\"exec_shell\",\"tool_call_id\":\"c1\",\"content\":\"out\","
               "\"displayed\":true,\"header\":\"git status --short\",\"render\":\"plain\"}");
    /* markdown tool (todowrite-shaped), header declared as usual */
    add_msg_ts(turn, "2026-09-13 10:00:02",
               "{\"role\":\"tool\",\"name\":\"todowrite\",\"tool_call_id\":\"c2\",\"content\":\"# TODO\","
               "\"displayed\":true,\"header\":\"todowrite\",\"render\":\"markdown\"}");
    /* displayed, but the tool emitted no header at all → empty header, not the name */
    add_msg_ts(turn, "2026-09-13 10:00:03",
               "{\"role\":\"tool\",\"name\":\"device_shell\",\"tool_call_id\":\"c3\",\"content\":\"out3\","
               "\"displayed\":true}");
    /* never on screen live (e.g. the interrupted-tool note) → replay skips it too */
    add_msg_ts(turn, "2026-09-13 10:00:04",
               "{\"role\":\"tool\",\"name\":\"exec_shell\",\"tool_call_id\":\"c4\",\"content\":\"note\","
               "\"displayed\":false}");
    /* pre-display-metadata data: falls back to the tool name as the header */
    add_msg_ts(turn, "2026-09-13 10:00:05",
               "{\"role\":\"tool\",\"name\":\"read_file\",\"tool_call_id\":\"c5\",\"content\":\"legacy\"}");
    edc_task_add_turn(t, turn);

    reset_bubbles();
    edc_session_chat_replay(s, collect, NULL);
    CHECK(got_n == 5, "5 bubbles (1 user + 4 tools; the hidden one dropped)");
    check_bubble(0, "user", "", "build it", "plain", "toolmeta");
    check_bubble(1, "tool", "git status --short", "out", "plain", "toolmeta");
    check_bubble(2, "tool", "todowrite", "# TODO", "markdown", "toolmeta");
    check_bubble(3, "tool", "", "out3", "plain", "toolmeta");
    check_bubble(4, "tool", "read_file", "legacy", "plain", "toolmeta");

    edc_session_free(s);
    edc_config_free(cfg);
}

/* ── 8. degenerate input ──────────────────────────────────── */

static void test_degenerate(void)
{
    TEST("NULL/empty session is safe");
    reset_bubbles();
    edc_session_chat_replay(NULL, collect, NULL);
    CHECK(got_n == 0, "NULL session emits nothing");

    struct edc_config *cfg = make_config();
    struct edc_session *s = edc_session_new(cfg, "s-empty", "Empty");
    edc_session_chat_replay(s, collect, NULL);
    CHECK(got_n == 0, "empty session emits nothing");
    edc_session_chat_replay(s, NULL, NULL); /* no callback: must not crash */
    CHECK(got_n == 0, "NULL callback emits nothing");
    edc_session_free(s);
    edc_config_free(cfg);
}

int main(void)
{
    log_info("=== chat replay (derived chat log) ===");
    test_mapping();
    test_skips();
    test_system_interleave();
    test_cross_task_order();
    test_active_turn();
    test_system_text_helper();
    test_tool_display_meta();
    test_degenerate();

    log_info("=== Results: %d passed, %d failed ===", test_passed, test_failed);
    return test_failed > 0 ? 1 : 0;
}
