#include "edc_path_guard.h"

#include "edc_compat.h"
#include "edc_path.h"
#include "edc_file_utils.h"

#include <inttypes.h> /* PRId64 / PRIu64 */
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif

/* Length of the longest real path prefix in |paths| that contains |resolved|, or -1.
 * The segment rules -- and the trailing-separator trimming, and the backslash/case
 * handling a slash-only test got wrong on Windows -- live in edc_path_prefix_len. */
static int prefix_match_len(const char *resolved, char *const *paths)
{
    if (!paths || !resolved)
        return -1;

    int best = -1;
    for (int i = 0; paths[i]; i++)
    {
        /* An entry that does not resolve cannot match, so a deny entry naming a directory
         * that does not exist yet does nothing. Known limit: nothing reports it. */
        char *entry = realpath(paths[i], NULL);
        if (!entry)
            continue;
        const int len = edc_path_prefix_len(resolved, entry);
        if (len > best)
            best = len;
        free(entry);
    }
    return best;
}

static bool denied(const struct edc_path_rules *rules, const char *resolved)
{
    if (!rules->deny_paths)
        return false;
    return prefix_match_len(resolved, rules->deny_paths) >= 0;
}

/* Is |resolved| inside |root| (path-boundary compare on both real paths)? */
static bool within_root(const char *resolved, const char *root)
{
    if (!root || !root[0])
        return true;

    char *root_real = realpath(root, NULL);
    if (!root_real)
        return false; /* a root that does not resolve cannot contain anything */
    /* A slash-only boundary test made every backslash path look like an escape, so on
     * Windows every write under a root was refused (chat.jsonl, jobs.json, skills). */
    const bool ok = edc_path_is_within(resolved, root_real);
    free(root_real);
    return ok;
}

sds edc_path_guard_resolve(const struct edc_path_rules *rules, const char *path, EdcError **error)
{
    if (error)
        *error = NULL;
    if (!rules || !path || !path[0])
    {
        edc_error_set(error, EDC_ERR_INVAL, "empty path");
        return NULL;
    }

    /* 1. Relative paths resolve against the caller's base -- a "@file" or a template name
     *    means what the session's directory means by it. */
    sds joined = NULL;
    if (rules->base && rules->base[0] && !edc_path_is_absolute(path))
        joined = edc_path_join(rules->base, path, NULL);
    else
        joined = sdsnew(path);

    /* 2. A symlink in the LAST component is refused before anything follows it. lstat is
     *    what sees the link itself; realpath would silently follow it. */
    struct stat lst;
    if (rules->refuse_symlink && lstat(joined, &lst) == 0 && S_ISLNK(lst.st_mode))
    {
        edc_error_set(error, EDC_ERR_FAILED, "'%s' is a symlink; refusing to use it", path);
        sdsfree(joined);
        return NULL;
    }

    /* 3. Resolve. A file that does not exist yet (a write) resolves through its parent,
     *    keeping the last component as given. */
    char *resolved = realpath(joined, NULL);
    if (!resolved && rules->writable)
    {
        sds parent = edc_path_dirname(joined);
        char *parent_real = realpath(parent, NULL);
        sdsfree(parent);
        if (parent_real)
        {
            const char *last_sep = NULL;
            for (const char *q = joined; *q; q++)
                if (edc_path_is_sep(*q))
                    last_sep = q;
            sds rebuilt = edc_path_join(parent_real, last_sep ? last_sep + 1 : joined, NULL);
            free(parent_real);
            resolved = strdup(rebuilt);
            sdsfree(rebuilt);
        }
    }
    if (!resolved)
    {
        edc_error_set(error, EDC_ERR_FAILED, "cannot resolve '%s': %s", path, strerror(errno));
        sdsfree(joined);
        return NULL;
    }
    sdsfree(joined);

    /* 4. Containment. This is the traversal rule: the file must be the one under the
     *    directory that named it, not something it links to. */
    if (!within_root(resolved, rules->root))
    {
        edc_error_set(error, EDC_ERR_FAILED, "'%s' is outside %s", path, rules->root);
        free(resolved);
        return NULL;
    }

    /* 5. A second name for the same inode (hardlink). lstat cannot see this; st_nlink can.
     *    Only asked for content that ends up in the model's context. */
    if (rules->refuse_hardlink && stat(resolved, &lst) == 0 && lst.st_nlink > 1)
    {
        edc_error_set(error, EDC_ERR_FAILED,
                      "'%s' has %" PRIu64 " links (a hardlink to another file); refusing to use it", path,
                      (uint64_t)lst.st_nlink);
        free(resolved);
        return NULL;
    }

    /* 6. Policy: the deny list is the only LIST that applies to model-facing reads. */
    if (denied(rules, resolved))
    {
        edc_error_set(error, EDC_ERR_FAILED, "'%s' is in a denied directory", path);
        free(resolved);
        return NULL;
    }

    sds out = sdsnew(resolved);
    free(resolved);
    return out;
}

sds edc_path_guard_read(const struct edc_path_rules *rules, const char *path, size_t max_bytes, EdcError **error)
{
    sds resolved = edc_path_guard_resolve(rules, path, error);
    if (!resolved)
        return NULL;

    int fd = open(resolved, O_RDONLY | O_NOFOLLOW);
    if (fd < 0)
    {
        edc_error_set(error, EDC_ERR_FAILED, "open '%s' failed: %s", resolved, strerror(errno));
        sdsfree(resolved);
        return NULL;
    }

    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode))
    {
        edc_error_set(error, EDC_ERR_FAILED, "'%s' is not a regular file", resolved);
        close(fd);
        sdsfree(resolved);
        return NULL;
    }
    if (max_bytes > 0 && (size_t)st.st_size > max_bytes)
    {
        edc_error_set(error, EDC_ERR_FAILED, "'%s' is larger than %zu bytes", resolved, max_bytes);
        close(fd);
        sdsfree(resolved);
        return NULL;
    }

    sds out = sdsempty();
    char chunk[8192];
    ssize_t n;
    while ((n = read(fd, chunk, sizeof(chunk))) > 0)
    {
        if (max_bytes > 0 && sdslen(out) + (size_t)n > max_bytes)
        {
            /* The size on disk lied (the file grew after fstat): fail rather than hand
             * back half a file as if it were the whole one. */
            edc_error_set(error, EDC_ERR_FAILED, "'%s' grew past %zu bytes", resolved, max_bytes);
            sdsfree(out);
            close(fd);
            sdsfree(resolved);
            return NULL;
        }
        out = sdscatlen(out, chunk, (size_t)n);
    }
    close(fd);
    sdsfree(resolved);
    return out;
}
