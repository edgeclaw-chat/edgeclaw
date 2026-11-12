#include "edc_update_cli.h"

#include "edc_config.h"
#include "edc_file_utils.h"
#include "edc_semver.h"
#include "edc_update.h"
#include "edc_version.h"

#include <stdio.h>
#include <string.h>

int edc_update_check_cli(bool allow_pre, const char *current_override)
{
    /* Both values honour the same environment variables the launch-time check reads (see
     * edc_update.h): an explicit argument wins, then the variable, then the build's own. That
     * ordering is what makes this command usable as a diagnostic -- it can be pointed at a
     * local server and given a version to compare against, which is otherwise impossible
     * because an unreachable endpoint and "no update" print the same thing. */
    const char *version = (current_override && current_override[0])
                              ? current_override
                              : edc_update_current_version(edc_version());
    const bool version_overridden =
        (current_override && current_override[0]) || strcmp(version, edc_version()) != 0;

    printf("current   %s%s\n", version, version_overridden ? "  (overridden -- diagnostics only)" : "");
    printf("target    %s\n", edc_update_target());
    printf("channel   %s\n", allow_pre ? "pre" : "stable");

    /* The same short-circuit the launch path applies, stated rather than assumed. A dev build
     * reports the commit hash, which is not a version, so there is nothing to compare a
     * release against. */
    if (!edc_semver_is_valid(version))
    {
        printf("check     skipped: '%s' is not a release version\n", version);
        printf("          (a dev build cannot tell whether a release is newer)\n");
        printf("          (set %s or pass --current <ver> to try anyway)\n",
               "EDGECLAW_UPDATE_CURRENT_VERSION");
        return 0;
    }

    sds home = edc_config_resolve_bc_home();
    sds cacert = home ? edc_path_join(home, "workspace/ssl/cacert.pem", NULL) : NULL;
    printf("cacert    %s\n", cacert ? cacert : "(none)");

    sds url = edc_update_releases_url();
    printf("url       %s\n", url);

    sds tag = NULL, asset = NULL;
    const bool found =
        edc_update_check(cacert, url, version, allow_pre, 1u << 20, 10000, &tag, &asset);
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
    sdsfree(url);
    sdsfree(cacert);
    sdsfree(home);
    return 0;
}
