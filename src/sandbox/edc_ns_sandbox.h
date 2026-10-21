#ifndef __BC_NS_SANDBOX_H__
#define __BC_NS_SANDBOX_H__

#include <stdbool.h>

/*
 * Apply a mount-namespace sandbox: unshare(CLONE_NEWUSER | CLONE_NEWNS),
 * remount "/" read-only and bind-mount each rw_path back read-write. This makes
 * the read-only set truly immutable at the kernel level -- chmod/chown/
 * utimes/xattr/ioctl(FS_IOC_SETFLAGS) on it fail with EROFS -- while writable
 * paths keep working normally (unlike a blanket seccomp deny, which is
 * path-blind).
 *
 * Returns false only on an unrecoverable error (e.g. "/" could not be remounted
 * read-only). When the namespace layers are unavailable (no unprivileged user
 * namespaces, or CLONE_NEWNS denied) it returns true with *enforced == false so
 * the caller can fall back to the seccomp metadata deny-list.
 *
 * *enforced is set true only after verifying the resulting mount flags: "/"
 * must be read-only AND every rw_path must be writable. Never trust the mounts
 * merely because the syscalls returned success.
 *
 * deny_paths (may be NULL) are masked out afterwards, last, so a deny wins over
 * any ro/rw mount: a directory gets an empty mode-000 tmpfs (its contents are
 * unreachable), a regular file gets an unopenable 0:0 character device bound
 * over it (so open() fails instead of handing back an empty file). This is the
 * only layer that can deny a path that lives inside an allowed parent (e.g.
 * ~/.ssh inside the read-only home) -- Landlock is an additive allow-list and
 * cannot.
 */
bool edc_ns_sandbox_apply(const char **ro_paths, const char **rw_paths, const char **deny_paths, bool *enforced);

#endif
