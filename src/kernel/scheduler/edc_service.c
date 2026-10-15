#include "edc_service.h"

#include "edc_actor.h"
#include "edc_scheduler.h"

#include "log.h"

#include <stdlib.h>
#include <string.h>

/* One shared uv_timer per scheduler, always armed for the EARLIEST next_due among the
 * registered services (see edc_service.h). Registering a service whose deadline is
 * sooner simply re-arms it (uv_timer_start overrides an armed timer). */

static void on_service_pump(uv_timer_t *timer);

static uint64_t service_now_ms(struct edc_scheduler *sched)
{
    if (sched && sched->self && sched->self->loop)
        return uv_now(sched->self->loop);
    return 0; /* no loop (unit tests): they pass an explicit clock to edc_services_pump */
}

/* Point the single timer at the earliest deadline (or stop it when there is none). */
static void services_rearm(struct edc_scheduler *sched)
{
    if (!sched)
        return;
    /* Disabled (or no config): never auto-arm. Manual edc_services_pump still works. */
    if (!sched->config || !sched->config->services_enabled)
        return;
    /* No loop yet: services stay manual (unit tests). Production wires the loop before
     * registering, so this is a test-only path. */
    if (!sched->self || !sched->self->loop)
        return;

    if (!sched->service_timer_inited)
    {
        uv_timer_init(sched->self->loop, &sched->service_timer);
        sched->service_timer_inited = true;
    }

    uint64_t now = uv_now(sched->self->loop);
    uint64_t best = UINT64_MAX;
    for (struct edc_service *s = sched->services; s; s = s->next)
        if (s->next_due_ms < best)
            best = s->next_due_ms;

    if (best == UINT64_MAX)
    {
        uv_timer_stop(&sched->service_timer);
        return;
    }
    uint64_t delay = (best > now) ? best - now : 0;
    uv_timer_start(&sched->service_timer, on_service_pump, delay, 0); /* one-shot, re-armed each time */
}

/* The timer callback: defer into the handler. It must NOT run a service here. */
static void on_service_pump(uv_timer_t *timer)
{
    struct edc_scheduler *sched = container_of(timer, struct edc_scheduler, service_timer);
    if (!sched->self)
        return;
    edc_actor_send(sched->self, edc_actor_msg_new(BC_ACTOR_MSG_SERVICE_PUMP, NULL, NULL, NULL, NULL));
}

struct edc_service *edc_service_register(struct edc_scheduler *sched, const char *name, uint64_t interval_ms,
                                         edc_service_tick_fn on_tick, void *ud)
{
    if (!sched || !name || !on_tick)
        return NULL;
    struct edc_service *existing = edc_service_find(sched, name);
    if (existing)
        return existing;

    struct edc_service *svc = calloc(1, sizeof(*svc));
    if (!svc)
        return NULL;
    svc->name = strdup(name);
    svc->interval_ms = interval_ms;
    svc->next_due_ms = service_now_ms(sched) + interval_ms;
    svc->on_tick = on_tick;
    svc->ud = ud;

    /* Append (order is irrelevant to the pump; it fires all due). */
    struct edc_service **tail = &sched->services;
    while (*tail)
        tail = &(*tail)->next;
    *tail = svc;

    log_info("service register: %s every %llums", svc->name, (unsigned long long)interval_ms);
    services_rearm(sched);
    return svc;
}

struct edc_service *edc_service_find(struct edc_scheduler *sched, const char *name)
{
    if (!sched || !name)
        return NULL;
    for (struct edc_service *s = sched->services; s; s = s->next)
        if (s->name && strcmp(s->name, name) == 0)
            return s;
    return NULL;
}

void edc_service_unregister(struct edc_scheduler *sched, const char *name)
{
    if (!sched || !name)
        return;
    struct edc_service **link = &sched->services;
    while (*link)
    {
        if ((*link)->name && strcmp((*link)->name, name) == 0)
        {
            struct edc_service *dead = *link;
            *link = dead->next;
            log_info("service unregister: %s", dead->name);
            free(dead->name);
            free(dead);
            services_rearm(sched);
            return;
        }
        link = &(*link)->next;
    }
}

void edc_service_set_interval(struct edc_scheduler *sched, struct edc_service *svc, uint64_t interval_ms)
{
    if (!sched || !svc)
        return;
    svc->interval_ms = interval_ms;
    svc->next_due_ms = service_now_ms(sched) + interval_ms;
    services_rearm(sched);
}

void edc_services_pump(struct edc_scheduler *sched, uint64_t now_ms)
{
    if (!sched)
        return;
    /* Capture `next` before running on_tick: a service must not unregister another one,
     * but capturing keeps a self-unregister safe too. */
    for (struct edc_service *s = sched->services; s;)
    {
        struct edc_service *next = s->next;
        if (s->next_due_ms <= now_ms)
        {
            uint64_t iv = s->interval_ms ? s->interval_ms : EDC_SERVICE_MIN_INTERVAL_MS;
            s->next_due_ms = now_ms + iv;
            if (s->on_tick)
                s->on_tick(s, s->ud); /* in the handler: this is the message boundary */
        }
        s = next;
    }
    services_rearm(sched);
}

void edc_services_close(struct edc_scheduler *sched)
{
    if (!sched)
        return;
    struct edc_service *s = sched->services;
    while (s)
    {
        struct edc_service *next = s->next;
        free(s->name);
        free(s);
        s = next;
    }
    sched->services = NULL;
}
