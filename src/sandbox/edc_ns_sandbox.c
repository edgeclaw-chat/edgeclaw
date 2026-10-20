#ifdef __linux__
#define _GNU_SOURCE
#include "edc_ns_sandbox.h"
#include "edc_compat.h"
#include "log.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <sys/sysmacros.h>
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
        return false;

    /* Parent directory, computed inline: this file must stay linkable into the
     * tiny sandbox_helper binary, so it avoids the sds-based edc_file_utils. */
    char parent[4096];
    if (snprintf(parent, sizeof(parent), "%s", path) >= (int)sizeof(parent))
        return false;
    size_t n = strlen(parent);
    while (n > 1 && parent[n - 1] == '/')
        parent[--n] = '\0';
    char *slash = strrchr(parent, '/');
    if (!slash)
        strcpy(parent, ".");
    else if (slash == parent)
        slash[1] = '\0'; /* keep "/" */
    else
        *slash = '\0';

    int ret = stat(parent, &st2);
    if (ret != 0)
        return false;

    return (st1.st_dev != st2.st_dev) || (st1.st_ino == 2);
}

static bool is_special_fs(const char *path)
{
    if (!path_is_mount_point(path))
        return false;

    struct statfs sfs;
    if (statfs(path, &sfs) != 0)
    {
        log_debug("edc_ns_sandbox: statfs(%s) failed: %s", path, strerror(errno));
        return false;
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
        return true;
    default:
        return false;
    }
}

static bool ns_mount_special_fs(const char *path, int flag)
{
    struct statfs sfs;
    if (statfs(path, &sfs) != 0)
    {
        log_debug("edc_ns_sandbox: statfs(%s) failed: %s", path, strerror(errno));
        return false;
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
        return false;
    }

    if ((sfs.f_type == PROC_SUPER_MAGIC || sfs.f_type == SYSFS_MAGIC || sfs.f_type == CGROUP_SUPER_MAGIC ||
         sfs.f_type == CGROUP2_SUPER_MAGIC || sfs.f_type == DEBUGFS_MAGIC || sfs.f_type == TRACEFS_MAGIC ||
         sfs.f_type == SECURITYFS_MAGIC || sfs.f_type == BPF_FS_MAGIC))
    {
        log_debug("edc_ns_sandbox: skip remount of restricted fs at %s in user namespace", path);
        return false;
    }

    if (umount2(path, MNT_DETACH) != 0 && errno != EINVAL)
    {
        log_debug("edc_ns_sandbox: umount2(%s) failed: %s", path, strerror(errno));
        return false;
    }

    if (mount(source, path, fstype, flag, NULL) != 0)
    {
        log_debug("edc_ns_sandbox: mount(%s, %s) at %s failed: %s", source, fstype, path, strerror(errno));
        return false;
    }

    return true;
}

static bool ns_mount_dir_as_rw(const char *path)
{
    if (mount(path, path, NULL, MS_BIND, NULL) != 0)
    {
        log_debug("edc_ns_sandbox: MS_BIND on %s failed: %s", path, strerror(errno));
        return false;
    }

    if (mount(NULL, path, NULL, MS_REMOUNT | MS_BIND, NULL) != 0)
    {
        /* The remount can fail with EPERM for a mount whose superblock lives in
         * an ancestor user namespace (notably the tmpfs /tmp), even though the
         * bind already inherited read-write access. Only treat it as an error
         * when the path really is still read-only -- otherwise the caller would
         * fall back to recreating the filesystem and silently discard its
         * contents. */
        struct statvfs vfs;
        if (statvfs(path, &vfs) == 0 && (vfs.f_flag & ST_RDONLY))
        {
            log_debug("edc_ns_sandbox: MS_REMOUNT | MS_BIND on %s failed: %s", path, strerror(errno));
            return false;
        }
        log_debug("edc_ns_sandbox: remount of %s not needed (bind already rw)", path);
    }

    return true;
}

/* Make an existing mount read-only without discarding its contents. This is
 * the read-only counterpart of ns_mount_dir_as_rw, but it does NOT bind first.
 *
 * It is what /run needs. Recreating a fresh empty tmpfs there erased
 * /run/systemd/resolve, so the /etc/resolv.conf symlink into it dangled and
 * name resolution broke.
 *
 * A bind of /run is refused outright -- the mount's superblock belongs to an
 * ancestor user namespace, so MS_BIND fails with EINVAL ("wrong fs type") --
 * while the read-only remount of the existing mount is allowed. That remount
 * only flips the flags of this namespace's copy of the mount (the sandbox has
 * already made every mount private), so the host is untouched. */
static bool ns_mount_dir_as_ro(const char *path)
{
    if (mount(NULL, path, NULL, MS_REMOUNT | MS_BIND | MS_RDONLY, NULL) != 0)
    {
        /* Some mounts may need the bind first; keep that as a fallback. */
        if (mount(path, path, NULL, MS_BIND, NULL) != 0 ||
            mount(NULL, path, NULL, MS_REMOUNT | MS_BIND | MS_RDONLY, NULL) != 0)
        {
            log_debug("edc_ns_sandbox: cannot remount %s read-only: %s", path, strerror(errno));
        }
    }

    struct statvfs vfs;
    if (statvfs(path, &vfs) == 0 && (vfs.f_flag & ST_RDONLY))
        return true;

    /* Could not make it read-only. The mount did keep its contents, so the
     * caller leaves it in place rather than recreating an empty filesystem. */
    log_debug("edc_ns_sandbox: %s kept writable (read-only remount not permitted)", path);
    return false;
}

/* Mask a REGULAR FILE so that open() FAILS, rather than turning it into a 0-byte
 * file. An empty file is not a mask: a reader cannot tell "denied" from "empty",
 * and a tool that treats the file as present-but-blank gets a wrong answer
 * instead of an error.
 *
 * The trick is the kernel's "unnamed" character device (0:0, what overlayfs uses
 * as a whiteout): opening it fails. Creating one needs CAP_MKNOD over the
 * superblock it lives on, so it has to be made on a tmpfs WE mounted (mknod on
 * the host's /tmp or /dev is refused). That tmpfs is detached and its directory
 * removed right after the bind -- the bind mount keeps the inode alive, so
 * nothing is left behind.
 *
 * Returns false when any step fails, leaving the caller to fall back to the
 * empty-file bind: degraded (open succeeds, contents gone) but logged, and still
 * better than a live file. */
static bool ns_mask_file(const char *path, const char *scratch_base)
{
    /* The scratch dir must be creatable AFTER the tree went read-only, so it has
     * to be inside a writable path: the first rw path, else /tmp. */
    const char *bases[2] = {scratch_base, "/tmp"};
    char tmpl[PATH_MAX];
    char *dir = NULL;
    for (size_t b = 0; b < sizeof(bases) / sizeof(bases[0]) && !dir; b++)
    {
        if (!bases[b] || !bases[b][0])
            continue;
        if (b > 0 && scratch_base && strcmp(bases[b - 1], bases[b]) == 0)
            continue; /* same base twice, no point retrying it */
        if (snprintf(tmpl, sizeof(tmpl), "%s/.edc-mask-XXXXXX", bases[b]) >= (int)sizeof(tmpl))
            continue;
        dir = mkdtemp(tmpl);
    }
    if (!dir)
        return false;

    bool ok = false;
    char node[PATH_MAX];
    if (mount("tmpfs", dir, "tmpfs", MS_NOSUID | MS_NODEV, "size=64k,mode=700") != 0)
    {
        log_debug("edc_ns_sandbox: mask tmpfs on %s failed: %s", dir, strerror(errno));
    }
    else if (snprintf(node, sizeof(node), "%s/m", dir) >= (int)sizeof(node))
    {
        log_debug("edc_ns_sandbox: scratch path too long: %s", dir);
    }
    else if (mknod(node, S_IFCHR | 0600, makedev(0, 0)) != 0)
    {
        log_debug("edc_ns_sandbox: mknod(0:0) in %s failed: %s", dir, strerror(errno));
    }
    else
    {
        /* Verify the node really refuses to open BEFORE binding it over the
         * target: if this kernel/filesystem allows it, the empty-file fallback is
         * the honest outcome rather than a silently readable "mask". */
        int probe = open(node, O_RDONLY | O_CLOEXEC);
        if (probe >= 0)
        {
            close(probe);
            log_debug("edc_ns_sandbox: a 0:0 node is openable here, cannot use it as a mask");
        }
        else if (mount(node, path, NULL, MS_BIND, NULL) != 0)
        {
            log_debug("edc_ns_sandbox: bind mask node over %s failed: %s", path, strerror(errno));
        }
        else
        {
            ok = true;
        }
    }

    /* Detach the scratch tmpfs (the bind above holds its own reference) and drop
     * the now-empty directory: no leftovers on the host filesystem. */
    umount2(dir, MNT_DETACH);
    rmdir(dir);
    return ok;
}

/* Mask a path so its contents become unreachable: overlay an empty mode-000
 * tmpfs on a directory, or bind an unopenable 0:0 device node over a regular
 * file. Landlock cannot express a deny inside an allowed parent, so masking is
 * done here, in the mount namespace, where the last mount on a path wins. */
static bool ns_mask_path(const char *path, const char *scratch_base)
{
    struct stat st;
    if (stat(path, &st) != 0)
        return false; /* nothing there to mask */

    if (S_ISDIR(st.st_mode))
    {
        if (mount("tmpfs", path, "tmpfs", MS_RDONLY | MS_NOSUID | MS_NODEV, "size=1k,mode=000") != 0)
        {
            log_debug("edc_ns_sandbox: mask tmpfs on %s failed: %s", path, strerror(errno));
            return false;
        }
        log_info("edc_ns_sandbox: masked %s", path);
        return true;
    }

    if (ns_mask_file(path, scratch_base))
    {
        log_info("edc_ns_sandbox: masked %s (unopenable)", path);
        return true;
    }

    /* Fallback: bind an empty file over the target, so the CONTENTS are gone even
     * though open() still succeeds. Do NOT use /dev/null here: the rw handling
     * above may have replaced /dev with a fresh empty tmpfs, so /dev/null is not
     * necessarily present in this namespace. /tmp is writable, so create the
     * blank there and drop its name afterwards -- the bind mount keeps the inode
     * alive. */
    char tmpl[] = "/tmp/.edc-mask-XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd < 0)
    {
        log_debug("edc_ns_sandbox: cannot create mask file: %s", strerror(errno));
        return false;
    }
    close(fd);
    if (mount(tmpl, path, NULL, MS_BIND, NULL) != 0)
    {
        log_debug("edc_ns_sandbox: mask empty file on %s failed: %s", path, strerror(errno));
        unlink(tmpl);
        return false;
    }
    unlink(tmpl);

    log_warn("edc_ns_sandbox: masked %s with an EMPTY FILE (open still succeeds, contents are gone)", path);
    return true;
}

bool edc_ns_sandbox_apply(const char **ro_paths, const char **rw_paths, const char **deny_paths, bool *enforced)
{
    if (enforced)
        *enforced = false;
    if (!rw_paths || !rw_paths[0])
    {
        log_error("edc_ns_sandbox: requires at least one rw_path");
        return false;
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
    bool have_userns = false;
    if (syscall(SYS_unshare, CLONE_NEWUSER) == 0)
    {
        have_userns = true;

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
        return true;
    }

    if (syscall(SYS_unshare, CLONE_NEWNS) != 0)
    {
        log_debug("edc_ns_sandbox: CLONE_NEWNS failed: %s", strerror(errno));
        return true;
    }

    /* Make root private so our changes don't propagate to the parent namespace */
    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) != 0)
    {
        log_debug("edc_ns_sandbox: MS_PRIVATE on / failed: %s", strerror(errno));
        return false;
    }

    if (mount(NULL, "/", NULL, MS_REMOUNT | MS_BIND | MS_RDONLY, NULL))
    {
        log_debug("edc_ns_sandbox: MS_REMOUNT | MS_BIND | MS_RDONLY on / failed: %s", strerror(errno));
        return false;
    }

    for (int i = 0; ro_paths[i]; ++i)
    {
        if (!is_special_fs(ro_paths[i]))
            continue;

        /* Bind the existing mount onto itself and remount it read-only, so it
         * keeps its contents (/run in particular carries the resolv.conf
         * target). The old code replaced these with a fresh empty filesystem,
         * which silently discarded what was there and broke DNS. */
        if (!ns_mount_dir_as_ro(ro_paths[i]))
        {
            /* Left as a writable bind on purpose: the contents are preserved,
             * and dropping them to force the read-only flag would do more
             * damage than it prevents. */
            log_debug("edc_ns_sandbox: %s left read-only not enforced", ro_paths[i]);
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
        /* Already writable? Leave it alone. The read-only remount of "/" above
         * is not recursive, so every separate submount -- the devtmpfs /dev,
         * the tmpfs /tmp, a bind-mounted workspace, ... -- keeps its own rw
         * flag. Binding such a path is pointless, and the "recreate a special
         * filesystem" fallback below is actively harmful: for /dev it replaces
         * the populated devtmpfs with an empty tmpfs, so every node drops out
         * (/dev/null, /dev/zero, /dev/urandom, /dev/pts, /dev/shm) and tools
         * that need /dev/null (git, ssh, ...) break. */
        struct statvfs pre;
        if (statvfs(rw_paths[i], &pre) == 0 && !(pre.f_flag & ST_RDONLY))
            continue;

        /* Under the read-only rootfs: prefer a bind mount, which keeps the
         * directory's contents, then fall back to recreating a special
         * filesystem (devtmpfs, pts, some cgroup mounts). */
        if (ns_mount_dir_as_rw(rw_paths[i]))
            continue;
        if (is_special_fs(rw_paths[i]) && ns_mount_special_fs(rw_paths[i], 0))
            continue;
        log_debug("edc_ns_sandbox: could not make %s writable", rw_paths[i]);
    }

    /* Mask denied paths last so they win over any ro/rw mount above. A mask that
     * does not take effect is a security hole, not a cosmetic problem -- masking
     * is the only layer that can deny a path inside an allowed parent -- so it is
     * a warning rather than a debug line. */
    if (deny_paths)
    {
        for (int i = 0; deny_paths[i]; i++)
        {
            /* Mask files through a tmpfs under the first writable path (or /tmp),
             * so the 0:0 node is created on a superblock we own. */
            if (!ns_mask_path(deny_paths[i], rw_paths[0]))
                log_warn("edc_ns_sandbox: deny path %s was NOT masked -- it stays reachable", deny_paths[i]);
        }
    }

    /* Verify rather than trust: only report "enforced" when "/" really is
     * read-only and every writable path really is writable. The caller uses
     * this to decide whether the seccomp metadata deny-list is still needed. */
    bool root_ro = false;
    struct statvfs vfs;
    if (statvfs("/", &vfs) == 0)
        root_ro = (vfs.f_flag & ST_RDONLY) != 0;

    bool rw_ok = true;
    for (int i = 0; rw_paths[i]; i++)
    {
        struct statvfs rv;
        if (statvfs(rw_paths[i], &rv) == 0 && (rv.f_flag & ST_RDONLY))
        {
            log_debug("edc_ns_sandbox: rw path %s is still read-only", rw_paths[i]);
            rw_ok = false;
            break;
        }
    }

    if (enforced)
        *enforced = root_ro && rw_ok;

    log_info("edc_ns_sandbox: namespace sandbox applied (userns=%s, root_ro=%d, rw_ok=%d)", have_userns ? "yes" : "no",
             root_ro, rw_ok);
    return true;
}

#else /* !__linux__ */
#include "edc_ns_sandbox.h"
bool edc_ns_sandbox_apply(const char **ro_paths, const char **rw_paths, const char **deny_paths, bool *enforced)
{
    (void)ro_paths;
    (void)rw_paths;
    (void)deny_paths;
    if (enforced)
        *enforced = false;
    return false;
}

#endif
