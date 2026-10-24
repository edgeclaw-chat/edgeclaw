#ifndef EDC_TEST_WS_H
#define EDC_TEST_WS_H

/* ── A real, disposable workspace for tests ──────────────────────────────────
 *
 * Messages are rows now (see edc_state_store.h), so a test that builds a turn has to have
 * a store open or its messages go nowhere. This helper gives every test binary one:
 *
 *   - edc_test_workspace() creates a mkdtemp directory on FIRST use and opens the state
 *     store on it. Because the store is process-global and reuses its connection when the
 *     workspace matches, every later config built on the same path shares that store --
 *     which is exactly what a test that writes messages and then reads them back needs.
 *   - edc_test_store_down() closes the store and deletes the directory.
 *
 * Usage in a test:
 *
 *     cfg->workspace = sdsnew(edc_test_workspace());   // instead of a fixed /tmp path
 *     ...
 *     int main(void) { ...; edc_test_store_down(NULL); return failures; }
 *
 * Deliberately a REAL FILE on disk (option B), not SQLite's ":memory:": the point is that
 * the tests exercise the same pager, the same WAL and the same paths the product uses, so
 * a mistake in the on-disk shape cannot hide behind an in-memory one.
 *
 * Header-only and `static`, so each test binary stays a single translation unit and the
 * helper cannot collide across targets.
 */

#include "edc_compat.h"
#include "edc_config.h"
#include "edc_state_store.h"
#include "edc_turn.h"
#include "sds.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char g_edc_test_ws[512];

/* Create the workspace and open the store on it, once. Every subsequent call returns the
 * same path, so configs built from it reuse the open connection. */
static const char *edc_test_workspace(void)
{
    if (!g_edc_test_ws[0])
    {
        snprintf(g_edc_test_ws, sizeof(g_edc_test_ws), "/tmp/edgeclaw-test-XXXXXX");
        if (!mkdtemp(g_edc_test_ws))
        {
            perror("mkdtemp");
            abort();
        }
        struct edc_config tmp;
        memset(&tmp, 0, sizeof(tmp));
        tmp.workspace = sdsnew(g_edc_test_ws);
        if (!edc_state_store_open(&tmp))
        {
            fprintf(stderr, "edc_test_workspace: could not open the state store on %s\n",
                    g_edc_test_ws);
            abort();
        }
        sdsfree(tmp.workspace); /* the store keeps its own copy */
    }
    return g_edc_test_ws;
}

/* A config on the shared test workspace, store already open. */
static struct edc_config *edc_test_config(void)
{
    struct edc_config *cfg = g_new0(struct edc_config, 1);
    cfg->workspace = sdsnew(edc_test_workspace());
    return cfg;
}

/* Wipe the data but keep the same workspace: close, delete the database files, reopen.
 *
 * This is what a test's clean_workspace() has to mean now. Deleting the DIRECTORY while
 * the store is open would pull the file out from under the connection (the store notices
 * and reopens an empty one, silently dropping everything written so far), and doing
 * nothing at all would leak rows from one sub-test into the next. */
static void edc_test_store_reset(void)
{
    if (!g_edc_test_ws[0])
        return;
    edc_state_store_close();
    char cmd[700];
    snprintf(cmd, sizeof(cmd), "rm -f '%s'/edgeclaw.db '%s'/edgeclaw.db-wal '%s'/edgeclaw.db-shm",
             g_edc_test_ws, g_edc_test_ws, g_edc_test_ws);
    if (system(cmd) != 0)
        fprintf(stderr, "warning: could not reset %s\n", g_edc_test_ws);

    struct edc_config tmp;
    memset(&tmp, 0, sizeof(tmp));
    tmp.workspace = sdsnew(g_edc_test_ws);
    if (!edc_state_store_open(&tmp))
        fprintf(stderr, "edc_test_store_reset: reopen failed\n");
    sdsfree(tmp.workspace);
}

/* Close the store and remove the directory (database, -wal and -shm included).
 * Pass the config the test owns, or NULL. */
static void edc_test_store_down(struct edc_config *cfg)
{
    edc_state_store_close();
    if (g_edc_test_ws[0])
    {
        char cmd[600];
        snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_edc_test_ws);
        if (system(cmd) != 0)
            fprintf(stderr, "warning: could not remove %s\n", g_edc_test_ws);
        g_edc_test_ws[0] = '\0';
    }
    if (cfg)
    {
        if (cfg->workspace)
            sdsfree(cfg->workspace);
        free(cfg);
    }
}

/* Fixture session ids live at the TOP of the band, descending (see edc_test_session_id):
 * an explicit insert raises SQLite's AUTOINCREMENT sequence to that value and its own
 * allocations then climb AWAY from the fixtures. Under 2^53 because ids travel through
 * cJSON, which stores numbers as `double`. */
#define EDC_TEST_SESSION_ID_BASE 9000000000000000LL

/* A turn with a usable storage key, so a test can add messages without threading the
 * session/group through. Tests that care about the real ids overwrite task_id (and
 * edc_task_add_turn rebinds the ordinal when the turn is queued). */
static struct edc_turn *edc_test_turn_new(void)
{
    /* Creating a turn is the earliest point a test can need the store: open it here so the
     * messages added right after have somewhere to go. */
    edc_test_workspace();
    /* A DISTINCT key per turn. Two turns sharing (task_id, turn_ord) would write to the
     * same rows and the second would silently overwrite the first -- which is what makes
     * a multi-turn test see "0 messages" instead of 4. */
    static int64_t seq = 100000;
    return edc_turn_new_for(EDC_TEST_SESSION_ID_BASE - 1, 90000 + (seq % 1000), ++seq, 0);
}

/* The ids a task/group carries are NUMBERS now (INTEGER PRIMARY KEYs), while fixtures name
 * things with strings -- "task-1", "parent", "L". This maps a name to a stable, small
 * positive id so a fixture keeps its identity across calls and a CHECK can compare
 * `t->task_id == edc_test_id("task-1")`.
 *
 * A table rather than a hash: a collision would make two different fixtures land on the
 * same row and fail somewhere far away from the cause. 512 distinct names per test binary
 * is far more than any test needs, and running out is loud (returns 0) rather than silent.
 *
 * Not thread safe, and does not need to be: each test binary is single-threaded here. */
#define EDC_TEST_ID_MAX 8192
static int64_t edc_test_id(const char *name)
{
    static char **names;
    static int n;
    if (!name || !*name)
        return 0;

    /* A purely numeric name IS the id: fixtures say "100" or "3" and mean that number, and
     * several assertions still spell it out in the expected value. */
    {
        char *end = NULL;
        const long long v = strtoll(name, &end, 10);
        if (end && *end == '\0' && v > 0)
            return (int64_t)v;
    }

    for (int i = 0; i < n; i++)
        if (strcmp(names[i], name) == 0)
            return (int64_t)i + 1000000; /* high range: never collides with a numeric name */
    if (!names)
        names = (char **)calloc(EDC_TEST_ID_MAX, sizeof(*names));
    if (n >= EDC_TEST_ID_MAX)
    {
        fprintf(stderr, "edc_test_id: more than %d distinct names\n", EDC_TEST_ID_MAX);
        abort();
    }
    names[n] = strdup(name);
    return (int64_t)(n++) + 1000000;
}

/* A stable session id for a fixture that names its sessions -- "sess-a", "test-sess".
 *
 * Session ids are numbers now (sessions.session_id is an INTEGER PRIMARY KEY AUTOINCREMENT),
 * so a fixture has to hand edc_session_new() a number. The values DESCEND from the top of the
 * band: a test that inserts one explicitly raises SQLite's AUTOINCREMENT sequence to that
 * value, and its own allocations then climb upward -- away from the fixtures rather than into
 * them (the same trap that made a fold's system task collide with a fixture group's id).
 *
 * The band stays under 2^53 because every id travels through cJSON, which stores numbers as
 * `double`; above 2^53 an integer is not exactly representable.
 *
 * Lookups by NAME ("sess-a", "dev") keep working untouched: edc_session_manager_find() falls
 * back to comparing the display name. */
static int64_t edc_test_session_id(const char *name)
{
    static char **names;
    static int n;
    if (!name || !*name)
        return 0;
    for (int i = 0; i < n; i++)
        if (strcmp(names[i], name) == 0)
            return EDC_TEST_SESSION_ID_BASE - (int64_t)i;
    if (!names)
        names = (char **)calloc(EDC_TEST_ID_MAX, sizeof(*names));
    if (n >= EDC_TEST_ID_MAX)
    {
        fprintf(stderr, "edc_test_session_id: more than %d distinct names\n", EDC_TEST_ID_MAX);
        abort();
    }
    names[n] = strdup(name);
    return EDC_TEST_SESSION_ID_BASE - (int64_t)(n++);
}

/* The TEXT form of a fixture session id, as a string that stays put.
 *
 * edc_id_str() is a rotating buffer, so two calls in one expression can alias -- and these
 * tests do compare ids inside a single CHECK. This returns a string allocated once per name,
 * stable for the life of the process, spelling the id the same single way everything else
 * does (the integer). */
static const char *edc_test_session_id_str(const char *name)
{
    static char *names[EDC_TEST_ID_MAX];
    static char *texts[EDC_TEST_ID_MAX];
    static int n;
    if (!name || !*name)
        return "";
    for (int i = 0; i < n; i++)
        if (strcmp(names[i], name) == 0)
            return texts[i];
    if (!names[0])
    {
        for (int i = 0; i < EDC_TEST_ID_MAX; i++)
        {
            names[i] = NULL;
            texts[i] = NULL;
        }
    }
    if (n >= EDC_TEST_ID_MAX)
    {
        fprintf(stderr, "edc_test_session_id_str: too many names\n");
        abort();
    }
    names[n] = strdup(name);
    texts[n] = sdscatprintf(sdsempty(), "%lld", (long long)edc_test_session_id(name));
    return texts[n++];
}

/* Tests build turns directly all over the place; this keeps them working now that a turn
 * needs a key to write its messages. */
#define edc_turn_new() edc_test_turn_new()

#endif /* EDC_TEST_WS_H */
