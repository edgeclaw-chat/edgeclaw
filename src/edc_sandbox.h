#ifndef __BC_SANDBOX_H__
#define __BC_SANDBOX_H__

#include <glib.h>

struct edc_config;

extern GQuark edc_sandbox_error_quark(void);
#define BC_SANDBOX_ERROR (edc_sandbox_error_quark())
typedef enum
{
    BC_SANDBOX_ERROR_ESCAPE = 0,
    BC_SANDBOX_ERROR_INIT,
} edc_sandbox_err_t;

extern gboolean edc_sandbox_init(struct edc_config *conf);
extern const gchar *edc_sandbox_get_root(struct edc_config *conf);
extern gchar *edc_sandbox_verify_path(struct edc_config *conf, const gchar *path, gboolean writable, GError **error);
extern gboolean edc_sandbox_apply(struct edc_config *conf);

#endif
