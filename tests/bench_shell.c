/* bench_shell: end-to-end cost THROUGH the public door.
 *
 * bench_store measured the store directly. This measures what the product actually pays:
 * edc_session_save_state() / edc_session_load_state(), i.e. the thin shell that the whole
 * kernel calls (18 call sites). It is the number to quote when asking "is it faster than
 * session.json yet".
 *
 *   bazel run //tests:bench_shell -- <workspace> [messages] [flushes]
 */

#include "cJSON.h"
#include "edc_compat.h"
#include "edc_config.h"
#include "edc_log.h"
#include "edc_session.h"
#include "edc_session_manager.h"
#include "edc_scheduler.h"
#include "edc_state_probe.h"
#include "edc_state_store.h"
#include "edc_task.h"
#include "edc_task_group.h"
#include "edc_turn.h"
#include "log.h"
#include "sds.h"
#include "utlist_fix.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>

volatile sig_atomic_t _run_loop = 1;

static void install_sessions(struct edc_scheduler *sched, struct edc_session *sess)
{
    sched->ctx = sess;
    struct edc_session_manager *sm = g_new0(struct edc_session_manager, 1);
    sm->sessions = g_new0(struct edc_session *, 1);
    sm->sessions[0] = sess;
    sm->session_count = sess ? 1 : 0;
    sm->active_index = 0;
    sched->sessions = sm;
}

static double now_ms(void)
{
    return (double)uv_hrtime() / 1e6;
}

int main(int argc, char **argv)
{
    const char *ws = (argc > 1) ? argv[1] : NULL;
    const int want = (argc > 2) ? atoi(argv[2]) : 40000;
    const int flushes = (argc > 3) ? atoi(argv[3]) : 3;
    if (!ws)
    {
        fprintf(stderr, "usage: bench_shell <workspace> [messages] [flushes]\n");
        return 2;
    }

    edc_log_init();
    char cmd[600];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s' && mkdir -p '%s'", ws, ws);
    if (system(cmd) != 0)
        fprintf(stderr, "(warning: could not reset %s)\n", ws);

    struct edc_config *cfg = g_new0(struct edc_config, 1);
    cfg->workspace = sdsnew(ws);
    /* Open the store BEFORE building the tree: messages are rows and are written as they
     * are added, so without an open store they would have nowhere to go. */
    if (!edc_state_store_open(cfg))
    {
        fprintf(stderr, "cannot open the state store on %s\n", ws);
        return 1;
    }

    struct edc_session *s = edc_session_new(cfg, "bench", "Bench");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_sessions(sched, s);

    struct edc_task *leader = edc_task_new("b-1", s->session_id, "bg-1", "bench task",
                                           edc_channel_adapter);
    leader->is_group_leader = 1;
    leader->step = EDC_TASKSTEP_DONE;
    leader->life = EDC_LIFE_COMPLETED;
    edc_queue_init(&leader->turns);
    edc_session_add_group(s, edc_task_group_new(leader));

    char *body = malloc(4200);
    memset(body, 'x', 4199);
    body[4199] = '\0';
    int made = 0;
    while (made < want)
    {
        /* Bound to the task's storage key up front: messages are written as they are
         * added, so a turn without a key would have nowhere to put them. */
        struct edc_turn *t = edc_turn_new_for(leader->session_id, leader->group_id,
                                              leader->task_id, (int)edc_queue_length(&leader->turns));
        for (int i = 0; i < 200 && made < want; i++, made++)
        {
            cJSON *m = cJSON_CreateObject();
            cJSON_AddStringToObject(m, "role", (made % 2) ? "assistant" : "user");
            cJSON_AddStringToObject(m, "content", body);
            edc_turn_add_message(t, m);
        }
        /* edc_task_add_turn, not a raw push: it binds the turn to the task's storage key
         * (a bare push would leave the rows under the constructor's placeholder key). */
        edc_task_add_turn(leader, t);
    }
    free(body);

    printf("workspace : %s\n", ws);
    printf("messages  : %d (%.1f MB)\n", made, (double)made * 4.2 / 1024.0);
    printf("door      : edc_session_save_state / load_state (the real call path)\n\n");

    for (int i = 0; i < flushes; i++)
    {
        const double t0 = now_ms();
        const bool ok = edc_session_save_state(s, sched);
        printf("  save_state #%d : %8.1f ms  %s\n", i + 1, now_ms() - t0, ok ? "ok" : "FAILED");
    }

    {
        struct edc_session *r = edc_session_new(cfg, "bench", "Bench");
        struct edc_scheduler *rsched = edc_scheduler_new(cfg);
        install_sessions(rsched, r);
        const double t0 = now_ms();
        const bool ok = edc_session_load_state(r, rsched);
        printf("\n  load_state    : %8.1f ms  %s  (%zu messages)\n", now_ms() - t0,
               ok ? "ok" : "FAILED", edc_session_get_message_count(r));
        edc_session_free(r);
        edc_scheduler_free(rsched);
    }

    printf("\n  baseline to beat (session.json): save 770~841 ms, load 526~683 ms\n");

    edc_state_store_close();
    edc_session_free(s);
    edc_scheduler_free(sched);
    edc_log_close();
    return 0;
}
