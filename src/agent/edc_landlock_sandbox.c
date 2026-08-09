#ifdef __linux__

#define _GNU_SOURCE

#include "edc_landlock_sandbox.h"
#include "edc_compat.h"
#include "log.h"
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef SYS_landlock_create_ruleset
#define SYS_landlock_create_ruleset 444
#define SYS_landlock_add_rule 445
#define SYS_landlock_restrict_self 446
#endif

#ifndef LANDLOCK_ACCESS_FS_EXECUTE
#define LANDLOCK_ACCESS_FS_EXECUTE (1ULL << 0)
#define LANDLOCK_ACCESS_FS_WRITE_FILE (1ULL << 1)
#define LANDLOCK_ACCESS_FS_READ_FILE (1ULL << 2)
#define LANDLOCK_ACCESS_FS_READ_DIR (1ULL << 3)
#define LANDLOCK_ACCESS_FS_REMOVE_DIR (1ULL << 4)
#define LANDLOCK_ACCESS_FS_REMOVE_FILE (1ULL << 5)
#define LANDLOCK_ACCESS_FS_MAKE_CHAR (1ULL << 6)
#define LANDLOCK_ACCESS_FS_MAKE_DIR (1ULL << 7)
#define LANDLOCK_ACCESS_FS_MAKE_REG (1ULL << 8)
#define LANDLOCK_ACCESS_FS_MAKE_SOCK (1ULL << 9)
#define LANDLOCK_ACCESS_FS_MAKE_FIFO (1ULL << 10)
#define LANDLOCK_ACCESS_FS_MAKE_BLOCK (1ULL << 11)
#define LANDLOCK_ACCESS_FS_MAKE_SYM (1ULL << 12)
#define LANDLOCK_ACCESS_FS_REFER (1ULL << 13)
#define LANDLOCK_ACCESS_FS_TRUNCATE (1ULL << 14)
#endif

#define LANDLOCK_RULE_PATH_BENEATH 1

struct landlock_ruleset_attr_v1
{
    uint64_t handled_access_fs;
};

#ifndef landlock_create_ruleset
static int landlock_create_ruleset(const void *attr, size_t size, uint32_t flags)
{
    return syscall(SYS_landlock_create_ruleset, attr, size, flags);
}
#endif

#ifndef landlock_add_rule
static int landlock_add_rule(int ruleset_fd, int rule_type, const void *rule_attr, uint32_t flags)
{
    return syscall(SYS_landlock_add_rule, ruleset_fd, rule_type, rule_attr, flags);
}
#endif

#ifndef landlock_restrict_self
static int landlock_restrict_self(int ruleset_fd, uint32_t flags)
{
    return syscall(SYS_landlock_restrict_self, ruleset_fd, flags);
}
#endif

#define LANDLOCK_ACCESS_FS_BASE                                                                                        \
    (LANDLOCK_ACCESS_FS_EXECUTE | LANDLOCK_ACCESS_FS_WRITE_FILE | LANDLOCK_ACCESS_FS_READ_FILE |                       \
     LANDLOCK_ACCESS_FS_READ_DIR | LANDLOCK_ACCESS_FS_REMOVE_FILE | LANDLOCK_ACCESS_FS_REMOVE_DIR |                    \
     LANDLOCK_ACCESS_FS_MAKE_CHAR | LANDLOCK_ACCESS_FS_MAKE_DIR | LANDLOCK_ACCESS_FS_MAKE_REG |                        \
     LANDLOCK_ACCESS_FS_MAKE_SOCK | LANDLOCK_ACCESS_FS_MAKE_FIFO | LANDLOCK_ACCESS_FS_MAKE_BLOCK |                     \
     LANDLOCK_ACCESS_FS_MAKE_SYM)

static int landlock_add_path(int ruleset_fd, const char *path, uint64_t access)
{
    int fd = open(path, O_PATH | O_CLOEXEC);
    if (fd < 0)
        return -1;

    struct
    {
        uint64_t allowed_access;
        int32_t parent_fd;
    } attr = {.allowed_access = access, .parent_fd = fd};

    int ret = landlock_add_rule(ruleset_fd, LANDLOCK_RULE_PATH_BENEATH, &attr, 0);
    close(fd);
    return ret;
}

bool edc_landlock_sandbox_apply(const char **ro_paths, const char **rw_paths)
{
    if (!rw_paths || !rw_paths[0])
    {
        log_error("edc_sandbox: Landlock requires at least one rw_path");
        return false;
    }

    prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);

    uint64_t handled = LANDLOCK_ACCESS_FS_BASE;

    /* Probe REFER (Linux 5.19+) */
    {
        struct landlock_ruleset_attr_v1 attr = {.handled_access_fs = handled | LANDLOCK_ACCESS_FS_REFER};
        int fd = landlock_create_ruleset(&attr, sizeof(attr), 0);
        if (fd >= 0)
        {
            handled |= LANDLOCK_ACCESS_FS_REFER;
            close(fd);
        }
    }

    /* Probe TRUNCATE (Linux 6.2+) */
    {
        struct landlock_ruleset_attr_v1 attr = {.handled_access_fs = handled | LANDLOCK_ACCESS_FS_TRUNCATE};
        int fd = landlock_create_ruleset(&attr, sizeof(attr), 0);
        if (fd >= 0)
        {
            handled |= LANDLOCK_ACCESS_FS_TRUNCATE;
            close(fd);
        }
    }

    const uint64_t access_rw = handled;
    const uint64_t access_ro = LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR | LANDLOCK_ACCESS_FS_EXECUTE |
                               (handled & LANDLOCK_ACCESS_FS_REFER);

    struct landlock_ruleset_attr_v1 ruleset_attr = {.handled_access_fs = handled};

    int ruleset_fd = landlock_create_ruleset(&ruleset_attr, sizeof(ruleset_attr), 0);
    if (ruleset_fd < 0)
    {
        log_debug("edc_sandbox: Landlock not available: %s", strerror(errno));
        return false;
    }

    for (int i = 0; rw_paths[i]; i++)
    {
        if (landlock_add_path(ruleset_fd, rw_paths[i], access_rw) != 0)
        {
            log_debug("edc_sandbox: failed to add rw Landlock rule for '%s': %s", rw_paths[i], strerror(errno));
        }
    }

    if (ro_paths)
    {
        for (int i = 0; ro_paths[i]; i++)
        {
            if (landlock_add_path(ruleset_fd, ro_paths[i], access_ro) != 0)
            {
                log_debug("edc_sandbox: failed to add ro Landlock rule for '%s': %s", ro_paths[i], strerror(errno));
            }
        }
    }

    if (landlock_restrict_self(ruleset_fd, 0) != 0)
    {
        log_error("edc_sandbox: Landlock restrict failed: %s", strerror(errno));
        close(ruleset_fd);
        return false;
    }

    close(ruleset_fd);
    log_info("edc_sandbox: Landlock enforced");
    return true;
}

#else /* !__linux__ */

#include "edc_landlock_sandbox.h"

bool edc_landlock_sandbox_apply(const char **ro_paths, const char **rw_paths)
{
    (void)ro_paths;
    (void)rw_paths;
    return false;
}

#endif
