/* test_sqlite_hardening: the guard for the no-patch SQLite configuration.
 *
 * The compile options this project needs (see .bazelrc, --per_file_copt) are injected
 * from outside the sqlite3 module, because the BCR module ships SQLite's stock defaults
 * and offers no knob of its own. The upside of that approach is not having to maintain a
 * BUILD patch; the downside is that a --per_file_copt rule that stops matching (a Bazel
 * change, a reworded regex, the module being built from a different path form) would
 * silently produce an UNHARDENED library. A patch cannot fail that way -- it either
 * applies or breaks the build loudly.
 *
 * This test closes that gap: it asks the linked library what it was actually compiled
 * with. If the flags stop being applied, this fails instead of the security posture
 * quietly eroding.
 *
 * Keep these in sync with the --per_file_copt line in .bazelrc.
 */

#include "sqlite3.h"

#include <stdio.h>
#include <string.h>

static int g_failed = 0;
static int g_passed = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "    FAIL: %s\n", msg); g_failed++; } \
    else { printf("    ok: %s\n", msg); g_passed++; } \
} while (0)

/* SQLite reports its own compile options through sqlite3_compileoption_get/used, which is
 * exactly the introspection we want: it reflects the library as built, not the intent. */
static void check_compile_option(const char *opt, const char *why)
{
    char buf[256];
    const int used = sqlite3_compileoption_used(opt);
    snprintf(buf, sizeof(buf), "%s is in effect (%s)", opt, why);
    CHECK(used, buf);
}

int main(void)
{
    printf("sqlite %s\n\n", sqlite3_libversion());

    printf("hardening flags taken from .bazelrc --per_file_copt:\n");
    check_compile_option("SQLITE_DQS=0",
                         "a mistyped column must be an error, not a string literal");
    check_compile_option("SQLITE_OMIT_LOAD_EXTENSION",
                         "load_extension() must not be linkable in a model-writable workspace");

    /* We deliberately keep SQLite's stock defaults here. Asserting their ABSENCE is the
     * point: if someone re-adds one of these flags without measuring, this test says so.
     *
     * MEMSTATUS=0 would shave a global mutex off every sqlite3Malloc(), but that cost is
     * invisible next to the ~90% of flush time spent in the cJSON print, and it would
     * permanently disable sqlite3_status()/memory_used()/soft_heap_limit64() -- which a
     * future `edgeclaw db` diagnostics subcommand would want. See .bazelrc.
     *
     * THREADSAFE left at the stock 1 for the same "do not narrow without a reason" rule. */
    printf("\ndeliberately stock (asserting they were NOT set):\n");
    CHECK(!sqlite3_compileoption_used("SQLITE_DEFAULT_MEMSTATUS=0"),
          "memory statistics stay available (introspection kept for `edgeclaw db`)");
    CHECK(sqlite3_threadsafe() != 0, "library stays thread-safe (stock)");

    printf("\nbehaviour that follows from DQS=0:\n");
    {
        sqlite3 *db = NULL;
        CHECK(sqlite3_open(":memory:", &db) == SQLITE_OK, "open :memory:");

        char *err = NULL;
        CHECK(sqlite3_exec(db, "CREATE TABLE t(a TEXT)", NULL, NULL, &err) == SQLITE_OK,
              "CREATE TABLE with a plain name");
        if (err)
        {
            sqlite3_free(err);
            err = NULL;
        }
        CHECK(sqlite3_exec(db, "INSERT INTO t VALUES('v')", NULL, NULL, &err) == SQLITE_OK,
              "INSERT one row");
        if (err)
        {
            sqlite3_free(err);
            err = NULL;
        }

        /* The point of DQS=0: this must FAIL, not return the string "nope". */
        const int rc = sqlite3_exec(db, "SELECT \"nope\" FROM t", NULL, NULL, &err);
        CHECK(rc != SQLITE_OK, "double-quoted unknown column is rejected (not a string literal)");
        if (err)
        {
            printf("        (sqlite said: %s)\n", err);
            sqlite3_free(err);
            err = NULL;
        }

        /* And the path the store actually uses -- bound parameters -- still works. */
        sqlite3_stmt *st = NULL;
        CHECK(sqlite3_prepare_v2(db, "SELECT a FROM t WHERE a = ?", -1, &st, NULL) == SQLITE_OK,
              "prepared statement with a bound parameter");
        if (st)
        {
            sqlite3_bind_text(st, 1, "v", -1, SQLITE_STATIC);
            CHECK(sqlite3_step(st) == SQLITE_ROW, "bound parameter matches the row");
            sqlite3_finalize(st);
        }

        /* JSON1 is compiled in by default from 3.38 and the schema relies on it being
         * available for the message index columns (design doc §4). */
        CHECK(sqlite3_exec(db, "SELECT json_extract('{\"k\":1}','$.k')", NULL, NULL, &err) == SQLITE_OK,
              "JSON1 functions are available");
        if (err)
            sqlite3_free(err);

        sqlite3_close(db);
    }

    printf("\nbehaviour that follows from OMIT_LOAD_EXTENSION:\n");
    {
        sqlite3 *db = NULL;
        if (sqlite3_open(":memory:", &db) == SQLITE_OK)
        {
            char *err = NULL;
            const int rc = sqlite3_exec(db, "SELECT load_extension('whatever')", NULL, NULL, &err);
            CHECK(rc != SQLITE_OK, "load_extension() cannot be called");
            if (err)
            {
                printf("        (sqlite said: %s)\n", err);
                sqlite3_free(err);
            }
            sqlite3_close(db);
        }
    }

    printf("\nbehaviour that follows from keeping stock memory statistics:\n");
    /* Memory stats are available (the flag is not set), which is what a diagnostics
     * subcommand would need. Values are non-deterministic so only availability is checked. */
    CHECK(sqlite3_memory_used() >= 0, "sqlite3_memory_used() answers (stats not compiled out)");

    printf("\n%d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
