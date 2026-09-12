#include "edc_compat.h"
#include "edc_config.h"
#include "edc_session.h"
#include "edc_task.h"
#include "edc_task_group.h"
#include "log.h"
#include <signal.h>
#include <stdio.h>
#include <string.h>

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

static struct edc_config *make_config(void)
{
    struct edc_config *cfg = g_new0(struct edc_config, 1);
    cfg->workspace = sdsnew("/tmp/test_edc_session_status_ws");
    return cfg;
}

/* ── 1. Current session: groups/tasks/states/fg/active ── */
static void test_current_session_status(void)
{
    TEST("current session status summarises groups + task states");
    struct edc_config *cfg = make_config();
    struct edc_session *s = edc_session_new(cfg, "sess-c", "Current");

    /* group 1: leader COMPLETED + a member LLM_PROCESSING */
    struct edc_task *l1 = edc_task_new("1", "sess-c", "1", "leader one", edc_channel_adapter);
    struct edc_task_group *g1 = edc_task_group_new(l1);
    l1->state = TASK_COMPLETED;
    struct edc_task *m1 = edc_task_new("2", "sess-c", "1", "member", edc_channel_adapter);
    m1->state = TASK_LLM_PROCESSING;
    edc_task_group_add_task(g1, m1);
    edc_session_add_group(s, g1);

    /* group 2: orphaned, leader ERROR */
    struct edc_task *l2 = edc_task_new("3", "sess-c", "3", "leader two", edc_channel_adapter);
    struct edc_task_group *g2 = edc_task_group_new(l2);
    l2->state = TASK_ERROR;
    edc_task_group_mark_orphaned(g2);
    edc_session_add_group(s, g2);

    edc_session_set_active_task(s, m1);

    sds out = edc_session_status_text(s, true, "1");
    CHECK(out != NULL, "status text built");
    CHECK(strstr(out, "[sess-c] Current") != NULL, "id + name header");
    CHECK(strstr(out, "· current") != NULL, "current marker");
    CHECK(strstr(out, "**Groups**: 2 (1 orphaned)") != NULL, "group count + orphan count");
    CHECK(strstr(out, "**Tasks**: 3") != NULL, "task count");
    CHECK(strstr(out, "COMPLETED 1") != NULL, "completed bucket");
    CHECK(strstr(out, "LLM_PROCESSING 1") != NULL, "llm bucket");
    CHECK(strstr(out, "ERROR 1") != NULL, "error bucket");
    CHECK(strstr(out, "**Foreground group**: 1") != NULL, "foreground group id");
    CHECK(strstr(out, "**Active task**: 2 — member") != NULL, "active task line");
    CHECK(strstr(out, "**Working Directory**: (default)") != NULL, "default workdir");
    sdsfree(out);

    /* g1/g2 are owned by the session; edc_session_free releases them. */
    edc_session_free(s);
    edc_config_free(cfg);
}

/* ── 2. Background session with no live groups ── */
static void test_background_session_status(void)
{
    TEST("background session with no loaded state");
    struct edc_config *cfg = make_config();
    struct edc_session *s = edc_session_new(cfg, "sess-b", "Bg");

    sds out = edc_session_status_text(s, false, NULL);
    CHECK(out != NULL, "status text built");
    CHECK(strstr(out, "· background") != NULL, "background marker");
    CHECK(strstr(out, "**Groups**: 0") != NULL, "zero groups");
    CHECK(strstr(out, "**Tasks**: 0") != NULL, "zero tasks");
    CHECK(strstr(out, "not loaded") != NULL, "not-loaded note");
    CHECK(strstr(out, "**Foreground group**") == NULL, "no foreground line for background");
    sdsfree(out);

    edc_session_free(s);
    edc_config_free(cfg);
}

int main(void)
{
    log_info("== test_session_status ==");
    test_current_session_status();
    test_background_session_status();
    log_info("passed=%d failed=%d", test_passed, test_failed);
    return test_failed == 0 ? 0 : 1;
}
