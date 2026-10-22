#ifndef EDC_BUNDLED_FILES_H
#define EDC_BUNDLED_FILES_H

#include <stddef.h>

/* ── Embedded file trees ──────────────────────────────────────
 *
 * Some trees have to travel INSIDE the executable: the default workspace and the user
 * manual. Both are turned into a C table at build time by //tools:embed_workspace.py and
 * released into $EDC_HOME on startup (see edc_init_env), so a release is one file with
 * no source tree beside it.
 *
 * One entry = one file. `path` is relative to the tree's root and always uses '/'
 * separators, so the releasing side joins it onto the destination directory. */

struct edc_bundled_file
{
    const char *path;          /* path relative to the tree root, '/' separators */
    const unsigned char *data; /* raw contents; may contain NUL bytes */
    size_t size;               /* number of bytes in `data` */
};

#endif /* EDC_BUNDLED_FILES_H */
