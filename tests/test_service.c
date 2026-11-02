/* Background services: one shared timer, re-armed to the earliest next_due.
 *
 * This file pins the registry + pump contract without a uv loop: services are driven
 * by edc_services_pump(sched, fake_now), which is the same body the timer's
 * SERVICE_PUMP message runs -- so the tests exercise the real path. */

#include "edc_compat.h"
#include "test_ws.h"
#include "edc_config.h"
#include "edc_scheduler.h"
#include "edc_service.h"
#include "log.h"
#include <stdio.h>
#include <string.h>

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

static int g_a = 0;
static int g_b = 0;
static struct edc_service *g_self = NULL;
static int g_self_ticks = 0;

static void tick_a(struct edc_service *svc, void *ud)
{
    (void)svc;
    (void)ud;
    g_a++;
}
static void tick_b(struct edc_service *svc, void *ud)
{
    (void)svc;
    (void)ud;
    g_b++;
}
/* Re-arms itself to 5000ms on the first tick (ud carries the scheduler). */
static void tick_self_rearm(struct edc_service *svc, void *ud)
{
    g_self_ticks++;
    if (g_self_ticks == 1)
        edc_service_set_interval((struct edc_scheduler *)ud, svc, 5000);
}

static struct edc_config *make_config(void)
{
    struct edc_config *cfg = g_new0(struct edc_config, 1);
    cfg->workspace = sdsnew(edc_test_workspace());
    return cfg;
}

static void test_register_and_pump(void)
{
    TEST("register / pump: fires at the deadline, not before, again at the next period");
    struct edc_config *cfg = make_config();
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    g_a = 0;

    struct edc_service *a = edc_service_register(sched, "a", 1000, tick_a, NULL);
    CHECK(a != NULL && strcmp(a->name, "a") == 0, "registered by name");
    CHECK(a && a->next_due_ms == 1000, "first deadline = now(0) + interval");
    CHECK(edc_service_register(sched, "a", 9999, tick_b, NULL) == a, "register is idempotent by name");

    edc_services_pump(sched, 0);
    CHECK(g_a == 0, "nothing before the deadline");
    edc_services_pump(sched, 999);
    CHECK(g_a == 0, "still nothing at 999");
    edc_services_pump(sched, 1000);
    CHECK(g_a == 1, "fires at 1000");
    edc_services_pump(sched, 1999);
    CHECK(g_a == 1, "not again before the next period");
    edc_services_pump(sched, 2000);
    CHECK(g_a == 2, "fires again at 2000");

    edc_scheduler_free(sched);
    edc_config_free(cfg);
}

static void test_multiple_services_independent(void)
{
    TEST("several services: only the due ones fire");
    struct edc_config *cfg = make_config();
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    g_a = g_b = 0;

    edc_service_register(sched, "a", 1000, tick_a, NULL);
    edc_service_register(sched, "b", 2500, tick_b, NULL);

    edc_services_pump(sched, 1000);
    CHECK(g_a == 1 && g_b == 0, "only a is due at 1000");
    edc_services_pump(sched, 2000);
    CHECK(g_a == 2 && g_b == 0, "only a again at 2000");
    edc_services_pump(sched, 2500);
    CHECK(g_a == 2 && g_b == 1, "b fires at 2500, a is not due yet");
    edc_services_pump(sched, 3000);
    CHECK(g_a == 3 && g_b == 1, "a back at 3000");

    edc_scheduler_free(sched);
    edc_config_free(cfg);
}

static void test_unregister(void)
{
    TEST("unregister: it stops firing");
    struct edc_config *cfg = make_config();
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    g_a = 0;

    edc_service_register(sched, "a", 1000, tick_a, NULL);
    edc_services_pump(sched, 1000);
    CHECK(g_a == 1, "fired once");
    edc_service_unregister(sched, "a");
    CHECK(edc_service_find(sched, "a") == NULL, "gone from the registry");
    edc_services_pump(sched, 9999);
    CHECK(g_a == 1, "no more ticks after unregister");
    edc_service_unregister(sched, "a"); /* idempotent */
    CHECK(true, "unregister of an absent service is a no-op");

    edc_scheduler_free(sched);
    edc_config_free(cfg);
}

static void test_tick_can_reschedule_itself(void)
{
    TEST("a service may re-arm itself from inside on_tick");
    struct edc_config *cfg = make_config();
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    g_self_ticks = 0;
    g_self = edc_service_register(sched, "self", 1000, tick_self_rearm, sched);

    edc_services_pump(sched, 1000);
    CHECK(g_self_ticks == 1, "first tick at 1000");
    CHECK(g_self && g_self->interval_ms == 5000, "it changed its own period");
    edc_services_pump(sched, 2000);
    CHECK(g_self_ticks == 1, "no tick while it sleeps to the new deadline");
    edc_services_pump(sched, 5000);
    CHECK(g_self_ticks == 2, "tick again at the new deadline");

    edc_scheduler_free(sched);
    edc_config_free(cfg);
}

static void test_close_is_safe(void)
{
    TEST("services_close frees the registry and is safe with no loop");
    struct edc_config *cfg = make_config();
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    edc_service_register(sched, "a", 1000, tick_a, NULL);
    edc_service_register(sched, "b", 2000, tick_b, NULL);
    edc_services_close(sched);
    CHECK(edc_service_find(sched, "a") == NULL && edc_service_find(sched, "b") == NULL, "registry emptied");
    edc_services_close(sched); /* idempotent */
    edc_scheduler_free(sched); /* calls close again: must not double-free */
    edc_config_free(cfg);
    CHECK(true, "no crash");
}

static void test_sleep_cap(void)
{
    TEST("next-delay: the earliest deadline wins, capped at EDC_SERVICE_MAX_SLEEP_MS");
    struct edc_config *cfg = make_config();
    struct edc_scheduler *sched = edc_scheduler_new(cfg);

    CHECK(edc_services_next_delay_ms(sched) == UINT64_MAX, "no services => no deadline");
    edc_service_register(sched, "near", 1000, tick_a, NULL);
    CHECK(edc_services_next_delay_ms(sched) == 1000, "near deadline unchanged");
    edc_service_register(sched, "far", 10 * 60 * 1000, tick_b, NULL);
    CHECK(edc_services_next_delay_ms(sched) == 1000, "the earliest still wins");
    edc_service_unregister(sched, "near");
    CHECK(edc_services_next_delay_ms(sched) == EDC_SERVICE_MAX_SLEEP_MS, "a far deadline is capped at 60s");

    edc_scheduler_free(sched);
    edc_config_free(cfg);
}

/* The real thing: a loop thread running the shared uv_timer, whose PUMP message the
 * actor's wakeup drains into the handler. */
static volatile int g_loop_ticks = 0;
static void tick_loop(struct edc_service *svc, void *ud)
{
    (void)svc;
    (void)ud;
    g_loop_ticks++;
}

static void test_real_loop_timer(void)
{
    TEST("real uv loop: the shared timer arms and fires a registered service");
    struct edc_config *cfg = make_config();
    cfg->services_enabled = true; /* arm the timer */
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    edc_actor_runner_t *runner = edc_actor_runner_new();
    edc_actor_runner_add(runner, edc_scheduler_get_actor(sched));

    /* Register BEFORE the loop runs: uv_timer_init must not race the loop thread. */
    edc_service_register(sched, "loop", 10, tick_loop, NULL);
    g_loop_ticks = 0;
    edc_actor_runner_start(runner);

    for (int i = 0; i < 400 && g_loop_ticks == 0; i++)
        uv_sleep(5);
    CHECK(g_loop_ticks >= 1, "fired through the real loop timer + actor pump");

    edc_actor_runner_free(runner);
    edc_scheduler_free(sched);
    edc_config_free(cfg);
}

int main(void)
{
    edc_test_workspace(); /* open the disposable store before anything writes */
    test_register_and_pump();
    test_multiple_services_independent();
    test_unregister();
    test_tick_can_reschedule_itself();
    test_close_is_safe();
    test_sleep_cap();
    test_real_loop_timer();

    log_info("  ====== %d passed, %d failed ======", test_passed, test_failed);
    return test_failed ? 1 : 0;
}
