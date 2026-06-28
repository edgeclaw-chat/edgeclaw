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
    err->message = sdsempty();
    if (err->message)
        err->message = sdscatvprintf(err->message, fmt, ap);
    va_end(ap);

    return err;
}

void edc_error_free(EdcError *error)
{
    if (error)
    {
        sdsfree(error->message);
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
    err->message = sdsempty();
    if (err->message)
        err->message = sdscatvprintf(err->message, fmt, ap);
    va_end(ap);

    *errp = err;
}
