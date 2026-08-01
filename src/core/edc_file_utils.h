#ifndef EDC_FILE_UTILS_H
#define EDC_FILE_UTILS_H

#include "edc_error.h"
#include "sds.h"
#include <stdbool.h>
#include <sys/types.h>
#include <uv.h>

void edc_file_utils_init(uv_loop_t *loop);

sds edc_file_read(const char *path, EdcError **error);
bool edc_file_write(const char *path, const char *content, ssize_t len, EdcError **error);

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
