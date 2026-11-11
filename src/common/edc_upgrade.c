#include "edc_upgrade.h"

#include "edc_compat.h"
#include "edc_file_utils.h" /* edc_path_dirname, edc_path_join */
#include "edc_path.h"
#include "log.h"

#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <stdlib.h>
#else /* Linux and other ELF systems */
#include <limits.h>
#include <unistd.h>
#endif

sds edc_platform_exe_path(void)
{
#if defined(_WIN32)
    /* GetModuleFileNameW returns the length written and sets ERROR_INSUFFICIENT_BUFFER when
     * the buffer is too small, in which case the value is the REQUIRED size. */
    DWORD cap = MAX_PATH;
    for (int attempt = 0; attempt < 4; attempt++)
    {
        wchar_t *buf = malloc(cap * sizeof(wchar_t));
        if (!buf)
            return NULL;
        const DWORD n = GetModuleFileNameW(NULL, buf, cap);
        if (n == 0)
        {
            free(buf);
            return NULL;
        }
        if (n < cap)
        {
            char *utf8 = edc_wide_to_utf8_alloc(buf);
            sds out = utf8 ? sdsnew(utf8) : NULL;
            free(utf8);
            free(buf);
            return out;
        }
        free(buf);
        cap *= 2;
    }
    return NULL;
#elif defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(NULL, &size); /* first call: asks for the needed size */
    if (size == 0)
        return NULL;
    char *buf = malloc(size);
    if (!buf)
        return NULL;
    if (_NSGetExecutablePath(buf, &size) != 0)
    {
        free(buf);
        return NULL;
    }
    /* The path may contain symlinks and "..", so resolve it: the install-location rules below
     * compare directory names, and a symlinked spelling would defeat them. */
    char resolved[PATH_MAX];
    const char *out = realpath(buf, resolved) ? resolved : buf;
    sds ret = sdsnew(out);
    free(buf);
    return ret;
#else
    /* /proc/self/exe is a symlink to the real file. readlink does not NUL-terminate. */
    char buf[PATH_MAX];
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0)
        return NULL;
    buf[n] = '\0';
    return sdsnew(buf);
#endif
}

const char *edc_upgrade_method_name(edc_upgrade_method m)
{
    switch (m)
    {
    case EDC_UPGRADE_METHOD_INSTALLER:
        return "installer";
    case EDC_UPGRADE_METHOD_UNKNOWN:
    default:
        return "unknown";
    }
}

/* Is |dir| exactly one of the directories the installer puts the binary in?
 *
 * Compared after trimming one trailing separator, because both "$HOME/.local/bin" and
 * "$HOME/.local/bin/" name the same place and the latter is what edc_path_dirname may hand
 * back. Compared EXACTLY rather than by prefix: a substring test would accept
 * "$HOME/.local/bin-backup" or "/tmp/$HOME/.local/bin", and accepting those means running an
 * installer against a directory nobody meant it to touch. */
static bool dir_is(const char *dir, sds candidate)
{
    if (!dir || !candidate)
        return false;
    size_t n = sdslen(candidate);
    while (n > 1 && (candidate[n - 1] == '/' || candidate[n - 1] == '\\'))
        n--;
    const size_t dlen = strlen(dir);
    size_t dn = dlen;
    while (dn > 1 && (dir[dn - 1] == '/' || dir[dn - 1] == '\\'))
        dn--;
    return dn == n && strncmp(dir, candidate, n) == 0;
}

edc_upgrade_method edc_upgrade_detect(const char *exe_path, const char *home)
{
    if (!exe_path || !exe_path[0] || !home || !home[0])
        return EDC_UPGRADE_METHOD_UNKNOWN;

    /* Normalise both sides before comparing. edc_path_join appends a separator
     * unconditionally, so a home spelled with a trailing slash would produce
     * "/home/u//.local/bin" and never match; edc_path_norm is lexical and never touches the
     * disk, which is what keeps this function pure. */
    sds raw_dir = edc_path_dirname(exe_path);
    if (!raw_dir)
        return EDC_UPGRADE_METHOD_UNKNOWN;
    sds dir = edc_path_norm(raw_dir);
    sdsfree(raw_dir);
    if (!dir)
        return EDC_UPGRADE_METHOD_UNKNOWN;

    /* The installer's default target, plus the conventional per-user bin when the binary is
     * already there. Both are spelled from |home| so this stays a pure function. */
    sds local_bin_joined = edc_path_join(home, ".local/bin", NULL);
    sds home_bin_joined = edc_path_join(home, "bin", NULL);
    sds local_bin = edc_path_norm(local_bin_joined);
    sds home_bin = edc_path_norm(home_bin_joined);
    sdsfree(local_bin_joined);
    sdsfree(home_bin_joined);

    edc_upgrade_method m = EDC_UPGRADE_METHOD_UNKNOWN;
    if (dir_is(dir, local_bin) || dir_is(dir, home_bin))
        m = EDC_UPGRADE_METHOD_INSTALLER;

    sdsfree(local_bin);
    sdsfree(home_bin);
    sdsfree(dir);
    return m;
}

bool edc_upgrade_supported(void)
{
    /* Every platform this build ships for has a native installer, so the answer is always
     * yes -- what differs is WHICH one, chosen by edc_upgrade_argv().
     *
     * POSIX gets tools/install.sh, a sh script. Windows gets tools/install.ps1, because a
     * default Windows install has no sh at all: the quick-start path there is "download the
     * tarball, unpack it with the tar that ships with Windows, run the exe". Handing that
     * user a .sh script to enable updates would mean requiring Git for Windows purely for
     * this -- and a development machine that happens to have one is not evidence that a
     * user's machine does.
     *
     * The PowerShell script needs nothing beyond what Windows 10+ already carries:
     * Invoke-WebRequest, tar, Get-FileHash, and .NET's ECDsa for the signature. */
    return true;
}

/* Which installer runs on this platform. */
const char *edc_upgrade_installer_filename(void)
{
#if defined(_WIN32)
    return "install.ps1";
#else
    return "install.sh";
#endif
}

const char *edc_upgrade_unavailable_reason(edc_upgrade_method method)
{
    switch (method)
    {
    case EDC_UPGRADE_METHOD_INSTALLER:
        return "cannot run the installer";
    case EDC_UPGRADE_METHOD_UNKNOWN:
    default:
        return "this copy was not installed by the installer, so it cannot upgrade itself";
    }
}

sds edc_upgrade_manual_hint(const char *tag)
{
    const char *t = (tag && tag[0]) ? tag : "<version>";

    /* Two routes, because the first one needs nothing installed.
     *
     * The release page is always there: a user who has neither curl nor a shell can open it
     * and unpack the archive by hand. That is not a worse experience for them -- it is the
     * only one that works -- and it is also how they installed the program in the first place
     * if it lives somewhere this module cannot recognise.
     *
     * The command is the shortcut for everyone else. */
#if defined(_WIN32)
    /* NOT `powershell -File <url>`: -File takes a local script path, so a URL there fails
     * with "The argument ... does not exist". Download first, then run the local copy --
     * which is also what makes the -Tag argument possible at all. Two statements on separate
     * lines rather than one -Command string, because quoting three levels deep is how the
     * broken version got written. */
    return sdscatprintf(sdsempty(),
                        "download %s from\n"
                        "  https://github.com/edgeclaw-chat/edgeclaw/releases/tag/%s\n"
                        "or, in PowerShell:\n"
                        "  iwr -UseBasicParsing "
                        "https://raw.githubusercontent.com/edgeclaw-chat/edgeclaw/%s/tools/install.ps1 "
                        "-OutFile \"$env:TEMP\\edgeclaw-install.ps1\"\n"
                        "  & \"$env:TEMP\\edgeclaw-install.ps1\" -Tag %s",
                        t, t, t, t);
#else
    return sdscatprintf(sdsempty(),
                        "download %s from\n"
                        "  https://github.com/edgeclaw-chat/edgeclaw/releases/tag/%s\n"
                        "or run:\n"
                        "  curl -fsSL "
                        "https://raw.githubusercontent.com/edgeclaw-chat/edgeclaw/%s/tools/install.sh "
                        "| sh -s -- --tag %s",
                        t, t, t, t);
#endif
}

sds *edc_upgrade_argv(edc_upgrade_method method, const char *script_path, const char *tag, const char *dir)
{
    if (!edc_upgrade_supported())
        return NULL;
    if (method != EDC_UPGRADE_METHOD_INSTALLER)
        return NULL;
    if (!script_path || !script_path[0] || !tag || !tag[0] || !dir || !dir[0])
        return NULL;

#if defined(_WIN32)
    /* powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File <script> -Tag <tag> -Dir <dir>
     *
     * -NoProfile: the user's profile is not a dependency of an upgrade, and a profile that
     * prints or sets an alias would corrupt what we capture.
     * -NonInteractive: nothing may block on a prompt. stdin is /dev/null-equivalent here, so a
     * prompt would hang until the timeout.
     * -ExecutionPolicy Bypass: the default policy forbids running unsigned scripts. This script
     * is fetched over TLS pinned to the release tag and is the thing performing the
     * signature-checked install -- requiring the user to loosen a machine-wide policy first
     * would be worse than scoping it to this one invocation. */
    /* Every argument is spelled out rather than built from a table: the flag set alternates
     * name/value, and index arithmetic over it was harder to read than eleven lines. */
    sds *argv = calloc(12, sizeof(sds));
    if (!argv)
        return NULL;
    int n = 0;
    argv[n++] = sdsnew("powershell");
    argv[n++] = sdsnew("-NoProfile");
    argv[n++] = sdsnew("-NonInteractive");
    argv[n++] = sdsnew("-ExecutionPolicy");
    argv[n++] = sdsnew("Bypass");
    argv[n++] = sdsnew("-File");
    argv[n++] = sdsnew(script_path);
    argv[n++] = sdsnew("-Tag");
    argv[n++] = sdsnew(tag);
    argv[n++] = sdsnew("-Dir");
    argv[n++] = sdsnew(dir);
    argv[n] = NULL;
    for (int i = 0; i < n; i++)
    {
        if (!argv[i])
        {
            edc_upgrade_argv_free(argv);
            return NULL;
        }
    }
    return argv;
#else
    /* argv is `sh <script> --tag <tag> --dir <dir>`.
     *
     * The script is a FILE rather than fed on stdin, deliberately: stdin has to stay on
     * /dev/null so the installer cannot consume the TUI's terminal (it would read the user's
     * keystrokes and corrupt the rendering). `sh -s` would need a pipe, and a pipe means the
     * script shares a stream with whatever else the child reads.
     *
     * --tag is passed even though the installer could look the version up itself: the UI has
     * already told the user which one they are getting, and letting the installer decide again
     * would let the two disagree if a release lands in between. --dir pins the destination to
     * where the running binary is, so the upgrade replaces itself and not something else. */
    sds *argv = calloc(7, sizeof(sds));
    if (!argv)
        return NULL;
    argv[0] = sdsnew("sh");
    argv[1] = sdsnew(script_path);
    argv[2] = sdsnew("--tag");
    argv[3] = sdsnew(tag);
    argv[4] = sdsnew("--dir");
    argv[5] = sdsnew(dir);
    argv[6] = NULL;
    for (int i = 0; i < 6; i++)
    {
        if (!argv[i])
        {
            edc_upgrade_argv_free(argv);
            return NULL;
        }
    }
    return argv;
#endif
}

void edc_upgrade_argv_free(sds *argv)
{
    if (!argv)
        return;
    for (int i = 0; argv[i]; i++)
        sdsfree(argv[i]);
    free(argv);
}
