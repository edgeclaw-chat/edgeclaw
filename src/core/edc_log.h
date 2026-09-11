#ifndef BC_LOG_H
#define BC_LOG_H

#ifdef __cplusplus
extern "C" {
#endif
void edc_log_init(void);
void edc_log_check_rotate(void);
void edc_log_close(void);
#ifdef __cplusplus
}
#endif

#endif
