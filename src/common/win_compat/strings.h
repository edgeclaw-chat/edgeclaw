#ifndef EDC_WIN_COMPAT_STRINGS_H
#define EDC_WIN_COMPAT_STRINGS_H

/*
 * <strings.h> for MSVC -- the BSD case-insensitive comparisons.
 *
 * MSVC has _stricmp/_strnicmp and nothing under the POSIX names. Functions rather than
 * macros, so that a struct member or variable named `strcasecmp` (this project has neither,
 * but the next one might) is not rewritten behind the author's back.
 */

#ifdef _MSC_VER

#include <string.h>

static __inline int strcasecmp(const char *a, const char *b)
{
    return _stricmp(a, b);
}

static __inline int strncasecmp(const char *a, const char *b, size_t n)
{
    return _strnicmp(a, b, n);
}

#endif /* _MSC_VER */

#endif /* EDC_WIN_COMPAT_STRINGS_H */
