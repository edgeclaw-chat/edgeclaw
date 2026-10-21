#include "edc_platform.h"

#ifdef _WIN32

/* No kernel sandbox backend yet (see src/sandbox: Landlock/seccomp/BPF and Seatbelt are
 * POSIX mechanisms). Windows spawns powershell.exe: -Command takes the rest of the line,
 * and -NonInteractive keeps a prompt from waiting on a console nobody is watching. */
static const char *const k_shell_argv_head[] = {"powershell.exe", "-NonInteractive", "-Command", NULL};

static const struct edc_platform_caps k_caps = {
    .kernel_sandbox = false,
    .spawn_mode = EDC_SPAWN_HIDE,
    .shell_file = "powershell.exe",
    .shell_argv_head = k_shell_argv_head,
};

enum edc_lock_result edc_platform_lock_memory(void *addr, size_t len)
{
    (void)addr;
    (void)len;
    return EDC_LOCK_UNSUPPORTED; /* no mlock: the key is not pinned, only kept in memory */
}

void edc_platform_unlock_memory(void *addr, size_t len)
{
    (void)addr;
    (void)len;
}

void edc_platform_kill_process_group(long pid)
{
    (void)pid; /* no process groups here; the caller terminates the child itself */
}

#else /* POSIX */

#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>

static const char *const k_shell_argv_head[] = {"sh", "-c", NULL};

static const struct edc_platform_caps k_caps = {
#if defined(__linux__) || defined(__APPLE__)
    .kernel_sandbox = true,
#else
    .kernel_sandbox = false, /* no backend for this system yet */
#endif
    .spawn_mode = EDC_SPAWN_DETACH,
    .shell_file = "/bin/sh",
    .shell_argv_head = k_shell_argv_head,
};

enum edc_lock_result edc_platform_lock_memory(void *addr, size_t len)
{
    return mlock(addr, len) == 0 ? EDC_LOCK_OK : EDC_LOCK_FAILED;
}

void edc_platform_unlock_memory(void *addr, size_t len)
{
    munlock(addr, len);
}

void edc_platform_kill_process_group(long pid)
{
    if (pid > 0)
        kill(-(pid_t)pid, SIGKILL);
}

#endif

const struct edc_platform_caps *edc_platform_caps(void)
{
    return &k_caps;
}
