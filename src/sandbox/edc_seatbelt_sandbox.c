/*
 * macOS Seatbelt backend. See edc_seatbelt_sandbox.h for the contract and for
 * why this file has no sds/log-formatting dependencies.
 */

#include "edc_seatbelt_sandbox.h"

/* Needed by the non-Apple stubs below, which is the only branch Linux builds.
 * Everything else (stdlib/string/dlfcn) belongs to the Apple branch, so leave
 * them there -- but keep NULL reachable from both. */
#include <stddef.h>

#if defined(__APPLE__)


#include "edc_compat.h"
#include "log.h"
#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* ── sandbox_init() via dlsym ──────────────────────────────────────────────
 *
 * The header declares it API_DEPRECATED("No longer supported", macos(10.5,
 * 10.8)) and says flags "must be SANDBOX_NAMED". That is stale: an inline
 * profile with flags == 0 works (measured on macOS 27.0.1), and it is exactly
 * what /usr/bin/sandbox-exec does -- that binary links only libsbox.1.dylib and
 * libSystem. Looking the symbol up instead of linking -lsandbox keeps the
 * deprecation warning out of the build, keeps the BUILD linkopts unchanged, and
 * turns "Apple finally removed it" into a clean fail-closed error. */

typedef int (*edc_sandbox_init_fn)(const char *profile, uint64_t flags, char **errorbuf);
typedef void (*edc_sandbox_free_error_fn)(char *errorbuf);

struct edc_seatbelt_api
{
    edc_sandbox_init_fn init;
    edc_sandbox_free_error_fn free_error;
};

static bool seatbelt_api_load(struct edc_seatbelt_api *api)
{
    memset(api, 0, sizeof(*api));

    /* On every shipping macOS libsandbox is already mapped (libSystem
     * re-exports it), so this normally succeeds without a dlopen. */
    void *h = dlopen("/usr/lib/libsandbox.1.dylib", RTLD_LAZY | RTLD_LOCAL);

    api->init = (edc_sandbox_init_fn)dlsym(RTLD_DEFAULT, "sandbox_init");
    api->free_error = (edc_sandbox_free_error_fn)dlsym(RTLD_DEFAULT, "sandbox_free_error");

    if (!api->init && h)
        api->init = (edc_sandbox_init_fn)dlsym(h, "sandbox_init");
    if (!api->free_error && h)
        api->free_error = (edc_sandbox_free_error_fn)dlsym(h, "sandbox_free_error");

    /* h is intentionally not dlclose()d: the symbol stays live for the life of
     * this (short-lived) helper. */
    return api->init != NULL;
}

bool edc_seatbelt_available(void)
{
    struct edc_seatbelt_api api;
    return seatbelt_api_load(&api);
}

bool edc_seatbelt_apply(const char *profile, char **err)
{
    if (err)
        *err = NULL;
    if (!profile)
        return false;

    struct edc_seatbelt_api api;
    if (!seatbelt_api_load(&api))
    {
        if (err)
            *err = strdup("sandbox_init unavailable (libsandbox not loaded)");
        return false;
    }

    char *e = NULL;
    int rc = api.init(profile, 0, &e);
    if (rc != 0)
    {
        if (err)
            *err = strdup(e ? e : "sandbox_init failed");
        if (e && api.free_error)
            api.free_error(e);
        return false;
    }

    if (e && api.free_error)
        api.free_error(e);
    return true;
}

/* ── growable text buffer (no sds: see the header) ───────────────────────── */

struct sbuf
{
    char *p;
    size_t len;
    size_t cap;
    bool oom;
};

static void sbuf_need(struct sbuf *b, size_t extra)
{
    if (b->oom)
        return;
    if (b->len + extra + 1 <= b->cap)
        return;
    size_t cap = b->cap ? b->cap : 1024;
    while (cap < b->len + extra + 1)
        cap *= 2;
    char *np = realloc(b->p, cap);
    if (!np)
    {
        b->oom = true;
        return;
    }
    b->p = np;
    b->cap = cap;
}

static void sbuf_addn(struct sbuf *b, const char *s, size_t n)
{
    sbuf_need(b, n);
    if (b->oom)
        return;
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = '\0';
}

static void sbuf_adds(struct sbuf *b, const char *s)
{
    sbuf_addn(b, s, strlen(s));
}

/* Emit one SBPL path filter: (subpath "escaped") or (literal "escaped").
 *
 * realpath() is mandatory, not cosmetic: SBPL compares the *resolved* path, so
 * "/tmp" silently matches nothing while "/private/tmp" works, and on the
 * case-insensitive default volume realpath() is also what fixes the case of
 * /users/foo → /Users/foo. This mirrors edc_sandbox_report_unresolved_paths().
 *
 * |filter| is "subpath" for the ro/rw/deny lists, whose semantics are prefix
 * matching at a path boundary (edc_sandbox.c:path_prefix_match_len) -- SBPL's
 * subpath is exactly that, component-aware, so /a/b does not match /a/bc.
 * Returns false when the path does not resolve. */
static bool sbuf_path(struct sbuf *b, const char *filter, const char *path)
{
    char real[PATH_MAX];
    if (!realpath(path, real))
        return false;

    sbuf_adds(b, "(");
    sbuf_adds(b, filter);
    sbuf_adds(b, " \"");
    for (const char *p = real; *p; p++)
    {
        if (*p == '"' || *p == '\\')
        {
            char esc[2] = {'\\', *p};
            sbuf_addn(b, esc, 2);
        }
        else
            sbuf_addn(b, p, 1);
    }
    sbuf_adds(b, "\")");
    return true;
}

/* ── profile construction ───────────────────────────────────────────────── */

struct entry
{
    char *path;   /* resolved */
    bool rw;      /* grants write; ro entries only grant read */
    size_t depth; /* strlen(path): a prefix relation implies a shorter string */
};

/* Ascending depth => the deepest rule is written last => it wins. SBPL is
 * last-match-wins, which is how "longest prefix wins" from
 * edc_sandbox.c:path_allowed() is expressed here without any code.
 *
 * At equal depth the rw allow is written last, i.e. rw wins the tie. That is
 * the *kernel* layer's behaviour on Linux -- the mount namespace remounts "/"
 * read-only and binds the rw entries back, and Landlock's rules are additive,
 * so a path in both sets ends up writable. Only the user-space list check in
 * path_allowed() resolves a tie to read-only; the kernel layer must not, or a
 * "/dev" that a user happens to put in both sets would deny file-write* on
 * /dev and break 2>/dev/null for every command. */
static int entry_cmp(const void *va, const void *vb)
{
    const struct entry *a = va;
    const struct entry *b = vb;
    if (a->depth != b->depth)
        return a->depth < b->depth ? -1 : 1;
    if (a->rw != b->rw)
        return a->rw ? 1 : -1;
    return 0;
}

static size_t count_paths(const char **paths)
{
    size_t n = 0;
    while (paths && paths[n])
        n++;
    return n;
}

/* Operations that are not path-scoped. This block is the equivalent of the
 * Linux side's seccomp allow-set, except inverted: (deny default) at the top
 * already refuses ptrace, mount, bpf, module loading, io_uring, keyring and
 * every other escape vector, so only the capabilities a shell genuinely needs
 * are listed. */
static void emit_capabilities(struct sbuf *b)
{
    sbuf_adds(b,
              "(allow process-exec)\n"
              "(allow process-fork)\n"
              /* fork/exec/wait on our own children; not process-info* on others,
               * which would be the macOS analogue of allowing ptrace. */
              "(allow process-info-pidinfo (target self))\n"
              "(allow signal)\n"
              /* df / statfs. dyld's CacheFinder reads vfs.disk-space before
               * anything else runs. */
              "(allow sysctl-read)\n"
              "(allow system-info)\n"
              /* Without mach-lookup a command does not fail, it blocks until the
               * tool's timeout kills it -- a sandbox denial that looks like a
               * hang. */
              "(allow mach-lookup)\n"
              "(allow ipc-posix-shm)\n"
              "(allow pseudo-tty)\n"
              /* The Linux seccomp filter does not block sockets, so allowing the
               * network here is what keeps the two platforms equivalent. Denying
               * it would be a behaviour change, not an alignment. */
              "(allow network-outbound network-inbound system-socket)\n"
              /* Opening /dev/null, /dev/fd/N (process substitution) and the
               * inherited stdio streams is not covered by the rw allow-list. */
              "(allow file-write* (literal \"/dev/null\") (literal \"/dev/stdout\") (literal \"/dev/stderr\")"
              " (literal \"/dev/stdin\") (literal \"/dev/tty\"))\n"
              "(allow file-write* (regex #\"^/dev/fd/\"))\n"
              "(allow file-map-executable)\n"
              "(allow file-read-metadata)\n");
}

/* Minimal set of paths a process needs before main() runs. Everything here was
 * found empirically from `log stream --predicate 'sender == "Sandbox"'`; a
 * strict read allow-list without it does not fail with EPERM, it aborts inside
 * dyld4::CacheFinder. */
static void emit_read_baseline(struct sbuf *b)
{
    sbuf_adds(b, "(allow file-read* (literal \"/\"))\n");
    sbuf_adds(b,
              "(allow file-read*"
              " (subpath \"/usr/lib\") (subpath \"/usr/share\")"
              " (subpath \"/System\") (subpath \"/System/Volumes/Preboot\")"
              " (subpath \"/Library/Apple\") (subpath \"/private/var/db/dyld\")"
              " (subpath \"/private/etc\") (subpath \"/dev\"))\n");
}

char *edc_seatbelt_build_profile(const char **ro_paths, const char **rw_paths, const char **deny_paths,
                                 bool strict_read)
{
    struct sbuf b = {0};
    size_t nro = count_paths(ro_paths);
    size_t nrw = count_paths(rw_paths);

    struct entry *entries = calloc(nro + nrw + 1, sizeof(*entries));
    if (!entries)
        return NULL;

    size_t n = 0;
    for (size_t i = 0; i < nrw; i++)
    {
        char real[PATH_MAX];
        if (!realpath(rw_paths[i], real))
        {
            /* Fail closed: an allow entry that does not resolve allows nothing.
             * A typo here cannot open anything, which is why only deny entries
             * are worth warning about (edc_sandbox_report_unresolved_paths). */
            continue;
        }
        entries[n].path = strdup(real);
        entries[n].rw = true;
        entries[n].depth = strlen(real);
        n++;
    }
    for (size_t i = 0; i < nro; i++)
    {
        char real[PATH_MAX];
        if (!realpath(ro_paths[i], real))
        {
            continue;
        }
        entries[n].path = strdup(real);
        entries[n].rw = false;
        entries[n].depth = strlen(real);
        n++;
    }
    qsort(entries, n, sizeof(*entries), entry_cmp);

    sbuf_adds(&b, "(version 1)\n(deny default)\n");
    emit_capabilities(&b);

    if (!strict_read)
    {
        /* Relaxed read side: the write allow-list and the deny masks below are
         * still exact, only reads are unrestricted. */
        sbuf_adds(&b, "(allow file-read*)\n");
    }
    else
        emit_read_baseline(&b);

    for (size_t i = 0; i < n; i++)
    {
        struct entry *e = &entries[i];
        if (e->rw)
        {
            sbuf_adds(&b, strict_read ? "(allow file-read* file-write* " : "(allow file-write* ");
            sbuf_path(&b, "subpath", e->path);
            sbuf_adds(&b, ")\n");
        }
        else
        {
            if (strict_read)
            {
                sbuf_adds(&b, "(allow file-read* ");
                sbuf_path(&b, "subpath", e->path);
                sbuf_adds(&b, ")\n");
            }
            /* A read-only path must revoke write even when a *shallower* rw
             * entry already granted it (rw=/a, ro=/a/b ⇒ /a/b stays read-only).
             * Emitted in depth order so a deeper rw re-grants it. */
            sbuf_adds(&b, "(deny file-write* ");
            sbuf_path(&b, "subpath", e->path);
            sbuf_adds(&b, ")\n");
        }
        free(e->path);
    }
    free(entries);

    /* Deny last, unconditionally: deny wins over any allow regardless of depth,
     * which is why this block is outside the sorted list. */
    for (size_t i = 0; deny_paths && deny_paths[i]; i++)
    {
        char real[PATH_MAX];
        if (!realpath(deny_paths[i], real))
        {
            log_warn("seatbelt: deny entry '%s' does not resolve -- it matches nothing, so what it "
                     "names is NOT masked",
                     deny_paths[i]);
            continue;
        }
        sbuf_adds(&b, "(deny file-read* file-write* ");
        sbuf_path(&b, "subpath", real);
        sbuf_adds(&b, ")\n");
    }

    if (b.oom)
    {
        free(b.p);
        return NULL;
    }
    /* Guarantee a non-NULL empty-string-owning pointer even with no rules. */
    if (!b.p)
        b.p = strdup("");
    return b.p;
}

/* ── probe ──────────────────────────────────────────────────────────────── */

bool edc_seatbelt_probe(const char *profile, char **err)
{
    if (err)
        *err = NULL;
    if (!profile)
        return false;

    /* Safe: the helper is single-threaded (see sandbox_helper.c). */
    pid_t pid = fork();
    if (pid < 0)
    {
        if (err)
            *err = strdup(strerror(errno));
        return false;
    }
    if (pid == 0)
    {
        /* Distinct exit codes so the parent can say WHY nothing started. */
        char *e = NULL;
        if (!edc_seatbelt_apply(profile, &e))
            _exit(125); /* sandbox_init refused the profile itself */
        free(e);
        execl("/usr/bin/true", "true", (char *)NULL);
        _exit(127); /* profile applied, exec refused */
    }

    int status = 0;
    while (waitpid(pid, &status, 0) < 0)
    {
        if (errno != EINTR)
        {
            if (err)
                *err = strdup(strerror(errno));
            return false;
        }
    }

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
        return true;

    char msg[256];
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    if (WIFSIGNALED(status))
        snprintf(msg, sizeof(msg), "profile cannot start a process (probe killed by signal %d)", WTERMSIG(status));
    else if (code == 125)
        /* The usual cause is nesting: sandbox_init() is refused when the calling
         * process is already confined, which is what happens when the helper is
         * invoked from inside another sandbox (a build sandbox, an App
         * Sandboxed app, an Xcode test). Worth spelling out -- the kernel's own
         * message is only "Operation not permitted". */
        snprintf(msg, sizeof(msg),
                 "sandbox_init refused the profile -- already inside a sandbox? Seatbelt profiles cannot be nested");
    else
        snprintf(msg, sizeof(msg), "profile cannot start a process (probe exited %d)", code);
    if (err)
        *err = strdup(msg);
    log_warn("seatbelt: %s", msg);
    return false;
}

#else /* !__APPLE__ */

char *edc_seatbelt_build_profile(const char **ro_paths, const char **rw_paths, const char **deny_paths,
                                 bool strict_read)
{
    (void)ro_paths;
    (void)rw_paths;
    (void)deny_paths;
    (void)strict_read;
    return NULL;
}

bool edc_seatbelt_apply(const char *profile, char **err)
{
    (void)profile;
    if (err)
        *err = NULL;
    return false;
}

bool edc_seatbelt_probe(const char *profile, char **err)
{
    (void)profile;
    if (err)
        *err = NULL;
    return false;
}

bool edc_seatbelt_available(void)
{
    return false;
}

#endif /* __APPLE__ */
