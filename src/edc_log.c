#include "edc_log.h"
#include "edc_config.h"
#include "log.h"

#include <glib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static FILE *g_log_file = NULL;
static char g_current_date[32] = {0};
static GMutex g_log_mutex;

static void edc_log_file_callback(log_Event *ev)
{
    g_mutex_lock(&g_log_mutex);
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
    g_mutex_unlock(&g_log_mutex);
}

static void edc_log_rotate(void)
{
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    char new_date[32] = {0};
    strftime(new_date, sizeof(new_date), "%Y-%m-%d", &tm_now);

    if (strcmp(new_date, g_current_date) == 0)
        return;

    strncpy(g_current_date, new_date, sizeof(g_current_date) - 1);
    g_current_date[sizeof(g_current_date) - 1] = '\0';

    char *edc_home = edc_config_resolve_bc_home();
    char *log_dir = g_build_filename(edc_home, "log", NULL);
    free(edc_home);
    g_mkdir_with_parents(log_dir, 0755);

    char *filename = edc_strdup_printf("%s/edgeclaw-%s.log", log_dir, new_date);

    g_mutex_lock(&g_log_mutex);
    FILE *new_file = fopen(filename, "a");
    if (new_file)
    {
        setvbuf(new_file, NULL, _IOLBF, 0);
        FILE *old = g_log_file;
        g_log_file = new_file;
        if (old)
            fclose(old);
    }
    g_mutex_unlock(&g_log_mutex);

    free(filename);
    free(log_dir);
}

void edc_log_init(void)
{
    log_set_quiet(true);
    edc_log_rotate();
    log_add_callback(edc_log_file_callback, NULL, LOG_TRACE);
}

void edc_log_check_rotate(void)
{
    edc_log_rotate();
}
