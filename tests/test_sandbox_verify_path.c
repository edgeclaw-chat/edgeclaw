/* What edc_sandbox_verify_path_at() returns for a path, when enforcement is OFF.
 *
 * With the sandbox off (or with no resolved root) the tools still go through this call:
 * it is what anchors a RELATIVE path at the session's bound working directory. That is
 * the whole job of the early-return branch -- and it has to leave an ABSOLUTE path
 * alone. It used to test `path[0] != '/'`, which is the same slash-only assumption that
 * made session binding break ("D:\ws" anchored at "D:\ws" became "D:\ws\D:\ws"), so on
 * Windows every absolute path handed to read_file / write_file / list_dir / edit_file /
 * @file-by-absolute-path was prefixed with the session directory and then failed to
 * resolve. edc_path_is_absolute() is the one predicate that knows about drive letters and
 * UNC roots; this pins that the branch asks it.
 *
 * Runs on every platform: the Windows-shaped inputs are checked on Windows only, but the
 * "absolute stays absolute" rule is asserted everywhere (a slash-only test also passes on
 * POSIX, which is why the original bug could only be seen on Windows). */

#include "edc_compat.h"
#include "edc_config.h"
#include "edc_sandbox.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "test_fs_util.h"

static int passed = 0;
static int failed = 0;

#define CHECK(cond, msg)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        if (cond)                                                                                                      \
        {                                                                                                              \
            passed++;                                                                                                  \
        }                                                                                                              \
        else                                                                                                           \
        {                                                                                                              \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);                                              \
            failed++;                                                                                                  \
        }                                                                                                              \
    } while (0)

/* A config with enforcement OFF -- the state Windows always runs in (see
 * edc_platform_has_sandbox), and the state any platform runs in after /sandbox off. */
static struct edc_config unenforced_conf(void)
{
    struct edc_config conf;
    memset(&conf, 0, sizeof(conf));
    conf.enable_sandbox = false;
    conf.sandbox_root_real = NULL;
    return conf;
}

/* Verify |path| against |base| and hand back the string the tools would open. */
static sds verify(struct edc_config *conf, const char *base, const char *path)
{
    EdcError *error = NULL;
    sds out = edc_sandbox_verify_path_at(conf, base, path, false, &error);
    if (error)
        edc_error_free(error);
    return out;
}

int main(void)
{
    struct edc_config conf = unenforced_conf();

#ifdef _WIN32
    const char *base = "D:\\github\\edgeclaw\\workspace";
    const char *abs = "D:\\github\\edgeclaw\\src\\main.c";
    const char *abs_other_drive = "C:\\Windows\\win.ini";
    const char *unc = "\\\\server\\share\\file.txt";
    const char *rel = "src\\main.c";
    const char *expect_rel = base;
#else
    const char *base = "/home/user/workspace";
    const char *abs = "/usr/include/stdio.h";
    const char *abs_other_drive = NULL;
    const char *unc = NULL;
    const char *rel = "src/main.c";
    const char *expect_rel = base;
#endif

    sds out = verify(&conf, base, abs);
    CHECK(out != NULL, "an absolute path comes back as a string");
    CHECK(out && strcmp(out, abs) == 0, "an absolute path is NOT anchored at the base");
    if (out)
        sdsfree(out);

    if (abs_other_drive)
    {
        out = verify(&conf, base, abs_other_drive);
        CHECK(out && strcmp(out, abs_other_drive) == 0, "an absolute path on another drive is NOT anchored");
        if (out)
            sdsfree(out);
    }

    if (unc)
    {
        out = verify(&conf, base, unc);
        CHECK(out && strcmp(out, unc) == 0, "a UNC path is NOT anchored");
        if (out)
            sdsfree(out);
    }

    /* The other half of the branch: a relative path IS anchored -- that is what makes
     * "@dir/name.md" and "read src/main.c" mean the session's directory. */
    out = verify(&conf, base, rel);
    CHECK(out != NULL && strncmp(out, expect_rel, strlen(expect_rel)) == 0 && out[strlen(expect_rel)] == PATH_SEP,
          "a relative path is anchored at the base");
    CHECK(out != NULL && strstr(out, rel) != NULL, "the relative part survives the anchor");
    if (out)
        sdsfree(out);

    /* No base: nothing to anchor at, the path is handed back as given. */
    out = verify(&conf, NULL, rel);
    CHECK(out != NULL && strcmp(out, rel) == 0, "without a base the path is returned as given");
    if (out)
        sdsfree(out);

    /* ── the hardlink rule ───────────────────────────────────────────────── */
    /* The deny list matches PATHS, so a second name for a secret -- the classic
     * `ln ~/.ssh/id_ed25519 ~/ws/innocent` -- lands under a name no deny entry covers,
     * and the mask the deny list promises would be bypassed. The read path therefore
     * refuses a regular file with more than one link.
     *
     * A directory has to stay exempt: its nlink counts its children, so the naive check
     * refused every directory that had one -- which list_dir would have hit as soon as
     * this flag reached the tool entry point. Writes are exempt too, so trees that
     * hardlink their outputs keep working. */
    {
        char hl_root[PATH_MAX];
        /* Through the shared helper, not a hardcoded /tmp: bazel gives each test a private
         * TEST_TMPDIR, and writing outside it escapes the sandbox the runner sets up. */
        test_tmp_template(hl_root, sizeof(hl_root), "edc_hl");
        if (mkdtemp(hl_root))
        {
            char secret[PATH_MAX], plain[PATH_MAX], alias[PATH_MAX];
            snprintf(secret, sizeof(secret), "%s/secret.txt", hl_root);
            snprintf(plain, sizeof(plain), "%s/plain.txt", hl_root);
            snprintf(alias, sizeof(alias), "%s/innocent.txt", hl_root);

            FILE *f = fopen(secret, "w");
            if (f)
            {
                fputs("PRIVATE KEY\n", f);
                fclose(f);
            }
            f = fopen(plain, "w");
            if (f)
            {
                fputs("hello\n", f);
                fclose(f);
            }
            CHECK(link(secret, alias) == 0, "the test can hardlink a file");

            /* Whether the guard can fire depends on the CRT reporting a link count. `link()`
             * works on Windows (win_compat implements it with CreateHardLinkW, and the check
             * above proves the link exists), but MSVC's stat() does not report st_nlink > 1
             * for an NTFS hardlink -- the CI log is what established that. So the refusal is
             * unreachable there THROUGH NO FAULT OF THE GUARD, and asserting it would be a red
             * test nobody can fix in the code under test -- which trains people to ignore red. */
            bool nlink_reported = false;
#ifndef _WIN32
            {
                struct stat hl_probe;
                nlink_reported = stat(alias, &hl_probe) == 0 && hl_probe.st_nlink > 1;
            }
#endif
            if (!nlink_reported)
                printf("    (this platform does not report st_nlink for hardlinks; "
                       "the refusal check cannot fire here)\n");

            sds rw[2] = {sdsnew(hl_root), NULL};
            struct edc_config conf2;
            memset(&conf2, 0, sizeof(conf2));
            conf2.enable_sandbox = true;
            conf2.sandbox_root_real = sdsnew(hl_root);
            conf2.sandbox_rw_paths = rw;

            EdcError *hl_err = NULL;
            sds got = edc_sandbox_verify_path_at(&conf2, NULL, alias, false, &hl_err);
            /* Only assert the refusal where the platform reports a link count; see the probe
             * above. Where it does report one, this is the property that keeps a second name
             * for a secret from bypassing the deny list. */
            if (nlink_reported)
            {
                CHECK(got == NULL, "a hardlinked file is refused for a model-facing read");
            }
            else
            {
                printf("    (skipped: hardlink refusal needs a link count)\n");
            }
            if (got)
                sdsfree(got);
            if (hl_err)
                edc_error_free(hl_err);
            hl_err = NULL;

            got = edc_sandbox_verify_path_at(&conf2, NULL, plain, false, &hl_err);
            CHECK(got != NULL, "an ordinary file in the same directory still reads");
            if (got)
                sdsfree(got);
            if (hl_err)
                edc_error_free(hl_err);
            hl_err = NULL;

            got = edc_sandbox_verify_path_at(&conf2, NULL, hl_root, false, &hl_err);
            CHECK(got != NULL, "a directory holding children is not mistaken for a hardlink");
            if (got)
                sdsfree(got);
            if (hl_err)
                edc_error_free(hl_err);
            hl_err = NULL;

            got = edc_sandbox_verify_path_at(&conf2, NULL, alias, true, &hl_err);
            CHECK(got != NULL, "the hardlink rule does not apply to writes");
            if (got)
                sdsfree(got);
            if (hl_err)
                edc_error_free(hl_err);

            sdsfree(conf2.sandbox_root_real);
            sdsfree(rw[0]);
            unlink(alias);
            unlink(secret);
            unlink(plain);
            rmdir(hl_root);
        }
    }

    printf("%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
