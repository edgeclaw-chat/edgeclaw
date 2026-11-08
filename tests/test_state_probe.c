/* test_state_probe: the stage-0 persistence probe, in both states it can be in.
 *
 * WHY THIS FILE FORKS
 *
 * probe_do_init() runs under uv_once: the env var is read, the CSV opened and
 * g_enabled decided EXACTLY ONCE per process, and never again. So one process can
 * only ever exercise ONE of the probe's four startup outcomes:
 *
 *   no env var          -> disabled
 *   path too long       -> disabled (and it says so)
 *   path unopenable     -> disabled (and it says so)
 *   writable path       -> enabled
 *
 * Re-driving any of them in the same process is impossible by construction, so the
 * three disabled outcomes each get a forked child. The forks happen at the very top
 * of main(), before anything has touched the probe -- a child forked after the parent
 * initialised would inherit the finished uv_once and test the wrong outcome.
 *
 * The enabled path is the parent: CSV shape, bucket de-duplication, the "a -1 stays
 * out of the aggregate" rule, sample-array growth, percentiles, and the one-shot
 * summary. The bucket-table overflow (PROBE_MAX_BUCKETS) gets its own child, so it
 * does not depend on how many buckets the parent happened to create first.
 *
 * Everything is asserted against the FILES the probe writes: that is its entire output,
 * and a test that only checked "no crash" would pass with an empty CSV.
 */

#include "edc_state_probe.h"

#include "sds.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "test_fs_util.h"

static int g_passed = 0, g_failed = 0;

#define CHECK(c, m) do { \
    if (!(c)) { fprintf(stderr, "    FAIL: %s\n", m); g_failed++; } \
    else { printf("    ok: %s\n", m); g_passed++; } \
} while (0)

static char g_dir[512];

/* ── reading what the probe wrote ───────────────────────────────────────────── */

static char *read_whole(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0)
    {
        fclose(f);
        return NULL;
    }
    const long n = ftell(f);
    if (n < 0)
    {
        fclose(f);
        return NULL;
    }
    rewind(f);
    char *buf = calloc((size_t)n + 1, 1);
    if (!buf)
    {
        fclose(f);
        return NULL;
    }
    const size_t got = fread(buf, 1, (size_t)n, f);
    buf[got] = '\0';
    fclose(f);
    return buf;
}

/* Number of lines containing |needle| (for counting CSV rows of one kind). */
static int count_lines_with(const char *text, const char *needle)
{
    int n = 0;
    const size_t len = strlen(needle);
    for (const char *p = text; (p = strstr(p, needle)) != NULL; p += len)
        n++;
    return n;
}

/* The summary line that mentions |tag|. The summary is column-aligned text, not a
 * format meant for machines, so the line is located by its tag and then parsed by
 * field name -- never by counting spaces. Returns a malloc'd copy of that line. */
static char *summary_line(const char *summary, const char *tag)
{
    if (!summary)
        return NULL;
    const char *p = strstr(summary, tag);
    if (!p)
        return NULL;
    const char *ls = p;
    while (ls > summary && ls[-1] != '\n')
        ls--;
    const char *le = p;
    while (*le && *le != '\n')
        le++;
    const size_t n = (size_t)(le - ls);
    char *line = calloc(n + 1, 1);
    if (!line)
        return NULL;
    memcpy(line, ls, n);
    return line;
}

/* Value after "<key>=" on a summary line, as a double. */
static double field_double(const char *line, const char *key)
{
    if (!line)
        return -1.0;
    char pat[32];
    snprintf(pat, sizeof(pat), "%s=", key);
    const char *p = strstr(line, pat);
    if (!p)
        return -1.0;
    return strtod(p + strlen(pat), NULL);
}

static long long field_int(const char *line, const char *key)
{
    if (!line)
        return -1;
    char pat[32];
    snprintf(pat, sizeof(pat), "%s=", key);
    const char *p = strstr(line, pat);
    if (!p)
        return -1;
    return strtoll(p + strlen(pat), NULL, 10);
}

/* ── the forked children ────────────────────────────────────────────────────────
 * Each child sets its own env, runs, and returns 0 on success. Diagnostics go to
 * stderr (the test log); the exit code is what the parent asserts. */

static int child_disabled_no_env(void)
{
    unsetenv("EDGECLAW_STATE_PROBE");
    if (edc_state_probe_enabled())
    {
        fprintf(stderr, "child(no env): expected disabled\n");
        return 1;
    }
    /* The probes must be no-ops, not crashes, and must not invent a file. */
    edc_state_probe_save(1000, 2, 3, 4, 5, 6, true, "s1", "t");
    edc_state_probe_load(1000, 2, true, 3, 4, "s1", "t");
    edc_state_probe_summary(); /* documented no-op when disabled */
    char p[600];
    snprintf(p, sizeof(p), "%s/never_disabled.csv", g_dir);
    struct stat st;
    if (stat(p, &st) == 0)
    {
        fprintf(stderr, "child(no env): a CSV appeared while disabled\n");
        return 1;
    }
    if (edc_state_probe_now_ns() == 0)
    {
        fprintf(stderr, "child(no env): now_ns returned 0\n");
        return 1;
    }
    return 0;
}

static int child_disabled_path_too_long(void)
{
    /* g_path is 1024; anything at or over that is refused with a message rather than
     * truncated into a path that would be silently wrong. */
    char longpath[1400];
    memset(longpath, 'x', sizeof(longpath) - 1);
    longpath[0] = '/';
    longpath[sizeof(longpath) - 1] = '\0';
    setenv("EDGECLAW_STATE_PROBE", longpath, 1);
    if (edc_state_probe_enabled())
    {
        fprintf(stderr, "child(long path): expected disabled\n");
        return 1;
    }
    return 0;
}

static int child_disabled_unopenable(void)
{
    char p[700];
    snprintf(p, sizeof(p), "%s/no_such_dir/probe.csv", g_dir);
    setenv("EDGECLAW_STATE_PROBE", p, 1);
    if (edc_state_probe_enabled())
    {
        fprintf(stderr, "child(bad path): expected disabled\n");
        return 1;
    }
    /* And it cleared the path, so a later summary() does not try to write one. */
    edc_state_probe_summary();
    char sp[720];
    snprintf(sp, sizeof(sp), "%s.summary", p);
    struct stat st;
    if (stat(sp, &st) == 0)
    {
        fprintf(stderr, "child(bad path): a summary appeared for an unusable path\n");
        return 1;
    }
    return 0;
}

/* PROBE_MAX_BUCKETS is 32: the 33rd distinct tag gets no bucket (the CSV row is still
 * written -- losing the aggregate is acceptable, losing the raw sample is not). Its own
 * child so the count is exact and independent of the parent's activity. */
static int child_bucket_table_full(void)
{
    char p[600], sp[620];
    snprintf(p, sizeof(p), "%s/overflow.csv", g_dir);
    snprintf(sp, sizeof(sp), "%s.summary", p);
    setenv("EDGECLAW_STATE_PROBE", p, 1);
    if (!edc_state_probe_enabled())
    {
        fprintf(stderr, "child(overflow): expected enabled\n");
        return 1;
    }
    for (int i = 1; i <= 33; i++)
    {
        char tag[16];
        snprintf(tag, sizeof(tag), "OVF%02d", i);
        edc_state_probe_save(1000000ULL, 10, 10, 1, 1, 1, true, "s", tag);
    }
    edc_state_probe_summary();

    char *summary = read_whole(sp);
    char *csv = read_whole(p);
    if (!summary || !csv)
    {
        fprintf(stderr, "child(overflow): missing output\n");
        free(summary);
        free(csv);
        return 1;
    }
    int rows = 0;
    for (const char *q = summary; *q; q++)
        if (*q == '\n' && (q[1] == 's' || q[1] == 'l')) /* "save"/"load" at line start */
            rows++;
    const int csv_rows = count_lines_with(csv, "OVF");
    const int rc = (rows == 32 && csv_rows == 33) ? 0 : 1;
    if (rc)
        fprintf(stderr, "child(overflow): summary rows=%d (want 32), csv rows=%d (want 33)\n", rows, csv_rows);
    free(summary);
    free(csv);
    return rc;
}

/* The summary cannot be created (its directory is read-only). This is the one failure a
 * user can actually cause -- EDGECLAW_STATE_PROBE pointing at a directory that is fine
 * for the CSV but not for the summary -- and it must not crash, nor claim the summary
 * was written. Also proves g_summarized is only set on success, so a retry works. */
static int child_summary_unwritable(void)
{
    char dir[600], csv[640], sum[680];
    snprintf(dir, sizeof(dir), "%s/ro", g_dir);
    snprintf(csv, sizeof(csv), "%s/probe.csv", dir);
    snprintf(sum, sizeof(sum), "%s.summary", csv);
    if (mkdir(dir, 0755) != 0)
        return 1;
    setenv("EDGECLAW_STATE_PROBE", csv, 1);
    if (!edc_state_probe_enabled())
    {
        fprintf(stderr, "child(ro dir): expected enabled\n");
        return 1;
    }
    edc_state_probe_save(1000000ULL, 1, 1, 1, 1, 1, true, "s", "RO");

    if (chmod(dir, 0555) != 0)
    {
        fprintf(stderr, "child(ro dir): cannot make the directory read-only\n");
        return 1;
    }
    edc_state_probe_summary(); /* must report and return, not crash */
    struct stat st;
    const int wrote_anyway = (stat(sum, &st) == 0);

    if (chmod(dir, 0755) != 0)
        return 1;
    edc_state_probe_summary(); /* now it must work: the failure did not set the one-shot */
    const int wrote_after = (stat(sum, &st) == 0);

    if (wrote_anyway)
    {
        fprintf(stderr, "child(ro dir): a summary appeared in a read-only directory\n");
        return 1;
    }
    if (!wrote_after)
    {
        fprintf(stderr, "child(ro dir): the retry after restoring permissions did not write\n");
        return 1;
    }
    return 0;
}

static int run_child(const char *what, int (*fn)(void))
{
    fflush(NULL);
    const pid_t pid = fork();
    if (pid < 0)
    {
        fprintf(stderr, "    (fork failed for %s)\n", what);
        return 0;
    }
    if (pid == 0)
    {
        const int rc = fn();
        fflush(NULL);
        /* exit(), not _exit(): gcov writes its counters from an atexit handler, and
         * _exit() skips those -- the child's coverage of the disabled branches would be
         * thrown away even though those lines ran. (The forks happen before the parent
         * touches the probe, so the counters are still zero at fork time and the child's
         * dump cannot double-count the parent's work.) */
        exit(rc);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0)
        return 0;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

/* ── the enabled path (this process) ─────────────────────────────────────────── */

static void test_enabled_basics(const char *csv_path)
{
    printf("\n── enabled: the CSV ──\n");
    CHECK(edc_state_probe_enabled(), "a writable path enables the probe");

    const uint64_t t1 = edc_state_probe_now_ns();
    const uint64_t t2 = edc_state_probe_now_ns();
    CHECK(t1 != 0 && t2 >= t1, "now_ns is monotonic and non-zero");

    /* A save with every field distinguishable, and no session id at all. */
    edc_state_probe_save(2500000ULL /* 2.5ms */, 4096, 8192, 12, 3, 30, true, NULL, "ZSAVE");
    /* A load, which has no document/io columns. */
    edc_state_probe_load(1500000ULL /* 1.5ms */, 77, true, 4, 70, "", "ZLOAD");

    char *csv = read_whole(csv_path);
    CHECK(csv != NULL, "the CSV exists");
    if (!csv)
        return;

    CHECK(strncmp(csv, "ts_ms,kind,tag,session,dur_ms,doc_bytes,io_bytes,rows,n_tasks,n_msgs,ok",
                  strlen("ts_ms,kind,tag,session,dur_ms,doc_bytes,io_bytes,rows,n_tasks,n_msgs,ok")) == 0,
          "the header names every column in order");
    CHECK(count_lines_with(csv, "ts_ms,kind,tag,") == 1, "the header is written exactly once");

    /* save row: ts,save,tag,sid,dur,doc,io,rows,tasks,msgs,ok */
    char *save_row = strstr(csv, ",save,ZSAVE,");
    CHECK(save_row != NULL, "the save row is present");
    if (save_row)
    {
        CHECK(strstr(save_row, ",save,ZSAVE,?,") == save_row,
              "a NULL session id is recorded as `?`");
        CHECK(strstr(save_row, ",2.500,4096,8192,12,3,30,1") != NULL,
              "the save row carries dur_ms, doc, io, rows, tasks, msgs and ok=1");
    }

    char *load_row = strstr(csv, ",load,ZLOAD,");
    CHECK(load_row != NULL, "the load row is present");
    if (load_row)
    {
        /* An empty (not NULL) session id is also `?`. */
        CHECK(strstr(load_row, ",load,ZLOAD,?,") == load_row, "an empty session id is recorded as `?` too");
        CHECK(strstr(load_row, ",1.500,-,-,77,4,70,1") != NULL,
              "the load row leaves doc/io as `-` and carries rows, tasks, msgs, ok");
    }
    free(csv);
}

static void test_bucket_dedup_and_negative_rule(void)
{
    printf("\n── enabled: de-duplication and the `-1` rule ──\n");
    /* Same kind+tag twice: one bucket, two samples. */
    edc_state_probe_save(1000000ULL, 100, 100, 1, 1, 1, true, "s", "ZDUP");
    edc_state_probe_save(3000000ULL, 100, 100, 1, 1, 1, true, "s", "ZDUP");

    /* Every "not measurable" field is -1: it stays in the CSV row but must not touch
     * the aggregate, or one unmeasurable sample would poison the average. */
    edc_state_probe_save(1000000ULL, 0, -1, -1, -1, -1, false, "s", "ZNEG");
}

static void test_percentiles(const char *csv_path)
{
    printf("\n── enabled: percentiles ──\n");
    /* Ten samples, 1..10 ms: mean 5.5, p50 = 5th (nearest rank), p95 = 10th, max 10. */
    for (int i = 1; i <= 10; i++)
        edc_state_probe_save((uint64_t)i * 1000000ULL, 10, 10, 1, 1, 1, true, "s", "ZPCT");
    (void)csv_path;
}

static void test_sample_growth(void)
{
    printf("\n── enabled: the sample array grows ──\n");
    /* cap_samples starts at 0 and doubles (128, 256, 512...), so 513 samples force two
     * reallocs. The count must stay exact across them. */
    for (int i = 0; i < 513; i++)
        edc_state_probe_save(100000ULL, 10, 10, 1, 1, 1, true, "s", "ZGROW");
}

/* PROBE_MAX_SAMPLES is 200000: past that the raw samples stop being kept (a long-running
 * probe would otherwise grow without bound), but the COUNT must stay exact -- otherwise
 * the mean would quietly start describing only the first 200k calls. Measured: 200001
 * calls take ~0.1s and produce a 6.7 MB CSV, so exercising this is cheap; it is not a
 * theoretical guard, a probe left on for days reaches it. */
static void test_sample_cap(void)
{
    printf("\n── enabled: the sample cap ──\n");
    for (int i = 0; i < 200001; i++)
        edc_state_probe_save(100000ULL, 10, 10, 1, 1, 1, true, "s", "ZCAP");
}

static void test_summary(const char *csv_path)
{
    printf("\n── enabled: the summary ──\n");
    char sum_path[620];
    snprintf(sum_path, sizeof(sum_path), "%s.summary", csv_path);

    edc_state_probe_summary();

    char *s = read_whole(sum_path);
    CHECK(s != NULL, "the summary file was written");
    if (!s)
        return;

    CHECK(strstr(s, "# source CSV:") != NULL, "it names the CSV it summarizes");
    CHECK(strstr(s, "doc_MB_per_call") != NULL, "it explains the columns it prints");
    CHECK(strstr(s, "Total time inside save+load") != NULL, "and closes with the grand total");

    /* De-duplication: one line for ZDUP with n=2 and the 1ms/3ms samples. */
    char *dup = summary_line(s, "ZDUP");
    CHECK(dup != NULL, "the duplicated tag has a bucket");
    CHECK(field_int(dup, "n") == 2, "and both calls landed in it (n=2)");
    CHECK(field_double(dup, "mean") > 1.99 && field_double(dup, "mean") < 2.01,
          "mean is 2.00 ms for samples of 1 and 3 ms");
    CHECK(field_double(dup, "max") > 2.99 && field_double(dup, "max") < 3.01, "max is the larger sample (3 ms)");
    free(dup);

    /* The -1 rule: io and rows were -1, so every aggregate stays 0. */
    char *neg = summary_line(s, "ZNEG");
    CHECK(neg != NULL, "the all-(-1) sample has a bucket");
    CHECK(field_double(neg, "io_MB_total") == 0.0, "a -1 io value does not enter io_MB_total");
    CHECK(field_double(neg, "rows_per_call") == 0.0, "a -1 row count does not enter rows_per_call");
    CHECK(field_double(neg, "avg_tasks") == 0.0, "a -1 task count does not enter avg_tasks");
    CHECK(field_double(neg, "avg_msgs") == 0.0, "a -1 message count does not enter avg_msgs");
    CHECK(field_int(neg, "n") == 1, "but the call itself is still counted");
    free(neg);

    /* Percentiles: 1..10 ms -> p50 5, p95 10, max 10. */
    char *pct = summary_line(s, "ZPCT");
    CHECK(pct != NULL, "the percentile bucket exists");
    CHECK(field_int(pct, "n") == 10, "with ten samples");
    CHECK(field_double(pct, "p50") > 4.99 && field_double(pct, "p50") < 5.01, "p50 is 5 ms (nearest rank)");
    CHECK(field_double(pct, "p95") > 9.99 && field_double(pct, "p95") < 10.01, "p95 is 10 ms");
    CHECK(field_double(pct, "max") > 9.99 && field_double(pct, "max") < 10.01, "max is 10 ms");
    CHECK(field_double(pct, "mean") > 5.49 && field_double(pct, "mean") < 5.51, "mean is 5.5 ms");
    CHECK(strstr(pct, "[dropped") == NULL, "no samples were dropped");
    free(pct);

    /* Growth: 513 samples kept, count exact. */
    char *grow = summary_line(s, "ZGROW");
    CHECK(grow != NULL, "the growth bucket exists");
    CHECK(field_int(grow, "n") == 513, "all 513 samples were kept through two reallocs");
    CHECK(strstr(grow, "[dropped") == NULL, "and none were dropped");
    free(grow);

    /* The cap: 200001 calls, 200000 samples kept, count exact, one dropped and said so. */
    char *cap = summary_line(s, "ZCAP");
    CHECK(cap != NULL, "the capped bucket exists");
    CHECK(field_int(cap, "n") == 200001, "the COUNT is exact even past the sample cap");
    CHECK(cap != NULL && strstr(cap, "[dropped 1 samples]") != NULL,
          "and the dropped sample is reported rather than hidden");
    CHECK(field_double(cap, "p95") > 0.09 && field_double(cap, "p95") < 0.11,
          "percentiles are still computed from the 200000 samples that were kept");
    free(cap);

    /* The load row prints rows_per_call but no save-only columns. */
    char *ld = summary_line(s, "ZLOAD");
    CHECK(ld != NULL && strncmp(ld, "load", 4) == 0, "the load bucket is labelled load");
    CHECK(ld != NULL && strstr(ld, "doc_MB_per_call") == NULL, "and carries no save-only columns");
    free(ld);

    /* One-shot: the file must not be rewritten by a second call. */
    {
        FILE *f = fopen(sum_path, "w");
        CHECK(f != NULL, "the summary can be replaced for the one-shot check");
        if (f)
        {
            fputs("SENTINEL\n", f);
            fclose(f);
        }
        edc_state_probe_summary(); /* second call: must be a no-op */
        char *again = read_whole(sum_path);
        CHECK(again != NULL && strcmp(again, "SENTINEL\n") == 0,
              "a second summary() call does not rewrite the file");
        free(again);
    }

    free(s);
}

int main(void)
{
    const char *tmp = getenv("TEST_TMPDIR");
    if (!tmp || !*tmp)
        tmp = getenv("TMPDIR");
    snprintf(g_dir, sizeof(g_dir), "%s/edc_test_state_probe_%ld", (tmp && *tmp) ? tmp : "/tmp",
             (long)getpid());
    char cmd[1400];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s' && mkdir -p '%s'", g_dir, g_dir);
    if (test_sh(cmd) != 0)
        fprintf(stderr, "    (warning: could not create %s)\n", g_dir);

    printf("workspace: %s\n", g_dir);

    /* ── the three disabled outcomes, one forked child each ──
     * Forked BEFORE this process touches the probe: uv_once has not run yet, so each
     * child starts from an uninitialised state and can decide for itself. */
    printf("\n── disabled: no env var ──\n");
    CHECK(run_child("no env", child_disabled_no_env), "with no env var the probe stays disabled and inert");
    printf("\n── disabled: path too long ──\n");
    CHECK(run_child("long path", child_disabled_path_too_long), "an over-long path is refused, not truncated");
    printf("\n── disabled: unopenable path ──\n");
    CHECK(run_child("bad path", child_disabled_unopenable), "an unopenable path is refused and no summary is written");

    printf("\n── enabled: bucket table full ──\n");
    CHECK(run_child("overflow", child_bucket_table_full),
          "the 33rd distinct tag gets no bucket, but its CSV row is still written");

    printf("\n── enabled: the summary cannot be created ──\n");
    CHECK(run_child("ro dir", child_summary_unwritable),
          "a summary that cannot be opened is reported, not faked, and the retry works");

    /* ── the enabled path, in this process ── */
    char csv_path[600];
    snprintf(csv_path, sizeof(csv_path), "%s/probe.csv", g_dir);
    setenv("EDGECLAW_STATE_PROBE", csv_path, 1);

    test_enabled_basics(csv_path);
    test_bucket_dedup_and_negative_rule();
    test_percentiles(csv_path);
    test_sample_growth();
    test_sample_cap();
    test_summary(csv_path);

    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_dir);
    if (test_sh(cmd) != 0)
        fprintf(stderr, "(warning: could not clean %s)\n", g_dir);

    printf("\n=== %d passed, %d failed ===\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
