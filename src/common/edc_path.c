#include "edc_path.h"
#include "edc_compat.h" /* PATH_SEP */

#include <stdlib.h>
#include <string.h>
#include <uv.h>

/* How much of |path| is its ROOT -- the part that has to be kept as written and that ".."
 * can never climb above. Returns 0 for a relative path.
 *
 *   "/x"                 -> 1    "/"
 *   "C:\x" / "C:/x"      -> 3    "C:\"   (a bare "C:" is drive-RELATIVE: 2)
 *   "\x"                 -> 1    "\"     (rooted on the current drive)
 *   "\\server\share\x"   -> 15   "\\server\share\"
 *
 * This is the same parse edc_dir_create_with_parents() has to do, and the reason it used
 * to fail: splitting a whole Windows path on '/' hands "C:\a\b" to mkdir() as ONE segment. */
static size_t root_prefix_len(const char *path)
{
    if (!path || !path[0])
        return 0;

#ifdef _WIN32
    /* UNC: "\\server\share", with as many of the two parts as are present. */
    if (edc_path_is_sep(path[0]) && edc_path_is_sep(path[1]))
    {
        size_t i = 2;
        for (int part = 0; part < 2 && path[i]; part++)
        {
            while (path[i] && !edc_path_is_sep(path[i]))
                i++;
            while (edc_path_is_sep(path[i]))
                i++;
        }
        return i;
    }

    /* Drive: "C:" and, when written, the separator after it. */
    if (((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) && path[1] == ':')
    {
        size_t i = 2;
        if (edc_path_is_sep(path[i]))
            i++;
        return i;
    }
#endif

    if (edc_path_is_sep(path[0]))
        return 1;

    return 0;
}

static void push_char(sds *p, char c)
{
    *p = sdscatlen(*p, &c, 1);
}

/* Drop |out| back to its last separator, but never below |floor| (the root we wrote). */
static void pop_segment(sds *out, size_t floor)
{
    size_t len = sdslen(*out);
    if (len <= floor)
        return;

    while (len > floor && (*out)[len - 1] != PATH_SEP)
        len--;
    if (len > floor)
        len--; /* the separator that introduced the segment */

    if (len == 0)
        sdsclear(*out);
    else
        sdsrange(*out, 0, (ssize_t)len - 1);
}

sds edc_path_norm(const char *path)
{
    if (!path)
        return NULL;

    const size_t root_len = root_prefix_len(path);
    const bool absolute = root_len > 0;

    sds out = sdsempty();

    /* 1. The root verbatim, with its separators normalised. */
    for (size_t i = 0; i < root_len; i++)
        push_char(&out, edc_path_is_sep(path[i]) ? PATH_SEP : path[i]);

    /* A root keeps its trailing separator ONLY where dropping it would change the meaning:
     * "/" (the POSIX root), "C:\" (a bare "C:" is drive-RELATIVE) and "\" (rooted on the
     * current drive). Anywhere else the separator is noise that would give one directory
     * two spellings -- "\\server\share\" and "\\server\share" -- which is exactly what the
     * normal form exists to rule out. */
    if (sdslen(out) > 1 && out[sdslen(out) - 1] == PATH_SEP)
    {
#ifdef _WIN32
        const bool root_needs_separator = sdslen(out) == 3 && out[1] == ':';
#else
        const bool root_needs_separator = false;
#endif
        if (!root_needs_separator)
            sdsrange(out, 0, (ssize_t)sdslen(out) - 2);
    }
    const size_t floor = sdslen(out);

#ifdef _WIN32
    /* "C:rel" -- the first segment follows the colon DIRECTLY. Inserting a separator here
     * would silently turn a drive-relative path into a drive-absolute one. */
    const bool drive_relative = (root_len == 2 && path[1] == ':');
#else
    const bool drive_relative = false;
#endif

    /* 2. The segments: "" and "." vanish, ".." cancels the previous segment, and one that
     *    cannot be cancelled is kept only in a relative path (an absolute path stops at
     *    its root -- "/.." is "/", exactly like realpath() would say). */
    const char *p = path + root_len;
    while (*p)
    {
        while (edc_path_is_sep(*p))
            p++;
        if (!*p)
            break;

        const char *q = p;
        while (*q && !edc_path_is_sep(*q))
            q++;
        const size_t len = (size_t)(q - p);

        if (len == 1 && p[0] == '.')
        {
            /* nothing */
        }
        else if (len == 2 && p[0] == '.' && p[1] == '.')
        {
            if (sdslen(out) > floor)
                pop_segment(&out, floor);
            else if (!absolute)
            {
                if (sdslen(out) > 0 && out[sdslen(out) - 1] != PATH_SEP)
                    push_char(&out, PATH_SEP);
                out = sdscatlen(out, "..", 2);
            }
        }
        else
        {
            const bool follows_drive_colon = drive_relative && sdslen(out) == floor;
            if (sdslen(out) > 0 && out[sdslen(out) - 1] != PATH_SEP && !follows_drive_colon)
                push_char(&out, PATH_SEP);
            out = sdscatlen(out, p, len);
        }

        p = q;
    }

    if (sdslen(out) == 0)
        return sdsnew(".");
    return out;
}

sds edc_path_abs(const char *base, const char *path)
{
    if (!path)
        return NULL;

    /* An absolute path is already anchored: joining it onto a base is the bug this exists
     * to prevent ("D:\ws" + "D:\ws\x" = "D:\ws\D:\ws\x"). */
    if (edc_path_is_absolute(path))
        return edc_path_norm(path);

    sds anchor = NULL;
    if (base && base[0])
    {
        anchor = sdsnew(base);
    }
    else
    {
        char buf[4096];
        size_t size = sizeof(buf);
        if (uv_cwd(buf, &size) == 0)
            anchor = sdsnewlen(buf, size);
    }
    if (!anchor)
        return edc_path_norm(path); /* no anchor anywhere: the best answer we have */

    sds joined = sdscatprintf(sdsempty(), "%s%c%s", anchor, PATH_SEP, path);
    sdsfree(anchor);

    sds out = edc_path_norm(joined);
    sdsfree(joined);
    return out;
}

sds edc_path_display(const char *path)
{
    if (!path)
        return NULL;

    sds out = sdsnew(path);
    for (size_t i = 0; i < sdslen(out); i++)
        if (edc_path_is_sep(out[i]))
            out[i] = '/';
    return out;
}

bool edc_path_equal(const char *a, const char *b)
{
    if (!a || !b)
        return false;

    sds na = edc_path_norm(a);
    sds nb = edc_path_norm(b);
    if (!na || !nb)
    {
        sdsfree(na);
        sdsfree(nb);
        return false;
    }

#ifdef _WIN32
    const bool same = strcasecmp(na, nb) == 0;
#else
    const bool same = strcmp(na, nb) == 0;
#endif

    sdsfree(na);
    sdsfree(nb);
    return same;
}
