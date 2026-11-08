#ifndef TEST_FS_UTIL_H
#define TEST_FS_UTIL_H

/*
 * Directory fixtures for tests: the two things nearly every harness here needs and used to get
 * from the shell.
 *
 *   test_rm_rf(dir)     -- "rm -rf dir"
 *   test_mkdir_p(dir)   -- "mkdir -p dir"
 *   test_sh(cmd)        -- run one of the commands the tests use, without a shell
 *
 * Why they exist: `system("rm -rf X && mkdir -p X/sub")` is POSIX shell syntax. On Windows the
 * same string goes to cmd.exe, which answers "The syntax of the command is incorrect." and
 * returns non-zero, so those tests failed while preparing their workspace and never reached an
 * assertion. That one construct was the largest remaining block of Windows test failures.
 *
 * test_sh() is deliberately not a shell: it understands exactly `rm -rf <paths>` and
 * `mkdir -p <paths>`, chained with `&&`, which is all the suite uses; anything else is handed to
 * system() unchanged. On POSIX it IS system(), so behaviour there does not change at all.
 *
 * Header-only and static: the tests are separate binaries with no shared library, and this is
 * three functions, not a framework.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <dirent.h>
#include <unistd.h>

#ifdef _WIN32
#include <direct.h>
/* MSVC's mkdir() takes one argument; the two-argument POSIX form is what every call site here
 * uses. Keeping the wrapper inside this header means the tests do not have to know. */
static int test_mkdir(const char *path)
{
    return _mkdir(path);
}
#else
static int test_mkdir(const char *path)
{
    return mkdir(path, 0755);
}
#endif

/* Create `path` and every missing parent. Already existing is success, like `mkdir -p`. */
static void test_mkdir_p(const char *path)
{
    char buf[1024];
    size_t i, len;

    if (path == NULL || path[0] == '\0')
        return;
    len = strlen(path);
    if (len >= sizeof(buf))
        return;
    memcpy(buf, path, len + 1);

    for (i = 1; i < len; i++)
    {
        if (buf[i] != '/' && buf[i] != '\\')
            continue;
        buf[i] = '\0';
        /* A Windows drive prefix ("C:") is not a directory on its own. */
        if (!(strlen(buf) == 2 && buf[1] == ':'))
            (void)test_mkdir(buf);
        buf[i] = path[i];
    }
    (void)test_mkdir(buf);
}

/* Remove `path` recursively; missing is success, like `rm -rf`. */
static void test_rm_rf(const char *path)
{
    DIR *dir;
    struct dirent *entry;

    if (path == NULL || path[0] == '\0')
        return;

    dir = opendir(path);
    if (dir == NULL)
    {
        /* Not a directory: it is a file, or already gone. */
        (void)remove(path);
        return;
    }

    while ((entry = readdir(dir)) != NULL)
    {
        char child[1024];
        size_t n;

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        n = (size_t)snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
        if (n >= sizeof(child))
            continue;
        /* d_type is filled in by the POSIX and the Win32 readdir alike. */
        if (entry->d_type == DT_DIR || entry->d_type == DT_UNKNOWN)
            test_rm_rf(child);
        else
            (void)remove(child);
    }
    closedir(dir);

    (void)rmdir(path);
}

#ifdef _WIN32

/* Tokenize in place on spaces/tabs, honouring single quotes. NULL-terminates each token. */
static size_t test_split_args(char *text, char **out, size_t out_max)
{
    size_t n = 0;
    char *p = text;

    while (*p != '\0' && n < out_max)
    {
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '\0')
            break;
        if (*p == '\'')
        {
            p++;
            out[n++] = p;
            while (*p != '\0' && *p != '\'')
                p++;
            if (*p != '\0')
                *p++ = '\0';
        }
        else
        {
            out[n++] = p;
            while (*p != '\0' && *p != ' ' && *p != '\t')
                p++;
            if (*p != '\0')
                *p++ = '\0';
        }
    }
    return n;
}

static int test_sh(const char *cmd)
{
    char work[2048];
    size_t len;
    char *part;
    char *next;
    int rc = 0;

    if (cmd == NULL)
        return -1;
    len = strlen(cmd);
    if (len >= sizeof(work))
        len = sizeof(work) - 1;
    memcpy(work, cmd, len);
    work[len] = '\0';

    part = work;
    while (part != NULL)
    {
        char *args[64];
        size_t argc, i, tail;

        next = strstr(part, "&&");
        if (next != NULL)
        {
            *next = '\0';
            next += 2;
        }

        while (*part == ' ' || *part == '\t')
            part++;
        tail = strlen(part);
        while (tail > 0 && (part[tail - 1] == ' ' || part[tail - 1] == '\t' ||
                            part[tail - 1] == '\r' || part[tail - 1] == '\n'))
            part[--tail] = '\0';

        argc = test_split_args(part, args, 64);
        if (argc == 0)
        {
            part = next;
            continue;
        }

        if (strcmp(args[0], "rm") == 0 && argc >= 2 && strcmp(args[1], "-rf") == 0)
        {
            for (i = 2; i < argc; i++)
                test_rm_rf(args[i]);
        }
        else if (strcmp(args[0], "mkdir") == 0 && argc >= 2 && strcmp(args[1], "-p") == 0)
        {
            for (i = 2; i < argc; i++)
                test_mkdir_p(args[i]);
        }
        else
        {
            int sys_rc = system(part);
            if (sys_rc != 0)
                rc = sys_rc;
        }

        part = next;
    }
    return rc;
}

#else /* !_WIN32 */

#define test_sh(cmd) system(cmd)

#endif /* _WIN32 */

#endif /* TEST_FS_UTIL_H */
