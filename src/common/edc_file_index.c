#include "edc_file_index.h"

#include "edc_file_utils.h"
#include "sds.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <uv.h>

/* One directory at a time, matched by STRICT PREFIX. There is no tree walk and no cache:
 * a path is typed a component at a time, so listing the directory the user is standing in
 * is enough -- and it is always current (no "I added a file and the picker does not see
 * it" class of bug, and no 20000-entry walk on the first `@`).
 *
 * Accepted as a query:
 *   ""            the completion root (the session's directory)
 *   "src/tu"      relative to that root, prefix by prefix
 *   "..", "../x"  relative with dot segments, resolved properly
 *   "~/", "~/x"   the home directory ('~user' is NOT supported)
 *   "/", "/usr/"  absolute
 * Only the DIRECTORY part has to exist; the last component is the prefix being typed, so
 * a name that does not exist yet still lists its siblings. */

#define EDC_FILES_MAX_RESULTS 20

/* Directories whose contents are noise when a directory is listed in full. An explicit
 * prefix bypasses this: typing "@node_modules" means you do want it. */
static const char *const kSkipDirs[] = {
    ".git",      ".hg",        ".svn",         "node_modules", "build",     "bazel-out",
    "bazel-bin", "bazel-testlogs", "__pycache__", ".cache",      ".venv",     "venv",
    "target",    ".idea",      ".vscode",      "dist",         ".next",     ".gradle",
};

struct hit
{
    sds path; /* absolute */
    bool dir;
};

struct list_ctx
{
    const char *dir;     /* absolute, ends with '/' */
    const char *prefix;  /* may be "" -- then the skip list applies */
    size_t plen;
    struct hit *hits;
    size_t n;
};

static bool skip_dir(const char *name)
{
    for (size_t i = 0; i < sizeof(kSkipDirs) / sizeof(kSkipDirs[0]); i++)
        if (strcmp(name, kSkipDirs[i]) == 0)
            return true;
    return false;
}

static bool prefix_ci(const char *name, const char *prefix, size_t plen)
{
    for (size_t i = 0; i < plen; i++)
    {
        if (!name[i] || tolower((unsigned char)name[i]) != tolower((unsigned char)prefix[i]))
            return false;
    }
    return true;
}

static void list_cb(const char *name, int type, void *ud)
{
    struct list_ctx *c = ud;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        return;
    if (c->n >= EDC_FILES_MAX_RESULTS)
        return;
    if (!prefix_ci(name, c->prefix, c->plen))
        return;

    bool dir = (type == UV_DIRENT_DIR);
    if (type == UV_DIRENT_UNKNOWN)
    {
        sds probe = sdscatprintf(sdsempty(), "%s%s", c->dir, name);
        struct stat st;
        dir = (stat(probe, &st) == 0) && S_ISDIR(st.st_mode);
        sdsfree(probe);
    }
    /* A full listing drops the noisy directories; a typed prefix keeps them ("@node_"). */
    if (c->plen == 0 && dir && skip_dir(name))
        return;

    c->hits = realloc(c->hits, sizeof(*c->hits) * (c->n + 1));
    c->hits[c->n].path = sdscatprintf(sdsempty(), "%s%s", c->dir, name);
    c->hits[c->n].dir = dir;
    c->n++;
}

static int hit_cmp(const void *a, const void *b)
{
    const struct hit *ha = a, *hb = b;
    if (ha->dir != hb->dir)
        return ha->dir ? -1 : 1; /* directories first */
    return strcmp(ha->path, hb->path);
}

void edc_file_index_reset(void)
{
    /* Nothing is cached any more; kept so callers (the TUI, after a submission) do not
     * have to care. */
}

size_t edc_file_index_query(const char *root, const char *query, char *buf, size_t cap)
{
    if (buf && cap > 0)
        buf[0] = '\0';
    if (!buf || cap == 0)
        return 0;

    const char *q = query ? query : "";

    /* 1. A leading '~' names the home directory ('~user' is not a thing here). */
    sds expanded = NULL;
    bool absolute = false;
    if (q[0] == '~')
    {
        sds home = edc_homedir();
        if (!home || !home[0])
        {
            if (home)
                sdsfree(home);
            return 0;
        }
        if (q[1] == '\0')
            expanded = home;
        else if (q[1] == '/')
            expanded = sdscat(home, q + 1); /* home already has no trailing '/'  */
        else
        {
            sdsfree(home);
            return 0;
        }
        q = expanded;
        absolute = true;
    }
    else if (q[0] == '/')
    {
        absolute = true;
    }

    /* 2. Split into "the directory to read" and "the name being typed". */
    const char *last = strrchr(q, '/');
    sds dir_part = last ? sdsnewlen(q, (size_t)(last - q) + 1) : sdsempty();
    sds prefix = sdsnew(last ? last + 1 : q);

    /* 3. Resolve the DIRECTORY part -- dot segments and symlinks included. Only this part
     *    has to exist; the prefix is just text. */
    const char *base = (root && root[0]) ? root : NULL;
    sds dir_joined = NULL;
    if (absolute)
    {
        dir_joined = sdslen(dir_part) ? sdsdup(dir_part) : sdsnew("/");
    }
    else if (!base)
    {
        dir_joined = sdslen(dir_part) ? sdsdup(dir_part) : sdsnew(".");
    }
    else
    {
        dir_joined = sdslen(dir_part) ? edc_path_join(base, dir_part, NULL) : sdsnew(base);
    }

    char *dir_real = realpath(dir_joined, NULL);
    sdsfree(dir_joined);
    sdsfree(dir_part);
    if (!dir_real)
    {
        sdsfree(prefix);
        if (expanded)
            sdsfree(expanded);
        return 0;
    }

    sds dir_with_slash = sdscatprintf(sdsempty(), "%s%s", dir_real, strcmp(dir_real, "/") == 0 ? "" : "/");
    struct list_ctx ctx = {.dir = dir_with_slash, .prefix = prefix, .plen = sdslen(prefix)};

    EdcError *error = NULL;
    edc_dir_foreach(dir_real, list_cb, &ctx, &error);
    if (error)
        edc_error_free(error);
    free(dir_real);

    qsort(ctx.hits, ctx.n, sizeof(*ctx.hits), hit_cmp);

    /* 4. Emit: an absolute (or '~') query keeps absolute paths; a relative one is written
     *    relative to the completion root, so what the picker inserts stays short -- unless
     *    it walked out of the root with "..", where an absolute path is the honest answer. */
    char *base_real = (!absolute && base) ? realpath(base, NULL) : NULL;
    const size_t base_len = base_real ? strlen(base_real) : 0;

    size_t used = 0, rows = 0;
    for (size_t i = 0; i < ctx.n && rows < EDC_FILES_MAX_RESULTS; i++)
    {
        const char *path = ctx.hits[i].path;
        if (!absolute && base_real && strncmp(path, base_real, base_len) == 0 && path[base_len] == '/')
            path += base_len + 1;

        const int need = snprintf(buf + used, cap - used, "%s%s\t%c\n", path, ctx.hits[i].dir ? "/" : "",
                                  ctx.hits[i].dir ? 'd' : 'f');
        if (need < 0 || (size_t)need >= cap - used)
            break;
        used += (size_t)need;
        rows++;
    }

    for (size_t i = 0; i < ctx.n; i++)
        sdsfree(ctx.hits[i].path);
    free(ctx.hits);
    sdsfree(dir_with_slash);
    sdsfree(prefix);
    if (base_real)
        free(base_real);
    if (expanded)
        sdsfree(expanded);
    return rows;
}
