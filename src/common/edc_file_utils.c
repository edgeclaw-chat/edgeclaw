#include "edc_file_utils.h"
#include "edc_path_guard.h"
#include "edc_compat.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif

static uv_loop_t *g_file_loop = NULL;

void edc_file_utils_init(uv_loop_t *loop)
{
    g_file_loop = loop;
}

sds edc_file_read(const char *path, EdcError **error)
{
    uv_fs_t req;
    int fd = (int)uv_fs_open(g_file_loop, &req, path, O_RDONLY, 0, NULL);
    if (fd < 0)
    {
        edc_error_set(error, EDC_ERR_FAILED, "open '%s' failed: %s", path, uv_strerror(fd));
        uv_fs_req_cleanup(&req);
        return NULL;
    }
    uv_fs_req_cleanup(&req);

    int r = (int)uv_fs_fstat(g_file_loop, &req, fd, NULL);
    if (r < 0)
    {
        edc_error_set(error, EDC_ERR_FAILED, "stat '%s' failed: %s", path, uv_strerror(r));
        uv_fs_req_cleanup(&req);
        uv_fs_t cr;
        uv_fs_close(g_file_loop, &cr, fd, NULL);
        uv_fs_req_cleanup(&cr);
        return NULL;
    }
    size_t file_size = (size_t)req.statbuf.st_size;
    uv_fs_req_cleanup(&req);

    char *buf = malloc(file_size + 1);
    if (!buf)
    {
        edc_error_set(error, EDC_ERR_FAILED, "malloc failed for '%s'", path);
        uv_fs_t cr;
        uv_fs_close(g_file_loop, &cr, fd, NULL);
        uv_fs_req_cleanup(&cr);
        return NULL;
    }

    uv_buf_t iov = uv_buf_init(buf, file_size);
    r = (int)uv_fs_read(g_file_loop, &req, fd, &iov, 1, 0, NULL);
    uv_fs_req_cleanup(&req);

    uv_fs_t cr;
    uv_fs_close(g_file_loop, &cr, fd, NULL);
    uv_fs_req_cleanup(&cr);

    if (r < 0)
    {
        free(buf);
        edc_error_set(error, EDC_ERR_FAILED, "read '%s' failed: %s", path, uv_strerror(r));
        return NULL;
    }

    buf[r] = '\0';
    sds result = sdsnewlen(buf, (size_t)r);
    free(buf);
    return result;
}

bool edc_file_write(const char *path, const char *content, ssize_t len, EdcError **error)
{
    size_t write_len = (len < 0) ? strlen(content) : (size_t)len;

    uv_fs_t req;
    int fd = (int)uv_fs_open(g_file_loop, &req, path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0644, NULL);
    if (fd < 0)
    {
        edc_error_set(error, EDC_ERR_FAILED, "open '%s' for write failed: %s", path, uv_strerror(fd));
        uv_fs_req_cleanup(&req);
        return false;
    }
    uv_fs_req_cleanup(&req);

    uv_buf_t iov = uv_buf_init((char *)content, write_len);
    int r = (int)uv_fs_write(g_file_loop, &req, fd, &iov, 1, 0, NULL);
    uv_fs_req_cleanup(&req);

    uv_fs_t cr;
    uv_fs_close(g_file_loop, &cr, fd, NULL);
    uv_fs_req_cleanup(&cr);

    if (r < 0)
    {
        edc_error_set(error, EDC_ERR_FAILED, "write '%s' failed: %s", path, uv_strerror(r));
        return false;
    }
    return true;
}

bool edc_file_rename_replace(const char *src, const char *dst, EdcError **error)
{
    if (!src || !dst)
    {
        edc_error_set(error, EDC_ERR_INVAL, "rename: NULL path");
        return false;
    }
#ifdef _WIN32
    /* The CRT's rename() refuses to overwrite (errno EEXIST), which breaks every
     * ".tmp -> real" state save after the first one. MoveFileExW replaces the
     * destination and takes UTF-8 paths through the wide API. */
    wchar_t *wsrc = edc_utf8_to_wide(src);
    wchar_t *wdst = edc_utf8_to_wide(dst);
    BOOL ok = FALSE;
    DWORD err = ERROR_INVALID_PARAMETER;
    if (wsrc && wdst)
    {
        ok = MoveFileExW(wsrc, wdst, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
        if (!ok)
            err = GetLastError();
    }
    free(wsrc);
    free(wdst);
    if (!ok)
    {
        edc_error_set(error, EDC_ERR_FAILED, "rename '%s' -> '%s' failed: Windows error %lu", src, dst,
                      (unsigned long)err);
        return false;
    }
    return true;
#else
    if (rename(src, dst) != 0)
    {
        edc_error_set(error, EDC_ERR_FAILED, "rename '%s' -> '%s' failed: %s", src, dst, strerror(errno));
        return false;
    }
    return true;
#endif
}

/* True when the resolved parent directory of |path| lives inside |root|.
 * |root| == NULL/"" disables the check. A parent that cannot be resolved (e.g.
 * does not exist yet) fails closed. */
static bool edc_path_parent_within(const char *path, const char *root)
{
    if (!root || !root[0])
        return true;

    sds parent = edc_path_dirname(path);
    char *pr = realpath(parent, NULL);
    char *rr = realpath(root, NULL);
    sdsfree(parent);

    bool ok = false;
    if (pr && rr)
    {
        size_t rl = strlen(rr);
        ok = strncmp(pr, rr, rl) == 0 && (pr[rl] == '\0' || pr[rl] == '/');
    }
    free(pr);
    free(rr);
    return ok;
}

static int edc_fopen_flags_from_mode(const char *mode)
{
    bool plus = strchr(mode, '+') != NULL;
    int flags;
    if (mode[0] == 'a')
        flags = O_WRONLY | O_CREAT | O_APPEND;
    else if (mode[0] == 'w')
        flags = O_WRONLY | O_CREAT | O_TRUNC;
    else
        flags = O_WRONLY | O_CREAT;
    return plus ? ((flags & ~O_WRONLY) | O_RDWR) : flags;
}

FILE *edc_file_fopen_safe(const char *path, const char *mode, const char *root)
{
    if (!path || !mode)
        return NULL;

    /* The rules live in edc_path_guard: symlink refused, target contained in |root|, a
     * file that does not exist yet resolved through its parent. |conf| NULL = no denylist
     * here; the guarded READERS are the ones that consult it. */
    struct edc_path_rules rules = {.root = root, .writable = true, .refuse_symlink = true};
    sds resolved = edc_path_guard_resolve(&rules, path, NULL);
    if (!resolved)
    {
        errno = EACCES;
        return NULL;
    }

    int fd = open(resolved, edc_fopen_flags_from_mode(mode) | O_NOFOLLOW, 0644);
    sdsfree(resolved);
    if (fd < 0)
        return NULL;

    FILE *f = fdopen(fd, mode);
    if (!f)
        close(fd);
    return f;
}

bool edc_file_write_safe(const char *path, const char *content, ssize_t len, const char *root, EdcError **error)
{
    if (!path || !content)
        return false;

    struct stat lst;
    if (lstat(path, &lst) == 0 && S_ISLNK(lst.st_mode))
    {
        edc_error_set(error, EDC_ERR_SANDBOX, "refusing to write through symlink '%s'", path);
        return false;
    }
    if (!edc_path_parent_within(path, root))
    {
        edc_error_set(error, EDC_ERR_SANDBOX, "refusing to write outside '%s': '%s'", root, path);
        return false;
    }
    return edc_file_write(path, content, len, error);
}

static int edc_file_test_flags(const char *path)
{
    uv_fs_t req;
    int r = (int)uv_fs_stat(g_file_loop, &req, path, NULL);
    uv_fs_req_cleanup(&req);
    return (r == 0) ? (int)req.statbuf.st_mode : 0;
}

bool edc_file_exists(const char *path)
{
    return edc_file_test_flags(path) != 0;
}

bool edc_file_is_dir(const char *path)
{
    return S_ISDIR(edc_file_test_flags(path));
}

bool edc_file_is_regular(const char *path)
{
    return S_ISREG(edc_file_test_flags(path));
}

bool edc_dir_create_with_parents(const char *path, int mode, EdcError **error)
{
    sds partial = sdsempty();
    const char *p = path;
    bool is_absolute = (*p == '/');

    if (is_absolute)
    {
        partial = sdscat(partial, "/");
        while (*p == '/')
            p++;
    }

    while (*p)
    {
        while (*p == '/')
            p++;
        const char *next = strchr(p, '/');
        size_t seg_len = next ? (size_t)(next - p) : strlen(p);
        if (seg_len == 0)
            break;

        if (sdslen(partial) > 0 && partial[sdslen(partial) - 1] != '/')
            partial = sdscat(partial, "/");
        partial = sdscatlen(partial, p, seg_len);

        uv_fs_t req;
        int r = (int)uv_fs_mkdir(g_file_loop, &req, partial, mode, NULL);
        uv_fs_req_cleanup(&req);
        if (r < 0 && r != UV_EEXIST) /* libuv codes are not -errno; UV_EEXIST is -4075 on Windows, not -EEXIST */
        {
            edc_error_set(error, EDC_ERR_FAILED, "mkdir '%s' failed: %s", partial, uv_strerror(r));
            sdsfree(partial);
            return false;
        }

        p = next ? next + 1 : p + seg_len;
    }

    sdsfree(partial);
    return true;
}

bool edc_dir_foreach(const char *path, edc_dir_entry_cb cb, void *userdata, EdcError **error)
{
#ifdef _WIN32
    sds pattern = edc_path_join(path, "*", NULL);
    WIN32_FIND_DATA fd;
    HANDLE hFind = FindFirstFile(pattern, &fd);
    sdsfree(pattern);

    if (hFind == INVALID_HANDLE_VALUE)
    {
        edc_error_set(error, EDC_ERR_FAILED, "FindFirstFile '%s' failed: %lu", path, GetLastError());
        return false;
    }

    do
    {
        int type = UV_DIRENT_UNKNOWN;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            type = UV_DIRENT_DIR;
        else
            type = UV_DIRENT_FILE;
        cb(fd.cFileName, type, userdata);
    } while (FindNextFile(hFind, &fd));

    FindClose(hFind);
    return true;
#else
    DIR *dir = opendir(path);
    if (!dir)
    {
        edc_error_set(error, EDC_ERR_FAILED, "opendir '%s' failed: %s", path, strerror(errno));
        return false;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)))
    {
        int type = UV_DIRENT_UNKNOWN;
        if (entry->d_type == DT_DIR)
            type = UV_DIRENT_DIR;
        else if (entry->d_type == DT_REG)
            type = UV_DIRENT_FILE;
        else if (entry->d_type == DT_LNK)
            type = UV_DIRENT_LINK;
        cb(entry->d_name, type, userdata);
    }

    closedir(dir);
    return true;
#endif
}

sds edc_path_join(const char *first, ...)
{
#ifdef _WIN32
    const char *sep = "\\";
    const char *alt_sep = "/";
#else
    const char *sep = "/";
#endif
    va_list args;
    va_start(args, first);

    sds result = sdsempty();
    bool need_sep = false;

    for (const char *seg = first; seg; seg = va_arg(args, const char *))
    {
        if (*seg == '\0')
            continue;
        if (need_sep)
            result = sdscat(result, sep);
        result = sdscat(result, seg);
        need_sep = true;
    }

    va_end(args);
#ifdef _WIN32
    {
        size_t len = sdslen(result);
        for (size_t i = 0; i < len; i++)
            if (result[i] == '/')
                result[i] = '\\';
    }
#endif
    return result;
}

sds edc_path_dirname(const char *path)
{
    if (!path)
        return sdsempty();
#ifdef _WIN32
    const char *last_slash = strrchr(path, '/');
    const char *last_bslash = strrchr(path, '\\');
    const char *last = last_slash > last_bslash ? last_slash : last_bslash;
#else
    const char *last = strrchr(path, '/');
#endif
    if (!last)
        return sdsnew(".");
    if (last == path)
        return sdsnewlen(path, 1);
    return sdsnewlen(path, (size_t)(last - path));
}

sds edc_cwd(void)
{
    char buf[4096];
    size_t size = sizeof(buf);
    int r = uv_cwd(buf, &size);
    if (r != 0)
        return sdsempty();
    return sdsnewlen(buf, size);
}

sds edc_homedir(void)
{
    char buf[4096];
    size_t size = sizeof(buf);
    int r = uv_os_homedir(buf, &size);
    if (r != 0)
        return sdsempty();
    return sdsnewlen(buf, size);
}

sds edc_os_name(void)
{
    uv_utsname_t info;
    int r = uv_os_uname(&info);
    if (r != 0)
        return sdsnew("Unknown");
    return sdsnew(info.sysname);
}
