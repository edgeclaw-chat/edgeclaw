/* Deterministic SQLite memory A/B probe (NOT a project test -- an experiment).
 *
 * Opens a copy of the real workspace DB with one of four PRAGMA combinations,
 * forces a full scan of messages.body (the same read path the runtime uses),
 * then reports this process's RSS / THP / db-mmap residency. One fork per
 * combination so allocator state from one run cannot leak into the next.
 *
 * This isolates the SQLite contribution to edgeclaw's ~472 MiB RSS without
 * needing the TUI to start. */
#include "sqlite3.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static long status_kb(const char *key)
{
    FILE *f = fopen("/proc/self/status", "r");
    if (!f)
        return -1;
    char line[256];
    long val = -1;
    while (fgets(line, sizeof(line), f))
    {
        if (strncmp(line, key, strlen(key)) == 0)
        {
            sscanf(line + strlen(key), "%ld", &val);
            break;
        }
    }
    fclose(f);
    return val;
}

static long rollup_kb(const char *key)
{
    FILE *f = fopen("/proc/self/smaps_rollup", "r");
    if (!f)
        return -1;
    char line[256];
    long val = -1;
    while (fgets(line, sizeof(line), f))
    {
        if (strncmp(line, key, strlen(key)) == 0)
        {
            sscanf(line + strlen(key), "%ld", &val);
            break;
        }
    }
    fclose(f);
    return val;
}

/* Sum Rss over VMAs whose map line names the db file (mmap read path). */
static long db_mmap_kb(void)
{
    FILE *f = fopen("/proc/self/smaps", "r");
    if (!f)
        return -1;
    char line[512];
    long total = 0;
    int is_db = 0;
    while (fgets(line, sizeof(line), f))
    {
        if (line[0] >= '0' && line[0] <= '9' && strchr(line, '-'))
            is_db = strstr(line, "edgeclaw.db") != NULL;
        else if (is_db && strncmp(line, "Rss:", 4) == 0)
        {
            long v = 0;
            sscanf(line + 4, "%ld", &v);
            total += v;
        }
    }
    fclose(f);
    return total;
}

/* Largest single VMA by Rss -- catches the db mmap even when the map line
 * does not carry a filename in this sandbox. */
static long biggest_vma_kb(void)
{
    FILE *f = fopen("/proc/self/smaps", "r");
    if (!f)
        return -1;
    char line[512];
    long best = 0, cur = 0;
    while (fgets(line, sizeof(line), f))
    {
        if (line[0] >= '0' && line[0] <= '9' && strchr(line, '-'))
            cur = 0;
        else if (strncmp(line, "Rss:", 4) == 0)
        {
            long v = 0;
            sscanf(line + 4, "%ld", &v);
            cur = v;
            if (v > best)
                best = v;
        }
    }
    fclose(f);
    return best;
}

struct config
{
    const char *label;
    long long mmap_size;
    int cache_kib; /* negative = KiB */
};

static void run(const struct config *cfg, const char *db_path)
{
    sqlite3 *db = NULL;
    if (sqlite3_open(db_path, &db) != SQLITE_OK)
    {
        printf("  %-22s OPEN FAILED: %s\n", cfg->label, sqlite3_errmsg(db));
        return;
    }

    char sql[256];
    snprintf(sql, sizeof(sql), "PRAGMA mmap_size=%lld", cfg->mmap_size);
    sqlite3_exec(db, sql, NULL, NULL, NULL);
    snprintf(sql, sizeof(sql), "PRAGMA cache_size=%d", cfg->cache_kib);
    sqlite3_exec(db, sql, NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA journal_mode=WAL", NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA synchronous=NORMAL", NULL, NULL, NULL);

    /* Force a full pass over the message bodies, like request assembly / folding
     * does. Sum the lengths so the optimizer cannot skip the column reads. */
    sqlite3_stmt *st = NULL;
    long long bytes = 0;
    long rows = 0;
    if (sqlite3_prepare_v2(db, "SELECT length(body) FROM messages", -1, &st, NULL) == SQLITE_OK)
    {
        while (sqlite3_step(st) == SQLITE_ROW)
        {
            bytes += sqlite3_column_int64(st, 0);
            rows++;
        }
        sqlite3_finalize(st);
    }

    const long rss = status_kb("VmRSS:");
    const long hwm = status_kb("VmHWM:");
    const long thp = rollup_kb("AnonHugePages:");
    const long dmmap = db_mmap_kb();
    const long biggest = biggest_vma_kb();
    printf("  %-22s RSS=%7ld kB  HWM=%7ld kB  THP=%7ld kB  dbmmap=%7ld kB  maxVMA=%7ld kB  (rows=%ld)\n",
           cfg->label, rss, hwm, thp, dmmap, biggest, rows);
    fflush(stdout);
    sqlite3_close(db);
}

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        fprintf(stderr, "usage: %s <db-copy-path>\n", argv[0]);
        return 2;
    }
    const char *path = argv[1];

    static const struct config cases[] = {
        {"old (512M/64M)", 536870912LL, -65536},
        {"cache only (0/64M)", 0LL, -65536},
        {"mmap only (32M/8M)", 33554432LL, -8192},
        {"current (0/8M)", 0LL, -8192},
    };

    printf("SQLite PRAGMA A/B on %s\n", path);
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    {
        pid_t pid = fork();
        if (pid == 0)
        {
            run(&cases[i], path);
            _exit(0);
        }
        int st = 0;
        waitpid(pid, &st, 0);
    }
    return 0;
}
