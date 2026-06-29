#include "edc_file_utils.h"

#include <fcntl.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static uv_loop_t *g_file_loop = NULL;

void edc_file_utils_init(uv_loop_t *loop)
{
    g_file_loop = loop;
}

sds edc_file_read(const char *path, EdcError **error)
{
    uv_fs_t req;
    int fd = (int)uv_fs_open(g_file_loop, &req, path, O_RDONLY, 0, NULL);
    if (fd < 0) {
        edc_error_set(error, EDC_ERR_FAILED, "open '%s' failed: %s",
                      path, uv_strerror(fd));
        uv_fs_req_cleanup(&req);
        return NULL;
    }
    uv_fs_req_cleanup(&req);

    int r = (int)uv_fs_fstat(g_file_loop, &req, fd, NULL);
    if (r < 0) {
        edc_error_set(error, EDC_ERR_FAILED, "stat '%s' failed: %s",
                      path, uv_strerror(r));
        uv_fs_req_cleanup(&req);
        uv_fs_t cr;
        uv_fs_close(g_file_loop, &cr, fd, NULL);
        uv_fs_req_cleanup(&cr);
        return NULL;
    }
    size_t file_size = (size_t)req.statbuf.st_size;
    uv_fs_req_cleanup(&req);

    char *buf = malloc(file_size + 1);
    if (!buf) {
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

    if (r < 0) {
        free(buf);
        edc_error_set(error, EDC_ERR_FAILED, "read '%s' failed: %s",
                      path, uv_strerror(r));
        return NULL;
    }

    buf[r] = '\0';
    sds result = sdsnewlen(buf, (size_t)r);
    free(buf);
    return result;
}

bool edc_file_write(const char *path, const char *content, ssize_t len,
                    EdcError **error)
{
    size_t write_len = (len < 0) ? strlen(content) : (size_t)len;

    uv_fs_t req;
    int fd = (int)uv_fs_open(g_file_loop, &req, path,
                             O_WRONLY | O_CREAT | O_TRUNC, 0644, NULL);
    if (fd < 0) {
        edc_error_set(error, EDC_ERR_FAILED, "open '%s' for write failed: %s",
                      path, uv_strerror(fd));
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

    if (r < 0) {
        edc_error_set(error, EDC_ERR_FAILED, "write '%s' failed: %s",
                      path, uv_strerror(r));
        return false;
    }
    return true;
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

    while (*p) {
        while (*p == '/') p++;
        const char *next = strchr(p, '/');
        size_t seg_len = next ? (size_t)(next - p) : strlen(p);
        if (seg_len == 0) break;

        if (sdslen(partial) > 0)
            partial = sdscat(partial, "/");
        partial = sdscatlen(partial, p, seg_len);

        uv_fs_t req;
        int r = (int)uv_fs_mkdir(g_file_loop, &req, partial, mode, NULL);
        uv_fs_req_cleanup(&req);
        if (r < 0 && r != -EEXIST) {
            edc_error_set(error, EDC_ERR_FAILED, "mkdir '%s' failed: %s",
                          partial, uv_strerror(r));
            sdsfree(partial);
            return false;
        }

        p = next ? next + 1 : p + seg_len;
    }

    sdsfree(partial);
    return true;
}

bool edc_dir_foreach(const char *path, edc_dir_entry_cb cb, void *userdata,
                     EdcError **error)
{
    uv_fs_t req;
    int n = (int)uv_fs_scandir(g_file_loop, &req, path, 0, NULL);
    if (n < 0) {
        edc_error_set(error, EDC_ERR_FAILED, "scandir '%s' failed: %s",
                      path, uv_strerror(n));
        uv_fs_req_cleanup(&req);
        return false;
    }

    uv_dirent_t *entries = (uv_dirent_t *)req.ptr;
    for (int i = 0; i < n; i++)
        cb(entries[i].name, (int)entries[i].type, userdata);

    uv_fs_req_cleanup(&req);
    return true;
}

sds edc_path_join(const char *first, ...)
{
    va_list args;
    va_start(args, first);

    sds result = sdsempty();
    bool need_sep = false;

    for (const char *seg = first; seg; seg = va_arg(args, const char *)) {
        if (*seg == '\0') continue;
        if (need_sep) result = sdscat(result, "/");
        result = sdscat(result, seg);
        need_sep = true;
    }

    va_end(args);
    return result;
}

sds edc_path_dirname(const char *path)
{
    if (!path) return sdsempty();
    const char *last = strrchr(path, '/');
    if (!last) return sdsnew(".");
    if (last == path) return sdsnew("/");
    return sdsnewlen(path, (size_t)(last - path));
}

sds edc_cwd(void)
{
    char buf[4096];
    size_t size = sizeof(buf);
    int r = uv_cwd(buf, &size);
    if (r != 0) return sdsempty();
    return sdsnewlen(buf, size > 0 ? size - 1 : 0);
}

sds edc_homedir(void)
{
    char buf[4096];
    size_t size = sizeof(buf);
    int r = uv_os_homedir(buf, &size);
    if (r != 0) return sdsempty();
    return sdsnewlen(buf, size);
}

sds edc_os_name(void)
{
    uv_utsname_t info;
    int r = uv_os_uname(&info);
    if (r != 0) return sdsnew("Unknown");
    return sdsnew(info.sysname);
}
