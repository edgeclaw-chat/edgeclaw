#ifndef EDC_COMPAT_H
#define EDC_COMPAT_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
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

#ifndef PATH_MAX
#define PATH_MAX MAX_PATH
#endif

/* ---- string helpers ---- */
#define strcasecmp(s1, s2)     _stricmp(s1, s2)
#define strncasecmp(s1, s2, n) _strnicmp(s1, s2, n)
#ifndef strdup
#define strdup(s)              _strdup(s)
#endif

/* ---- process ---- */
#define getpid()               GetCurrentProcessId()

/* ---- sleep ---- */
#define usleep(usec)           Sleep((DWORD)((usec) / 1000))

/* ---- path ---- */
static inline char *win32_realpath(const char *path, char *resolved)
{
    if (!_fullpath(resolved, path, PATH_MAX))
        return NULL;
    return resolved;
}
#define realpath(p, r)         win32_realpath(p, r)

/* ---- environment ---- */
#define unsetenv(name)         SetEnvironmentVariable(name, NULL)

/* ---- file mode macros (Windows _S_IF*) ---- */
#ifndef S_ISDIR
#define S_ISDIR(m)  (((m) & _S_IFMT) == _S_IFDIR)
#endif
#ifndef S_ISREG
#define S_ISREG(m)  (((m) & _S_IFMT) == _S_IFREG)
#endif
#ifndef S_ISLNK
#define S_ISLNK(m)  (0)
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
#define isatty(fd)             _isatty(fd)

/* ---- file descriptors (Windows console handles use 0/1/2 too) ---- */
#ifndef STDIN_FILENO
#define STDIN_FILENO  0
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
#define DT_DIR     4
#endif
#ifndef DT_REG
#define DT_REG     8
#endif
#ifndef DT_LNK
#define DT_LNK    10
#endif

#else  /* !_WIN32 — POSIX */

/* ---- POSIX headers commonly needed across the project ---- */
#include <unistd.h>

#endif /* _WIN32 */

/* =========================================================================
 *  GLib compatibility (transitional — remove once all callers use EDC_*)
 * ========================================================================= */

#ifndef TRUE
#define TRUE  true
#endif
#ifndef FALSE
#define FALSE false
#endif

#ifndef G_BEGIN_DECLS
typedef char  gchar;
typedef int   gint;
typedef bool  gboolean;
typedef int64_t gint64;
typedef int32_t gint32;
typedef unsigned int  guint;
typedef unsigned char guchar;
typedef unsigned long gulong;
typedef void* gpointer;
typedef const void* gconstpointer;
typedef size_t gsize;
typedef ssize_t gssize;

#define g_new(type, n)     ((type*)malloc((n) * sizeof(type)))
#define g_new0(type, n)    ((type*)calloc((n), sizeof(type)))
#define g_malloc0(n)       calloc(1, (n))
#define g_free             free
#endif

#ifndef G_N_ELEMENTS
#define G_N_ELEMENTS(arr) (sizeof(arr) / sizeof((arr)[0]))
#endif

#ifndef G_MAXINT64
#define G_MAXINT64 INT64_MAX
#endif
#ifndef G_MININT64
#define G_MININT64 INT64_MIN
#endif

#define EDC_NEW(type, n)   ((type*)malloc((n) * sizeof(type)))
#define EDC_NEW0(type, n)  ((type*)calloc((n), sizeof(type)))
#define EDC_MALLOC0(n)     calloc(1, (n))

#ifdef _WIN32
#define timegm _mkgmtime
#endif

#endif /* EDC_COMPAT_H */
