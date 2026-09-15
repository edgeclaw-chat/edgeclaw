#ifndef EDC_FILE_UTILS_H
#define EDC_FILE_UTILS_H

#include "edc_error.h"
#include "sds.h"
#include <stdbool.h>
#include <stdio.h>
#include <sys/types.h>
#include <uv.h>

void edc_file_utils_init(uv_loop_t *loop);

sds edc_file_read(const char *path, EdcError **error);
bool edc_file_write(const char *path, const char *content, ssize_t len, EdcError **error);

/* Symlink-safe variants for writers whose target path is derived from a
 * directory the sandboxed model can write to (workspace, session dirs, ...).
 * They refuse to operate when:
 *   - the final path component is a symlink (so a planted symlink such as
 *     workspace/jobs.json -> ~/.edgeclaw/config.json cannot redirect the write);
 *   - |root| is non-NULL and the resolved parent directory is not inside it
 *     (so a symlinked intermediate directory cannot escape either).
 * edc_file_fopen_safe() returns NULL with errno set on refusal. */
FILE *edc_file_fopen_safe(const char *path, const char *mode, const char *root);
bool edc_file_write_safe(const char *path, const char *content, ssize_t len, const char *root, EdcError **error);

bool edc_file_exists(const char *path);
bool edc_file_is_dir(const char *path);
bool edc_file_is_regular(const char *path);

bool edc_dir_create_with_parents(const char *path, int mode, EdcError **error);

typedef void (*edc_dir_entry_cb)(const char *name, int type, void *userdata);
bool edc_dir_foreach(const char *path, edc_dir_entry_cb cb, void *userdata, EdcError **error);

sds edc_path_join(const char *first, ...);
sds edc_path_dirname(const char *path);

sds edc_cwd(void);
sds edc_homedir(void);
sds edc_os_name(void);

#endif
