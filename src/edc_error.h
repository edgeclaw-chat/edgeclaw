#ifndef EDC_ERROR_H
#define EDC_ERROR_H

#ifdef __cplusplus
extern "C" {
#endif

#include "sds.h"

typedef enum {
    EDC_ERR_NONE = 0,
    EDC_ERR_FAILED,
    EDC_ERR_INVAL,
    EDC_ERR_NOTFOUND,
    EDC_ERR_EXEC,
    EDC_ERR_SANDBOX,
} EdcErrorCode;

typedef struct {
    EdcErrorCode code;
    sds message;
} EdcError;

EdcError *edc_error_new(EdcErrorCode code, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

void edc_error_free(EdcError *error);

void edc_error_set(EdcError **errp, EdcErrorCode code, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

#ifdef __cplusplus
}
#endif

#endif
