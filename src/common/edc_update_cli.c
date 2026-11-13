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
    edc_update_diag diag;
    diag.outcome = EDC_UPDATE_UP_TO_DATE;
    diag.http_status = 0;
    const bool found = edc_update_check_ex(cacert, url, version, allow_pre, 1u << 20, 10000, &tag, &asset, &diag);
    if (found)
    {
        printf("result    %s is available\n", tag);
        if (asset)
            printf("asset     %s\n", asset);
    }
    else
    {
        /* Two different answers, and conflating them is what made this command useless for
         * the case it exists for: a rate-limited or unreachable GitHub used to print exactly
         * what an up-to-date install prints. The launch-time check stays silent -- the user
         * can do nothing about a 403 -- but a command someone ran on purpose has to say which
         * of the two happened.
         *
         * Exit status stays 0 for every outcome, including the failures: this diagnoses the
         * updater, so a non-zero exit would look like a failure of the thing being diagnosed
         * (see tests/test_update_cli.c). Read the text, not $?. */
        switch (diag.outcome)
        {
        case EDC_UPDATE_UP_TO_DATE:
            printf("result    up to date -- no release newer than %s ships an archive for %s\n", version,
                   edc_update_target());
            break;
        case EDC_UPDATE_HTTP_STATUS:
            if (diag.http_status == 403)
                printf("result    could not check: HTTP 403, which is GitHub's unauthenticated rate limit "
                       "(60 requests/hour per IP)\n");
            else if (diag.http_status > 0)
                printf("result    could not check: HTTP %d\n", diag.http_status);
            else
                printf("result    could not check: the connection closed before answering\n");
            printf("          (not the same as up to date -- nothing was determined)\n");
            break;
        case EDC_UPDATE_TIMEOUT:
            printf("result    could not check: no response before the deadline\n");
            printf("          (not the same as up to date -- nothing was determined)\n");
            break;
        case EDC_UPDATE_CONNECT_FAILED:
            printf("result    could not check: could not connect (DNS, TCP, or the TLS handshake)\n");
            printf("          (not the same as up to date -- nothing was determined)\n");
            break;
        case EDC_UPDATE_BODY_TOO_LARGE:
            printf("result    could not check: the response was larger than the 1 MiB cap\n");
            printf("          (not the same as up to date -- nothing was determined)\n");
            break;
        case EDC_UPDATE_MALFORMED:
            printf("result    could not check: the response is not the release list (bad JSON or an error page)\n");
            printf("          (not the same as up to date -- nothing was determined)\n");
            break;
        case EDC_UPDATE_NO_CLIENT:
            printf("result    could not check: the CA bundle could not be loaded (%s)\n", cacert ? cacert : "(none)");
            break;
        case EDC_UPDATE_BAD_ARG:
        case EDC_UPDATE_OK: /* unreachable: OK is always reported through |found| */
        default:
            printf("result    could not check: the request could not be made\n");
            break;
        }
    }

    sdsfree(tag);
    sdsfree(asset);
    sdsfree(url);
    sdsfree(cacert);
    sdsfree(home);
    return 0;
}
