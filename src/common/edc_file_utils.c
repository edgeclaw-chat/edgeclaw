#include "edc_file_utils.h"
#include "edc_path_guard.h"
#include "edc_compat.h"
#include "edc_path.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
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

/* edc_file_read() with a ceiling, decided BEFORE any memory is allocated.
 *
 * This exists because "read the whole file" is not a safe primitive when the file is chosen by
 * the model or pasted by the user: a 10 GB file is a 10 GB malloc. The stat comes first, so an
 * oversized file costs one syscall instead of an out-of-memory death.
 *
 * |max_bytes| = 0 means "no ceiling" (the caller has its own reason to trust the size).
 * |out_size| receives the real size whenever it could be determined -- including when the call
 * is refused, so the message can say HOW big the file is and what to do about it. */
sds edc_file_read_capped(const char *path, size_t max_bytes, size_t *out_size, EdcError **error)
{
    if (out_size)
        *out_size = 0;

    uv_fs_t req;
    int r = (int)uv_fs_stat(g_file_loop, &req, path, NULL);
    if (r < 0)
    {
        edc_error_set(error, EDC_ERR_FAILED, "stat '%s' failed: %s", path, uv_strerror(r));
        uv_fs_req_cleanup(&req);
        return NULL;
    }
    const size_t size = (size_t)req.statbuf.st_size;
    const bool is_dir = (req.statbuf.st_mode & S_IFDIR) != 0;
    uv_fs_req_cleanup(&req);

    if (out_size)
        *out_size = size;
    if (is_dir)
    {
        edc_error_set(error, EDC_ERR_INVAL, "'%s' is a directory", path);
        return NULL;
    }
    if (max_bytes > 0 && size > max_bytes)
    {
        edc_error_set(error, EDC_ERR_INVAL, "'%s' is %zu bytes, over the %zu-byte limit", path, size, max_bytes);
        return NULL;
    }
    return edc_file_read(path, error);
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
        ok = edc_path_is_within(pr, rr);
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

bool edc_file_restrict_owner(const char *path)
{
#ifdef _WIN32
    /* Mode bits are not the mechanism here, and fabricating an ACL is out of scope. */
    (void)path;
    return true;
#else
    if (!path || !path[0])
        return false;

    struct stat st;
    if (stat(path, &st) != 0)
        return false;

    const mode_t want = S_ISDIR(st.st_mode) ? 0700 : 0600;
    if ((st.st_mode & 0777) == want)
        return true;
    return chmod(path, want) == 0;
#endif
}

bool edc_dir_create_with_parents(const char *path, int mode, EdcError **error)
{
    if (!path || !path[0])
    {
        edc_error_set(error, EDC_ERR_INVAL, "mkdir: empty path");
        return false;
    }

    sds partial = sdsempty();
    const char *p = path;

    /* Carry the root into the first segment instead of creating it: "/" on POSIX, a
     * drive ("C:") or a UNC share ("\\server\share") on Windows. Everything below is
     * created one level at a time -- splitting on '/' only handed a whole Windows path
     * to mkdir() as a single segment, which fails as soon as more than the last level
     * is missing (it only ever worked because EEXIST was tolerated). */
    if (edc_path_is_absolute(path))
    {
        if (edc_path_is_sep(p[0]) && edc_path_is_sep(p[1]))
        {
            const char *q = p + 2;
            for (int part = 0; part < 2 && *q; part++) /* \\server\share */
            {
                while (*q && !edc_path_is_sep(*q))
                    q++;
                while (edc_path_is_sep(*q))
                    q++;
            }
            partial = sdscatlen(partial, p, (size_t)(q - p));
            p = q;
        }
        else if (edc_path_is_sep(*p))
        {
            partial = sdscatlen(partial, p, 1); /* the rooted separator, kept as written */
            while (edc_path_is_sep(*p))
                p++;
        }
        else
        {
            p += 2; /* "C:" */
            if (edc_path_is_sep(*p))
                p++;
            partial = sdscatlen(partial, path, (size_t)(p - path));
        }
    }

    while (*p)
    {
        while (edc_path_is_sep(*p))
            p++;
        if (!*p)
            break;

        const char *next = p;
        while (*next && !edc_path_is_sep(*next))
            next++;

        if (sdslen(partial) > 0 && !edc_path_is_sep(partial[sdslen(partial) - 1]))
        {
            const char sep = PATH_SEP;
            partial = sdscatlen(partial, &sep, 1);
        }
        partial = sdscatlen(partial, p, (size_t)(next - p));

        uv_fs_t req;
        int r = (int)uv_fs_mkdir(g_file_loop, &req, partial, mode, NULL);
        uv_fs_req_cleanup(&req);
        if (r < 0 && r != UV_EEXIST) /* libuv codes are not -errno; UV_EEXIST is -4075 on Windows, not -EEXIST */
        {
            edc_error_set(error, EDC_ERR_FAILED, "mkdir '%s' failed: %s", partial, uv_strerror(r));
            sdsfree(partial);
            return false;
        }
        p = next;
    }

    sdsfree(partial);
    return true;
}

/* "." and ".." are on every directory listing and no caller means them: three callers
 * used to filter them out with their own copy of this test, and a fourth (the list_dir
 * tool) showed them to the model as if they were entries. */
static bool edc_dir_entry_is_dot(const char *name)
{
    return strcmp(name, ".") == 0 || strcmp(name, "..") == 0;
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
        /* DWORD is 32 bits by definition (Win32 API); say so instead of %lu, which is 32 on
         * Windows but 64 on LP64 -- a format that happens to match the platform here and
         * would be wrong the moment this value were anything else. */
        edc_error_set(error, EDC_ERR_FAILED, "FindFirstFile '%s' failed: %" PRIu32, path,
                      (uint32_t)GetLastError());
        return false;
    }

    do
    {
        if (edc_dir_entry_is_dot(fd.cFileName))
            continue;

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
        if (edc_dir_entry_is_dot(entry->d_name))
            continue;

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
