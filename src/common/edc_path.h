#ifndef EDC_PATH_H
#define EDC_PATH_H

/* Path semantics: what "separator", "absolute", "inside" and "normal form" mean.
 *
 * This used to live in edc_compat.h -- a header whose own comment calls itself
 * transitional -- so nobody treated it as THE implementation, and the same rules grew
 * copies in edc_sandbox.c, edc_app.c and edc_file_index.c. One copy knew about
 * backslashes; the others did not, and that is where every Windows path bug here came
 * from. A rule that lives in three places is a rule that is wrong in two of them.
 *
 * THE RULES, in one place so a caller does not have to remember them:
 *
 *   separator   '/' everywhere, and also '\' on Windows        (edc_path_is_sep)
 *   absolute    rooted: "/x" on POSIX; "D:\x", "D:/x", "\x" and
 *               "\\server\share" on Windows. "C:" alone is NOT absolute -- it is
 *               drive-RELATIVE, which is why a bare path[0] == '/' test broke
 *               `session new D:\ws` ("D:\ws" anchored at "D:\ws")
 *                                                              (edc_path_is_absolute)
 *   containment whole segments, and on Windows either separator and any case:
 *               "a/bc" is NOT inside "a/b", "D:\ws-evil" is NOT inside "D:\ws",
 *               but "D:/ws/x" IS inside "D:\ws"
 *                                        (edc_path_prefix_len / edc_path_is_within)
 *   normal form platform separators, no "." or ".." segments, no repeated or trailing
 *               separator (a root keeps its own), no mixed spellings.
 *               edc_path_norm() is the only producer; edc_path_abs() anchors first.
 *
 * NOT here on purpose: realpath(). That is the filesystem's opinion (symlinks, case,
 * existence) and it belongs to edc_path_guard_resolve(), which decides whether a path
 * may be opened. edc_path_norm() is lexical and never touches the disk -- so it can
 * normalise a path that does not exist yet, which is exactly what a write needs. */

#include "sds.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* True when |path| names an absolute path on this platform: a leading slash or, on
 * Windows, a leading backslash (rooted or UNC) or a drive prefix like "C:\" or "C:/".
 * Callers that anchor a user-supplied path at a base must ask this instead of testing
 * path[0] == slash, or an absolute Windows path gets joined onto the base
 * ("D:\ws" resolved against "D:\ws" became "D:\ws\D:\ws"). */
static inline bool edc_path_is_absolute(const char *path)
{
    if (!path || !path[0])
        return false;
    if (path[0] == '/')
        return true;
#ifdef _WIN32
    if (path[0] == '\\')
        return true;
    const char drive = path[0];
    const bool is_letter = (drive >= 'A' && drive <= 'Z') || (drive >= 'a' && drive <= 'z');
    return is_letter && path[1] == ':' && (path[2] == '\\' || path[2] == '/');
#else
    return false;
#endif
}

/* True when |c| separates path segments here: both '\' and '/' on Windows, '/'
 * elsewhere. */
static inline bool edc_path_is_sep(char c)
{
#ifdef _WIN32
    return c == '\\' || c == '/';
#else
    return c == '/';
#endif
}

/* How many bytes of |prefix| |path| starts with, as a whole-segment prefix: "a/bc" is
 * NOT inside "a/b" (the boundary is not a separator). Returns -1 when |prefix| does not
 * contain |path| at all, and the (separator-trimmed) length of |prefix| when it does --
 * a path is inside itself, and an empty prefix contains everything (0).
 *
 * A trailing separator on |prefix| is ignored. On Windows the comparison ignores case
 * and treats '\' and '/' as the same boundary, because realpath (_fullpath) returns
 * backslashes while a configured root may be written with '/' and any case.
 *
 * This is the ONE "is A inside B" primitive: edc_path_is_within() is its boolean form,
 * and the sandbox allow/deny lists use the LENGTH to decide which rule is the most
 * specific one. Before it, the same comparison was re-implemented in edc_sandbox.c,
 * edc_path_guard.c and edc_app.c, and only one of the copies knew about backslashes. */
static inline int edc_path_prefix_len(const char *path, const char *prefix)
{
    if (!path || !prefix)
        return -1;

    size_t len = strlen(prefix);
    while (len > 1 && edc_path_is_sep(prefix[len - 1]))
        len--;
    if (len == 0)
        return 0;

    for (size_t i = 0; i < len; i++)
    {
        const char a = path[i];
        if (a == '\0')
            return -1; /* path shorter than the prefix */
        if (edc_path_is_sep(a) && edc_path_is_sep(prefix[i]))
            continue; /* '/' and '\' are the same boundary */
#ifdef _WIN32
        const char lower_a = (a >= 'A' && a <= 'Z') ? (char)(a - 'A' + 'a') : a;
        char p = prefix[i];
        if (p >= 'A' && p <= 'Z')
            p = (char)(p - 'A' + 'a');
        if (lower_a != p)
            return -1;
#else
        if (a != prefix[i])
            return -1;
#endif
    }

    /* The prefix has to end on a segment boundary, or "a/bc" would be inside "a/b". */
    if (path[len] != '\0' && !edc_path_is_sep(path[len]))
        return -1;
    return (int)len;
}

/* True when |path| lies at or below |prefix| (whole segments; see edc_path_prefix_len
 * for the separator and case rules). A NULL argument is never contained. */
static inline bool edc_path_is_within(const char *path, const char *prefix)
{
    return edc_path_prefix_len(path, prefix) >= 0;
}

/* ── The normal form ─────────────────────────────────────────────────────────
 *
 * Put a path into normal form at the BOUNDARY (a path arriving from config.json, from
 * the model, from a session's working directory, from the command line), and let the
 * inside of the program see one shape only. The alternative -- every consumer defending
 * itself with its own path[0] / trailing-separator / strrchr test -- is what produced
 * the copies this header replaced. */

/* Normal form of |path| (see the rules above), or NULL when |path| is NULL. An
 * unresolvable path is not an error here: the normal form of a path that does not exist
 * yet is still well defined. Returns a new sds (caller frees), "" for "". */
sds edc_path_norm(const char *path);

/* Normal form of |path| made absolute against |base|. An absolute |path| ignores |base|;
 * a relative one is anchored at |base| or, when that is NULL/empty, at the process's
 * current directory. This is what replaces the `path[0] == '/' ? dup : join(base, path)`
 * pattern -- which silently mangled every absolute Windows path. */
sds edc_path_abs(const char *base, const char *path);

/* |path| in DISPLAY form: the platform's separators rewritten to '/' so it reads the
 * same on both platforms and can be written into the input box / a mention / a
 * message. Not for open() -- feed edc_path_norm() (or a resolved path) to the system.
 * Returns a new sds, or NULL when |path| is NULL. */
sds edc_path_display(const char *path);

/* Do |a| and |b| name the same path, up to normal form (and, on Windows, case)? Does
 * not touch the disk. false when either is NULL. */
bool edc_path_equal(const char *a, const char *b);

#endif /* EDC_PATH_H */
