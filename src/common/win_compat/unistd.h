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
 */

#ifdef _MSC_VER

#include <direct.h>
#include <io.h>
#include <process.h>
#include <sys/stat.h>
#include <sys/types.h>

/* ssize_t. <sys/types.h> defines _SSIZE_T/_ssize_t but not ssize_t, so the POSIX name has to
 * be supplied. A duplicate typedef of the same type is legal in C11, which is what this
 * project builds C with, so this does not clash with the copy in sds.h. */
#if !defined(EDC_SSIZE_T_DEFINED)
#define EDC_SSIZE_T_DEFINED
typedef __int64 ssize_t;
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

/* <limits.h> defines PATH_MAX on POSIX; MSVC's limits.h does not (paths are MAX_PATH). */
#ifndef PATH_MAX
#define PATH_MAX MAX_PATH
#endif

/* Owner permission bits. MSVC's <sys/stat.h> spells these _S_IREAD/_S_IWRITE/_S_IEXEC and
 * has no S_IXUSR at all, while the POSIX S_IRUSR/S_IWUSR may or may not exist. */
#ifndef S_IXUSR
#define S_IXUSR _S_IEXEC
#endif
#ifndef S_IWUSR
#define S_IWUSR _S_IWRITE
#endif
#ifndef S_IRUSR
#define S_IRUSR _S_IREAD
#endif

/* No POSIX sleep()/usleep() in the CRT. Both return 0 like their POSIX counterparts so that
 * `while (usleep(...) == -1 && errno == EINTR)` style call sites keep working. */
#include <windows.h>

/* sleep()/usleep()/getpid() are declared here only if nothing else already provides them.
 * src/common/edc_compat.h has supplied usleep()/getpid() as macros for MSVC all along, and a
 * source file that includes edc_compat.h BEFORE <unistd.h> would have those macros expand
 * inside the declarations below -- `usleep(unsigned int usec)` turned into
 * `Sleep((DWORD)((unsigned int usec) / 1000))` and produced a pile of C2059/C2091. Guarding
 * on the macro name keeps whichever definition came first, and both behave the same. */
#if !defined(sleep)
static __inline unsigned int sleep(unsigned int seconds)
{
    Sleep((DWORD)seconds * 1000u);
    return 0;
}
#endif

#if !defined(usleep)
static __inline int usleep(unsigned int usec)
{
    Sleep((DWORD)(usec / 1000u));
    return 0;
}
#endif

#if !defined(getpid)
static __inline int getpid(void)
{
    return (int)_getpid();
}
#endif

#endif /* _MSC_VER */

#endif /* EDC_WIN_COMPAT_UNISTD_H */
