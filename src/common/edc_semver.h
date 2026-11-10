#ifndef __EDC_SEMVER_H__
#define __EDC_SEMVER_H__

#include <stdbool.h>

/* Semantic-version comparison, for "is there a newer release than this build".
 *
 * Deliberately not a parser that stores anything: the updater needs one question answered
 * ("is A newer than B") and keeping a version struct alive would outlive its usefulness.
 *
 * Accepted input: `1.2.3`, `v1.2.3` (the tag form this project uses), and a pre-release
 * suffix `1.2.3-rc.1`. Build metadata (`1.2.3+build.5`) is ignored, per the spec -- it does
 * not participate in precedence.
 *
 * Precedence follows semver 2.0.0 §11:
 *   - 1.2.3 < 1.2.4 < 1.3.0 < 2.0.0
 *   - a pre-release sorts BEFORE its release: 1.2.3-rc.1 < 1.2.3
 *   - pre-release identifiers compare field by field: numeric ones numerically, others
 *     bytewise, numeric < alphanumeric, and fewer fields < more when the prefix is equal
 *     (so rc.1 < rc.1.1, and rc.2 < rc.10).
 *
 * Anything unparseable is treated as "not newer" by edc_semver_is_newer(), which is the safe
 * direction: a version we cannot read must never trigger an update offer.
 */
bool edc_semver_is_newer(const char *candidate, const char *current);

#endif /* __EDC_SEMVER_H__ */
