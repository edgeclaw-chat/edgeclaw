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
    static int seq;
    char id[64];
    snprintf(id, sizeof(id), "task-test-%d", ++seq);
    return edc_turn_new_for("sess-test", "g-test", id, 0);
}

/* Tests build turns directly all over the place; this keeps them working now that a turn
 * needs a key to write its messages. */
#define edc_turn_new() edc_test_turn_new()

#endif /* EDC_TEST_WS_H */
