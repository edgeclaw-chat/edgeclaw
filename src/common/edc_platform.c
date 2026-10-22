#include "edc_platform.h"

#ifdef _WIN32

/* No kernel sandbox backend yet (see src/sandbox: Landlock/seccomp/BPF and Seatbelt are
 * POSIX mechanisms). Windows spawns powershell.exe: -Command takes the rest of the line,
 * and -NonInteractive keeps a prompt from waiting on a console nobody is watching. */
/* The prologue that makes PowerShell's output UTF-8. Three statements, one broken boundary
 * each -- and all three are needed, they are not belt and braces:
 *
 *  1. [Console]::OutputEncoding -- PS's own text, and the console code page (so a cmd
 *                                 built-in child inherits the fix too: measured);
 *  2. $OutputEncoding           -- what PS uses when it pipes TO a native program. Its
 *                                 default is us-ascii, which silently DROPS non-ASCII;
 *  3. $PSDefaultParameterValues -- the implicit -Encoding of Get-Content / Set-Content /
 *                                 Out-File. Without it `Get-Content f | Select-String 匹配`
 *                                 answers "no match" on a file that contains it: a WRONG
 *                                 ANSWER, not a display artefact. Deliberately these three
 *                                 named cmdlets and NOT '*:Encoding', which would also move
 *                                 Export-Csv / ConvertTo-Json under the user's feet.
 *
 * UTF8Encoding::new($false) rather than [Text.Encoding]::UTF8: the latter's encoder emits a
 * preamble, and a BOM in front of every tool result is not something to depend on NOT
 * happening. It ends with ';' so the command line can be appended to it as-is.
 *
 * -NoProfile in the head below is about TIMING, not about cleaning up: a profile that writes
 * anything to stdout runs BEFORE the prologue and pins the very encoding we are setting. */
#define EDC_PS_PROLOGUE                                                                                \
    "[Console]::OutputEncoding=[Text.UTF8Encoding]::new($false);"                                      \
    "$OutputEncoding=[Console]::OutputEncoding;"                                                       \
    "$PSDefaultParameterValues['Get-Content:Encoding']='utf8';"                                        \
    "$PSDefaultParameterValues['Set-Content:Encoding']='utf8';"                                        \
    "$PSDefaultParameterValues['Out-File:Encoding']='utf8';"

static const char *const k_shell_argv_head[] = {"powershell.exe", "-NoProfile", "-NonInteractive", "-Command",
                                                NULL};

/* The shell tool sizes its spawn vector from EDC_SHELL_ARGV_HEAD_MAX, so a head that grows past
 * it is a compile error HERE instead of an overflow there. */
_Static_assert(sizeof(k_shell_argv_head) / sizeof(k_shell_argv_head[0]) - 1 <= EDC_SHELL_ARGV_HEAD_MAX,
               "raise EDC_SHELL_ARGV_HEAD_MAX (edc_platform.h): the spawn vector is sized from it");

static const struct edc_platform_caps k_caps = {
    .kernel_sandbox = false,
    .spawn_mode = EDC_SPAWN_HIDE,
    .shell_file = "powershell.exe",
    .shell_argv_head = k_shell_argv_head,
    .shell_prologue = EDC_PS_PROLOGUE,
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

/* Compile-time only (no runtime effect on this branch): keeps this head and the spawn vector
 * the shell tool sizes from EDC_SHELL_ARGV_HEAD_MAX provably in agreement. */
_Static_assert(sizeof(k_shell_argv_head) / sizeof(k_shell_argv_head[0]) - 1 <= EDC_SHELL_ARGV_HEAD_MAX,
               "raise EDC_SHELL_ARGV_HEAD_MAX (edc_platform.h): the spawn vector is sized from it");

static const struct edc_platform_caps k_caps = {
#if defined(__linux__) || defined(__APPLE__)
    .kernel_sandbox = true,
#else
    .kernel_sandbox = false, /* no backend for this system yet */
#endif
    .spawn_mode = EDC_SPAWN_DETACH,
    .shell_file = "/bin/sh",
    .shell_argv_head = k_shell_argv_head,
    /* sh -c has no encoding layer of its own: the bytes a command writes are the bytes we
     * read, and they are already UTF-8. Nothing to put in front of the command line. */
    .shell_prologue = NULL,
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
