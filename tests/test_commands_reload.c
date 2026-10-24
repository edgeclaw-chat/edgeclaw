/* `/reload`: the system prompt is built ONCE per group and CACHED in `group->history[0]`
 * (every request references that node -- that constancy is what keeps the provider's cache
 * prefix stable). So a skill or AGENTS.md added on disk would never take effect without an
 * explicit rebuild -- that is what `/reload` is for.
 *
 * This drives the real command handler and pins two things:
 *   ① history[0]'s content is re-derived from disk (a skill added after the prompt was
 *      built shows up);
 *   ② the NODE is kept -- only its `content` is replaced -- so a request holding a
 *      reference to it cannot dangle.
 *
 * Harness copied from test_commands_prompt.c. */
#include "edc_channel_actor.h"
#include "test_ws.h"
#include "edc_commands.h"
#include "edc_config.h"
#include "edc_context.h"
#include "edc_scheduler.h"
#include "edc_session.h"
#include "edc_session_manager.h"
#include "edc_task.h"
#include "edc_task_group.h"

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
    struct edc_session_manager *sm = calloc(1, sizeof(*sm));
    sm->sessions = calloc(1, sizeof(struct edc_session *));
    sm->sessions[0] = sess;
    sm->session_count = sess ? 1 : 0;
    sm->active_index = 0;
    sched->sessions = sm; /* 测试进程内不释放: 与测试同生共死 */
}

static void drive_command(struct edc_config *cfg, struct edc_session_manager *sm, struct edc_scheduler *sched,
                          const char *content)
{
    struct edc_channel_message *cm =
        edc_channel_message_new(edc_channel_adapter, EDC_CHANNEL_MESSAGE_USER_TYPE, sdsnew(content));
    edc_commands_handler(cfg, sm, sched, (struct edc_reply_to){.type = cm->type}, cm);
    edc_scheduler_schedule(sched); /* "this message is done" */
}

static void write_file_at(const char *path, const char *content)
{
    FILE *f = fopen(path, "w");
    if (!f)
    {
        printf("  (cannot write %s)\n", path);
        return;
    }
    fputs(content, f);
    fclose(f);
}

int main(void)
{
    edc_test_workspace(); /* open the disposable store before anything writes */
    const char *ws = edc_test_workspace();
    char cmd[1024];
    /* A workspace with ONE skill at first. */
    snprintf(cmd, sizeof(cmd), "rm -rf %s && mkdir -p %s/skills/first", ws, ws);
    if (system(cmd) != 0)
        return 1;
    char skill[1024];
    snprintf(skill, sizeof(skill), "%s/skills/first/SKILL.md", ws);
    write_file_at(skill, "---\ndescription: the first skill\n---\nbody\n");

    struct edc_config *cfg = calloc(1, sizeof(*cfg));
    cfg->workspace = sdsnew(ws);

    struct edc_session_manager *sm = edc_session_manager_new(cfg);
    struct edc_session *sess = edc_session_manager_get_active(sm);
    if (!sess)
    {
        printf("  FAIL: no active session\n");
        return 1;
    }

    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, sess);
    sched->send_llm = NULL;

    TEST("the initial system prompt names the skill that exists now");
    sds prompt0 = edc_context_build_system_prompt(cfg, NULL);
    CHECK(prompt0 && strstr(prompt0, "first") != NULL, "`first` is in the prompt");

    /* A group whose cached prompt is the one we just built. */
    struct edc_task *leader = edc_task_new("1", "s1", "1", "lead", edc_channel_adapter);
    struct edc_task_group *g = edc_task_group_new(leader);
    edc_session_add_group(sess, g);

    cJSON *sys = cJSON_CreateObject();
    cJSON_AddStringToObject(sys, "role", "system");
    cJSON_AddStringToObject(sys, "content", prompt0);
    edc_task_group_history_add_owned(g, sys);
    sdsfree(prompt0);

    TEST("the group cached it as history[0]");
    CHECK(edc_task_group_history_count(g) >= 1, "history has a first slot");
    cJSON *slot0 = edc_task_group_history_owned_at(g, 0);
    cJSON *content0 = cJSON_GetObjectItem(slot0, "content");
    CHECK(content0 && cJSON_IsString(content0) && strstr(content0->valuestring, "first") != NULL,
          "the slot holds the prompt");

    /* A NEW skill appears on disk after the prompt was built. */
    snprintf(cmd, sizeof(cmd), "mkdir -p %s/skills/second", ws);
    if (system(cmd) != 0)
        return 1;
    snprintf(skill, sizeof(skill), "%s/skills/second/SKILL.md", ws);
    write_file_at(skill, "---\ndescription: the second skill\n---\nbody\n");

    TEST("the cached prompt does NOT see it (that is why /reload exists)");
    CHECK(strstr(content0->valuestring, "second") == NULL, "still the old bytes");

    drive_command(cfg, sm, sched, "/reload");

    TEST("/reload re-derives history[0] from disk, keeping the node");
    cJSON *slot1 = edc_task_group_history_owned_at(g, 0);
    CHECK(slot1 == slot0, "the very same node (references cannot dangle)");
    cJSON *content1 = cJSON_GetObjectItem(slot1, "content");
    CHECK(content1 && cJSON_IsString(content1), "content is still a string");
    CHECK(content1 && strstr(content1->valuestring, "second") != NULL, "the new skill is in the prompt now");
    CHECK(content1 && strstr(content1->valuestring, "first") != NULL, "and so is the old one");

    TEST("/reload again is a no-op");
    sds after_first = sdsnew(content1->valuestring);
    drive_command(cfg, sm, sched, "/reload");
    cJSON *content2 = cJSON_GetObjectItem(edc_task_group_history_owned_at(g, 0), "content");
    CHECK(content2 && strcmp(content2->valuestring, after_first) == 0, "the bytes did not move");
    sdsfree(after_first);

    snprintf(cmd, sizeof(cmd), "rm -rf %s", ws);
    if (system(cmd) != 0)
        printf("  (cleanup failed)\n");

    printf("\n  %d passed, %d failed\n", passed, failed);
    return failed > 0 ? 1 : 0;
}
