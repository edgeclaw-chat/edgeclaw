#ifdef __linux__
#define _GNU_SOURCE
#include "edc_ns_sandbox.h"
#include "log.h"
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <sys/vfs.h>
#include <unistd.h>

#ifndef PROC_SUPER_MAGIC
#define PROC_SUPER_MAGIC 0x9fa0
#endif
#ifndef SYSFS_MAGIC
#define SYSFS_MAGIC 0x62656572
#endif
#ifndef TMPFS_MAGIC
#define TMPFS_MAGIC 0x01021994
#endif
#ifndef DEVPTS_SUPER_MAGIC
#define DEVPTS_SUPER_MAGIC 0x1cd1
#endif
#ifndef CGROUP_SUPER_MAGIC
#define CGROUP_SUPER_MAGIC 0x27e0eb
#endif
#ifndef CGROUP2_SUPER_MAGIC
#define CGROUP2_SUPER_MAGIC 0x63677270
#endif
#ifndef DEBUGFS_MAGIC
#define DEBUGFS_MAGIC 0x64626720
#endif
#ifndef TRACEFS_MAGIC
#define TRACEFS_MAGIC 0x74726163
#endif
#ifndef SECURITYFS_MAGIC
#define SECURITYFS_MAGIC 0x73636673
#endif
#ifndef BPF_FS_MAGIC
#define BPF_FS_MAGIC 0xcafe4a11
#endif
#ifndef NSFS_MAGIC
#define NSFS_MAGIC 0x6e736673
#endif
#ifndef RAMFS_MAGIC
#define RAMFS_MAGIC 0x858458f6
#endif
#ifndef HUGETLBFS_MAGIC
#define HUGETLBFS_MAGIC 0x958458f6
#endif
#ifndef CONFIGFS_MAGIC
#define CONFIGFS_MAGIC 0x62656570
#endif
#ifndef MQUEUE_MAGIC
#define MQUEUE_MAGIC 0x19800202
#endif
#ifndef SELINUX_MAGIC
#define SELINUX_MAGIC 0xf97cff8c
#endif
#ifndef PSTOREFS_MAGIC
#define PSTOREFS_MAGIC 0x6165676C
#endif
#ifndef PIPEFS_MAGIC
#define PIPEFS_MAGIC 0x50495045
#endif

static int write_id_map(const char *map_file, unsigned int inside_id, unsigned int outside_id)
{
    int fd = open(map_file, O_WRONLY);
    if (fd < 0)
    {
        log_debug("edc_ns_sandbox: cannot open %s: %s", map_file, strerror(errno));
        return -1;
    }

    char buf[64];
    int n = snprintf(buf, sizeof(buf), "%u %u 1\n", inside_id, outside_id);
    if (write(fd, buf, n) != n)
    {
        log_debug("edc_ns_sandbox: cannot write %s: %s", map_file, strerror(errno));
        close(fd);
        return -1;
    }

    close(fd);
    return 0;
}

static bool path_is_mount_point(const char *path)
{
    struct stat st1, st2;
    if (stat(path, &st1) != 0)
        return FALSE;

    char *parent = g_path_get_dirname(path);
    int ret = stat(parent, &st2);
    free(parent);
    if (ret != 0)
        return FALSE;

    return (st1.st_dev != st2.st_dev) || (st1.st_ino == 2);
}

static bool is_special_fs(const char *path)
{
    if (!path_is_mount_point(path))
        return FALSE;

    struct statfs sfs;
    if (statfs(path, &sfs) != 0)
    {
        log_debug("edc_ns_sandbox: statfs(%s) failed: %s", path, strerror(errno));
        return FALSE;
    }

    switch (sfs.f_type)
    {
    case PROC_SUPER_MAGIC:
    case SYSFS_MAGIC:
    case TMPFS_MAGIC:
    case DEVPTS_SUPER_MAGIC:
    case CGROUP_SUPER_MAGIC:
    case CGROUP2_SUPER_MAGIC:
    case DEBUGFS_MAGIC:
    case TRACEFS_MAGIC:
    case SECURITYFS_MAGIC:
    case BPF_FS_MAGIC:
    case NSFS_MAGIC:
    case RAMFS_MAGIC:
    case HUGETLBFS_MAGIC:
    case CONFIGFS_MAGIC:
    case MQUEUE_MAGIC:
    case SELINUX_MAGIC:
    case PSTOREFS_MAGIC:
    case PIPEFS_MAGIC:
        return TRUE;
    default:
        return FALSE;
    }
}

static bool ns_mount_special_fs(const char *path, int flag)
{
    struct statfs sfs;
    if (statfs(path, &sfs) != 0)
    {
        log_debug("edc_ns_sandbox: statfs(%s) failed: %s", path, strerror(errno));
        return FALSE;
    }

    const char *source = "none";
    const char *fstype = NULL;

    switch (sfs.f_type)
    {
    case PROC_SUPER_MAGIC:
        source = "proc";
        fstype = "proc";
        break;
    case SYSFS_MAGIC:
        source = "sysfs";
        fstype = "sysfs";
        break;
    case TMPFS_MAGIC:
        fstype = "tmpfs";
        break;
    case DEVPTS_SUPER_MAGIC:
        fstype = "devpts";
        break;
    case CGROUP_SUPER_MAGIC:
        fstype = "cgroup";
        break;
    case CGROUP2_SUPER_MAGIC:
        fstype = "cgroup2";
        break;
    case DEBUGFS_MAGIC:
        source = "debugfs";
        fstype = "debugfs";
        break;
    case TRACEFS_MAGIC:
        source = "tracefs";
        fstype = "tracefs";
        break;
    case SECURITYFS_MAGIC:
        source = "securityfs";
        fstype = "securityfs";
        break;
    case BPF_FS_MAGIC:
        source = "bpf";
        fstype = "bpf";
        break;
    case RAMFS_MAGIC:
        fstype = "ramfs";
        break;
    case HUGETLBFS_MAGIC:
        fstype = "hugetlbfs";
        break;
    case CONFIGFS_MAGIC:
        source = "configfs";
        fstype = "configfs";
        break;
    case MQUEUE_MAGIC:
        source = "mqueue";
        fstype = "mqueue";
        break;
    case PSTOREFS_MAGIC:
        fstype = "pstore";
        break;
    default:
        log_debug("edc_ns_sandbox: no mount type for fs 0x%lx at %s", (unsigned long)sfs.f_type, path);
        return FALSE;
    }

    if ((sfs.f_type == PROC_SUPER_MAGIC || sfs.f_type == SYSFS_MAGIC || sfs.f_type == CGROUP_SUPER_MAGIC ||
         sfs.f_type == CGROUP2_SUPER_MAGIC || sfs.f_type == DEBUGFS_MAGIC || sfs.f_type == TRACEFS_MAGIC ||
         sfs.f_type == SECURITYFS_MAGIC || sfs.f_type == BPF_FS_MAGIC))
    {
        log_debug("edc_ns_sandbox: skip remount of restricted fs at %s in user namespace", path);
        return FALSE;
    }

    if (umount2(path, MNT_DETACH) != 0 && errno != EINVAL)
    {
        log_debug("edc_ns_sandbox: umount2(%s) failed: %s", path, strerror(errno));
        return FALSE;
    }

    if (mount(source, path, fstype, flag, NULL) != 0)
    {
        log_debug("edc_ns_sandbox: mount(%s, %s) at %s failed: %s", source, fstype, path, strerror(errno));
        return FALSE;
    }

    return TRUE;
}

static bool ns_mount_dir_as_rw(const char *path)
{
    if (mount(path, path, NULL, MS_BIND, NULL) != 0)
    {
        log_debug("edc_ns_sandbox: MS_BIND on %s failed: %s", path, strerror(errno));
        return FALSE;
    }

    if (mount(NULL, path, NULL, MS_REMOUNT | MS_BIND, NULL) != 0)
    {
        log_debug("edc_ns_sandbox: MS_REMOUNT | MS_BIND on %s failed: %s", path, strerror(errno));
        return FALSE;
    }

    return TRUE;
}

bool edc_ns_sandbox_apply(const char **ro_paths, const char **rw_paths)
{
    if (!rw_paths || !rw_paths[0])
    {
        log_error("edc_ns_sandbox: requires at least one rw_path");
        return FALSE;
    }

    uid_t uid = getuid();
    gid_t gid = getgid();

    /*
     * Create a user namespace first. On this kernel, CLONE_NEWUSER
     * must come before CLONE_NEWNS. Any user can create a user
     * namespace; after unshare the process has full capabilities
     * (including CAP_SETUID) within the new namespace, so it can
     * write its own uid_map/gid_map to become root, which then
     * allows subsequent mount operations.
     *
     * If CLONE_NEWUSER fails and we are not root, mount namespace
     * creation is also unavailable. In that case we skip namespace
     * isolation and rely on seccomp-bpf as the primary sandbox layer.
     */
    bool have_userns = FALSE;
    if (syscall(SYS_unshare, CLONE_NEWUSER) == 0)
    {
        have_userns = TRUE;

        int fd = open("/proc/self/setgroups", O_WRONLY);
        if (fd >= 0)
        {
            int ret = write(fd, "deny\n", 5);
            (void)ret;
            close(fd);
        }

        if (write_id_map("/proc/self/uid_map", 0, uid) != 0 || write_id_map("/proc/self/gid_map", 0, gid) != 0)
        {
            log_warn("edc_ns_sandbox: uid_map/gid_map write failed");
        }
    }

    log_info("edc_ns_sandbox: uid = %d, gid = %d --> uid = %d, gid = %d", (int)uid, (int)gid, (int)getuid(),
             (int)getgid());

    /*
     * Create a mount namespace. This gives us a private copy of the
     * mount tree, fully isolated from the parent namespace.
     *
     * CLONE_NEWNS requires CAP_SYS_ADMIN. This is only available
     * to root outside a user namespace, or to namespace root
     * after writing uid_map=0 inside a new user namespace.
     */
    if (!have_userns)
    {
        log_debug("edc_ns_sandbox: need root privileges for mount namespace");
        return TRUE;
    }

    if (syscall(SYS_unshare, CLONE_NEWNS) != 0)
    {
        log_debug("edc_ns_sandbox: CLONE_NEWNS failed: %s", strerror(errno));
        return TRUE;
    }

    /* Make root private so our changes don't propagate to the parent namespace */
    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) != 0)
    {
        log_debug("edc_ns_sandbox: MS_PRIVATE on / failed: %s", strerror(errno));
        return FALSE;
    }

    if (mount(NULL, "/", NULL, MS_REMOUNT | MS_BIND | MS_RDONLY, NULL))
    {
        log_debug("edc_ns_sandbox: MS_REMOUNT | MS_BIND | MS_RDONLY on / failed: %s", strerror(errno));
        return FALSE;
    }

    for (int i = 0; ro_paths[i]; ++i)
    {
        if (!is_special_fs(ro_paths[i]))
            continue;

        if (!ns_mount_special_fs(ro_paths[i], MS_RDONLY))
        {
            continue;
        }

        log_info("edc_ns_sandbox: mount %s as readonly.", ro_paths[i]);
    }

    /*
     * Ensure writable directories. All caller-specified rw_paths need
     * to be writable for shell commands to function.
     *
     * After root is remounted read-only, each path is either on a
     * special/virtual filesystem (which gets unmounted and remounted
     * fresh) or on the regular rootfs (which gets a bind mount +
     * remount to clear the read-only flag without touching the
     * superblock).
     */
    for (int i = 0; rw_paths[i]; i++)
    {
        if (is_special_fs(rw_paths[i]))
        {
            if (!ns_mount_special_fs(rw_paths[i], 0))
            {
                continue;
            }
        }
        else
        {
            if (!ns_mount_dir_as_rw(rw_paths[i]))
            {
                continue;
            }
        }
    }

    log_info("edc_ns_sandbox: namespace sandbox enforced (userns=%s)", have_userns ? "yes" : "no");
    return TRUE;
}

#else /* !__linux__ */
#include "edc_ns_sandbox.h"
bool edc_ns_sandbox_apply(const char **ro_paths, const char **rw_paths)
{
    (void)ro_paths;
    (void)rw_paths;
    return FALSE;
}

#endif
