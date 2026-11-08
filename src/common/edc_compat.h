#ifndef EDC_COMPAT_H
#define EDC_COMPAT_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/types.h>

/*
 * Transitional compatibility header for migrating away from GLib,
 * and for cross-platform (POSIX / Windows) support.
 *
 * All definitions are guarded so this header can be safely included
 * alongside <glib.h> during the transition period.
 */

/* =========================================================================
 *  Windows platform compatibility layer
 * ========================================================================= */

#ifdef _WIN32

/* ---- Windows SDK headers ---- */
/* clang-format off */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <io.h>
#include <process.h>
#include <direct.h>
/* clang-format on */

/* ---- basic types ---- */
typedef SSIZE_T ssize_t;

/* POSIX puts SSIZE_MAX in <limits.h>; MSVC defines neither that nor ssize_t. Supplying it
 * here is not cosmetic: lwIP's <lwip/arch.h> chooses between taking ssize_t from <unistd.h>
 * and `typedef int ssize_t` by testing #ifdef SSIZE_MAX. With the macro missing, lwIP would
 * typedef it as `int` on top of the SSIZE_T above -- error C2371: redefinition; different
 * basic types -- in any file that includes this header and then a lwIP one. Every header in
 * this project that supplies ssize_t therefore has to supply SSIZE_MAX with it
 * (see src/common/win_compat/unistd.h and the sds patch). */
#ifndef SSIZE_MAX
#define SSIZE_MAX (((ssize_t)((((size_t)-1) >> 1))))
#endif

#ifndef PATH_MAX
#define PATH_MAX MAX_PATH
#endif

/* ---- string helpers ---- */
#define strcasecmp(s1, s2) _stricmp(s1, s2)
#define strncasecmp(s1, s2, n) _strnicmp(s1, s2, n)
#ifndef strdup
#define strdup(s) _strdup(s)
#endif
#ifndef strndup
static inline char *win32_strndup(const char *s, size_t n)
{
    size_t len = strnlen(s, n);
    char *d = (char *)malloc(len + 1);
    if (d == NULL)
        return NULL;
    memcpy(d, s, len);
    d[len] = '\0';
    return d;
}
#define strndup(s, n) win32_strndup(s, n)
#endif
#define mkdir(path, mode) _mkdir(path)

/* ---- process ---- */
#define getpid() GetCurrentProcessId()

/* ---- sleep ---- */
#ifdef __GNUC__
#include <unistd.h>
#else
#define usleep(usec) Sleep((DWORD)((usec) / 1000))
#endif

/* ---- I/O ---- */
#ifdef _WIN32
#include <stdio.h>
static inline ssize_t win32_getline(char **lineptr, size_t *n, FILE *stream)
{
    if (*lineptr == NULL || *n == 0)
    {
        *n = 128;
        *lineptr = (char *)malloc(*n);
        if (*lineptr == NULL)
            return -1;
    }
    size_t pos = 0;
    for (;;)
    {
        int c = fgetc(stream);
        if (c == EOF)
        {
            if (pos == 0)
                return -1;
            break;
        }
        if (pos + 1 >= *n)
        {
            *n *= 2;
            char *tmp = (char *)realloc(*lineptr, *n);
            if (tmp == NULL)
                return -1;
            *lineptr = tmp;
        }
        (*lineptr)[pos++] = (char)c;
        if (c == '\n')
            break;
    }
    (*lineptr)[pos] = '\0';
    return (ssize_t)pos;
}
#define getline(lineptr, n, stream) win32_getline(lineptr, n, stream)
#endif

/* ---- path ---- */
static inline char *win32_realpath(const char *path, char *resolved)
{
    int need_free = 0;
    if (!resolved)
    {
        resolved = (char *)malloc(PATH_MAX);
        if (!resolved)
            return NULL;
        need_free = 1;
    }
    if (!_fullpath(resolved, path, PATH_MAX))
    {
        if (need_free)
            free(resolved);
        return NULL;
    }
    /* _fullpath only NORMALISES the string: it happily returns an absolute path for something
     * that does not exist. POSIX realpath() fails with ENOENT instead, and callers rely on
     * that -- "does this directory exist?" is answered by realpath() returning NULL all over
     * this codebase (edc_file_index_query, edc_path_guard_resolve, the sandbox checks).
     * Without this test a missing directory looks present on Windows and the caller goes on
     * to read it. _access(path, 0) is the existence check; use the ORIGINAL path, because the
     * normalised one can differ in case on a case-insensitive filesystem. */
    if (_access(path, 0) != 0)
    {
        if (need_free)
            free(resolved);
        errno = ENOENT;
        return NULL;
    }
    return resolved;
}
#define realpath(p, r) win32_realpath(p, r)

/* ---- environment ---- */
/* unsetenv must reach the CRT, not just Win32: getenv() reads the CRT's copy of the
 * environment, and SetEnvironmentVariable(name, NULL) only updates the Win32 block. With that
 * version a variable looked unset to the OS and still set to the program -- which is how
 * test_models_catalog kept loading its fixture after unsetenv("EDGECLAW_MODELS_CATALOG").
 * _putenv_s(name, "") is the call that removes it from the CRT's environment. */
#define unsetenv(name) _putenv_s((name), "")
static inline int win32_setenv(const char *name, const char *value, int overwrite)
{
    (void)overwrite;
    SetEnvironmentVariable(name, value);
    return _putenv_s(name, value) == 0 ? 0 : -1;
}
#define setenv(name, value, overwrite) win32_setenv(name, value, overwrite)

/* ---- binary mode flag (Windows text-mode open() breaks binary I/O) ---- */
#ifndef O_BINARY
#ifdef _WIN32
#define O_BINARY _O_BINARY
#else
#define O_BINARY 0
#endif
#endif

/* ---- UTF-8 path support ---- */
/* The Windows ANSI CRT APIs (stat/open/fopen) interpret paths using the
 * local code page, which mangles non-ASCII (e.g. Chinese) file names.
 * Convert UTF-8 paths to UTF-16 so wide-char APIs (_wstat64/_wopen) can
 * handle them correctly. Caller must free() the returned buffer. */
#ifdef _WIN32
#include <wchar.h>
static inline wchar_t *edc_utf8_to_wide(const char *utf8)
{
    if (!utf8)
        return NULL;
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    if (len <= 0)
        return NULL;
    wchar_t *wide = (wchar_t *)malloc((size_t)len * sizeof(wchar_t));
    if (!wide)
        return NULL;
    if (MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide, len) <= 0)
    {
        free(wide);
        return NULL;
    }
    return wide;
}
#endif /* _WIN32 */

/* mode_t: <sys/types.h> has _mode_t, not mode_t. Shared guard with src/common/win_compat's
 * <unistd.h>, so a file including both does not define it twice. */
#if !defined(EDC_MODE_T_DEFINED)
#define EDC_MODE_T_DEFINED
typedef unsigned short mode_t;
#endif

/* POSIX permission bits. MSVC's <sys/stat.h> spells them _S_IREAD/_S_IWRITE/_S_IEXEC and has
 * no S_IXUSR at all, while code ported from POSIX tests S_IXUSR directly. */
#ifndef S_IRUSR
#define S_IRUSR _S_IREAD
#endif
#ifndef S_IWUSR
#define S_IWUSR _S_IWRITE
#endif
#ifndef S_IXUSR
#define S_IXUSR _S_IEXEC
#endif

/* ---- file mode macros (Windows _S_IF*) ---- */
#ifndef S_ISDIR
#define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
#endif
#ifndef S_ISREG
#define S_ISREG(m) (((m) & _S_IFMT) == _S_IFREG)
#endif
#ifndef S_ISLNK
#define S_ISLNK(m) (0)
#endif
#ifndef lstat
#define lstat(path, buf) stat(path, buf)
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

/* ---- terminal ---- */
#define isatty(fd) _isatty(fd)

/* ---- file descriptors (Windows console handles use 0/1/2 too) ---- */
#ifndef STDIN_FILENO
#define STDIN_FILENO 0
#endif
#ifndef STDOUT_FILENO
#define STDOUT_FILENO 1
#endif
#ifndef STDERR_FILENO
#define STDERR_FILENO 2
#endif

/* ---- directory entry types (for edc_file_utils edc_dir_foreach) ---- */
#ifndef DT_UNKNOWN
#define DT_UNKNOWN 0
#endif
#ifndef DT_DIR
#define DT_DIR 4
#endif
#ifndef DT_REG
#define DT_REG 8
#endif
#ifndef DT_LNK
#define DT_LNK 10
#endif

#define PATH_SEP '\\'

#else /* !_WIN32 — POSIX */

/* ---- POSIX headers commonly needed across the project ---- */
#include <unistd.h>

#define PATH_SEP '/'

#endif /* _WIN32 */

/* =========================================================================
 *  Time
 * ========================================================================= */

/* localtime_r / gmtime_r as FUNCTIONS with the POSIX signature on every platform.
 *
 * These cannot be `#define`s: the platforms disagree on the SHAPE of the call -- POSIX
 * returns struct tm *, the Windows CRT returns errno_t and takes the arguments in the
 * opposite order -- so a macro is only usable where the return value is ignored.
 * `return localtime_r(&t, out) != NULL;` expanded to `errno_t != NULL`, which is always
 * true, and that is why four files kept their own #ifdef copy instead of using it.
 * A caller asks for a pointer here and gets one, on both platforms. */
static inline struct tm *edc_localtime_r(const time_t *t, struct tm *out)
{
#ifdef _WIN32
    return localtime_s(out, t) == 0 ? out : NULL;
#else
    return localtime_r(t, out);
#endif
}

static inline struct tm *edc_gmtime_r(const time_t *t, struct tm *out)
{
#ifdef _WIN32
    return gmtime_s(out, t) == 0 ? out : NULL;
#else
    return gmtime_r(t, out);
#endif
}

/* =========================================================================
 *  Platform capabilities
 * ========================================================================= */

/* What this build can do lives in edc_platform.h: one table of CHOICES (is there a
 * kernel sandbox, which shell to spawn, how to start it) and the few OPERATIONS whose
 * implementation differs per platform (mlock, signalling a process group). This header
 * kept the single yes/no answer while there was exactly one question; several later,
 * "ask the platform" needed a home of its own. */

/* =========================================================================
 *  GLib compatibility (transitional — remove once all callers use EDC_*)
 * ========================================================================= */

#ifndef true
#define true true
#endif
#ifndef false
#define false false
#endif

#ifndef G_BEGIN_DECLS
typedef char gchar;
typedef int gint;
typedef bool gboolean;
typedef int64_t gint64;
typedef int32_t gint32;
typedef unsigned int guint;
typedef unsigned char guchar;
typedef unsigned long gulong;
typedef void *gpointer;
typedef const void *gconstpointer;
typedef size_t gsize;
typedef ssize_t gssize;

#define g_new(type, n) ((type *)malloc((n) * sizeof(type)))
#define g_new0(type, n) ((type *)calloc((n), sizeof(type)))
#define g_malloc0(n) calloc(1, (n))
#define g_free free
#endif

#ifndef G_N_ELEMENTS
#define G_N_ELEMENTS(arr) (sizeof(arr) / sizeof((arr)[0]))
#endif

#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#endif

#ifndef G_MAXINT64
#define G_MAXINT64 INT64_MAX
#endif
#ifndef G_MININT64
#define G_MININT64 INT64_MIN
#endif

#define EDC_NEW(type, n) ((type *)malloc((n) * sizeof(type)))
#define EDC_NEW0(type, n) ((type *)calloc((n), sizeof(type)))
#define EDC_MALLOC0(n) calloc(1, (n))

#ifdef _WIN32
#define timegm _mkgmtime
#endif

/* Path semantics live in edc_path.h (separator, absolute, containment, normal form).
 * They were moved out of this transitional header because nothing treated them as the
 * one implementation while they sat here -- three copies had already grown elsewhere.
 * This header keeps only what is a PLATFORM fact (PATH_SEP) or a libc shim. */

#endif /* EDC_COMPAT_H */
