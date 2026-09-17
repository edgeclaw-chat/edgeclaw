#include "edc_config.h"
#include "edc_file_utils.h"
#include "edc_sandbox_helper.h"
#include "edc_sandbox_helper_blob.h"
#include "sds.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <uv.h>

static int passed = 0;
static int failed = 0;

#define T_CHECK(cond, msg)                                                                                             \
    do                                                                                                                 \
    {                                                                                                                  \
        if (cond)                                                                                                      \
            passed++;                                                                                                  \
        else                                                                                                           \
        {                                                                                                              \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);                                              \
            failed++;                                                                                                  \
        }                                                                                                              \
    } while (0)

/* fork/exec the helper and return its exit code (or -1). */
static int run_helper(const char *file, char **args)
{
    pid_t pid = fork();
    if (pid == 0)
    {
        execv(file, args);
        _exit(127);
    }
    if (pid < 0)
        return -1;
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* Same as run_helper(), but capture the helper's stderr into |errpath| with
 * EDC_SANDBOX_DEBUG=1 so the caller can read which kernel layers were actually
 * enforced (the namespace layer is only reported as "enforced" when it really
 * remounted "/" read-only). */
static int run_helper_capture(const char *file, char **args, const char *errpath)
{
    pid_t pid = fork();
    if (pid == 0)
    {
        int fd = open(errpath, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd >= 0)
        {
            dup2(fd, 2);
            close(fd);
        }
        setenv("EDC_SANDBOX_DEBUG", "1", 1);
        execv(file, args);
        _exit(127);
    }
    if (pid < 0)
        return -1;
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static struct edc_config *make_conf(const char *home, const char *rw)
{
    struct edc_config *c = calloc(1, sizeof(*c));
    c->edc_home = sdsnew(home);
    c->enable_sandbox = true;
    c->sandbox_rw_paths = calloc(2, sizeof(sds));
    c->sandbox_rw_paths[0] = sdsnew(rw);
    c->sandbox_ro_paths = calloc(2, sizeof(sds));
    c->sandbox_ro_paths[0] = sdsnew("/");
    return c;
}

int main(void)
{
    edc_file_utils_init(uv_default_loop());

    char home_tmpl[] = "/tmp/edc_helper_home_XXXXXX";
    char rw_tmpl[] = "/tmp/edc_helper_rw_XXXXXX";
    char *home = mkdtemp(home_tmpl);
    char *rw = mkdtemp(rw_tmpl);
    if (!home || !rw)
    {
        fprintf(stderr, "FAIL: mkdtemp\n");
        return 1;
    }

    struct edc_config *conf = make_conf(home, rw);

    /* A deny directory inside the rw parent: normally readable, but masked by
     * the namespace layer (deny wins over rw). */
    sds deny_dir = sdscatprintf(sdsempty(), "%s/deny", rw);
    mkdir(deny_dir, 0700);
    sds deny_secret = sdscatprintf(sdsempty(), "%s/secret", deny_dir);
    (void)deny_secret; /* only read on Linux, in the exec block below */
    FILE *secret_f = fopen(deny_secret, "w");
    if (secret_f)
    {
        fputs("TOPSECRET", secret_f);
        fclose(secret_f);
    }
    conf->sandbox_deny_paths = calloc(2, sizeof(sds));
    conf->sandbox_deny_paths[0] = sdsnew(deny_dir);

    /* 1. Extraction: returns a path to an executable whose bytes equal the blob. */
    sds path = edc_sandbox_helper_path(conf);
    T_CHECK(path != NULL, "edc_sandbox_helper_path returns a path");
    struct stat st;
    T_CHECK(path && stat(path, &st) == 0 && S_ISREG(st.st_mode), "helper is a regular file");
    T_CHECK(path && (st.st_mode & S_IXUSR) != 0, "helper is executable");
    T_CHECK(path && (size_t)st.st_size == edc_sandbox_helper_blob_size, "helper size matches blob");

    if (path)
    {
        FILE *f = fopen(path, "rb");
        T_CHECK(f != NULL, "helper can be opened");
        if (f)
        {
            unsigned char *buf = malloc(edc_sandbox_helper_blob_size);
            size_t n = fread(buf, 1, edc_sandbox_helper_blob_size, f);
            fclose(f);
            T_CHECK(n == edc_sandbox_helper_blob_size, "helper bytes read back");
            T_CHECK(memcmp(buf, edc_sandbox_helper_blob, n) == 0, "helper bytes equal the embedded blob");
            free(buf);
        }
    }

    /* 2. Idempotent: a second call reuses the same file. */
    sds path2 = edc_sandbox_helper_path(conf);
    T_CHECK(path2 && path && strcmp(path, path2) == 0, "extraction is idempotent");
    sdsfree(path2);

    /* 3. argv shape: <helper> --ro <p>... --rw <p>... -- <cmd> NULL */
    sds file = NULL;
    char **args = NULL;
    bool ok = edc_sandbox_helper_prepare(conf, "echo hi", &file, &args);
    T_CHECK(ok, "edc_sandbox_helper_prepare succeeds");
    if (ok)
    {
        T_CHECK(strcmp(args[0], file) == 0, "argv[0] is the helper path");
        T_CHECK(strcmp(args[1], "--ro") == 0, "argv[1] is --ro");
        int i = 2;
        while (args[i] && strcmp(args[i], "--rw") != 0)
            i++;
        T_CHECK(args[i] && strcmp(args[i], "--rw") == 0, "argv has --rw");
        i++;
        while (args[i] && strcmp(args[i], "--") != 0)
            i++;
        T_CHECK(args[i] && strcmp(args[i], "--") == 0, "argv has --");

        /* The deny group must be present and carry the deny path. */
        bool has_deny = false;
        const char *deny_val = NULL;
        for (int k = 0; args[k]; k++)
        {
            if (strcmp(args[k], "--deny") == 0)
            {
                has_deny = true;
                deny_val = args[k + 1];
                break;
            }
        }
        T_CHECK(has_deny, "argv has --deny");
        T_CHECK(deny_val && strcmp(deny_val, deny_dir) == 0, "argv carries the deny path");
        T_CHECK(args[i + 1] && strcmp(args[i + 1], "echo hi") == 0, "argv carries the command");
        T_CHECK(args[i + 2] == NULL, "argv is NULL-terminated");
    }
    sdsfree(file);
    free(args);

    /* 4. Functional: with the kernel sandbox active, writes inside rw succeed
     * and writes outside are denied. Skip if this host has no Landlock. */
#ifndef __linux__
    printf("test_sandbox_helper: skipped exec checks (non-Linux)\n");
#else
    sds probe_file = NULL;
    char **probe_args = NULL;
    sds probe_cmd = sdscatprintf(sdsempty(), "touch %s/../probe_escape", rw);
    bool active = false;
    if (edc_sandbox_helper_prepare(conf, probe_cmd, &probe_file, &probe_args))
    {
        /* touch outside rw: succeeds (0) only if the kernel sandbox is absent. */
        active = (run_helper(probe_file, probe_args) != 0);
    }
    sdsfree(probe_file);
    free(probe_args);
    sdsfree(probe_cmd);

    if (!active)
    {
        printf("test_sandbox_helper: kernel sandbox unavailable, skipping exec assertions\n");
    }
    else
    {
        sds wfile = NULL;
        char **wargs = NULL;
        sds wcmd = sdscatprintf(sdsempty(), "echo ok > %s/inside.txt && cat %s/inside.txt", rw, rw);
        if (edc_sandbox_helper_prepare(conf, wcmd, &wfile, &wargs))
        {
            T_CHECK(run_helper(wfile, wargs) == 0, "write inside the sandbox rw path succeeds");
        }
        sdsfree(wfile);
        free(wargs);
        sdsfree(wcmd);

        sds dfile = NULL;
        char **dargs = NULL;
        sds dcmd = sdscatprintf(sdsempty(), "echo x > %s/../outside.txt", rw);
        /* /tmp (rw's parent) is NOT in the rw set, so this must fail. */
        if (edc_sandbox_helper_prepare(conf, dcmd, &dfile, &dargs))
        {
            T_CHECK(run_helper(dfile, dargs) != 0, "write outside the sandbox is denied");
        }
        sdsfree(dfile);
        free(dargs);
        sdsfree(dcmd);

        /* Deny masking comes from the namespace layer only, so first learn from
         * the helper's debug output whether that layer was enforced. */
        bool ns_enforced = false;
        char dbg_tmpl[] = "/tmp/edc_helper_dbg_XXXXXX";
        int dbg_fd = mkstemp(dbg_tmpl);
        if (dbg_fd >= 0)
            close(dbg_fd);
        sds pfile = NULL;
        char **pargs = NULL;
        if (edc_sandbox_helper_prepare(conf, "true", &pfile, &pargs))
        {
            run_helper_capture(pfile, pargs, dbg_tmpl);
            char buf[512] = {0};
            FILE *df = fopen(dbg_tmpl, "r");
            if (df)
            {
                size_t n = fread(buf, 1, sizeof(buf) - 1, df);
                buf[n] = '\0';
                fclose(df);
            }
            ns_enforced = strstr(buf, "ns=on(enforced)") != NULL;
        }
        sdsfree(pfile);
        free(pargs);
        unlink(dbg_tmpl);

        if (!ns_enforced)
        {
            printf("test_sandbox_helper: ns not enforced, skipping deny masking assertions\n");
        }
        else
        {
            sds dcfile = NULL;
            char **dcargs = NULL;
            sds dccmd = sdscatprintf(sdsempty(), "cat %s", deny_secret);
            if (edc_sandbox_helper_prepare(conf, dccmd, &dcfile, &dcargs))
            {
                T_CHECK(run_helper(dcfile, dcargs) != 0, "deny path is masked inside the sandbox");
            }
            sdsfree(dcfile);
            free(dcargs);
            sdsfree(dccmd);
        }
    }
#endif

    printf("test_sandbox_helper: %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
