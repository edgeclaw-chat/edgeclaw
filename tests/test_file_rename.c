/* edc_file_rename_replace: the "write .tmp, then rename" state saves depend on the
 * destination being REPLACED. POSIX rename() does that; the Windows CRT rename()
 * fails with EEXIST, which silently kept the first version of session.json /
 * sessions.json forever (the session appeared to lose its latest history). */
#include "edc_file_utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return 0;
    int ok = fputs(text, f) >= 0;
    fclose(f);
    return ok;
}

static int read_is(const char *path, const char *want)
{
    char buf[64] = {0};
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    return strcmp(buf, want) == 0;
}

int main(void)
{
    const char *src = "edc_rename_src.tmp";
    const char *dst = "edc_rename_dst.txt";
    int failed = 0;

    remove(src);
    remove(dst);
    if (!write_file(src, "new") || !write_file(dst, "old"))
    {
        fprintf(stderr, "setup failed\n");
        return 1;
    }

    EdcError *err = NULL;
    if (!edc_file_rename_replace(src, dst, &err))
    {
        fprintf(stderr, "replace failed: %s\n", err ? err->message : "unknown");
        if (err)
            edc_error_free(err);
        failed = 1;
    }
    else if (!read_is(dst, "new"))
    {
        fprintf(stderr, "destination was not replaced\n");
        failed = 1;
    }
    else if (edc_file_exists(src))
    {
        fprintf(stderr, "source still exists after rename\n");
        failed = 1;
    }

    remove(src);
    remove(dst);
    if (!failed)
        printf("PASS\n");
    return failed;
}
