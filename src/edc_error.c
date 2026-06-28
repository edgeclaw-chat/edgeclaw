#include "edc_error.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

EdcError *edc_error_new(EdcErrorCode code, const char *fmt, ...)
{
    EdcError *err = calloc(1, sizeof(EdcError));
    if (!err)
        return NULL;
    err->code = code;

    va_list ap;
    va_start(ap, fmt);
    if (vasprintf(&err->message, fmt, ap) < 0)
        err->message = NULL;
    va_end(ap);

    return err;
}

void edc_error_free(EdcError *error)
{
    if (error)
    {
        free(error->message);
        free(error);
    }
}

void edc_error_set(EdcError **errp, EdcErrorCode code, const char *fmt, ...)
{
    if (!errp)
        return;

    edc_error_free(*errp);

    EdcError *err = calloc(1, sizeof(EdcError));
    if (!err)
    {
        *errp = NULL;
        return;
    }
    err->code = code;

    va_list ap;
    va_start(ap, fmt);
    if (vasprintf(&err->message, fmt, ap) < 0)
        err->message = NULL;
    va_end(ap);

    *errp = err;
}
