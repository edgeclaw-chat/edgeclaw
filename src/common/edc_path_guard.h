#ifndef EDC_PATH_GUARD_H
#define EDC_PATH_GUARD_H

/* What has to be true about a path before anything may open it.
 *
 * This is the ONE place that answers "which file is this, and may I touch it". Before it,
 * the same questions were answered twice, in two places that did not know about each
 * other: the sandbox layer (realpath + allow/deny lists) and the file helpers (lstat +
 * "is the parent inside root"). Two copies meant two behaviours -- a symlink was refused
 * by one and followed by the other, and nothing at all looked at hardlinks.
 *
 * The two questions are deliberately kept apart:
 *
 *   MECHANISM (this header): is the path the file it claims to be?
 *     - a relative path resolves against `base`;
 *     - the result must sit inside `root` (when one is given) -- that is what makes
 *       "@dir/name.md" mean "a file in that directory" and not "whatever it points at";
 *     - the final component must not be a symlink, and (when asked) the file must not
 *       have a second name (st_nlink > 1: a hardlink is invisible to lstat).
 *
 *   POLICY (the caller's): does the sandbox allow these bytes out?
 *     - the tool layer asks "inside ro/rw and outside deny" -> edc_sandbox_verify_path_at();
 *     - model-facing reads (a template body, a SKILL.md) ask ONLY "outside deny", because
 *       their resource roots (~/.claude, ~/.pi/agent, ...) are outside the rw/ro lists by
 *       design and must stay readable -> apply_deny below.
 */
#include "edc_error.h"
#include "sds.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

struct edc_path_rules
{
    /* Relative |path| resolves against this (NULL = the process's cwd). */
    const char *base;
    /* Containment boundary for the RESOLVED path (NULL = no containment requirement).
     * This is the directory the caller discovered the file under -- not the sandbox root. */
    const char *root;
    /* A write may name a file that does not exist yet, so the resolution falls back to
     * "resolve the parent, keep the last component". */
    bool writable;
    bool refuse_symlink;
    /* st_nlink > 1: the file has another name somewhere else, which lstat cannot show.
     * Set for content that is handed to the model; leave false for the general tool IO
     * (a hardlinked file there is legitimate). */
    bool refuse_hardlink;
    /* NULL-terminated list of denied directories (the sandbox policy's deny set), or NULL.
     * Deny wins over everything, reads included; a caller that is not subject to the
     * sandbox passes NULL. An entry that does not resolve is inert -- the same limit the
     * allow lists have. */
    char *const *deny_paths;
};

/* Resolve |path| and apply |rules|. Returns the absolute, resolved path (caller frees) or
 * NULL with |error| set to the reason. */
sds edc_path_guard_resolve(const struct edc_path_rules *rules, const char *path, EdcError **error);

/* Read a whole file through the guard: resolved, checked, opened with O_NOFOLLOW, must be
 * a regular file, and at most |max_bytes| (0 = no cap). NULL + |error| on refusal. */
sds edc_path_guard_read(const struct edc_path_rules *rules, const char *path, size_t max_bytes, EdcError **error);

#endif
