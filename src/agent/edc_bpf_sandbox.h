#ifndef __BC_BPF_SANDBOX_H__
#define __BC_BPF_SANDBOX_H__

#include <stdbool.h>

/* Install the seccomp-BPF filter.
 *
 * The filter always denies the capability/escape class (ptrace, bpf, module
 * loading, mount/setns, io_uring, keyring, identity switching, fd-by-handle,
 * cross-process memory access, ...).
 *
 * |block_metadata| additionally denies the metadata mutators that Landlock does
 * not mediate (chmod/chown/utimes families, xattr writes, and
 * ioctl(FS_IOC_SETFLAGS)). Those are path-blind, so they are only a fallback:
 * when the mount-namespace sandbox is active the read-only paths already return
 * EROFS, and the caller passes block_metadata=false to keep chmod/xattr usable
 * on writable paths. */
bool edc_bpf_sandbox_apply_ex(const char *sandbox_root, bool block_metadata);

/* Backwards-compatible wrapper: the conservative "block metadata too" policy. */
bool edc_bpf_sandbox_apply(const char *sandbox_root);

#endif
