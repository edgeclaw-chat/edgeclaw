#ifndef EDC_BUNDLED_WORKSPACE_H
#define EDC_BUNDLED_WORKSPACE_H

#include <stddef.h>

/*
 * The default workspace tree (AGENTS.md, TOOLS.md, memory/, skills/, ...) is embedded
 * into the binary at build time by //tools:embed_workspace.py, so a fresh
 * install no longer needs a `workspace/` directory next to the executable or
 * in the current working directory. edc_init_env() releases this table into
 * $EDC_HOME/workspace on first run.
 */

/* A single file embedded into the binary. */
struct edc_bundled_file
{
    const char *path;          /* path relative to the workspace root, '/' separators */
    const unsigned char *data; /* raw contents; may contain NUL bytes */
    size_t size;               /* number of bytes in `data` */
};

/* Generated table, in stable path-sorted order (see //src/core:workspace_embedded_c). */
extern const struct edc_bundled_file edc_bundled_workspace_entries[];
extern const size_t edc_bundled_workspace_count;

#endif /* EDC_BUNDLED_WORKSPACE_H */
