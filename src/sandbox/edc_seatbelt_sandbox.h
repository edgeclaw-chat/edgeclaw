#ifndef __BC_SEATBELT_SANDBOX_H__
#define __BC_SEATBELT_SANDBOX_H__

#include <stdbool.h>

/*
 * macOS kernel sandbox backend (Seatbelt), the counterpart of the Linux
 * ns / landlock / seccomp trio.
 *
 * One SBPL profile covers what Linux needs three mechanisms for:
 *
 *   allow-list on file read/write ......... Landlock
 *   deny masks that beat the allow-list ... mount-namespace masking
 *   chmod/chown/xattr/utimes on read-only . seccomp metadata deny
 *   escape syscalls (ptrace/mount/...) .... (deny default)
 *
 * Like the Linux layers, SBPL rules are irreversible and inherited, so they may
 * only be applied inside the per-command helper, never in the app process.
 *
 * Profiles are built as text and handed to sandbox_init() looked up with
 * dlsym(): the symbol lives in /usr/lib/libsandbox.1.dylib, is marked
 * API_DEPRECATED since 10.8, and is the exact function /usr/bin/sandbox-exec
 * calls. Going through dlsym keeps the deprecation warning out of the build and
 * degrades at runtime (fail-closed) instead of at link time if Apple ever drops
 * it.
 *
 * No sds dependency: this file is linked into the stand-alone helper binary
 * (//src/sandbox:sandbox_helper), whose only deps are edc_compat and log.c.
 */

/* Build the SBPL text for the policy. Returns a malloc'd NUL-terminated string
 * (caller free()), or NULL only on allocation failure.
 *
 * |strict_read| selects the read side:
 *   true  - reads are confined to ro ∪ rw plus a system baseline, matching the
 *           Linux backends (Landlock is an additive allow-list too).
 *   false - reads are unrestricted. Writes and deny masks are still enforced.
 *           This is the fallback when the strict profile cannot even start a
 *           process (dyld needs paths no allow-list predicts), see
 *           edc_seatbelt_probe().
 *
 * An allow entry that does not resolve is dropped (fail-closed: the path is not
 * allowed), matching edc_ns_sandbox_apply's handling. A deny entry that does not
 * resolve matches nothing and is logged once by the caller -- dropping it
 * silently is how a ~/.ssh mask stops masking.
 */
char *edc_seatbelt_build_profile(const char **ro_paths, const char **rw_paths, const char **deny_paths,
                                 bool strict_read);

/* Apply |profile| to the CURRENT process. On success the sandbox is active and
 * survives exec. On failure returns false and stores a malloc'd diagnostic in
 * |*err| (caller free()); |err| may be NULL. */
bool edc_seatbelt_apply(const char *profile, char **err);

/* fork → apply → exec("/bin/true") → waitpid. Proves the profile actually lets
 * a process start: the usual casualty of an over-tight read allow-list is dyld
 * aborting before main() (SIGABRT), which is indistinguishable from "the command
 * crashed" once it reaches tool output.
 *
 * Safe here because the helper is single-threaded; do NOT call it from the app.
 * Costs two forks per command; the caller is expected to memoize by policy. */
bool edc_seatbelt_probe(const char *profile, char **err);

/* True when the Seatbelt symbols could be resolved on this machine. */
bool edc_seatbelt_available(void);

#endif /* __BC_SEATBELT_SANDBOX_H__ */
