#include "edc_log.h"
#include "edc_compat.h"
#include "edc_config.h"
#include "edc_file_utils.h"
#include "log.h"
#include "sds.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <uv.h>

static FILE *g_log_file = NULL;
static char g_current_date[32] = {0};
static uv_mutex_t g_log_mutex;

static void edc_log_file_callback(log_Event *ev)
{
    uv_mutex_lock(&g_log_mutex);
    FILE *fp = g_log_file;
    if (fp)
    {
        char buf[64];
        buf[strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", ev->time)] = '\0';
        fprintf(fp, "%s %-5s %s:%d: ", buf, log_level_string(ev->level), ev->file, ev->line);
        vfprintf(fp, ev->fmt, ev->ap);
        fprintf(fp, "\n");
        fflush(fp);
    }
    uv_mutex_unlock(&g_log_mutex);
}

static void edc_log_rotate(void)
{
    time_t now = time(NULL);
    struct tm tm_now = {0};
#ifdef _WIN32
    localtime_s(&tm_now, &now);
#else
    localtime_r(&now, &tm_now);
#endif
    char new_date[32] = {0};
    strftime(new_date, sizeof(new_date), "%Y-%m-%d", &tm_now);

    if (strcmp(new_date, g_current_date) == 0)
        return;

    strncpy(g_current_date, new_date, sizeof(g_current_date) - 1);
    g_current_date[sizeof(g_current_date) - 1] = '\0';

    sds edc_home = edc_config_resolve_bc_home();
    sds log_dir = edc_path_join(edc_home, "log", NULL);
    sdsfree(edc_home);
    edc_dir_create_with_parents(log_dir, 0755, NULL);

    sds filename = sdscatprintf(sdsempty(), "%s%cedgeclaw-%s.log", log_dir, PATH_SEP, new_date);

    uv_mutex_lock(&g_log_mutex);
    FILE *new_file = fopen(filename, "a");
    if (new_file)
    {
        setvbuf(new_file, NULL, _IOLBF, 0);
        FILE *old = g_log_file;
        g_log_file = new_file;
        if (old)
            fclose(old);
    }
    uv_mutex_unlock(&g_log_mutex);

    sdsfree(filename);
    sdsfree(log_dir);
}

void edc_log_init(void)
{
    uv_mutex_init(&g_log_mutex);
    log_set_quiet(true);
#ifdef EDGECLAW_DEBUG
    /* Debug build (bazel -c dbg): keep every level, trace/debug included. */
    const int level = LOG_TRACE;
#else
    /* Release/fastbuild: INFO and above only; log_debug/log_trace are dropped
     * so debug breadcrumbs never reach the log file in normal builds. */
    const int level = LOG_INFO;
#endif
    log_set_level(level);
    edc_log_rotate();
    /* NB: in this pinned log.c, log_set_level() only gates the (quiet) stderr
     * path — a callback is filtered solely by the level it is registered with.
     * The file callback therefore has to be registered at |level|, not
     * LOG_TRACE, or log_debug/log_trace would still land in the file. */
    log_add_callback(edc_log_file_callback, NULL, level);
}

void edc_log_check_rotate(void)
{
    edc_log_rotate();
}

/* Close the log file handle. Must be called after all actor threads have stopped (else
 * a concurrent log writes to a closed FILE*). Once g_log_file is NULLed, a late log
 * callback safely no-ops -- so the mutex is kept (not destroyed), or a late callback
 * would lock a destroyed mutex. */
void edc_log_close(void)
{
    uv_mutex_lock(&g_log_mutex);
    if (g_log_file)
    {
        fclose(g_log_file);
        g_log_file = NULL;
    }
    uv_mutex_unlock(&g_log_mutex);
}
