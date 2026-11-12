#include "edc_update_cli.h"

#include "edc_config.h"
#include "edc_file_utils.h"
#include "edc_semver.h"
#include "edc_update.h"
#include "edc_version.h"

#include <stdio.h>

#define EDC_UPDATE_RELEASES_URL "https://api.github.com/repos/edgeclaw-chat/edgeclaw/releases"

int edc_update_check_cli(bool allow_pre)
{
    const char *version = edc_version();
    printf("current   %s\n", version);
    printf("target    %s\n", edc_update_target());
    printf("channel   %s\n", allow_pre ? "pre" : "stable");

    /* The same short-circuit the launch path applies, stated rather than assumed. A dev build
     * reports the commit hash, which is not a version, so there is nothing to compare a
     * release against. */
    if (!edc_semver_is_valid(version))
    {
        printf("check     skipped: '%s' is not a release version\n", version);
        printf("          (a dev build cannot tell whether a release is newer)\n");
        return 0;
    }

    sds home = edc_config_resolve_bc_home();
    sds cacert = home ? edc_path_join(home, "workspace/ssl/cacert.pem", NULL) : NULL;
    printf("cacert    %s\n", cacert ? cacert : "(none)");

    sds tag = NULL, asset = NULL;
    const bool found =
        edc_update_check(cacert, EDC_UPDATE_RELEASES_URL, version, allow_pre, 1u << 20, 10000, &tag, &asset);
    if (found)
    {
        printf("result    %s is available\n", tag);
        if (asset)
            printf("asset     %s\n", asset);
    }
    else
    {
        /* One line for every reason -- offline, rate limited, nothing newer, no asset for this
         * platform. The check distinguishes them in its own log at debug level; a command that
         * answers a question should not read like a stack trace. */
        printf("result    nothing to offer\n");
    }

    sdsfree(tag);
    sdsfree(asset);
    sdsfree(cacert);
    sdsfree(home);
    return 0;
}
