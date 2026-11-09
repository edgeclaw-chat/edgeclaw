#include "edc_sandbox_helper.h"
#include "edc_config.h"
#include "edc_error.h"
#include "edc_file_utils.h"
#include "edc_sandbox_helper_blob.h"
#include "edc_compat.h"
#include "log.h"
#include <stdlib.h>
#include <string.h>

#include <sys/stat.h>

sds edc_sandbox_helper_path(struct edc_config *conf)
{
    if (!conf || !conf->edc_home || sdslen(conf->edc_home) == 0)
        return NULL;

    /* Keep the helper in <edc_home>/.cache: it must NOT live inside a sandbox
     * writable path (the model could otherwise overwrite it), and .cache is not
     * part of the default rw set. */
    sds dir = edc_path_join(conf->edc_home, ".cache", NULL);
    if (!dir || sdslen(dir) == 0)
    {
        sdsfree(dir);
        return NULL;
    }

    sds path = edc_path_join(dir, "edgeclaw-sandbox-helper", NULL);
    sdsfree(dir);
    if (!path || sdslen(path) == 0)
    {
        sdsfree(path);
        return NULL;
    }

    /* Reuse an already-extracted, executable copy of the right size. */
    struct stat st;
    if (stat(path, &st) == 0 && (size_t)st.st_size == edc_sandbox_helper_blob_size && (st.st_mode & S_IXUSR))
        return path;

    sds parent = edc_path_dirname(path);
    bool dir_ok = edc_dir_create_with_parents(parent, 0700, NULL);
    sdsfree(parent);
    if (!dir_ok)
    {
        log_error("sandbox: cannot create helper directory for %s", path);
        sdsfree(path);
        return NULL;
    }

    EdcError *error = NULL;
    if (!edc_file_write(path, (const char *)edc_sandbox_helper_blob, (ssize_t)edc_sandbox_helper_blob_size, &error))
    {
        log_error("sandbox: cannot write helper %s: %s", path, error ? error->message : "unknown");
        if (error)
            edc_error_free(error);
        sdsfree(path);
        return NULL;
    }
    chmod(path, 0700);
    return path;
}

bool edc_sandbox_helper_prepare(struct edc_config *conf, const char *cmd, sds *out_file, char ***out_args)
{
    if (!conf || !cmd || !out_file || !out_args)
        return false;

    sds path = edc_sandbox_helper_path(conf);
    if (!path)
        return false;

    size_t nro = 0;
    for (int i = 0; conf->sandbox_ro_paths && conf->sandbox_ro_paths[i]; i++)
        nro++;
    size_t nrw = 0;
    for (int i = 0; conf->sandbox_rw_paths && conf->sandbox_rw_paths[i]; i++)
        nrw++;
    size_t ndeny = 0;
    for (int i = 0; conf->sandbox_deny_paths && conf->sandbox_deny_paths[i]; i++)
        ndeny++;

    /* prog --ro <p>... --rw <p>... --deny <p>... -- <cmd> NULL */
    size_t count = 1 + 1 + nro + 1 + nrw + 1 + ndeny + 1 + 1 + 1;
    char **argv = calloc(count, sizeof(char *));
    if (!argv)
    {
        sdsfree(path);
        return false;
    }

    size_t k = 0;
    argv[k++] = path;
    argv[k++] = "--ro";
    for (int i = 0; conf->sandbox_ro_paths && conf->sandbox_ro_paths[i]; i++)
        argv[k++] = conf->sandbox_ro_paths[i];
    argv[k++] = "--rw";
    for (int i = 0; conf->sandbox_rw_paths && conf->sandbox_rw_paths[i]; i++)
        argv[k++] = conf->sandbox_rw_paths[i];
    argv[k++] = "--deny";
    for (int i = 0; conf->sandbox_deny_paths && conf->sandbox_deny_paths[i]; i++)
        argv[k++] = conf->sandbox_deny_paths[i];
    argv[k++] = "--";
    argv[k++] = (char *)cmd;
    argv[k] = NULL;

    *out_file = path;
    *out_args = argv;
    return true;
}
