#include "edc_state_probe.h"

#include "log.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>

/* ── Why this file exists ────────────────────────────────────────────────────
 * Baseline numbers for the SQLite migration. The design doc asks for
 * "log_debug: duration, bytes, call site" — but edc_log_init() registers the file
 * callback at LOG_INFO outside debug builds, so log_debug() is compiled away
 * precisely in the build we must time. Rather than ship the numbers from a slow
 * debug build, this probe is gated by EDGECLAW_STATE_PROBE (env) and always
 * compiled, mirroring the existing EDGECLAW_SNAPSHOT_TRACE pattern.
 *
 * Cost when disabled: one uv_once + one bool test per call.
 *
 * Two numbers here were WRONG in the first cut and are worth remembering:
 *  - dur_ms included the probe's own second serialization, because the caller
 *    stopped its clock after producing doc_bytes. 268 bytes read as 0.32 ms.
 *  - written_bytes was set to doc_bytes by the caller whenever the flush returned
 *    ok, so "write amplification" was a restatement of the document size. It is
 *    now what the store actually appended to the WAL.
 * The probe cannot enforce either rule; the caller must (see edc_session.c).
 */

#define PROBE_MAX_BUCKETS 32
#define PROBE_MAX_SAMPLES 200000

struct probe_bucket
{
    const char *kind; /* "save" | "load": static literals, compared by pointer */
    char tag[48];
    uint64_t count;
    uint64_t total_ns;
    uint64_t max_ns;
    uint64_t total_doc;    /* save: snapshot bytes; load: rows produced */
    uint64_t total_io;     /* save: bytes the pager wrote (negatives dropped) */
    uint64_t total_rows;   /* save: rows inserted/deleted (negatives dropped) */
    uint64_t total_tasks;  /* tree size: loaded (load) / flushed (save) */
    uint64_t total_msgs;
    uint64_t *samples; /* durations in ns, for percentiles */
    size_t n_samples;
    size_t cap_samples;
    uint64_t dropped; /* samples beyond PROBE_MAX_SAMPLES (count still exact) */
};

static uv_once_t g_once = UV_ONCE_INIT;
static uv_mutex_t g_mutex;
static bool g_enabled;
static bool g_summarized;
static char g_path[1024];
static FILE *g_csv;
static uint64_t g_t0;

static struct probe_bucket g_buckets[PROBE_MAX_BUCKETS];
static int g_n_buckets;

static void probe_do_init(void)
{
    uv_mutex_init(&g_mutex);
    g_t0 = uv_hrtime();

    const char *path = getenv("EDGECLAW_STATE_PROBE");
    if (!path || !*path)
        return;

    if (strlen(path) >= sizeof(g_path))
    {
        fprintf(stderr, "state_probe: path too long, disabled\n");
        return;
    }
    memcpy(g_path, path, strlen(path) + 1);

    g_csv = fopen(g_path, "w");
    if (!g_csv)
    {
        fprintf(stderr, "state_probe: cannot open %s: %s\n", g_path, strerror(errno));
        g_path[0] = '\0';
        return;
    }
    fprintf(g_csv, "ts_ms,kind,tag,session,dur_ms,doc_bytes,io_bytes,rows,n_tasks,n_msgs,ok\n");
    fflush(g_csv);
    g_enabled = true;
}

void edc_state_probe_init(void)
{
    uv_once(&g_once, probe_do_init);
}

bool edc_state_probe_enabled(void)
{
    edc_state_probe_init();
    return g_enabled;
}

uint64_t edc_state_probe_now_ns(void)
{
    return uv_hrtime();
}

/* Caller holds g_mutex. */
static struct probe_bucket *probe_bucket(const char *kind, const char *tag)
{
    const char *t = tag ? tag : "?";
    for (int i = 0; i < g_n_buckets; i++)
    {
        if (g_buckets[i].kind == kind && strcmp(g_buckets[i].tag, t) == 0)
            return &g_buckets[i];
    }
    if (g_n_buckets >= PROBE_MAX_BUCKETS)
        return NULL; /* bucket table full: the CSV still has the row */

    struct probe_bucket *b = &g_buckets[g_n_buckets++];
    memset(b, 0, sizeof(*b));
    b->kind = kind;
    snprintf(b->tag, sizeof(b->tag), "%s", t);
    return b;
}

/* Caller holds g_mutex. */
static void probe_keep_sample(struct probe_bucket *b, uint64_t dur_ns)
{
    if (b->n_samples >= PROBE_MAX_SAMPLES)
    {
        b->dropped++;
        return;
    }
    if (b->n_samples == b->cap_samples)
    {
        size_t ncap = b->cap_samples ? b->cap_samples * 2 : 128;
        if (ncap > PROBE_MAX_SAMPLES)
            ncap = PROBE_MAX_SAMPLES;
        uint64_t *grown = realloc(b->samples, ncap * sizeof(*grown));
        if (!grown)
        {
            b->dropped++;
            return;
        }
        b->samples = grown;
        b->cap_samples = ncap;
    }
    b->samples[b->n_samples++] = dur_ns;
}

/* Caller holds g_mutex. A "not measurable" -1 is kept out of the sums but stays in
 * the CSV row, so one unmeasurable sample cannot poison the aggregate. */
static void probe_tally(const char *kind, const char *tag, uint64_t dur_ns, uint64_t doc_bytes,
                        long long io_bytes, long long rows, long n_tasks, long n_msgs)
{
    struct probe_bucket *b = probe_bucket(kind, tag);
    if (!b)
        return;
    b->count++;
    b->total_ns += dur_ns;
    if (dur_ns > b->max_ns)
        b->max_ns = dur_ns;
    b->total_doc += doc_bytes;
    if (io_bytes > 0)
        b->total_io += (uint64_t)io_bytes;
    if (rows > 0)
        b->total_rows += (uint64_t)rows;
    if (n_tasks > 0)
        b->total_tasks += (uint64_t)n_tasks;
    if (n_msgs > 0)
        b->total_msgs += (uint64_t)n_msgs;
    probe_keep_sample(b, dur_ns);
}

static const char *probe_sid(const char *session_id)
{
    return (session_id && *session_id) ? session_id : "?";
}

void edc_state_probe_save(uint64_t dur_ns, size_t doc_bytes, long long io_bytes, long long rows,
                          long n_tasks, long n_msgs, bool ok, const char *session_id,
                          const char *tag)
{
    edc_state_probe_init();
    if (!g_enabled)
        return;

    uv_mutex_lock(&g_mutex);
    probe_tally("save", tag, dur_ns, doc_bytes, io_bytes, rows, n_tasks, n_msgs);
    if (g_csv)
    {
        fprintf(g_csv, "%llu,save,%s,%s,%.3f,%zu,%lld,%lld,%ld,%ld,%d\n",
                (unsigned long long)((uv_hrtime() - g_t0) / 1000000ULL), tag ? tag : "?",
                probe_sid(session_id), (double)dur_ns / 1e6, doc_bytes, io_bytes, rows,
                n_tasks, n_msgs, ok ? 1 : 0);
        fflush(g_csv);
    }
    uv_mutex_unlock(&g_mutex);
}

void edc_state_probe_load(uint64_t dur_ns, long long rows, bool ok, long n_tasks, long n_msgs,
                          const char *session_id, const char *tag)
{
    edc_state_probe_init();
    if (!g_enabled)
        return;

    uv_mutex_lock(&g_mutex);
    probe_tally("load", tag, dur_ns, (uint64_t)(rows > 0 ? rows : 0), -1, rows, n_tasks, n_msgs);
    if (g_csv)
    {
        fprintf(g_csv, "%llu,load,%s,%s,%.3f,-,-,%lld,%ld,%ld,%d\n",
                (unsigned long long)((uv_hrtime() - g_t0) / 1000000ULL), tag ? tag : "?",
                probe_sid(session_id), (double)dur_ns / 1e6, rows, n_tasks, n_msgs, ok ? 1 : 0);
        fflush(g_csv);
    }
    uv_mutex_unlock(&g_mutex);
}

static int probe_cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a;
    uint64_t y = *(const uint64_t *)b;
    return (x < y) ? -1 : (x > y) ? 1 : 0;
}

/* Nearest-rank percentile over an ascending array. */
static double probe_pct(const uint64_t *sorted, size_t n, double p)
{
    if (!sorted || n == 0)
        return -1.0; /* not computable */
    size_t rank = (size_t)(p * (double)n + 0.999);
    if (rank < 1)
        rank = 1;
    if (rank > n)
        rank = n;
    return (double)sorted[rank - 1] / 1e6; /* ms */
}

static void probe_print_row(FILE *out, struct probe_bucket *b)
{
    uint64_t *sorted = NULL;
    if (b->n_samples > 1)
    {
        sorted = malloc(b->n_samples * sizeof(*sorted));
        if (sorted)
        {
            memcpy(sorted, b->samples, b->n_samples * sizeof(*sorted));
            qsort(sorted, b->n_samples, sizeof(*sorted), probe_cmp_u64);
        }
    }
    else
    {
        sorted = b->samples; /* 0 or 1 element: no copy needed */
    }

    const double mean = b->count ? (double)b->total_ns / (double)b->count / 1e6 : 0.0;

    fprintf(out, "%-5s %-22s n=%-6llu mean=%9.2fms p50=%8.2fms p95=%8.2fms max=%9.2fms",
            b->kind, b->tag, (unsigned long long)b->count, mean,
            probe_pct(sorted, b->n_samples, 0.50), probe_pct(sorted, b->n_samples, 0.95),
            (double)b->max_ns / 1e6);

    if (strcmp(b->kind, "save") == 0)
        fprintf(out,
                " doc_MB_per_call=%8.3f io_MB_total=%9.2f rows_per_call=%8.1f avg_tasks=%7.1f "
                "avg_msgs=%8.1f",
                b->count ? (double)b->total_doc / (double)b->count / (1024.0 * 1024.0) : 0.0,
                (double)b->total_io / (1024.0 * 1024.0),
                b->count ? (double)b->total_rows / (double)b->count : 0.0,
                b->count ? (double)b->total_tasks / (double)b->count : 0.0,
                b->count ? (double)b->total_msgs / (double)b->count : 0.0);
    else if (b->count)
        fprintf(out, " rows_per_call=%8.1f avg_tasks=%7.1f avg_msgs=%8.1f",
                (double)b->total_rows / (double)b->count, (double)b->total_tasks / (double)b->count,
                (double)b->total_msgs / (double)b->count);

    if (b->dropped)
        fprintf(out, " [dropped %llu samples]", (unsigned long long)b->dropped);
    fprintf(out, "\n");

    if (sorted && sorted != b->samples)
        free(sorted);
}

void edc_state_probe_summary(void)
{
    edc_state_probe_init();
    if (!g_enabled || g_summarized)
        return;

    char path[1100];
    snprintf(path, sizeof(path), "%s.summary", g_path);
    FILE *out = fopen(path, "w");
    if (!out)
    {
        fprintf(stderr, "state_probe: cannot open %s: %s\n", path, strerror(errno));
        return;
    }
    g_summarized = true;

    fprintf(out, "# session state persistence baseline (stage 0 of the SQLite migration)\n");
    fprintf(out, "# source CSV: %s\n", g_path);
    fprintf(out, "#\n");
    fprintf(out, "# mean/p50/p95/max  wall time of the session save_state / load_state call.\n");
    fprintf(out, "#                   Excludes the probe's own work (the snapshot is\n");
    fprintf(out, "#                   serialized a second time for doc_bytes, after timing stops).\n");
    fprintf(out, "# doc_MB_per_call   snapshot size of one flush: the O(history) it rewrites.\n");
    fprintf(out, "# io_MB_total       bytes the pager wrote (WAL frames) -- the REAL write\n");
    fprintf(out, "#                   amplification, NOT derived from doc_MB_per_call. Measured with\n");
    fprintf(out, "#                   SQLITE_DBSTATUS_CACHE_WRITE, not by watching the -wal file grow:\n");
    fprintf(out, "#                   after an auto-checkpoint the WAL is reused, so its length stays\n");
    fprintf(out, "#                   flat while whole snapshots are still being appended to it.\n");
    fprintf(out, "# rows_per_call     rows inserted or deleted by one flush (FK cascades included),\n");
    fprintf(out, "#                   or rows the loader produced, for load rows.\n");
    fprintf(out, "# load rows are ATTEMPTS: ok=0 with rows_per_call=0 means the store held nothing\n");
    fprintf(out, "#   for that session (brand-new), not that the load failed.\n");
    fprintf(out, "#\n");

    double grand_ns = 0.0;
    uint64_t grand_io = 0;

    for (int i = 0; i < g_n_buckets; i++)
    {
        struct probe_bucket *b = &g_buckets[i];
        probe_print_row(out, b);
        grand_ns += (double)b->total_ns;
        grand_io += b->total_io;
    }

    fprintf(out, "#\n");
    fprintf(out, "# Total time inside save+load: %.1f ms\n", grand_ns / 1e6);
    fprintf(out, "# Total bytes written by the pager: %.2f MB\n", (double)grand_io / (1024.0 * 1024.0));
    fprintf(out, "#\n");
    fprintf(out, "# Reading the CSV: ts_ms is monotonic ms since probe init. Rows sharing a\n");
    fprintf(out, "# ts_ms (or within a few ms) belong to ONE tool boundary -- that count is what\n");
    fprintf(out, "# decides whether the SYNC/DEFERRED merge window in stage 2 has to do the work.\n");

    fclose(out);

    log_warn("state_probe: summary written to %s", path);
}
