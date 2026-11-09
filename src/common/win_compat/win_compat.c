/*
 * The parts of the POSIX surface that cannot live in a header.
 *
 * <unistd.h> here deliberately includes no Windows header (doing so drags in winsock.h and
 * breaks every translation unit that later includes <winsock2.h>), so anything that needs the
 * Win32 API is implemented in this file and declared in the shim header. Compiled for Windows
 * only; see the `win_compat` target in src/common/BUILD.
 */

#ifdef _MSC_VER

#include <errno.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <stdlib.h>
#include <string.h>

static wchar_t *edc_win_utf8_to_wide(const char *s)
{
    int n;
    wchar_t *w;
    if (s == NULL)
        return NULL;
    n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0)
        return NULL;
    w = (wchar_t *)malloc((size_t)n * sizeof(wchar_t));
    if (w == NULL)
        return NULL;
    if (MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n) <= 0)
    {
        free(w);
        return NULL;
    }
    return w;
}

/*
 * link(): a hard link. CreateHardLinkW needs no privilege on NTFS, so this behaves like the
 * POSIX call for the cases the tests use (two names for one file, on one volume).
 */
int link(const char *oldpath, const char *newpath)
{
    wchar_t *wold = edc_win_utf8_to_wide(oldpath);
    wchar_t *wnew = edc_win_utf8_to_wide(newpath);
    int rc;

    if (wold == NULL || wnew == NULL)
    {
        free(wold);
        free(wnew);
        errno = EINVAL;
        return -1;
    }
    rc = CreateHardLinkW(wnew, wold, NULL) ? 0 : -1;
    if (rc != 0)
        errno = EACCES;
    free(wold);
    free(wnew);
    return rc;
}

/*
 * symlink(): CreateSymbolicLinkW requires SeCreateSymbolicLinkPrivilege -- an elevated process
 * or Developer Mode. That is a real, permanent difference from POSIX rather than something to
 * paper over, so a failure is reported honestly with a non-zero return and errno, and callers
 * that check the return value skip the case (test_state_store_open already does).
 *
 * The directory flag matters: a symbolic link to a directory must be created with
 * SYMBOLIC_LINK_FLAG_DIRECTORY or it is rejected.
 */
int symlink(const char *target, const char *linkpath)
{
    wchar_t *wtarget = edc_win_utf8_to_wide(target);
    wchar_t *wlink = edc_win_utf8_to_wide(linkpath);
    DWORD attrs;
    DWORD flags = 0;
    int rc;
    DWORD err;

    if (wtarget == NULL || wlink == NULL)
    {
        free(wtarget);
        free(wlink);
        errno = EINVAL;
        return -1;
    }

    attrs = GetFileAttributesW(wtarget);
    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0)
        flags |= SYMBOLIC_LINK_FLAG_DIRECTORY;

    rc = CreateSymbolicLinkW(wlink, wtarget, flags) ? 0 : -1;
    err = GetLastError();
    if (rc != 0)
    {
        /* EPERM is the useful answer for "you are not allowed", which is the common case
         * here; POSIX would have succeeded. ENOSYS would suggest Windows lacks the call. */
        errno = (err == ERROR_PRIVILEGE_NOT_HELD) ? EPERM : EACCES;
    }
    free(wtarget);
    free(wlink);
    return rc;
}

#endif /* _MSC_VER */
