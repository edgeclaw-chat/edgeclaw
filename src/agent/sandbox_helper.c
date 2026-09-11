/*
 * sandbox_helper — tiny, dependency-light executable that applies the kernel
 * sandbox and then execs the real shell.
 *
 * Why a separate program (see docs / MEMORY): Landlock and seccomp can only be
 * applied in the process that will be confined (they are irreversible and
 * inherited). Doing it inside the agent would sandbox the agent itself, and
 * doing it between fork() and exec() inside the multithreaded agent is unsafe
 * (only async-signal-safe calls are allowed there). This helper is exec'd by
 * the agent, so it applies the sandbox in its own fresh, single-threaded
 * process -- where ordinary libc calls are perfectly safe -- and then execs
 * /bin/sh. Because exec keeps the pid, the parent's process-group kill and
 * exit handling are unchanged.
 *
 * Usage:
 *     sandbox_helper --ro <path>... --rw <path>... -- <command>
 *
 * It is intentionally silent on success so it never pollutes tool output.
 * Set EDC_SANDBOX_DEBUG=1 to get diagnostics on stderr.
 *
 * On non-Linux platforms the sandbox backends are no-ops; the helper simply
 * execs the command (it is only used on Linux anyway).
 */

#include "edc_bpf_sandbox.h"
#include "edc_landlock_sandbox.h"
#include "log.h"
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif

#define MAX_PATHS 512

static bool verbose(void)
{
    const char *v = getenv("EDC_SANDBOX_DEBUG");
    return v && v[0] && strcmp(v, "0") != 0;
}

int main(int argc, char **argv)
{
    /* Silence the sandbox backends' log_*() output: it goes to stderr, which is
     * piped back as the command's stderr, so on success it would pollute every
     * tool result. We still print our own diagnostics when EDC_SANDBOX_DEBUG=1. */
    log_set_quiet(true);

    const char *ro[MAX_PATHS];
    const char *rw[MAX_PATHS];
    int nro = 0;
    int nrw = 0;
    const char *cmd = NULL;

    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--ro") == 0)
        {
            i++;
            while (i < argc && strcmp(argv[i], "--rw") != 0 && strcmp(argv[i], "--") != 0)
            {
                if (nro < MAX_PATHS - 1)
                    ro[nro++] = argv[i];
                i++;
            }
            i--;
        }
        else if (strcmp(argv[i], "--rw") == 0)
        {
            i++;
            while (i < argc && strcmp(argv[i], "--ro") != 0 && strcmp(argv[i], "--") != 0)
            {
                if (nrw < MAX_PATHS - 1)
                    rw[nrw++] = argv[i];
                i++;
            }
            i--;
        }
        else if (strcmp(argv[i], "--") == 0)
        {
            if (i + 1 < argc)
                cmd = argv[i + 1];
            break;
        }
        /* unknown tokens are ignored */
    }

    ro[nro] = NULL;
    rw[nrw] = NULL;

    if (!cmd)
    {
        fprintf(stderr, "sandbox_helper: missing '-- <command>'\n");
        return 2;
    }

#ifdef __linux__
    /* seccomp filter installation requires CAP_SYS_ADMIN or NO_NEW_PRIVS; set
     * it here so the seccomp layer works even when Landlock is unavailable.
     * Harmless otherwise (Landlock sets it too). */
    prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);

    bool landlock_ok = edc_landlock_sandbox_apply(ro, rw);
    bool seccomp_ok = edc_bpf_sandbox_apply(nrw > 0 ? rw[0] : (nro > 0 ? ro[0] : "/"));

    if (verbose())
    {
        fprintf(stderr, "sandbox_helper: landlock=%s seccomp=%s\n", landlock_ok ? "on" : "off",
                seccomp_ok ? "on" : "off");
    }
    if (!landlock_ok && !seccomp_ok && verbose())
        fprintf(stderr, "sandbox_helper: no kernel sandbox available (path checks only)\n");
#else
    (void)ro;
    (void)rw;
    (void)nro;
    (void)nrw;
#endif

    /* exec keeps our pid, so the parent's kill(-pid) / exit handling still work;
     * never fork here. */
    char *args[] = {"sh", "-c", (char *)cmd, NULL};
    execvp("/bin/sh", args);

    fprintf(stderr, "sandbox_helper: exec /bin/sh failed: %s\n", strerror(errno));
    return 127;
}
