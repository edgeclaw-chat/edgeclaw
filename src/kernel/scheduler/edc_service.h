#ifndef __BC_SERVICE_H__
#define __BC_SERVICE_H__

#include <stdbool.h>
#include <stdint.h>

struct edc_scheduler;

/* Background services: named, code-registered "silent checks" the host runs on a
 * timer (e.g. "scan idle groups and start an extraction"). They exist because there
 * is no global tick -- today the message boundary is the tick -- and some work has to
 * happen when NOBODY is talking. A single uv_timer on the scheduler drives them all,
 * re-armed to the earliest next_due, so idle time costs nothing.
 *
 * Invariant (why this does not break the "no tick" rule): a service NEVER runs from
 * the timer callback. The timer only posts BC_ACTOR_MSG_SERVICE_PUMP, and the
 * scheduler handler runs the due services -- so every state change still happens at a
 * message boundary, in the one serial handler. A timer callback that mutated state
 * directly is exactly what this module exists to prevent. */

struct edc_service;

/* Runs in the scheduler handler: must be non-blocking (decide + start a task / post a
 * message), defensive, and must NOT unregister another service or free `svc`. */
typedef void (*edc_service_tick_fn)(struct edc_service *svc, void *ud);

struct edc_service
{
    char *name;           /* stable id (owned) */
    uint64_t interval_ms; /* period; the next deadline is fire-time + this */
    uint64_t next_due_ms; /* monotonic (uv_now) deadline */
    edc_service_tick_fn on_tick;
    void *ud; /* caller's; the service does not own it */
    struct edc_service *next;
};

/* A service may not fire more often than this (guards against a busy loop). */
#define EDC_SERVICE_MIN_INTERVAL_MS 1000

/* Register a service. Idempotent by name (an existing name is returned unchanged).
 * Returns NULL only on allocation failure. The first deadline is now + interval. */
extern struct edc_service *edc_service_register(struct edc_scheduler *sched, const char *name, uint64_t interval_ms,
                                                edc_service_tick_fn on_tick, void *ud);
extern struct edc_service *edc_service_find(struct edc_scheduler *sched, const char *name);

/* Remove a service (no-op when absent). */
extern void edc_service_unregister(struct edc_scheduler *sched, const char *name);

/* Change a service's period and re-arm (a service uses this to "sleep until the next
 * thing"; e.g. crontab sets the delay to the next job). */
extern void edc_service_set_interval(struct edc_scheduler *sched, struct edc_service *svc, uint64_t interval_ms);

/* Run every service whose next_due <= now_ms, then re-arm the single timer. This IS
 * the pump body the scheduler handler calls; tests call it directly with a fake clock
 * (there is no loop in most unit tests, so this is the only way to drive services). */
extern void edc_services_pump(struct edc_scheduler *sched, uint64_t now_ms);

/* Free the registry. Deliberately does NOT touch the uv timer: the actor runner closes
 * every remaining handle on shutdown (uv_walk), and by the time this runs the loop may
 * already be gone. The timer lives inside `struct edc_scheduler`, so there is nothing
 * heap-allocated to leak. */
extern void edc_services_close(struct edc_scheduler *sched);

#endif
