#include "edc_version.h"

#include <stdio.h>

const char *edc_version(void)
{
    return EDC_VERSION;
}

const char *edc_version_commit(void)
{
    return EDC_COMMIT;
}

const char *edc_version_build_time(void)
{
    return EDC_BUILD_TIME;
}

int edc_version_banner(char *buf, size_t cap)
{
    if (!buf || cap == 0)
        return 0;
    return snprintf(buf, cap, "edgeclaw %s (%s) built %s", EDC_VERSION, EDC_COMMIT,
                    EDC_BUILD_TIME);
}

const char *edc_platform(void)
{
    /* Spelled the way the release tarballs are (edgeclaw_aarch64_linux.tar.gz), so a
     * version report and a download page use the same words. */
#if defined(_WIN32)
#if defined(_M_ARM64) || defined(__aarch64__)
    return "windows/arm64";
#else
    return "windows/amd64";
#endif
#elif defined(__APPLE__)
#if defined(__aarch64__)
    return "macos/arm64";
#else
    return "macos/amd64";
#endif
#elif defined(__ANDROID__)
#if defined(__aarch64__)
    return "android/aarch64";
#else
    return "android/arm";
#endif
#else
#if defined(__aarch64__)
    return "linux/aarch64";
#elif defined(__arm__)
    return "linux/arm";
#elif defined(__x86_64__)
    return "linux/amd64";
#else
    return "linux/unknown";
#endif
#endif
}

void edc_version_print(int schema_version, FILE *out)
{
    if (!out)
        out = stdout;
    fprintf(out, "edgeclaw %s (%s)\n", EDC_VERSION, EDC_COMMIT);
    fprintf(out, "schema   v%d\n", schema_version);
    fprintf(out, "built    %s\n", EDC_BUILD_TIME);
    fprintf(out, "platform %s\n", edc_platform());
}
