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

/* Read a file the model or the user picked, with a byte ceiling checked BEFORE allocating.
 * A file over |max_bytes| (0 = no ceiling) is refused with its real size in the message, so the
 * caller can say "too big, use offset/limit". |out_size| (optional) always receives the size
 * when it could be determined, refused calls included. */
sds edc_file_read_capped(const char *path, size_t max_bytes, size_t *out_size, EdcError **error);
bool edc_file_write(const char *path, const char *content, ssize_t len, EdcError **error);

/* Replace |dst| with |src| (both paths), atomically where the platform allows it.
 *
 * POSIX rename() replaces an existing destination; the Windows CRT rename() does
 * NOT -- it fails with EEXIST -- so the "write .tmp, then rename" pattern silently
 * kept the FIRST version of every state file on Windows (session.json, sessions
 * index, sequence watermarks). This wrapper uses MoveFileExW with REPLACE_EXISTING
 * on Windows and rename() elsewhere. Returns false with *error set on failure. */
bool edc_file_rename_replace(const char *src, const char *dst, EdcError **error);

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

/* Reading a path the model can influence lives in edc_path_guard.h (edc_path_guard_read):
 * it is the one place that owns the symlink / hardlink / containment / deny rules. */


bool edc_file_exists(const char *path);
bool edc_file_is_dir(const char *path);
bool edc_file_is_regular(const char *path);

bool edc_dir_create_with_parents(const char *path, int mode, EdcError **error);

/* Visit every entry of |path| except "." and ".." -- they name the directory itself and
 * its parent, no caller ever wanted them, and three of them filtered them out with their
 * own copy of the test. |name| is an entry name, not a path: join it with edc_path_join().
 * |type| is one of the UV_DIRENT_* values (UNKNOWN when the platform does not say). */
typedef void (*edc_dir_entry_cb)(const char *name, int type, void *userdata);
bool edc_dir_foreach(const char *path, edc_dir_entry_cb cb, void *userdata, EdcError **error);

sds edc_path_join(const char *first, ...);
sds edc_path_dirname(const char *path);

sds edc_cwd(void);
sds edc_homedir(void);
sds edc_os_name(void);

#endif
