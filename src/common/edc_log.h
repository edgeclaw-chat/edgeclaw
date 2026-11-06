#ifndef BC_LOG_H
#define BC_LOG_H

#ifdef __cplusplus
extern "C" {
#endif
void edc_log_init(void);
void edc_log_close(void);

/* The log file currently being appended to, or "" before the first successful open.
 *
 * Exists so a failure message can name the file instead of saying "see the log": a user
 * who is told where to look is one step from the reason; a user told "see the log" has to
 * find out where logs go, on a program that just refused to start. Never NULL. */
const char *edc_log_path(void);
#ifdef __cplusplus
}
#endif

#endif
