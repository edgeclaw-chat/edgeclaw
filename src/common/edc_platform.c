#include "edc_platform.h"

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "edc_utf8.h"

#include <stdlib.h>
#include <string.h>

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

unsigned int edc_platform_ansi_codepage(void)
{
    return (unsigned int)GetACP();
}

/* A buffer that grows as lines are appended. Local on purpose: this file is the one that must
 * NOT grow dependencies (see the header), so it does not borrow sds. */
struct edc_outbuf
{
    char *p;
    size_t len;
    size_t cap;
};

static bool outbuf_append(struct edc_outbuf *o, const char *p, size_t n)
{
    if (o->len + n + 1 > o->cap)
    {
        size_t cap = o->cap ? o->cap : 512;
        while (cap < o->len + n + 1)
            cap *= 2;
        char *grown = realloc(o->p, cap);
        if (!grown)
            return false;
        o->p = grown;
        o->cap = cap;
    }
    memcpy(o->p + o->len, p, n);
    o->len += n;
    o->p[o->len] = '\0';
    return true;
}

/* A line that is neither UTF-8 nor legal in the code page: binary, or a character cut in half
 * by the capture cap. Keep what is ASCII and put one U+FFFD where a sequence should have
 * started, so the result is valid UTF-8 and the damage is VISIBLE instead of plausible. */
static bool outbuf_append_repaired(struct edc_outbuf *o, const char *p, size_t n)
{
    static const char replacement[] = "\xEF\xBF\xBD"; /* U+FFFD */
    size_t i = 0;
    while (i < n)
    {
        size_t seq = edc_utf8_seq_len((const unsigned char *)p + i, n - i);
        if (seq)
        {
            if (!outbuf_append(o, p + i, seq))
                return false;
            i += seq;
        }
        else
        {
            if (!outbuf_append(o, replacement, sizeof(replacement) - 1))
                return false;
            i++;
        }
    }
    return true;
}

/* One line that is not UTF-8, transcoded from the ANSI code page. MB_ERR_INVALID_CHARS is what
 * separates "a line of GBK" from "a line of bytes that is neither": a byte sequence that is not
 * legal in the code page fails here instead of being mapped to something plausible. */
static bool outbuf_append_ansi_line(struct edc_outbuf *o, const char *line, size_t len)
{
    int wide_len = MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, line, (int)len, NULL, 0);
    if (wide_len <= 0)
        return false;

    bool ok = false;
    wchar_t *wide = malloc((size_t)wide_len * sizeof(wchar_t));
    if (!wide)
        return false;

    if (MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, line, (int)len, wide, wide_len) == wide_len)
    {
        int utf8_len = WideCharToMultiByte(CP_UTF8, 0, wide, wide_len, NULL, 0, NULL, NULL);
        if (utf8_len > 0)
        {
            char *utf8 = malloc((size_t)utf8_len);
            if (utf8)
            {
                /* The line keeps its '\n': it is ASCII, so the round trip preserves it. */
                if (WideCharToMultiByte(CP_UTF8, 0, wide, wide_len, utf8, utf8_len, NULL, NULL) == utf8_len)
                    ok = outbuf_append(o, utf8, (size_t)utf8_len);
                free(utf8);
            }
        }
    }
    free(wide);
    return ok;
}

char *edc_platform_output_to_utf8(const char *buf, size_t len, size_t *out_len, size_t *converted_lines)
{
    if (out_len)
        *out_len = 0;
    if (converted_lines)
        *converted_lines = 0;
    if (!buf || len == 0)
        return NULL;

    /* Most output is ASCII, and a buffer with no byte >= 0x80 is valid UTF-8 by construction:
     * the decoder never runs for it, and it is the common case. */
    bool maybe_not_utf8 = false;
    for (size_t i = 0; i < len; i++)
    {
        if ((unsigned char)buf[i] >= 0x80)
        {
            maybe_not_utf8 = true;
            break;
        }
    }
    if (!maybe_not_utf8)
        return NULL;

    struct edc_outbuf o = {0};
    size_t converted = 0;
    size_t i = 0;
    while (i < len)
    {
        const char *nl = memchr(buf + i, '\n', len - i);
        size_t end = nl ? (size_t)(nl - buf) + 1 : len; /* the line KEEPS its '\n' */
        size_t line_len = end - i;

        bool ok;
        if (edc_utf8_is_valid(buf + i, line_len))
        {
            ok = outbuf_append(&o, buf + i, line_len);
        }
        else
        {
            ok = outbuf_append_ansi_line(&o, buf + i, line_len);
            if (!ok)
                ok = outbuf_append_repaired(&o, buf + i, line_len);
            converted++;
        }
        if (!ok)
        {
            free(o.p);
            return NULL;
        }
        i = end;
    }

    if (converted == 0)
    {
        free(o.p);
        return NULL; /* everything was already UTF-8: nothing to say */
    }
    if (out_len)
        *out_len = o.len;
    if (converted_lines)
        *converted_lines = converted;
    return o.p;
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

/* A command's bytes are already what the caller wants here, and nothing converts them:
 * returning NULL IS the platform's answer, so the call site needs one check and no #ifdef.
 * Note what this deliberately does NOT do -- it does not validate. Piping a binary file
 * through `cat`, or running a tool that still emits GBK, must keep returning those exact
 * bytes; a "helpful" validation pass here would turn them into a field of U+FFFD, which is a
 * behaviour change, not a bug fix. */
char *edc_platform_output_to_utf8(const char *buf, size_t len, size_t *out_len, size_t *converted_lines)
{
    (void)buf;
    (void)len;
    if (out_len)
        *out_len = 0;
    if (converted_lines)
        *converted_lines = 0;
    return NULL;
}

unsigned int edc_platform_ansi_codepage(void)
{
    return 0; /* no code page to name: nothing is converted on this platform */
}
#endif

const struct edc_platform_caps *edc_platform_caps(void)
{
    return &k_caps;
}
