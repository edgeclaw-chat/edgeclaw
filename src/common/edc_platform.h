#ifndef EDC_PLATFORM_H
#define EDC_PLATFORM_H

#include <stdbool.h>
#include <stddef.h>

/* What this build can DO, as data -- and the few OPERATIONS whose implementation differs.
 *
 * Some platform differences are implementation details and belong in the file that owns
 * them: the Windows CRT's rename, _fullpath, the two path separators (edc_compat.h,
 * edc_path.c), DPAPI versus termios (edc_secret.c). Others are CHOICES the caller has to
 * make -- which shell to spawn, how to start it, whether anything enforces a filesystem
 * policy -- and those were spelled as #ifdef at each call site. That is how one decision
 * ends up in four files and drifts: the shell tool, the kill path and the key-locking code
 * each knew their own answer, and the answers were only visible one file at a time.
 *
 * The split used here:
 *   - a CHOICE is a field of the table below (one place to read, one place to change);
 *   - an OPERATION whose implementation differs (mlock, signalling a process group) is a
 *     function in edc_platform.c. A flag would not help: the call itself does not compile
 *     on the other platform, so a caller testing a flag would still need #ifdef. */

enum edc_spawn_mode
{
    /* POSIX: run the child in its own session (setsid). It then has no controlling
     * terminal, so interactive prompts (sudo, ssh, apt) cannot be written over the TUI and
     * block forever -- open("/dev/tty") fails and the command aborts with a message the
     * model can act on. */
    EDC_SPAWN_DETACH = 1,
    /* Windows: DETACHED_PROCESS has the same NAME but the opposite effect -- a console
     * program started that way exits without writing to the redirected pipes, so every
     * tool call came back empty. UV_PROCESS_WINDOWS_HIDE gives what is actually wanted: no
     * console window, no interactive prompt surface, pipes still connected. */
    EDC_SPAWN_HIDE = 2,
};

/* The longest argv prefix any platform needs. The shell tool SIZES its spawn vector from this
 * instead of a literal, and each platform's table is checked against it at compile time
 * (edc_platform.c), so "the head grew by one word" can never quietly overflow the vector. */
#define EDC_SHELL_ARGV_HEAD_MAX 4

struct edc_platform_caps
{
    /* A kernel filesystem sandbox exists (Landlock/seccomp/BPF, or Seatbelt). Everything
     * that would turn the sandbox ON asks this: where it is false, config load clamps the
     * switch off, /sandbox on is refused and the CLI override does not take effect. */
    bool kernel_sandbox;

    enum edc_spawn_mode spawn_mode;

    /* "Run this command line": the file to spawn and the argument prefix the command
     * string is appended to. POSIX is /bin/sh with "sh -c"; Windows is powershell.exe,
     * whose -Command takes the rest of the line. */
    const char *shell_file;
    const char *const *shell_argv_head; /* NULL-terminated */

    /* Text to put IN FRONT of the command line, or NULL where the shell needs none.
     *
     * POSIX: NULL. `sh -c` has no encoding layer of its own -- the bytes a command writes are
     * the bytes we read, and they are already UTF-8.
     *
     * Windows: powershell.exe 5.1 writes its OWN stdout with the console code page (CP936
     * here), so every Chinese string it prints arrives as GBK and the tool result is
     * mojibake. The fix is not "set the encoding" but "set it BEFORE the first byte is
     * written": Console.Out's StreamWriter takes its encoding when it is first USED, so
     * setting it one statement later changes nothing observable (measured: a probe whose
     * first statement writes ASCII and only then sets the encoding stays mojibake). chcp is
     * not a substitute either -- it moves the OS console code page, which the already-pinned
     * writer ignores. Hence a prologue IN FRONT, not a wrapper around. */
    const char *shell_prologue;
};

const struct edc_platform_caps *edc_platform_caps(void);

/* The readable name for the one field that gates the whole sandbox subsystem. */
static inline bool edc_platform_has_sandbox(void)
{
    return edc_platform_caps()->kernel_sandbox;
}

/* ── Output encoding ─────────────────────────────────────────────────────────
 *
 * A command's output has to be UTF-8 before anything reads it: read_file, the TUI and the
 * model's copy (which is cut at the HEAD) all assume it, and there is no way to tell from the
 * bytes afterwards. On Windows they cannot be assumed: a native grandchild follows the CRT
 * locale (python, gcc -- measured: CP936 even with the console code page moved), while
 * PowerShell's own text follows the prologue above. On POSIX they can be: sh -c passes bytes
 * through.
 *
 * So the conversion is an OPERATION here rather than a choice in the table -- the POSIX answer
 * is "there is nothing to convert", which a NULL return expresses better than a flag. */

/* Convert |len| bytes of command output to UTF-8, line by line, and return a MALLOC'D buffer
 * (*out_len = its length; free() it -- this header stays sds-free so this target keeps no new
 * dependency). |converted_lines| gets how many lines had to be transcoded.
 *
 * Per LINE, not per buffer: one command really does emit both encodings in one invocation
 * (measured: PowerShell's own text plus a python grandchild), and judging the whole buffer
 * would corrupt the half that was already right.
 *
 * POSIX: returns NULL, meaning "nothing to convert". Deliberately NOT a validation pass --
 * piping a binary file through `cat` must keep returning those exact bytes. */
char *edc_platform_output_to_utf8(const char *buf, size_t len, size_t *out_len, size_t *converted_lines);

/* The ANSI code page a shell on this platform encodes with, or 0 where the question does not
 * apply. Only used to say WHICH code page a notice is about. */
unsigned int edc_platform_ansi_codepage(void);

/* TRUE where a leading UTF-8 BOM is METADATA rather than content.
 *
 * Windows: true. PowerShell's Set-Content/Out-File and most editors write UTF-8 WITH a BOM, so a
 * file the model created through exec_shell starts with U+FEFF -- invisible in read_file's
 * result, and enough to make edit_file's first-line match fail against a string that looks
 * identical on screen.
 *
 * POSIX: false. A BOM there is just bytes, nothing here writes one, and stripping it would
 * change what read_file returns. */
bool edc_platform_bom_is_metadata(void);

/* The result is an enum rather than a bool because the caller has to tell the two
 * "no" answers apart: a platform that HAS mlock and failed it is worth a warning, a
 * platform that has none is normal (the key is usable either way, it is just not pinned). */
enum edc_lock_result
{
    EDC_LOCK_OK = 0,
    EDC_LOCK_UNSUPPORTED, /* no mlock on this platform */
    EDC_LOCK_FAILED,      /* mlock exists and refused */
};

enum edc_lock_result edc_platform_lock_memory(void *addr, size_t len);
void edc_platform_unlock_memory(void *addr, size_t len);

/* SIGKILL the process group led by |pid| (POSIX: the child was spawned detached, so the
 * group signal is what also takes down grandchildren -- for `sh -c 'a && sudo b'`, killing
 * only the shell leaves the grandchild running and holding the pipes open). A no-op where
 * there are no process groups; the caller still terminates the child itself. */
void edc_platform_kill_process_group(long pid);

#endif /* EDC_PLATFORM_H */
