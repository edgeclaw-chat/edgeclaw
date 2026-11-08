#ifndef EDC_WIN_COMPAT_UNISTD_H
#define EDC_WIN_COMPAT_UNISTD_H

/*
 * <unistd.h> for MSVC.
 *
 * MSVC ships no <unistd.h>, so a source file that includes it -- as most POSIX-flavoured C
 * does -- fails with "fatal error C1083: cannot open include file: 'unistd.h'". This header
 * is put on the include path for Windows builds only, so those files compile unchanged
 * instead of growing an #ifdef each.
 *
 * Deliberately built out of FUNCTIONS rather than macros wherever the CRT already has a real
 * declaration: MSVC's <io.h> declares read/write/close/isatty/access/unlink/lseek and
 * <direct.h> declares chdir/getcwd under their POSIX names (the non-underscore spellings are
 * visible by default via _CRT_DECLARE_NONSTDC_NAMES). A `#define read _read` would break any
 * struct that has a member called `read` -- this project has one (edc_fold_files uses
 * `f->read[i]`), and the macro would rewrite it into `f->_read[i]`.
 *
 * This header must NOT include <windows.h>. It is pulled in very early by ordinary source
 * files, and <windows.h> drags in winsock.h; any translation unit that then includes
 * <winsock2.h> -- which lwIP and the transport code do -- gets
 *   error C2011: 'fd_set': 'struct' type redefinition (and sockaddr, timeval, hostent, ...)
 * because winsock2.h cannot be mixed with the winsock.h that already defined them. So the
 * sleep helpers below are spelled with UCRT's _sleep() instead of Sleep(), which is the only
 * reason <windows.h> would have been needed here.
 */

#ifdef _MSC_VER

#include <direct.h>
#include <io.h>
#include <process.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

/* ssize_t. <sys/types.h> defines _SSIZE_T/_ssize_t but not ssize_t, so the POSIX name has to
 * be supplied. A duplicate typedef of the same type is legal in C11, which is what this
 * project builds C with, so this does not clash with the copy in sds.h.
 *
 * SSIZE_MAX is what makes this interoperable: lwIP's <lwip/arch.h> does
 *     #ifdef SSIZE_MAX ... #include <unistd.h> ... #else typedef int ssize_t; ...
 * and MSVC's <limits.h> defines no SSIZE_MAX, so lwIP would typedef ssize_t as `int` and the
 * two definitions collide (error C2371: redefinition; different basic types) in every file
 * that includes both. Defining SSIZE_MAX puts lwIP on its unistd branch instead. Supplying it
 * is also simply correct: POSIX puts SSIZE_MAX in <limits.h>, and MSVC has no equivalent. */
#if !defined(EDC_SSIZE_T_DEFINED)
#define EDC_SSIZE_T_DEFINED
#if !defined(SSIZE_MAX)
typedef __int64 ssize_t;
#define SSIZE_MAX INT64_MAX
#endif
#endif

/* mode_t: <sys/types.h> has _mode_t, not mode_t. */
#if !defined(EDC_MODE_T_DEFINED)
#define EDC_MODE_T_DEFINED
typedef unsigned short mode_t;
#endif

#ifndef STDIN_FILENO
#define STDIN_FILENO 0
#endif
#ifndef STDOUT_FILENO
#define STDOUT_FILENO 1
#endif
#ifndef STDERR_FILENO
#define STDERR_FILENO 2
#endif

#ifndef F_OK
#define F_OK 0
#endif
#ifndef R_OK
#define R_OK 4
#endif
#ifndef W_OK
#define W_OK 2
#endif
#ifndef X_OK
#define X_OK 1
#endif

/* <limits.h> defines PATH_MAX on POSIX; MSVC's limits.h does not (paths are MAX_PATH), and
 * MAX_PATH itself is declared by <windows.h>, which this header must not include. UCRT's
 * _MAX_PATH (from <stdlib.h>) is the same 260. */
#ifndef MAX_PATH
#ifdef _MAX_PATH
#define MAX_PATH _MAX_PATH
#else
#define MAX_PATH 260
#endif
#endif
#ifndef PATH_MAX
#define PATH_MAX MAX_PATH
#endif

/* sleep()/usleep()/getpid() are declared here only if nothing else already provides them.
 * src/common/edc_compat.h has supplied usleep()/getpid() as macros for MSVC all along, and a
 * source file that includes edc_compat.h BEFORE <unistd.h> would have those macros expand
 * inside the declarations below -- `usleep(unsigned int usec)` turned into
 * `Sleep((DWORD)((unsigned int usec) / 1000))` and produced a pile of C2059/C2091. Guarding
 * on the macro name keeps whichever definition came first, and both behave the same. */
#if !defined(sleep)
static __inline unsigned int sleep(unsigned int seconds)
{
    _sleep((unsigned long)seconds * 1000u);
    return 0;
}
#endif

#if !defined(usleep)
static __inline int usleep(unsigned int usec)
{
    _sleep((unsigned long)(usec / 1000u));
    return 0;
}
#endif

#if !defined(getpid)
static __inline int getpid(void)
{
    return (int)_getpid();
}
#endif

/* setenv()/unsetenv() are declared in <stdlib.h> on POSIX and absent from the CRT. Defined
 * here because this is the one shim every POSIX-flavoured source file already includes; the
 * edc_compat.h macros, when present, win because of the guards. */
#if !defined(setenv)
static __inline int setenv(const char *name, const char *value, int overwrite)
{
    if (!overwrite && getenv(name) != NULL)
        return 0;
    return _putenv_s(name, value) == 0 ? 0 : -1;
}
#endif

#if !defined(unsetenv)
static __inline int unsetenv(const char *name)
{
    return _putenv_s(name, "") == 0 ? 0 : -1;
}
#endif

/* strtok_r() is POSIX; MSVC spells the reentrant variant strtok_s with the same semantics and
 * argument order, so this is a rename rather than a reimplementation. */
#if !defined(strtok_r)
#define strtok_r(str, delim, save) strtok_s((str), (delim), (save))
#endif

/* realpath() is absent. edc_compat.h supplies a macro when it is included; this is the
 * fallback for translation units that do not include it. Caller frees a NULL `resolved`. */
#if !defined(realpath)
static __inline char *realpath(const char *path, char *resolved)
{
    char *buf = resolved;
    if (buf == NULL)
    {
        buf = (char *)malloc((size_t)PATH_MAX);
        if (buf == NULL)
            return NULL;
    }
    if (_fullpath(buf, path, (size_t)PATH_MAX) == NULL)
    {
        if (resolved == NULL)
            free(buf);
        return NULL;
    }
    return buf;
}
#endif

/* mkdtemp(): create a directory from a template ending in "XXXXXX", replacing the X's with a
 * unique suffix. Used by most of the test suite, so its absence accounted for 67 of the 70
 * test binaries that would not link on Windows.
 *
 * Races are resolved the way POSIX does: _mkdir() fails with EEXIST if another process took
 * the name, and the loop retries with a different suffix rather than failing. */
#if !defined(mkdtemp)
static __inline char *mkdtemp(char *tmpl)
{
    static const char alphabet[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    const size_t alpha_n = sizeof(alphabet) - 1;
    const char suffix[] = "XXXXXX";
    size_t len;
    int attempt, i;

    if (tmpl == NULL)
        return NULL;
    len = strlen(tmpl);
    if (len < sizeof(suffix) - 1 || strcmp(tmpl + len - (sizeof(suffix) - 1), suffix) != 0)
        return NULL;

    for (attempt = 0; attempt < 100; attempt++)
    {
        /* Seeded from the pid, the clock and the attempt so that two processes templating in
         * the same directory do not walk the same sequence. */
        unsigned long r = (unsigned long)_getpid() * 2654435761u;
        r ^= (unsigned long)time(NULL) + (unsigned long)attempt * 40503u;
        for (i = 0; i < (int)(sizeof(suffix) - 1); i++)
        {
            r = r * 1103515245u + 12345u;
            tmpl[len - (sizeof(suffix) - 1) + (size_t)i] = alphabet[(r >> 16) % alpha_n];
        }
        if (_mkdir(tmpl) == 0)
            return tmpl;
        if (errno != EEXIST)
            return NULL;
    }
    return NULL;
}
#endif

#endif /* _MSC_VER */

#endif /* EDC_WIN_COMPAT_UNISTD_H */
